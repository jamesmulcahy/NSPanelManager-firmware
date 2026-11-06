#include <PanelRestart.hpp>
#include <ConfigManager.hpp>
#include <MqttManager.hpp>
#include <PanelMetrics.hpp>
#include <NSPM_ConfigManager.hpp>
#include <WiFiManager.hpp>
#include <esp_log.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <esp_timer.h>
#include <esp_transport.h>
#include <lwip/sockets.h>

// SUBSCRIBEs allowed to be awaiting a SUBACK at once. esp_mqtt_client_subscribe_single() holds
// esp-mqtt's internal API lock across its socket write, and the MQTT client task needs that same
// lock to read from the socket. While we are sending, nothing can be received — including the
// SUBACKs we are waiting for. Keeping few in flight bounds how long a stalled write can lock the
// client task out.
#define SUBSCRIBE_MAX_IN_FLIGHT 4
// Timeout handed to esp-mqtt for a single network operation. Set explicitly rather than left at 0
// (which makes esp-mqtt substitute its own MQTT_NETWORK_TIMEOUT_MS) because SUBACK_TIMEOUT_MS is
// derived from it and that default lives in a header private to the component.
#define MQTT_WRITE_TIMEOUT_MS 10000
// Resend a SUBSCRIBE that has not been acknowledged after this long. A stalled write blocks the
// MQTT client task for a whole MQTT_WRITE_TIMEOUT_MS, so this has to outlast a full set of
// in-flight writes stalling back to back — otherwise every in-flight SUBSCRIBE expires at once and
// one slow write becomes a self-sustaining resend storm.
#define SUBACK_TIMEOUT_MS 60000
static_assert(SUBACK_TIMEOUT_MS > SUBSCRIBE_MAX_IN_FLIGHT * MQTT_WRITE_TIMEOUT_MS,
              "SUBACK_TIMEOUT_MS must outlast SUBSCRIBE_MAX_IN_FLIGHT stalled writes, or a single "
              "slow write expires every in-flight SUBSCRIBE and triggers a resend storm");
// Backoff after a failed SUBSCRIBE doubles from 1s up to this limit.
#define SUBSCRIBE_MAX_BACKOFF_MS 30000
// Number of unmatched SUBACKs to remember, see _early_subacks. Never needs to hold more than
// SUBSCRIBE_MAX_IN_FLIGHT of them, with room to spare.
#define EARLY_SUBACKS_MAX 32
// Retained values that take at least this long to arrive after subscribe() are logged at INFO rather than DEBUG,
// so panels that only log INFO and above still report slow page switches.
#define SUBSCRIBE_LATENCY_INFO_MS 250

static int64_t millis() {
  return esp_timer_get_time() / 1000;
}

