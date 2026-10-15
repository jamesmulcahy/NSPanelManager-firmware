#include <ConfigManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <StatusUpdateManager.hpp>
#include <StatusUpdateManager_events.hpp>
#include <UpdateManager_event.hpp>
#include <WiFiManager.hpp>
#include <cmath>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_wifi.h>

// If we are compiling for custom NSPanel PCB, include sources to work with those components.
#if defined(BOARD_CUSTOM)
#include <bmx280.h>
#include <driver/i2c_master.h>
#endif

ESP_EVENT_DEFINE_BASE(STATUSUPDATEMANAGER_EVENT);

void StatusUpdateManager::init() {
  esp_log_level_set("StatusUpdateManager", ConfigManager::log_level);

  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, ESP_EVENT_ANY_ID, StatusUpdateManager::_event_handler, NULL);
  esp_event_handler_register(UPDATEMANAGER_EVENT, ESP_EVENT_ANY_ID, StatusUpdateManager::_update_manager_event_handler, NULL);

  // Setup Mutex and default values before starting timers/tasks that handle _status_report
  StatusUpdateManager::_status_report_mutex = xSemaphoreCreateMutex();
  nspanel_status_report__init(&StatusUpdateManager::_status_report);
  StatusUpdateManager::_status_report.nspanel_state = NSPanelStatusReport__State::NSPANEL_STATUS_REPORT__STATE__ONLINE;
  StatusUpdateManager::_status_report.update_progress = 0;
  StatusUpdateManager::_status_report.rssi = 0;
  StatusUpdateManager::_status_report.temperature = 0;
  StatusUpdateManager::_status_report.mac_address = (char *)WiFiManager::mac_string();
  StatusUpdateManager::_status_report.md5_firmware = (char *)ConfigManager::md5_firmware.c_str();
  StatusUpdateManager::_status_report.md5_littlefs = (char *)ConfigManager::md5_data_file.c_str();
  StatusUpdateManager::_status_report.md5_tft_gui = (char *)ConfigManager::md5_gui.c_str();
#if defined(BOARD_CUSTOM)
  StatusUpdateManager::_status_report.has_humidity = true;
  StatusUpdateManager::_status_report.has_pressure = true;
#else
  StatusUpdateManager::_status_report.has_humidity = false;
  StatusUpdateManager::_status_report.has_pressure = false;
#endif

  // Create status update timer

  esp_timer_create_args_t status_update_timer_args = {};
  status_update_timer_args.callback = StatusUpdateManager::_send_status_update;
  status_update_timer_args.name = "status_update_timer";
  esp_err_t err = esp_timer_create(&status_update_timer_args, &StatusUpdateManager::_status_update_timer);
  if (err != ESP_OK) {
    ESP_LOGE("StatusUpdateManager", "Failed to start ESP timer to periodically send status updates! Error: %s", esp_err_to_name(err));
  }

#if defined(BOARD_CUSTOM)
  // This is the custom PCB with I2C components. Initialize I2C as Master mode to communicate with sensors.

  i2c_master_bus_config_t i2c_mst_config = {};
  i2c_mst_config.i2c_port = I2C_NUM_0;
  i2c_mst_config.sda_io_num = GPIO_NUM_8;
  i2c_mst_config.scl_io_num = GPIO_NUM_18;
  i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c_mst_config.glitch_ignore_cnt = 7;
  i2c_mst_config.flags.enable_internal_pullup = true;

  // Create master bus
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &StatusUpdateManager::_i2c_master_bus_handle));
  ESP_ERROR_CHECK(i2c_master_bus_reset(StatusUpdateManager::_i2c_master_bus_handle));

  StatusUpdateManager::_initialize_bme280();
  // StatusUpdateManager::_initialize_ltr303();

  vTaskDelay(pdMS_TO_TICKS(50)); // Wait for sensors to start
#endif

