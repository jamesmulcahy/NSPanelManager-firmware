#include <BuzzerManager.hpp>
#include <ConfigManager.hpp>
#include <LvglUi.hpp>
#include <PanelClock.hpp>
#include <TimerManager.hpp>
#include <TimerSchedule.hpp>
#include <algorithm>
#include <ctime>
#include <esp_log.h>

static const char *TAG = "TimerManager";

// The sound repeats every REPEAT_INTERVAL_US, for REPEAT_MAX_COUNT repeats after the first (the last is at 57 s).
static constexpr int64_t REPEAT_INTERVAL_US = 3'000'000;
static constexpr int REPEAT_MAX_COUNT = 19;

void TimerManager::init() {
  esp_log_level_set(TAG, ConfigManager::log_level);

  esp_timer_create_args_t args = {};
  args.callback = TimerManager::_timer_callback;
  args.name = "timer_check";
  if (esp_timer_create(&args, &TimerManager::_timer) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create the scheduler timer. Timers will not run.");
    return;
  }
  args.callback = TimerManager::_repeat_callback;
  args.name = "timer_repeat";
  if (esp_timer_create(&args, &TimerManager::_repeat_timer) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create the repeat timer. The alarm sound will play once.");
  }

  esp_event_handler_register(PANEL_CLOCK_EVENT, panel_clock_event::CLOCK_SET, TimerManager::_clock_event_handler, NULL);
  // The clock may have been set before this ran.
  if (PanelClock::is_set()) {
    LvglUi::defer([]() { TimerManager::_on_clock_set(); });
  }
}

bool TimerManager::start(uint32_t duration_s) {
  if (!TimerSchedule::duration_valid(duration_s) || static_cast<int>(_active.size()) >= MAX_TIMERS || TimerManager::_timer == NULL) {
    return false;
  }

  Entry entry;
  entry.id = TimerManager::_next_id++;
  entry.duration_s = duration_s;
  entry.deadline_us = esp_timer_get_time() + static_cast<int64_t>(duration_s) * 1'000'000;
  // Without the wall clock (or before the saved timers are restored) it runs from the monotonic deadline only.
  entry.deadline_epoch_s = (TimerManager::_restored && PanelClock::is_set()) ? static_cast<int64_t>(time(NULL)) + duration_s : 0;
  TimerManager::_active.push_back(entry);
  ESP_LOGI(TAG, "Timer %u started for %u s%s.", static_cast<unsigned>(entry.id), static_cast<unsigned>(duration_s),
           entry.deadline_epoch_s == 0 ? " (clock not set, not saved yet)" : "");

  if (entry.deadline_epoch_s != 0) {
    TimerManager::_save();
  }
  TimerManager::_rearm();
  return true;
}

void TimerManager::cancel(uint32_t id) {
  auto it = std::find_if(_active.begin(), _active.end(), [id](const Entry &e) { return e.id == id; });
  if (it == _active.end()) {
    return;
  }
  ESP_LOGI(TAG, "Timer %u cancelled with %lld s left.", static_cast<unsigned>(id), static_cast<long long>((it->deadline_us - esp_timer_get_time()) / 1'000'000));
  _active.erase(it);
  TimerManager::_save();
  TimerManager::_rearm();
}

std::vector<TimerInfo> TimerManager::snapshot() {
  int64_t now = esp_timer_get_time();
  std::vector<TimerInfo> result;
  for (const Entry &e : _active) {
    result.push_back({e.id, e.duration_s, std::max<int64_t>(e.deadline_us - now, 0)});
  }
  std::sort(result.begin(), result.end(), [](const TimerInfo &a, const TimerInfo &b) { return a.remaining_us < b.remaining_us; });
  return result;
}

std::vector<TimerInfo> TimerManager::fired() {
  return _fired;
}

void TimerManager::dismiss() {
  if (TimerManager::_repeat_timer != NULL) {
    esp_timer_stop(TimerManager::_repeat_timer);
  }
  BuzzerManager::stop();
  if (!_fired.empty()) {
    ESP_LOGI(TAG, "Dismissed %u fired timer(s).", static_cast<unsigned>(_fired.size()));
    _fired.clear();
  }
  // Fired timers were already removed from the saved ones when they fired, so there is nothing more to save.
}

void TimerManager::_timer_callback(void *arg) {
  // esp_timer task: only hand over to LvglUi's task.
  LvglUi::defer([]() { TimerManager::_check(); });
}

void TimerManager::_repeat_callback(void *arg) {
  BuzzerManager::raise_event(buzzer_event_t::TIMER_FIRED);
  if (++TimerManager::_repeat_count >= REPEAT_MAX_COUNT) {
    esp_timer_stop(TimerManager::_repeat_timer);
  }
}

void TimerManager::_clock_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // Default event loop: hand over to LvglUi's task.
  LvglUi::defer([]() { TimerManager::_on_clock_set(); });
}

