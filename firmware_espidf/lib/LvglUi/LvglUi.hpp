#pragma once
#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <functional>
#include <lvgl.h>
#include <string>

/**
 * LVGL as the panel's whole UI, on a display running an nspanel-terminal TFT (docs/LVGL.md).
 *
 * Such a TFT has no Nextion pages, so on it every page is drawn by LVGL and streamed through
 * NextionTerminal. NSPM's page classes (lib/Pages) stay in charge of what is shown and of talking
 * to the manager; where they would drive the Nextion they hand a view's state to show() instead.
 * A page with no LVGL version yet shows PlaceholderView (Nextion::go_to_page() lands here).
 *
 * The mode follows the TFT: init() turns LVGL on only when the display identifies as a terminal
 * TFT and did not send NSPM's startup flag, so reinstalling NSPM's TFT rolls a panel back, and
 * ConfigManager::lvgl_ui = false keeps the Nextion pages whatever the TFT.
 *
 * Also moves into the firmware what NSPM's HMI did by itself: the screensaver timeout (a SLEEP_EVENT
 * after the manager's screensaver_activation_timeout without a touch), waking on a touch (a
 * WAKE_EVENT; that touch reaches nothing else), and the backlight (Nextion::set_brightness_level()
 * goes to the receiver).
 */
class LvglUi {
public:
  /**
   * After Nextion::init(), before the first page: decide the mode, and in LVGL mode start LVGL and
   * the terminal. Returns active().
   */
  static bool init();

  /**
   * Whether LVGL is the panel's UI (init() decided so).
   */
  static bool active() { return LvglUi::_active; }

  // The views show() can host; one is built on the pages screen at a time.
  enum class View : uint8_t { NONE, PLACEHOLDER, SCREENSAVER, ENTITIES, MEDIA_PLAYER, HOME, LIGHT, THERMOSTAT, COVER, MENU, CLEAN_SCREEN };

  /**
   * From any task: show `view`, built with `build` if another view is shown, then run `apply` (which
   * sets the view's state) on the GUI task. Capture state by value: `apply` runs later.
   */
  static void show(View view, void (*build)(lv_obj_t *parent, bool landscape), std::function<void()> apply);

  /**
   * From any task (views' actions run on the GUI task, which must not publish or switch pages):
   * run `work` on LvglUi's own task, in order.
   */
  static void defer(std::function<void()> work);

  /**
   * Before a TFT update: end the terminal session (the update needs the UART) and keep it ended.
   * Waits up to `timeout_ms` for the display to be free; false if it was not.
   */
  static bool suspend(uint32_t timeout_ms);

  /**
   * A page with no LVGL version: PlaceholderView titled `page`.
   */
  static void show_placeholder(const char *page);

  /**
   * From any task: draw the pages over background preset `background` ("none", "ocean", "dusk",
   * "aurora", "peaks"; Background.hpp) in palette `theme` ("dark", "light", "ocean", "dusk", "aurora",
   * "peaks"; Theme.hpp; anything else is "auto", the background's own), rebuilding the screens.
   */
  static void set_look(const std::string &background, const std::string &theme);

  /**
   * Called on LvglUi's task when the wall clock's minute changes (pages showing the time redraw).
   */
  static inline void (*on_minute)() = nullptr;

  /**
   * Whether a press plays the touch sound (off on the clean screen).
   */
  static inline std::atomic<bool> touch_sound = true;

  /**
   * Whether the touch going on (or just released) has moved far enough to be a swipe rather than
   * a tap. Any task.
   */
  static bool swiping() { return LvglUi::_swiping; }

private:
  static void _task(void *param);
  static void _tick();
  static bool _touch(bool down, uint16_t x, uint16_t y);
  static void _start_terminal();
  static void _rebuild(); // GUI task: build the last view again and apply its last state

  static inline std::atomic<bool> _active = false;
  static inline std::atomic<bool> _suspended = false;
  static inline QueueHandle_t _work = NULL;
  // The last touch, or the last time the screen woke (esp_timer microseconds).
  static inline std::atomic<int64_t> _last_activity_us = 0;
  // A SLEEP_EVENT was posted and the screensaver has not been left since.
  static inline std::atomic<bool> _sleep_posted = false;
  // Between the press that woke the screen and its release: touches go nowhere.
  static inline std::atomic<bool> _swallowing = false;
  // The press being tracked, for swipes: where and when it started (terminal task only), and whether
  // it has moved far enough to count as one.
  static inline int _press_x = 0, _press_y = 0;
  static inline int64_t _press_us = 0;
  static inline std::atomic<bool> _swiping = false;
  static inline bool _was_screensaver = false; // _tick() only
  static inline int64_t _last_start_us = 0;    // _tick() only
  static inline View _last_view = View::NONE;  // GUI task only
  static inline int64_t _last_minute = -1;     // _tick() only

  // GUI task only: what is built on the pages screen, and the last show(), to rebuild it.
  static inline View _view = View::NONE;
  static inline void (*_build)(lv_obj_t *, bool) = nullptr;
  static inline std::function<void()> _apply;
};