// Setup ADC for reading temperature
#if not defined(BOARD_CUSTOM) && not defined(NSPM_QEMU)
  // Init ADC1
  adc_oneshot_unit_init_cfg_t adc1_init_config = {};
  adc1_init_config.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&adc1_init_config, &StatusUpdateManager::_temp_adc_handle) == ESP_OK) {
    adc_oneshot_chan_cfg_t config = {};
    config.atten = ADC_ATTEN_DB_12;
    config.bitwidth = ADC_BITWIDTH_12;
    if (adc_oneshot_config_channel(StatusUpdateManager::_temp_adc_handle, ADC_CHANNEL_2, &config) == ESP_OK) {
      adc_cali_line_fitting_config_t cali_config = {};
      cali_config.unit_id = ADC_UNIT_1;
      cali_config.atten = ADC_ATTEN_DB_12;
      cali_config.bitwidth = ADC_BITWIDTH_12;
      if (adc_cali_create_scheme_line_fitting(&cali_config, &StatusUpdateManager::_temp_calibration_data) == ESP_OK) {
        ESP_LOGI("StatusUpdateManager", "Temperature ADC init successfully!");
      } else {
        ESP_LOGE("StatusUpdateManager", "Failed to create line fitting calibration data/scheme for temp reading!");
      }
    } else {
      ESP_LOGE("StatusUpdateManager", "Failed to oneshot config ADC for temperature reading!");
    }
  } else {
    ESP_LOGE("StatusUpdateManager", "Failed to configure ADC for temperature reading!");
  }
#endif

  err = esp_timer_start_periodic(StatusUpdateManager::_status_update_timer, 30000 * 1000); // Send status update every 30 seconds
  if (err != ESP_OK) {
    ESP_LOGE("StatusUpdateManager", "Failed to start periodic timer for sending status updates! Error: %s", esp_err_to_name(err));
  }

  // Start the task that is responsible for reading new temperature data
  // We use a task for this instead of a periodic timer as a periodic timer does not wake CPU from light sleep
  if (xTaskCreatePinnedToCore(StatusUpdateManager::_measure_temperature, "measure_temp", 3072, NULL, 1, NULL, 1) != pdTRUE) {
    ESP_LOGE("StatusUpdateManager", "Failed to create task to read temperature.");
  }
}

float StatusUpdateManager::current_temperature() {
  return StatusUpdateManager::_measured_average_temperature.get();
}

void StatusUpdateManager::reboot() {
  esp_timer_stop(StatusUpdateManager::_status_update_timer);
  StatusUpdateManager::_status_report.nspanel_state = NSPANEL_STATUS_REPORT__STATE__REBOOTING;
  StatusUpdateManager::_send_status_update(NULL);

  vTaskDelay(pdMS_TO_TICKS(2000)); // Wait two seconds to message to be sent then reboot
  esp_restart();
}

void StatusUpdateManager::_send_status_update(void *arg) {
  int current_wifi_rssi;
#if defined(NSPM_QEMU)
  // Wi-Fi is never initialised under QEMU, and esp_wifi_sta_get_rssi()
  // dereferences driver state that only esp_wifi_init() creates.
  current_wifi_rssi = -50;
  if (true) {
#else
  if (esp_wifi_sta_get_rssi(&current_wifi_rssi) == ESP_OK) [[likely]] {
#endif
    StatusUpdateManager::_status_report.rssi = current_wifi_rssi;
  } else {
    ESP_LOGW("StatusUpdateManager", "Failed to get current wifi RSSI. Will use old value.");
  }

  size_t packed_data_size = 0;
  std::vector<uint8_t> buffer;
  if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    float total_heap_size = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    float total_heap_used = total_heap_size - heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

    size_t heap_used_pct = (total_heap_used / total_heap_size) * 100;
    StatusUpdateManager::_status_report.heap_used_pct = heap_used_pct;
    StatusUpdateManager::_status_report.ip_address = (char *)WiFiManager::ip_string().c_str();
    StatusUpdateManager::_status_report.temperature = StatusUpdateManager::_measured_average_temperature.get();
#if defined(BOARD_CUSTOM)
    StatusUpdateManager::_status_report.humidity = StatusUpdateManager::_measured_average_humidity.get();
    StatusUpdateManager::_status_report.pressure = StatusUpdateManager::_measured_average_pressure.get();
#endif

    // TODO: Load warnings
    // Send status update
    size_t packed_length = nspanel_status_report__get_packed_size(&StatusUpdateManager::_status_report);
    buffer.resize(packed_length);
    packed_data_size = nspanel_status_report__pack(&StatusUpdateManager::_status_report, buffer.data());
    xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
  } else {
    ESP_LOGE("StatusUpdateManager", "Failed to get _status_update_mutex when trying to send status update!");
  }

  if (packed_data_size > 0) {
    // Build MQTT topic to publish status report on:
    std::string status_report_topic = "nspanel/";
    status_report_topic.append(WiFiManager::mac_string());
    status_report_topic.append("/status_report");

    if (MqttManager::publish(status_report_topic, (const char *)buffer.data(), packed_data_size, false) != ESP_OK) {
      ESP_LOGW("StatusUpdateManager", "Failed to send status report. Will try again next time.");
    }
  }
}

