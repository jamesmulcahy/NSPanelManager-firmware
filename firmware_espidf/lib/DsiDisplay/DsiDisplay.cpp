#if defined(BOARD_CUSTOM)
#include "DsiDisplay.hpp"
#include "DsiPanel.h"

#include <ConfigManager.hpp>
#include <cstring>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>

static const char *TAG = "DsiDisplay";

// Waveshare's board definition (waveshare/waveshare_boards 0.3.7, esp32_p4_wifi6_touch_lcd_4b).
static constexpr int MIPI_PHY_LDO_CHANNEL = 3; // LDO_VO3 feeds VDD_MIPI_DPHY
static constexpr int MIPI_PHY_LDO_MV = 2500;
static constexpr int DSI_LANES = 2;
static constexpr int DSI_LANE_MBPS = 480;
static constexpr int PANEL_RESET_GPIO = 27;
static constexpr gpio_num_t BACKLIGHT_GPIO = GPIO_NUM_26;
static constexpr ledc_timer_t BACKLIGHT_TIMER = LEDC_TIMER_1;
static constexpr ledc_channel_t BACKLIGHT_CHANNEL = LEDC_CHANNEL_1;
static constexpr uint32_t BACKLIGHT_MAX_DUTY = 1023; // 10 bits
static constexpr i2c_port_num_t TOUCH_I2C_PORT = I2C_NUM_1;
static constexpr gpio_num_t TOUCH_SDA_GPIO = GPIO_NUM_7;
static constexpr gpio_num_t TOUCH_SCL_GPIO = GPIO_NUM_8;
static constexpr gpio_num_t TOUCH_RESET_GPIO = GPIO_NUM_23;

// How often the GT911 is read: its INT line is not wired to the P4.
static constexpr TickType_t TOUCH_POLL = pdMS_TO_TICKS(15);
// The longest show() waits for the panel to take the new frame (a frame is ~16 ms).
static constexpr TickType_t FRAME_WAIT = pdMS_TO_TICKS(100);

// End of every frame the DPI DMA sends: the frame buffer it came from may be written again. ISR, IRAM.
static IRAM_ATTR bool frame_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(user_ctx), &woken);
  return woken == pdTRUE;
}

esp_err_t DsiDisplay::init() {
  static bool started = false;
  if (started) {
    return DsiDisplay::_ready ? ESP_OK : ESP_FAIL;
  }
  started = true;
  esp_log_level_set(TAG, ConfigManager::log_level);
  // Off first, so nothing shows until the first frame.
  esp_err_t err = DsiDisplay::_init_backlight();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Backlight: %s", esp_err_to_name(err));
  }
  err = DsiDisplay::_init_panel();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Display did not start: %s", esp_err_to_name(err));
    return err;
  }
  DsiDisplay::_ready = true;
  err = DsiDisplay::_init_touch();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Touch did not start: %s", esp_err_to_name(err));
  }
  return ESP_OK;
}

