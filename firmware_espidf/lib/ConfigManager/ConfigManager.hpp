#pragma once
#include <ConfigFileState.hpp>
#include <ConfigSaveCaller.hpp>
#include <cstdint>
#include <esp_err.h>
#include <esp_log.h>
#include <string>
#include <utility>
#include <vector>

class ConfigManager {
public:
  /**
   * @brief Load the saved config from LittleFS. This requires that LittleFS has already been mounted.
   * @return ESP_OK if everything was loaded correctly from config. ESP_ERR_NOT_FOUND if there is no config file.
   * ESP_ERR_NOT_FINISHED if there is a file but it could not be used: empty, a short read, out of memory, or bad JSON.
   */
  static esp_err_t load_config();

  /**
   * @brief What app_main() does after load_config() returned `load_result`: only a missing file is replaced by
   * the defaults. Any other failure may be transient (the read buffer's malloc), so the file is left alone.
   */
  static ConfigFileState::BootAction boot_action(esp_err_t load_result);

  /**
   * Create a default config file. This will not be saved to LittleFS. It simply sets default values.
   */
  static void create_default();

  /**
   * @brief Save the current config values to the config file in LittleFS. This requires that LittleFS has already been mounted.
   *
   * Any task may call it. Saves are serialised by a lock (waited for up to SAVE_LOCK_TIMEOUT_MS), the JSON is built
   * before the file is opened, and a save that would write exactly what the file already holds touches no flash.
   * It does not wait on the network, the display or any other lock, but a real write costs tens of ms of flash time
   * on the calling task: call it only when something changed, and never from an esp_timer callback.
   *
   * @param replace_unreadable: replace config.json even if it could not be read at boot. Only for saves the user
   * asked for (the web UI's save and factory reset); every other save is refused then, so that a file that failed
   * to load for a transient reason is not lost to the defaults.
   * @return ESP_OK if the file holds the current config (written, or already identical). ESP_ERR_TIMEOUT if the lock
   * was not free in time, ESP_ERR_INVALID_STATE if refused as above, ESP_ERR_NOT_FINISHED if the write failed.
   */
  static esp_err_t save_config(ConfigSaveCaller caller, bool replace_unreadable = false);

  /**
   * @brief Change config fields and save them, both under the save lock, so that no other task's save serialises
   * a string or the timers half-way through an assignment. Use it for the std::string and std::vector fields
   * changed after boot. `change` must only assign fields: it must not wait, log at length, or call out.
   *
   * If the lock is not free in time, `change` still runs (so the running panel uses the new values, as before)
   * and ESP_ERR_TIMEOUT is returned without saving.
   */
  template <typename F> static esp_err_t change(ConfigSaveCaller caller, F &&change, bool replace_unreadable = false) {
    if (!ConfigManager::_lock()) {
      ESP_LOGE("ConfigManager", "Config lock busy for %d ms. Changed the config without saving it.", SAVE_LOCK_TIMEOUT_MS);
      std::forward<F>(change)();
      ConfigManager::_count_lock_timeout(caller);
      return ESP_ERR_TIMEOUT;
    }
    std::forward<F>(change)();
    esp_err_t result = ConfigManager::_save_locked(caller, replace_unreadable);
    ConfigManager::_unlock();
    return result;
  }

  // The longest a save waits for another task's save to finish. See ConfigManager.cpp.
  static constexpr int SAVE_LOCK_TIMEOUT_MS = 1000;

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

  // MD5 of the LittleFS image this panel was last updated with. Still loaded and saved, so that rolling back to an
  // older firmware reports a real value, but no longer reported: the web UI is in the firmware (NSPM_WEB_UI_EMBEDDED).
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
  // The page background preset with LVGL as the UI (Background.hpp): "none", "ocean", "dusk", "aurora", "peaks", "vesuvius" or "parthenon".
  static inline std::string lvgl_background = "none";
  // The LVGL screens' palette (Theme.hpp): "auto" (the background preset's own, dark without one),
  // "dark", "grey" ("light" is read as "grey"), "ocean", "dusk", "aurora", "peaks", "vesuvius" or "parthenon".
  static inline std::string lvgl_theme = "auto";
  // The look above was chosen on the panel (or by a command) and the manager has not confirmed it
  // yet: a config with another look is not applied, and the look is reported again (LvglLook).
  static inline bool lvgl_look_unconfirmed = false;

  // EventLoopWatchdog: log every task's backtrace if the default event loop stops dispatching. Off by default (it
  // costs a task and its stack); the loop_watchdog command turns it on and off without a reboot.
  static inline bool loop_watchdog = false;

  // A timer running when the panel last saved (TimerManager), kept across a reboot. deadline_epoch_s is the
  // wall-clock deadline. Saved as the "timers" array of {duration_s, deadline_epoch_s}.
  struct PersistedTimer {
    uint32_t duration_s;
    int64_t deadline_epoch_s;
  };
  static inline std::vector<PersistedTimer> timers;

private:
  // The save lock: recursive, so that change() can save while holding it. Bounded by SAVE_LOCK_TIMEOUT_MS.
  static bool _lock();
  static void _unlock();
  // save_config() with the lock held.
  static esp_err_t _save_locked(ConfigSaveCaller caller, bool replace_unreadable);
  // Counts a save that gave up waiting for the lock (here, so the header needs no PanelMetrics).
  static void _count_lock_timeout(ConfigSaveCaller caller);
  // The config as save_config() writes it.
  static std::string _serialise();
  // What config.json holds, as far as this boot knows. Under the save lock.
  static inline ConfigFileState _file_state;
};
