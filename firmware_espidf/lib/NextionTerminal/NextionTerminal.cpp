// The hot paths, the PackBits encoder (TerminalStreamEngine.hpp) and copy_rotated(), run for every
// pixel drawn: build this file for speed, whatever the rest of the firmware is built with.
#pragma GCC optimize("O2")

#include <ConfigManager.hpp>
#include <NextionTerminal.hpp>
#include <NextionTerminalCopy.hpp>
#include <Nextion.hpp>
#include <TerminalStreamEngine.hpp>
#include <algorithm>
#include <cstring>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

static const char *TAG = "NextionTerminal";

// How far outside the drawing area a touch may land and still count as an edge, not nonsense.
static constexpr int TOUCH_MARGIN = 26;

static bool portrait_for(const NextionTerminalOptions &o) {
  return o.rotation == 90 || o.rotation == 270;
}
// The screen's size along the receiver's x and y.
static int screen_long_for(const NextionTerminalOptions &o) {
  return portrait_for(o) ? o.height : o.width;
}
static int screen_short_for(const NextionTerminalOptions &o) {
  return portrait_for(o) ? o.width : o.height;
}
// The receiver columns used: its width (454 for the EU receiver, which rejects columns 454-479).
static int width_for(const NextionTerminalOptions &o) {
  return o.receiver_width;
}
// Screen lines along the receiver's x that it cannot reach (26 on the EU receiver, else 0).
static int crop_for(const NextionTerminalOptions &o) {
  return screen_long_for(o) - width_for(o);
}
static constexpr uint32_t STREAM_BAUD = 4000000;
// How long draw() waits for one rectangle before giving up on the session.
static constexpr uint32_t JOB_TIMEOUT_MS = 3000;
// Failed sessions in a row before start() has to be asked again.
static constexpr int MAX_ATTEMPTS = 3;
// The largest rectangle buffer worth taking from internal RAM (20 lines of a portrait screen is 12.8 KB).
static constexpr size_t INTERNAL_JOB_BYTES = 16 * 1024;
static constexpr int64_t STATS_LOG_INTERVAL_US = 10 * 1000 * 1000;

uint32_t NextionTerminal::_now() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// nspanel-terminal's native versions: 94 (protocol 8) up to, not including, NSPM's 128.
static constexpr int RECEIVER_NATIVE_MIN = 94;
static constexpr int RECEIVER_NATIVE_MAX = 127;
static const char RECEIVER_MODEL[] = "NX4832F035_011C,";
static const char RECEIVER_MCU[] = ",61744,";

// The native version in a `connect` reply from this display, or -1 if it is another display.
static int native_version(const char *identity, size_t length) {
  const char *end = identity + length;
  const char *model = std::search(identity, end, RECEIVER_MODEL, RECEIVER_MODEL + strlen(RECEIVER_MODEL));
  if (model == end || std::search(identity, end, RECEIVER_MCU, RECEIVER_MCU + strlen(RECEIVER_MCU)) == end) {
    return -1;
  }
  return atoi(model + strlen(RECEIVER_MODEL));
}

bool NextionTerminal::is_receiver(const char *identity) {
  int native = native_version(identity, strlen(identity));
  return native >= RECEIVER_NATIVE_MIN && native <= RECEIVER_NATIVE_MAX;
}

NextionTerminal::Options NextionTerminal::options_for(bool eu_receiver, uint16_t width, uint16_t height) {
  Options options;
  options.width = width;
  options.height = height;
  if (eu_receiver) {
    options.receiver_width = 454;
    options.receiver_height = 320;
    options.rotation = width > height ? 0 : 90;
    options.touch_offset = 26;
    options.max_rows = 320;
  } else {
    options.receiver_width = width;
    options.receiver_height = height;
    options.rotation = 180; // The US TFT's runtime draws upside down on Office's panel.
  }
  return options;
}

void NextionTerminal::set_brightness(uint8_t percent) {
  NextionTerminal::_brightness = percent > 100 ? 100 : percent;
  NextionTerminal::_brightness_changed = true;
  if (NextionTerminal::_task_handle != NULL) {
    xTaskNotifyGive(NextionTerminal::_task_handle);
  }
}

