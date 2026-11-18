#pragma once
#include <MutexWrapper.hpp>
#include <atomic>
#include <driver/i2c_master.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>
#include <protobuf_nspanel.pb-c.h>
#include <string>
#include <vector>

#if defined(BOARD_CUSTOM)
#include <bmx280.h>
#endif

class StatusUpdateManager {
public:
  /**
   * Initialize the StatusUpdateManager to setup logging and start timers
   */
  static void init();

  // The current average temperature in the display unit (Fahrenheit or Celsius per the config), with the calibration
  // offset applied. Only meaningful when has_valid_temperature() is true.
  static float current_temperature();

  // Whether the sensor has produced a usable average yet. False from boot until the first few readings are in.
  static bool has_valid_temperature();

  // Send the reboot state over MQTT and then reboot.
  static void reboot();

private:
  /**
   * Create a NSPanelStatusReport protobuf object and fill in all data. Then send it out over MQTT.
   */
  static void _send_status_update(void *arg);

  /**
   * Measure the temperature periodically to create a moving average
   */
  static void _measure_temperature(void *arg);

  /**
   * Update internal values from loaded config
   */
  static void _update_from_config();

  /**
   * Handle events such as new config loaded
   */
  static void _event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Handle firmware/littlefs/nextion update events
   */
  static void _update_manager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  // Vars:
  // Handle to timer responsible for sending status updates periodically
  static inline esp_timer_handle_t _status_update_timer;

  /**
   * Convert a temperature in Celsius to the display unit and apply the calibration offset. The offset is in the
   * display unit and is applied after the conversion. The readings themselves are always kept in Celsius without
   * offset, so a change of unit or offset takes effect on the next output with no mixed window.
   */
  static double _to_display_temperature(double celsius);

  /**
   * Store one temperature reading in Celsius (no offset) and publish a new average when due: once
   * _TEMPERATURE_WARMUP_READINGS readings exist, then every _TEMPERATURE_AVERAGE_READINGS readings.
   */
  static void _add_temperature_reading(double celsius);

  /**
   * Account for a reading that could not be used. Logs when a run of bad readings starts, and after
   * _TEMPERATURE_FAILURE_READINGS in a row clears the valid flag and posts TEMPERATURE_SENSOR_FAILED.
   */
  static void _note_bad_temperature_reading(const char *reason);

  // Post AVERAGE_TEMP_UPDATE with the current average in the display unit. The caller says how long it may wait for room in the event queue.
  static void _post_average_temperature(TickType_t timeout);

  // Number of readings the first average waits for.
  static constexpr uint8_t _TEMPERATURE_WARMUP_READINGS = 5;

  // Consecutive bad readings (about one a second) after which the sensor counts as failed.
  static constexpr uint8_t _TEMPERATURE_FAILURE_READINGS = 60;

  // Thermistor divider voltage, in mV, outside which a reading is taken to be the ADC at a rail (a broken or
  // disconnected thermistor) and not a temperature. About -19 C at the top, and well beyond 60 C at the bottom.
  static constexpr int _TEMPERATURE_ADC_MIN_MV = 500;
  static constexpr int _TEMPERATURE_ADC_MAX_MV = 3000;

  // Size of the reading buffer, and the number of readings between two averages once warmed up.
  static constexpr uint8_t _TEMPERATURE_AVERAGE_READINGS = 30;

  // Should temperature be reported in fahrenheit or celsius?
  static inline std::atomic<bool> _measure_temperature_in_fahrenheit = false;

  // Calibration value for the read temperature from the manager, in the display unit.
  static inline std::atomic<float> _temperature_offset_calibration = 0;

  // The current average of the readings, in Celsius with no offset.
  static inline MutexWrapped<double> _measured_average_temperature = 0;

  // Whether the unit and offset have been read from the config. The first average waits for it: the unit
  // arrives with the retained config some seconds after boot, and an average in the wrong unit must never be published.
  static inline std::atomic<bool> _temperature_unit_known = false;

  // False until the first average exists.
  static inline std::atomic<bool> _temperature_valid = false;

  // Records of measured temperatures from _measure_temperature function, in Celsius with no offset.
  static inline float _measured_temperatures[_TEMPERATURE_AVERAGE_READINGS];

  // What index is the next index to insert/replace read temperature into.
  static inline uint8_t _measured_temperature_next_index;

  // How many readings have been read from the temperature sensor (at most the buffer size)
  static inline uint8_t _measured_temperature_total_samples;

  // Bad readings in a row, and whether the failed state has been reported.
  static inline uint8_t _bad_temperature_readings = 0;

  // Readings taken since the last average was published.
  static inline uint8_t _readings_since_average;

#if not defined(BOARD_CUSTOM)

  // ADC handle for temperature reading
  static inline adc_oneshot_unit_handle_t _temp_adc_handle;

  // Temperature ADC calibration data
  static inline adc_cali_handle_t _temp_calibration_data;

  // ADC characteristics
  static inline adc_cali_line_fitting_config_t *_adc_chars;

#endif

#if defined(BOARD_CUSTOM)
  // What is the current average measured temperature
  static inline MutexWrapped<double> _measured_average_humidity = 0;

  // Records of measured humidity from _measure_temperature function.
  static inline float _measured_humidity[30];

  // What index is the next index to insert/replace read humidity into.
  static inline uint8_t _measured_humidity_next_index;

  // How many samples has been read from the pressure sensor
  static inline uint8_t _measured_humidity_total_samples;

  // What is the current average measured temperature
  static inline MutexWrapped<double> _measured_average_pressure = 0;

  // Records of measured pressure from _measure_temperature function.
  static inline float _measured_pressure[30];

  // What index is the next index to insert/replace read pressure into.
  static inline uint8_t _measured_pressure_next_index;

  // How many samples has been read from the humidity sensor
  static inline uint8_t _measured_pressure_total_samples;

#endif

  // Mutex to only allow once task at the time access to the _status_report.
  static inline SemaphoreHandle_t _status_report_mutex;

  // Status report object used to send protobuf data to manager
  static inline NSPanelStatusReport _status_report;

  // Warning sent in _status_report when the display is not at its operating baud rate. Guarded by
  // _status_report_mutex; the text must outlive packing, since the protobuf only points at it.
  static inline NSPanelWarning _link_baud_warning;
  static inline std::string _link_baud_warning_text;
  // Holds the text _status_report.ip_address points at while the report is packed.
  static inline std::string _ip_address_text;
  // The LVGL look in the report (empty outside LVGL mode), kept for the same reason.
  static inline std::string _lvgl_theme_text;
  static inline std::string _lvgl_background_text;
  static inline NSPanelWarning *_warnings[1];

#if defined(BOARD_CUSTOM)
  // Custom PCB that uses I2C sensors. Define them:

  static bool _initialize_bme280();

  // I2C Master bus handle
  static inline i2c_master_bus_handle_t _i2c_master_bus_handle;

  // Was the BME280 sensor initialized correctly
  static inline bool _bme280_initialized = false;

  // Time of last try to initialize the BME280 sensor
  static inline uint64_t _last_bme280_init_try = 0;

  // Device handle for BME280 sensor on I2C bus.
  static inline bmx280_t *_bme280_dev_handle = NULL;

  // static bool _initialize_ltr303();

  // // Was the LTR303 sensor initialized correctly
  // static inline bool _ltr303_initialized = false;

  // // Time of last try to initialize the LTR303 sensor
  // static inline uint64_t _last_ltr303_init_try = 0;

  // // Device handle for LTR-303ALS-01 sensor on I2C bus.
  // static inline LTR303 *_ltr303_dev_handle = NULL;
#endif
};