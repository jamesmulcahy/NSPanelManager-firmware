#pragma once

#include <MqttSubscriptions.hpp>
#include <atomic>
#include <esp_err.h>
#include <esp_event_base.h>
#include <esp_task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <memory>
#include <protobuf_nspanel.pb-c.h>
#include <string>
#include <vector>

/**
 * NSPM_ConfigManager does not handle the config stored on the panel. NSPM_ConfigManager is responsible for loading and keeping the stored config from
 * NSPanel Manager manager container in sync. When changes occurs it fires the relevant events defined in NSPM_ConfigManager_events.hpp
 */
class NSPM_ConfigManager {
public:
  /**
   * @brief the config manager and load all relevant configuration from the manager.
   */
  static void init();

  /**
   * @brief Get a copy of the config object in order to read data from it.
   * @param config: The place to copy the NSPanelConfig object to
   * @return ESP_OK if everything was done correctly, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t get_config(std::shared_ptr<NSPanelConfig> *config);

  /**
   * @brief Get a copy of the config object in order to manipulate it and later push it in as a new config.
   * @param config: The place to copy the NSPanelConfig object to
   * @return ESP_OK if everything was done correctly, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t get_mutable_config(std::shared_ptr<NSPanelConfig> *config);

  /**
   * @brief Replace the current config object with a new one
   * @param config: The new config
   * @return ESP_OK if everything was done correctly, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t replace_config(std::shared_ptr<NSPanelConfig> *config);

  /**
   * Get the NSPanel ID as registered with the manager.
   */
  static int32_t get_nspanel_id();

  /**
   * @brief Get the manager IP address
   */
  static std::string get_manager_address();

  /**
   * @brief Get the port used to communicate with the manager
   */
  static uint16_t get_manager_port();

  /**
   * @brief Get the MQTT topic to send commands to the manager
   */
  static std::string get_manager_command_topic();

  /**
   * @brief Returns true if the status topic of the manager is true and MQTT is connected.
   */
  static bool get_manager_online();

private:
  /**
   * @brief The panel's command topic: register_accept (other commands on it are for UpdateManager). Runs on mqtt_dispatch.
   */
  static void _mqtt_command_handler(const mqtt::Message &message, void *context);

  /**
   * @brief New config from the manager. Runs on mqtt_dispatch, where the protobuf is decoded.
   */
  static void _mqtt_config_handler(const mqtt::Message &message, void *context);

  /**
   * @brief The manager's online/offline status. Runs on mqtt_dispatch.
   */
  static void _mqtt_manager_status_handler(const mqtt::Message &message, void *context);

  /**
   * @brief MQTT connect and disconnect. Runs on the MQTT client task, see MqttManager::add_connection_listener().
   */
  static void _mqtt_connection_listener(bool connected);

  /**
   * @brief sending request_requests to any manager.
   */
  static void _task_send_register_request(void *arg);

  /**
   * @brief Spawn the register-request task if it isn't already running, and
   * arm _send_register_requests so any live instance keeps publishing. Safe
   * to call from an MQTT event handler; does not block.
   */
  static void _start_register_request_task();

  /**
   * @brief Handle a "register_accept" request from MQTT
   * @param data: Received data from MQTT
   * @param data_length: Number of bytes that were received
   */
  static void _handle_register_accept(const char *data, size_t data_length);

  /**
   * @brief Handle data received on the MQTT config topic
   * @param data: Received data from MQTT
   * @param data_length: Number of bytes that were received
   */
  static void _handle_new_config_data(const char *data, size_t data_length);

  /**
   * When the shared_ptr for the NSPanelConfig goes out of scope and will delete itself, call this to clean up the underlying pointer
   */
  static void _delete_nspanelconfig_object_from_shared_ptr(NSPanelConfig *config);

  /**
   * @brief Task that raises CONFIG_LOADED on the default event loop.
   *
   * Posting from the MQTT client task is not safe with a blocking timeout: the default
   * event loop queue is shared with Wi-Fi, and a waiter on it can starve the post of
   * WIFI_EVENT_STA_DISCONNECTED, which is what triggers a reconnect. This task posts with
   * a zero timeout and backs off in its own vTaskDelay instead, so it only ever takes a
   * slot that is already free.
   *
   * CONFIG_LOADED carries no payload and _config always holds the newest config, so
   * several notifications arriving while a post is in flight collapse into one further
   * post. That is correct rather than lossy, and it is why duplicate configs do not need
   * to be filtered out before they get here.
   */
  static void _task_post_config_loaded(void *arg);

  /**
   * @brief Ask for CONFIG_LOADED to be raised. Never blocks, so it is safe to call from
   * an MQTT event handler. Callers must have published the new config to _config first.
   */
  static void _notify_config_loaded();

  // Vars:
  // Task handle for the task responsible for sending all register requests to the manager. This is used to stop the task once a register_accept has been received.
  static inline TaskHandle_t _task_send_register_request_handle = NULL;

  // Task handle for the task that raises CONFIG_LOADED. NULL if it could not be created,
  // in which case _notify_config_loaded() falls back to posting inline.
  static inline TaskHandle_t _task_post_config_loaded_handle = NULL;

  // Guards _task_send_register_request_handle and the interaction between the
  // task's self-teardown and _start_register_request_task's spawn check.
  static inline SemaphoreHandle_t _register_request_task_mutex = NULL;

  // Topic the manager sends register_accept (and other commands) on: nspanel/<mac>/command.
  static inline std::string _mqtt_command_topic;

  // Topic that NSPanels can send command to the MQTTManager on:
  static inline std::string _mqtt_manager_command_topic;

  // Used to stop "register_request" task so that it cleans up the JSON data.
  static inline bool _send_register_requests;

  // IP address to the MQTT Manager container
  static inline std::string _manager_address = "";

  // Port used to download files from MQTT Manager container
  static inline uint16_t _manager_port;

  // Last known MQTT manager status state
  static inline std::atomic<bool> _manager_online = false;
  // What _manager_online was when the connection dropped, put back on reconnect (see _mqtt_connection_listener()).
  // Only accessed from the MQTT client task.
  static inline bool _manager_online_before_disconnect = false;

  // Topic for manager status message
  static inline std::string _mqtt_manager_status_topic;

  // MQTT topic used from manager to send new config data to panel
  static inline std::string _mqtt_config_topic;

  // The holds, and for the two that move, a generation their deliveries carry as context so one from a replaced
  // hold is dropped. _config_subscription_topic is the topic that hold was taken for.
  static inline mqtt::Subscription _command_subscription;
  static inline mqtt::Subscription _config_subscription;
  static inline std::string _config_subscription_topic;
  static inline std::atomic<uint32_t> _config_generation = 0;
  static inline mqtt::Subscription _manager_status_subscription;
  static inline std::atomic<uint32_t> _manager_status_generation = 0;

  // Mutex used to lock the writing to the NSPanel Config object.
  static inline SemaphoreHandle_t _config_mutex;

  // The protobuf NSPanelConfig object decoded from MQTT.
  static inline std::shared_ptr<NSPanelConfig> _config = NULL;
};