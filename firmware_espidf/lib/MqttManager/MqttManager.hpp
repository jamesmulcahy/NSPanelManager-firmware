#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <mqtt_client.h>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

class MqttManager {
public:
  /**
   * @brief Start the MQTT client and try to connecto to @param server with given port, username and password
   * @param server: Address to MQTT server
   * @param port: port number used to connect to MQTT server
   * @param username: Login username, blank if anonymous
   * @param password: Login password, blank if anonymous
   */
  static void start(std::string *server, uint16_t *port, std::string *username, std::string *password);

  /**
   * @brief Check if currently connected to MQTT
   * @return True if connected, otherwise false
   */
  static bool connected();

  /**
   * @brief Add the given MQTT topic to the set of topics the panel stays subscribed to.
   * Never blocks on the network, so it is safe to call from any event handler. The
   * subscription task sends the SUBSCRIBE, retries it until the broker acknowledges it,
   * and subscribes again after every reconnect. Subscribing to a topic that is already
   * in the set does nothing. One hold per topic in mqtt::Subscriptions, however many callers.
   * @param mqtt_topic: The MQTT topic to subscribe to.
   * @return ESP_OK once the topic is in the set, ESP_ERR_INVALID_STATE if the MQTT client was never started,
   * or ESP_ERR_NO_MEM.
   */
  static esp_err_t subscribe(std::string mqtt_topic);

  /**
   * @brief Remove the given MQTT topic from the set of subscribed topics and queue an UNSUBSCRIBE.
   * Never blocks on the network. The UNSUBSCRIBE is retried until the broker acknowledges it. Unsubscribes and subscribes reach the broker in the order they were made,
   * so unsubscribing and then subscribing to the same topic makes the broker send its retained message again.
   * @param mqtt_topic: The MQTT topic to unsubscribe from.
   * @return ESP_OK once the unsubscribe is queued, or ESP_ERR_INVALID_STATE if the MQTT client was never started.
   */
  static esp_err_t unsubscribe(std::string mqtt_topic);

  /**
   * @brief Send data to the given topic
   * @param topic: The MQTT topic to send data to
   * @param data: The data to send
   * @param data_length: The length of the data to send (number of bytes)
   * @param retian: Should MQTT retain the message
   * @return ESP_OK in case it was successful or ESP_ERR_NOT_FINISHED in case publish failed.
   */
  static esp_err_t publish(std::string topic, const char *data, size_t data_length, bool retain);

  /**
   * @brief Register Handler for MQTT events, timed as Id (a PanelMetrics::MqttHandler) in
   * nspanel_mqtt_handler_duration_seconds.
   * @param event_id: The MQTT event id to subscribe to.
   * @param event_handler_arg: Any argument to the event handler.
   */
  template <esp_event_handler_t Handler, int Id>
  static esp_err_t register_handler(esp_mqtt_event_id_t event_id, void *event_handler_arg) {
    return esp_mqtt_client_register_event(MqttManager::_mqtt_client, event_id, &MqttManager::_timed_handler<Handler, Id>, event_handler_arg);
  }

  /**
   * @brief Unregister a handler registered with register_handler<Handler, Id>().
   * @param event_id: The MQTT event id it was registered for.
   */
  template <esp_event_handler_t Handler, int Id>
  static esp_err_t unregister_handler(esp_mqtt_event_id_t event_id) {
    return esp_mqtt_client_unregister_event(MqttManager::_mqtt_client, event_id, &MqttManager::_timed_handler<Handler, Id>);
  }

private:
  /**
   * Runs Handler and records how long it held the MQTT client task. esp_event identifies a handler by
   * its function pointer, so each (Handler, Id) gets its own wrapper function: registering and
   * unregistering the same pair always name the same wrapper, with nothing stored and no lock taken.
   * Recording is relaxed atomics only, as it runs on the MQTT client task.
   */
  template <esp_event_handler_t Handler, int Id>
  static void _timed_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    int64_t start_us = esp_timer_get_time();
    Handler(arg, event_base, event_id, event_data);
    MqttManager::_record_handler_duration(Id, esp_timer_get_time() - start_us);
  }
  // In MqttManager.cpp, so this header does not need PanelMetrics.hpp (PlatformIO's LDF does not
  // follow includes made from another library's headers).
  static void _record_handler_duration(int id, int64_t elapsed_us);

  /**
   * @brief Handle MQTT events.
   */
  static void _mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * The broker housekeeping task (mqtt_subs): on every pass it first publishes the "online"
   * status if a connect still owes it (_send_online_update), then sends whatever
   * SUBSCRIBE/UNSUBSCRIBE traffic is due (mqtt::Subscriptions::service(), see MqttSubscriptions.hpp).
   * Both can block for seconds on a weak link, so neither happens on the MQTT client task or the
   * default event loop. Woken by subscribe()/unsubscribe()/connect/SUBACK and otherwise every
   * second (sooner when an online status retry is due), to retry failed or unacknowledged packets
   * and end lingers. Created once in start() and never deleted: esp-mqtt dispatches its events
   * synchronously on whichever task called into it, so MQTT_EVENT_DISCONNECTED can be delivered
   * to _mqtt_event_handler on this very task, from inside a publish or a subscribe. Deleting it
   * from there would orphan esp-mqtt's internal API lock and wedge the client for good.
   */
  static void _task_manage_subscriptions(void *param);

  /**
   * Where the online status stands, for _send_online_update. Only mqtt_subs touches it.
   */
  struct OnlineUpdate {
    uint32_t generation = 0;      // The _online_update_generation last seen.
    bool pending = false;         // That generation is a connect whose "online" is not published yet.
    int64_t next_attempt_us = 0;  // After a failed publish, when to try again (esp_timer clock).
  };

  /**
   * Publish the retained "online" status once per connect, retrying every 5s until it succeeds.
   * Gives up when its generation is superseded (a disconnect or a newer connect) or the client is
   * disconnected. Never sleeps: a retry is due at next_attempt_us, and the task's wait is cut short
   * for it, so a failing publish does not hold up the SUBSCRIBE traffic.
   */
  static void _send_online_update(OnlineUpdate &online);

  /**
   * The delivery task (mqtt_dispatch): runs the callbacks of mqtt::subscribe() holders, through
   * mqtt::Subscriptions::deliver(), off the MQTT client task. Woken by a task notification whenever
   * there is something to deliver. Its stack is internal RAM, never PSRAM: callbacks may write flash.
   */
  static void _task_dispatch(void *param);

  /**
   * The configuration used to init and setup the MQTT client.
   */
  static inline esp_mqtt_client_config_t _mqtt_config;

  /**
   * The underlying MQTT client that is used
   */
  static inline esp_mqtt_client_handle_t _mqtt_client;

  /**
   * Are we connected to the MQTT manager?
   */
  static inline bool _connected = false;

  /**
   * Topic to send state updates (online/offline) to when connected/disconnected
   */
  static inline std::string _state_topic;

  /**
   * When disconnected from MQTT, send this message to tell all other entities on MQTT that the panel is offline.
   */
  static inline std::string _last_will_message;

  /**
   * Pre-built "online" JSON payload published by _send_online_update.
   * Built once in start() so the task holds no heap allocation of its own.
   */
  static inline std::string _online_status_message;

  /**
   * Bumped on every connect and every disconnect. _send_online_update picks up a new connect from
   * it and stops retrying as soon as it changes, so a retry left over from a dropped connection
   * can never publish "online" on behalf of a connection that is gone.
   */
  static inline std::atomic<uint32_t> _online_update_generation = 0;
};