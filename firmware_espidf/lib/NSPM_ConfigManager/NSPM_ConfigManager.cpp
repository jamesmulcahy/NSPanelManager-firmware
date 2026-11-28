#include "NSPM_ConfigManager.hpp"
#include <BuildIdentity.hpp>
#include <ConfigManager.hpp>
#include <MqttManager.hpp>
#include <MqttSubscriptions.hpp>
#include <PsramProtobuf.hpp>
#include <PanelMetrics.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <NSPM_version.hpp>
#include <WebManager.hpp>
#include <WebUiEmbedded.hpp>
#include <WiFiManager.hpp>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nlohmann/json.hpp>
#include <string.h>

ESP_EVENT_DEFINE_BASE(NSPM_CONFIGMANAGER_EVENT);

void NSPM_ConfigManager::init() {
  esp_log_level_set("NSPM_ConfigManager", ConfigManager::log_level);
  ESP_LOGI("NSPM_ConfigManager", "Initializing NSPM_ConfigManager.");
  NSPM_ConfigManager::_config_mutex = xSemaphoreCreateMutex();
  NSPM_ConfigManager::_register_request_task_mutex = xSemaphoreCreateMutex();
  if (xTaskCreatePinnedToCore(NSPM_ConfigManager::_task_post_config_loaded, "post_cfg_loaded", 2048, NULL, 2, &NSPM_ConfigManager::_task_post_config_loaded_handle, 1) != pdPASS) {
    ESP_LOGE("NSPM_ConfigManager", "Failed to create task to post CONFIG_LOADED. Will post inline instead.");
    NSPM_ConfigManager::_task_post_config_loaded_handle = NULL;
  }
  MqttManager::add_connection_listener(&NSPM_ConfigManager::_mqtt_connection_listener, PanelMetrics::HANDLER_NSPM_CONFIG_MANAGER);

  // Subscribe to the topic the manager answers register_request on. It is keyed on the MAC,
  // so it survives the manager renaming the panel.
  NSPM_ConfigManager::_mqtt_command_topic = "nspanel/";
  NSPM_ConfigManager::_mqtt_command_topic.append(WiFiManager::mac_string());
  NSPM_ConfigManager::_mqtt_command_topic.append("/command");
  // Wait until subscribe is successful. A COMMAND hold: every message is delivered once and never replayed.
  for (;;) {
    NSPM_ConfigManager::_command_subscription = mqtt::subscribe(NSPM_ConfigManager::_mqtt_command_topic, mqtt::Kind::COMMAND, &NSPM_ConfigManager::_mqtt_command_handler, NULL, PanelMetrics::HANDLER_NSPM_CONFIG_MANAGER);
    if (NSPM_ConfigManager::_command_subscription.active()) {
      break;
    }
    ESP_LOGE("NSPM_ConfigManager", "Tried to subscribe to NSPanel command topic '%s' but subscribe call was unsuccessful! Will try again.", NSPM_ConfigManager::_mqtt_command_topic.c_str());
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // The manager status topic is deliberately not subscribed here. _manager_address is only
  // set once a register_accept arrives, so at this point the topic would be
  // "nspanel/mqttmanager_/status/status", which nothing ever publishes to.
  // _handle_new_config_data() subscribes to the real topic as soon as the address is known,
  // and moves it if the manager ever changes address.

  // We have now subscribed to MQTT command topic, start the task to send MQTT register_requests for managers to answer to
  NSPM_ConfigManager::_start_register_request_task();
}

static inline void *generation_context(uint32_t generation) { return reinterpret_cast<void *>(static_cast<uintptr_t>(generation)); }
static inline bool current(void *context, const std::atomic<uint32_t> &generation) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context)) == generation.load(); }

void NSPM_ConfigManager::_mqtt_command_handler(const mqtt::Message &message, void *context) {
  // This topic also carries other commands (reboot, updates), which _handle_register_accept() ignores.
  NSPM_ConfigManager::_handle_register_accept(reinterpret_cast<const char *>(message.payload.data()), message.payload.size());
}

