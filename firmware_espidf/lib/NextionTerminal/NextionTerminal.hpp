#pragma once
#include <atomic>
#include <cstdint>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

struct NextionTerminalOptions {
  // Screen size in the caller's coordinates.
  uint16_t width = 320;
  uint16_t height = 480;
  // The receiver's area, which the TFT fixes: 320x480 for nspanel-terminal's US TFT
  // (terminal-us.tft), 454x320 for its EU one (terminal-eu.tft, landscape, 26 short of 480).
  uint16_t receiver_width = 320;
  uint16_t receiver_height = 480;
  // Degrees the screen is turned to fit the receiver's area: 0 (same orientation), 180 (the
  // US receiver on Office: its runtime draws upside down), or 90 or 270 (a portrait screen on the
  // EU receiver).
  uint16_t rotation = 0;
  // When the receiver's area is short of the screen (EU receiver on a US panel): drop the first
  // lines of the long side rather than the last (the top of the status area).
  bool crop_start = true;
  // Rows of the receiver's area per rectangle, 1-receiver_height. The nspanel-terminal host never
  // sends more than 16; Office drew 60x320 rectangles cleanly, and they are a round trip each.
  uint16_t max_rows = 480;
  // Added to the receiver's touch x to land in its drawing coordinates. On the EU receiver it is 26:
  // on Office touch ran about -18..431 top to bottom where drawing ran 0..453, i.e. the touch
  // controller counts all 480 lines (480 - 454 = 26) from another origin. Touch is signed.
  int16_t touch_offset = 0;
};

/**
 * Streams RGB565 rectangles to the display at 4 Mbaud, through the nspanel-terminal receiver (native 94)
 * that a patched TFT adds to the Nextion runtime (protocol 8: COBS records with a header CRC,
 * PackBits pixels, credit-based flow control, touch in the replies). See README.md in this directory.
 *
 * The receiver's coordinates are its own area: 320x480 portrait with the US TFT, 454x320
 * landscape with the EU one. Callers draw in the screen's coordinates (320x480 on a US panel,
 * 480x320 on an EU one); `rotation` and `crop_start` map those onto the receiver's area. The EU
 * area is 26 pixels short of the long side, so 26 lines are not drawn: the first 26 with
 * crop_start, else the last.
 *
 * One task of its own owns the UART for the whole session (Nextion::terminal_acquire()): it keeps
 * the link alive with heartbeats (the receiver gives up after ~300 ms of silence), reads the
 * replies and sends what draw() hands it. Leaving power-cycles the display (Nextion::terminal_release()).
 */
class NextionTerminal {
public:
  using Options = NextionTerminalOptions;

  /**
   * Start a session with `options`, from any task. Returns at once; streaming() says when it is up.
   */
  static esp_err_t start(const Options &options);

  /**
   * End the session, from any task. Returns at once; the display is power-cycled afterwards.
   */
  static void stop();

  /**
   * Whether `identity`, a display's `connect` reply, is an nspanel-terminal protocol-8 TFT: this
   * display (NX4832F035_011C, MCU code 61744) with a native version of 94-127. NSPanel Manager's own
   * TFTs report other versions (72 on Formal Living, 128 on Office); touch_j on those starts touch
   * calibration, so nothing outside the range is entered.
   */
  static bool is_receiver(const char *identity);

  /**
   * Options for nspanel-terminal's US receiver (the default: 320x480, turned 180 degrees, as its
   * runtime draws upside down on Office's panel) or EU one (454x320 landscape: a US screen turned
   * 90 degrees, 26 lines cropped, touch offset 26), for a `width` x `height` screen.
   */
  static Options options_for(bool eu_receiver, uint16_t width, uint16_t height);

  /**
   * From any task: set the backlight to `percent` (0-100; 0 is off), now if a session streams,
   * else as soon as one does. Only with a receiver that offers live brightness (READY_BRIGHTNESS).
   */
  static void set_brightness(uint8_t percent);

  /**
   * Whether the receiver accepts rectangles now.
   */
  static bool streaming() { return NextionTerminal::_streaming; }

  /**
   * Whether a session is starting, running or ending: the display is not the Nextion's own.
   */
  static bool owns_display() { return NextionTerminal::_session; }

  /**
   * Draw the area x1,y1-x2,y2 (inclusive, screen coordinates) from `pixels`, its RGB565 pixels
   * row by row. `pixels` is copied before this returns, but the last rectangle may still be on its
   * way: the next call waits for it first, and reports its failure. Not from two tasks at once.
   * @return ESP_OK, ESP_ERR_INVALID_STATE (no session) or ESP_FAIL (this or the previous area
   * was not drawn: repaint).
   */
  static esp_err_t draw(int x1, int y1, int x2, int y2, const uint16_t *pixels, bool last = true);

  /**
   * Called from the terminal's task for each touch edge or move, in screen coordinates.
   */
  static inline void (*on_touch)(bool down, uint16_t x, uint16_t y) = nullptr;

  /**
   * Called from the terminal's task when streaming() changes.
   */
  static inline void (*on_streaming_changed)(bool streaming) = nullptr;


private:
  static void _task(void *param);
  static bool _session_once();
  static bool _command(const char *command);
  static int _read_reply(uint8_t *buffer, size_t size, uint32_t timeout_ms);
  static bool _stream_session(uint16_t session);
  static void _finish_job(bool ok);
  static esp_err_t _submit(int x, int y, int w, int h, bool final = true);
  static esp_err_t _wait_job();
  static esp_err_t _close_burst();
  static void _set_streaming(bool streaming);
  static bool _map_touch(int tx, int ty, uint16_t &x, uint16_t &y);
  static uint32_t _now();

  static inline TaskHandle_t _task_handle = NULL;
  static inline SemaphoreHandle_t _job_done = NULL;
  static inline Options _options;
  static inline std::atomic<bool> _wanted = false;
  static inline std::atomic<bool> _session = false;
  static inline std::atomic<bool> _streaming = false;
  // The backlight set_brightness() asked for, -1 for none yet, and whether it is still to be sent.
  static inline std::atomic<int> _brightness = -1;
  static inline std::atomic<bool> _brightness_changed = false;

  // The rectangle draw() hands over, in the receiver's coordinates, and how it ended.
  static inline uint16_t *_job_pixels = NULL;
  static inline size_t _job_capacity = 0; // Pixels
  static inline uint16_t _job_x = 0, _job_y = 0, _job_w = 0, _job_h = 0;
  static inline std::atomic<bool> _job_pending = false;
  static inline std::atomic<bool> _job_ok = false;
  // The rectangle ends its burst (the receiver confirms it with DONE); else more follow.
  static inline std::atomic<bool> _job_final = true;
  // draw() handed over a rectangle and has not waited for it yet (draw()'s task only).
  static inline bool _job_outstanding = false;
  // The last rectangle handed over left its burst open (draw()'s task only).
  static inline bool _burst_open = false;
  // For the log every 10 s.
  static inline std::atomic<uint32_t> _rectangles = 0;
  static inline std::atomic<uint32_t> _bytes = 0;
  static inline std::atomic<uint32_t> _resyncs = 0;
  static inline std::atomic<uint32_t> _sessions = 0;
};
