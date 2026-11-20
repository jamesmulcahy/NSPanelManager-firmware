#include "ConfigManager.hpp"
#include <LittleFS.hpp>
#include <PanelMetrics.hpp>
#include <errno.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nlohmann/json.hpp>
#include <sdkconfig.h>
#include <string.h>

// save_config() opens config.json with "w" and rewrites it in place, with no temporary file and rename. That is
// only safe because esp_littlefs commits nothing until fclose(): O_TRUNC just marks the open file dirty in RAM,
// the new contents and the truncation reach the metadata together in lfs_file_sync() on close, and a write
// error skips that sync, so a power cut or a failed write leaves the old file whole. Flushing on every write
// commits the truncated file at the first fwrite(), and a cut after it would leave config.json empty or short.
#if CONFIG_LITTLEFS_FLUSH_FILE_EVERY_WRITE
#error "CONFIG_LITTLEFS_FLUSH_FILE_EVERY_WRITE makes ConfigManager::save_config()'s in-place rewrite unsafe: switch it off, or save to a temporary file and rename it over config.json."
#endif

static const char *TAG = "ConfigManager";
static const char *CONFIG_PATH = "/littlefs/config.json";

// The save lock. Created on first use (a function-local static, which C++ initialises once even with several
// tasks racing), so that it exists however early the first load or save comes.
static SemaphoreHandle_t config_lock() {
  static StaticSemaphore_t buffer;
  static SemaphoreHandle_t handle = xSemaphoreCreateRecursiveMutexStatic(&buffer);
  return handle;
}

// SAVE_LOCK_TIMEOUT_MS (1 s): the lock is held only for one save, which serialises about 1 KB of JSON and writes
// it to LittleFS. That waits on nothing but the flash: no network, no display, no other component's lock. On the
// panel a write takes tens of ms, and one that has to erase a block or compact LittleFS's metadata stays within
// a few hundred. A second waits well past that, so a timeout means a save is stuck, not merely slow, while still
// bounding how long a default-loop or MQTT handler can stall behind it far below the Wi-Fi and MQTT keepalive
// margins (and LoopWatchdog's 15 s). A save that times out returns ESP_ERR_TIMEOUT and logs; the values stay in
// RAM and the next save writes them.
bool ConfigManager::_lock() {
  return xSemaphoreTakeRecursive(config_lock(), pdMS_TO_TICKS(SAVE_LOCK_TIMEOUT_MS)) == pdTRUE;
}

void ConfigManager::_unlock() {
  xSemaphoreGiveRecursive(config_lock());
}

ConfigFileState::BootAction ConfigManager::boot_action(esp_err_t load_result) {
  ConfigFileState::LoadOutcome outcome = load_result == ESP_OK               ? ConfigFileState::LoadOutcome::LOADED
                                         : load_result == ESP_ERR_NOT_FOUND ? ConfigFileState::LoadOutcome::MISSING
                                                                             : ConfigFileState::LoadOutcome::UNREADABLE;
  PanelMetrics::config_load_state.set(outcome == ConfigFileState::LoadOutcome::LOADED ? 0 : outcome == ConfigFileState::LoadOutcome::MISSING ? 1 : 2);
  return ConfigFileState::boot_action(outcome);
}

