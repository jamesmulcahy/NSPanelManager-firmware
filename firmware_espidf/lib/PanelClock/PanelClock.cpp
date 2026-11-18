#include <ConfigManager.hpp>
#include <EpochParser.hpp>
#include <MqttSubscriptions.hpp>
#include <PanelMetrics.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <PanelClock.hpp>
#include <cstdlib>
#include <ctime>
#include <esp_log.h>
#include <sys/time.h>

ESP_EVENT_DEFINE_BASE(PANEL_CLOCK_EVENT);

void PanelClock::init() {
  esp_log_level_set("PanelClock", ConfigManager::log_level);

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

    if (new_topic != PanelClock::_epoch_topic) {
      PanelClock::_epoch_topic = new_topic;
      // Replacing the handle releases the old topic (none on the first config of this boot). A delivery from the
      // old hold that is still queued carries the old generation and is dropped.
      uint32_t generation = ++PanelClock::_generation;
      PanelClock::_subscription = mqtt::subscribe(new_topic, mqtt::Kind::STATE, &PanelClock::_mqtt_message_handler, reinterpret_cast<void *>(static_cast<uintptr_t>(generation)),
                                                  PanelMetrics::HANDLER_PANEL_CLOCK);
      if (!PanelClock::_subscription.active()) [[unlikely]] {
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

void PanelClock::_mqtt_message_handler(const mqtt::Message &message, void *context) {
  if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context)) != PanelClock::_generation.load() || message.payload.empty()) {
    return; // A hold replaced since, or a cleared retained value.
  }

  // The manager never retains the time. A retained one was left by something else and could be any age. Neither
  // is one replayed from the cache.
  if (message.retained || message.cached) {
    ESP_LOGW("PanelClock", "Ignoring retained time on %.*s.", (int)message.topic.size(), message.topic.data());
    return;
  }

  std::optional<int64_t> epoch_ms = EpochParser::parse_ms(reinterpret_cast<const char *>(message.payload.data()), message.payload.size());
  if (!epoch_ms.has_value()) {
    ESP_LOGW("PanelClock", "Ignoring invalid time from manager: '%.*s'.", (int)message.payload.size(), reinterpret_cast<const char *>(message.payload.data()));
    return;
  }

  struct timeval now;
  gettimeofday(&now, NULL);
  int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;

  struct timeval new_time;
  new_time.tv_sec = static_cast<time_t>(epoch_ms.value() / 1000);
  new_time.tv_usec = static_cast<suseconds_t>((epoch_ms.value() % 1000) * 1000);
  // The clock is kept in UTC. Local time comes from the TZ rule (_apply_timezone()), so the panel follows DST
  // changes on its own, even while the manager is offline.
  if (settimeofday(&new_time, NULL) != 0) {
    ESP_LOGE("PanelClock", "Failed to set the clock to %lld ms.", epoch_ms.value());
    return;
  }

  if (!PanelClock::_clock_set.exchange(true)) {
    ESP_LOGI("PanelClock", "Clock set from manager to %lld ms since the Unix epoch.", epoch_ms.value());
    PanelClock::_log_local_time();
    // Timeout 0: this runs on the mqtt_dispatch task, which must not wait on the default loop's queue (see CLAUDE.md).
    if (esp_event_post(PANEL_CLOCK_EVENT, panel_clock_event::CLOCK_SET, NULL, 0, 0) != ESP_OK) {
      ESP_LOGW("PanelClock", "Failed to post CLOCK_SET; things waiting for the clock will not be told.");
    }
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