#if defined(BOARD_SONOFF)
void StatusUpdateManager::_measure_temperature(void *arg) {
  for (;;) {
    int read_voltage_mv;
#if defined(NSPM_QEMU)
    // QEMU does not emulate the SAR ADC: a conversion spins forever on a
    // conversion-done bit with the ADC spinlock held, freezing the core.
    // Report the NTC voltage for roughly 22 C instead.
    read_voltage_mv = 1680;
#else
    int read_voltage_mv_raw;
    if (adc_oneshot_read(StatusUpdateManager::_temp_adc_handle, ADC_CHANNEL_2, &read_voltage_mv_raw) != ESP_OK) {
      ESP_LOGE("StatusUpdateManager", "Failed to read raw voltage data from temp sensor. Will retry in 5 seconds.");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (adc_cali_raw_to_voltage(StatusUpdateManager::_temp_calibration_data, read_voltage_mv_raw, &read_voltage_mv) != ESP_OK) {
      ESP_LOGE("StatusUpdateManager", "Failed to convert read raw voltage with calibration data.");
      return;
    }
#endif

    // We now have temperature as a voltage. Convert voltage into celsius:
    double read_voltage_v = (double)read_voltage_mv / 1000.0; // Convert mV to V.
    if (read_voltage_v <= 0.5 || read_voltage_v >= 3.25) {
      ESP_LOGW("StatusUpdateManager", "Voltage read from thermistor outside of range. Will skip measurement.");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // Calculate temperature from NTC using the Steinhart–Hart equation
    // See https://robertvicol.com/tech/arduino-measuring-temperature-with-ntc-steinhart-hart-formula/ for example
    // Thanks to @atirage in https://github.com/arendst/Tasmota/discussions/15737 for providing reference values.
    double R2 = 10000.0 / ((3.3 / read_voltage_v) - 1);
    double steinhart = R2 / 10000.0;                 // (R/Ro)
    steinhart = log(steinhart);                      // ln(R/Ro)
    steinhart /= 3950.0;                             // 1/B * ln(R/Ro)
    steinhart += 1.0 / (25.0 + 273.15);              // + (1/To)
    steinhart = 1.0 / steinhart;                     // Invert
    double current_temperature = steinhart - 273.15; // convert Kelvin to *C

    if (StatusUpdateManager::_measure_temperature_in_fahrenheit) {
      current_temperature = (current_temperature * 1.8) + 32;
    }

    current_temperature += StatusUpdateManager::_temperature_offset_calibration;

    StatusUpdateManager::_measured_temperatures[StatusUpdateManager::_measured_temperature_next_index++] = current_temperature;
    // We only have space for 30 samples.
    if (StatusUpdateManager::_measured_temperature_total_samples < 30) {
      StatusUpdateManager::_measured_temperature_total_samples++;
    }

    if (StatusUpdateManager::_measured_temperature_next_index >= 30) {
      StatusUpdateManager::_measured_temperature_next_index = 0;
      // Only calculate and send event ever 30 seconds to skip unnecessary events

      double current_average_temperature = 0;
      for (int i = 0; i < StatusUpdateManager::_measured_temperature_total_samples; i++) {
        current_average_temperature += StatusUpdateManager::_measured_temperatures[i] / StatusUpdateManager::_measured_temperature_total_samples;
      }
      StatusUpdateManager::_measured_average_temperature.set(current_average_temperature);
      esp_event_post(STATUSUPDATEMANAGER_EVENT, statusupdatemanagerevent_t::AVERAGE_TEMP_UPDATE, &current_average_temperature, sizeof(current_average_temperature), pdMS_TO_TICKS(250));
    }

    vTaskDelay(pdMS_TO_TICKS(1000)); // Read temp again in 1 second
  }
}
#elif defined(BOARD_CUSTOM)
void StatusUpdateManager::_measure_temperature(void *arg) {
  for (;;) {
    if (StatusUpdateManager::_bme280_initialized) [[likely]] {
      float temperature = 0;
      float pressure = 0;
      float humidity = 0;
      // While for sensor to finish sampling.
      while (bmx280_isSampling(StatusUpdateManager::_bme280_dev_handle)) {
        vTaskDelay(pdMS_TO_TICKS(1));
      }

      ESP_ERROR_CHECK(bmx280_readoutFloat(StatusUpdateManager::_bme280_dev_handle, &temperature, &pressure, &humidity));

      // Update temperature
      temperature += StatusUpdateManager::_temperature_offset_calibration;
      if (StatusUpdateManager::_measure_temperature_in_fahrenheit) {
        temperature = (temperature * 1.8) + 32;
      }

      StatusUpdateManager::_measured_temperatures[StatusUpdateManager::_measured_temperature_next_index++] = temperature;
      // We only have space for 30 samples.
      if (StatusUpdateManager::_measured_temperature_total_samples < 30) {
        StatusUpdateManager::_measured_temperature_total_samples++;
      }

      if (StatusUpdateManager::_measured_temperature_next_index >= 30) {
        StatusUpdateManager::_measured_temperature_next_index = 0;
        // Only calculate and send event ever 30 seconds to skip unnecessary events

        double current_average_temperature = 0;
        for (int i = 0; i < StatusUpdateManager::_measured_temperature_total_samples; i++) {
          current_average_temperature += StatusUpdateManager::_measured_temperatures[i] / StatusUpdateManager::_measured_temperature_total_samples;
        }
        StatusUpdateManager::_measured_average_temperature.set(current_average_temperature);
        esp_event_post(STATUSUPDATEMANAGER_EVENT, statusupdatemanagerevent_t::AVERAGE_TEMP_UPDATE, &current_average_temperature, sizeof(current_average_temperature), pdMS_TO_TICKS(250));
      }

      // Update humidity
      StatusUpdateManager::_measured_humidity[StatusUpdateManager::_measured_humidity_next_index++] = humidity;
      // We only have space for 30 samples.
      if (StatusUpdateManager::_measured_humidity_total_samples < 30) {
        StatusUpdateManager::_measured_humidity_total_samples++;
      }

      if (StatusUpdateManager::_measured_humidity_next_index >= 30) {
        StatusUpdateManager::_measured_humidity_next_index = 0;
        // Only calculate and send event ever 30 seconds to skip unnecessary events

        double current_average_humidity = 0;
        for (int i = 0; i < StatusUpdateManager::_measured_humidity_total_samples; i++) {
          current_average_humidity += StatusUpdateManager::_measured_humidity[i] / StatusUpdateManager::_measured_humidity_total_samples;
        }
        StatusUpdateManager::_measured_average_humidity.set(current_average_humidity);
        esp_event_post(STATUSUPDATEMANAGER_EVENT, statusupdatemanagerevent_t::AVERAGE_HUMIDITY_UPDATE, &current_average_humidity, sizeof(current_average_humidity), pdMS_TO_TICKS(250));
      }

      // Update pressure
      StatusUpdateManager::_measured_pressure[StatusUpdateManager::_measured_pressure_next_index++] = pressure;
      // We only have space for 30 samples.
      if (StatusUpdateManager::_measured_pressure_total_samples < 30) {
        StatusUpdateManager::_measured_pressure_total_samples++;
      }

      if (StatusUpdateManager::_measured_pressure_next_index >= 30) {
        StatusUpdateManager::_measured_pressure_next_index = 0;
        // Only calculate and send event ever 30 seconds to skip unnecessary events

        double current_average_pressure = 0;
        for (int i = 0; i < StatusUpdateManager::_measured_pressure_total_samples; i++) {
          current_average_pressure += StatusUpdateManager::_measured_pressure[i] / StatusUpdateManager::_measured_pressure_total_samples;
        }
        StatusUpdateManager::_measured_average_pressure.set(current_average_pressure);
        esp_event_post(STATUSUPDATEMANAGER_EVENT, statusupdatemanagerevent_t::AVERAGE_PRESSURE_UPDATE, &current_average_pressure, sizeof(current_average_pressure), pdMS_TO_TICKS(250));
      }
    } else {
      ESP_LOGW("StatusUpdateManager", "Skipping temperature, humidity and pressure reading as BME280 sensor was not initialized correctly.");

      if ((esp_timer_get_time() / 1000) - StatusUpdateManager::_last_bme280_init_try >= 10000) { // More than >= 10 seconds since last init try.
        StatusUpdateManager::_initialize_bme280();
      }
    }

    // if (StatusUpdateManager::_ltr303_initialized) [[likely]] {
    //   // for (int i = 0; i < 10; i++) {
    //   //   uint8_t status;

    //   //   if (ltr303_als_status_get(StatusUpdateManager::_ltr303_dev_handle, &status, 10) == ESP_OK) {
    //   //     if (als_status_data_valid(status) != ALS_STATUS_DATA_INVALID) {
    //   //       break; // Successfully got status. Exit loop.
    //   //     }
    //   //   }
    //   //   vTaskDelay(pdMS_TO_TICKS(10));
    //   // }

    //   uint16_t ch0;
    //   uint16_t ch1;
    //   if (StatusUpdateManager::_ltr303_dev_handle->readBothChannels(ch0, ch1) == ESP_OK) {
    //     // float lux = ltr303_als_to_lux(ALS_CONTR_GAIN_1X, ALS_INT_TIME_100, 1.0, ch0, ch1);
    //     uint16_t lux = StatusUpdateManager::_ltr303_dev_handle->computeLux(ch0, ch1);
    //     ESP_LOGI("StatusUpdateManager", "Got CH0: %d, CH1: %d, lux: %.2f", ch0, ch1, lux);
    //   } else {
    //     ESP_LOGE("StatusUpdateManager", "Failed to get LTR303 data.");
    //   }
    // } else {
    //   ESP_LOGW("StatusUpdateManager", "Skipping lux reading as LTR303 sensor was not initialized correctly.");

    //   if ((esp_timer_get_time() / 1000) - StatusUpdateManager::_last_ltr303_init_try >= 10000) { // More than >= 10 seconds since last init try.
    //     StatusUpdateManager::_initialize_ltr303();
    //   }
    // }

    // Read lux
    // uint8_t tries = 0;
    // while (tries < 10) {
    //   uint8_t status;
    //   if (ltr303_als_status_get(StatusUpdateManager::_ltr303_dev_handle, &status, 10) == ESP_OK) {
    //     if (als_status_data_valid(status) == ALS_STATUS_DATA_INVALID) {
    //       vTaskDelay(pdMS_TO_TICKS(10));
    //       continue;
    //     }

    //     uint16_t ch0;
    //     uint16_t ch1;
    //     if (ltr303_als_data_get(StatusUpdateManager::_ltr303_dev_handle, &ch0, &ch1, 10) == ESP_OK) {
    //       float lux = ltr303_als_to_lux(ALS_CONTR_GAIN_1X, ALS_INT_TIME_100, 1.0, ch0, ch1);
    //       ESP_LOGI("StatusUpdateManager", "Got CH0: %d, CH1: %d, lux: %.2f", ch0, ch1, lux);
    //     } else {
    //       ESP_LOGE("StatusUpdateManager", "Failed to get LTR303 data.");
    //     }
    //   } else {
    //     ESP_LOGE("StatusUpdateManager", "Failed to get LTR303 status. Will retry in 100ms.");
    //     vTaskDelay(pdMS_TO_TICKS(100));
    //   }
    //   tries++;
    // }
    // uint8_t start_reg = 0x88;
    // uint16_t result = 0;
    // if (i2c_master_transmit_receive(StatusUpdateManager::_ltr303_dev_handle, &start_reg, 1, (uint8_t *)&result, 2, 500) == ESP_OK) {
    //   ESP_LOGD("StatusUpdateManager", "Read %" PRIu16 " from LTR303 CH0", result);
    // } else {
    //   ESP_LOGE("StatusUpdateManager", "Failed to read data from LTR303 CH1 sensor!");
    // }

    // start_reg = 0x8A;
    // result = 0;
    // if (i2c_master_transmit_receive(StatusUpdateManager::_ltr303_dev_handle, &start_reg, 1, (uint8_t *)&result, 2, 500) == ESP_OK) {
    //   ESP_LOGD("StatusUpdateManager", "Read %" PRIu16 " from LTR303 CH1", result);
    // } else {
    //   ESP_LOGE("StatusUpdateManager", "Failed to read data from LTR303 CH0 sensor!");
    // }

    vTaskDelay(pdMS_TO_TICKS(1000)); // Read temp again in 1 second
  }
}
#endif

void StatusUpdateManager::_update_from_config() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    StatusUpdateManager::_measure_temperature_in_fahrenheit = config->use_fahrenheit;
    StatusUpdateManager::_temperature_offset_calibration = (config->temperature_calibration / 10.0);
  } else {
    ESP_LOGE("StatusUpdateManager", "Failed to get config to determine if we should measure temperature in C or F. Will assume C.");
  }
}

