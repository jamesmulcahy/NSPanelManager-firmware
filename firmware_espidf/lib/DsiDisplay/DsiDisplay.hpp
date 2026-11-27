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
 * LVGL draws a 480x480 screen, the size the pages were made for (the NSPanel's are 320 or 480 px a
 * side); present() scales each frame x1.5 with the PPA into the frame buffer the panel is not showing,
 * and shows it from the next frame on (two frame buffers: no tearing). Touches are scaled back to
 * 480x480.
 *
 *   LDO VO3 2.5 V (D-PHY), DSI 2 lanes at 480 Mbps, panel reset GPIO 27
 *   backlight GPIO 26 (LEDC, 5 kHz, inverted), GT911 on I2C port 1 SDA 7 / SCL 8, reset GPIO 23, no INT
 */
class DsiDisplay {
public:
  // The panel, and the screen LVGL draws on it.
  static constexpr uint16_t PANEL_SIZE = 720;
  static constexpr uint16_t SCREEN_SIZE = 480;

  /**
   * Bring up the panel, the backlight (off until the first frame) and touch. Once only. Touch failing is
   * logged and leaves the display working.
   */
  static esp_err_t init();

  // Whether init() succeeded: frames can be shown.
  static bool ready() { return DsiDisplay::_ready; }

  /**
   * GUI task: show `screen` (SCREEN_SIZE x SCREEN_SIZE RGB565, cache-line aligned) from the next frame
   * on. Returns once the panel shows it, so `screen` and the other frame buffer may be written again.
   */
  static esp_err_t present(const uint16_t *screen);

  // Any task: backlight 0-100 %. 0 turns it off. Held until the first frame is shown.
  static void set_brightness(uint8_t percent);

  // Touch task: a press, move or release, in screen (480x480) coordinates.
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
  static inline void *_ppa = nullptr;

  static inline std::atomic<uint8_t> _brightness = 100;
  static inline std::atomic<bool> _shown = false;

  static inline void *_touch = nullptr;
};
#endif