void NSPM_ConfigManager::_mqtt_config_handler(const mqtt::Message &message, void *context) {
  if (message.payload.empty() || !current(context, NSPM_ConfigManager::_config_generation)) {
    return; // A cleared retained config, or a delivery from a config topic that has been replaced since.
  }
  NSPM_ConfigManager::_handle_new_config_data(reinterpret_cast<const char *>(message.payload.data()), message.payload.size());
}

void NSPM_ConfigManager::_mqtt_manager_status_handler(const mqtt::Message &message, void *context) {
  if (message.payload.empty() || !current(context, NSPM_ConfigManager::_manager_status_generation)) {
    return; // A cleared retained status, or a delivery from a topic that has been replaced since.
  }
  const char *data = reinterpret_cast<const char *>(message.payload.data());
  size_t data_length = message.payload.size();
  if (!NSPM_ConfigManager::_manager_online && strncmp("online", data, data_length) == 0) {
    ESP_LOGI("NSPM_ConfigManager", "Manager is online!");
    NSPM_ConfigManager::_manager_online = true;
    esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::MANAGER_STATE_CHANGE, NULL, 0, pdMS_TO_TICKS(250));
  } else if (NSPM_ConfigManager::_manager_online && strncmp("offline", data, data_length) == 0) {
    ESP_LOGE("NSPM_ConfigManager", "Manager is offline!");
    NSPM_ConfigManager::_manager_online = false;
    esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::MANAGER_STATE_CHANGE, NULL, 0, pdMS_TO_TICKS(250));
  }
}

void NSPM_ConfigManager::_mqtt_connection_listener(bool connected) {
  // Runs on the MQTT client task.
  if (connected) {
    NSPM_ConfigManager::_mqtt_connected = true;
    // MqttManager re-subscribes the command, manager status and config topics after a reconnect.

    // The broker replays the retained manager status, but a replay identical to the last one delivered is not
    // delivered again. The disconnect cleared _manager_online, so put back what the last status said: a replay that
    // differs (the manager went offline meanwhile) is delivered and overrides it. Repeated disconnects without a
    // connect in between keep the first saved value.
    if (NSPM_ConfigManager::_manager_online_before_disconnect && !NSPM_ConfigManager::_manager_online.exchange(true)) {
      esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::MANAGER_STATE_CHANGE, NULL, 0, pdMS_TO_TICKS(250));
    }

    // Re-announce ourselves to any manager listening. Covers the case where
    // the manager container restarted and lost its in-memory panel state:
    // without a fresh register_request the manager would never send us
    // config, and we would sit forever on "Lost connection to manager".
    if (!NSPM_ConfigManager::_manager_address.empty()) {
      NSPM_ConfigManager::_start_register_request_task();
    }
  } else {
    if (NSPM_ConfigManager::_mqtt_connected) {
      NSPM_ConfigManager::_manager_online_before_disconnect = NSPM_ConfigManager::_manager_online.exchange(false);
      NSPM_ConfigManager::_mqtt_connected = false;
    }
    esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::MANAGER_STATE_CHANGE, NULL, 0, pdMS_TO_TICKS(250));
  }
}