void MqttManager::start(std::string *server, uint16_t *port, std::string *username, std::string *password) {
  esp_log_level_set("MqttManager", ConfigManager::log_level);
  ESP_LOGI("MqttManager", "Starting MQTTManager, will connect to %s:%d", server->c_str(), *port);
  MqttManager::_connected = false;
  MqttManager::_subscriptions_mutex = xSemaphoreCreateMutex();
  if (xTaskCreatePinnedToCore(MqttManager::_task_send_online_update, "mqtt_online_upd", 3072, NULL, 2, &MqttManager::_send_online_update_task_handle, 1) != pdPASS) {
    // Without this task the panel never announces itself as online, so the manager only ever
    // sees the retained last will. Everything else still works.
    ESP_LOGE("MqttManager", "Failed to create MQTT online status task!");
    MqttManager::_send_online_update_task_handle = NULL;
  }
  if (xTaskCreatePinnedToCore(MqttManager::_task_manage_subscriptions, "mqtt_subs", 4096, NULL, 4, &MqttManager::_manage_subscriptions_task_handle, 1) != pdPASS) {
    // Nothing will send SUBSCRIBE or UNSUBSCRIBE without this task. subscribe() and
    // unsubscribe() check the handle and report the failure to their callers.
    ESP_LOGE("MqttManager", "Failed to create MQTT subscription task!");
    MqttManager::_manage_subscriptions_task_handle = NULL;
  }
  MqttManager::_mqtt_config.broker.address.hostname = server->c_str();
  MqttManager::_mqtt_config.broker.address.port = *port;
  MqttManager::_mqtt_config.broker.address.transport = esp_mqtt_transport_t::MQTT_TRANSPORT_OVER_TCP;
  if (username->size() > 0 && password->size() > 0) {
    MqttManager::_mqtt_config.credentials.username = username->c_str();
    MqttManager::_mqtt_config.credentials.authentication.password = password->c_str();
  }
  std::string mqtt_client_id = "NSPMPanel-";
  mqtt_client_id.append(WiFiManager::mac_string());

  MqttManager::_mqtt_config.credentials.client_id = mqtt_client_id.c_str();
  MqttManager::_mqtt_config.buffer.size = 4096;
  MqttManager::_mqtt_config.buffer.out_size = 4096;

  MqttManager::_mqtt_config.task.priority = 10;
  MqttManager::_mqtt_config.task.stack_size = 8192;

  MqttManager::_mqtt_config.network.timeout_ms = MQTT_WRITE_TIMEOUT_MS;

  MqttManager::_state_topic = "nspanel/";
  MqttManager::_state_topic.append(WiFiManager::mac_string());
  MqttManager::_state_topic.append("/status");

  std::string mac_string = WiFiManager::mac_string();
  {
    // Build "offline" last-will payload
    nlohmann::json json;
    json["mac"] = mac_string.c_str();
    json["state"] = "offline";
    MqttManager::_last_will_message = json.dump();
  }

  {
    // Build "online" payload (pre-built so the retry task owns no heap allocation)
    nlohmann::json json;
    json["mac"] = mac_string.c_str();
    json["state"] = "online";
    MqttManager::_online_status_message = json.dump();
  }

  // Set last will message in config and update config of client
  MqttManager::_mqtt_config.session.last_will.msg = MqttManager::_last_will_message.c_str();
  MqttManager::_mqtt_config.session.last_will.msg_len = MqttManager::_last_will_message.length();
  MqttManager::_mqtt_config.session.last_will.topic = MqttManager::_state_topic.c_str();
  MqttManager::_mqtt_config.session.last_will.retain = true;
  MqttManager::_mqtt_config.session.last_will.qos = 0;

  // Initialize MQTT client with built config
  MqttManager::_mqtt_client = esp_mqtt_client_init(&MqttManager::_mqtt_config);
  if (MqttManager::_mqtt_client == NULL) {
    ESP_LOGE("MqttManager", "Failed to create MQTT client!");
    PanelRestart::restart();
    return;
  }

  // Register event handler for MQTT events
  esp_err_t result = esp_mqtt_client_register_event(MqttManager::_mqtt_client, esp_mqtt_event_id_t::MQTT_EVENT_ANY, MqttManager::_mqtt_event_handler, NULL);
  switch (result) {
  case ESP_ERR_NO_MEM:
    ESP_LOGE("MqttManager", "Failed to allocate MQTT event handler!");
    PanelRestart::restart();
    break;

  case ESP_ERR_INVALID_ARG:
    ESP_LOGE("MqttManager", "Failed to initialize MQTT event handler!");
    PanelRestart::restart();
    break;

  case ESP_OK:
    ESP_LOGV("MqttManager", "Attached MQTT event handler.");
    break;

  default:
    ESP_LOGW("MqttManager", "Unknown status code when registering MQTT event handler: %s", esp_err_to_name(result));
    break;
  }

  // Start the MQTT client
  result = esp_mqtt_client_start(MqttManager::_mqtt_client);
  switch (result) {
  case ESP_ERR_INVALID_ARG:
    ESP_LOGE("MqttManager", "Failed to start MQTT client!");
    PanelRestart::restart();
    break;

  case ESP_OK:
    ESP_LOGI("MqttManager", "Started MQTT client.");
    break;

  default:
    ESP_LOGW("MqttManager", "Unknown status code when registering MQTT event handler: %s", esp_err_to_name(result));
    break;
  }
}