esp_err_t NextionTerminal::start(const Options &options) {
  esp_log_level_set(TAG, ConfigManager::log_level);
  if ((options.rotation != 0 && options.rotation != 180 && !portrait_for(options)) || screen_short_for(options) != options.receiver_height ||
      options.receiver_width > screen_long_for(options) || options.receiver_width == 0) {
    ESP_LOGE(TAG, "Unsupported geometry: screen %ux%u at %u degrees on a %ux%u receiver.", options.width, options.height, options.rotation,
             options.receiver_width, options.receiver_height);
    return ESP_ERR_INVALID_ARG;
  }
  if (options.max_rows < 1 || options.max_rows > options.receiver_height) {
    ESP_LOGE(TAG, "max_rows must be 1-%u.", options.receiver_height);
    return ESP_ERR_INVALID_ARG;
  }
  if (NextionTerminal::_task_handle == NULL) {
    NextionTerminal::_job_done = xSemaphoreCreateBinary();
    if (NextionTerminal::_job_done == NULL) {
      ESP_LOGE(TAG, "Failed to allocate the terminal's buffers.");
      return ESP_ERR_NO_MEM;
    }
    // Internal RAM stack: it drives the UART and logs. On core 0, away from LVGL (core 1): encoding a
    // rectangle costs ~0.8 us a pixel, which on LVGL's core came straight off rendering.
    if (xTaskCreatePinnedToCore(NextionTerminal::_task, "terminal", 6144, NULL, 4, &NextionTerminal::_task_handle, 0) != pdPASS) {
      ESP_LOGE(TAG, "Failed to start the terminal task.");
      return ESP_ERR_NO_MEM;
    }
  }
  if (NextionTerminal::_session) {
    ESP_LOGW(TAG, "A session is already running; stop it before changing its options.");
    return ESP_ERR_INVALID_STATE;
  }
  NextionTerminal::_options = options;
  NextionTerminal::_wanted = true;
  xTaskNotifyGive(NextionTerminal::_task_handle);
  return ESP_OK;
}

void NextionTerminal::stop() {
  NextionTerminal::_wanted = false;
  if (NextionTerminal::_task_handle != NULL) {
    xTaskNotifyGive(NextionTerminal::_task_handle);
  }
}

void NextionTerminal::_set_streaming(bool streaming) {
  if (NextionTerminal::_streaming.exchange(streaming) != streaming && NextionTerminal::on_streaming_changed != nullptr) {
    NextionTerminal::on_streaming_changed(streaming);
  }
}

bool NextionTerminal::_map_touch(int tx, int ty, uint16_t &x, uint16_t &y) {
  const Options &o = NextionTerminal::_options;
  // From the touch controller's frame into the drawing one; a touch at the very edge may fall a
  // little outside, so clamp rather than drop it.
  const int t_height = o.receiver_height;
  tx += o.touch_offset;
  if (tx < -TOUCH_MARGIN || tx >= width_for(o) + TOUCH_MARGIN || ty < -TOUCH_MARGIN || ty >= t_height + TOUCH_MARGIN) {
    return false; // Nonsense, not an edge.
  }
  tx = std::clamp(tx, 0, width_for(o) - 1);
  ty = std::clamp(ty, 0, t_height - 1);
  int crop = o.crop_start ? crop_for(o) : 0;
  switch (o.rotation) {
  case 90:
    x = t_height - 1 - ty;
    y = tx + crop;
    break;
  case 270:
    x = ty;
    y = width_for(o) - 1 - tx + crop;
    break;
  case 180:
    x = width_for(o) - 1 - tx + crop;
    y = t_height - 1 - ty;
    break;
  default:
    x = tx + crop;
    y = ty;
    break;
  }
  return true;
}

