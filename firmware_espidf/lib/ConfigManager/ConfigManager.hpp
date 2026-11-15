#pragma once
#include <esp_err.h>
#include <esp_log.h>
#include <string>

class ConfigManager {
public:
  /**
   * @brief Load the saved config from LittleFS. This requires that LittleFS has already been mounted.
   * @return ESP_OK if everything was loaded correctly from config. ESP_ERR_NOT_FOUND if config was not found on LittleFS. ESP_ERR_INVALID_STATE if failed to parse JSON.
   */
  static esp_err_t load_config();

  /**
   * Create a default config file. This will not be saved to LittleFS. It simply sets default values.
   */
  static void create_default();

  /**
   * @brief Save the current config values to the config file in LittleFS. This requires that LittleFS has already been mounted.
   * @return ESP_OK if everything was saved correctly, otherwise ESP_ERR_NOT_FINISHED.
   */
  static esp_err_t save_config();

  // Hostname of this NSPanel
  static inline std::string wifi_hostname;

  // WiFi SSID to connect to
  static inline std::string wifi_ssid;

  // The password to the WiFi to connect to
  static inline std::string wifi_psk;

  // IP address to the MQTT server to connect to
  static inline std::string mqtt_server;

  // What port to connect to the MQTT server on
  static inline uint16_t mqtt_port;

  // Username for MQTT server authentication
  static inline std::string mqtt_username;

  // Password for MQTT server authentication
  static inline std::string mqtt_password;

  // Use latest Nextion upload protocol?
  static inline bool use_latest_nextion_upload_protocol;

  // Nextion upload protocol baudrate
  static inline uint32_t nextion_upload_baudrate;

  // Baud rate the panel first connects to the Nextion display at. Must match the display's stored default.
  // Also the fallback: if the display does not answer at operating_baud_rate, it is power-cycled back to this.
  static inline uint32_t communication_baud_rate = 115200;

  // Baud rate to move the display to with baud= once connected, or 0 to stay at communication_baud_rate.
  // Lasts only until the display is next power-cycled, so the display's stored default never changes.
  static inline uint32_t operating_baud_rate = 0;

  // MD5 checksum for installed firmware, used to check if new firmware update is available in manager
  static inline std::string md5_firmware;

  // MD5 checksum for installed data_file (LittleFS), used to check if new data_file update is available in manager
  static inline std::string md5_data_file;

  // MD5 checksum for installed gui, used to check if new gui update is available in manager
  static inline std::string md5_gui;

  // Should relays be reversed as though the cables on the back switched places?
  static inline bool reverse_relays = false;

  // Should relay 1 be on or off on boot?
  static inline bool relay1_default_mode = false;

  // Should relay 2 be on or off on boot?
  static inline bool relay2_default_mode = false;

  // Relay1 is thermostat heat mode
  static inline bool relay1_thermostat_heat_mode = false;

  // Relay1 is thermostat cool mode
  static inline bool relay1_thermostat_cool_mode = false;

  // Relay 1 on temperature (heating) or off temperature (cooling)
  static inline uint16_t relay1_lower_temperature = 0;

  // Relay 1 off temperature (heating) or on temperature (cooling)
  static inline uint16_t relay1_upper_temperature = 0;

  // Relay2 is thermostat heat mode
  static inline bool relay2_thermostat_heat_mode = false;

  // Relay2 is thermostat cool mode
  static inline bool relay2_thermostat_cool_mode = false;

  // Relay 1 on temperature (heating) or off temperature (cooling)
  static inline uint16_t relay2_lower_temperature = 0;

  // Relay 1 off temperature (heating) or on temperature (cooling)
  static inline uint16_t relay2_upper_temperature = 0;

  // Milliseconds to debounce button/button has to be held for before registering as a press.
  static inline uint32_t min_button_push_time = 50;

  // Milliseconds a button has to be held for before registering as a long press/hold press.
  static inline uint32_t min_button_long_push_time = 300;

  // Used to indicate that the stored checksum for the firmware should be updated on next reboot.
  static inline bool has_updated;

  // MD5 checksum of the firmware that was flashed, promoted to md5_firmware on next boot when has_updated is set.
  static inline std::string md5_firmware_pending;

  // Global log level to set for all components
  static inline esp_log_level_t log_level = esp_log_level_t::ESP_LOG_DEBUG;

  // When log lines are also written to the serial console. Writing to UART0 at 115200 baud takes
  // about 9ms per line on whichever task logs, so once lines reach the manager over MQTT it is skipped.
  enum class SerialLogMode : uint8_t {
    AUTO = 0,   // Only while not connected to MQTT, i.e. while lines cannot reach the manager.
    ALWAYS = 1, // Every line, for development with a serial cable attached.
    NEVER = 2,  // Never.
  };
  static inline SerialLogMode serial_log = SerialLogMode::AUTO;

  // Whether the LVGL screens are laid out for a US (portrait) panel. Taken from the manager's
  // config (is_us_panel) and kept here, so the first screen after a boot has the right shape.
  static inline bool lvgl_us_layout = true;
  // LVGL as the panel's whole UI when the display runs an nspanel-terminal TFT (LvglUi). false keeps
  // the Nextion pages whatever the TFT: a kill switch that needs no TFT reinstall.
  static inline bool lvgl_ui = true;
  // The page background preset with LVGL as the UI (Background.hpp): "none", "ocean", "dusk", "aurora" or "peaks".
  static inline std::string lvgl_background = "none";
  // The LVGL screens' palette (Theme.hpp): "auto" (the background preset's own, dark without one),
  // "dark", "light", "ocean", "dusk", "aurora" or "peaks".
  static inline std::string lvgl_theme = "auto";
};