esp_err_t ConfigManager::load_config() {
  FILE *f = fopen(CONFIG_PATH, "r");
  if (f == NULL) {
    if (errno == ENOENT) {
      ESP_LOGE("ConfigManager", "No /littlefs/config.json to load.");
      ConfigManager::_file_state.missing();
      return ESP_ERR_NOT_FOUND;
    }
    // LittleFS not mounted, out of memory, an I/O error: the file may well be there.
    ESP_LOGE("ConfigManager", "Failed to open /littlefs/config.json for reading: %s.", strerror(errno));
    ConfigManager::_file_state.unreadable();
    return ESP_ERR_NOT_FINISHED;
  }

  // Get file size.
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *read_buffer = NULL;
  if (file_size > 0) {
    // One extra byte for the null terminator, cJSON_Parse expects a null-terminated string.
    read_buffer = (char *)malloc(file_size + 1);
    if (read_buffer == NULL) {
      ESP_LOGE("ConfigManager", "Failed to allocate %ld bytes for read buffer while reading config from LittleFS.", file_size + 1);
      fclose(f);
      ConfigManager::_file_state.unreadable();
      return ESP_ERR_NOT_FINISHED;
    }
  } else {
    ESP_LOGE("ConfigManager", "Returned invalid file size %ld of config file.", file_size);
    fclose(f);
    ConfigManager::_file_state.unreadable();
    return ESP_ERR_NOT_FINISHED;
  }

  size_t bytes_read = fread(read_buffer, 1, file_size, f);
  fclose(f);
  if (bytes_read != (size_t)file_size) {
    ESP_LOGE("ConfigManager", "Read %zu of %ld bytes from config file.", bytes_read, file_size);
    free(read_buffer);
    ConfigManager::_file_state.unreadable();
    return ESP_ERR_NOT_FINISHED;
  }
  read_buffer[bytes_read] = '\0';

  nlohmann::json config_data = nlohmann::json::parse(read_buffer, read_buffer + file_size, nullptr, false);
  free(read_buffer); // Data parsed, free read buffer.

  if (config_data.is_discarded()) {
    ESP_LOGE("ConfigManager", "Failed to parse JSON while reading config from LittleFS!");
    ConfigManager::_file_state.unreadable();
    return ESP_ERR_NOT_FINISHED;
  }

  if (config_data.contains("log_level") && config_data["log_level"].is_number_integer()) {
    ConfigManager::log_level = static_cast<esp_log_level_t>(config_data.at("log_level").get<int32_t>());
    // One level for everything (CONFIG_LOG_TAG_LEVEL_IMPL_NONE): esp_log_level_set() with any tag sets
    // it. ESP-IDF's components are capped at INFO at compile time (CMakeLists.txt).
    esp_log_level_set("*", ConfigManager::log_level);
    ESP_LOGE("ConfigManager", "Loaded log level: %d", config_data.at("log_level").get<int32_t>());
  } else {
    ESP_LOGE("ConfigManager", "Log level is not of type int!");
  }

  // Absent from configs saved by older firmware, which then keeps the AUTO default.
  if (config_data.contains("serial_log") && config_data["serial_log"].is_number_integer()) {
    // Read wider than the enum's uint8_t and range-check before narrowing: get<uint8_t>() would
    // wrap a corrupt value (258, -254) into a valid mode before the check could catch it.
    int32_t serial_log = config_data["serial_log"].get<int32_t>();
    if (serial_log >= 0 && serial_log <= static_cast<int32_t>(ConfigManager::SerialLogMode::NEVER)) {
      ConfigManager::serial_log = static_cast<ConfigManager::SerialLogMode>(serial_log);
    } else {
      ESP_LOGW("ConfigManager", "Ignoring invalid serial_log value %ld in config, keeping Auto.", (long)serial_log);
    }
  }

  // Handle WiFi settings
  if (config_data.contains("wifi_hostname") && config_data["wifi_hostname"].is_string()) {
    ConfigManager::wifi_hostname = config_data["wifi_hostname"].get<std::string>();
  }

  if (config_data.contains("wifi_ssid") && config_data["wifi_ssid"].is_string()) {
    ConfigManager::wifi_ssid = config_data["wifi_ssid"].get<std::string>();
  }

  if (config_data.contains("wifi_psk") && config_data["wifi_psk"].is_string()) {
    ConfigManager::wifi_psk = config_data["wifi_psk"].get<std::string>();
  }

  // Handle MQTT settings
  if (config_data.contains("mqtt_server") && config_data["mqtt_server"].is_string()) {
    ConfigManager::mqtt_server = config_data["mqtt_server"].get<std::string>();
  }

  if (config_data.contains("mqtt_port") && config_data["mqtt_port"].is_number_integer()) {
    ConfigManager::mqtt_port = config_data["mqtt_port"].get<int32_t>();
  }

  if (config_data.contains("mqtt_username") && config_data["mqtt_username"].is_string()) {
    ConfigManager::mqtt_username = config_data["mqtt_username"].get<std::string>();
  }

  if (config_data.contains("mqtt_password") && config_data["mqtt_password"].is_string()) {
    ConfigManager::mqtt_password = config_data["mqtt_password"].get<std::string>();
  }

  // Handle MD5 checksums
  if (config_data.contains("md5_firmware") && config_data["md5_firmware"].is_string()) {
    ConfigManager::md5_firmware = config_data["md5_firmware"].get<std::string>();
  }

  if (config_data.contains("md5_data_file") && config_data["md5_data_file"].is_string()) {
    ConfigManager::md5_data_file = config_data["md5_data_file"].get<std::string>();
  }

  if (config_data.contains("md5_gui") && config_data["md5_gui"].is_string()) {
    ConfigManager::md5_gui = config_data["md5_gui"].get<std::string>();
  }

  // Handle relay settings
  if (config_data.contains("reverse_relays") && config_data["reverse_relays"].is_boolean()) {
    ConfigManager::reverse_relays = config_data["reverse_relays"].get<bool>();
  }

  if (config_data.contains("relay1_default_mode") && config_data["relay1_default_mode"].is_boolean()) {
    ConfigManager::relay1_default_mode = config_data["relay1_default_mode"].get<bool>();
  }

  if (config_data.contains("relay2_default_mode") && config_data["relay2_default_mode"].is_boolean()) {
    ConfigManager::relay2_default_mode = config_data["relay2_default_mode"].get<bool>();
  }

  // Handle upload protocol
  if (config_data.contains("use_new_upload_protocol") && config_data["use_new_upload_protocol"].is_boolean()) {
    ConfigManager::use_latest_nextion_upload_protocol = config_data["use_new_upload_protocol"].get<bool>();
  } else if (config_data.contains("use_new_upload_protocol") && config_data["use_new_upload_protocol"].is_string()) {
    ConfigManager::use_latest_nextion_upload_protocol =
        (strncmp("true", config_data["use_new_upload_protocol"].get<std::string>().c_str(), sizeof("true")) == 0);
  }

  // Handle has_updated
  if (config_data.contains("has_updated") && config_data["has_updated"].is_boolean()) {
    ConfigManager::has_updated = config_data["has_updated"].get<bool>();
  }

  // Handle baud rates
  if (config_data.contains("upload_baud") && config_data["upload_baud"].is_number_integer()) {
    ConfigManager::nextion_upload_baudrate = config_data["upload_baud"].get<int32_t>();
  }

  if (config_data.contains("md5_firmware_pending") && config_data["md5_firmware_pending"].is_string()) {
    ConfigManager::md5_firmware_pending = config_data["md5_firmware_pending"].get<std::string>();
  }

  if (config_data.contains("comms_baud") && config_data["comms_baud"].is_number_integer()) {
    ConfigManager::communication_baud_rate = config_data["comms_baud"].get<int32_t>();
  }

  if (config_data.contains("operating_baud") && config_data["operating_baud"].is_number_integer()) {
    ConfigManager::operating_baud_rate = config_data["operating_baud"].get<int32_t>();
  }

  if (config_data.contains("lvgl_us_layout") && config_data["lvgl_us_layout"].is_boolean()) {
    ConfigManager::lvgl_us_layout = config_data["lvgl_us_layout"].get<bool>();
  }

  if (config_data.contains("lvgl_ui") && config_data["lvgl_ui"].is_boolean()) {
    ConfigManager::lvgl_ui = config_data["lvgl_ui"].get<bool>();
  }

  if (config_data.contains("lvgl_background") && config_data["lvgl_background"].is_string()) {
    ConfigManager::lvgl_background = config_data["lvgl_background"].get<std::string>();
  }

  if (config_data.contains("lvgl_theme") && config_data["lvgl_theme"].is_string()) {
    ConfigManager::lvgl_theme = config_data["lvgl_theme"].get<std::string>();
  }

  ConfigManager::timers.clear();
  if (config_data.contains("timers") && config_data["timers"].is_array()) {
    for (const auto &entry : config_data["timers"]) {
      // Skip malformed entries rather than failing the whole config.
      if (!entry.is_object() || !entry.contains("duration_s") || !entry["duration_s"].is_number_integer() || !entry.contains("deadline_epoch_s") ||
          !entry["deadline_epoch_s"].is_number_integer()) {
        continue;
      }
      int64_t duration_s = entry["duration_s"].get<int64_t>();
      int64_t deadline_epoch_s = entry["deadline_epoch_s"].get<int64_t>();
      if (duration_s < 1 || duration_s > UINT32_MAX || deadline_epoch_s <= 0) {
        continue;
      }
      ConfigManager::timers.push_back({static_cast<uint32_t>(duration_s), deadline_epoch_s});
    }
  }

  if (config_data.contains("lvgl_look_unconfirmed") && config_data["lvgl_look_unconfirmed"].is_boolean()) {
    ConfigManager::lvgl_look_unconfirmed = config_data["lvgl_look_unconfirmed"].get<bool>();
  }

  if (config_data.contains("loop_watchdog") && config_data["loop_watchdog"].is_boolean()) {
    ConfigManager::loop_watchdog = config_data["loop_watchdog"].get<bool>();
  }

  // Handle thermostat modes
  if (config_data.contains("relay1_is_thermostat_heat_mode") && config_data["relay1_is_thermostat_heat_mode"].is_boolean()) {
    ConfigManager::relay1_thermostat_heat_mode = config_data["relay1_is_thermostat_heat_mode"].get<bool>();
  }

  if (config_data.contains("relay1_is_thermostat_cool_mode") && config_data["relay1_is_thermostat_cool_mode"].is_boolean()) {
    ConfigManager::relay1_thermostat_cool_mode = config_data["relay1_is_thermostat_cool_mode"].get<bool>();
  }

  if (config_data.contains("relay2_is_thermostat_heat_mode") && config_data["relay2_is_thermostat_heat_mode"].is_boolean()) {
    ConfigManager::relay2_thermostat_heat_mode = config_data["relay2_is_thermostat_heat_mode"].get<bool>();
  }

  if (config_data.contains("relay2_is_thermostat_cool_mode") && config_data["relay2_is_thermostat_cool_mode"].is_boolean()) {
    ConfigManager::relay2_thermostat_cool_mode = config_data["relay2_is_thermostat_cool_mode"].get<bool>();
  }

  // Handle temperature thresholds
  if (config_data.contains("relay1_lower_temperature") && config_data["relay1_lower_temperature"].is_number_integer()) {
    ConfigManager::relay1_lower_temperature = config_data["relay1_lower_temperature"].get<int32_t>();
  }

  if (config_data.contains("relay1_upper_temperature") && config_data["relay1_upper_temperature"].is_number_integer()) {
    ConfigManager::relay1_upper_temperature = config_data["relay1_upper_temperature"].get<int32_t>();
  }

  if (config_data.contains("relay2_lower_temperature") && config_data["relay2_lower_temperature"].is_number_integer()) {
    ConfigManager::relay2_lower_temperature = config_data["relay2_lower_temperature"].get<int32_t>();
  }

  if (config_data.contains("relay2_upper_temperature") && config_data["relay2_upper_temperature"].is_number_integer()) {
    ConfigManager::relay2_upper_temperature = config_data["relay2_upper_temperature"].get<int32_t>();
  }

  // What a save of the values just loaded would write: a later save of the same values is then skipped. Fields
  // the file lacks (an older firmware's) or that failed their type checks read back as their defaults, which is
  // also what any save writes, so the comparison is against what the panel runs with, not the file's exact bytes.
  if (ConfigManager::_lock()) {
    ConfigManager::_file_state.loaded(ConfigManager::_serialise());
    ConfigManager::_unlock();
  }
  return ESP_OK;
}