void StatusUpdateManager::_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == NSPM_CONFIGMANAGER_EVENT) {
    switch (event_id) {
    case nspm_configmanager_event::CONFIG_LOADED: {
      StatusUpdateManager::_update_from_config();
      break;
    }

    default:
      break;
    }
  }
}

void StatusUpdateManager::_update_manager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case updatemanager_event_t::FIRMWARE_UPDATE_STARTED: {
    ESP_LOGD("StatusUpdateManager", "Updating status report state.");
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      StatusUpdateManager::_status_report.nspanel_state = NSPanelStatusReport__State::NSPANEL_STATUS_REPORT__STATE__UPDATING_FIRMWARE;
      StatusUpdateManager::_status_report.update_progress = 0;
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }

    // Restart timer with a 1 second interval instead of default 30
    esp_timer_stop(StatusUpdateManager::_status_update_timer);
    esp_timer_start_periodic(StatusUpdateManager::_status_update_timer, 1000 * 1000);
    break;
  }

  case updatemanager_event_t::LITTLEFS_UPDATE_STARTED: {
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      StatusUpdateManager::_status_report.nspanel_state = NSPanelStatusReport__State::NSPANEL_STATUS_REPORT__STATE__UPDATING_LITTLEFS;
      StatusUpdateManager::_status_report.update_progress = 0;
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }

    // Restart timer with a 1 second interval instead of default 30
    esp_timer_stop(StatusUpdateManager::_status_update_timer);
    esp_timer_start_periodic(StatusUpdateManager::_status_update_timer, 1000 * 1000);
    break;
  }

  case updatemanager_event_t::NEXTION_UPDATE_STARTED: {
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      StatusUpdateManager::_status_report.nspanel_state = NSPanelStatusReport__State::NSPANEL_STATUS_REPORT__STATE__UPDATING_TFT;
      StatusUpdateManager::_status_report.update_progress = 0;
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }

    // Restart timer with a 5 second interval instead of default 30. Use 5 seconds as the TFT/Nextion update is really slow.
    esp_timer_stop(StatusUpdateManager::_status_update_timer);
    esp_timer_start_periodic(StatusUpdateManager::_status_update_timer, 5000 * 1000);
    break;
  }

  case updatemanager_event_t::FIRMWARE_UPDATE_PROGRESS: {
    float *progress_percentage = (float *)event_data;
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      StatusUpdateManager::_status_report.update_progress = std::round(*progress_percentage);
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }
    break;
  }

  case updatemanager_event_t::LITTLEFS_UPDATE_PROGRESS: {
    float *progress_percentage = (float *)event_data;
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      StatusUpdateManager::_status_report.update_progress = std::round(*progress_percentage);
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }
    break;
  }

  case updatemanager_event_t::NEXTION_UPDATE_PROGRESS: {
    float *progress_percentage = (float *)event_data;
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      StatusUpdateManager::_status_report.update_progress = std::round(*progress_percentage);
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }
    break;
  }

  case updatemanager_event_t::FIRMWARE_UPDATE_FINISHED: {
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      StatusUpdateManager::_status_report.update_progress = 100;
      StatusUpdateManager::_status_report.nspanel_state = NSPanelStatusReport__State::NSPANEL_STATUS_REPORT__STATE__REBOOTING;
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }
    break;
  }

  case updatemanager_event_t::FIRMWARE_UPDATE_FAILED: {
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      StatusUpdateManager::_status_report.update_progress = 0;
      StatusUpdateManager::_status_report.nspanel_state = NSPanelStatusReport__State::NSPANEL_STATUS_REPORT__STATE__ONLINE;
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }

    // Restore the default 30 second status update interval
    esp_timer_stop(StatusUpdateManager::_status_update_timer);
    esp_timer_start_periodic(StatusUpdateManager::_status_update_timer, 30000 * 1000);
    break;
  }

  case updatemanager_event_t::LITTLEFS_UPDATE_FINISHED: {
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      StatusUpdateManager::_status_report.update_progress = 100;
      StatusUpdateManager::_status_report.nspanel_state = NSPanelStatusReport__State::NSPANEL_STATUS_REPORT__STATE__REBOOTING;
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }
    break;
  }

  case updatemanager_event_t::NEXTION_UPDATE_FINISHED: {
    if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      StatusUpdateManager::_status_report.update_progress = 100;
      StatusUpdateManager::_status_report.nspanel_state = NSPanelStatusReport__State::NSPANEL_STATUS_REPORT__STATE__REBOOTING;
      xSemaphoreGive(StatusUpdateManager::_status_report_mutex);
    }
    break;
  }

  default:
    break;
  }
}

