#pragma once
#if defined(BOARD_CUSTOM)
// Phase 2a: declarations only. Phase 2b writes VoiceRecorder.cpp from these and docs/VOICE_RECORDER.md,
// which has the reasons behind the numbers here. Nothing includes this header yet.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include <esp_http_server.h>

/**
 * Hold-to-talk voice messages on the custom PCB: records the processed microphone audio (16 kHz mono,
 * 16-bit, from VoiceCapture's streaming API) into PSRAM, serves it as a WAV at
 * http://<panel IP>/rec/<token>.wav, and announces that URL on nspanel/<mac>/voice/recording for Home
 * Assistant to play.
 *
 * States, and the calls that move between them:
 *
 *   IDLE --start()--> STARTING --(codec open, settle)--> RECORDING --stop()--> IDLE (recording READY, announced)
 *                                                          |  |
 *                                                          |  +--cancel()--> IDLE (nothing kept)
 *                                                          +--(MAX_FRAMES)--> FULL --stop()/auto--> IDLE
 *
 * Tasks: start(), stop(), cancel() from any one task at a time (the LVGL task, or mqtt_dispatch for the
 * test command); they are serialised by an internal mutex, never called from an ISR or esp_timer
 * callback. level(), state() and frames() from any task, lock-free. serve() from the web server's task.
 * The sink VoiceRecorder hands VoiceCapture runs on the capture task.
 */
class VoiceRecorder {
public:
  static constexpr uint32_t SAMPLE_RATE = 16000; // Must equal VoiceCapture::SAMPLE_RATE (static_assert in the .cpp)
  static constexpr uint32_t MAX_SECONDS = 30;
  static constexpr size_t MAX_FRAMES = MAX_SECONDS * SAMPLE_RATE; // 480 000 samples, 960 000 bytes
  // Shorter than this at stop() is an accidental tap: discarded, ESP_ERR_INVALID_SIZE.
  static constexpr uint32_t MIN_MS = 300;
  // Recordings kept: the one HA may still be downloading, and the one being made.
  static constexpr size_t SLOTS = 2;
  static constexpr size_t WAV_HEADER_BYTES = 44;
  // Each slot's PSRAM buffer: the header, then the samples. 960 044 bytes; 1.92 MB for both.
  static constexpr size_t SLOT_BYTES = WAV_HEADER_BYTES + MAX_FRAMES * sizeof(int16_t);
  // How long a finished recording's URL works, from stop().
  static constexpr uint32_t TOKEN_TTL_S = 300;
  // 16 bytes from esp_fill_random(), as 32 lowercase hex digits.
  static constexpr size_t TOKEN_BYTES = 16;
  static constexpr size_t TOKEN_CHARS = TOKEN_BYTES * 2;
  // The ES7210 gain the recorder asks VoiceCapture for.
  static constexpr int GAIN_DB = 30;

  enum class State : uint8_t {
    IDLE,      // Nothing being recorded (finished recordings may still be served).
    STARTING,  // start() returned OK; the codec is settling, no samples kept yet.
    RECORDING, // Samples are going into the slot.
    FULL,      // MAX_FRAMES reached: further samples are dropped until stop() (or the auto-send) runs.
  };

  // A finished recording, as stop() reports it and the MQTT message carries it.
  struct Recording {
    char token[TOKEN_CHARS + 1]; // NUL-terminated
    uint32_t duration_ms;        // frames * 1000 / SAMPLE_RATE
    uint32_t bytes;              // The whole WAV, header included: the Content-Length of a full GET
    int64_t expires_us;          // esp_timer clock: stop() time + TOKEN_TTL_S
    uint32_t sequence;           // 1, 2, 3, ... since boot: tells HA's automation one message from a repeat
  };

  struct Options {
    // At MAX_FRAMES, have the recorder's own task run stop() and announce(), as if the user let go. False:
    // stay FULL until the caller's stop() or cancel().
    bool auto_send_at_limit = true;
    // > 0: the recorder's task runs stop() and announce() this long after start() (the MQTT test command).
    uint32_t stop_after_ms = 0;
  };

