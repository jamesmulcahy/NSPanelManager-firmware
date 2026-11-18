#include <PanelRestart.hpp>
#include <ConfigManager.hpp>
#include <LvglLook.hpp>
#include <LvglUi.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <Nextion.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <StatusUpdateManager.hpp>
#include <StatusUpdateManager_events.hpp>
#include <TimedEspTimer.hpp>
#include <UpdateManager_event.hpp>
#include <WebManager.hpp>
#include <WebUiEmbedded.hpp>
#include <WiFiManager.hpp>
#include <algorithm>
#include <cmath>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_wifi.h>

static_assert((int)LvglUi::DisplayMode::NEXTION == NSPANEL_STATUS_REPORT__NSPANEL_DISPLAY_MODE__DISPLAY_MODE_NEXTION);
static_assert((int)LvglUi::DisplayMode::LVGL == NSPANEL_STATUS_REPORT__NSPANEL_DISPLAY_MODE__DISPLAY_MODE_LVGL);
static_assert((int)LvglUi::DisplayMode::TERMINAL_IDLE == NSPANEL_STATUS_REPORT__NSPANEL_DISPLAY_MODE__DISPLAY_MODE_TERMINAL_IDLE);

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
  StatusUpdateManager::_status_report.has_temperature = false; // Until the first average exists.
  StatusUpdateManager::_status_report.mac_address = (char *)WiFiManager::mac_string();
  StatusUpdateManager::_status_report.md5_firmware = (char *)ConfigManager::md5_firmware.c_str();
  StatusUpdateManager::_status_report.md5_littlefs = (char *)NSPM_WEB_UI_EMBEDDED;
  StatusUpdateManager::_status_report.md5_tft_gui = (char *)ConfigManager::md5_gui.c_str();
  nspanel_warning__init(&StatusUpdateManager::_link_baud_warning);
  StatusUpdateManager::_link_baud_warning.level = NSPANEL_WARNING_LEVEL__WARNING;
  // Constant, so set once here rather than in _send_status_update. Repeated from register_request
  // so a restarted manager relearns it from the next report.
  StatusUpdateManager::_status_report.metrics_port = WebManager::HTTP_PORT;
#if defined(BOARD_CUSTOM)
  StatusUpdateManager::_status_report.has_humidity = true;
  StatusUpdateManager::_status_report.has_pressure = true;
#else
  StatusUpdateManager::_status_report.has_humidity = false;
  StatusUpdateManager::_status_report.has_pressure = false;
#endif

  // Create status update timer

  esp_timer_create_args_t status_update_timer_args = {};
  status_update_timer_args.name = "status_update_timer";
  esp_err_t err = timed_esp_timer_create<StatusUpdateManager::_send_status_update, PanelMetrics::TIMER_STATUS_UPDATE>(status_update_timer_args, &StatusUpdateManager::_status_update_timer);
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
      cali_config.default_vref = 1100; // Used when the chip has no factory reference voltage burned in; without one the conversion fails every time.
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

  err = esp_timer_start_periodic(StatusUpdateManager::_status_update_timer, StatusUpdateManager::_STATUS_UPDATE_INTERVAL_US); // Send status update every 30 seconds
  if (err != ESP_OK) {
    ESP_LOGE("StatusUpdateManager", "Failed to start periodic timer for sending status updates! Error: %s", esp_err_to_name(err));
  }

  // Start the task that is responsible for reading new temperature data
  // We use a task for this instead of a periodic timer as a periodic timer does not wake CPU from light sleep
  if (xTaskCreatePinnedToCore(StatusUpdateManager::_measure_temperature, "measure_temp", 3072, NULL, 1, NULL, 1) != pdTRUE) {
    ESP_LOGE("StatusUpdateManager", "Failed to create task to read temperature.");
  }
}

double StatusUpdateManager::_to_display_temperature(double celsius) {
  if (StatusUpdateManager::_measure_temperature_in_fahrenheit) {
    celsius = (celsius * 1.8) + 32;
  }
  return celsius + StatusUpdateManager::_temperature_offset_calibration;
}

float StatusUpdateManager::current_temperature() {
  return StatusUpdateManager::_to_display_temperature(StatusUpdateManager::_measured_average_temperature.get());
}

bool StatusUpdateManager::has_valid_temperature() {
  return StatusUpdateManager::_temperature_valid;
}

void StatusUpdateManager::_post_average_temperature(TickType_t timeout) {
  double average = StatusUpdateManager::_to_display_temperature(StatusUpdateManager::_measured_average_temperature.get());
  StatusUpdateManager::_last_posted_temperature.set(average);
  StatusUpdateManager::_readings_since_post = 0;
  esp_event_post(STATUSUPDATEMANAGER_EVENT, statusupdatemanagerevent_t::AVERAGE_TEMP_UPDATE, &average, sizeof(average), timeout);
}

