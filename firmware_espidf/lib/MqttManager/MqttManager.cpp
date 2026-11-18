#include <PanelRestart.hpp>
#include <ConfigManager.hpp>
#include <MqttManager.hpp>
#include <iterator>
#include <EspMqttTransport.hpp>
#include <MqttSubscriptions.hpp>
#include <PanelMetrics.hpp>
#include <NSPM_ConfigManager.hpp>
#include <WiFiManager.hpp>
#include <esp_log.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_transport.h>
#include <cstdio>
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
// Number of unmatched SUBACKs/UNSUBACKs to remember (see Subscriptions::_remember_early_ack). Never
// needs to hold more than SUBSCRIBE_MAX_IN_FLIGHT of them, with room to spare.
#define EARLY_ACKS_MAX 32
// Retained values that take at least this long to arrive after subscribe() are logged at INFO rather than DEBUG,
// so panels that only log INFO and above still report slow page switches.
#define SUBSCRIBE_LATENCY_INFO_MS 250

void MqttManager::_record_handler_duration(int id, int64_t elapsed_us) {
  PanelMetrics::mqtt_handler_duration[id].observe_us(elapsed_us);
}

// Handle of the subscription task, notified whenever there is work to do.
static TaskHandle_t manage_subscriptions_task_handle = NULL;

static void wake_subscriptions() {
  if (manage_subscriptions_task_handle != NULL) [[likely]] {
    xTaskNotifyGive(manage_subscriptions_task_handle);
  }
}

// Handle of the delivery task (mqtt_dispatch), notified whenever there are callbacks to run.
static TaskHandle_t dispatch_task_handle = NULL;

static void wake_dispatch() {
  if (dispatch_task_handle != NULL) [[likely]] {
    xTaskNotifyGive(dispatch_task_handle);
  }
}

// mqtt_dispatch's stack. Internal RAM: callbacks may write flash, and a task with a PSRAM stack
// must never touch flash. Watch nspanel_task_stack_unused_bytes{task="mqtt_dispatch"} as consumers move to it.
// Measured on a panel at 5 KB: a thermostat state decode left about 2.2 KB free (protobuf-c recurses per
// nested message), before the config moves here. nspanel_mqtt_handler_stack_peak_bytes says which handler is deepest.
#define DISPATCH_TASK_STACK 6144
// Below the MQTT client (10), so on_data() returns before its callbacks run, and above the page
// tasks (6), so a delivery is not held up behind drawing.
#define DISPATCH_TASK_PRIORITY 7
// How long to wait between attempts to publish the online status.
#define ONLINE_STATUS_RETRY_MS 5000

// Times each delivered callback in nspanel_mqtt_handler_duration_seconds, keyed by the metrics id
// passed to mqtt::subscribe() (a PanelMetrics::MqttHandler). An id out of range is not recorded.
static void record_callback_duration(int id, int64_t elapsed_us) {
  if (id >= 0 && id < PanelMetrics::HANDLER_COUNT) [[likely]] {
    PanelMetrics::mqtt_handler_duration[id].observe_us(elapsed_us);
  }
}

// Lowest free stack mqtt_dispatch has had so far, in bytes (uxTaskGetStackHighWaterMark() is in bytes on
// ESP-IDF). Only touched on mqtt_dispatch. Logged whenever a callback drives it lower, to find which
// consumer is the deepest.
static UBaseType_t dispatch_stack_low = UINT32_MAX;

static void log_dispatch_stack_after(int id, std::string_view topic) {
  UBaseType_t free_now = uxTaskGetStackHighWaterMark(NULL); // Read before logging: the log call uses stack.
  if (free_now >= dispatch_stack_low) [[likely]] {
    return;
  }
  UBaseType_t was = dispatch_stack_low;
  dispatch_stack_low = free_now;
  // Also a metric: lows set during the boot burst are often dropped from the log (queue full).
  if (id >= 0 && id < PanelMetrics::HANDLER_COUNT) [[likely]] {
    PanelMetrics::mqtt_handler_stack_peak[id].set_max(static_cast<int32_t>(DISPATCH_TASK_STACK - free_now));
  }
  ESP_LOGI("MqttManager", "mqtt_dispatch stack low %u B free after '%.*s' (id %d), was %u", static_cast<unsigned>(free_now),
           static_cast<int>(topic.size()), topic.data(), id, static_cast<unsigned>(was));
}