  /**
   * Start a recording. The first call allocates both slots in PSRAM (SLOT_BYTES each, kept until reboot,
   * never freed) and the recorder's task. Picks the slot to record into (see the .md: never one with a
   * reader), invalidates the token it held, and starts VoiceCapture with the recorder's sink. Returns once
   * VoiceCapture has opened the codec (state STARTING or RECORDING), typically under 50 ms, at most ~1 s.
   *
   * @return ESP_OK;
   *         ESP_ERR_INVALID_STATE already recording (STARTING, RECORDING or FULL), or another VoiceCapture
   *                               user (the temporary /rec/test.wav) has the microphones;
   *         ESP_ERR_NOT_FINISHED  both slots are being downloaded right now (busy; try again in a second);
   *         ESP_ERR_NO_MEM        the PSRAM slots or the task could not be allocated;
   *         ESP_ERR_NOT_ALLOWED   no IP address, so no URL could be given out (checked before recording);
   *         anything VoiceCapture::start() returns (codec or I2S failure).
   */
  static esp_err_t start(const Options &options);
  /** start() with the default Options. */
  static esp_err_t start();

  /**
   * End the recording and make it servable: stops VoiceCapture (waits for its last sink call, bounded at
   * ~1 s), writes the WAV header, generates the token and marks the slot READY. Does not publish: call
   * announce() (or use the auto paths in Options). Does not block on the network.
   *
   * @param out: filled on ESP_OK; may be nullptr.
   * @return ESP_OK;
   *         ESP_ERR_INVALID_STATE not recording;
   *         ESP_ERR_INVALID_SIZE  shorter than MIN_MS: discarded, the slot is free again;
   *         ESP_FAIL              VoiceCapture reported an I2S failure during the recording: discarded.
   */
  static esp_err_t stop(Recording *out);

  /**
   * Queue the MQTT announcement of a finished recording to the recorder's task, which publishes it (an MQTT
   * publish can block for seconds on a weak link, so never on the caller's). Returns at once.
   * @return ESP_OK queued; ESP_ERR_NOT_FOUND the token is no longer servable; ESP_ERR_NO_MEM queue full.
   */
  static esp_err_t announce(const Recording &recording);

  /** stop() then announce(): what the UI calls on release. Same errors as stop(), then announce(). */
  static esp_err_t stop_and_send(Recording *out = nullptr);

  /**
   * Abandon the recording: stops VoiceCapture, keeps nothing, publishes nothing. The slot is free again.
   * @return ESP_OK; ESP_ERR_INVALID_STATE not recording.
   */
  static esp_err_t cancel();

  /** The current state. Any task. */
  static State state() { return _state.load(std::memory_order_acquire); }

  /**
   * The input level for a meter: the peak of the last 20 ms chunk of processed audio, as 0-100 (0 at
   * -60 dBFS and below, 100 at 0 dBFS, linear in dB). 0 when not RECORDING. Written by the capture task,
   * read by any task (the UI polls it, e.g. every 50 ms).
   */
  static uint8_t level() { return _level.load(std::memory_order_relaxed); }

  /** Frames kept so far in the current recording (elapsed time = frames() * 1000 / SAMPLE_RATE ms). */
  static uint32_t frames() { return _frames.load(std::memory_order_relaxed); }

  /**
   * The GET and HEAD handler for /rec/<token>.wav (registered by WebManager, with httpd_uri_match_wildcard,
   * as the pattern /rec/ followed by the wildcard; WebManager keeps its own /rec/test.wav registered first while it lasts). Runs on the web
   * server's task. 200 with the whole WAV, 206 for a single byte range, 404 for an unknown token, 410 for
   * one that expired, 416 for a range outside the file. See the .md for the headers.
   */
  static esp_err_t serve(httpd_req_t *req);

  /**
   * The MQTT test trigger, from UpdateManager's nspanel/<mac>/command handler (mqtt_dispatch task), for
   * {"command": "voice_record", "action": "start"|"stop"|"cancel", "seconds": 1-30}. "start" with
   * "seconds" records that long and sends; "stop" is stop_and_send(). Logs the outcome. Temporary: the UI
   * replaces it (phase 3).
   */
  static void handle_command(const char *action, int seconds);

private:
  static inline std::atomic<State> _state = State::IDLE;
  static inline std::atomic<uint8_t> _level = 0;
  static inline std::atomic<uint32_t> _frames = 0;
};
#endif