void MqttManager::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  // esp_mqtt_client_handle_t client = event->client;

  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    MqttManager::_connected = true;
    PanelMetrics::mqtt_connects.add();
    {
      // Turn off Nagle's algorithm on the broker connection. With it on, a small packet such as a
      // SUBSCRIBE is held back until everything sent before it has been acknowledged, and the panel
      // is almost always sending something (log lines, status). The broker host delays its ACKs by
      // 40ms or more, so every page switch waited that long for its SUBACK, and often much longer.
      // The socket is new on every connect, so this has to be done each time.
      esp_transport_handle_t transport = esp_mqtt_client_get_transport(MqttManager::_mqtt_client, (char *)"mqtt");
      int sock = transport != NULL ? esp_transport_get_socket(transport) : -1;
      int nodelay = 1;
      if (sock >= 0 && setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) == 0) {
        ESP_LOGD("MqttManager", "Set TCP_NODELAY on MQTT socket %d.", sock);
      } else {
        ESP_LOGW("MqttManager", "Failed to set TCP_NODELAY on MQTT socket %d: errno %d.", sock, errno);
      }
    }
    // Supersede any retry still running for an earlier connection, then wake the task to
    // publish the retained "online" status for this one.
    MqttManager::_online_update_generation++;
    ESP_LOGI("MqttManager", "Connected to MQTT server. Online status generation %lu.", MqttManager::_online_update_generation.load());
    if (MqttManager::_send_online_update_task_handle != NULL) [[likely]] {
      xTaskNotifyGive(MqttManager::_send_online_update_task_handle);
    }

    // Sessions are clean (disable_clean_session is not set), so the broker has dropped all
    // our subscriptions: subscribe to everything again, and there is nothing left to unsubscribe from.
    xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
    {
      int64_t now = millis();
      for (Subscription &subscription : MqttManager::_subscriptions) {
        subscription.state = SubscriptionState::PENDING;
        subscription.msg_id = -1;
        subscription.failures = 0;
        subscription.next_attempt_ms = 0;
        // The broker replays every retained value again, so time those from the reconnect.
        subscription.requested_ms = now;
        subscription.suback_ms = 0;
        subscription.attempts = 0;
        subscription.after_reconnect = true;
        subscription.retained_seen = false;
      }
    }
    MqttManager::_pending_unsubscribes.clear();
    MqttManager::_early_subacks.clear();
    xSemaphoreGive(MqttManager::_subscriptions_mutex);
    if (MqttManager::_manage_subscriptions_task_handle != NULL) [[likely]] {
      xTaskNotifyGive(MqttManager::_manage_subscriptions_task_handle);
    }
    break;

  case MQTT_EVENT_SUBSCRIBED:
    xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
    MqttManager::_handle_suback(event->msg_id, event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED, millis());
    xSemaphoreGive(MqttManager::_subscriptions_mutex);
    break;

  case MQTT_EVENT_DATA:
    // A publish larger than the MQTT buffer is delivered as several MQTT_EVENT_DATA events: the
    // first carries the topic and data_len < total_data_len, the rest have no topic. No handler
    // reassembles them, so each one drops the message; log it here once, from the first fragment,
    // as that is the only one that still knows which topic it was for.
    if (event->current_data_offset == 0 && event->data_len < event->total_data_len) [[unlikely]] {
      ESP_LOGE("MqttManager", "Dropping %d byte message on %.*s, larger than the %d byte MQTT buffer.",
               event->total_data_len, event->topic_len, event->topic, MqttManager::_mqtt_config.buffer.size);
    }
    MqttManager::_log_subscribe_latency(event);
    break;

  case MQTT_EVENT_DISCONNECTED:
    MqttManager::_connected = false;
    PanelMetrics::mqtt_disconnects.add();
    // Stop any online-status retry still running for the connection that just dropped. This
    // handler can be running on that very task — esp_mqtt_client_publish() dispatches
    // MQTT_EVENT_DISCONNECTED inline when its write fails — so it must never delete it.
    MqttManager::_online_update_generation++;
    ESP_LOGW("MqttManager", "Lost connection to MQTT server. Online status generation %lu.", MqttManager::_online_update_generation.load());
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGI("MqttManager", "MQTT_EVENT_ERROR");
    if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
      if (event->error_handle->esp_tls_last_esp_err != 0) {
        ESP_LOGE("MqttManager", "Error report from esp-tls!");
      } else if (event->error_handle->esp_tls_stack_err != 0) {
        ESP_LOGE("MqttManager", "Error report from tls stack!");
      } else if (event->error_handle->esp_transport_sock_errno != 0) {
        ESP_LOGE("MqttManager", "Captured as transport's socket errno!");
      }
      ESP_LOGI("MqttManager", "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
    }
    break;

  default:
    break;
  }
}