void NSPM_ConfigManager::_handle_register_accept(const char *data, size_t data_length) {
  // An empty message is the manager clearing a retained message on this topic, not a command.
  if (data_length == 0) {
    return;
  }

  nlohmann::json json = nlohmann::json::parse(data, data + data_length, nullptr, false);
  if (json.is_discarded()) {
    ESP_LOGE("NSPM_ConfigManager", "Failed to parse register accept message!");
    return;
  }

  if (!json.contains("command") || !json["command"].is_string() || json["command"].get<std::string>().compare("register_accept") != 0) {
    return; // Not a "register_accept", e.g. a reboot or update command on the MAC topic.
  }

  if (json.contains("address") && json["address"].is_string() && json["address"].get<std::string>().length() > 0) {
    NSPM_ConfigManager::_manager_address = json["address"].get<std::string>();
  } else {
    ESP_LOGE("NSPM_ConfigManager", "register_accept does not contain valid 'address' field.");
    return;
  }

  if (json.contains("port") && json["port"].is_number_integer() && json["port"].get<int32_t>() > 0) {
    NSPM_ConfigManager::_manager_port = json["port"];
  } else {
    ESP_LOGE("NSPM_ConfigManager", "register_accept does not contain valid 'port' field.");
    return;
  }

  if (json.contains("config_topic") && json["config_topic"].is_string() && json["config_topic"].get<std::string>().length() > 0) {
    std::string config_topic = json["config_topic"].get<std::string>();
    // The old config topic is released when its handle is replaced below.
    NSPM_ConfigManager::_mqtt_config_topic = config_topic;
  } else {
    ESP_LOGE("NSPM_ConfigManager", "register_accept does not contain valid 'config_topic' field.");
    return;
  }

  ESP_LOGI("NSPM_ConfigManager", "Received register_accept from manager. Registered to manager at %s:%d", NSPM_ConfigManager::_manager_address.c_str(), NSPM_ConfigManager::_manager_port);
  NSPM_ConfigManager::_mqtt_manager_command_topic = "nspanel/mqttmanager_";
  NSPM_ConfigManager::_mqtt_manager_command_topic.append(NSPM_ConfigManager::_manager_address);
  NSPM_ConfigManager::_mqtt_manager_command_topic.append("/command");
  // Subscribe to where the NSPanel Manager container will send the config for this panel.
  // Only stop sending register_requests once that has succeeded, otherwise the panel is
  // registered with no way to receive a config and nothing left to retry.
  // A repeated register_accept for the topic already held changes nothing: replacing the hold would only get the
  // cached config delivered again.
  if (!NSPM_ConfigManager::_config_subscription.active() || NSPM_ConfigManager::_config_subscription_topic != NSPM_ConfigManager::_mqtt_config_topic) {
    uint32_t generation = ++NSPM_ConfigManager::_config_generation;
    NSPM_ConfigManager::_config_subscription = mqtt::subscribe(NSPM_ConfigManager::_mqtt_config_topic, mqtt::Kind::STATE, &NSPM_ConfigManager::_mqtt_config_handler, generation_context(generation),
                                                               PanelMetrics::HANDLER_NSPM_CONFIG_MANAGER);
    NSPM_ConfigManager::_config_subscription_topic = NSPM_ConfigManager::_mqtt_config_topic;
  }
  if (!NSPM_ConfigManager::_config_subscription.active()) {
    ESP_LOGE("NSPM_ConfigManager", "Failed to subscribe to NSPanel config topic '%s'. Will keep sending register_requests.", NSPM_ConfigManager::_mqtt_config_topic.c_str());
    return;
  }
  NSPM_ConfigManager::_send_register_requests = false;

  ESP_LOGI("NSPM_ConfigManager", "Register accept fully processed. Subscribed to panel config topic: %s", NSPM_ConfigManager::_mqtt_config_topic.c_str());
}

