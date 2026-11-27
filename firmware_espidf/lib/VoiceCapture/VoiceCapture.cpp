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
#include <esp_heap_caps.h>
#include <esp_log.h>
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

// Read in 20 ms pieces. The DMA holds 8 of them (160 ms): how long the capture task may be held up.
static constexpr size_t CHUNK_FRAMES = VoiceCapture::SAMPLE_RATE / 50;
static constexpr uint32_t DMA_DESCRIPTORS = 8;
// Dropped after the ES7210 is enabled: its start (and its high-pass filter settling) clicks.
static constexpr size_t SETTLE_FRAMES = VoiceCapture::SAMPLE_RATE / 10;

// esp_codec_dev's handles, after _init_codec(). Only the capture task uses them.
static i2s_chan_handle_t rx_channel = nullptr;
static const audio_codec_data_if_t *data_if = nullptr;
static const audio_codec_ctrl_if_t *ctrl_if = nullptr;
static const audio_codec_if_t *codec_if = nullptr;
static esp_codec_dev_handle_t codec_dev = nullptr;

esp_err_t VoiceCapture::record(int16_t *out, size_t frames, int gain_db) {
  if (out == nullptr || frames == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (VoiceCapture::_busy.exchange(true)) {
    return ESP_ERR_INVALID_STATE;
  }
  // The task starts on the first recording, and stays (the codec is stopped between recordings).
  if (VoiceCapture::_requests == NULL) {
    esp_log_level_set(TAG, ConfigManager::log_level);
    VoiceCapture::_requests = xQueueCreate(1, sizeof(Request *));
    VoiceCapture::_done = xSemaphoreCreateBinary();
    // Above the web server (5) and the UI, so that its reads keep up with the DMA.
    if (VoiceCapture::_requests == NULL || VoiceCapture::_done == NULL ||
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
      VoiceCapture::_busy = false;
      return ESP_ERR_NO_MEM;
    }
  }
  Request request = {out, frames, gain_db, ESP_FAIL};
  Request *pending = &request;
  xQueueSend(VoiceCapture::_requests, &pending, portMAX_DELAY);
  // Bounded: every I2S read times out after a second, and a failed one ends the recording.
  xSemaphoreTake(VoiceCapture::_done, portMAX_DELAY);
  VoiceCapture::_busy = false;
  return request.result;
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
  channel_config.dma_frame_num = CHUNK_FRAMES;
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

esp_err_t VoiceCapture::_capture(Request *request) {
  // A chunk as read (TDM, SLOTS interleaved), then one array per channel.
  int16_t *tdm = static_cast<int16_t *>(heap_caps_malloc(CHUNK_FRAMES * (SLOTS + CHANNELS) * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (tdm == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  int16_t *channels[CHANNELS];
  for (int channel = 0; channel < CHANNELS; channel++) {
    channels[channel] = tdm + CHUNK_FRAMES * (SLOTS + channel);
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
    return ESP_FAIL;
  }
  // After the open, which sets the gain it kept (0 dB until one is set).
  ret = esp_codec_dev_set_in_gain(codec_dev, (float)request->gain_db);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "Gain %d dB: %d.", request->gain_db, ret);
  }

  int64_t sum[SLOTS] = {};
  uint64_t squares[SLOTS] = {};
  int peak[SLOTS] = {};
  esp_err_t result = ESP_OK;
  size_t skip = SETTLE_FRAMES, done = 0;
  while (done < request->frames) {
    size_t wanted = skip > 0 ? skip : request->frames - done;
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
        channels[SLOT_CHANNEL[slot]][i / SLOTS] = sample;
      }
    }
    VoiceCapture::_process(channels, frames, request->out + done * CHANNELS);
    done += frames;
  }
  esp_codec_dev_close(codec_dev); // Powers the ES7210 down and stops I2S
  heap_caps_free(tdm);

  // Which slot is which shows here: with the speaker silent, the reference is near silence and MIC4's
  // slot (nothing connected, not enabled) is zero, while the microphones hear the room.
  ESP_LOGI(TAG, "Recorded %u frames (%lu ms) at %d dB:", (unsigned)done, (unsigned long)(done * 1000 / SAMPLE_RATE), request->gain_db);
  for (int slot = 0; slot < SLOTS && done > 0; slot++) {
    float rms = sqrtf((float)squares[slot] / done);
    int dbfs = rms >= 1.0f ? (int)lroundf(20.0f * log10f(rms / 32768.0f)) : -91;
    ESP_LOGI(TAG, "  slot %d %-10s peak %5d, RMS %5d (%d dBFS), mean %d", slot, SLOT_NAME[slot], peak[slot], (int)lroundf(rms), dbfs,
             (int)(sum[slot] / (int64_t)done));
  }
  return result;
}

void VoiceCapture::_process(const int16_t *const channels[CHANNELS], size_t frames, int16_t *out) {
  // Phase 0b: the raw channels, interleaved again for the WAV.
  //
  // The seam for the processing to come: esp-sr's GSC beamformer on MIC1 and MIC2 (128-sample frames),
  // then WebRTC's noise suppression (ns_pro, 160-sample, 10 ms frames), with REF kept for echo
  // cancellation. The chunks here are 20 ms (320 frames) per channel, so each stage gathers its own frame
  // size from them (a FIFO per stage), and what comes out, 16 kHz mono, is written to the recording in
  // place of these three channels.
  for (size_t frame = 0; frame < frames; frame++) {
    for (int channel = 0; channel < CHANNELS; channel++) {
      out[frame * CHANNELS + channel] = channels[channel][frame];
    }
  }
}

#endif