esp_err_t DsiDisplay::_init_panel() {
  esp_ldo_channel_handle_t ldo = nullptr;
  esp_ldo_channel_config_t ldo_config = {};
  ldo_config.chan_id = MIPI_PHY_LDO_CHANNEL;
  ldo_config.voltage_mv = MIPI_PHY_LDO_MV;
  ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &ldo), TAG, "D-PHY LDO");

  esp_lcd_dsi_bus_handle_t bus = nullptr;
  esp_lcd_dsi_bus_config_t bus_config = {};
  bus_config.bus_id = 0;
  bus_config.num_data_lanes = DSI_LANES;
  // phy_clk_src left 0: the driver picks the D-PHY PLL reference for the chip revision (XTAL on rev 3).
  // MIPI_DSI_PHY_CLK_SRC_DEFAULT is still the pre-rev-3 PLL_F20M, which rev 3's HAL abort()s on.
  bus_config.lane_bit_rate_mbps = DSI_LANE_MBPS;
  ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &bus), TAG, "DSI bus");

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_dbi_io_config_t io_config = {};
  io_config.virtual_channel = 0;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &io_config, &io), TAG, "DBI panel IO");

  dsi_panel_kind_t kind = DSI_PANEL_UNKNOWN;
  ESP_RETURN_ON_ERROR(dsi_panel_create(bus, io, PANEL_RESET_GPIO, 2, &DsiDisplay::_panel, &kind), TAG, "panel");
  ESP_LOGI(TAG, "Panel controller: %s.", dsi_panel_name(kind));
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(DsiDisplay::_panel), TAG, "panel reset");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(DsiDisplay::_panel), TAG, "panel init");

  ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(DsiDisplay::_panel, 2, &DsiDisplay::_fbs[0], &DsiDisplay::_fbs[1]), TAG, "frame buffers");
  DsiDisplay::_fb_size = PANEL_SIZE * PANEL_SIZE * sizeof(uint16_t);
  for (void *fb : DsiDisplay::_fbs) {
    memset(fb, 0, DsiDisplay::_fb_size);
    esp_cache_msync(fb, DsiDisplay::_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  }
  DsiDisplay::_frame_sem = xSemaphoreCreateBinary();
  ESP_RETURN_ON_FALSE(DsiDisplay::_frame_sem != NULL, ESP_ERR_NO_MEM, TAG, "frame semaphore");
  esp_lcd_dpi_panel_event_callbacks_t callbacks = {};
  callbacks.on_frame_buf_complete = frame_done;
  ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(DsiDisplay::_panel, &callbacks, DsiDisplay::_frame_sem), TAG, "frame callback");
  esp_err_t on = esp_lcd_panel_disp_on_off(DsiDisplay::_panel, true);
  if (on != ESP_OK && on != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "Display on: %s", esp_err_to_name(on));
  }
  return ESP_OK;
}

esp_err_t DsiDisplay::_init_backlight() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = BACKLIGHT_TIMER;
  timer.freq_hz = 5000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
  ledc_channel_config_t channel = {};
  channel.gpio_num = BACKLIGHT_GPIO;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = BACKLIGHT_CHANNEL;
  channel.timer_sel = BACKLIGHT_TIMER;
  channel.duty = 0;
  channel.flags.output_invert = 1; // The backlight driver's enable is active low
  return ledc_channel_config(&channel);
}

void DsiDisplay::set_brightness(uint8_t percent) {
  DsiDisplay::_brightness = percent > 100 ? 100 : percent;
  DsiDisplay::_apply_brightness();
}

void DsiDisplay::_apply_brightness() {
  // Dark until there is a frame to see (the frame buffers hold black until then anyway).
  uint32_t duty = DsiDisplay::_shown ? DsiDisplay::_brightness * BACKLIGHT_MAX_DUTY / 100 : 0;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_CHANNEL, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_CHANNEL);
}

void *DsiDisplay::frame_buffer(int index) { return index == 0 || index == 1 ? DsiDisplay::_fbs[index] : nullptr; }

esp_err_t DsiDisplay::show(const void *frame) {
  if (!DsiDisplay::_ready) {
    return ESP_ERR_INVALID_STATE;
  }
  const int back = frame == DsiDisplay::_fbs[0] ? 0 : frame == DsiDisplay::_fbs[1] ? 1 : -1;
  ESP_RETURN_ON_FALSE(back >= 0, ESP_ERR_INVALID_ARG, TAG, "not one of the panel's frame buffers");
  // LVGL drew it through the cache; the panel's DMA reads memory.
  esp_cache_msync(DsiDisplay::_fbs[back], DsiDisplay::_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  // The panel's own frame buffer: the driver only switches to it, from the frame after the one it is
  // sending. Then wait for a frame to end: by then the panel sends from `back`, and the old front may be
  // written. (A frame that ended just before the switch only makes this wait one frame more.)
  esp_err_t err = esp_lcd_panel_draw_bitmap(DsiDisplay::_panel, 0, 0, PANEL_SIZE, PANEL_SIZE, DsiDisplay::_fbs[back]);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Showing the frame failed: %s", esp_err_to_name(err));
    return err;
  }
  xSemaphoreTake(DsiDisplay::_frame_sem, 0);
  if (xSemaphoreTake(DsiDisplay::_frame_sem, FRAME_WAIT) != pdTRUE) {
    ESP_LOGW(TAG, "The panel did not finish a frame in %lu ms.", (unsigned long)pdTICKS_TO_MS(FRAME_WAIT));
  }
  DsiDisplay::_front = back;
  if (!DsiDisplay::_shown.exchange(true)) {
    DsiDisplay::_apply_brightness(); // The first frame: light it.
  }
  return ESP_OK;
}

