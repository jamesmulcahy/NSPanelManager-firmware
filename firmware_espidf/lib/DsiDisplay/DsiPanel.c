/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-License-Identifier: Apache-2.0
 *
 * The panel probe and the two controllers' settings are Waveshare's (waveshare/waveshare_boards 0.3.7,
 * boards/esp32_p4_wifi6_touch_lcd_4b: lcd_panel_probe.c, setup_device.c, board_devices.yaml), taken out
 * of their Board Manager hooks.
 */
#if defined(BOARD_CUSTOM)

#include "DsiPanel.h"

#include <limits.h>
#include <string.h>

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_lcd_jd9365.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_dev.h>
#include <esp_lcd_st7703.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/mipi_dsi_ll.h>

#include "jd9365_720_720_init.h"

static const char *TAG = "DsiPanel";

// A DCS read the panel never answers must not hang the boot: every wait is bounded.
#define PROBE_TIMEOUT_US (100000)

static esp_err_t send_header(dsi_host_dev_t *host, mipi_dsi_data_type_t type, uint8_t high, uint8_t low, int64_t deadline) {
  while (mipi_dsi_host_ll_gen_is_cmd_fifo_full(host)) {
    if (esp_timer_get_time() >= deadline) {
      return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(1);
  }
  mipi_dsi_host_ll_gen_set_packet_header(host, 0, type, high, low);
  return ESP_OK;
}

// A DCS read of `size` bytes, with bounded FIFO waits (esp_lcd's own read waits for ever).
static esp_err_t bounded_read(uint8_t command, uint8_t *param, size_t size) {
  dsi_host_dev_t *host = MIPI_DSI_LL_GET_HOST(0);
  const int64_t deadline = esp_timer_get_time() + PROBE_TIMEOUT_US;
  memset(param, 0, size);
  while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(host)) {
    (void)mipi_dsi_host_ll_gen_read_payload_fifo(host);
    if (esp_timer_get_time() >= deadline) {
      return ESP_ERR_TIMEOUT;
    }
  }
  esp_err_t ret = send_header(host, MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT, (size >> 8) & 0xff, size & 0xff, deadline);
  if (ret != ESP_OK) {
    return ret;
  }
  mipi_dsi_host_ll_enable_video_mode(host, false);
  mipi_dsi_host_ll_enable_bta(host, true);
  mipi_dsi_host_ll_gen_set_rx_vcid(host, 0);
  ret = send_header(host, MIPI_DSI_DT_DCS_READ_0, 0, command, deadline);
  if (ret != ESP_OK) {
    goto done;
  }
  while (mipi_dsi_host_ll_gen_is_read_cmd_busy(host) || mipi_dsi_host_ll_gen_is_read_fifo_empty(host)) {
    if (esp_timer_get_time() >= deadline) {
      ret = ESP_ERR_TIMEOUT;
      goto done;
    }
    vTaskDelay(1);
  }
  size_t copied = 0;
  while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(host)) {
    uint32_t word = mipi_dsi_host_ll_gen_read_payload_fifo(host);
    for (int i = 0; i < 4 && copied < size; ++i) {
      param[copied++] = (word >> (i * 8)) & 0xff;
    }
    if (esp_timer_get_time() >= deadline) {
      ret = ESP_ERR_TIMEOUT;
      goto done;
    }
  }
  ret = copied == size ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
done:
  mipi_dsi_host_ll_enable_bta(host, false);
  return ret;
}

static esp_err_t probe_write(uint8_t command, uint8_t value) {
  return send_header(MIPI_DSI_LL_GET_HOST(0), MIPI_DSI_DT_DCS_SHORT_WRITE_1, value, command, esp_timer_get_time() + PROBE_TIMEOUT_US);
}

static esp_err_t probe(dsi_panel_kind_t *kind) {
  *kind = DSI_PANEL_UNKNOWN;
  uint8_t id[3] = {0};
  esp_err_t ret = bounded_read(0x04, id, sizeof(id));
  ESP_LOGI(TAG, "DCS display ID: %02X %02X %02X (%s)", id[0], id[1], id[2], esp_err_to_name(ret));
  if (ret != ESP_OK) {
    return ret; // A timeout leaves the DSI command engine in an unknown state: no JD9365 page writes.
  }
  if (id[0] == 0x38 && id[1] == 0x21 && id[2] == 0x1f) {
    *kind = DSI_PANEL_ST7703;
    return ESP_OK;
  }
  // The JD9365DA answers its ID only on page 1, behind its unlock sequence.
  static const uint8_t unlock[][2] = {{0xe0, 0x00}, {0xe1, 0x93}, {0xe2, 0x65}, {0xe3, 0xf8}, {0xe0, 0x01}};
  for (size_t i = 0; i < sizeof(unlock) / sizeof(unlock[0]); ++i) {
    ret = probe_write(unlock[i][0], unlock[i][1]);
    if (ret != ESP_OK) {
      return ret;
    }
  }
  uint8_t jd_id[2] = {0};
  ret = bounded_read(0x8d, &jd_id[0], 1);
  if (ret == ESP_OK) {
    ret = bounded_read(0x8e, &jd_id[1], 1);
  }
  ESP_LOGI(TAG, "JD9365 page 1 ID: %02X %02X (%s)", jd_id[0], jd_id[1], esp_err_to_name(ret));
  if (ret == ESP_OK && jd_id[0] == 0x93 && jd_id[1] == 0x65) {
    *kind = DSI_PANEL_JD9365;
    return ESP_OK;
  }
  return ret == ESP_OK ? ESP_ERR_NOT_SUPPORTED : ret;
}

