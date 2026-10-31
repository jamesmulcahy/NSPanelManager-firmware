#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
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
   * in the set does nothing.
   * @param mqtt_topic: The MQTT topic to subscribe to.
   * @return ESP_OK once the topic is in the set, or ESP_ERR_INVALID_STATE if the MQTT client was never started.
   */
  static esp_err_t subscribe(std::string mqtt_topic);

  /**
   * @brief Remove the given MQTT topic from the set of subscribed topics and queue an UNSUBSCRIBE.
   * Never blocks on the network. Unsubscribes and subscribes reach the broker in the order they were made,
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
   * @brief Register event handler for MQTT events.
   * @param event_id: The MQTT event id to subscribe to.
   * @param event_handler: The event handler callback to register
   * @param event_handler_arg: Any argument to the event handler.
   */
  static esp_err_t register_handler(esp_mqtt_event_id_t event_id, esp_event_handler_t event_handler, void *event_handler_arg);

  /**
   * @brief Unregister event handler for MQTT events.
   * @param event_id: The MQTT event id to subscribe to.
   * @param event_handler: The event handler callback to register
   */
  static esp_err_t unregister_handler(esp_mqtt_event_id_t event_id, esp_event_handler_t event_handler);

private:
  /**
   * @brief Handle MQTT events.
   */
  static void _mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Task that publishes the "online" retained status message, retrying every 5s until it
   * succeeds. Created once in start() and never deleted: esp-mqtt dispatches its events
   * synchronously on whichever task called into it, so MQTT_EVENT_DISCONNECTED can be delivered
   * to _mqtt_event_handler on this very task, from inside the publish below. Deleting it from
   * there would orphan esp-mqtt's internal API lock and wedge the client for good. Woken on
   * MQTT_EVENT_CONNECTED, gives up when its generation is superseded.
   */
  static void _task_send_online_update(void *param);

  /**
   * Task that owns all SUBSCRIBE/UNSUBSCRIBE traffic. Sending either can block for
   * seconds on a weak link, so it never happens on the MQTT client task or the default
   * event loop. Woken by subscribe()/unsubscribe()/connect and otherwise every second
   * to retry failed or unacknowledged subscribes.
   */
  static void _task_manage_subscriptions(void *param);

  /**
   * Handle a SUBACK (MQTT_EVENT_SUBSCRIBED) for the given message id, received at received_ms. Must be called with _subscriptions_mutex held.
   */
  static void _handle_suback(int msg_id, bool failed, int64_t received_ms);

  /**
   * Log how long the broker took to deliver the retained value of a newly subscribed topic, the first time
   * one arrives after subscribe() or a reconnect. Called on the MQTT client task for every MQTT_EVENT_DATA.
   */
  static void _log_subscribe_latency(esp_mqtt_event_handle_t event);

  enum class SubscriptionState {
    PENDING,   // Needs a SUBSCRIBE sent (new, after reconnect, or after a failure).
    IN_FLIGHT, // SUBSCRIBE sent (or being sent), waiting for SUBACK.
    SUBSCRIBED // Broker acknowledged the subscription.
  };

  struct Subscription {
    std::string topic;
    SubscriptionState state = SubscriptionState::PENDING;
    int msg_id = -1;              // Message id of the in-flight SUBSCRIBE, -1 while the send call is running.
    int64_t sent_ms = 0;          // When the in-flight SUBSCRIBE was sent.
    int64_t next_attempt_ms = 0;  // Earliest time to send the next SUBSCRIBE (backoff after failures).
    uint8_t failures = 0;         // Consecutive failed attempts, used for backoff.

    // Latency bookkeeping for _log_subscribe_latency(). Reset by subscribe() and on every reconnect.
    int64_t requested_ms = 0;     // When subscribe() was called, or when the reconnect happened.
    int64_t suback_ms = 0;        // When the SUBACK was received, 0 until then.
    uint8_t attempts = 0;         // SUBSCRIBEs sent since requested_ms.
    bool after_reconnect = false; // Resubscribed by a reconnect rather than asked for by subscribe().
    bool retained_seen = false;   // A retained value has arrived since requested_ms, so there is nothing more to log.
  };

  /**
   * Topics the panel wants to be subscribed to. Guarded by _subscriptions_mutex.
   */
  static inline std::vector<Subscription> _subscriptions;

  /**
   * Topics waiting for an UNSUBSCRIBE, in call order. Guarded by _subscriptions_mutex.
   */
  static inline std::vector<std::string> _pending_unsubscribes;

  struct EarlySuback {
    int msg_id;
    bool failed;
    int64_t received_ms; // Used to discard SUBACKs too old to belong to a live SUBSCRIBE.
  };

  /**
   * SUBACKs that arrived before the subscription task recorded the message id of the
   * SUBSCRIBE (esp-mqtt can process the SUBACK as soon as the send call releases its lock).
   * A SUBACK carries no topic, so entries can only be matched on message id, and message
   * ids are reused once they wrap at 65535. Entries are therefore dropped once they are
   * older than a SUBSCRIBE could still be waiting on them. Guarded by _subscriptions_mutex,
   * kept short.
   */
  static inline std::vector<EarlySuback> _early_subacks;

  /**
   * Guards _subscriptions, _pending_unsubscribes and _early_subacks. Never held across a network call.
   */
  static inline SemaphoreHandle_t _subscriptions_mutex = NULL;

  /**
   * Handle of the subscription task, notified whenever there is work to do.
   */
  static inline TaskHandle_t _manage_subscriptions_task_handle = NULL;

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
   * Pre-built "online" JSON payload published by _task_send_online_update.
   * Built once in start() so the task holds no heap allocation of its own.
   */
  static inline std::string _online_status_message;

  /**
   * Handle of the permanent online-status publish task, notified on every MQTT_EVENT_CONNECTED.
   */
  static inline TaskHandle_t _send_online_update_task_handle = NULL;

  /**
   * Bumped on every connect and every disconnect. _task_send_online_update reads it before it
   * starts publishing and stops retrying as soon as it changes, so a retry left over from a
   * dropped connection can never publish "online" on behalf of a connection that is gone.
   */
  static inline std::atomic<uint32_t> _online_update_generation = 0;
};