// Payloads, the cache and the per-filter bookkeeping. PSRAM, falling back to internal RAM.
static void *subscriptions_alloc(size_t size) {
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

static void subscriptions_log(mqtt::LogLevel level, const char *format, va_list args) {
  static const esp_log_level_t levels[] = {ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG};
  esp_log_level_t esp_level = levels[static_cast<int>(level)];
  if (esp_level > LOG_LOCAL_LEVEL || esp_level > esp_log_level_get("MqttManager")) {
    return; // Not formatted at all, as with ESP_LOGx.
  }
  char line[160];
  vsnprintf(line, sizeof(line), format, args);
  ESP_LOG_LEVEL_LOCAL(esp_level, "MqttManager", "%s", line);
}

/**
 * Log how long the broker took to deliver the retained value of a newly subscribed topic, the first time
 * one arrives after subscribe() or a reconnect. Called on the MQTT client task.
 */
static void log_subscribe_latency(const mqtt::LatencyReport &found) {
  int64_t now = found.received_ms;
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
  int topic_len = static_cast<int>(found.topic.size());
  if (found.suback_ms > 0) {
    ESP_LOG_LEVEL_LOCAL(level, "MqttManager", "Retained value for '%.*s' arrived %lldms after %s (SUBSCRIBE sent +%lldms, SUBACK +%lldms, data +%lldms, %u attempt(s)).",
                        topic_len, found.topic.data(), total_ms, cause, queued_ms, suback_ms, now - found.suback_ms, found.attempts);
  } else {
    // The SUBACK has not been matched to this subscription yet (see the early acks in MqttSubscriptions).
    ESP_LOG_LEVEL_LOCAL(level, "MqttManager", "Retained value for '%.*s' arrived %lldms after %s (SUBSCRIBE sent +%lldms, data +%lldms, SUBACK not yet matched, %u attempt(s)).",
                        topic_len, found.topic.data(), total_ms, cause, queued_ms, now - found.sent_ms, found.attempts);
  }
}

static mqtt::Hooks subscriptions_hooks() {
  mqtt::Hooks hooks;
  hooks.now_us = &esp_timer_get_time;
  hooks.alloc = &subscriptions_alloc;
  hooks.free = &heap_caps_free;
  hooks.wake_service = &wake_subscriptions;
  hooks.wake_delivery = &wake_dispatch;
  hooks.log = &subscriptions_log;
  hooks.latency = &log_subscribe_latency;
  hooks.callback_duration = &record_callback_duration;
  hooks.callback_done = &log_dispatch_stack_after;
  return hooks;
}

static mqtt::Limits subscriptions_limits() {
  mqtt::Limits limits;
  limits.qos = 2;
  limits.max_subscribes_in_flight = SUBSCRIBE_MAX_IN_FLIGHT;
  limits.ack_timeout_ms = SUBACK_TIMEOUT_MS;
  limits.max_backoff_ms = SUBSCRIBE_MAX_BACKOFF_MS;
  limits.early_acks_max = EARLY_ACKS_MAX;
  return limits;
}

static EspMqttTransport transport;
static mqtt::Subscriptions subscriptions(transport, subscriptions_hooks(), subscriptions_limits());

void MqttManager::start(std::string *server, uint16_t *port, std::string *username, std::string *password) {
  esp_log_level_set("MqttManager", ConfigManager::log_level);
  ESP_LOGI("MqttManager", "Starting MQTTManager, will connect to %s:%d", server->c_str(), *port);
  MqttManager::_connected = false;
  if (xTaskCreatePinnedToCore(MqttManager::_task_dispatch, "mqtt_dispatch", DISPATCH_TASK_STACK, NULL, DISPATCH_TASK_PRIORITY, &dispatch_task_handle, 1) != pdPASS) {
    // No mqtt::subscribe() callback will run without this task. Legacy subscribers, which read
    // esp-mqtt's events themselves, still work.
    ESP_LOGE("MqttManager", "Failed to create MQTT dispatch task!");
    dispatch_task_handle = NULL;
  }
  if (xTaskCreatePinnedToCore(MqttManager::_task_manage_subscriptions, "mqtt_subs", 4096, NULL, 4, &manage_subscriptions_task_handle, 1) != pdPASS) {
    // Nothing will send SUBSCRIBE or UNSUBSCRIBE without this task, and the panel never announces
    // itself as online, so the manager only ever sees the retained last will. subscribe() and
    // unsubscribe() check the handle and report the failure to their callers.
    ESP_LOGE("MqttManager", "Failed to create MQTT subscription task!");
    manage_subscriptions_task_handle = NULL;
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
  transport.set_client(MqttManager::_mqtt_client);
  mqtt::set_default(&subscriptions);

  // Register event handler for MQTT events
  esp_err_t result = MqttManager::register_handler<MqttManager::_mqtt_event_handler, PanelMetrics::HANDLER_MQTT_MANAGER>(esp_mqtt_event_id_t::MQTT_EVENT_ANY, NULL);
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
    // Supersede any retry still running for an earlier connection. The new generation tells
    // mqtt_subs to publish the retained "online" status for this one, before its SUBSCRIBEs.
    MqttManager::_online_update_generation++;
    ESP_LOGI("MqttManager", "Connected to MQTT server. Online status generation %lu.", MqttManager::_online_update_generation.load());

    // Sessions are clean (disable_clean_session is not set), so the broker has dropped all
    // our subscriptions: subscribe to everything again, and there is nothing left to unsubscribe
    // from. mqtt_subs then publishes the online status first and sends the SUBSCRIBEs after it.
    subscriptions.on_connected();
    // on_connected() wakes mqtt_subs too, but the online status must not depend on that.
    wake_subscriptions();
    MqttManager::_notify_connection_listeners(true);
    break;

  case MQTT_EVENT_SUBSCRIBED:
    subscriptions.on_suback(event->msg_id, event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED);
    break;

  case MQTT_EVENT_UNSUBSCRIBED:
    subscriptions.on_unsuback(event->msg_id);
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
    // The subscription core logs subscribe latency here. Handlers registered with register_handler()
    // still receive every event as before.
    subscriptions.on_data(EspMqttTransport::data_event(event));
    break;

  case MQTT_EVENT_DISCONNECTED:
    MqttManager::_connected = false;
    PanelMetrics::mqtt_disconnects.add();
    // Stop any online-status retry still running for the connection that just dropped. This
    // handler can be running on that very task — esp_mqtt_client_publish() dispatches
    // MQTT_EVENT_DISCONNECTED inline when its write fails — so it must never delete it.
    MqttManager::_online_update_generation++;
    ESP_LOGW("MqttManager", "Lost connection to MQTT server. Online status generation %lu.", MqttManager::_online_update_generation.load());
    subscriptions.on_disconnected();
    MqttManager::_notify_connection_listeners(false);
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

bool MqttManager::add_connection_listener(void (*listener)(bool connected), int metrics_id) {
  size_t count = MqttManager::_connection_listener_count.load();
  if (count >= std::size(MqttManager::_connection_listeners)) {
    return false;
  }
  MqttManager::_connection_listeners[count] = {listener, metrics_id};
  MqttManager::_connection_listener_count.store(count + 1);
  return true;
}

void MqttManager::_notify_connection_listeners(bool connected) {
  size_t count = MqttManager::_connection_listener_count.load();
  for (size_t i = 0; i < count; i++) {
    int64_t start_us = esp_timer_get_time();
    MqttManager::_connection_listeners[i].listener(connected);
    MqttManager::_record_handler_duration(MqttManager::_connection_listeners[i].metrics_id, esp_timer_get_time() - start_us);
  }
}

void MqttManager::_send_online_update(OnlineUpdate &online) {
  // A connect and a disconnect both bump the generation. The connect handler sets _connected
  // before it bumps, and the disconnect handler clears it before it bumps, so a new generation
  // seen while connected is a connect that owes an "online".
  uint32_t generation = MqttManager::_online_update_generation.load();
  if (generation != online.generation) {
    if (online.pending) {
      ESP_LOGD("MqttManager", "Online status task parked; generation %lu -> %lu, connected=%d.",
               online.generation, generation, MqttManager::connected());
    }
    online.generation = generation;
    online.pending = MqttManager::connected();
    online.next_attempt_us = 0;
    if (online.pending) {
      ESP_LOGD("MqttManager", "Online status task woke for generation %lu.", generation);
    }
  }
  if (!online.pending || esp_timer_get_time() < online.next_attempt_us) {
    return;
  }
  // The publish can deliver MQTT_EVENT_DISCONNECTED to _mqtt_event_handler on this task before it
  // returns, so the generation may already have moved on by the time it does.
  if (MqttManager::_online_update_generation.load() == online.generation && MqttManager::connected()) {
    if (MqttManager::publish(MqttManager::_state_topic,
                             MqttManager::_online_status_message.c_str(),
                             MqttManager::_online_status_message.size(),
                             true) == ESP_OK) {
      ESP_LOGD("MqttManager", "Published online status for generation %lu.", online.generation);
    } else {
      // If the connection has gone, the next pass parks without publishing.
      ESP_LOGE("MqttManager", "Failed to publish online status to %s. Will retry in 5s.", MqttManager::_state_topic.c_str());
      online.next_attempt_us = esp_timer_get_time() + ONLINE_STATUS_RETRY_MS * 1000LL;
      return;
    }
  }
  // Published, superseded by a newer generation, or disconnected. If a wake is not followed by this
  // line, the task is still inside publish(), which is the state that used to wedge the client for good.
  ESP_LOGD("MqttManager", "Online status task parked; generation %lu -> %lu, connected=%d.",
           online.generation, MqttManager::_online_update_generation.load(), MqttManager::connected());
  online.pending = false;
}

void MqttManager::_task_dispatch(void *param) {
  dispatch_stack_low = uxTaskGetStackHighWaterMark(NULL);
  ESP_LOGI("MqttManager", "mqtt_dispatch started, %u B stack free", static_cast<unsigned>(dispatch_stack_low));
  for (;;) {
    // Waits only for work: deliver() never waits on the network, and every callback runs without
    // the subscriptions lock. A notification while deliver() runs brings it straight back.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    subscriptions.deliver();
  }
}

bool MqttManager::connected() {
  return MqttManager::_connected;
}

mqtt::Stats MqttManager::subscription_stats() {
  return subscriptions.stats();
}

esp_err_t MqttManager::subscribe(std::string topic) {
  if (manage_subscriptions_task_handle == NULL) {
    ESP_LOGE("MqttManager", "Failed to subscribe to MQTT topic '%s'. MQTT client is not started.", topic.c_str());
    return ESP_ERR_INVALID_STATE;
  }
  // One hold per topic, however many callers: subscribing again does nothing.
  return subscriptions.legacy_hold(topic) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t MqttManager::unsubscribe(std::string topic) {
  if (manage_subscriptions_task_handle == NULL) {
    ESP_LOGE("MqttManager", "Failed to unsubscribe from MQTT topic '%s'. MQTT client is not started.", topic.c_str());
    return ESP_ERR_INVALID_STATE;
  }
  // Releases the topic for every caller (FIXES_PENDING #5 is fixed only for mqtt::Subscription
  // handles). With nothing else holding it, UNSUBSCRIBE goes out at once, before any later SUBSCRIBE
  // for the same topic, so unsubscribe-then-subscribe still makes the broker replay its retained value.
  subscriptions.legacy_release(topic);
  return ESP_OK;
}

void MqttManager::_task_manage_subscriptions(void *param) {
  OnlineUpdate online;
  for (;;) {
    TickType_t wait = pdMS_TO_TICKS(1000);
    if (online.pending && online.next_attempt_us > 0) {
      // Wake for the online status retry when it is due, not up to a second later.
      int64_t until_us = online.next_attempt_us - esp_timer_get_time();
      TickType_t until = until_us > 0 ? pdMS_TO_TICKS((until_us + 999) / 1000) : 0;
      wait = std::min(wait, until);
    }
    ulTaskNotifyTake(pdTRUE, wait);
    // The online status first: on a connect, it goes out before this connection's SUBSCRIBEs, so
    // the manager is not kept waiting behind them on a weak link.
    MqttManager::_send_online_update(online);
    subscriptions.service();
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