void ConfigManager::create_default() {
  // Load MAC-address. Last three bytes are specific to this device and will be used as a unique ID.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA); // Read MAC address for Wi-Fi Station
  char mac_str[7];                     // Format: XXXXXX
  snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X", mac[3], mac[4], mac[5]);

  ConfigManager::wifi_hostname = "NSPMPanel-";
  ConfigManager::wifi_hostname.append(mac_str);

  ConfigManager::wifi_ssid = "";
  ConfigManager::wifi_psk = "";

  ConfigManager::mqtt_server = "";
  ConfigManager::mqtt_port = 1883;
  ConfigManager::mqtt_username = "";
  ConfigManager::mqtt_password = "";

  ConfigManager::has_updated = false;
  ConfigManager::md5_firmware_pending = "";
  ConfigManager::md5_firmware = "";
  ConfigManager::md5_data_file = "";
  ConfigManager::md5_gui = "";

  ConfigManager::use_latest_nextion_upload_protocol = true;
  ConfigManager::nextion_upload_baudrate = 115200;
  ConfigManager::communication_baud_rate = 115200;
  ConfigManager::operating_baud_rate = 0;
  ConfigManager::log_level = ESP_LOG_WARN;
  ConfigManager::serial_log = ConfigManager::SerialLogMode::AUTO;
  ConfigManager::lvgl_us_layout = true;
  ConfigManager::lvgl_ui = true;
  ConfigManager::lvgl_background = "none";
  ConfigManager::lvgl_theme = "auto";
  ConfigManager::lvgl_look_unconfirmed = false;
  ConfigManager::loop_watchdog = false;
  ConfigManager::timers.clear();
}

