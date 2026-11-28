#if defined(BOARD_CUSTOM)
#include "VoiceCapture.hpp"

#include <BoardI2c.hpp>
#include <ConfigManager.hpp>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <driver/i2s_tdm.h>
#include <es7210_adc.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_gsc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ns.h>
#include <esp_timer.h>
#include <freertos/task.h>

static const char *TAG = "VoiceCapture";

// Waveshare's schematic and board definition (waveshare/waveshare_boards 0.3.7, esp32_p4_wifi6_touch_lcd_4b).
static constexpr int I2S_PORT = 0;
static constexpr gpio_num_t I2S_MCLK_GPIO = GPIO_NUM_13;
static constexpr gpio_num_t I2S_BCLK_GPIO = GPIO_NUM_12;
static constexpr gpio_num_t I2S_WS_GPIO = GPIO_NUM_10;
static constexpr gpio_num_t I2S_DIN_GPIO = GPIO_NUM_11; // ES7210 SDOUT
static constexpr uint8_t ES7210_ADDRESS = 0x40;         // 7-bit; esp_codec_dev takes it shifted (0x80)
static constexpr uint8_t ES8311_ADDRESS = 0x18;
static constexpr int I2C_HZ = 100000;
static constexpr int MCLK_MULTIPLE = 256; // 4.096 MHz at 16 kHz
static constexpr int SLOTS = 4;
// Where each TDM slot goes in a recording: MIC1, MIC3 (the reference), MIC2, MIC4 (not connected, dropped).
static constexpr int SLOT_CHANNEL[SLOTS] = {VoiceCapture::MIC1, VoiceCapture::REF, VoiceCapture::MIC2, -1};
static constexpr const char *SLOT_NAME[SLOTS] = {"MIC1", "MIC3 (ref)", "MIC2", "MIC4 (NC)"};

// esp-sr's frame sizes: the GSC beamformer takes 128 samples a channel per call, WebRTC NS (ns_pro) 160
// (10 ms). The reads are 640 frames (40 ms), a whole number of both, so each stage runs on whole frames
// straight from the chunk: no FIFO between the stages, and no delay but esp-sr's own.
static constexpr size_t GSC_FRAMES = 128;
static constexpr size_t NS_FRAMES = 160;
static constexpr size_t CHUNK_FRAMES = 640;
static_assert(CHUNK_FRAMES % GSC_FRAMES == 0 && CHUNK_FRAMES % NS_FRAMES == 0, "a chunk must be whole esp-sr frames");
// 20 ms (320 frames) is not a whole number of GSC frames, so the streaming start()/stop() of phase 2
// (a sink fed 320-frame mono chunks) keeps these 40 ms chunks and hands each one's output to its sink
// as two halves; the Pipeline below already keeps every stage's state from one chunk to the next.
// The DMA in 20 ms buffers (2,560 bytes each: a DMA buffer is at most 4,092), 8 of them (160 ms): how long
// the capture task may be held up, a 40 ms chunk's processing included.
static constexpr size_t DMA_FRAMES = VoiceCapture::SAMPLE_RATE / 50;
static constexpr uint32_t DMA_DESCRIPTORS = 8;
// Dropped after the ES7210 is enabled: its start (and its high-pass filter settling) clicks.
static constexpr size_t SETTLE_FRAMES = VoiceCapture::SAMPLE_RATE / 10;

// esp_codec_dev's handles, after _init_codec(). Only the capture task uses them.
static i2s_chan_handle_t rx_channel = nullptr;
static const audio_codec_data_if_t *data_if = nullptr;
static const audio_codec_ctrl_if_t *ctrl_if = nullptr;
static const audio_codec_if_t *codec_if = nullptr;
static esp_codec_dev_handle_t codec_dev = nullptr;

// Each Output's name, for the log, lowest bit first.
static constexpr const char *OUTPUT_NAME[] = {"MIC1", "MIC2", "REF", "NS(MIC1)", "BF", "NS(BF)", "VOICE", "VOICE-B"};
static constexpr int OUTPUTS = sizeof(OUTPUT_NAME) / sizeof(OUTPUT_NAME[0]);
static constexpr uint8_t ALL_OUTPUTS = (1 << OUTPUTS) - 1;

VoiceCapture::Options VoiceCapture::voice_options() {
  Options options;
  options.outputs = MODE_VOICE;
  options.gain_db = 36;
  options.ns_level = 1;
  // 200 Hz: round 2's speech had over half its energy at 150-300 Hz (the voice's fundamental, about
  // 180 Hz), and boomed through the 100 Hz filter; a broadcast voice loses little below 200.
  options.hpf_hz = 200;
  return options;
}

int VoiceCapture::channels(uint8_t outputs) {
  return __builtin_popcount(outputs & ALL_OUTPUTS);
}

esp_err_t VoiceCapture::record(int16_t *out, size_t frames, int gain_db) {
  Options options;
  options.gain_db = gain_db;
  return VoiceCapture::record(out, frames, options);
}

VoiceCapture::Request &VoiceCapture::_stream_request() {
  static Request request = {nullptr, 0, Options(), ESP_FAIL};
  return request;
}

