#pragma once
#include <atomic>
#include <cstdint>
#include <esp_event.h>
#include <esp_timer.h>
#include <vector>

struct TimerInfo {
  uint32_t id;
  uint32_t duration_s;
  int64_t remaining_us;
};

/**
 * Countdown timers started on the panel. They survive a reboot (ConfigManager "timers"), are checked without
 * polling (one one-shot esp_timer, re-armed for the soonest deadline with TimerSchedule::next_wake_us), and
 * play BuzzerManager's TIMER_FIRED sound when they finish, every 3 s for up to 60 s until dismissed.
 *
 * The engine runs on LvglUi's task: the esp_timer callback only defers to it, so the timer list needs no mutex.
 * The public functions below must be called from that task, which is where page actions already run.
 */
class TimerManager {
public:
  static constexpr int MAX_TIMERS = 4;

  /**
   * @brief Create the timers and register for PANEL_CLOCK_EVENT. Call after LvglLook::init(), and only when
   * LvglUi::active().
   */
  static void init();

  /**
   * @brief Start a timer. LvglUi task.
   * @return false when MAX_TIMERS are already running or duration_s is outside 1 s to 23:59:59
   */
  static bool start(uint32_t duration_s);

  /**
   * @brief Cancel a running timer. Does nothing for an unknown id. LvglUi task.
   */
  static void cancel(uint32_t id);

  /**
   * @brief The running timers, soonest first. LvglUi task.
   */
  static std::vector<TimerInfo> snapshot();

  /**
   * @brief The timers that have fired and not been dismissed, in the order they fired (remaining_us is 0).
   * LvglUi task.
   */
  static std::vector<TimerInfo> fired();

  /**
   * @brief Dismiss everything that has fired: stops the repeating sound and the buzzer. LvglUi task.
   */
  static void dismiss();

  // Called after a timer fires, to show the alarm. Set by the alarm wiring. LvglUi task.
  static inline void (*on_fired)() = nullptr;

private:
  struct Entry {
    uint32_t id;
    uint32_t duration_s;
    int64_t deadline_us;      // esp_timer_get_time() deadline: monotonic, so clock steps can't move it
    int64_t deadline_epoch_s; // Wall-clock deadline for persistence; 0 until the clock is set and restored
  };

  static void _timer_callback(void *arg);
  static void _repeat_callback(void *arg);
  static void _clock_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  // LvglUi task
  static void _check();
  static void _on_clock_set();
  static void _rearm();
  static void _fire(const Entry &entry, bool late);
  static void _save();

  // Only touched from LvglUi's task.
  static inline std::vector<Entry> _active;
  static inline std::vector<TimerInfo> _fired;
  static inline uint32_t _next_id = 1;
  // Whether the saved timers have been restored. Until then nothing is saved, so the saved ones aren't overwritten.
  static inline bool _restored = false;

  static inline esp_timer_handle_t _timer = NULL;
  static inline esp_timer_handle_t _repeat_timer = NULL;
  // Written on LvglUi's task before the repeat timer is started, read and counted by its callback.
  static inline std::atomic<int> _repeat_count = 0;
};
