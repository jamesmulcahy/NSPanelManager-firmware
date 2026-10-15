#include "NSPM_ConfigManager.hpp"
#include <ConfigManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <NSPM_version.hpp>
#include <WiFiManager.hpp>
#include <esp_log.h>
#include <esp_mac.h>
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
  MqttManager::register_handler(MQTT_EVENT_ANY, &NSPM_ConfigManager::_mqtt_event_handler, NULL);

  // Subscribe to MQTT command topic
  NSPM_ConfigManager::_mqtt_command_topic = "nspanel/";
  NSPM_ConfigManager::_mqtt_command_topic.append(ConfigManager::wifi_hostname);
  NSPM_ConfigManager::_mqtt_command_topic.append("/command");
  // Wait until subscribe is successful
  while (MqttManager::subscribe(NSPM_ConfigManager::_mqtt_command_topic.c_str()) != ESP_OK) {
    ESP_LOGE("NSPM_ConfigManager", "Tried to subscribe to NSPanel command topic but subscribe call was unsuccessful! Will try again.");
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

void NSPM_ConfigManager::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == MQTT_EVENT_DATA) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    std::string topic_string = std::string(event->topic, event->topic_len);
    // esp_mqtt_client_handle_t client = event->client;

    if (NSPM_ConfigManager::_mqtt_command_topic.compare(topic_string) == 0) {
      NSPM_ConfigManager::_handle_register_accept(event->data, event->data_len);
    } else if (NSPM_ConfigManager::_mqtt_config_topic.compare(topic_string) == 0) {
      NSPM_ConfigManager::_handle_new_config_data(event->data, event->data_len);
    } else if (NSPM_ConfigManager::_mqtt_manager_status_topic.compare(topic_string) == 0) {
      if (!NSPM_ConfigManager::_manager_online && strncmp("online", event->data, event->data_len) == 0) {
        ESP_LOGI("NSPM_ConfigManager", "Manager is online!");
        NSPM_ConfigManager::_manager_online = true;
        esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::MANAGER_STATE_CHANGE, NULL, 0, pdMS_TO_TICKS(250));
      } else if (NSPM_ConfigManager::_manager_online && strncmp("offline", event->data, event->data_len) == 0) {
        ESP_LOGE("NSPM_ConfigManager", "Manager is offline!");
        NSPM_ConfigManager::_manager_online = false;
        esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::MANAGER_STATE_CHANGE, NULL, 0, pdMS_TO_TICKS(250));
      }
    }
  } else if (event_id == MQTT_EVENT_CONNECTED) {
    // MqttManager re-subscribes the command, manager status and config topics after a reconnect.

    // Re-announce ourselves to any manager listening. Covers the case where
    // the manager container restarted and lost its in-memory panel state:
    // without a fresh register_request the manager would never send us
    // config, and we would sit forever on "Lost connection to manager".
    if (!NSPM_ConfigManager::_manager_address.empty()) {
      NSPM_ConfigManager::_start_register_request_task();
    }
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    NSPM_ConfigManager::_manager_online = false;
    esp_event_post(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::MANAGER_STATE_CHANGE, NULL, 0, pdMS_TO_TICKS(250));
  }
}