#if defined(BOARD_CUSTOM)
bool StatusUpdateManager::_initialize_bme280() {
  StatusUpdateManager::_last_bme280_init_try = esp_timer_get_time() / 1000;

  // BME280 setup
  StatusUpdateManager::_bme280_dev_handle = bmx280_create_master(StatusUpdateManager::_i2c_master_bus_handle);
  if (bmx280_init(StatusUpdateManager::_bme280_dev_handle) == ESP_OK) {
    bmx280_config_t bme280_config = BMX280_DEFAULT_CONFIG;
    if (bmx280_configure(StatusUpdateManager::_bme280_dev_handle, &bme280_config) == ESP_OK) {
      if (bmx280_setMode(StatusUpdateManager::_bme280_dev_handle, BMX280_MODE_CYCLE) == ESP_OK) {
        StatusUpdateManager::_bme280_initialized = true;
        ESP_LOGE("StatusUpdateManager", "BME280 initialized!");
        return true;
      } else {
        ESP_LOGE("StatusUpdateManager", "Failed to set BME280 mode!");
      }
    } else {
      ESP_LOGE("StatusUpdateManager", "Failed to initialize the BME280 sensor!");
    }
  } else {
    ESP_LOGE("StatusUpdateManager", "Failed to initialize the BME280 sensor!");
  }

  return false;
}

