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
 * Processing (on the capture task, as the audio comes in). The voice path (MODE_VOICE, what the recorder
 * takes): a microphone (MIC1, MIC2 or their average), the high-pass filter, WebRTC noise suppression
 * (esp-sr's ns_pro), makeup gain (an AGC, or a fixed gain) and a peak limiter, out as 16 kHz mono. The
 * beamformer (esp-sr's GSC, MIC1 + MIC2 steered broadside) stays for comparison (MODE_BF, MODE_BF_NS) but
 * is not on the voice path: see docs/CUSTOM_PCB.md. Which of the stages run, and which of their outputs a recording
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
   *   OUT_BF_NS                    the beamformer, then WebRTC NS
   *   OUT_VOICE                    the production chain: Options::voice_mic, HPF, NS, makeup gain, limiter
   *   OUT_VOICE_B                  the same chain on Options::voice_mic_b, for comparing mic choices
   */
  enum Output : uint8_t {
    OUT_MIC1 = 1 << 0,
    OUT_MIC2 = 1 << 1,
    OUT_REF = 1 << 2,
    OUT_NS = 1 << 3,
    OUT_BF = 1 << 4,
    OUT_BF_NS = 1 << 5,
    OUT_VOICE = 1 << 6,
    OUT_VOICE_B = 1 << 7,
  };
  // Which input the voice chain starts from. AVG is (MIC1 + MIC2) / 2.
  enum VoiceMic : uint8_t { VOICE_MIC1 = 0, VOICE_MIC2 = 1, VOICE_AVG = 2 };
  // The modes the temporary /rec/test.wav offers. RAW is what record() wrote before processing existed.
  static constexpr uint8_t MODE_RAW = OUT_MIC1 | OUT_MIC2 | OUT_REF;
  static constexpr uint8_t MODE_NS = OUT_NS;
  static constexpr uint8_t MODE_BF = OUT_BF;
  static constexpr uint8_t MODE_BF_NS = OUT_BF_NS;
  // All four beamformer stages of the same audio: MIC1 as it came in, NS(MIC1), GSC, NS(GSC).
  static constexpr uint8_t MODE_STAGES = OUT_MIC1 | OUT_NS | OUT_BF | OUT_BF_NS;
  /**
   * THE PRODUCTION MODE: mono 16 kHz int16, the voice chain. Record it with voice_options() (gain 36 dB,
   * HPF 100 Hz, NS level 1, the microphones' average, AGC and limiter on): Options with `outputs`
   * MODE_VOICE and nothing else needs setting.
   */
  static constexpr uint8_t MODE_VOICE = OUT_VOICE;
  // The listening comparison, one recording, 4 channels: MIC1 raw, NS(MIC1), the voice chain on MIC1, the
  // voice chain on voice_mic_b (the average by default).
  static constexpr uint8_t MODE_COMPARE = OUT_MIC1 | OUT_NS | OUT_VOICE | OUT_VOICE_B;

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
    // The distance between MIC1 and MIC2, for the beamformer's geometry: 60 mm on this board (measured).
    float mic_spacing_mm = 60.0f;
    // The voice chain's input (OUT_VOICE), and the second chain's (OUT_VOICE_B).
    VoiceMic voice_mic = VOICE_AVG;
    VoiceMic voice_mic_b = VOICE_AVG;
    // Makeup gain after NS, dB. With the AGC off, the fixed gain; with it on, where the gain starts and
    // what it settles to when there is no speech (the residual noise floor is never raised past this).
    float makeup_db = 12.0f;
    // AGC: moves the gain, between 0 dB and agc_max_gain_db, to bring the speech's level (10 ms RMS,
    // following the louder frames) to agc_target_dbfs. Only frames 10 dB above the tracked noise floor
    // (and above -60 dBFS) count as speech.
    bool agc = true;
    float agc_target_dbfs = -20.0f;
    float agc_max_gain_db = 24.0f;
    // The expander after the makeup gain: how far frames near the noise floor (between words) are turned
    // down, dB; 0 is off.
    float gate_depth_db = 12.0f;
    // The EQ on NS's output, before the AGC: a presence peak and a low-mid cut (peaking filters, Q 1;
    // 0 dB is off).
    float eq_presence_db = 5.0f;
    float eq_presence_hz = 2800.0f;
    float eq_lowmid_db = -3.0f;
    float eq_lowmid_hz = 400.0f;
    // The compressor after the AGC and the expander: ratio (1 is off), threshold on its peak envelope,
    // and the gain after it.
    float comp_ratio = 3.0f;
    float comp_threshold_dbfs = -28.0f;
    float comp_makeup_db = 5.0f;
  };
  // Options for the voice chain as the recorder uses it (see MODE_VOICE).
  static Options voice_options();

  // How many channels (samples per frame) a recording with these outputs holds.
  static int channels(uint8_t outputs);

  /**
   * Record `frames` frames (1/SAMPLE_RATE s each) into `out`: frames x channels(options.outputs) samples,
   * interleaved (it may be in PSRAM). Blocks the calling task until done. One recording at a time:
   * ESP_ERR_INVALID_STATE while another is under way; ESP_ERR_INVALID_ARG for no outputs or a bad option.
   * Each recording logs the inputs' peak and RMS, each output's (the voice chains' makeup gain, min/avg/max,
   * and how much the limiter did), the CPU time of each stage and the heap taken.
   */
  static esp_err_t record(int16_t *out, size_t frames, const Options &options);
  // The three inputs, interleaved MIC1, MIC2, REF (MODE_RAW).
  static esp_err_t record(int16_t *out, size_t frames, int gain_db = DEFAULT_GAIN_DB);

  /**
   * The streaming sink: called on the capture task with `frames` frames of the processed audio, 16 kHz mono
   * 16-bit, at most SINK_FRAMES (320, 20 ms) at a time (each 40 ms read's output is handed over as two
   * halves). Must not block or take a lock that a blocking task holds. `mono` is only valid during the call.
   */
  using Sink = void (*)(const int16_t *mono, size_t frames, void *context);
  static constexpr size_t SINK_FRAMES = 320;

  /**
   * Stream processed audio to `sink` until stop(). Shares record()'s busy flag, so the two exclude each
   * other. Blocks until the codec is open (or failed), then returns; the first 100 ms are dropped before the
   * first sink call. `options.outputs` must name exactly one output (the sink gets that one, mono).
   * @return ESP_OK; ESP_ERR_INVALID_ARG (no sink, bad options, not a single output); ESP_ERR_INVALID_STATE
   *         (record() or another stream is running); ESP_ERR_NO_MEM; or the codec's failure.
   */
  static esp_err_t start(Sink sink, void *context, const Options &options);

  /**
   * End the stream. Returns once the capture task has returned from its last sink call and closed the codec
   * (about 40 ms, at most one chunk plus one I2S read timeout). The sink is never called again.
   * @return ESP_OK; ESP_ERR_INVALID_STATE (no stream); ESP_FAIL if an I2S read failed during the stream (it
   *         had ended itself; stop() still releases it).
   */
  static esp_err_t stop();

private:
  struct Request {
    int16_t *out;
    size_t frames;
    Options options;
    esp_err_t result;
    // Streaming (start()): the sink, and the flags between the caller and the capture task.
    Sink sink = nullptr;
    void *context = nullptr;
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> opened{false};
  };
  static esp_err_t _validate(const Options &options);
  static esp_err_t _ensure_task();
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
  static inline SemaphoreHandle_t _started = NULL; // Given once per stream: the codec is open, or it will not be
  static Request &_stream_request(); // The stream's request (in the .cpp: Options is not complete here)
  static inline std::atomic<bool> _stream_active = false;
};
#endif