void MqttManager::_task_send_online_update(void *param) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Keep retrying for as long as this connection lasts. A disconnect and a newer connect both
    // bump the generation, which drops us back to waiting for the next notification. The publish
    // below can deliver MQTT_EVENT_DISCONNECTED to _mqtt_event_handler on this task before it
    // returns, so the generation may already have moved on by the time it does.
    uint32_t generation = MqttManager::_online_update_generation.load();
    ESP_LOGD("MqttManager", "Online status task woke for generation %lu.", generation);
    while (MqttManager::_online_update_generation.load() == generation && MqttManager::connected()) {
      if (MqttManager::publish(MqttManager::_state_topic,
                               MqttManager::_online_status_message.c_str(),
                               MqttManager::_online_status_message.size(),
                               true) == ESP_OK) {
        ESP_LOGD("MqttManager", "Published online status for generation %lu.", generation);
        break;
      }
      ESP_LOGE("MqttManager", "Failed to publish online status to %s. Will retry in 5s.", MqttManager::_state_topic.c_str());
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
    // Reached on every exit from the loop: published, superseded by a newer generation, or
    // disconnected. If a wake is not followed by this line, the task is still inside publish()
    // — which is the state that used to wedge the client for good.
    ESP_LOGD("MqttManager", "Online status task parked; generation %lu -> %lu, connected=%d.",
             generation, MqttManager::_online_update_generation.load(), MqttManager::connected());
  }
}

bool MqttManager::connected() {
  return MqttManager::_connected;
}

esp_err_t MqttManager::subscribe(std::string topic) {
  if (MqttManager::_subscriptions_mutex == NULL || MqttManager::_manage_subscriptions_task_handle == NULL) {
    ESP_LOGE("MqttManager", "Failed to subscribe to MQTT topic '%s'. MQTT client is not started.", topic.c_str());
    return ESP_ERR_INVALID_STATE;
  }

  xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
  for (const Subscription &subscription : MqttManager::_subscriptions) {
    if (subscription.topic == topic) {
      xSemaphoreGive(MqttManager::_subscriptions_mutex);
      return ESP_OK; // Already subscribed or in progress.
    }
  }
  ESP_LOGD("MqttManager", "Subscribing to '%s'", topic.c_str());
  Subscription subscription;
  subscription.topic = topic;
  subscription.requested_ms = millis();
  MqttManager::_subscriptions.push_back(subscription);
  xSemaphoreGive(MqttManager::_subscriptions_mutex);

  xTaskNotifyGive(MqttManager::_manage_subscriptions_task_handle);
  return ESP_OK;
}

esp_err_t MqttManager::unsubscribe(std::string topic) {
  if (MqttManager::_subscriptions_mutex == NULL || MqttManager::_manage_subscriptions_task_handle == NULL) {
    ESP_LOGE("MqttManager", "Failed to unsubscribe from MQTT topic '%s'. MQTT client is not started.", topic.c_str());
    return ESP_ERR_INVALID_STATE;
  }

  xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
  for (auto it = MqttManager::_subscriptions.begin(); it != MqttManager::_subscriptions.end(); ++it) {
    if (it->topic == topic) {
      MqttManager::_subscriptions.erase(it);
      break;
    }
  }
  // Queue the UNSUBSCRIBE even if the topic was not in the set: a SUBSCRIBE for it may
  // already be on its way to the broker.
  MqttManager::_pending_unsubscribes.push_back(topic);
  xSemaphoreGive(MqttManager::_subscriptions_mutex);

  xTaskNotifyGive(MqttManager::_manage_subscriptions_task_handle);
  return ESP_OK;
}

