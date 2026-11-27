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
   * Take one temperature reading in Celsius (no offset). The first average is the mean of the readings so far, once
   * _TEMPERATURE_WARMUP_READINGS exist; after that the average is an exponential moving average. A new value is
   * posted when it has moved by _TEMPERATURE_PUBLISH_DELTA, or after _AVERAGE_READINGS readings.
   */
  static void _add_temperature_reading(double celsius);

  /**
   * Account for a reading that could not be used. Logs when a run of bad readings starts, and after
   * _TEMPERATURE_FAILURE_READINGS in a row clears the valid flag and posts TEMPERATURE_SENSOR_FAILED.
   */
  static void _note_bad_temperature_reading(const char *reason);

  // Post AVERAGE_TEMP_UPDATE with the current average in the display unit. The caller says how long it may wait for room in the event queue.
  static void _post_average_temperature(TickType_t timeout);

  // Time between two status reports.
  static constexpr uint64_t _STATUS_UPDATE_INTERVAL_US = 30ULL * 1000 * 1000;

  // Number of readings the first average waits for.
  static constexpr uint8_t _TEMPERATURE_WARMUP_READINGS = 5;

  // Consecutive bad readings (about one a second) after which the sensor counts as failed.
  static constexpr uint8_t _TEMPERATURE_FAILURE_READINGS = 60;

  // Readings (one a second) in the humidity and pressure buffers, and the longest the temperature goes
  // between two AVERAGE_TEMP_UPDATE events.
  static constexpr uint8_t _AVERAGE_READINGS = 30;

  // Smoothing across readings: each new reading moves the average by this fraction of the difference. At one
  // reading a second, 0.1 is a time constant of about 10 s.
  static constexpr double _TEMPERATURE_EMA_ALPHA = 0.1;

  // Post AVERAGE_TEMP_UPDATE when the average has moved by this much (in the display unit) since the last one.
  static constexpr double _TEMPERATURE_PUBLISH_DELTA = 0.1;

  // The NTC thermistor and its divider (BOARD_SONOFF): 10 kOhm at 25 C, B = 3950, on the ground side of a
  // 10 kOhm series resistor fed from 3.3 V. Not read from the schematic; see TEMP_SENSOR.md.
  static constexpr double _THERMISTOR_R0_OHMS = 10000.0;
  static constexpr double _THERMISTOR_B = 3950.0;
  static constexpr double _THERMISTOR_T0_KELVIN = 25.0 + 273.15;
  static constexpr double _THERMISTOR_SERIES_OHMS = 10000.0;
  static constexpr double _ADC_SUPPLY_VOLTS = 3.3;

  // Thermistor divider voltage, in mV, outside which a reading is taken to be the ADC at a rail (a broken or
  // disconnected thermistor) and not a temperature. About -19 C at the top, and well beyond 60 C at the bottom.
  static constexpr int _TEMPERATURE_ADC_MIN_MV = 500;
  static constexpr int _TEMPERATURE_ADC_MAX_MV = 3000;

  // ADC samples taken in a burst for each reading. The highest and lowest are dropped and the rest averaged. A reading
  // needs at least _ADC_MIN_GOOD_SAMPLES successful ones.
  static constexpr int _ADC_SAMPLES_PER_READING = 24;
  static constexpr int _ADC_MIN_GOOD_SAMPLES = 16;

  // Should temperature be reported in fahrenheit or celsius?
  static inline std::atomic<bool> _measure_temperature_in_fahrenheit = false;

  // Calibration value for the read temperature from the manager, in the display unit.
  static inline std::atomic<float> _temperature_offset_calibration = 0;

  // The current smoothed temperature, in Celsius with no offset.
  static inline MutexWrapped<double> _measured_average_temperature = 0;

  // Whether the unit and offset have been read from the config. The first average waits for it: the unit
  // arrives with the retained config some seconds after boot, and an average in the wrong unit must never be published.
  static inline std::atomic<bool> _temperature_unit_known = false;

  // False until the first average exists.
  static inline std::atomic<bool> _temperature_valid = false;

  // While warming up: the readings so far, in Celsius with no offset.
  static inline double _warmup_sum = 0;
  static inline uint32_t _warmup_count = 0;

  // The average last posted in AVERAGE_TEMP_UPDATE (display unit), and the readings since.
  static inline MutexWrapped<double> _last_posted_temperature = 0;
  static inline uint8_t _readings_since_post = 0;

  // Bad readings in a row, and whether the failed state has been reported.
  static inline uint8_t _bad_temperature_readings = 0;

#if not defined(BOARD_CUSTOM)

  // ADC handle for temperature reading
  static inline adc_oneshot_unit_handle_t _temp_adc_handle;

  // Temperature ADC calibration data
  static inline adc_cali_handle_t _temp_calibration_data;

  // ADC characteristics
  static inline adc_cali_line_fitting_config_t *_adc_chars;

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

};