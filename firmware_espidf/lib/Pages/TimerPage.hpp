#pragma once
#include <cstdint>
#include <esp_timer.h>

/**
 * The timers pages (TimerView), from the swipe-down menu's "Timers" item; LVGL UI only. All of them run on
 * LvglUi's task (actions come back through LvglUi::defer), which is the task TimerManager is used from.
 *
 * - show(): the running timers, each with a cancel button, redrawn once a second. With none running it goes
 *   straight to the set page.
 * - show_set(): hours, minutes and seconds rollers, quick picks, Start and Cancel. Opens on the last duration
 *   started (kept in RAM only), or 5:00.
 * - show_alarm(): "Timer done" with a large Dismiss button, for when a timer has fired. Dismiss goes to the
 *   Timers page while timers remain, otherwise to the default page.
 *
 * Each is its own page for InterfaceManager (own unshow), so is_current_page() tells them apart.
 */
class TimerPage {
public:
  static void show();
  static void unshow();
  static void show_set();
  static void unshow_set();
  static void show_alarm();
  static void unshow_alarm();

private:
  static void _action(int action, int32_t value);
  static void _render();
  static void _tick(void *arg);

  static inline uint32_t _last_duration_s = 5 * 60;
  static inline esp_timer_handle_t _timer = NULL;
};