std::string ConfigManager::_serialise() {
  nlohmann::json json = nlohmann::json::object();
  json["log_level"] = static_cast<int>(ConfigManager::log_level);
  json["serial_log"] = static_cast<int>(ConfigManager::serial_log);
  json["lvgl_us_layout"] = ConfigManager::lvgl_us_layout;
  json["lvgl_ui"] = ConfigManager::lvgl_ui;
  json["lvgl_background"] = ConfigManager::lvgl_background;
  json["lvgl_theme"] = ConfigManager::lvgl_theme;
  json["lvgl_look_unconfirmed"] = ConfigManager::lvgl_look_unconfirmed;
  json["loop_watchdog"] = ConfigManager::loop_watchdog;
  json["timers"] = nlohmann::json::array();
  for (const auto &timer : ConfigManager::timers) {
    json["timers"].push_back({{"duration_s", timer.duration_s}, {"deadline_epoch_s", timer.deadline_epoch_s}});
  }
  json["wifi_hostname"] = ConfigManager::wifi_hostname;
  json["wifi_ssid"] = ConfigManager::wifi_ssid;
  json["wifi_psk"] = ConfigManager::wifi_psk;
  json["mqtt_server"] = ConfigManager::mqtt_server;
  json["mqtt_port"] = ConfigManager::mqtt_port;
  json["mqtt_username"] = ConfigManager::mqtt_username;
  json["mqtt_password"] = ConfigManager::mqtt_password;
  json["md5_firmware"] = ConfigManager::md5_firmware;
  json["md5_firmware_pending"] = ConfigManager::md5_firmware_pending;
  json["md5_data_file"] = ConfigManager::md5_data_file;
  json["md5_gui"] = ConfigManager::md5_gui;
  json["reverse_relays"] = ConfigManager::reverse_relays;
  json["relay1_default_mode"] = ConfigManager::relay1_default_mode;
  json["relay2_default_mode"] = ConfigManager::relay2_default_mode;
  json["relay1_is_thermostat_heat_mode"] = ConfigManager::relay1_thermostat_heat_mode;
  json["relay1_is_thermostat_cool_mode"] = ConfigManager::relay1_thermostat_cool_mode;
  json["relay2_is_thermostat_heat_mode"] = ConfigManager::relay2_thermostat_heat_mode;
  json["relay2_is_thermostat_cool_mode"] = ConfigManager::relay2_thermostat_cool_mode;
  json["relay1_lower_temperature"] = ConfigManager::relay1_lower_temperature;
  json["relay1_upper_temperature"] = ConfigManager::relay1_upper_temperature;
  json["relay2_lower_temperature"] = ConfigManager::relay2_lower_temperature;
  json["relay2_upper_temperature"] = ConfigManager::relay2_upper_temperature;

  json["use_new_upload_protocol"] = ConfigManager::use_latest_nextion_upload_protocol;
  json["has_updated"] = ConfigManager::has_updated;

  json["upload_baud"] = ConfigManager::nextion_upload_baudrate;
  json["comms_baud"] = ConfigManager::communication_baud_rate;
  json["operating_baud"] = ConfigManager::operating_baud_rate;

  // replace, not the default strict: exceptions are off, so a string that is not valid UTF-8 (a panel name from
  // the manager) would abort the panel in dump().
  return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

esp_err_t ConfigManager::save_config(bool replace_unreadable) {
  if (!ConfigManager::_lock()) {
    ESP_LOGE(TAG, "Config lock busy for %d ms. Not saving.", SAVE_LOCK_TIMEOUT_MS);
    PanelMetrics::config_saves[PanelMetrics::CONFIG_SAVE_LOCK_TIMEOUT].add();
    return ESP_ERR_TIMEOUT;
  }
  esp_err_t result = ConfigManager::_save_locked(replace_unreadable);
  ConfigManager::_unlock();
  return result;
}

esp_err_t ConfigManager::_save_locked(bool replace_unreadable) {
  // Serialised under the save lock, before the file is opened. The std::string and std::vector fields changed
  // after boot are changed through change(), under the same lock, so they are never read mid-assignment. The
  // rest are bools and integers (a word-sized read can be stale but never torn), and the md5 strings, which
  // only UpdateManager's update tasks write, one update at a time, each assignment followed by the task's own
  // save. A save racing one of those can serialise a stale or garbled md5 (the dump's replace handler keeps
  // that from aborting), and the writer's save, queued behind it on this lock, replaces it at once.
  // WebManager's setup form also assigns its fields unlocked, but the panel restarts right after its save.
  std::string content = ConfigManager::_serialise();

  switch (ConfigManager::_file_state.save_action(content, replace_unreadable)) {
  case ConfigFileState::SaveAction::SKIP_UNCHANGED:
    ESP_LOGD(TAG, "config.json unchanged, not written.");
    PanelMetrics::config_saves[PanelMetrics::CONFIG_SAVE_UNCHANGED].add();
    return ESP_OK;
  case ConfigFileState::SaveAction::REFUSE_UNREADABLE:
    ESP_LOGW(TAG, "config.json could not be read at boot. Not saving over it until the settings are saved from the web UI.");
    PanelMetrics::config_saves[PanelMetrics::CONFIG_SAVE_REFUSED_UNREADABLE].add();
    return ESP_ERR_INVALID_STATE;
  case ConfigFileState::SaveAction::WRITE:
    break;
  }

  int64_t write_start_us = esp_timer_get_time();
  FILE *f = fopen(CONFIG_PATH, "w");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open /littlefs/config.json for writing: %s.", strerror(errno));
    PanelMetrics::config_saves[PanelMetrics::CONFIG_SAVE_FAILED].add();
    return ESP_ERR_NOT_FINISHED;
  }

  size_t bytes_written = fwrite(content.data(), 1, content.size(), f);
  bool write_ok = bytes_written == content.size() && ferror(f) == 0;
  // fclose() is what commits the new contents (see the #error above). After a failed write LittleFS skips the
  // commit and the old file stays, but fclose() must still be called to free the handle.
  int close_result = fclose(f);
  PanelMetrics::config_write_duration.observe_us(esp_timer_get_time() - write_start_us);
  if (!write_ok) {
    ESP_LOGE(TAG, "Wrote %zu of %zu bytes to /littlefs/config.json. The file keeps its old contents.", bytes_written, content.size());
    PanelMetrics::config_saves[PanelMetrics::CONFIG_SAVE_FAILED].add();
    return ESP_ERR_NOT_FINISHED;
  }
  if (close_result != 0) {
    ESP_LOGE(TAG, "Failed to close /littlefs/config.json after writing: %s. The file keeps its old contents.", strerror(errno));
    PanelMetrics::config_saves[PanelMetrics::CONFIG_SAVE_FAILED].add();
    return ESP_ERR_NOT_FINISHED;
  }

  ConfigManager::_file_state.saved(std::move(content));
  PanelMetrics::config_saves[PanelMetrics::CONFIG_SAVE_WRITTEN].add();
  PanelMetrics::config_written_bytes.add(bytes_written);
  PanelMetrics::config_file_bytes.set(static_cast<int32_t>(bytes_written));
  ESP_LOGI(TAG, "Saved %zu bytes to /littlefs/config.json", bytes_written);
  return ESP_OK;
}
