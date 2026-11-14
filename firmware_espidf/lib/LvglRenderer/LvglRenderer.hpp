#pragma once
#include <atomic>
#include <cstdint>
#include <esp_err.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <functional>
#include <lvgl.h>
#include <string>

/**
 * LVGL on the ESP32, drawn on the display through NextionTerminal (the nspanel-terminal receiver
 * in a patched TFT, at 4 Mbaud). LvglUi starts it when the display runs such a TFT.
 *
 * LVGL runs on one task of its own, and nothing else calls LVGL: other tasks post() work to it, or
 * hand over state (set_loading_text()) that it applies before each refresh. LVGL draws into a band
 * of internal RAM; its heap is in PSRAM (lv_mem_core_psram.c).
 *
 * Two screens: the boot screen (BootScreen, LoadingPage's), and the pages screen that LvglUi builds
 * the page views on. Touches come from the receiver, through touch_filter, into LVGL's pointer.
 */
class LvglRenderer {
public:
  /**
   * Allocate the buffers, start LVGL and its task, and build the screens. Nothing is drawn until
   * activate(), and nothing reaches the display until NextionTerminal streams. Once only.
   */
  static esp_err_t start();

  // What activate() draws.
  enum class Screen : uint8_t { BOOT, PAGES };

  /**
   * Whether start() succeeded.
   */
  static bool enabled() { return LvglRenderer::_enabled; }

  /**
   * From any task: draw `screen`. Switching repaints the whole display.
   */
  static void activate(Screen screen);

  /**
   * From any task: run `work` on the GUI task, before its next refresh, in the order posted. The
   * only way other tasks touch LVGL objects. False if the queue was full (work dropped).
   */
  static bool post(std::function<void()> work);

  /**
   * GUI task only: the screen Screen::PAGES shows, and whether screens are laid out landscape.
   */
  static lv_obj_t *pages_screen() { return LvglRenderer::_pages_screen; }
  static bool landscape() { return !LvglRenderer::_us_layout; }

  /**
   * GUI task: called after the screens were rebuilt for another region, so whatever builds on
   * pages_screen() builds again.
   */
  static inline void (*on_pages_rebuilt)() = nullptr;

  /**
   * Terminal task: sees every touch edge and move (screen coordinates) before LVGL does; returning
   * true keeps it from LVGL (e.g. the touch that wakes the screen).
   */
  static inline bool (*touch_filter)(bool down, uint16_t x, uint16_t y) = nullptr;

  /**
   * From any task: a touch edge or move in screen coordinates, as if the terminal reported it (it
   * goes through touch_filter too). The terminal's touches arrive here; also for testing over MQTT
   * (the "lvgl_tap" command).
   */
  static void inject_touch(bool down, uint16_t x, uint16_t y);

  /**
   * The boot screen's texts, from any task (LoadingPage's).
   */
  static void set_loading_text(const std::string &text);
  static void set_secondary_text(const std::string &text);

  /**
   * Wake the GUI task to apply new state now rather than at its next tick.
   */
  static void notify();

  /**
   * Screen size: 320x480 on a US panel, 480x320 on an EU one.
   */
  static uint16_t width() { return LvglRenderer::_us_layout ? 320 : 480; }
  static uint16_t height() { return LvglRenderer::_us_layout ? 480 : 320; }

private:
  static void _task(void *param);
  static void _flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map);
  static void _read_pointer(lv_indev_t *indev, lv_indev_data_t *data);
  static void _config_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
  static void _build_screens();
  static void _show_status();
  static uint32_t _tick();

  struct Touch {
    uint16_t x, y;
    bool pressed;
  };

  static inline TaskHandle_t _task_handle = NULL;
  static inline lv_display_t *_display = NULL;
  static inline std::atomic<bool> _enabled = false;
  static inline std::atomic<bool> _active = false;
  // The screen activate() asked for, and the one LVGL shows (GUI task only).
  static inline std::atomic<Screen> _wanted_screen = Screen::BOOT;
  static inline Screen _loaded_screen = Screen::BOOT;
  static inline lv_obj_t *_boot_screen = NULL;
  static inline lv_obj_t *_pages_screen = NULL;
  // Repaint everything on the next refresh: a new screen, or the receiver (re)started.
  static inline std::atomic<bool> _repaint = false;
  // The region the screens are laid out for (GUI task only, after start()).
  static inline bool _us_layout = true;
  // The region the manager's config asks for: -1 nothing new, 0 EU, 1 US.
  static inline std::atomic<int> _wanted_region = -1;
  // post()'s work, as heap-allocated std::function pointers.
  static inline QueueHandle_t _work = NULL;

  // The boot screen's texts, handed over from other tasks.
  static inline SemaphoreHandle_t _text_mutex = NULL;
  static inline std::string _loading_text;
  static inline std::string _secondary_text;

  // Presses, moves and releases, in order, for LVGL's pointer. Queued, as LVGL reads the pointer only
  // between refreshes: a tap must still arrive as a press and a release.
  static inline QueueHandle_t _touches = NULL;
  // The pointer as LVGL last read it (GUI task only).
  static inline lv_point_t _point = {0, 0};
  static inline bool _pressed = false;
};