esp_err_t NextionTerminal::_wait_job() {
  if (!NextionTerminal::_job_outstanding) {
    return ESP_OK;
  }
  NextionTerminal::_job_outstanding = false;
  BaseType_t taken = xSemaphoreTake(NextionTerminal::_job_done, pdMS_TO_TICKS(JOB_TIMEOUT_MS));
  if (taken != pdTRUE) {
    NextionTerminal::_job_pending = false;
    ESP_LOGE(TAG, "Rectangle %u,%u %ux%u not drawn within %lu ms.", NextionTerminal::_job_x, NextionTerminal::_job_y, NextionTerminal::_job_w,
             NextionTerminal::_job_h, (unsigned long)JOB_TIMEOUT_MS);
    return ESP_FAIL;
  }
  return NextionTerminal::_job_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t NextionTerminal::_submit(int x, int y, int w, int h, bool final) {
  // _job_pixels holds w x h pixels for receiver x,y.
  NextionTerminal::_job_x = x;
  NextionTerminal::_job_y = y;
  NextionTerminal::_job_w = w;
  NextionTerminal::_job_h = h;
  NextionTerminal::_job_final = final;
  xSemaphoreTake(NextionTerminal::_job_done, 0); // A late give from an abandoned job.
  NextionTerminal::_job_ok = false;
  NextionTerminal::_job_pending = true;
  NextionTerminal::_job_outstanding = true;
  xTaskNotifyGive(NextionTerminal::_task_handle);
  return ESP_OK;
}

esp_err_t NextionTerminal::_close_burst() {
  // Resend the previous rectangle's first pixel, ending the burst: _job_pixels still holds it.
  esp_err_t result = NextionTerminal::_wait_job();
  if (result != ESP_OK) {
    return result;
  }
  NextionTerminal::_burst_open = false;
  return NextionTerminal::_submit(NextionTerminal::_job_x, NextionTerminal::_job_y, 1, 1, true);
}

esp_err_t NextionTerminal::draw(int x1, int y1, int x2, int y2, const uint16_t *pixels, bool last) {
  // The last rectangle of the previous call may still be on its way: its pixels are in
  // _job_pixels, so it must be done before they are overwritten. Its failure is reported here.
  esp_err_t previous = NextionTerminal::_wait_job();
  if (!NextionTerminal::_streaming) {
    // No session: give the rectangle buffer back (it may be internal RAM). Only draw()'s task
    // touches it, and no job is outstanding now.
    heap_caps_free(NextionTerminal::_job_pixels);
    NextionTerminal::_job_pixels = NULL;
    NextionTerminal::_job_capacity = 0;
    NextionTerminal::_burst_open = false;
    return ESP_ERR_INVALID_STATE;
  }
  if (previous != ESP_OK) {
    return previous;
  }
  const Options &o = NextionTerminal::_options;
  const int stride = x2 - x1 + 1;
  const int crop = o.crop_start ? crop_for(o) : 0;
  const int t_width = width_for(o);
  const bool portrait = portrait_for(o);
  // Clip the long side to the lines the receiver can reach.
  int lo = crop, hi = crop + t_width - 1;
  int cx1 = x1, cx2 = x2, cy1 = y1, cy2 = y2;
  if (portrait) {
    cy1 = std::max(cy1, lo);
    cy2 = std::min(cy2, hi);
  } else {
    cx1 = std::max(cx1, lo);
    cx2 = std::min(cx2, hi);
  }
  if (cx1 > cx2 || cy1 > cy2) {
    // All of it in the lines that are not drawn. A burst still open needs its end: an empty
    // refresh tail cannot close it, so the previous rectangle must have (draw() is told `last`
    // only once per refresh, and an all-cropped last area is rare): close it with a 1-pixel redraw.
    if (last && NextionTerminal::_burst_open) {
      return NextionTerminal::_close_burst();
    }
    return ESP_OK;
  }

  // The rectangle in the receiver's coordinates, and where each of its pixels comes from.
  int tx0, ty0, tw, th;
  switch (o.rotation) {
  case 90: // tx = y - crop, ty = 319 - x
    tx0 = cy1 - crop;
    tw = cy2 - cy1 + 1;
    ty0 = o.receiver_height - 1 - cx2;
    th = cx2 - cx1 + 1;
    break;
  case 270: // tx = 453 - (y - crop), ty = x
    tx0 = t_width - 1 - (cy2 - crop);
    tw = cy2 - cy1 + 1;
    ty0 = cx1;
    th = cx2 - cx1 + 1;
    break;
  case 180: // tx = t_width - 1 - (x - crop), ty = t_height - 1 - y
    tx0 = t_width - 1 - (cx2 - crop);
    tw = cx2 - cx1 + 1;
    ty0 = o.receiver_height - 1 - cy2;
    th = cy2 - cy1 + 1;
    break;
  default: // tx = x - crop, ty = y
    tx0 = cx1 - crop;
    tw = cx2 - cx1 + 1;
    ty0 = cy1;
    th = cy2 - cy1 + 1;
    break;
  }

  const int max_rows = o.max_rows;
  size_t needed = (size_t)tw * std::min(max_rows, th);
  // Grow; and move to internal RAM whenever it fits there (it may have grown in PSRAM before).
  // Not again for a size internal RAM already refused.
  static size_t internal_refused = 0;
  const bool want_internal = needed * sizeof(uint16_t) <= INTERNAL_JOB_BYTES && needed != internal_refused;
  const bool is_internal = NextionTerminal::_job_pixels != NULL && esp_ptr_internal(NextionTerminal::_job_pixels);
  if (needed > NextionTerminal::_job_capacity || (want_internal && !is_internal)) {
    // Only draw()'s task touches the buffer outside a job, and no job is running. Internal RAM if
    // it is small enough to spare (the encoder reads every pixel of it), else PSRAM.
    heap_caps_free(NextionTerminal::_job_pixels);
    NextionTerminal::_job_pixels = NULL;
    if (want_internal) {
      NextionTerminal::_job_pixels = static_cast<uint16_t *>(heap_caps_malloc(needed * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (NextionTerminal::_job_pixels == NULL) {
      if (want_internal) {
        internal_refused = needed;
      }
      NextionTerminal::_job_pixels = static_cast<uint16_t *>(heap_caps_malloc(needed * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    }
    ESP_LOGD(TAG, "Rectangle buffer: %u pixels in %s.", (unsigned)needed,
             NextionTerminal::_job_pixels != NULL && esp_ptr_internal(NextionTerminal::_job_pixels) ? "internal RAM" : "PSRAM");
    NextionTerminal::_job_capacity = NextionTerminal::_job_pixels != NULL ? needed : 0;
    if (NextionTerminal::_job_pixels == NULL) {
      ESP_LOGE(TAG, "Failed to allocate %u pixels for a rectangle.", (unsigned)needed);
      return ESP_ERR_NO_MEM;
    }
  }
  for (int row0 = 0; row0 < th; row0 += max_rows) {
    int rows = std::min(max_rows, th - row0);
    nspanel_terminal::copy_rotated(o.rotation, NextionTerminal::_job_pixels, tw, rows, tx0, ty0 + row0, pixels, x1, y1, stride, crop, t_width, o.receiver_height);
    // A burst (protocol 7+): every rectangle of a refresh but its last ends with END x=0, so the
    // receiver confirms only the last.
    const bool final = last && row0 + rows >= th;
    NextionTerminal::_burst_open = !final;
    esp_err_t result = NextionTerminal::_submit(tx0, ty0 + row0, tw, rows, final);
    if (result != ESP_OK) {
      return result;
    }
    // Return with the last rectangle still on its way: the caller's pixels were copied, so it can
    // render the next area meanwhile. Any earlier rectangle must finish first (one buffer).
    if (row0 + rows < th) {
      result = NextionTerminal::_wait_job();
      if (result != ESP_OK) {
        return result;
      }
    }
  }
  return ESP_OK;
}

void NextionTerminal::_task(void *param) {
  int failures = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (NextionTerminal::_wanted) {
      NextionTerminal::_session = true;
      bool clean = NextionTerminal::_session_once();
      NextionTerminal::_session = false;
      if (clean) {
        failures = 0;
        continue; // Ended because stop() was called: _wanted is false.
      }
      if (++failures >= MAX_ATTEMPTS) {
        ESP_LOGE(TAG, "%d sessions failed in a row; not trying again until asked.", failures);
        NextionTerminal::_wanted = false;
        failures = 0;
      } else if (NextionTerminal::_wanted) {
        ESP_LOGW(TAG, "Session failed; trying again (%d of %d).", failures + 1, MAX_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }
  }
}

bool NextionTerminal::_command(const char *command) {
  // A byte at a time with a gap, as the nspanel-terminal host sends the commands that set the receiver up.
  size_t length = strlen(command);
  for (size_t i = 0; i < length + 3; i++) {
    uint8_t byte = i < length ? (uint8_t)command[i] : 0xFF;
    if (Nextion::terminal_write(&byte, 1) != 1) {
      return false;
    }
    esp_rom_delay_us(190);
  }
  return true;
}

int NextionTerminal::_read_reply(uint8_t *buffer, size_t size, uint32_t timeout_ms) {
  // Up to and including FF FF FF; returns the length, or -1.
  size_t have = 0;
  uint32_t start = NextionTerminal::_now();
  while (have < size && NextionTerminal::_now() - start < timeout_ms) {
    if (Nextion::terminal_read(buffer + have, 1, pdMS_TO_TICKS(20)) == 1) {
      have++;
      if (have >= 3 && buffer[have - 1] == 0xFF && buffer[have - 2] == 0xFF && buffer[have - 3] == 0xFF) {
        return (int)have;
      }
    }
  }
  return -1;
}

bool NextionTerminal::_session_once() {
  esp_err_t acquired = Nextion::terminal_acquire(1000);
  if (acquired != ESP_OK) {
    ESP_LOGE(TAG, "Could not take the display's UART: %s", esp_err_to_name(acquired));
    return false;
  }

  // Never send touch_j to a display without the receiver: on the stock runtime it starts touch calibration.
  // The TFT's startup code sets the display's baud rate: NSPM's TFT 115200, the terminal TFTs
  // leaves 9600, where NSPM's boot (115200, then the operating rate) cannot reach it. If the
  // link's rate gets no answer, try those. The receiver moves to sys0's rate itself, so the rate
  // the setup runs at does not matter.
  uint32_t link = Nextion::get_link_baud();
  uint32_t candidates[3] = {link, 0, 0};
  size_t count = 1;
  for (uint32_t baud : {9600u, 115200u}) {
    if (baud != link) {
      candidates[count++] = baud;
    }
  }
  uint8_t reply[128];
  int length = -1;
  for (size_t i = 0; i < count && length < 0; i++) {
    uint32_t baud = candidates[i];
    if (baud != link) {
      Nextion::terminal_set_uart_baud(baud);
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Whatever an attempt at another rate left in the display's parser would swallow `connect`
    // (it then answers 0x1A): end it with a bare terminator first, as NSPM's boot does.
    const uint8_t terminator[] = {0xFF, 0xFF, 0xFF};
    Nextion::terminal_write(terminator, sizeof(terminator));
    vTaskDelay(pdMS_TO_TICKS(50));
    Nextion::terminal_drain();
    if (NextionTerminal::_command("connect")) {
      // Skip error replies (0x1A and the like) until comok, or the time is up.
      uint32_t start = NextionTerminal::_now();
      while (NextionTerminal::_now() - start < 700) {
        length = NextionTerminal::_read_reply(reply, sizeof(reply) - 1, 700 - (NextionTerminal::_now() - start));
        static const char COMOK[] = "comok";
        if (length < 0 || std::search(reply, reply + length, COMOK, COMOK + 5) != reply + length) {
          break;
        }
        length = -1;
      }
    }
    if (length >= 0 && baud != link) {
      ESP_LOGW(TAG, "The display answered at %lu baud, not the link's %lu; setting the receiver up at %lu.", (unsigned long)baud,
               (unsigned long)link, (unsigned long)baud);
    }
  }
  if (length < 0) {
    ESP_LOGE(TAG, "No reply to connect at %lu, 9600 or 115200 baud.", (unsigned long)link);
    Nextion::terminal_set_uart_baud(link);
    Nextion::terminal_release(false);
    return false;
  }
  reply[length - 3] = 0;
  const char *text = reinterpret_cast<const char *>(reply);
  // Native 94-127: nspanel-terminal's protocol-8 TFTs, and builds of them that carry a version of
  // their own (the Nextion updater keeps native code whose version matches the installed one, so a
  // changed build must be numbered apart). READY then proves protocol 8. Not NSPM's TFTs (is_receiver()).
  if (!NextionTerminal::is_receiver(text)) {
    ESP_LOGE(TAG, "The display has no terminal receiver (connect: %s). Install nspanel-terminal's terminal-us.tft (or -eu) first.", text);
    NextionTerminal::_wanted = false; // Retrying cannot help.
    Nextion::terminal_set_uart_baud(link);
    Nextion::terminal_release(false);
    return true;
  }
  ESP_LOGD(TAG, "Receiver found (%s).", text);

  uint16_t session = (uint16_t)esp_timer_get_time() | 1;
  char sys2[24];
  snprintf(sys2, sizeof(sys2), "sys2=%u", session);
  const char *setup[] = {"bkcmd=0", "sys0=4000000", "sys1=1", sys2}; // As nspanel-terminal's host (no sendxy=0).
  for (const char *command : setup) {
    Nextion::terminal_drain();
    NextionTerminal::_command(command);
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  Nextion::terminal_drain();
  NextionTerminal::_command("touch_j"); // Runs the receiver, which moves to sys0's baud rate.
  if (Nextion::terminal_set_uart_baud(STREAM_BAUD) != ESP_OK) {
    ESP_LOGE(TAG, "Could not move the UART to %lu baud.", (unsigned long)STREAM_BAUD);
    Nextion::terminal_release(true);
    return false;
  }
  ESP_LOGI(TAG, "Session %u: UART at %lu baud, waiting for the receiver.", session, (unsigned long)STREAM_BAUD);

  bool clean = NextionTerminal::_stream_session(session);
  NextionTerminal::_set_streaming(false);
  if (NextionTerminal::_job_pending.exchange(false)) {
    NextionTerminal::_job_ok = false;
    xSemaphoreGive(NextionTerminal::_job_done);
  }
  Nextion::terminal_release(true);
  return clean;
}

void NextionTerminal::_finish_job(bool ok) {
  NextionTerminal::_job_ok = ok;
  xSemaphoreGive(NextionTerminal::_job_done);
}

// In IRAM, with the encoder it inlines: it runs for every byte sent, on core 0 next to Wi-Fi's
// code, and the flash cache is shared.
IRAM_ATTR bool NextionTerminal::_stream_session(uint16_t session) {
  using nspanel_terminal::StreamEngine;
  namespace protocol = nspanel_terminal::protocol;
  // Static: the engine's buffers stay off this task's stack.
  static StreamEngine engine;
  const Options &o = NextionTerminal::_options;
  engine.begin(session, NextionTerminal::_now(), width_for(o), o.receiver_height);

  uint8_t buffer[256];
  uint32_t fed = 0;         // Reply bytes fed, to know where the current 32-byte reply ends.
  bool job_active = false;  // A rectangle of draw()'s is with the engine.
  bool job_final = true;    // ... and it ends its burst.
  unsigned completed_before = 0;
  bool exit_sent = false;
  bool wrote = true;
  int64_t last_stats = esp_timer_get_time();
  uint32_t stats_rects = NextionTerminal::_rectangles, stats_bytes = NextionTerminal::_bytes;

  for (;;) {
    // Read: when nothing went out last time, wait (briefly) for the rest of the reply under way.
    // Between the rectangles of a burst with none handed over yet, wait for draw() (a notification),
    // not for reply bytes: the next band is what holds things up.
    const bool between = engine.stage() == StreamEngine::Stage::BETWEEN && !engine.pending();
    bool busy = job_active || exit_sent || engine.stage() == StreamEngine::Stage::READY || (!engine.idle() && !between);
    int n;
    if (!wrote && busy) {
      n = Nextion::terminal_read(buffer, protocol::RESPONSE - fed % protocol::RESPONSE, 1);
      if (n > 0) {
        int more = Nextion::terminal_read(buffer + n, sizeof(buffer) - n, 0);
        n += more > 0 ? more : 0;
      }
    } else {
      if (!wrote) {
        ulTaskNotifyTake(pdTRUE, 1); // Idle: until draw() or stop(), or the next tick.
      }
      n = Nextion::terminal_read(buffer, sizeof(buffer), 0);
    }
    uint32_t now = NextionTerminal::_now();
    for (int i = 0; i < n; i++) {
      protocol::Reply reply;
      fed++;
      if (!engine.feed(buffer[i], now, reply)) {
        continue;
      }
      if ((reply.status == protocol::READY || reply.status == protocol::READY_BRIGHTNESS) && engine.stage() == StreamEngine::Stage::IDLE) {
        ESP_LOGD(TAG, "Receiver ready%s.", reply.status == protocol::READY_BRIGHTNESS ? " (live brightness)" : "");
        NextionTerminal::_brightness_changed = true; // A new session starts at the TFT's own level.
        NextionTerminal::_set_streaming(true);
      } else if (reply.status == protocol::ERROR) {
        NextionTerminal::_resyncs++;
        ESP_LOGW(TAG, "Receiver reported an error; resynchronising (SYNC).");
      }
      uint16_t x, y;
      if (reply.freshness == 1) {
        static bool was_down = false;
        bool down = reply.mask & 1;
        // Signed 16-bit: the touch frame starts above the drawing one.
        int raw_x = (int16_t)reply.x, raw_y = (int16_t)reply.y;
        bool mapped = NextionTerminal::_map_touch(raw_x, raw_y, x, y);
        // Edges at info, moves at debug: what the receiver reports, and where it lands on the screen.
        if (down != was_down) {
          ESP_LOGD(TAG, "Touch %s: receiver %d,%d (mask 0x%02X) -> screen %d,%d.", down ? "down" : "up", raw_x, raw_y, reply.mask, mapped ? x : -1,
                   mapped ? y : -1);
        } else if (down) {
          ESP_LOGD(TAG, "Touch move: receiver %d,%d -> screen %d,%d.", raw_x, raw_y, mapped ? x : -1, mapped ? y : -1);
        }
        // A "move" with nothing pressed means nothing: the receiver sends one at 0,0 every ~20 s on
        // Office, which would count as a touch (and hold off the screensaver).
        const bool meaningful = down || was_down;
        was_down = down;
        if (mapped && meaningful && NextionTerminal::on_touch != nullptr) {
          NextionTerminal::on_touch(down, x, y);
        }
      }
    }
    if (engine.failed()) {
      ESP_LOGE(TAG, "Session %u stopped: %s.", session, engine.error());
      if (job_active) {
        NextionTerminal::_finish_job(false);
      }
      return false;
    }
    if (engine.stage() == StreamEngine::Stage::STOPPED) {
      ESP_LOGI(TAG, "Session %u ended; the receiver said goodbye.", session);
      if (job_active) {
        NextionTerminal::_finish_job(false);
      }
      return true;
    }

    // A rectangle that ends its burst is done at its DONE; one with more to follow as soon as its END
    // is queued (BETWEEN): the engine has read all its pixels, so draw() may reuse the buffer.
    const bool job_done = job_final ? engine.completed() != completed_before : engine.stage() == StreamEngine::Stage::BETWEEN;
    if (job_active && job_done) {
      job_active = false;
      NextionTerminal::_rectangles++;
      NextionTerminal::_finish_job(true);
    }
    if (!NextionTerminal::_wanted && !exit_sent && engine.idle()) {
      engine.request_exit();
      exit_sent = true;
    }
    // The backlight, between refreshes: the engine sends it only when idle.
    if (!exit_sent && engine.idle() && engine.live_brightness() && NextionTerminal::_brightness >= 0 &&
        NextionTerminal::_brightness_changed.exchange(false)) {
      engine.request_brightness((uint8_t)NextionTerminal::_brightness.load());
      ESP_LOGD(TAG, "Backlight to %d%%.", NextionTerminal::_brightness.load());
    }
    if (!job_active && !exit_sent && engine.can_submit() && NextionTerminal::_job_pending.exchange(false)) {
      completed_before = engine.completed();
      job_final = NextionTerminal::_job_final;
      if (engine.submit(NextionTerminal::_job_x, NextionTerminal::_job_y, NextionTerminal::_job_w, NextionTerminal::_job_h,
                        reinterpret_cast<const uint8_t *>(NextionTerminal::_job_pixels), now, job_final)) {
        job_active = true;
      } else {
        ESP_LOGE(TAG, "Rectangle %u,%u %ux%u refused.", NextionTerminal::_job_x, NextionTerminal::_job_y, NextionTerminal::_job_w, NextionTerminal::_job_h);
        NextionTerminal::_finish_job(false);
      }
    }

    // Write everything the credits allow, a record at a time.
    wrote = false;
    for (;;) {
      engine.prepare(NextionTerminal::_now());
      if (engine.failed()) {
        break; // Reported at the top of the loop.
      }
      size_t allowed = engine.allowance();
      if (allowed == 0) {
        break;
      }
      int written = Nextion::terminal_write(engine.data(), allowed);
      if (written <= 0) {
        break;
      }
      engine.sent(written, NextionTerminal::_now());
      NextionTerminal::_bytes += written;
      wrote = true;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us - last_stats > STATS_LOG_INTERVAL_US) {
      uint32_t rects = NextionTerminal::_rectangles - stats_rects;
      uint32_t bytes = NextionTerminal::_bytes - stats_bytes;
      if (rects > 0) {
        ESP_LOGD(TAG, "Last %lld s: %lu rectangles, %lu KB on the wire (%lu KB/s), %lu resyncs since boot.", (now_us - last_stats) / 1000000,
                 (unsigned long)rects, (unsigned long)(bytes / 1024), (unsigned long)(bytes * 1000000ULL / (now_us - last_stats) / 1024),
                 (unsigned long)NextionTerminal::_resyncs);
      }
      last_stats = now_us;
      stats_rects = NextionTerminal::_rectangles;
      stats_bytes = NextionTerminal::_bytes;
    }
  }
}