void MqttManager::_handle_suback(int msg_id, bool failed, int64_t received_ms) {
  for (Subscription &subscription : MqttManager::_subscriptions) {
    if (subscription.state == SubscriptionState::IN_FLIGHT && subscription.msg_id == msg_id) {
      if (failed) {
        // For example denied by a broker ACL. Keep retrying with backoff in case it is fixed.
        subscription.state = SubscriptionState::PENDING;
        subscription.msg_id = -1;
        uint32_t backoff_ms = std::min(1000 << std::min<uint8_t>(subscription.failures, 5), SUBSCRIBE_MAX_BACKOFF_MS);
        subscription.next_attempt_ms = millis() + backoff_ms;
        if (subscription.failures < UINT8_MAX) [[likely]] {
          subscription.failures++; // Saturate rather than wrap back round to no backoff.
        }
        ESP_LOGE("MqttManager", "Broker rejected subscription to '%s'. Will retry in %lums.", subscription.topic.c_str(), backoff_ms);
      } else {
        subscription.state = SubscriptionState::SUBSCRIBED;
        subscription.failures = 0;
        subscription.suback_ms = received_ms;
        ESP_LOGD("MqttManager", "Subscribed to '%s'.", subscription.topic.c_str());
      }
      // A slot just freed up: let the subscription task send the next batch straight away
      // instead of waiting for its next poll.
      if (MqttManager::_manage_subscriptions_task_handle != NULL) [[likely]] {
        xTaskNotifyGive(MqttManager::_manage_subscriptions_task_handle);
      }
      return;
    }
  }

  // No match: either the subscription task has not recorded this message id yet, or the
  // subscription was removed or reset in the meantime. Remember it briefly for the first case.
  std::erase_if(MqttManager::_early_subacks, [received_ms](const EarlySuback &early_suback) {
    return received_ms - early_suback.received_ms > SUBACK_TIMEOUT_MS;
  });
  if (MqttManager::_early_subacks.size() >= EARLY_SUBACKS_MAX) {
    MqttManager::_early_subacks.erase(MqttManager::_early_subacks.begin());
  }
  MqttManager::_early_subacks.push_back({msg_id, failed, received_ms});
}