// bool StatusUpdateManager::_initialize_ltr303() {
//   StatusUpdateManager::_last_ltr303_init_try = esp_timer_get_time() / 1000;
//   // ltr303_free(StatusUpdateManager::_ltr303_dev_handle);

//   // if (ltr303_new(&StatusUpdateManager::_ltr303_dev_handle, StatusUpdateManager::_i2c_master_bus_handle) == ESP_OK) {
//   //   vTaskDelay(pdMS_TO_TICKS(500));
//   //   if (ltr303_als_contr_set(StatusUpdateManager::_ltr303_dev_handle, ALS_CONTR_GAIN_1X, ALS_SW_RESET_OFF, ALS_ACTIVE, 100) == ESP_OK) {
//   //     vTaskDelay(pdMS_TO_TICKS(500));
//   //     if (ltr303_als_meas_rate_set(StatusUpdateManager::_ltr303_dev_handle, ALS_INT_TIME_100, ASL_MEAS_RATE_500, 100) == ESP_OK) {
//   //       vTaskDelay(pdMS_TO_TICKS(100));
//   //       StatusUpdateManager::_ltr303_initialized = true;
//   //       ESP_LOGI("StatusUpdateManager", "LTR303 Initialized.");
//   //       return true;
//   //     } else {
//   //       ESP_LOGE("StatusUpdateManager", "Failed to configure measurement rate of LTR303 I2C device!");
//   //     }
//   //   } else {
//   //     ESP_LOGE("StatusUpdateManager", "Failed to configure LTR303 I2C device!");
//   //   }
//   // } else {
//   //   ESP_LOGE("StatusUpdateManager", "Failed to initialize LTR303 I2C device!");
//   // }
//   delete StatusUpdateManager::_ltr303_dev_handle;
//   StatusUpdateManager::_ltr303_dev_handle = new LTR303(StatusUpdateManager::_i2c_master_bus_handle, LTR303_I2C_ADDR);
//   if (StatusUpdateManager::_ltr303_dev_handle->enable(true) == ESP_OK) {
//     ESP_LOGI("StatusUpdateManager", "LTR303 Initialized.");
//     return true;
//   } else {
//     ESP_LOGE("StatusUpdateManager", "Failed to initialize LTR303 I2C device!");
//   }

//   return false;
// }

#endif