// The controller's ID can be read only once the panel is out of reset.
static esp_err_t reset_panel(int reset_gpio) {
  gpio_config_t config = {
      .pin_bit_mask = 1ULL << reset_gpio,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "panel reset GPIO");
  gpio_set_level(reset_gpio, 1);
  vTaskDelay(pdMS_TO_TICKS(5));
  gpio_set_level(reset_gpio, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(reset_gpio, 1);
  vTaskDelay(pdMS_TO_TICKS(120));
  return ESP_OK;
}

// Both kept for the panel's lifetime: the drivers keep pointers to them.
static esp_lcd_dpi_panel_config_t dpi_config;

esp_err_t dsi_panel_create(esp_lcd_dsi_bus_handle_t bus, esp_lcd_panel_io_handle_t io, int reset_gpio, uint8_t num_fbs, esp_lcd_panel_handle_t *panel,
                           dsi_panel_kind_t *kind) {
  ESP_RETURN_ON_ERROR(reset_panel(reset_gpio), TAG, "panel reset");
  ESP_RETURN_ON_ERROR(probe(kind), TAG, "no known panel controller");

  // ST7703: ST7703_720_720_PANEL_60HZ_DPI_CONFIG's timings (waveshare/esp_lcd_st7703).
  dpi_config = (esp_lcd_dpi_panel_config_t){
      .virtual_channel = 0,
      .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
      .dpi_clock_freq_mhz = 38,
      .in_color_format = LCD_COLOR_FMT_RGB565,
      .out_color_format = LCD_COLOR_FMT_RGB565,
      .num_fbs = num_fbs,
      .video_timing =
          {
              .h_size = 720,
              .v_size = 720,
              .hsync_back_porch = 50,
              .hsync_pulse_width = 20,
              .hsync_front_porch = 50,
              .vsync_back_porch = 20,
              .vsync_pulse_width = 4,
              .vsync_front_porch = 20,
          },
  };
  esp_lcd_panel_dev_config_t dev_config = {
      .reset_gpio_num = reset_gpio,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
  };

  if (*kind == DSI_PANEL_JD9365) {
    // Waveshare's 8904ANT module. The P4 makes 240/7 MHz exactly; 36 MHz made the DSI driver's timing
    // compensation give an invalid front porch.
    dpi_config.dpi_clock_freq_mhz = 240.0f / 7.0f;
    dpi_config.video_timing.hsync_back_porch = 30;
    dpi_config.video_timing.hsync_pulse_width = 24;
    dpi_config.video_timing.hsync_front_porch = 30;
    dpi_config.video_timing.vsync_back_porch = 16;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_front_porch = 12;
    static jd9365_vendor_config_t vendor;
    vendor = (jd9365_vendor_config_t){
        .init_cmds = waveshare_jd9365_720_720_init_cmds,
        .init_cmds_size = sizeof(waveshare_jd9365_720_720_init_cmds) / sizeof(waveshare_jd9365_720_720_init_cmds[0]),
        .mipi_config = {.dsi_bus = bus, .dpi_config = &dpi_config, .lane_num = 2},
    };
    dev_config.vendor_config = &vendor;
    return esp_lcd_new_panel_jd9365(io, &dev_config, panel);
  }

  static st7703_vendor_config_t vendor;
  vendor = (st7703_vendor_config_t){
      .mipi_config = {.dsi_bus = bus, .dpi_config = &dpi_config},
      .flags = {.use_mipi_interface = 1},
  };
  dev_config.vendor_config = &vendor;
  return esp_lcd_new_panel_st7703(io, &dev_config, panel);
}

const char *dsi_panel_name(dsi_panel_kind_t kind) {
  switch (kind) {
  case DSI_PANEL_ST7703:
    return "ST7703";
  case DSI_PANEL_JD9365:
    return "JD9365DA";
  default:
    return "unknown";
  }
}

#endif
