#pragma once
#if defined(BOARD_CUSTOM)
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

/**
 * The custom PCB's (Waveshare ESP32-P4-WIFI6-Touch-LCD-4B) microphones: an ES7210 4-channel ADC, driven
 * with esp_codec_dev, its control on BoardI2c's bus (0x40) and its data over I2S 0 as TDM: 4 slots of
 * 16 bits at 16 kHz, MCLK 256 x fs (4.096 MHz), the P4 the I2S master and the ES7210 the slave.
 *
 *   MCLK GPIO 13, BCLK GPIO 12, WS GPIO 10, ES7210 SDOUT -> P4 GPIO 11
 *   (P4 -> ES8311 on GPIO 9 is the playback side, unused, as is the speaker amplifier's enable, GPIO 53)
 *
 * The ES7210's inputs: MIC1 the front-left microphone, MIC2 the front-right, MIC3 the ES8311's output
 * looped back (the reference for echo cancellation), MIC4 not connected. The TDM slots carry MIC1, MIC3,
 * MIC2, MIC4, in that order (Waveshare's, verified on their hardware); record() hands back MIC1, MIC2 and
 * the reference.
 *
 * Nothing starts at boot. The capture task, the I2S channel and the codec come up on the first record(),
 * and between recordings the ES7210 is powered down and I2S stopped. The I2S reads run on the capture
 * task, not the caller's.
 */
class VoiceCapture {
public:
  static constexpr uint32_t SAMPLE_RATE = 16000;
  // What record() writes, interleaved, in this order.
  enum Channel : uint8_t { MIC1 = 0, MIC2 = 1, REF = 2, CHANNELS = 3 };
  // The ES7210's PGA gain, every input: 0 to 37.5 dB, in 3 dB steps up to 33.
  static constexpr int DEFAULT_GAIN_DB = 30;

  /**
   * Record `frames` frames (1/SAMPLE_RATE s each) into `out`: frames x CHANNELS samples, interleaved
   * MIC1, MIC2, REF (it may be in PSRAM). Blocks the calling task until done. One recording at a time:
   * ESP_ERR_INVALID_STATE while another is under way. The per-slot peak and RMS of each recording are
   * logged.
   */
  static esp_err_t record(int16_t *out, size_t frames, int gain_db = DEFAULT_GAIN_DB);

private:
  struct Request {
    int16_t *out;
    size_t frames;
    int gain_db;
    esp_err_t result;
  };

  static void _task(void *param);
  // Capture task: the I2S channel and the codec, once. Undone if any of it fails, so a later record() tries again.
  static esp_err_t _init_codec();
  static void _deinit_codec();
  static esp_err_t _capture(Request *request);
  /**
   * Capture task: `frames` frames just read, one array per channel (MIC1, MIC2, REF), into the recording
   * at `out`. The seam for audio processing (see the definition).
   */
  static void _process(const int16_t *const channels[CHANNELS], size_t frames, int16_t *out);

  static inline std::atomic<bool> _busy = false;
  static inline QueueHandle_t _requests = NULL;
  static inline SemaphoreHandle_t _done = NULL;
};
#endif