void StatusUpdateManager::_note_bad_temperature_reading(const char *reason) {
  // Log when a run of bad readings starts and when it becomes a failure, not once a second.
  if (StatusUpdateManager::_bad_temperature_readings == 0) {
    ESP_LOGW("StatusUpdateManager", "Temperature reading unusable: %s. Will keep trying every second.", reason);
  }
  if (StatusUpdateManager::_bad_temperature_readings < StatusUpdateManager::_TEMPERATURE_FAILURE_READINGS) {
    StatusUpdateManager::_bad_temperature_readings++;
    if (StatusUpdateManager::_bad_temperature_readings == StatusUpdateManager::_TEMPERATURE_FAILURE_READINGS) {
      ESP_LOGE("StatusUpdateManager", "No usable temperature reading for %d seconds (last: %s). Reporting no temperature and switching thermostat relays off.", (int)StatusUpdateManager::_TEMPERATURE_FAILURE_READINGS, reason);
      StatusUpdateManager::_temperature_valid = false;
      // Start over when the sensor comes back: a new warm-up, not an average with readings from before the outage.
      StatusUpdateManager::_warmup_sum = 0;
      StatusUpdateManager::_warmup_count = 0;
      StatusUpdateManager::_readings_since_post = 0;
      esp_event_post(STATUSUPDATEMANAGER_EVENT, statusupdatemanagerevent_t::TEMPERATURE_SENSOR_FAILED, NULL, 0, pdMS_TO_TICKS(250));
    }
  }
}

void StatusUpdateManager::_add_temperature_reading(double celsius) {
  if (StatusUpdateManager::_bad_temperature_readings > 0) {
    ESP_LOGI("StatusUpdateManager", "Temperature readings are usable again after %d bad ones.", (int)StatusUpdateManager::_bad_temperature_readings);
    StatusUpdateManager::_bad_temperature_readings = 0;
  }

  if (!StatusUpdateManager::_temperature_valid) {
    // Warm-up: the first average is the mean of what is there once a few readings exist, and the unit is known.
    StatusUpdateManager::_warmup_sum += celsius;
    StatusUpdateManager::_warmup_count++;
    if (StatusUpdateManager::_warmup_count < StatusUpdateManager::_TEMPERATURE_WARMUP_READINGS || !StatusUpdateManager::_temperature_unit_known) {
      return;
    }
    double average = StatusUpdateManager::_warmup_sum / StatusUpdateManager::_warmup_count;
    StatusUpdateManager::_measured_average_temperature.set(average);
    ESP_LOGI("StatusUpdateManager", "Temperature: first average %.1f from %d readings.", StatusUpdateManager::_to_display_temperature(average), (int)StatusUpdateManager::_warmup_count);
    StatusUpdateManager::_temperature_valid = true;
    StatusUpdateManager::_post_average_temperature(pdMS_TO_TICKS(250));
    return;
  }

  double smoothed = StatusUpdateManager::_measured_average_temperature.get();
  smoothed += StatusUpdateManager::_TEMPERATURE_EMA_ALPHA * (celsius - smoothed);
  StatusUpdateManager::_measured_average_temperature.set(smoothed);
  StatusUpdateManager::_readings_since_post++;
  double moved = std::fabs(StatusUpdateManager::_to_display_temperature(smoothed) - StatusUpdateManager::_last_posted_temperature.get());
  if (moved >= StatusUpdateManager::_TEMPERATURE_PUBLISH_DELTA || StatusUpdateManager::_readings_since_post >= StatusUpdateManager::_AVERAGE_READINGS) {
    StatusUpdateManager::_post_average_temperature(pdMS_TO_TICKS(250));
  }
}

void StatusUpdateManager::reboot() {
  esp_timer_stop(StatusUpdateManager::_status_update_timer);
  StatusUpdateManager::_status_report.nspanel_state = NSPANEL_STATUS_REPORT__STATE__REBOOTING;
  StatusUpdateManager::_send_status_update(NULL);

  vTaskDelay(pdMS_TO_TICKS(2000)); // Wait two seconds to message to be sent then reboot
  PanelRestart::restart();
}

