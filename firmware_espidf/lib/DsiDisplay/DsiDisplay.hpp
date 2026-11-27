#pragma once
#if defined(BOARD_CUSTOM)
#include <atomic>
#include <cstdint>
#include <esp_err.h>
#include <esp_lcd_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/**
 * The custom PCB's (Waveshare ESP32-P4-WIFI6-Touch-LCD-4B) display, in place of the NSPanel's Nextion:
 * a 720x720 MIPI-DSI LCD (ST7703 or JD9365DA, probed), its GT911 touch controller and its PWM backlight.
 *
 * LVGL draws straight into the panel's two frame buffers (in PSRAM), the pages laid out at 1.5x the
 * NSPanel's size (Theme::set_hd()): into the one the panel is not sending, which show() then hands to the
 * panel from its next frame on (no tearing). Touches are in the panel's 720x720 coordinates.
 *
 *   LDO VO3 2.5 V (D-PHY), DSI 2 lanes at 480 Mbps, panel reset GPIO 27
 *   backlight GPIO 26 (LEDC, 5 kHz, inverted), GT911 on BoardI2c's bus (I2C port 1, SDA 7 / SCL 8), reset GPIO 23, no INT
 */
class DsiDisplay {
public:
  // The panel's side, in pixels: the screen LVGL draws.
  static constexpr uint16_t PANEL_SIZE = 720;

  /**
   * Bring up the panel, the backlight (off until the first frame) and touch. Once only. Touch failing is
   * logged and leaves the display working.
   */
  static esp_err_t init();

  // Whether init() succeeded: frames can be shown.
  static bool ready() { return DsiDisplay::_ready; }

  /**
   * The panel's frame buffers (0 and 1), PANEL_SIZE x PANEL_SIZE RGB565 each, after init(): what LVGL
   * draws into. 0 is the one the panel sends first. nullptr before init() or for another index.
   */
  static void *frame_buffer(int index);

  /**
   * GUI task: show `frame` (one of the frame buffers) from the panel's next frame on. Returns once the panel
   * sends it, so the other frame buffer may be written again.
   */
  static esp_err_t show(const void *frame);

  // Any task: backlight 0-100 %. 0 turns it off. Held until the first frame is shown.
  static void set_brightness(uint8_t percent);

  // Touch task: a press, move or release, in panel (720x720) coordinates.
  static inline void (*on_touch)(bool down, uint16_t x, uint16_t y) = nullptr;

private:
  static esp_err_t _init_panel();
  static esp_err_t _init_backlight();
  static esp_err_t _init_touch();
  static void _apply_brightness();
  static void _touch_task(void *param);

  static inline std::atomic<bool> _ready = false;
  static inline esp_lcd_panel_handle_t _panel = nullptr;
  static inline void *_fbs[2] = {nullptr, nullptr};
  static inline size_t _fb_size = 0;
  // The frame buffer the panel shows.
  static inline int _front = 0;
  // Given at the end of every frame the panel sends (the frame buffer it came from may be written).
  static inline SemaphoreHandle_t _frame_sem = NULL;

  static inline std::atomic<uint8_t> _brightness = 100;
  static inline std::atomic<bool> _shown = false;

  static inline void *_touch = nullptr;
};
#endif