void NSPM_ConfigManager::_handle_register_accept(const char *data, size_t data_length) {
  nlohmann::json json = nlohmann::json::parse(data, data + data_length, nullptr, false);
  if (json.is_discarded()) {
    ESP_LOGE("NSPM_ConfigManager", "Failed to parse register accept message!");
    return;
  }

  if (json.contains("command") && json["command"].is_string()) {
    if (json["command"].get<std::string>().compare("register_accept") != 0) {
      return; // Command we received was not a "register_accept" from manager, cancel processing.
    }
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
    if (!NSPM_ConfigManager::_mqtt_config_topic.empty() && NSPM_ConfigManager::_mqtt_config_topic.compare(config_topic) != 0) {
      MqttManager::unsubscribe(NSPM_ConfigManager::_mqtt_config_topic);
    }
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
  if (MqttManager::subscribe(NSPM_ConfigManager::_mqtt_config_topic) != ESP_OK) {
    ESP_LOGE("NSPM_ConfigManager", "Failed to subscribe to NSPanel config topic '%s'. Will keep sending register_requests.", NSPM_ConfigManager::_mqtt_config_topic.c_str());
    return;
  }
  NSPM_ConfigManager::_send_register_requests = false;

  ESP_LOGI("NSPM_ConfigManager", "Register accept fully processed. Subscribed to panel config topic: %s", NSPM_ConfigManager::_mqtt_config_topic.c_str());
}

void NSPM_ConfigManager::_handle_new_config_data(const char *data, size_t data_length) {
  ESP_LOGD("NSPM_ConfigManager", "Received new config data, start processing.");
  if (xSemaphoreTake(NSPM_ConfigManager::_config_mutex, pdMS_TO_TICKS(5000))) {
    // Every delivery is applied, including byte-identical repeats after an MQTT reconnect.
    // Filtering duplicates out here is not worth it: re-applying is cheap (ScreensaverPage
    // ::init() only re-subscribes when a topic actually changed, and MqttManager::subscribe()
    // is idempotent), and _notify_config_loaded() already collapses a burst of deliveries
    // into a single CONFIG_LOADED. A byte comparison that decides not to notify would only
    // add a way for a config to be received and then never reach anything.
    bool trigger_new_config_event = false;
    NSPanelConfig *new_config = nspanel_config__unpack(NULL, data_length, (const uint8_t *)data);
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
      // Empty on the first config of this boot: there is nothing subscribed yet to move away from.
      if (!NSPM_ConfigManager::_mqtt_manager_status_topic.empty()) {
        MqttManager::unsubscribe(NSPM_ConfigManager::_mqtt_manager_status_topic);
      }
      NSPM_ConfigManager::_mqtt_manager_status_topic = new_manager_status_topic;
      if (MqttManager::subscribe(NSPM_ConfigManager::_mqtt_manager_status_topic.c_str()) != ESP_OK) {
        ESP_LOGE("NSPM_ConfigManager", "Failed to subscribe to manager status topic.");
      }
    }

    if (trigger_new_config_event) {
      ESP_LOGI("NSPM_ConfigManager", "Received new config data from MQTT, will trigger event.");
      NSPM_ConfigManager::_notify_config_loaded();
    }

    if (ConfigManager::wifi_hostname.compare(new_config->name) != 0) {
      ESP_LOGI("NSPM_ConfigManager", "Updating internal WiFi hostname to match manager provided friendly name.");
      ConfigManager::wifi_hostname = new_config->name;
      ConfigManager::save_config();

      // register_request carries the hostname as friendly_name, and the manager
      // answers on nspanel/<friendly_name>/command. Follow the rename, or the
      // next register_accept (e.g. after an MQTT reconnect) goes unheard and the
      // panel re-sends register_request forever.
      std::string new_command_topic = "nspanel/";
      new_command_topic.append(ConfigManager::wifi_hostname);
      new_command_topic.append("/command");
      if (new_command_topic != NSPM_ConfigManager::_mqtt_command_topic) {
        MqttManager::unsubscribe(NSPM_ConfigManager::_mqtt_command_topic);
        NSPM_ConfigManager::_mqtt_command_topic = new_command_topic;
        if (MqttManager::subscribe(NSPM_ConfigManager::_mqtt_command_topic) != ESP_OK) {
          ESP_LOGE("NSPM_ConfigManager", "Failed to subscribe to renamed command topic '%s'.", NSPM_ConfigManager::_mqtt_command_topic.c_str());
        }
      }
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
  nspanel_config__free_unpacked(config, NULL);
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

      temp_config = nspanel_config__unpack(NULL, config_pack_size, buffer.data());
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
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA); // Read MAC address for Wi-Fi Station
  char mac_str[18];                    // Format: AA:BB:CC:DD:EE:FF
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

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
  json["md5_data_file"] = ConfigManager::md5_data_file.c_str();
  json["md5_tft_file"] = ConfigManager::md5_gui.c_str();
  json["address"] = ip_address_str;
#if defined(BOARD_SONOFF)
  json["model"] = "sonoff";
#elif defined(BOARD_CUSTOM)
  json["model"] = "custom";
#endif
  std::string json_string = json.dump();

  // Publish register_request every 5s until _send_register_requests goes false
  // (register_accept received). After the loop we re-check the flag under the
  // mutex: if _start_register_request_task re-armed it between our last check
  // and the mutex acquire, we loop again instead of exiting, which closes the
  // race that would otherwise leave the flag set with no live task.
  for (;;) {
    while (NSPM_ConfigManager::_send_register_requests) {
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