void StatusUpdateManager::_send_status_update(void *arg) {
#if defined(NSPM_QEMU)
  // Wi-Fi is never initialised under QEMU, and esp_wifi_sta_get_rssi()
  // dereferences driver state that only esp_wifi_init() creates.
  StatusUpdateManager::_status_report.rssi = -50;
#else
  int current_wifi_rssi;
  if (esp_wifi_sta_get_rssi(&current_wifi_rssi) == ESP_OK) [[likely]] {
    StatusUpdateManager::_status_report.rssi = current_wifi_rssi;
  } else {
    ESP_LOGW("StatusUpdateManager", "Failed to get current wifi RSSI. Will use old value.");
  }
#endif

  size_t packed_data_size = 0;
  std::vector<uint8_t> buffer;
  if (xSemaphoreTake(StatusUpdateManager::_status_report_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    float total_heap_size = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    float total_heap_used = total_heap_size - heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

    size_t heap_used_pct = (total_heap_used / total_heap_size) * 100;
    StatusUpdateManager::_status_report.heap_used_pct = heap_used_pct;
    // Kept in a member: ip_string() returns a temporary, which would be freed before the report is
    // packed below (it read as an empty address once built -Os).
    StatusUpdateManager::_ip_address_text = WiFiManager::ip_string();
    StatusUpdateManager::_status_report.ip_address = (char *)StatusUpdateManager::_ip_address_text.c_str();
    // What the display shows, and the look in LVGL mode (docs/LVGL.md). The enum's values are the proto's.
    StatusUpdateManager::_status_report.display_mode = (NSPanelStatusReport__NSPanelDisplayMode)LvglUi::display_mode();
    StatusUpdateManager::_lvgl_theme_text.clear();
    StatusUpdateManager::_lvgl_background_text.clear();
    if (LvglUi::active()) {
      LvglLook::current(&StatusUpdateManager::_lvgl_background_text, &StatusUpdateManager::_lvgl_theme_text);
    }
    StatusUpdateManager::_status_report.lvgl_theme = (char *)StatusUpdateManager::_lvgl_theme_text.c_str();
    StatusUpdateManager::_status_report.lvgl_background = (char *)StatusUpdateManager::_lvgl_background_text.c_str();
    StatusUpdateManager::_status_report.has_temperature = StatusUpdateManager::_temperature_valid;
    StatusUpdateManager::_status_report.temperature = StatusUpdateManager::_temperature_valid ? StatusUpdateManager::current_temperature() : 0;
#if defined(BOARD_CUSTOM)
    StatusUpdateManager::_status_report.humidity = StatusUpdateManager::_measured_average_humidity.get();
    StatusUpdateManager::_status_report.pressure = StatusUpdateManager::_measured_average_pressure.get();
#endif

    // Warnings are rebuilt on every report, so the manager drops one once it no longer applies.
    StatusUpdateManager::_status_report.n_warnings = 0;
    StatusUpdateManager::_status_report.warnings = NULL;
    StatusUpdateManager::_link_baud_warning_text = Nextion::get_link_baud_warning();
    if (!StatusUpdateManager::_link_baud_warning_text.empty()) {
      StatusUpdateManager::_link_baud_warning.text = (char *)StatusUpdateManager::_link_baud_warning_text.c_str();
      StatusUpdateManager::_warnings[StatusUpdateManager::_status_report.n_warnings++] = &StatusUpdateManager::_link_baud_warning;
      StatusUpdateManager::_status_report.warnings = StatusUpdateManager::_warnings;
    }

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
    // One reading is a burst of ADC samples: drop the highest and lowest and average the rest, so a spike
    // (Wi-Fi transmit, the relays, the backlight) does not become a reading.
    int samples_mv[StatusUpdateManager::_ADC_SAMPLES_PER_READING];
    int good_samples = 0;
    const char *failure = "too few good ADC samples";
    for (int i = 0; i < StatusUpdateManager::_ADC_SAMPLES_PER_READING; i++) {
#if defined(NSPM_QEMU)
      // QEMU does not emulate the SAR ADC: a conversion spins forever on a
      // conversion-done bit with the ADC spinlock held, freezing the core.
      // Report the NTC voltage for roughly 22 C instead.
      samples_mv[good_samples++] = 1680;
#else
      int raw;
      int mv;
      if (adc_oneshot_read(StatusUpdateManager::_temp_adc_handle, ADC_CHANNEL_2, &raw) != ESP_OK) {
        failure = "ADC read failed";
        continue;
      }
      if (adc_cali_raw_to_voltage(StatusUpdateManager::_temp_calibration_data, raw, &mv) != ESP_OK) {
        // A task that returns is aborted by the OS and reboots the panel, so this must not return.
        failure = "ADC calibration conversion failed";
        continue;
      }
      samples_mv[good_samples++] = mv;
#endif
    }
    if (good_samples < StatusUpdateManager::_ADC_MIN_GOOD_SAMPLES) {
      StatusUpdateManager::_note_bad_temperature_reading(failure);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    std::sort(samples_mv, samples_mv + good_samples);
    long sum_mv = 0;
    for (int i = 1; i < good_samples - 1; i++) { // Not the lowest and highest.
      sum_mv += samples_mv[i];
    }
    double read_voltage_mv = (double)sum_mv / (good_samples - 2);

    // We now have temperature as a voltage. Convert voltage into celsius:
    if (read_voltage_mv <= StatusUpdateManager::_TEMPERATURE_ADC_MIN_MV || read_voltage_mv >= StatusUpdateManager::_TEMPERATURE_ADC_MAX_MV) {
      StatusUpdateManager::_note_bad_temperature_reading("thermistor voltage near the ADC's rails");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    double read_voltage_v = read_voltage_mv / 1000.0; // Convert mV to V.

    // Calculate temperature from NTC using the Steinhart–Hart equation
    // See https://robertvicol.com/tech/arduino-measuring-temperature-with-ntc-steinhart-hart-formula/ for example
    // Thanks to @atirage in https://github.com/arendst/Tasmota/discussions/15737 for providing reference values.
    double thermistor_ohms = StatusUpdateManager::_THERMISTOR_SERIES_OHMS / ((StatusUpdateManager::_ADC_SUPPLY_VOLTS / read_voltage_v) - 1);
    double steinhart = thermistor_ohms / StatusUpdateManager::_THERMISTOR_R0_OHMS; // (R/Ro)
    steinhart = log(steinhart);                                                    // ln(R/Ro)
    steinhart /= StatusUpdateManager::_THERMISTOR_B;                               // 1/B * ln(R/Ro)
    steinhart += 1.0 / StatusUpdateManager::_THERMISTOR_T0_KELVIN;                 // + (1/To)
    steinhart = 1.0 / steinhart;                                                   // Invert
    double current_temperature = steinhart - 273.15;                               // convert Kelvin to *C

    StatusUpdateManager::_add_temperature_reading(current_temperature);

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

      // ESP_ERROR_CHECK here aborted, rebooting the panel on a single I2C glitch. Count it as a bad reading instead.
      esp_err_t readout = bmx280_readoutFloat(StatusUpdateManager::_bme280_dev_handle, &temperature, &pressure, &humidity);
      if (readout != ESP_OK) {
        StatusUpdateManager::_note_bad_temperature_reading(esp_err_to_name(readout));
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      // Update temperature. The reading stays in Celsius; unit and offset are applied on output.
      StatusUpdateManager::_add_temperature_reading(temperature);

      // Update humidity
      StatusUpdateManager::_measured_humidity[StatusUpdateManager::_measured_humidity_next_index++] = humidity;
      // We only have space for _AVERAGE_READINGS samples.
      if (StatusUpdateManager::_measured_humidity_total_samples < StatusUpdateManager::_AVERAGE_READINGS) {
        StatusUpdateManager::_measured_humidity_total_samples++;
      }

      if (StatusUpdateManager::_measured_humidity_next_index >= StatusUpdateManager::_AVERAGE_READINGS) {
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
      // We only have space for _AVERAGE_READINGS samples.
      if (StatusUpdateManager::_measured_pressure_total_samples < StatusUpdateManager::_AVERAGE_READINGS) {
        StatusUpdateManager::_measured_pressure_total_samples++;
      }

      if (StatusUpdateManager::_measured_pressure_next_index >= StatusUpdateManager::_AVERAGE_READINGS) {
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
      // Logged once, and fails safe like any other run of bad readings, rather than a warning every second.
      StatusUpdateManager::_note_bad_temperature_reading("BME280 sensor was not initialized correctly");

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
    // A new unit or offset changes what the average reads as. Tell the listeners now rather than at the next average.
    if (StatusUpdateManager::_temperature_valid) {
      ESP_LOGI("StatusUpdateManager", "Temperature: unit or offset changed, average is now %.1f.", StatusUpdateManager::current_temperature());
      // Don't wait: this runs on the default event loop, which is the queue being posted to.
      StatusUpdateManager::_post_average_temperature(0);
    }
  } else {
    ESP_LOGE("StatusUpdateManager", "Failed to get config to determine if we should measure temperature in C or F. Will assume C.");
  }
  StatusUpdateManager::_temperature_unit_known = true;
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
    esp_timer_start_periodic(StatusUpdateManager::_status_update_timer, StatusUpdateManager::_STATUS_UPDATE_INTERVAL_US);
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