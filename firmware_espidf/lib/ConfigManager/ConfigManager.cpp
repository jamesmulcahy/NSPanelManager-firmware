#include "ConfigManager.hpp"
#include <LittleFS.hpp>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <nlohmann/json.hpp>
#include <string.h>

esp_err_t ConfigManager::load_config() {
  FILE *f = fopen("/littlefs/config.json", "r");
  if (f == NULL) {
    ESP_LOGE("ConfigManager", "Failed to open /littlefs/config.json for reading.");
    return ESP_ERR_NOT_FOUND;
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
      return ESP_ERR_NOT_FINISHED;
    }
  } else {
    ESP_LOGE("ConfigManager", "Returned invalid file size %ld of config file.", file_size);
    fclose(f);
    return ESP_ERR_NOT_FINISHED;
  }

  size_t bytes_read = fread(read_buffer, 1, file_size, f);
  fclose(f);
  if (bytes_read != (size_t)file_size) {
    ESP_LOGE("ConfigManager", "Read %zu of %ld bytes from config file.", bytes_read, file_size);
    free(read_buffer);
    return ESP_ERR_NOT_FINISHED;
  }
  read_buffer[bytes_read] = '\0';

  nlohmann::json config_data = nlohmann::json::parse(read_buffer, read_buffer + file_size, nullptr, false);
  free(read_buffer); // Data parsed, free read buffer.

  if (config_data.is_discarded()) {
    ESP_LOGE("ConfigManager", "Failed to parse JSON while reading config from LittleFS!");
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

  if (config_data.contains("lvgl_look_unconfirmed") && config_data["lvgl_look_unconfirmed"].is_boolean()) {
    ConfigManager::lvgl_look_unconfirmed = config_data["lvgl_look_unconfirmed"].get<bool>();
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

  return esp_err_t();
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
}

esp_err_t ConfigManager::save_config() {
  FILE *f = fopen("/littlefs/config.json", "w");
  if (f == NULL) {
    ESP_LOGE("ConfigManager", "Failed to open /littlefs/config.json for writing.");
    return ESP_ERR_NOT_FINISHED;
  }

  nlohmann::json json = nlohmann::json::object();
  json["log_level"] = static_cast<int>(ConfigManager::log_level);
  json["serial_log"] = static_cast<int>(ConfigManager::serial_log);
  json["lvgl_us_layout"] = ConfigManager::lvgl_us_layout;
  json["lvgl_ui"] = ConfigManager::lvgl_ui;
  json["lvgl_background"] = ConfigManager::lvgl_background;
  json["lvgl_theme"] = ConfigManager::lvgl_theme;
  json["lvgl_look_unconfirmed"] = ConfigManager::lvgl_look_unconfirmed;
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

  std::string json_string = json.dump();
  size_t bytes_written = fwrite(json_string.c_str(), sizeof(char), json_string.length(), f);

  ESP_LOGI("ConfigManager", "Saved %d bytes to /littlefs/config.json", bytes_written);

  bool wrote_correct_num_bytes = (bytes_written == (sizeof(char) * strlen(json_string.c_str())));
  fclose(f);

  if (wrote_correct_num_bytes) {
    return ESP_OK;
  } else {
    return ESP_ERR_NOT_FINISHED;
  }
}