void NSPM_ConfigManager::_handle_new_config_data(const char *data, size_t data_length) {
  ESP_LOGD("NSPM_ConfigManager", "Received new config data, start processing.");
  if (xSemaphoreTake(NSPM_ConfigManager::_config_mutex, pdMS_TO_TICKS(5000))) {
    // Every delivery is applied. A reconnect's replay of the config already delivered never gets here (the
    // subscription core drops a replay identical to the last value), but a manager that publishes the same config
    // again does, and re-applying is cheap: _notify_config_loaded() collapses a burst of deliveries into a single
    // CONFIG_LOADED.
    bool trigger_new_config_event = false;
    NSPanelConfig *new_config = nspanel_config__unpack(psram_protobuf_allocator(), data_length, (const uint8_t *)data);
    if (new_config != NULL) [[likely]] {
      NSPM_ConfigManager::_config = std::shared_ptr<NSPanelConfig>(new_config, &NSPM_ConfigManager::_delete_nspanelconfig_object_from_shared_ptr);
      trigger_new_config_event = true;
    } else {
      ESP_LOGE("NSPM_ConfigManager", "Received new config but failed to parse into protobuf object.");
    }
    xSemaphoreGive(NSPM_ConfigManager::_config_mutex);

    // Move the manager status subscription if the manager address has changed.
    std::string new_manager_status_topic = "nspanel/mqttmanager_";
    new_manager_status_topic.append(NSPM_ConfigManager::get_manager_address());
    new_manager_status_topic.append("/status/status");
    if (new_manager_status_topic != NSPM_ConfigManager::_mqtt_manager_status_topic) {
      // Replacing the handle releases the old topic (none on the first config of this boot).
      NSPM_ConfigManager::_mqtt_manager_status_topic = new_manager_status_topic;
      uint32_t generation = ++NSPM_ConfigManager::_manager_status_generation;
      NSPM_ConfigManager::_manager_status_subscription = mqtt::subscribe(new_manager_status_topic, mqtt::Kind::STATE, &NSPM_ConfigManager::_mqtt_manager_status_handler, generation_context(generation),
                                                                         PanelMetrics::HANDLER_NSPM_CONFIG_MANAGER);
      if (!NSPM_ConfigManager::_manager_status_subscription.active()) {
        ESP_LOGE("NSPM_ConfigManager", "Failed to subscribe to manager status topic.");
      }
    }

    if (trigger_new_config_event) {
      ESP_LOGI("NSPM_ConfigManager", "Received new config data from MQTT, will trigger event.");
      NSPM_ConfigManager::_notify_config_loaded();
    }

    if (ConfigManager::wifi_hostname.compare(new_config->name) != 0) {
      ESP_LOGI("NSPM_ConfigManager", "Updating internal WiFi hostname to match manager provided friendly name.");
      std::string name = new_config->name;
      ConfigManager::change(CONFIG_SAVE_BY_PANEL_NAME, [&name]() { ConfigManager::wifi_hostname = std::move(name); });
    }
  } else {
    ESP_LOGE("NSPM_ConfigManager", "Failed to gain config mutex while processing new config from MQTT!");
  }
}

void NSPM_ConfigManager::_task_post_config_loaded(void *arg) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Zero timeout: never wait on the default event loop queue, it is shared with the
    // Wi-Fi events that drive reconnection. Back off here instead.
    while (esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, NULL, 0, 0) != ESP_OK) {
      ESP_LOGW("NSPM_ConfigManager", "Default event loop full, could not post CONFIG_LOADED. Will retry in 500ms.");
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGD("NSPM_ConfigManager", "Posted CONFIG_LOADED.");
  }
}

void NSPM_ConfigManager::_notify_config_loaded() {
  if (NSPM_ConfigManager::_task_post_config_loaded_handle != NULL) [[likely]] {
    // Notifications that land while a post is in flight are kept by the notification
    // counter, so the task always posts again after the most recent config.
    xTaskNotifyGive(NSPM_ConfigManager::_task_post_config_loaded_handle);
    return;
  }

  // The task could not be created at init. Post inline as a last resort, accepting that
  // this blocks the caller on the shared queue.
  if (esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, NULL, 0, pdMS_TO_TICKS(250)) != ESP_OK) {
    ESP_LOGE("NSPM_ConfigManager", "Failed to post CONFIG_LOADED inline. Components still hold the previous config.");
  }
}

void NSPM_ConfigManager::_delete_nspanelconfig_object_from_shared_ptr(NSPanelConfig *config) {
  nspanel_config__free_unpacked(config, psram_protobuf_allocator());
}

