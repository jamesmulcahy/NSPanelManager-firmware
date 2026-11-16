#pragma once
#include <atomic>
#include <esp_timer.h>

/**
 * The HMI's swipe-down `dropdown` page and its `cleanscreen` page, which the Nextion ran by
 * itself; with LVGL as the UI (LvglUi) the firmware runs them. Swiping down on Home opens the
 * menu. "Look" opens the look picker (LookPage). "Clean screen" works as in the HMI: the screen
 * ignores touches for 10 s (each touch starts the countdown again), then goes back to Home. "Timers" opens the
 * timer pages (TimerPage); its tile shows the next timer's time left, redrawn once a second while timers run.
 */
class MenuPage {
public:
  static void show();
  static void unshow();
  static void show_clean_screen();
  static void unshow_clean_screen();

  /**
   * A swipe down (LvglUi detects it): from Home to the menu; from the menu, the look picker or the clean screen back
   * to Home, as the HMI's gestures did; likewise the Timers and New timer pages. The timer alarm ignores it.
   * LvglUi's task.
   */
  static void swipe_down();

private:
  static void _action(int action, int32_t item);
  static void _render();
  static void _menu_tick(void *arg);
  static void _render_clean_screen();
  static void _tick(void *arg);

  // Countdown, 100 -> 0, a step every 100 ms (the HMI's tm_loadbar).
  static inline std::atomic<int> _remaining = 100;
  static inline esp_timer_handle_t _timer = NULL;
  // The menu's own once-a-second redraw while timers run.
  static inline esp_timer_handle_t _menu_timer = NULL;
};