void MqttManager::_task_manage_subscriptions(void *param) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (!MqttManager::connected()) {
      continue; // Everything is subscribed again on MQTT_EVENT_CONNECTED.
    }

    // Send queued UNSUBSCRIBEs first, in order, so that unsubscribe(topic) followed by
    // subscribe(topic) reaches the broker in that order. If one fails, send nothing else
    // this round to keep that ordering.
    bool unsubscribes_done = true;
    for (;;) {
      xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
      if (MqttManager::_pending_unsubscribes.empty()) {
        xSemaphoreGive(MqttManager::_subscriptions_mutex);
        break;
      }
      std::string topic = MqttManager::_pending_unsubscribes.front();
      xSemaphoreGive(MqttManager::_subscriptions_mutex);

      int result_code = esp_mqtt_client_unsubscribe(MqttManager::_mqtt_client, topic.c_str());

      xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
      if (result_code >= 0) {
        // The queue may have been cleared by a reconnect while we were sending.
        if (!MqttManager::_pending_unsubscribes.empty() && MqttManager::_pending_unsubscribes.front() == topic) {
          MqttManager::_pending_unsubscribes.erase(MqttManager::_pending_unsubscribes.begin());
        }
        xSemaphoreGive(MqttManager::_subscriptions_mutex);
      } else {
        xSemaphoreGive(MqttManager::_subscriptions_mutex);
        ESP_LOGE("MqttManager", "Failed to unsubscribe from '%s'. Got return code: %d. Will retry.", topic.c_str(), result_code);
        unsubscribes_done = false;
        break;
      }
    }
    if (!unsubscribes_done) {
      continue;
    }

    // Pick the subscriptions that need a SUBSCRIBE and mark them in flight so the next
    // round does not send them again while we are still sending. Stop at SUBSCRIBE_MAX_IN_FLIGHT:
    // every send locks the MQTT client task out for the duration of its write, and each SUBACK
    // notifies this task, so the remainder goes out as soon as slots free up.
    std::vector<std::string> topics_to_subscribe;
    int64_t now = millis();
    xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
    size_t in_flight = 0;
    for (const Subscription &subscription : MqttManager::_subscriptions) {
      if (subscription.state == SubscriptionState::IN_FLIGHT && now - subscription.sent_ms <= SUBACK_TIMEOUT_MS) {
        in_flight++;
      }
    }
    for (Subscription &subscription : MqttManager::_subscriptions) {
      if (subscription.state == SubscriptionState::PENDING && now >= subscription.next_attempt_ms) {
        // Never sent, or a retry that has waited out its backoff.
      } else if (subscription.state == SubscriptionState::IN_FLIGHT && subscription.msg_id >= 0 && now - subscription.sent_ms > SUBACK_TIMEOUT_MS) {
        ESP_LOGW("MqttManager", "No SUBACK for '%s' after %dms. Sending SUBSCRIBE again.", subscription.topic.c_str(), SUBACK_TIMEOUT_MS);
      } else {
        continue;
      }
      if (in_flight >= SUBSCRIBE_MAX_IN_FLIGHT) {
        break;
      }
      in_flight++;
      subscription.state = SubscriptionState::IN_FLIGHT;
      subscription.msg_id = -1;
      subscription.sent_ms = now;
      if (subscription.attempts < UINT8_MAX) [[likely]] {
        subscription.attempts++;
      }
      topics_to_subscribe.push_back(subscription.topic);
    }
    xSemaphoreGive(MqttManager::_subscriptions_mutex);

    for (const std::string &topic : topics_to_subscribe) {
      // Can block for up to the network timeout on a weak link. That is fine here: no one waits on this task.
      int result_code = esp_mqtt_client_subscribe_single(MqttManager::_mqtt_client, topic.c_str(), 2);

      xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
      for (Subscription &subscription : MqttManager::_subscriptions) {
        // Skip if the subscription was removed, re-added or reset by a reconnect while sending.
        if (subscription.topic != topic || subscription.state != SubscriptionState::IN_FLIGHT || subscription.msg_id != -1) {
          continue;
        }
        if (result_code >= 0) {
          subscription.msg_id = result_code;
          subscription.sent_ms = millis();
          for (auto it = MqttManager::_early_subacks.begin(); it != MqttManager::_early_subacks.end(); ++it) {
            // Ignore anything too old to be the SUBACK for the SUBSCRIBE just sent: message
            // ids wrap at 65535, so a stale entry can collide with a freshly issued id.
            if (it->msg_id == result_code && subscription.sent_ms - it->received_ms <= SUBACK_TIMEOUT_MS) {
              bool failed = it->failed;
              int64_t received_ms = it->received_ms;
              MqttManager::_early_subacks.erase(it);
              MqttManager::_handle_suback(result_code, failed, received_ms);
              break;
            }
          }
        } else {
          subscription.state = SubscriptionState::PENDING;
          uint32_t backoff_ms = std::min(1000 << std::min<uint8_t>(subscription.failures, 5), SUBSCRIBE_MAX_BACKOFF_MS);
          subscription.next_attempt_ms = millis() + backoff_ms;
          if (subscription.failures < UINT8_MAX) [[likely]] {
          subscription.failures++; // Saturate rather than wrap back round to no backoff.
        }
          ESP_LOGE("MqttManager", "Failed to subscribe to '%s'. Got return code: %d. Will retry in %lums.", topic.c_str(), result_code, backoff_ms);
        }
        break;
      }
      xSemaphoreGive(MqttManager::_subscriptions_mutex);
    }
  }
}

