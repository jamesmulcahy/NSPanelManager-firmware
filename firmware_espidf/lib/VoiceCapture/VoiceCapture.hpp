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
 *
 * Processing (on the capture task, as the audio comes in): MIC1 and MIC2 through esp-sr's GSC beamformer
 * (two microphones, steered broadside: at the talker in front of the panel), then WebRTC noise suppression
 * (esp-sr's ns_pro), out as 16 kHz mono. Which of the stages run, and which of their outputs a recording
 * holds, is the Options' `outputs`: any set of Output bits, written interleaved in Output order, so one
 * recording can hold several stages' outputs of the same audio, to compare. esp-sr's handles are made for
 * each recording that needs them and freed after it (see _capture()).
 */
class VoiceCapture {
public:
  static constexpr uint32_t SAMPLE_RATE = 16000;
  // What record() writes, interleaved, in this order.
  enum Channel : uint8_t { MIC1 = 0, MIC2 = 1, REF = 2, CHANNELS = 3 };
  // The ES7210's PGA gain, every input: 0 to 37.5 dB, in 3 dB steps up to 33.
  static constexpr int DEFAULT_GAIN_DB = 30;

  /**
   * What a recording can hold, one channel each, interleaved in this order (the lowest bit first).
   *   OUT_MIC1, OUT_MIC2, OUT_REF  the inputs as read (after the high-pass filter, if on)
   *   OUT_NS                       WebRTC NS on MIC1 alone
   *   OUT_BF                       the GSC beamformer on MIC1 + MIC2
   *   OUT_BF_NS                    the beamformer, then WebRTC NS: the voice path
   */
  enum Output : uint8_t {
    OUT_MIC1 = 1 << 0,
    OUT_MIC2 = 1 << 1,
    OUT_REF = 1 << 2,
    OUT_NS = 1 << 3,
    OUT_BF = 1 << 4,
    OUT_BF_NS = 1 << 5,
  };
  // The modes the temporary /rec/test.wav offers. RAW is what record() wrote before processing existed.
  static constexpr uint8_t MODE_RAW = OUT_MIC1 | OUT_MIC2 | OUT_REF;
  static constexpr uint8_t MODE_NS = OUT_NS;
  static constexpr uint8_t MODE_BF = OUT_BF;
  static constexpr uint8_t MODE_BF_NS = OUT_BF_NS;
  // All four stages of the same audio: MIC1 as it came in, NS(MIC1), GSC, NS(GSC).
  static constexpr uint8_t MODE_COMPARE = OUT_MIC1 | OUT_NS | OUT_BF | OUT_BF_NS;

  struct Options {
    uint8_t outputs = MODE_RAW;
    // The ES7210's PGA gain.
    int gain_db = DEFAULT_GAIN_DB;
    // WebRTC NS's strength: 0 mild, 1 medium, 2 aggressive.
    int ns_level = 1;
    // A 2nd-order Butterworth high-pass on MIC1, MIC2 and REF before anything else; 0 is off.
    int hpf_hz = 0;
    // The beamformer's steering, degrees from the axis through MIC1 and MIC2: 90 is broadside, straight
    // out of the panel (and anywhere in the plane at right angles to that axis).
    float bf_angle_deg = 90.0f;
    // The distance between MIC1 and MIC2, for the beamformer's geometry.
    float mic_spacing_mm = 65.0f;
  };

  // How many channels (samples per frame) a recording with these outputs holds.
  static int channels(uint8_t outputs);

  /**
   * Record `frames` frames (1/SAMPLE_RATE s each) into `out`: frames x channels(options.outputs) samples,
   * interleaved (it may be in PSRAM). Blocks the calling task until done. One recording at a time:
   * ESP_ERR_INVALID_STATE while another is under way; ESP_ERR_INVALID_ARG for no outputs or a bad option.
   * Each recording logs the inputs' peak and RMS, each output's, the CPU time of each stage and the heap
   * taken.
   */
  static esp_err_t record(int16_t *out, size_t frames, const Options &options);
  // The three inputs, interleaved MIC1, MIC2, REF (MODE_RAW).
  static esp_err_t record(int16_t *out, size_t frames, int gain_db = DEFAULT_GAIN_DB);

private:
  struct Request {
    int16_t *out;
    size_t frames;
    Options options;
    esp_err_t result;
  };
  struct Pipeline;

  static void _task(void *param);
  // Capture task: the I2S channel and the codec, once. Undone if any of it fails, so a later record() tries again.
  static esp_err_t _init_codec();
  static void _deinit_codec();
  static esp_err_t _capture(Request *request);
  /**
   * Capture task: a chunk of `frames` frames just read, one array per channel (MIC1, MIC2, REF), through
   * the stages the pipeline has, into the recording at `out`, interleaved as its outputs say.
   */
  static void _process(Pipeline &pipeline, int16_t *const channels[CHANNELS], size_t frames, int16_t *out);

  static inline std::atomic<bool> _busy = false;
  static inline QueueHandle_t _requests = NULL;
  static inline SemaphoreHandle_t _done = NULL;
};
#endif
