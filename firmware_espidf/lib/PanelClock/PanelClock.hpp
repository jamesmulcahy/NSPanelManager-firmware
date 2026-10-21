#pragma once
#include <atomic>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string>

// Sets the panel's wall clock from the time the manager sends on nspanel/mqttmanager_<address>/status/epoch,
// and its timezone from NSPanelConfig.tz_posix. An older manager sends neither, in which case the clock is
// never set and is_set() stays false, so anything using the wall clock must check is_set() first.
class PanelClock {
public:
  /**
   * @brief Register for config updates, which give the manager address to subscribe to the epoch topic of and
   * the timezone, and for MQTT messages on that topic. Call after MqttManager::start().
   */
  static void init();

  /**
   * @brief Whether the wall clock has been set from the manager since boot. Until then time() and
   * gettimeofday() count from the Unix epoch at boot and must not be shown or used as the real time.
   */
  static bool is_set();

private:
  /**
   * @brief Subscribe to the epoch topic for the current manager address, moving the subscription if the
   * address has changed, and apply the timezone from the config.
   */
  static void _config_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Set the clock from a message on the epoch topic.
   */
  static void _mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Set the TZ environment variable to tz_posix and call tzset(), unless it is already set to that.
   * An empty rule unsets TZ, so local time is UTC.
   */
  static void _apply_timezone(const char *tz_posix);

  /**
   * @brief Log the local time, as the panel's own clock and timezone give it, so the rule newlib actually applied
   * can be checked against tz_posix.
   */
  static void _log_local_time();

  static inline std::atomic<bool> _clock_set = false;

  // Written from the default event loop task, read from the MQTT task. Only accessed while holding _topic_mutex.
  static inline SemaphoreHandle_t _topic_mutex = NULL;
  static inline std::string _epoch_topic;

  // Only accessed from the default event loop task.
  static inline std::string _tz_posix;
};