void MqttManager::_log_subscribe_latency(esp_mqtt_event_handle_t event) {
  // Only the first fragment of a message carries its topic. A broker sets the retain flag only on the
  // replay a new SUBSCRIBE triggers, never on live updates, so a topic with no retained value logs nothing.
  if (event->current_data_offset != 0 || event->topic_len <= 0 || !event->retain) {
    return;
  }
  int64_t now = millis();
  std::string topic(event->topic, event->topic_len);

  Subscription found;
  bool log = false;
  xSemaphoreTake(MqttManager::_subscriptions_mutex, portMAX_DELAY);
  for (Subscription &subscription : MqttManager::_subscriptions) {
    if (subscription.topic == topic) {
      if (!subscription.retained_seen && subscription.requested_ms > 0) {
        subscription.retained_seen = true;
        found = subscription;
        log = true;
      }
      break;
    }
  }
  xSemaphoreGive(MqttManager::_subscriptions_mutex);
  if (!log) {
    return;
  }

  int64_t total_ms = now - found.requested_ms;
  // sent_ms is the latest SUBSCRIBE, so with several attempts the queue figure includes the earlier ones.
  int64_t queued_ms = found.sent_ms - found.requested_ms;
  // sent_ms is stamped once esp_mqtt_client_subscribe_single() has returned, and a quick SUBACK can be
  // received on the MQTT task before then, so this can come out a millisecond or so negative.
  int64_t suback_ms = std::max<int64_t>(found.suback_ms - found.sent_ms, 0);
  LatencyHistogram *latency = PanelMetrics::subscribe_latency[found.after_reconnect ? PanelMetrics::CAUSE_RECONNECT : PanelMetrics::CAUSE_SUBSCRIBE];
  latency[PanelMetrics::STAGE_TOTAL].observe_ms(total_ms);
  latency[PanelMetrics::STAGE_QUEUED].observe_ms(queued_ms);
  if (found.suback_ms > 0) {
    latency[PanelMetrics::STAGE_SUBACK].observe_ms(suback_ms);
    latency[PanelMetrics::STAGE_DATA].observe_ms(now - found.suback_ms);
  }
  const char *cause = found.after_reconnect ? "reconnect" : "subscribe()";
  // Reconnects replay every topic at once, so only a slow subscribe() is worth an INFO line.
  esp_log_level_t level = (!found.after_reconnect && total_ms >= SUBSCRIBE_LATENCY_INFO_MS) ? ESP_LOG_INFO : ESP_LOG_DEBUG;
  if (found.suback_ms > 0) {
    ESP_LOG_LEVEL_LOCAL(level, "MqttManager", "Retained value for '%s' arrived %lldms after %s (SUBSCRIBE sent +%lldms, SUBACK +%lldms, data +%lldms, %u attempt(s)).",
                        topic.c_str(), total_ms, cause, queued_ms, suback_ms, now - found.suback_ms, found.attempts);
  } else {
    // The SUBACK has not been matched to this subscription yet (see _early_subacks).
    ESP_LOG_LEVEL_LOCAL(level, "MqttManager", "Retained value for '%s' arrived %lldms after %s (SUBSCRIBE sent +%lldms, data +%lldms, SUBACK not yet matched, %u attempt(s)).",
                        topic.c_str(), total_ms, cause, queued_ms, now - found.sent_ms, found.attempts);
  }
}

esp_err_t MqttManager::publish(std::string topic, const char *data, size_t length, bool retain) {
  if (MqttManager::connected()) {
    int64_t start_us = esp_timer_get_time();
    int result_code = esp_mqtt_client_publish(MqttManager::_mqtt_client, topic.c_str(), data, length, 0, retain);
    PanelMetrics::publish_duration.observe_us(esp_timer_get_time() - start_us);
    if (result_code >= 0) {
      return ESP_OK;
    } else {
      PanelMetrics::publish_failures_error.add();
      ESP_LOGE("MqttManager", "Failed to publish to '%s'. Got return code: %d", topic.c_str(), result_code);
    }
  } else {
    PanelMetrics::publish_failures_disconnected.add();
    ESP_LOGE("MqttManager", "Failed to publish to MQTT topic. Not connected to MQTT server.");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t MqttManager::register_handler(esp_mqtt_event_id_t event_id, esp_event_handler_t event_handler, void *event_handler_arg) {
  return esp_mqtt_client_register_event(MqttManager::_mqtt_client, event_id, event_handler, event_handler_arg);
}

esp_err_t MqttManager::unregister_handler(esp_mqtt_event_id_t event_id, esp_event_handler_t event_handler) {
  return esp_mqtt_client_unregister_event(MqttManager::_mqtt_client, event_id, event_handler);
}