esp_err_t DsiDisplay::_init_touch() {
  // Reset the GT911 with INT not driven (it is not wired): it then answers on 0x5D or 0x14, so try both.
  gpio_config_t reset_config = {};
  reset_config.pin_bit_mask = 1ULL << TOUCH_RESET_GPIO;
  reset_config.mode = GPIO_MODE_OUTPUT;
  ESP_RETURN_ON_ERROR(gpio_config(&reset_config), TAG, "touch reset GPIO");
  gpio_set_level(TOUCH_RESET_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(TOUCH_RESET_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(100));

  i2c_master_bus_handle_t i2c = nullptr;
  i2c_master_bus_config_t bus_config = {};
  bus_config.i2c_port = TOUCH_I2C_PORT;
  bus_config.sda_io_num = TOUCH_SDA_GPIO;
  bus_config.scl_io_num = TOUCH_SCL_GPIO;
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.flags.enable_internal_pullup = true;
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c), TAG, "I2C bus");

  uint8_t address = 0;
  for (uint8_t candidate : {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP}) {
    if (i2c_master_probe(i2c, candidate, 50) == ESP_OK) {
      address = candidate;
      break;
    }
  }
  ESP_RETURN_ON_FALSE(address != 0, ESP_ERR_NOT_FOUND, TAG, "no GT911 on 0x%02X or 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                      ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  io_config.dev_addr = address;
  io_config.scl_speed_hz = 400000;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c, &io_config, &io), TAG, "touch IO");

  esp_lcd_touch_io_gt911_config_t gt911_config = {};
  gt911_config.dev_addr = address;
  esp_lcd_touch_config_t touch_config = {};
  touch_config.x_max = PANEL_SIZE;
  touch_config.y_max = PANEL_SIZE;
  touch_config.rst_gpio_num = GPIO_NUM_NC; // Reset above: another would need INT to keep the address
  touch_config.int_gpio_num = GPIO_NUM_NC;
  touch_config.driver_data = &gt911_config;
  esp_lcd_touch_handle_t touch = nullptr;
  ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io, &touch_config, &touch), TAG, "GT911");
  DsiDisplay::_touch = touch;
  ESP_LOGI(TAG, "GT911 touch on 0x%02X.", address);

  if (xTaskCreatePinnedToCore(DsiDisplay::_touch_task, "touch", 3072, NULL, 4, NULL, 0) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void DsiDisplay::_touch_task(void *param) {
  esp_lcd_touch_handle_t touch = static_cast<esp_lcd_touch_handle_t>(DsiDisplay::_touch);
  bool down = false;
  uint16_t last_x = 0, last_y = 0;
  for (;;) {
    vTaskDelay(TOUCH_POLL);
    if (esp_lcd_touch_read_data(touch) != ESP_OK) {
      continue;
    }
    esp_lcd_touch_point_data_t points[1];
    uint8_t count = 0;
    bool pressed = esp_lcd_touch_get_data(touch, points, &count, 1) == ESP_OK && count > 0;
    if (pressed) {
      last_x = points[0].x < PANEL_SIZE ? points[0].x : PANEL_SIZE - 1;
      last_y = points[0].y < PANEL_SIZE ? points[0].y : PANEL_SIZE - 1;
    } else if (!down) {
      continue;
    }
    down = pressed;
    // A release comes at the last point the finger was.
    if (DsiDisplay::on_touch != nullptr) {
      DsiDisplay::on_touch(pressed, last_x, last_y);
    }
  }
}

#endif