esp_err_t NSPM_ConfigManager::get_config(std::shared_ptr<NSPanelConfig> *config) {
  if (NSPM_ConfigManager::_config != NULL) {
    if (xSemaphoreTake(NSPM_ConfigManager::_config_mutex, pdMS_TO_TICKS(5000))) {
      *config = NSPM_ConfigManager::_config;
      xSemaphoreGive(NSPM_ConfigManager::_config_mutex);
      return ESP_OK;
    } else {
      ESP_LOGE("NSPM_ConfigManager", "Failed to gain config mutex while processing request for config from other task!");
    }
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t NSPM_ConfigManager::get_mutable_config(std::shared_ptr<NSPanelConfig> *config) {
  if (NSPM_ConfigManager::_config != NULL) {
    if (xSemaphoreTake(NSPM_ConfigManager::_config_mutex, pdMS_TO_TICKS(5000))) {
      size_t config_pack_size = nspanel_config__get_packed_size(NSPM_ConfigManager::_config.get());
      std::vector<uint8_t> buffer(config_pack_size);
      nspanel_config__pack(NSPM_ConfigManager::_config.get(), buffer.data());
      NSPanelConfig *temp_config;

      temp_config = nspanel_config__unpack(psram_protobuf_allocator(), config_pack_size, buffer.data());
      if (temp_config != NULL) [[likely]] {
        xSemaphoreGive(NSPM_ConfigManager::_config_mutex);
        (*config) = std::shared_ptr<NSPanelConfig>(temp_config, &NSPM_ConfigManager::_delete_nspanelconfig_object_from_shared_ptr);
        return ESP_OK;
      } else {
        xSemaphoreGive(NSPM_ConfigManager::_config_mutex);
        return ESP_ERR_NOT_FINISHED;
      }
    } else {
      ESP_LOGE("NSPM_ConfigManager", "Failed to gain config mutex while processing request for mutable config from other task!");
    }
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t NSPM_ConfigManager::replace_config(std::shared_ptr<NSPanelConfig> *config) {
  if (NSPM_ConfigManager::_config != NULL) {
    if (xSemaphoreTake(NSPM_ConfigManager::_config_mutex, pdMS_TO_TICKS(5000))) {
      NSPM_ConfigManager::_config = *config;
      xSemaphoreGive(NSPM_ConfigManager::_config_mutex);
      NSPM_ConfigManager::_notify_config_loaded();
      return ESP_OK;
    } else {
      ESP_LOGE("NSPM_ConfigManager", "Failed to gain config mutex while replacing config from other task!");
    }
  }
  return ESP_ERR_NOT_FINISHED;
}

int32_t NSPM_ConfigManager::get_nspanel_id() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    return config->nspanel_id;
  }
  ESP_LOGE("NSPM_ConfigManager", "Failed to get config to get nspanel ID!");
  return 0;
}

std::string NSPM_ConfigManager::get_manager_address() {
  return NSPM_ConfigManager::_manager_address;
}

uint16_t NSPM_ConfigManager::get_manager_port() {
  return NSPM_ConfigManager::_manager_port;
}

std::string NSPM_ConfigManager::get_manager_command_topic() {
  return NSPM_ConfigManager::_mqtt_manager_command_topic;
}

bool NSPM_ConfigManager::get_manager_online() {
  return NSPM_ConfigManager::_manager_online;
}

void NSPM_ConfigManager::_task_send_register_request(void *arg) {
  // Get MAC address string
  const char *mac_str = WiFiManager::mac_string(); // Format: AA:BB:CC:DD:EE:FF

  // Get IP address string
  char ip_address_str[IP4ADDR_STRLEN_MAX];
  esp_netif_ip_info_t ip_info = WiFiManager::ip_info();
  sprintf(ip_address_str, IPSTR, IP2STR(&ip_info.ip));

  nlohmann::json json;
  json["command"] = "register_request";
  json["mac_origin"] = mac_str;
  json["friendly_name"] = ConfigManager::wifi_hostname.c_str();
  json["version"] = NSPM_VERSION;
  json["md5_firmware"] = ConfigManager::md5_firmware.c_str();
  json["md5_data_file"] = NSPM_WEB_UI_EMBEDDED;
  json["md5_tft_file"] = ConfigManager::md5_gui.c_str();
  json["address"] = ip_address_str;
  // What this firmware does that a manager may rely on. "lvgl_look": it applies the look in
  // NSPanelConfig (lvgl_theme, lvgl_background) and reports a look chosen on the panel as an
  // NSPanelMQTTManagerCommand (lvgl_look, LvglLook), so the manager need not send the lvgl_theme and
  // lvgl_background commands. "no_hmi": it has no Nextion HMI pages, so on NSPM's legacy TFT it shows only a
  // "Display update needed" screen (display_mode DISPLAY_MODE_NEXTION) until the terminal TFT is installed.
  // What the board lacks is sent as negative flags, so a manager that predates them assumes the board has it:
  // "no_tft": no Nextion display, so no GUI (TFT) update to offer. "no_temperature": no room temperature sensor.
  // "no_buttons": no physical buttons. "no_relays": no physical relays. (Humidity and pressure are in the status
  // report, not here.) Older managers ignore keys they do not know.
#if defined(BOARD_CUSTOM)
  json["features"] = nlohmann::json::array({"lvgl_look", "no_hmi", "no_tft", "no_temperature", "no_buttons", "no_relays"});
#else
  json["features"] = nlohmann::json::array({"lvgl_look", "no_hmi"});
#endif
  // WebManager serves GET /metrics (Prometheus text) on this port. The manager lists only panels
  // that send this in its Prometheus service discovery.
  json["metrics_port"] = WebManager::HTTP_PORT;
  json["model"] = BOARD_MODEL;
  // Which build this is (version, image SHA-256, ...). Managers that predate it ignore the key.
  json["build"] = BuildIdentity::get().to_json();
  std::string json_string = json.dump();

  // Publish register_request every 5s until _send_register_requests goes false
  // (register_accept received). After the loop we re-check the flag under the
  // mutex: if _start_register_request_task re-armed it between our last check
  // and the mutex acquire, we loop again instead of exiting, which closes the
  // race that would otherwise leave the flag set with no live task.
  for (;;) {
    while (NSPM_ConfigManager::_send_register_requests) {
      // The manager answers on the command topic at once, and so may a command right behind its
      // register_accept. Until the broker has acknowledged our SUBSCRIBE to that topic, both would be
      // dropped and the panel would only register on the next attempt, 5 s later. Wait for the SUBACK
      // (a few ms), but never longer than one attempt's interval.
      for (int waited_ms = 0; !mqtt::acknowledged(NSPM_ConfigManager::_mqtt_command_topic) && waited_ms < 5000; waited_ms += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      MqttManager::publish("nspanel/mqttmanager/command", json_string.c_str(), json_string.length(), false);
      vTaskDelay(pdMS_TO_TICKS(5000));
    }

    xSemaphoreTake(NSPM_ConfigManager::_register_request_task_mutex, portMAX_DELAY);
    if (!NSPM_ConfigManager::_send_register_requests) {
      NSPM_ConfigManager::_task_send_register_request_handle = NULL;
      xSemaphoreGive(NSPM_ConfigManager::_register_request_task_mutex);
      break;
    }
    xSemaphoreGive(NSPM_ConfigManager::_register_request_task_mutex);
  }

  vTaskDelete(NULL); // Delete own task.
}

void NSPM_ConfigManager::_start_register_request_task() {
  xSemaphoreTake(NSPM_ConfigManager::_register_request_task_mutex, portMAX_DELAY);
  NSPM_ConfigManager::_send_register_requests = true;
  if (NSPM_ConfigManager::_task_send_register_request_handle == NULL) {
    xTaskCreatePinnedToCore(NSPM_ConfigManager::_task_send_register_request, "register_request_task", 4096, NULL, 2, &NSPM_ConfigManager::_task_send_register_request_handle, 1);
  }
  xSemaphoreGive(NSPM_ConfigManager::_register_request_task_mutex);
}
