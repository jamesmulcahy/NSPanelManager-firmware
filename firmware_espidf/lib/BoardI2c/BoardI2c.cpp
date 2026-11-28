#if defined(BOARD_CUSTOM)
#include "BoardI2c.hpp"

#include <esp_log.h>

static const char *TAG = "BoardI2c";

// Waveshare's board definition (waveshare/waveshare_boards 0.3.7, esp32_p4_wifi6_touch_lcd_4b).
static constexpr i2c_port_num_t I2C_PORT = I2C_NUM_1;
static constexpr gpio_num_t SDA_GPIO = GPIO_NUM_7;
static constexpr gpio_num_t SCL_GPIO = GPIO_NUM_8;

static i2c_master_bus_handle_t create_bus() {
  i2c_master_bus_config_t config = {};
  config.i2c_port = I2C_PORT;
  config.sda_io_num = SDA_GPIO;
  config.scl_io_num = SCL_GPIO;
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  esp_err_t err = i2c_new_master_bus(&config, &bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus %d (SDA %d, SCL %d) did not start: %s", (int)I2C_PORT, (int)SDA_GPIO, (int)SCL_GPIO, esp_err_to_name(err));
    return nullptr;
  }
  return bus;
}

i2c_master_bus_handle_t BoardI2c::bus() {
  // Created by the first caller; any other that comes meanwhile waits for it (a guarded static).
  static const i2c_master_bus_handle_t bus = create_bus();
  return bus;
}

#endif