void TimerManager::_check() {
  int64_t now = esp_timer_get_time();

  std::vector<Entry> due;
  for (auto it = _active.begin(); it != _active.end();) {
    if (it->deadline_us <= now) {
      due.push_back(*it);
      it = _active.erase(it);
    } else {
      ++it;
    }
  }

  if (!due.empty()) {
    // One save for all of them, before the sound, so a reboot can't bring them back.
    TimerManager::_save();
    for (const Entry &e : due) {
      TimerManager::_fire(e, false);
    }
  }

  int64_t soonest = INT64_MAX;
  for (const Entry &e : _active) {
    soonest = std::min(soonest, e.deadline_us - now);
  }
  if (soonest == INT64_MAX) {
    ESP_LOGD(TAG, "Wakeup: %u fired, no timers left.", static_cast<unsigned>(due.size()));
  } else {
    ESP_LOGD(TAG, "Wakeup: %u fired, soonest has %lld ms left, next wake in %lld ms.", static_cast<unsigned>(due.size()),
             static_cast<long long>(soonest / 1000), static_cast<long long>(TimerSchedule::next_wake_us(soonest) / 1000));
  }
  TimerManager::_rearm();

  if (!due.empty() && TimerManager::on_fired != nullptr) {
    TimerManager::on_fired();
  }
}

void TimerManager::_on_clock_set() {
  time_t now_t = time(NULL);
  int64_t now_epoch = static_cast<int64_t>(now_t);
  int64_t now_us = esp_timer_get_time();
  bool changed = false;

  // Timers started before the clock was set get their wall-clock deadline now.
  for (Entry &e : _active) {
    if (e.deadline_epoch_s == 0) {
      e.deadline_epoch_s = now_epoch + std::max<int64_t>((e.deadline_us - now_us) / 1'000'000, 0);
      changed = true;
    }
  }

  std::vector<Entry> late;
  if (!TimerManager::_restored) {
    TimerManager::_restored = true;
    for (const auto &saved : ConfigManager::timers) {
      int64_t amount_s = 0;
      switch (TimerSchedule::restore_decision(saved.deadline_epoch_s, now_epoch, amount_s)) {
      case TimerSchedule::RestoreAction::RESTORE:
        if (static_cast<int>(_active.size()) >= MAX_TIMERS || !TimerSchedule::duration_valid(saved.duration_s)) {
          ESP_LOGI(TAG, "Dropped a saved timer of %u s: no room or out of range.", static_cast<unsigned>(saved.duration_s));
        } else {
          Entry e{TimerManager::_next_id++, saved.duration_s, now_us + amount_s * 1'000'000, saved.deadline_epoch_s};
          _active.push_back(e);
          ESP_LOGI(TAG, "Timer %u restored: %u s timer with %lld s left.", static_cast<unsigned>(e.id), static_cast<unsigned>(e.duration_s), static_cast<long long>(amount_s));
        }
        break;
      case TimerSchedule::RestoreAction::FIRE_LATE:
        late.push_back({TimerManager::_next_id++, saved.duration_s, now_us, saved.deadline_epoch_s});
        break;
      case TimerSchedule::RestoreAction::DROP:
        ESP_LOGI(TAG, "Dropped a saved %u s timer that expired %lld s ago.", static_cast<unsigned>(saved.duration_s), static_cast<long long>(amount_s));
        break;
      }
    }
    changed = changed || !ConfigManager::timers.empty();
  }

  if (changed) {
    TimerManager::_save(); // Also removes the late and dropped ones from the saved timers
  }
  TimerManager::_rearm();

  for (const Entry &e : late) {
    TimerManager::_fire(e, true);
  }
  if (!late.empty() && TimerManager::on_fired != nullptr) {
    TimerManager::on_fired();
  }
}

void TimerManager::_rearm() {
  if (TimerManager::_timer == NULL) {
    return;
  }
  esp_timer_stop(TimerManager::_timer); // Not running is fine

  if (_active.empty()) {
    return;
  }
  int64_t now = esp_timer_get_time();
  int64_t soonest = INT64_MAX;
  for (const Entry &e : _active) {
    soonest = std::min(soonest, e.deadline_us - now);
  }
  // At least 1 us, as a one-shot of 0 is allowed but a deadline already passed must still wake us.
  int64_t wake = std::max<int64_t>(TimerSchedule::next_wake_us(soonest), 1);
  esp_timer_start_once(TimerManager::_timer, wake);
}

void TimerManager::_fire(const Entry &entry, bool late) {
  if (late) {
    ESP_LOGI(TAG, "Timer %u (%u s) fired late: it expired while the panel was away.", static_cast<unsigned>(entry.id), static_cast<unsigned>(entry.duration_s));
  } else {
    ESP_LOGI(TAG, "Timer %u (%u s) fired.", static_cast<unsigned>(entry.id), static_cast<unsigned>(entry.duration_s));
  }
  _fired.push_back({entry.id, entry.duration_s, 0});

  // Restart the repeat window from this timer. The first sound is now, the next ones come from the repeat timer.
  BuzzerManager::raise_event(buzzer_event_t::TIMER_FIRED);
  if (TimerManager::_repeat_timer != NULL) {
    esp_timer_stop(TimerManager::_repeat_timer);
    TimerManager::_repeat_count = 0;
    esp_timer_start_periodic(TimerManager::_repeat_timer, REPEAT_INTERVAL_US);
  }
}

void TimerManager::_save() {
  if (!TimerManager::_restored) {
    return; // The saved timers haven't been read back yet, and would be lost
  }
  std::vector<ConfigManager::PersistedTimer> timers;
  for (const Entry &e : _active) {
    if (e.deadline_epoch_s != 0) {
      timers.push_back({e.duration_s, e.deadline_epoch_s});
    }
  }
  // Under the save lock: another task's save must not iterate the vector while it is replaced.
  if (ConfigManager::change(CONFIG_SAVE_BY_TIMERS, [&timers]() { ConfigManager::timers.swap(timers); }) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to save the timers.");
  }
}