esp_err_t VoiceCapture::_validate(const Options &options) {
  if ((options.outputs & ALL_OUTPUTS) == 0 || (options.outputs & ~ALL_OUTPUTS) != 0 || options.ns_level < 0 || options.ns_level > 2 ||
      (options.hpf_hz != 0 && (options.hpf_hz < 20 || options.hpf_hz > 2000)) || options.voice_mic > VOICE_AVG ||
      options.voice_mic_b > VOICE_AVG || options.makeup_db < -20.0f || options.makeup_db > 40.0f || options.agc_target_dbfs < -40.0f ||
      options.agc_target_dbfs > -3.0f || options.agc_max_gain_db < 0.0f || options.agc_max_gain_db > 40.0f ||
      options.gate_depth_db < 0.0f || options.gate_depth_db > 40.0f ||
      options.bf_angle_deg < 0.0f || options.bf_angle_deg > 360.0f || options.mic_spacing_mm < 5.0f || options.mic_spacing_mm > 500.0f) {
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

// The capture task starts on the first use, and stays (the codec is stopped between recordings).
esp_err_t VoiceCapture::_ensure_task() {
  if (VoiceCapture::_requests != NULL) {
    return ESP_OK;
  }
  esp_log_level_set(TAG, ConfigManager::log_level);
  VoiceCapture::_requests = xQueueCreate(1, sizeof(Request *));
  VoiceCapture::_done = xSemaphoreCreateBinary();
  VoiceCapture::_started = xSemaphoreCreateBinary();
  // Above the web server (5) and the UI, so that its reads keep up with the DMA. 4 KB: the 8 KB it had
  // used under 2 KB with GSC (the high-water mark is logged after each recording).
  if (VoiceCapture::_requests == NULL || VoiceCapture::_done == NULL || VoiceCapture::_started == NULL ||
      xTaskCreatePinnedToCore(VoiceCapture::_task, "voice_capture", 4096, NULL, 6, NULL, tskNO_AFFINITY) != pdPASS) {
    ESP_LOGE(TAG, "No memory for the capture task.");
    if (VoiceCapture::_requests != NULL) {
      vQueueDelete(VoiceCapture::_requests);
      VoiceCapture::_requests = NULL;
    }
    if (VoiceCapture::_done != NULL) {
      vSemaphoreDelete(VoiceCapture::_done);
      VoiceCapture::_done = NULL;
    }
    if (VoiceCapture::_started != NULL) {
      vSemaphoreDelete(VoiceCapture::_started);
      VoiceCapture::_started = NULL;
    }
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t VoiceCapture::record(int16_t *out, size_t frames, const Options &options) {
  if (out == nullptr || frames == 0 || VoiceCapture::_validate(options) != ESP_OK) {
    return ESP_ERR_INVALID_ARG;
  }
  if (VoiceCapture::_busy.exchange(true)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (VoiceCapture::_ensure_task() != ESP_OK) {
    VoiceCapture::_busy = false;
    return ESP_ERR_NO_MEM;
  }
  Request request = {out, frames, options, ESP_FAIL};
  Request *pending = &request;
  xQueueSend(VoiceCapture::_requests, &pending, portMAX_DELAY);
  // Bounded: every I2S read times out after a second, and a failed one ends the recording.
  xSemaphoreTake(VoiceCapture::_done, portMAX_DELAY);
  VoiceCapture::_busy = false;
  return request.result;
}

esp_err_t VoiceCapture::start(Sink sink, void *context, const Options &options) {
  if (sink == nullptr || VoiceCapture::_validate(options) != ESP_OK || VoiceCapture::channels(options.outputs) != 1) {
    return ESP_ERR_INVALID_ARG;
  }
  if (VoiceCapture::_busy.exchange(true)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (VoiceCapture::_ensure_task() != ESP_OK) {
    VoiceCapture::_busy = false;
    return ESP_ERR_NO_MEM;
  }
  Request &request = VoiceCapture::_stream_request();
  request.out = nullptr;
  request.frames = 0;
  request.options = options;
  request.result = ESP_FAIL;
  request.sink = sink;
  request.context = context;
  request.stop_flag = false;
  request.opened = false;
  Request *pending = &request;
  xQueueSend(VoiceCapture::_requests, &pending, portMAX_DELAY);
  // Given when the codec is open, or by _task() if it is not going to be.
  xSemaphoreTake(VoiceCapture::_started, portMAX_DELAY);
  if (!request.opened) {
    xSemaphoreTake(VoiceCapture::_done, portMAX_DELAY);
    VoiceCapture::_busy = false;
    return request.result != ESP_OK ? request.result : ESP_FAIL;
  }
  VoiceCapture::_stream_active = true;
  return ESP_OK;
}

esp_err_t VoiceCapture::stop() {
  if (!VoiceCapture::_stream_active.exchange(false)) {
    return ESP_ERR_INVALID_STATE;
  }
  VoiceCapture::_stream_request().stop_flag = true;
  xSemaphoreTake(VoiceCapture::_done, portMAX_DELAY);
  VoiceCapture::_busy = false;
  return VoiceCapture::_stream_request().result;
}

void VoiceCapture::_task(void *param) {
  for (;;) {
    Request *request = nullptr;
    if (xQueueReceive(VoiceCapture::_requests, &request, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    request->result = codec_dev != nullptr ? ESP_OK : VoiceCapture::_init_codec();
    if (request->result == ESP_OK) {
      request->result = VoiceCapture::_capture(request);
    }
    if (request->sink != nullptr && !request->opened) {
      xSemaphoreGive(VoiceCapture::_started); // The stream never opened: start() is waiting
    }
    xSemaphoreGive(VoiceCapture::_done);
  }
}

// A register of the chip at `address` on the board's bus, or -1. Read without esp_codec_dev, to log the IDs.
static int read_register(i2c_master_bus_handle_t bus, uint8_t address, uint8_t reg) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = I2C_HZ;
  i2c_master_dev_handle_t device = nullptr;
  if (i2c_master_bus_add_device(bus, &config, &device) != ESP_OK) {
    return -1;
  }
  uint8_t value = 0;
  esp_err_t err = i2c_master_transmit_receive(device, &reg, 1, &value, 1, 50);
  i2c_master_bus_rm_device(device);
  return err == ESP_OK ? value : -1;
}

esp_err_t VoiceCapture::_init_codec() {
  i2c_master_bus_handle_t bus = BoardI2c::bus();
  if (bus == nullptr) {
    return ESP_FAIL;
  }
  // The IDs: the ES7210's 0x3D-0x3F (chip ID 0x72 0x10 by its datasheet, then the version), the ES8311's
  // 0xFD-0xFE (0x83 0x11). The ES8311 is only looked at: it plays, and nothing plays yet.
  if (i2c_master_probe(bus, ES7210_ADDRESS, 50) != ESP_OK) {
    ESP_LOGE(TAG, "No ES7210 on I2C 0x%02X.", ES7210_ADDRESS);
    return ESP_ERR_NOT_FOUND;
  }
  ESP_LOGI(TAG, "ES7210 on 0x%02X: chip ID %02X %02X, version %02X.", ES7210_ADDRESS, read_register(bus, ES7210_ADDRESS, 0x3D) & 0xFF,
           read_register(bus, ES7210_ADDRESS, 0x3E) & 0xFF, read_register(bus, ES7210_ADDRESS, 0x3F) & 0xFF);
  if (i2c_master_probe(bus, ES8311_ADDRESS, 50) == ESP_OK) {
    ESP_LOGI(TAG, "ES8311 on 0x%02X: chip ID %02X %02X (playback, not used).", ES8311_ADDRESS, read_register(bus, ES8311_ADDRESS, 0xFD) & 0xFF,
             read_register(bus, ES8311_ADDRESS, 0xFE) & 0xFF);
  } else {
    ESP_LOGW(TAG, "No ES8311 on I2C 0x%02X.", ES8311_ADDRESS);
  }

  // RX only, the P4 the master: it drives MCLK, BCLK and WS. esp_codec_dev sets the slots and clock again
  // on every esp_codec_dev_open(), from the stream's format.
  i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
  channel_config.dma_desc_num = DMA_DESCRIPTORS;
  channel_config.dma_frame_num = DMA_FRAMES;
  esp_err_t err = i2s_new_channel(&channel_config, NULL, &rx_channel);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S %d RX channel: %s", (int)I2S_PORT, esp_err_to_name(err));
    rx_channel = nullptr;
    return err;
  }
  i2s_tdm_config_t tdm_config = {};
  tdm_config.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  tdm_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  tdm_config.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                            static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
  tdm_config.gpio_cfg.mclk = I2S_MCLK_GPIO;
  tdm_config.gpio_cfg.bclk = I2S_BCLK_GPIO;
  tdm_config.gpio_cfg.ws = I2S_WS_GPIO;
  tdm_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  tdm_config.gpio_cfg.din = I2S_DIN_GPIO;
  err = i2s_channel_init_tdm_mode(rx_channel, &tdm_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S TDM mode: %s", esp_err_to_name(err));
    VoiceCapture::_deinit_codec();
    return err;
  }

  audio_codec_i2s_cfg_t i2s_config = {};
  i2s_config.port = I2S_PORT;
  i2s_config.rx_handle = rx_channel;
  data_if = audio_codec_new_i2s_data(&i2s_config);
  audio_codec_i2c_cfg_t i2c_config = {};
  i2c_config.port = I2C_NUM_1;
  i2c_config.addr = ES7210_CODEC_DEFAULT_ADDR; // 0x40 << 1
  i2c_config.bus_handle = bus;
  i2c_config.clock_speed_hz = I2C_HZ;
  ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
  if (data_if == nullptr || ctrl_if == nullptr) {
    ESP_LOGE(TAG, "esp_codec_dev's I2S or I2C interface did not start.");
    VoiceCapture::_deinit_codec();
    return ESP_FAIL;
  }
  // The P4 drives the clocks. Waveshare's inputs: the two microphones and the reference; MIC4 stays off.
  // Three inputs or more put the ES7210 in TDM mode.
  es7210_codec_cfg_t es7210_config = {};
  es7210_config.ctrl_if = ctrl_if;
  es7210_config.master_mode = false;
  es7210_config.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
  es7210_config.mclk_src = ES7210_MCLK_FROM_PAD;
  es7210_config.mclk_div = MCLK_MULTIPLE;
  codec_if = es7210_codec_new(&es7210_config);
  if (codec_if == nullptr) {
    ESP_LOGE(TAG, "ES7210 did not start (see the ES7210 lines above).");
    VoiceCapture::_deinit_codec();
    return ESP_FAIL;
  }
  esp_codec_dev_cfg_t dev_config = {};
  dev_config.dev_type = ESP_CODEC_DEV_TYPE_IN;
  dev_config.codec_if = codec_if;
  dev_config.data_if = data_if;
  codec_dev = esp_codec_dev_new(&dev_config);
  if (codec_dev == nullptr) {
    ESP_LOGE(TAG, "No memory for the codec device.");
    VoiceCapture::_deinit_codec();
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "ES7210 ready: I2S %d TDM RX, %lu Hz, %d slots of 16 bits, MCLK %lu Hz (esp_codec_dev %s).", (int)I2S_PORT,
           (unsigned long)SAMPLE_RATE, SLOTS, (unsigned long)(SAMPLE_RATE * MCLK_MULTIPLE), esp_codec_dev_get_version());
  return ESP_OK;
}

void VoiceCapture::_deinit_codec() {
  if (codec_dev != nullptr) {
    esp_codec_dev_delete(codec_dev);
    codec_dev = nullptr;
  }
  if (codec_if != nullptr) {
    audio_codec_delete_codec_if(codec_if);
    codec_if = nullptr;
  }
  if (ctrl_if != nullptr) {
    audio_codec_delete_ctrl_if(ctrl_if);
    ctrl_if = nullptr;
  }
  if (data_if != nullptr) {
    audio_codec_delete_data_if(data_if);
    data_if = nullptr;
  }
  if (rx_channel != nullptr) {
    i2s_del_channel(rx_channel);
    rx_channel = nullptr;
  }
}

namespace {

// A 2nd-order Butterworth high-pass (RBJ's cookbook, Q = 1/sqrt(2)), transposed direct form II.
struct HighPass {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void design(float hz) {
    const float w = 2.0f * (float)M_PI * hz / VoiceCapture::SAMPLE_RATE, alpha = sinf(w) / (2.0f * (float)M_SQRT1_2), c = cosf(w);
    const float a0 = 1.0f + alpha;
    b0 = (1.0f + c) / 2.0f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
    z1 = z2 = 0;
  }
  void run(int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
      const float x = samples[i], y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      samples[i] = y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : (int16_t)lroundf(y);
    }
  }
};

struct Level {
  uint64_t squares = 0;
  int peak = 0;
  void add(int16_t sample) {
    squares += (int64_t)sample * sample;
    peak = abs(sample) > peak ? abs(sample) : peak;
  }
  void log(const char *name, size_t frames) const {
    const float rms = sqrtf((float)squares / frames);
    const int dbfs = rms >= 1.0f ? (int)lroundf(20.0f * log10f(rms / 32768.0f)) : -91;
    const int peak_dbfs = peak > 0 ? (int)lroundf(20.0f * log10f(peak / 32768.0f)) : -91;
    ESP_LOGI(TAG, "  out %-8s peak %5d (%d dBFS), RMS %5d (%d dBFS)", name, peak, peak_dbfs, (int)lroundf(rms), dbfs);
  }
};

// The voice chain's last two stages, per 10 ms frame (one NS frame), state carried from frame to frame:
// makeup gain, then a peak limiter.
//
// Makeup is an AGC, or a fixed gain with the AGC off. The AGC measures each frame's RMS after NS, and
// follows the noise floor NS leaves (a minimum tracker: down within a few frames, up over about 5 s).
// Frames more than SPEECH_DB above that floor (and above GATE_DB) are speech: a level envelope follows
// them (up fast, down over about a second) and the gain heads for target - envelope, clamped to 0..max,
// falling quickly (loud talker: about 30 ms) and rising slowly (about half a second), so it does not pump
// between words. Other frames never raise the gain; they let it drift to the idle gain (makeup_db), so a
// long pause cannot leave +24 dB waiting for the next breath of noise.
//
// The floor test matters in a noisy room: NS left one at about -62 dBFS, above a fixed -60 gate, so the
// room counted as speech and was raised by the full 17 dB the voice was (round 2 of the listening tests).
//
// Then a downward expander (Options::gate_depth_db): frames within OPEN_DB of the floor are turned down by
// up to the depth, opening within a frame for the next word and closing over about 150 ms, so the room
// between words sits where NS left it instead of being raised with the voice. Both gains are ramped
// across the frame, sample by sample.
//
// The limiter has a ceiling of -3 dBFS: per sample the gain drops at once to what keeps the sample at the
// ceiling, and recovers over 80 ms, so nothing passes the ceiling (no wraparound, no hard clipping).
struct Makeup {
  static constexpr float GATE_DB = -60.0f;   // Never speech below this, however quiet the room
  static constexpr float SPEECH_DB = 10.0f;  // Speech: this far above the floor
  static constexpr float OPEN_DB = 6.0f;     // The expander opens this far above the floor
  static constexpr float CEILING = 32768.0f * 0.70794578f; // -3 dBFS
  static constexpr float RELEASE = 0.00078f;               // 1 - exp(-1 / (0.08 s * 16000 Hz))
  bool agc = true;
  float idle_db = 12.0f, max_db = 24.0f, target_db = -20.0f;
  float gain_db = 12.0f, envelope_db = -90.0f, limiter = 1.0f;
  float floor_db = 0.0f;  // The noise floor, after NS (set from the first frame)
  bool floor_set = false;
  float gate_depth = 1.0f, gate = 1.0f; // The expander: its deepest gain, and its gain now (linear)
  // What it did, over the frames that reached the recording.
  float gain_min = 1e9f, gain_max = -1e9f, gain_sum = 0.0f;
  uint32_t frames = 0, limited_frames = 0, gated_frames = 0;
  uint64_t limited_samples = 0;

  void design(const VoiceCapture::Options &options) {
    agc = options.agc;
    max_db = options.agc_max_gain_db;
    idle_db = agc && options.makeup_db > max_db ? max_db : options.makeup_db;
    target_db = options.agc_target_dbfs;
    gain_db = idle_db;
    gate_depth = powf(10.0f, -options.gate_depth_db / 20.0f);
  }

  // NS_FRAMES samples. `count` is false for the padding after a recording's last whole frame.
  void run(const int16_t *in, int16_t *out, bool count) {
    float squares = 0.0f;
    for (size_t i = 0; i < NS_FRAMES; i++) {
      squares += (float)in[i] * in[i];
    }
    const float level_db = 10.0f * log10f(squares / NS_FRAMES / 1073741824.0f + 1e-12f);
    if (!floor_set || level_db < floor_db) {
      floor_db = floor_set ? floor_db + 0.5f * (level_db - floor_db) : level_db;
      floor_set = true;
    } else {
      floor_db += 0.002f * (level_db - floor_db);
    }
    const bool speech = level_db > GATE_DB && level_db > floor_db + SPEECH_DB;
    float next_db = gain_db;
    if (!agc) {
      next_db = idle_db;
    } else if (speech) {
      envelope_db += (level_db > envelope_db ? 0.5f : 0.01f) * (level_db - envelope_db);
      float want_db = target_db - envelope_db;
      want_db = want_db < 0.0f ? 0.0f : want_db > max_db ? max_db : want_db;
      next_db += (want_db < gain_db ? 0.3f : 0.02f) * (want_db - gain_db);
    } else {
      next_db += 0.02f * (idle_db - gain_db);
    }
    const bool open = level_db > floor_db + OPEN_DB;
    const float next_gate = gate + (open ? 0.7f : 0.065f) * ((open ? 1.0f : gate_depth) - gate);
    const float from = powf(10.0f, gain_db / 20.0f) * gate, to = powf(10.0f, next_db / 20.0f) * next_gate;
    const float step = (to - from) / NS_FRAMES;
    uint32_t limited = 0;
    float gain = from;
    for (size_t i = 0; i < NS_FRAMES; i++) {
      gain += step;
      const float v = in[i] * gain, magnitude = fabsf(v);
      const float need = magnitude > CEILING ? CEILING / magnitude : 1.0f;
      if (need < limiter) {
        limiter = need;
      }
      if (limiter < 0.999f) {
        limited++;
      }
      const float y = v * limiter;
      limiter += (1.0f - limiter) * RELEASE;
      out[i] = y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : (int16_t)lroundf(y);
    }
    gain_db = next_db;
    gate = next_gate;
    if (count) {
      gated_frames += !open;
      frames++;
      gain_min = next_db < gain_min ? next_db : gain_min;
      gain_max = next_db > gain_max ? next_db : gain_max;
      gain_sum += next_db;
      limited_samples += limited;
      limited_frames += limited > 0;
    }
  }
};

// One voice chain: its input, its NS (its own, or MIC1's from OUT_NS, which is the same audio through
// the same filter), and the makeup stage. `out` is the chain's output chunk.
struct VoiceChain {
  bool on = false, owns_ns = false;
  VoiceCapture::VoiceMic mic = VoiceCapture::VOICE_AVG;
  ns_handle_t ns = nullptr;
  int16_t *ns_out = nullptr, *out = nullptr;
  Makeup makeup;
  int64_t us_ns = 0, us_makeup = 0;
};

struct HeapMark {
  size_t internal, psram;
  static HeapMark now() { return {heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM)}; }
};

} // namespace

/**
 * One recording's processing: the stages its outputs need, their state, and what they cost.
 *
 * esp-sr's handles are made at the start of each recording that needs them and freed at its end, not
 * kept. GSC and one WebRTC NS took 64.8 KB of internal RAM in the 1a spike (and 42 KB of PSRAM, the GSC
 * core's buffers): too much to hold through the hours between recordings on a board whose internal RAM
 * also carries Wi-Fi, the display and LVGL, when making them takes about a millisecond (logged as
 * "setup"). Made per recording, each one also gets that recording's NS level, and MODE_COMPARE its
 * second NS. Where esp-sr puts its state is its own choice (GSC's core in PSRAM on the P4, the rest
 * internal); the chunk buffers here are internal, and freed with the rest.
 */
struct VoiceCapture::Pipeline {
  uint8_t outputs = 0;
  int channels = 0;
  bool hpf_on = false;
  float bf_angle_deg = 90.0f;
  HighPass hpf[CHANNELS];
  gsc_handle_t *gsc = nullptr;
  ns_handle_t ns_mic = nullptr; // NS on MIC1 (OUT_NS)
  ns_handle_t ns_bf = nullptr;  // NS on the beamformer's output (OUT_BF_NS)
  // Internal RAM, CHUNK_FRAMES each: the beamformed chunk, NS(MIC1), NS(BF); then one GSC input frame (planar).
  int16_t *buffers = nullptr;
  int16_t *bf = nullptr, *ns_mic_out = nullptr, *ns_bf_out = nullptr, *gsc_in = nullptr;
  int64_t us_hpf = 0, us_bf = 0, us_ns_mic = 0, us_ns_bf = 0;
  Level level[OUTPUTS];
  VoiceChain chain[2];    // OUT_VOICE, OUT_VOICE_B
  int16_t *avg = nullptr; // (MIC1 + MIC2) / 2, for a chain on VOICE_AVG
  int64_t us_avg = 0;

  bool needs_bf() const { return outputs & (OUT_BF | OUT_BF_NS); }
  bool needs_sr() const { return outputs & (OUT_NS | OUT_BF | OUT_BF_NS | OUT_VOICE | OUT_VOICE_B); }

  esp_err_t start(const Options &options) {
    outputs = options.outputs;
    channels = VoiceCapture::channels(outputs);
    bf_angle_deg = options.bf_angle_deg;
    hpf_on = options.hpf_hz > 0;
    for (int channel = 0; channel < CHANNELS && hpf_on; channel++) {
      hpf[channel].design((float)options.hpf_hz);
    }
    if (!needs_sr()) {
      return ESP_OK;
    }
    for (int i = 0; i < 2; i++) {
      chain[i].on = outputs & (i == 0 ? OUT_VOICE : OUT_VOICE_B);
      chain[i].mic = i == 0 ? options.voice_mic : options.voice_mic_b;
      chain[i].makeup.design(options);
      // MIC1's NS is the same filter on the same audio as OUT_NS's: shared, if that is in the recording.
      chain[i].owns_ns = chain[i].on && !(chain[i].mic == VoiceCapture::VOICE_MIC1 && (outputs & OUT_NS));
    }
    // One block of internal RAM for what is needed, CHUNK_FRAMES a buffer (the GSC input is two frames).
    struct Want {
      int16_t **pointer;
      size_t samples;
    } wants[8];
    int count = 0;
    bool avg_used = false;
    for (const VoiceChain &c : chain) {
      avg_used |= c.on && c.mic == VoiceCapture::VOICE_AVG;
    }
    if (needs_bf()) {
      wants[count++] = {&bf, CHUNK_FRAMES};
      wants[count++] = {&gsc_in, GSC_FRAMES * 2};
    }
    if (outputs & OUT_NS) {
      wants[count++] = {&ns_mic_out, CHUNK_FRAMES};
    }
    if (outputs & OUT_BF_NS) {
      wants[count++] = {&ns_bf_out, CHUNK_FRAMES};
    }
    if (avg_used) {
      wants[count++] = {&avg, CHUNK_FRAMES};
    }
    for (VoiceChain &c : chain) {
      if (c.on) {
        wants[count++] = {&c.out, CHUNK_FRAMES};
        if (c.owns_ns) {
          wants[count++] = {&c.ns_out, CHUNK_FRAMES};
        }
      }
    }
    size_t total = 0;
    for (int i = 0; i < count; i++) {
      total += wants[i].samples;
    }
    buffers = static_cast<int16_t *>(heap_caps_calloc(total, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffers == nullptr) {
      return ESP_ERR_NO_MEM;
    }
    size_t at = 0;
    for (int i = 0; i < count; i++) {
      *wants[i].pointer = buffers + at;
      at += wants[i].samples;
    }
    for (VoiceChain &c : chain) {
      if (c.on && !c.owns_ns) {
        c.ns_out = ns_mic_out;
      }
    }
    if (needs_bf()) {
      // MIC1 front left, MIC2 front right: along x, the talker out along +y (esp-sr's "near to far").
      const float half = options.mic_spacing_mm / 2000.0f;
      PlaneCoord mics[2] = {{-half, 0.0f, 0.0f}, {half, 0.0f, 0.0f}};
      gsc = esp_gsc_create(mics, 2);
      if (gsc == nullptr) {
        ESP_LOGE(TAG, "GSC beamformer: no memory.");
        return ESP_ERR_NO_MEM;
      }
    }
    // ns_pro takes 10 ms frames only.
    if (outputs & OUT_NS) {
      ns_mic = ns_pro_create(NS_FRAMES * 1000 / SAMPLE_RATE, options.ns_level, SAMPLE_RATE);
    }
    if (outputs & OUT_BF_NS) {
      ns_bf = ns_pro_create(NS_FRAMES * 1000 / SAMPLE_RATE, options.ns_level, SAMPLE_RATE);
    }
    for (VoiceChain &c : chain) {
      if (c.owns_ns) {
        c.ns = ns_pro_create(NS_FRAMES * 1000 / SAMPLE_RATE, options.ns_level, SAMPLE_RATE);
        if (c.ns == nullptr) {
          ESP_LOGE(TAG, "WebRTC NS: no memory.");
          return ESP_ERR_NO_MEM;
        }
      }
    }
    if (((outputs & OUT_NS) && ns_mic == nullptr) || ((outputs & OUT_BF_NS) && ns_bf == nullptr)) {
      ESP_LOGE(TAG, "WebRTC NS: no memory.");
      return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
  }

  void stop() {
    if (gsc != nullptr) {
      esp_gsc_destroy(gsc);
      gsc = nullptr;
    }
    if (ns_mic != nullptr) {
      ns_destroy(ns_mic);
      ns_mic = nullptr;
    }
    if (ns_bf != nullptr) {
      ns_destroy(ns_bf);
      ns_bf = nullptr;
    }
    for (VoiceChain &c : chain) {
      if (c.ns != nullptr) {
        ns_destroy(c.ns);
        c.ns = nullptr;
      }
    }
    heap_caps_free(buffers);
    buffers = nullptr;
  }
};

esp_err_t VoiceCapture::_capture(Request *request) {
  const Options &options = request->options;
  // The heap before anything of this recording's, for what its buffers and esp-sr take.
  const HeapMark before = HeapMark::now();
  const bool stream = request->sink != nullptr;
  const int64_t setup_start = esp_timer_get_time();
  Pipeline pipeline;
  esp_err_t result = pipeline.start(options);
  const int64_t setup_us = esp_timer_get_time() - setup_start;
  // A chunk as read (TDM, SLOTS interleaved), then one array per channel.
  int16_t *tdm = result != ESP_OK ? nullptr
                                  : static_cast<int16_t *>(heap_caps_malloc(CHUNK_FRAMES * (SLOTS + CHANNELS + 1) * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (tdm == nullptr) {
    pipeline.stop();
    return result != ESP_OK ? result : ESP_ERR_NO_MEM;
  }
  const HeapMark ready = HeapMark::now();
  int16_t *mono = tdm + CHUNK_FRAMES * (SLOTS + CHANNELS); // A stream's processed chunk
  int16_t *inputs[CHANNELS];
  for (int channel = 0; channel < CHANNELS; channel++) {
    inputs[channel] = tdm + CHUNK_FRAMES * (SLOTS + channel);
  }
  esp_codec_dev_sample_info_t format = {};
  format.bits_per_sample = 16;
  format.channel = SLOTS;
  format.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3);
  format.sample_rate = SAMPLE_RATE;
  format.mclk_multiple = MCLK_MULTIPLE;
  int ret = esp_codec_dev_open(codec_dev, &format);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "Codec did not open: %d.", ret);
    heap_caps_free(tdm);
    pipeline.stop();
    return ESP_FAIL;
  }
  // After the open, which sets the gain it kept (0 dB until one is set).
  ret = esp_codec_dev_set_in_gain(codec_dev, (float)options.gain_db);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "Gain %d dB: %d.", options.gain_db, ret);
  }

  if (stream) {
    request->opened = true;
    xSemaphoreGive(VoiceCapture::_started);
  }
  int64_t sum[SLOTS] = {};
  uint64_t squares[SLOTS] = {};
  int peak[SLOTS] = {};
  size_t skip = SETTLE_FRAMES, done = 0;
  const int64_t start_us = esp_timer_get_time();
  // A recording ends at its frame count, a stream when stop() says (checked once per chunk).
  while (stream ? !request->stop_flag.load() : done < request->frames) {
    size_t wanted = skip > 0 ? skip : (stream ? CHUNK_FRAMES : request->frames - done);
    size_t frames = wanted < CHUNK_FRAMES ? wanted : CHUNK_FRAMES;
    ret = esp_codec_dev_read(codec_dev, tdm, frames * SLOTS * sizeof(int16_t));
    if (ret != ESP_CODEC_DEV_OK) {
      ESP_LOGE(TAG, "I2S read failed after %u frames: %d.", (unsigned)done, ret);
      result = ESP_FAIL;
      break;
    }
    if (skip > 0) {
      skip -= frames;
      continue;
    }
    for (size_t i = 0; i < frames * SLOTS; i++) {
      int slot = i % SLOTS, sample = tdm[i];
      sum[slot] += sample;
      squares[slot] += (int64_t)sample * sample;
      peak[slot] = abs(sample) > peak[slot] ? abs(sample) : peak[slot];
      if (SLOT_CHANNEL[slot] >= 0) {
        inputs[SLOT_CHANNEL[slot]][i / SLOTS] = sample;
      }
    }
    if (stream) {
      VoiceCapture::_process(pipeline, inputs, frames, mono);
      for (size_t at = 0; at < frames; at += SINK_FRAMES) {
        request->sink(mono + at, frames - at < SINK_FRAMES ? frames - at : SINK_FRAMES, request->context);
      }
    } else {
      VoiceCapture::_process(pipeline, inputs, frames, request->out + done * pipeline.channels);
    }
    done += frames;
  }
  const int64_t wall_us = esp_timer_get_time() - start_us;
  esp_codec_dev_close(codec_dev); // Powers the ES7210 down and stops I2S
  const UBaseType_t stack_left = uxTaskGetStackHighWaterMark(NULL);
  heap_caps_free(tdm);
  pipeline.stop();

  // Which slot is which shows here: with the speaker silent, the reference is near silence and MIC4's
  // slot (nothing connected, not enabled) is zero, while the microphones hear the room. The slots are
  // before the high-pass filter, the outputs after it.
  ESP_LOGI(TAG, "Recorded %u frames (%lu ms) at %d dB, outputs 0x%02x, NS level %d, HPF %d Hz, makeup %.0f dB, AGC %d (max %.0f dB), beam %.0f deg, spacing %.0f mm:",
           (unsigned)done, (unsigned long)(done * 1000 / SAMPLE_RATE), options.gain_db, options.outputs, options.ns_level, options.hpf_hz, options.makeup_db,
           (int)options.agc, options.agc_max_gain_db, options.bf_angle_deg, options.mic_spacing_mm);
  for (int slot = 0; slot < SLOTS && done > 0; slot++) {
    float rms = sqrtf((float)squares[slot] / done);
    int dbfs = rms >= 1.0f ? (int)lroundf(20.0f * log10f(rms / 32768.0f)) : -91;
    ESP_LOGI(TAG, "  slot %d %-10s peak %5d, RMS %5d (%d dBFS), mean %d", slot, SLOT_NAME[slot], peak[slot], (int)lroundf(rms), dbfs,
             (int)(sum[slot] / (int64_t)done));
  }
  static constexpr const char *MIC_NAME[] = {"MIC1", "MIC2", "avg"};
  for (int output = 0; output < OUTPUTS && done > 0; output++) {
    if (options.outputs & (1 << output)) {
      pipeline.level[output].log(OUTPUT_NAME[output], done);
    }
  }
  for (int i = 0; i < 2 && done > 0; i++) {
    const VoiceChain &c = pipeline.chain[i];
    if (c.on && c.makeup.frames > 0) {
      const Makeup &m = c.makeup;
      ESP_LOGI(TAG, "  %s (mic %s, %s, target %.0f dBFS): makeup gain min %.1f / avg %.1f / max %.1f dB; limiter on %u of %u frames (%llu samples, %.2f%%); "
                    "floor %.1f dBFS, gate closed on %u frames",
               OUTPUT_NAME[6 + i], MIC_NAME[c.mic], m.agc ? "AGC" : "fixed", m.target_db, m.gain_min, m.gain_sum / m.frames, m.gain_max,
               (unsigned)m.limited_frames, (unsigned)m.frames, (unsigned long long)m.limited_samples,
               m.limited_samples * 100.0f / ((float)m.frames * NS_FRAMES), m.floor_db, (unsigned)m.gated_frames);
    }
  }
  // CPU time per stage, against the audio's length: the share of one core it takes. The capture task is
  // not pinned, and this is wall time around each call, so preemption by a higher-priority task counts too.
  if (done > 0) {
    const float audio_us = done * 1000000.0f / SAMPLE_RATE;
    ESP_LOGI(TAG, "  CPU: setup %lld us; HPF %lld us (%.2f%%), GSC %lld us (%.2f%%), NS(MIC1) %lld us (%.2f%%), NS(BF) %lld us (%.2f%%); %lld ms wall for %lu ms of audio",
             (long long)setup_us, (long long)pipeline.us_hpf, pipeline.us_hpf * 100.0f / audio_us, (long long)pipeline.us_bf,
             pipeline.us_bf * 100.0f / audio_us, (long long)pipeline.us_ns_mic, pipeline.us_ns_mic * 100.0f / audio_us, (long long)pipeline.us_ns_bf,
             pipeline.us_ns_bf * 100.0f / audio_us, (long long)(wall_us / 1000), (unsigned long)(done * 1000 / SAMPLE_RATE));
    for (int i = 0; i < 2; i++) {
      const VoiceChain &c = pipeline.chain[i];
      if (c.on) {
        ESP_LOGI(TAG, "  CPU %s: NS %lld us (%.2f%%, %s), makeup+limiter %lld us (%.2f%%); mic average %lld us (%.2f%%, shared)", OUTPUT_NAME[6 + i],
                 (long long)c.us_ns, c.us_ns * 100.0f / audio_us, c.owns_ns ? "own" : "NS(MIC1)'s, counted there", (long long)c.us_makeup,
                 c.us_makeup * 100.0f / audio_us, (long long)pipeline.us_avg, pipeline.us_avg * 100.0f / audio_us);
      }
    }
  }
  // What this recording took (esp-sr's handles and the chunk buffers, not the caller's recording), and the
  // lowest the free heap has been since boot.
  ESP_LOGI(TAG, "  Heap: took %d bytes internal, %d PSRAM; free now %u internal, %u PSRAM; lowest since boot %u internal, %u PSRAM; stack %u bytes unused",
           (int)before.internal - (int)ready.internal, (int)before.psram - (int)ready.psram, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), (unsigned)(stack_left * sizeof(StackType_t)));
  return result;
}

void VoiceCapture::_process(Pipeline &pipeline, int16_t *const channels[CHANNELS], size_t frames, int16_t *out) {
  if (pipeline.hpf_on) {
    const int64_t t = esp_timer_get_time();
    for (int channel = 0; channel < CHANNELS; channel++) {
      pipeline.hpf[channel].run(channels[channel], frames);
    }
    pipeline.us_hpf += esp_timer_get_time() - t;
  }
  if (pipeline.needs_sr()) {
    // esp-sr runs on whole frames: the last chunk of a recording, if short, is made up with silence, and
    // what comes of the silence is not written.
    for (int channel = 0; channel < CHANNELS && frames < CHUNK_FRAMES; channel++) {
      memset(channels[channel] + frames, 0, (CHUNK_FRAMES - frames) * sizeof(int16_t));
    }
    if (pipeline.gsc != nullptr) {
      const int64_t t = esp_timer_get_time();
      for (size_t at = 0; at < CHUNK_FRAMES; at += GSC_FRAMES) {
        // Planar, as GSC takes it: MIC1's 128 samples, then MIC2's.
        memcpy(pipeline.gsc_in, channels[MIC1] + at, GSC_FRAMES * sizeof(int16_t));
        memcpy(pipeline.gsc_in + GSC_FRAMES, channels[MIC2] + at, GSC_FRAMES * sizeof(int16_t));
        esp_gsc_process(pipeline.gsc, pipeline.gsc_in, pipeline.bf_angle_deg, pipeline.bf + at);
      }
      pipeline.us_bf += esp_timer_get_time() - t;
    }
    if (pipeline.ns_mic != nullptr) {
      const int64_t t = esp_timer_get_time();
      for (size_t at = 0; at < CHUNK_FRAMES; at += NS_FRAMES) {
        ns_process(pipeline.ns_mic, channels[MIC1] + at, pipeline.ns_mic_out + at);
      }
      pipeline.us_ns_mic += esp_timer_get_time() - t;
    }
    if (pipeline.ns_bf != nullptr) {
      const int64_t t = esp_timer_get_time();
      for (size_t at = 0; at < CHUNK_FRAMES; at += NS_FRAMES) {
        ns_process(pipeline.ns_bf, pipeline.bf + at, pipeline.ns_bf_out + at);
      }
      pipeline.us_ns_bf += esp_timer_get_time() - t;
    }
  }
  if (pipeline.avg != nullptr) {
    const int64_t t = esp_timer_get_time();
    for (size_t i = 0; i < CHUNK_FRAMES; i++) {
      pipeline.avg[i] = (int16_t)(((int)channels[MIC1][i] + channels[MIC2][i]) >> 1);
    }
    pipeline.us_avg += esp_timer_get_time() - t;
  }
  for (VoiceChain &c : pipeline.chain) {
    if (!c.on) {
      continue;
    }
    int16_t *input = c.mic == VOICE_MIC1 ? channels[MIC1] : c.mic == VOICE_MIC2 ? channels[MIC2] : pipeline.avg;
    int64_t t = esp_timer_get_time();
    for (size_t at = 0; c.ns != nullptr && at < CHUNK_FRAMES; at += NS_FRAMES) {
      ns_process(c.ns, input + at, c.ns_out + at);
    }
    c.us_ns += c.ns != nullptr ? esp_timer_get_time() - t : 0;
    t = esp_timer_get_time();
    for (size_t at = 0; at < CHUNK_FRAMES; at += NS_FRAMES) {
      c.makeup.run(c.ns_out + at, c.out + at, at < frames);
    }
    c.us_makeup += esp_timer_get_time() - t;
  }
  // Interleaved, in Output order.
  const int16_t *sources[OUTPUTS] = {channels[MIC1], channels[MIC2], channels[REF], pipeline.ns_mic_out, pipeline.bf, pipeline.ns_bf_out,
                                     pipeline.chain[0].out, pipeline.chain[1].out};
  for (size_t frame = 0; frame < frames; frame++) {
    for (int output = 0; output < OUTPUTS; output++) {
      if (pipeline.outputs & (1 << output)) {
        const int16_t sample = sources[output][frame];
        pipeline.level[output].add(sample);
        *out++ = sample;
      }
    }
  }
}

#endif
