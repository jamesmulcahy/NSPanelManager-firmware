#include <ConfigManager.hpp>
#include <EpochParser.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <PanelClock.hpp>
#include <cstdlib>
#include <ctime>
#include <esp_log.h>
#include <sys/time.h>

void PanelClock::init() {
  esp_log_level_set("PanelClock", ConfigManager::log_level);

  PanelClock::_topic_mutex = xSemaphoreCreateMutex();
  if (PanelClock::_topic_mutex == NULL) [[unlikely]] {
    ESP_LOGE("PanelClock", "Failed to create epoch topic mutex. The clock will not be set.");
    return;
  }

  MqttManager::register_handler(MQTT_EVENT_DATA, &PanelClock::_mqtt_event_handler, NULL);
  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, PanelClock::_config_event_handler, NULL);
}

bool PanelClock::is_set() {
  return PanelClock::_clock_set;
}

void PanelClock::_config_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // The manager address is only known once a register_accept has arrived, so the topic can't be subscribed earlier.
  std::string manager_address = NSPM_ConfigManager::get_manager_address();
  if (!manager_address.empty()) {
    std::string new_topic = "nspanel/mqttmanager_";
    new_topic.append(manager_address);
    new_topic.append("/status/epoch");

    std::string old_topic;
    xSemaphoreTake(PanelClock::_topic_mutex, portMAX_DELAY);
    old_topic = PanelClock::_epoch_topic;
    PanelClock::_epoch_topic = new_topic;
    xSemaphoreGive(PanelClock::_topic_mutex);

    if (new_topic != old_topic) {
      // Empty on the first config of this boot: there is nothing subscribed yet to move away from.
      if (!old_topic.empty()) {
        MqttManager::unsubscribe(old_topic);
      }
      if (MqttManager::subscribe(new_topic) != ESP_OK) {
        ESP_LOGE("PanelClock", "Failed to subscribe to %s. The clock will not be set.", new_topic.c_str());
      }
    }
  }

  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    // An older manager doesn't send tz_posix, which then decodes as an empty string.
    PanelClock::_apply_timezone(config->tz_posix);
  } else {
    ESP_LOGE("PanelClock", "Failed to get config, keeping the current timezone.");
  }
}

void PanelClock::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  if (event->topic_len <= 0) {
    return;
  }

  xSemaphoreTake(PanelClock::_topic_mutex, portMAX_DELAY);
  bool is_epoch_topic = !PanelClock::_epoch_topic.empty() && PanelClock::_epoch_topic.compare(0, std::string::npos, event->topic, event->topic_len) == 0;
  xSemaphoreGive(PanelClock::_topic_mutex);
  if (!is_epoch_topic) {
    return;
  }

  // The manager never retains the time. A retained one was left by something else and could be any age.
  if (event->retain) {
    ESP_LOGW("PanelClock", "Ignoring retained time on %.*s.", event->topic_len, event->topic);
    return;
  }

  std::optional<int64_t> epoch_ms = EpochParser::parse_ms(event->data, event->total_data_len == event->data_len ? event->data_len : 0);
  if (!epoch_ms.has_value()) {
    ESP_LOGW("PanelClock", "Ignoring invalid time from manager: '%.*s'.", event->data_len, event->data);
    return;
  }

  struct timeval now;
  gettimeofday(&now, NULL);
  int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;

  struct timeval new_time;
  new_time.tv_sec = static_cast<time_t>(epoch_ms.value() / 1000);
  new_time.tv_usec = static_cast<suseconds_t>((epoch_ms.value() % 1000) * 1000);
  if (settimeofday(&new_time, NULL) != 0) {
    ESP_LOGE("PanelClock", "Failed to set the clock to %lld ms.", epoch_ms.value());
    return;
  }

  if (!PanelClock::_clock_set.exchange(true)) {
    ESP_LOGI("PanelClock", "Clock set from manager to %lld ms since the Unix epoch.", epoch_ms.value());
    PanelClock::_log_local_time();
  } else {
    // How far the clock had drifted since the last time, plus however long this message took to arrive.
    ESP_LOGD("PanelClock", "Clock set from manager, moved by %lld ms.", epoch_ms.value() - now_ms);
  }
}

void PanelClock::_apply_timezone(const char *tz_posix) {
  std::string rule = tz_posix != NULL ? tz_posix : "";
  if (rule == PanelClock::_tz_posix) {
    return;
  }

  // newlib's tzset() and localtime_r() take the same lock, so _log_local_time() on the MQTT task can't see a
  // half-applied rule.
  if (rule.empty()) {
    unsetenv("TZ");
    ESP_LOGI("PanelClock", "No timezone from manager, local time is UTC.");
  } else {
    setenv("TZ", rule.c_str(), 1);
    ESP_LOGI("PanelClock", "Timezone set from manager to '%s'.", rule.c_str());
  }
  tzset();
  PanelClock::_tz_posix = rule;

  // Before the clock is set, local time would be some time on 1970-01-01, which says nothing about the rule.
  if (PanelClock::_clock_set) {
    PanelClock::_log_local_time();
  }
}

void PanelClock::_log_local_time() {
  time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);

  // newlib falls back to UTC for a rule it can't parse, so this shows whether tz_posix was understood.
  char formatted[64];
  strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &local);
  ESP_LOGI("PanelClock", "Local time is %s at %lld s since the Unix epoch.", formatted, static_cast<long long>(now));
}
