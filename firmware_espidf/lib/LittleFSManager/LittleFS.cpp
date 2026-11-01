#include <LittleFS.hpp>
#include <esp_err.h>
#include <esp_littlefs.h>
#include <esp_log.h>

esp_err_t LittleFS::mount() {
  esp_log_level_set("LittleFS", esp_log_level_t::ESP_LOG_DEBUG); // LittleFS is always in DEBUG as it loads before config is loaded
  ESP_LOGI("LittleFS", "Initializing LittleFS.");
  LittleFS::_is_mounted = false;

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/littlefs",
  conf.partition_label = "spiffs";
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;
  esp_err_t result = esp_vfs_littlefs_register(&conf);

  if (result == ESP_OK) {
    ESP_LOGI("LittleFS", "Mounted LittleFS successfully!");
    LittleFS::_is_mounted = true;
  } else if (result == ESP_FAIL) {
    ESP_LOGE("LittleFS", "Failed to mount LittleFS!");
  } else if (result == ESP_ERR_NOT_FOUND) {
    ESP_LOGE("LittleFS", "Failed to find LittleFS partition!");
  } else {
    ESP_LOGE("LittleFS", "Unknown failure when mounting LittleFS: %s", esp_err_to_name(result));
  }

  return result;
}

esp_err_t LittleFS::unmount() {
  return esp_vfs_littlefs_unregister("spiffs");
}

esp_err_t LittleFS::format() {
  esp_err_t result = esp_littlefs_format("spiffs");
  if (result != ESP_OK) {
    ESP_LOGE("LittleFS", "Failed to format LittleFS: %s", esp_err_to_name(result));
  }
  return result;
}

bool LittleFS::is_mounted() {
  return LittleFS::_is_mounted;
}