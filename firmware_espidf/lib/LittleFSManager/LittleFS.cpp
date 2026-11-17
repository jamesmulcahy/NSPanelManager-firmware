#include <LittleFS.hpp>
#include <esp_err.h>
#include <esp_littlefs.h>
#include <esp_log.h>

esp_err_t LittleFS::mount() {
  ESP_LOGI("LittleFS", "Initializing LittleFS.");
  LittleFS::_is_mounted = false;

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/littlefs",
  conf.partition_label = "spiffs";
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;
  esp_err_t result = esp_vfs_littlefs_register(&conf);

  if (result == ESP_FAIL) {
    // The partition holds only config.json (the web UI is in the firmware), so a format is a safe
    // recovery: the panel then starts its setup AP, with a working setup page, instead of staying
    // broken. A blank partition, as after a fresh flash without a LittleFS image, comes here too.
    ESP_LOGW("LittleFS", "Could not mount LittleFS, formatting the partition.");
    conf.format_if_mount_failed = true;
    result = esp_vfs_littlefs_register(&conf);
  }

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