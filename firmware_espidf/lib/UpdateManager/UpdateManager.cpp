#include <ConfigManager.hpp>
#include <LittleFS.hpp>
#include <LoadingPage.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <Nextion.hpp>
#include <Nextion_event.hpp>
#include <StatusUpdateManager.hpp>
#include <UpdateManager.hpp>
#include <UpdateManager_event.hpp>
#include <WiFiManager.hpp>
#include <cmath>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <nlohmann/json.hpp>

ESP_EVENT_DEFINE_BASE(UPDATEMANAGER_EVENT);

void UpdateManager::init() {
  esp_log_level_set("UpdateManager", ConfigManager::log_level);
  UpdateManager::_download_data_store_mutex = xSemaphoreCreateMutex();

  MqttManager::register_handler(MQTT_EVENT_ANY, UpdateManager::_mqtt_event_handler, NULL);

  std::string command_topic = "nspanel/";
  command_topic.append(WiFiManager::mac_string());
  command_topic.append("/command");
  MqttManager::subscribe(command_topic);
}

void UpdateManager::update_gui(void *param) {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    esp_event_handler_register(NEXTION_EVENT, ESP_EVENT_ANY_ID, UpdateManager::_nextion_event_handler, NULL);

    std::string tft_file_url = "http://";
    tft_file_url.append(NSPM_ConfigManager::get_manager_address());
    tft_file_url.append(":");
    tft_file_url.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
    tft_file_url.append("/download_tft/");
    tft_file_url.append(std::to_string(config->nspanel_id));

    esp_http_client_config_t http_client_config = {};
    http_client_config.url = tft_file_url.c_str();
    http_client_config.cert_pem = NULL;
    http_client_config.keep_alive_enable = true;

    size_t remote_tft_file_size;
    while (UpdateManager::_get_remote_file_size(tft_file_url, &remote_tft_file_size) != ESP_OK) {
      ESP_LOGW("UpdateManager", "Failed to get remote TFT file size. Will try again in 500ms");
      vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI("UpdateManager", "Will start updating GUI. Remote file size: %zu bytes", remote_tft_file_size);
    while (Nextion::start_update(ConfigManager::nextion_upload_baudrate, ConfigManager::use_latest_nextion_upload_protocol, remote_tft_file_size) != ESP_OK) {
      ESP_LOGW("UpdateManager", "Failed to init update process with Nextion display. Will try again in 5000ms");
      vTaskDelay(pdMS_TO_TICKS(5000));
    }

    esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::NEXTION_UPDATE_STARTED, NULL, 0, pdMS_TO_TICKS(500));

    std::vector<uint8_t> tft_data_buffer;
    uint8_t consecutive_errors = 0;
    esp_http_client_handle_t http_client;
    for (;;) { // Try forever to setup a valid HTTP client
      http_client = esp_http_client_init(&http_client_config);
      if (http_client == NULL) {
        ESP_LOGE("UpdateManager", "Failed to init http_client. Got NULL! Will try again in 2 seconds.");
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }

      std::string range_header = "bytes=";
      range_header.append(std::to_string(UpdateManager::_nextion_update_current_offset));
      range_header.append("-");
      // NOTE: RFC 7233 byte ranges are inclusive, so the correct end would be
      // (remote_tft_file_size - 1). However, the server-side implementation uses
      // Python slice notation data[start:end] (exclusive end), so it interprets
      // the range end as exclusive too. Both sides use the same non-standard
      // convention and the protocol works correctly as a result. Do not change
      // this without a coordinated fix to the server. See PR #12 for context.
      range_header.append(std::to_string(remote_tft_file_size));
      esp_err_t err = esp_http_client_set_header(http_client, "Range", range_header.c_str());
      if (err != ESP_OK) {
        ESP_LOGE("UpdateManager", "Failed to set range header will trying to download chunk of data! Error: %s. Will try again in 2 seconds", esp_err_to_name(err));
        esp_http_client_cleanup(http_client);
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }

      err = esp_http_client_open(http_client, 0);
      if (err != ESP_OK) {
        ESP_LOGE("UpdateManager", "Failed to call esp_http_client_open! Error: %s. Will try again in 2 seconds", esp_err_to_name(err));
        esp_http_client_cleanup(http_client);
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }

      int64_t err_code = esp_http_client_fetch_headers(http_client);
      if (err_code > 0) {
        ESP_LOGI("UpdateManager", "HTTP Client status: %d, remote file size: %lld", esp_http_client_get_status_code(http_client), esp_http_client_get_content_length(http_client));
        break; // Client created successfully and header set.
      } else {
        ESP_LOGE("UpdateManager", "Failed to call esp_http_client_fetch_headers! Error: %lld. Will try again in 2 seconds", err_code);
        esp_http_client_cleanup(http_client);
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
    }

    // Update started. Wait for notifications and when they arrive, download data from offset into buffer and write to display.
    float progress;
    for (;;) {
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20000)) == pdPASS) {
        consecutive_errors = 0;
        if (UpdateManager::_nextion_update_reset_http_client) {
          esp_http_client_cleanup(http_client);
          UpdateManager::_nextion_update_reset_http_client = false;

          ESP_LOGI("UpdateManager", "Will reset HTTP client. Waiting 2 seconds for cleanup before establishing a new HTTP client.");
          vTaskDelay(pdMS_TO_TICKS(2000));

          while (UpdateManager::_setup_http_client(&http_client, &tft_data_buffer, tft_file_url.c_str(), UpdateManager::_nextion_update_current_offset, remote_tft_file_size - UpdateManager::_nextion_update_current_offset) != ESP_OK) {
            ESP_LOGE("UpdateManager", "Failed to setup HTTP client. Will try again in 5 seconds.");
            vTaskDelay(pdMS_TO_TICKS(5000));
          }
          ESP_LOGI("UpdateManager", "HTTP Client reset successfully. New range: %llu -> %llu.", UpdateManager::_nextion_update_current_offset, remote_tft_file_size - UpdateManager::_nextion_update_current_offset);
        }

        // Got event to send new data. Calculate next chunk size but a max of 4096 bytes:
        UpdateManager::_nextion_update_next_chunk_size = remote_tft_file_size - UpdateManager::_nextion_update_current_offset;
        if (UpdateManager::_nextion_update_next_chunk_size > 4096) {
          UpdateManager::_nextion_update_next_chunk_size = 4096;
        }

        UpdateManager::_download_data_max_bytes = UpdateManager::_nextion_update_next_chunk_size;
        tft_data_buffer.resize(UpdateManager::_download_data_max_bytes);
        size_t downloaded_bytes = 0;
        while (downloaded_bytes < UpdateManager::_nextion_update_next_chunk_size) {
          // esp_http_client_read() returns -1 on error, so this must stay signed for the check below to catch it.
          int chunk_size = esp_http_client_read(http_client, (char *)tft_data_buffer.data() + downloaded_bytes, UpdateManager::_nextion_update_next_chunk_size - downloaded_bytes);
          if (chunk_size > 0) {
            downloaded_bytes += chunk_size;
            if (downloaded_bytes < UpdateManager::_nextion_update_next_chunk_size) {
              ESP_LOGD("UpdateManager", "Read %d bytes. Expected %llu bytes. Will try again.", chunk_size, UpdateManager::_nextion_update_next_chunk_size - downloaded_bytes);
            }
          } else {
            ESP_LOGE("UpdateManager", "Failed to download data via HTTP client. Will cleanup old client and create new. Will try again in 5 seconds.");
            esp_http_client_cleanup(http_client);
            vTaskDelay(pdMS_TO_TICKS(5000));

            for (;;) { // Try forever to setup a valid HTTP client
              http_client = esp_http_client_init(&http_client_config);
              if (http_client == NULL) {
                ESP_LOGE("UpdateManager", "Failed to init http_client. Got NULL! Will try again in 2 seconds.");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
              }

              std::string range_header = "bytes=";
              range_header.append(std::to_string(UpdateManager::_nextion_update_current_offset + downloaded_bytes));
              range_header.append("-");
              // See comment above: end is intentionally non-RFC-compliant to match server.
              range_header.append(std::to_string(remote_tft_file_size));
              esp_err_t err = esp_http_client_set_header(http_client, "Range", range_header.c_str());
              if (err != ESP_OK) {
                ESP_LOGE("UpdateManager", "Failed to set range header will trying to download chunk of data! Error: %s. Will try again in 2 seconds", esp_err_to_name(err));
                esp_http_client_cleanup(http_client);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
              }

              err = esp_http_client_open(http_client, 0);
              if (err != ESP_OK) {
                ESP_LOGE("UpdateManager", "Failed to call esp_http_client_open! Error: %s. Will try again in 2 seconds", esp_err_to_name(err));
                esp_http_client_cleanup(http_client);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
              }

              int64_t err_code = esp_http_client_fetch_headers(http_client);
              if (err_code > 0) {
                ESP_LOGI("UpdateManager", "HTTP Client status: %d, remote file size: %lld", esp_http_client_get_status_code(http_client), esp_http_client_get_content_length(http_client));
                break; // Client created successfully and header set.
              } else {
                ESP_LOGE("UpdateManager", "Failed to call esp_http_client_fetch_headers! Error: %lld. Will try again in 2 seconds", err_code);
                esp_http_client_cleanup(http_client);
                vTaskDelay(pdMS_TO_TICKS(2000));
              }
            }
            ESP_LOGI("UpdateManager", "HTTP Client reset successfully. New range: %llu -> %zu.", UpdateManager::_nextion_update_current_offset, remote_tft_file_size);
          }
        }

        while (Nextion::write_update_bytes(tft_data_buffer.data(), tft_data_buffer.size()) != ESP_OK) {
          ESP_LOGE("UpdateManager", "Failed to write TFT chunk to UART while updating GUI. Will try again in 5 seconds.");
          vTaskDelay(pdMS_TO_TICKS(5000));
        }
        // ESP_LOGD("UpdateManager", "Wrote %zu bytes to Nextion display. Total bytes written: %llu", tft_data_buffer.size(), UpdateManager::_nextion_update_current_offset + tft_data_buffer.size());

        progress = (std::round((((double)UpdateManager::_nextion_update_current_offset + (double)UpdateManager::_nextion_update_next_chunk_size) / (double)remote_tft_file_size) * 100) / 100) * 100;
        esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::NEXTION_UPDATE_PROGRESS, &progress, sizeof(progress), pdMS_TO_TICKS(250));

        if (UpdateManager::_nextion_update_current_offset + UpdateManager::_nextion_update_next_chunk_size >= remote_tft_file_size) {
          break; // Update complete
        }
      } else {
        consecutive_errors++;
        ESP_LOGE("UpdateManager", "No Nextion update event in 20s (error %u/3). Will retry.", consecutive_errors);
        if (consecutive_errors >= 3) {
          ESP_LOGE("UpdateManager", "Too many consecutive update timeouts — rebooting.");
          vTaskDelay(pdMS_TO_TICKS(2000));
          StatusUpdateManager::reboot();
        }
        while (Nextion::start_update(ConfigManager::nextion_upload_baudrate, ConfigManager::use_latest_nextion_upload_protocol, remote_tft_file_size) != ESP_OK) {
          ESP_LOGW("UpdateManager", "Failed to init update process with Nextion display. Will try again in 5000ms");
          vTaskDelay(pdMS_TO_TICKS(5000));
        }
      }
    }

    // Cleanup HTTP client used in transfering TFT file.
    esp_http_client_close(http_client);
    esp_http_client_cleanup(http_client);

    esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::NEXTION_UPDATE_FINISHED, NULL, 0, pdMS_TO_TICKS(500));
    ESP_LOGI("UpdateManager", "Nextion GUI update complete. Updating stored GUI md5 checksum.");

    std::string gui_md5_checksum_url = "http://";
    gui_md5_checksum_url.append(NSPM_ConfigManager::get_manager_address());
    gui_md5_checksum_url.append(":");
    gui_md5_checksum_url.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
    gui_md5_checksum_url.append("/checksum_tft_file/");
    gui_md5_checksum_url.append(std::to_string(config->nspanel_id));

    ESP_LOGD("UpdateManager", "Will download md5 checksum from URL: %s", gui_md5_checksum_url.c_str());
    std::vector<uint8_t> data;
    while (UpdateManager::_download_data(&data, gui_md5_checksum_url.c_str(), -1, -1) != ESP_OK) {
      ESP_LOGE("UpdateManager", "Failed to get MD5 checksum from manager. Will try again in 1 second.");
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    std::string gui_md5 = std::string((char *)data.data(), data.size());
    ESP_LOGI("UpdateManager", "Got new GUI md5 checksum from manager: %s", gui_md5.c_str());
    ConfigManager::md5_gui = gui_md5;
    ConfigManager::save_config();

    ESP_LOGI("UpdateManager", "GUI file successfully updated and new md5 checksum stored. Will reboot in 5 seconds.");
    vTaskDelay(pdMS_TO_TICKS(5000));

    StatusUpdateManager::reboot();
    vTaskDelete(NULL);
  } else {
    ESP_LOGE("UpdateManager", "Failed to get NSPanelConfig when trying to update GUI.");
  }

  UpdateManager::_current_update_task = NULL;

  vTaskDelete(NULL);
}

void UpdateManager::update_firmware(void *param) {
  std::string firmware_md5_string = "http://";
  firmware_md5_string.append(NSPM_ConfigManager::get_manager_address());
  firmware_md5_string.append(":");
  firmware_md5_string.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
  firmware_md5_string.append("/checksum_firmware?model=");
#if defined(BOARD_SONOFF)
  firmware_md5_string.append("sonoff");
#elif defined(BOARD_CUSTOM)
  firmware_md5_string.append("custom");
#endif

  std::vector<uint8_t> data;
  // Always fetch the checksum, also for forced updates, so that it can be stored as the pending checksum for the firmware about to be flashed.
  if (UpdateManager::_download_data(&data, firmware_md5_string.c_str(), -1, -1) == ESP_OK) {
    std::string md5_string = std::string((char *)data.data(), data.size());
    ESP_LOGD("UpdateManager", "Got new MD5 sum from manager: %s", md5_string.c_str());

    if (UpdateManager::_force_update || md5_string.compare(ConfigManager::md5_firmware) != 0) {
      if (UpdateManager::_force_update) {
        ESP_LOGI("UpdateManager", "This is a force update, clearing stored checksum for firmware and littlefs.");
        ConfigManager::md5_data_file = "";
        ConfigManager::md5_firmware = "";
        ConfigManager::save_config();
      }
      ESP_LOGI("UpdateManager", "New firmware available. Will update OTA.");
      if (UpdateManager::_update_firmware_ota() == ESP_OK) {
        ConfigManager::has_updated = true;
        ConfigManager::md5_firmware_pending = md5_string;
        ConfigManager::save_config();

        ESP_LOGI("UpdateManager", "Firmware update complete. Stored firmware checksum will be updated on next successful boot.");

        UpdateManager::update_littlefs(NULL, true);

        ESP_LOGI("UpdateManager", "Update complete. Will restart in 2 seconds");
        vTaskDelay(pdMS_TO_TICKS(2000));
        StatusUpdateManager::reboot();
      }
    } else {
      ESP_LOGI("UpdateManager", "Firmware already up to date. Will check LittleFS.");
      UpdateManager::update_littlefs(NULL, false);
    }
  } else {
    ESP_LOGE("UpdateManager", "Failed to new firmware MD5 checksum.");
  }

  UpdateManager::_current_update_task = NULL;
  vTaskDelete(NULL);
}

void UpdateManager::update_littlefs(void *param, bool force_update) {
  // TODO: Implement
  std::string littlefs_md5_string = "http://";
  littlefs_md5_string.append(NSPM_ConfigManager::get_manager_address());
  littlefs_md5_string.append(":");
  littlefs_md5_string.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
  littlefs_md5_string.append("/checksum_data_file?model=");
#if defined(BOARD_SONOFF)
  littlefs_md5_string.append("sonoff");
#elif defined(BOARD_CUSTOM)
  littlefs_md5_string.append("custom");
#endif

  std::vector<uint8_t> data;
  if (UpdateManager::_download_data(&data, littlefs_md5_string.c_str(), -1, -1) == ESP_OK) {
    std::string md5_string = std::string((char *)data.data(), data.size());
    ESP_LOGD("UpdateManager", "Got new MD5 sum from manager: %s", md5_string.c_str());

    if (md5_string.compare(ConfigManager::md5_data_file) != 0 || force_update) {
      ESP_LOGI("UpdateManager", "New LittleFS available. Will update OTA.");
      LittleFS::unmount(); // Unmount LittleFS
      if (UpdateManager::_update_littlefs_ota() == ESP_OK) {
        ESP_LOGI("UpdateManager", "LittleFS update complete, will save new MD5 checksum.");
        ConfigManager::md5_data_file = md5_string;
        // Save the existing config loaded into memory into the new LittleFS partition.
        LittleFS::mount();
        ConfigManager::save_config();

        ESP_LOGI("UpdateManager", "Update complete. Will restart in 2 seconds");
        vTaskDelay(pdMS_TO_TICKS(2000));
        StatusUpdateManager::reboot();
      }
    } else {
      ESP_LOGI("UpdateManager", "LittleFS already up to date.");
    }
  } else {
    ESP_LOGE("UpdateManager", "Failed to new LittleFS MD5 checksum.");
  }
}

void UpdateManager::mark_boot_successful() {
  esp_ota_mark_app_valid_cancel_rollback();
  ESP_LOGI("UpdateManager", "Current boot marked as successful, will not rollback on reboot. Updating stored FW checksum in LittleFS.");
}

void UpdateManager::update_internal_firmware_checksum() {
  if (!ConfigManager::has_updated) {
    return;
  }

  // Use the checksum recorded when the firmware was flashed. The manager may be serving a different firmware by now.
  std::string md5_string = ConfigManager::md5_firmware_pending;
  if (md5_string.empty()) {
    // Update was installed by a firmware that did not record a pending checksum, fall back to asking the manager.
    std::string firmware_md5_string = "http://";
    firmware_md5_string.append(NSPM_ConfigManager::get_manager_address());
    firmware_md5_string.append(":");
    firmware_md5_string.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
    firmware_md5_string.append("/checksum_firmware?model=");
#if defined(BOARD_SONOFF)
    firmware_md5_string.append("sonoff");
#elif defined(BOARD_CUSTOM)
    firmware_md5_string.append("custom");
#endif

    std::vector<uint8_t> data;
    if (UpdateManager::_download_data(&data, firmware_md5_string.c_str(), -1, -1) != ESP_OK) {
      ESP_LOGE("UpdateManager", "Failed to get firmware checksum from manager.");
      return;
    }
    md5_string = std::string((char *)data.data(), data.size());
  }

  ConfigManager::md5_firmware = md5_string;
  ConfigManager::md5_firmware_pending = "";
  ConfigManager::has_updated = false;
  // Save the existing config loaded into memory into the new LittleFS partition.
  if (ConfigManager::save_config() == ESP_OK) {
    ESP_LOGI("UpdateManager", "Updated stored firmware checksum to %s. Will reboot.", md5_string.c_str());
  } else {
    ESP_LOGE("UpdateManager", "Failed to save config!");
  }

  // Always reboot: InterfaceManager stays on the "Updated checksums, rebooting." page while has_updated was set.
  vTaskDelay(pdTICKS_TO_MS(10));
  StatusUpdateManager::reboot();
}

esp_err_t UpdateManager::_setup_http_client(esp_http_client_handle_t *client, std::vector<uint8_t> *return_data, const char *download_url, int64_t offset, int64_t length) {
  UpdateManager::_download_data_store = return_data;

  esp_http_client_config_t config = {};
  config.url = download_url;
  config.event_handler = UpdateManager::_http_event_handler;

  *client = esp_http_client_init(&config);
  if (*client == NULL) {
    ESP_LOGE("UpdateManager", "Failed to init HTTP client for %s.", download_url);
    return ESP_ERR_NOT_FINISHED;
  }

  if (offset >= 0 && length > 0) {
    std::string range_header = "bytes=";
    range_header.append(std::to_string(offset));
    range_header.append("-");
    // See comment in update_gui(): end is intentionally non-RFC-compliant to match server.
    range_header.append(std::to_string(offset + length));
    esp_err_t err = esp_http_client_set_header(*client, "Range", range_header.c_str());
    if (err != ESP_OK) {
      ESP_LOGE("UpdateManager", "Failed to set range header will trying to download chunk of data! Error: %s", esp_err_to_name(err));
      esp_http_client_cleanup(*client);
      return ESP_ERR_NOT_FINISHED;
    }
  }
  return ESP_OK;
}

esp_err_t UpdateManager::_download_data(std::vector<uint8_t> *return_data, const char *download_url, int64_t offset, int64_t length) {
  if (xSemaphoreTake(UpdateManager::_download_data_store_mutex, portMAX_DELAY) == pdPASS) {
    esp_http_client_handle_t client;
    UpdateManager::_download_data_max_bytes = 0;
    if (UpdateManager::_setup_http_client(&client, return_data, download_url, offset, length) == ESP_OK) {
      // Perform the actual HTTP request to get data
      esp_err_t err = esp_http_client_perform(client);

      if (err == ESP_OK) {
        esp_http_client_cleanup(client);
        UpdateManager::_download_data_store = nullptr;
        xSemaphoreGive(UpdateManager::_download_data_store_mutex);
        return ESP_OK;
      } else {
        ESP_LOGE("UpdateManager", "Failed to download data from %s. Got error: %s. HTTP Status code: %d.", download_url, esp_err_to_name(err), esp_http_client_get_status_code(client));
        esp_http_client_cleanup(client);
        UpdateManager::_download_data_store = nullptr;
        xSemaphoreGive(UpdateManager::_download_data_store_mutex);
        return ESP_ERR_NOT_FINISHED;
      }
    } else {
      UpdateManager::_download_data_store = nullptr;
      xSemaphoreGive(UpdateManager::_download_data_store_mutex);
    }
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t UpdateManager::_get_remote_file_size(std::string url, size_t *file_size) {
  // Start actual littlefs update
  esp_http_client_config_t http_config = {};
  http_config.url = url.c_str();
  http_config.cert_pem = NULL;
  http_config.keep_alive_enable = true;

  // Setup HTTP connection:
  esp_http_client_handle_t client = esp_http_client_init(&http_config);
  if (client == NULL) {
    ESP_LOGE("UpdateManager", "Failed to initialise HTTP connection");
    return ESP_ERR_NOT_FINISHED;
  }
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE("UpdateManager", "Failed to open HTTP connection: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return ESP_ERR_NOT_FINISHED;
  }

  esp_http_client_fetch_headers(client);
  *file_size = esp_http_client_get_content_length(client);

  esp_http_client_cleanup(client);
  return ESP_OK;
}

esp_err_t UpdateManager::_http_event_handler(esp_http_client_event_t *event) {
  switch (event->event_id) {
  case HTTP_EVENT_ON_DATA: {
    if (!esp_http_client_is_chunked_response(event->client) && UpdateManager::_download_data_store != nullptr) {
      if (UpdateManager::_download_data_max_bytes != 0) {
        UpdateManager::_download_data_store->reserve(UpdateManager::_download_data_max_bytes);
        uint64_t bytes_left = UpdateManager::_download_data_max_bytes - UpdateManager::_download_data_store->size();
        UpdateManager::_download_data_store->insert(UpdateManager::_download_data_store->end(), (uint8_t *)event->data, (uint8_t *)event->data + (event->data_len > bytes_left ? bytes_left : event->data_len));
        if (UpdateManager::_download_data_store->size() >= UpdateManager::_download_data_max_bytes) {
          return ESP_OK;
        }
      } else {
        UpdateManager::_download_data_store->insert(UpdateManager::_download_data_store->end(), (uint8_t *)event->data, (uint8_t *)event->data + event->data_len);
      }
    } else {
      ESP_LOGE("UpdateManager", "Download data is chunked. Not supported!");
    }
    break;
  }

  default:
    break;
  }
  return ESP_OK;
}

void UpdateManager::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // TODO: Move all "command topic listening" to a separate "CommandManager" or something like that.

  if (event_id == MQTT_EVENT_DATA) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    std::string topic_string = std::string(event->topic, event->topic_len);

    std::string command_topic = "nspanel/";
    command_topic.append(WiFiManager::mac_string());
    command_topic.append("/command");

    if (topic_string.compare(command_topic) == 0 && event->data_len > 0) {
      nlohmann::json json = nlohmann::json::parse(event->data, event->data + event->data_len, nullptr, false);
      if (json.is_discarded()) {
        ESP_LOGW("UpdateManager", "Failed to parse payload as JSON.");
        // Failed to parse as JSON
        return;
      }

      if (json.contains("command") && json["command"].is_string()) {
        std::string command_string = json["command"];

        if (command_string.compare("reboot") == 0) { // TODO: Move to some place more fitting.
          ESP_LOGI("UpdateManager", "Received command to reboot. Will reboot NSPanel.");
          LoadingPage::show();
          LoadingPage::set_loading_text("Rebooting");
          LoadingPage::set_secondary_text("");
          StatusUpdateManager::reboot();
        } else if (command_string.compare("firmware_update") == 0 && UpdateManager::_current_update_task == NULL) {
          UpdateManager::_force_update = false;
          xTaskCreatePinnedToCore(UpdateManager::update_firmware, "update_firmware", 8192, NULL, 2, &UpdateManager::_current_update_task, 1);
        } else if (command_string.compare("firmware_update_force") == 0 && UpdateManager::_current_update_task == NULL) {
          UpdateManager::_force_update = true;
          xTaskCreatePinnedToCore(UpdateManager::update_firmware, "update_firmware", 8192, NULL, 2, &UpdateManager::_current_update_task, 1);
        } else if (command_string.compare("tft_update") == 0 && UpdateManager::_current_update_task == NULL) {
          if (NSPM_ConfigManager::get_manager_address().empty()) {
            ESP_LOGE("UpdateManager", "Cannot start TFT update: manager address not yet known.");
          } else {
            xTaskCreatePinnedToCore(UpdateManager::update_gui, "update_gui", 8192, NULL, 2, &UpdateManager::_current_update_task, 1);
          }
        } else {
          ESP_LOGW("UpdateManager", "Unknown command: %s", command_string.c_str());
        }
      }
    }
  }
}

void UpdateManager::_nextion_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case nextion_event_t::UPDATE_READY_FOR_NEXT_CHUNK: {
    UpdateManager::_nextion_update_current_offset += UpdateManager::_nextion_update_next_chunk_size;
    xTaskNotifyGive(UpdateManager::_current_update_task);
    break;
  }

  case nextion_event_t::UPDATE_JUMP_TO_OFFSET: {
    size_t *jump_offset = (size_t *)event_data;
    if (*jump_offset > 0) { // If set to zero, continue and treat it as a 0x05 as 0 means "continue and do not jump"
      UpdateManager::_nextion_update_current_offset = *jump_offset;
      UpdateManager::_nextion_update_reset_http_client = true; // We jumped in file. Reset HTTP client and reconnect with correct "Range"-header set.
    } else {
      UpdateManager::_nextion_update_current_offset += UpdateManager::_nextion_update_next_chunk_size;
    }
    ESP_LOGI("UpdateManager", "Received instruction from Nextion display to jump to %zu TFT file offset.", *jump_offset);
    xTaskNotifyGive(UpdateManager::_current_update_task);
    break;
  }

  default:
    break;
  }
}

esp_err_t UpdateManager::_update_firmware_ota() {
  // Firmware download URL
  std::string firmware_download_url = "http://";
  firmware_download_url.append(NSPM_ConfigManager::get_manager_address());
  firmware_download_url.append(":");
  firmware_download_url.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
  firmware_download_url.append("/download_firmware?model=");
#if defined(BOARD_SONOFF)
  firmware_download_url.append("sonoff");
#elif defined(BOARD_CUSTOM)
  firmware_download_url.append("custom");
#endif

  // Send event that we actually started with firmware update
  esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::FIRMWARE_UPDATE_STARTED, NULL, 0, pdMS_TO_TICKS(500));

  // Start actual firmware update
  esp_http_client_config_t http_config = {};
  http_config.url = firmware_download_url.c_str();
  http_config.cert_pem = NULL;
  http_config.keep_alive_enable = true;

  esp_err_t ret;
  esp_https_ota_config_t config = {};
  config.http_config = &http_config;

  esp_https_ota_handle_t https_ota_handle = 0;
  ret = esp_https_ota_begin(&config, &https_ota_handle);
  if (ret != ESP_OK) {
    ESP_LOGE("UpdateManager", "esp_https_ota_begin failed: %s", esp_err_to_name(ret));
    esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::FIRMWARE_UPDATE_FAILED, NULL, 0, pdMS_TO_TICKS(500));
    return ESP_ERR_NOT_FINISHED;
  }

  esp_app_desc_t app_desc;
  ret = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
  if (ret != ESP_OK) {
    ESP_LOGE("UpdateManager", "esp_https_ota_get_img_desc failed");
    esp_https_ota_abort(https_ota_handle);
    esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::FIRMWARE_UPDATE_FAILED, NULL, 0, pdMS_TO_TICKS(500));
    return ESP_ERR_NOT_FINISHED;
  }

  // ret = UpdateManager::_validate_image_header(&app_desc);
  // if (ret != ESP_OK) {
  //   ESP_LOGE("UpdateManager", "image header verification failed");
  //   return ESP_ERR_NOT_FINISHED;
  // }

  int firmware_size = esp_https_ota_get_image_size(https_ota_handle);
  ESP_LOGI("UpdateManager", "Will update firmware from %s. Remote file size: %d bytes.", firmware_download_url.c_str(), firmware_size);

  int data_read_len = 0;
  float progress = 0;
  uint64_t last_progress_update = 0;
  while (true) {
    ret = esp_https_ota_perform(https_ota_handle);
    if (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      if (esp_timer_get_time() / 1000 - last_progress_update > 250) {
        // 250ms has passed since last check, update progress percentage.
        data_read_len = esp_https_ota_get_image_len_read(https_ota_handle);
        // Calculate progress to within 1 decimals
        progress = (std::round(((double)data_read_len / (double)firmware_size) * 100) / 100) * 100;
        esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::FIRMWARE_UPDATE_PROGRESS, &progress, sizeof(progress), pdMS_TO_TICKS(50));

        // ESP_LOGD("UpdateManager", "Progress %d/%d (%f%%)", data_read_len, firmware_size, progress);
        last_progress_update = esp_timer_get_time() / 1000;
      }
    } else if (ret == ESP_OK) {
      progress = 100;
      break;
    } else {
      break;
    }
  }

  // Finish the OTA update
  if (ret == ESP_OK) {
    ret = esp_https_ota_finish(https_ota_handle);
    if (ret == ESP_OK) {
      esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::FIRMWARE_UPDATE_FINISHED, NULL, 0, pdMS_TO_TICKS(500));
      ESP_LOGI("UpdateManager", "Firmware OTA update successful.");
      return ESP_OK;
    } else {
      esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::FIRMWARE_UPDATE_FAILED, NULL, 0, pdMS_TO_TICKS(500));
      ESP_LOGE("UpdateManager", "Firmware OTA update failed: %s", esp_err_to_name(ret));
      return ESP_ERR_NOT_FINISHED;
    }
  } else {
    // esp_https_ota_finish frees the handle on its own; on this path it must be released explicitly.
    esp_https_ota_abort(https_ota_handle);
    esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::FIRMWARE_UPDATE_FAILED, NULL, 0, pdMS_TO_TICKS(500));
    ESP_LOGE("UpdateManager", "Firmware OTA update failed: %s", esp_err_to_name(ret));
    return ESP_ERR_NOT_FINISHED;
  }
}

esp_err_t UpdateManager::_update_littlefs_ota() {
  // Firmware download URL
  std::string littlefs_download_url = "http://";
  littlefs_download_url.append(NSPM_ConfigManager::get_manager_address());
  littlefs_download_url.append(":");
  littlefs_download_url.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
  littlefs_download_url.append("/download_data_file?model=");
#if defined(BOARD_SONOFF)
  littlefs_download_url.append("sonoff");
#elif defined(BOARD_CUSTOM)
  littlefs_download_url.append("custom");
#endif

  // Send event that we actually started with littlefs update
  esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::LITTLEFS_UPDATE_STARTED, NULL, 0, pdMS_TO_TICKS(500));

  // Start actual littlefs update
  esp_http_client_config_t http_config = {};
  http_config.url = littlefs_download_url.c_str();
  http_config.cert_pem = NULL;
  http_config.keep_alive_enable = true;

  // Find LittleFS partition
  esp_partition_t *littlefs_partition = NULL;
  esp_partition_iterator_t littlefs_partition_iterator = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  while (littlefs_partition_iterator != NULL) {
    littlefs_partition = (esp_partition_t *)esp_partition_get(littlefs_partition_iterator);
    littlefs_partition_iterator = esp_partition_next(littlefs_partition_iterator);
  }
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_partition_iterator_release(littlefs_partition_iterator);

  // Check that we found the partition
  if (littlefs_partition == NULL) {
    ESP_LOGE("UpdateManager", "Failed to find LittleFS partition! Will cancel update process.");
    return ESP_ERR_NOT_FINISHED;
  } else {
    ESP_LOGI("UpdateManager", "Found LittleFS partition:");
    ESP_LOGI("UpdateManager", "LittelFS: partition type = %d.", littlefs_partition->type);
    ESP_LOGI("UpdateManager", "LittelFS: partition subtype = %d.", littlefs_partition->subtype);
    ESP_LOGI("UpdateManager", "LittelFS: partition starting address = %ld.", littlefs_partition->address);
    ESP_LOGI("UpdateManager", "LittelFS: partition size = %ld.", littlefs_partition->size);
    ESP_LOGI("UpdateManager", "LittelFS: partition label = %s.", littlefs_partition->label);
    ESP_LOGI("UpdateManager", "LittelFS: partition subtype = %d.", littlefs_partition->encrypted);
    ESP_LOGI("UpdateManager", "---");
  }

  // Setup HTTP connection:
  esp_http_client_handle_t client = esp_http_client_init(&http_config);
  if (client == NULL) {
    ESP_LOGE("UpdateManager", "Failed to initialise HTTP connection");
    return ESP_ERR_NOT_FINISHED;
  }
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE("UpdateManager", "Failed to open HTTP connection: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return ESP_ERR_NOT_FINISHED;
  }

  esp_http_client_fetch_headers(client);
  uint64_t total_file_size = esp_http_client_get_content_length(client);
  ESP_LOGI("UpdateManager", "Remote LittleFS image size: %lld bytes.", total_file_size);

  // Delete current LittleFS partition
  err = esp_partition_erase_range(littlefs_partition, 0, littlefs_partition->size);
  if (err != ESP_OK) {
    ESP_LOGE("UpdateManager", "Failed to erase existing LittleFS partition. Error: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return ESP_ERR_NOT_FINISHED;
  }

  uint64_t last_progress_update = 0;
  float progress;

  uint64_t total_bytes_read = 0;
  constexpr uint16_t buffer_size = 4096;
  char data_buffer[buffer_size];
  for (;;) {
    int actual_data_read = esp_http_client_read(client, data_buffer, buffer_size);

    if (actual_data_read < 0) {
      ESP_LOGE("UpdateManager", "HTTP connection to manager failed! Will cancel operation.");
      esp_http_client_cleanup(client);
      return ESP_ERR_NOT_FINISHED;
    } else if (actual_data_read > 0) {
      err = esp_partition_write(littlefs_partition, total_bytes_read, (const void *)data_buffer, actual_data_read);
      if (err != ESP_OK) {
        ESP_LOGE("UpdateManager", "Failed to write data to buffer. Will cancel LittleFS update!");
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FINISHED;
      } else {
        total_bytes_read += actual_data_read;
      }

      if (esp_timer_get_time() / 1000 - last_progress_update > 250) {
        // 250ms has passed since last check, update progress percentage.        // Calculate progress to within 1 decimals
        progress = (std::round(((double)total_bytes_read / (double)total_file_size) * 100) / 100) * 100;
        esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::LITTLEFS_UPDATE_PROGRESS, &progress, sizeof(progress), pdMS_TO_TICKS(50));

        // ESP_LOGD("UpdateManager", "Progress %llu/%llu (%f%%)", total_bytes_read, total_file_size, progress);
        last_progress_update = esp_timer_get_time() / 1000;
      }
    } else {
      // Received 0 bytes ie. we've read all data and connection closed.
      ESP_LOGI("UpdateManager", "Wrote %llu bytes to LittleFS partition via OTA from manager. Update complete.", total_bytes_read);
      esp_http_client_cleanup(client);
      esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::LITTLEFS_UPDATE_FINISHED, NULL, 0, pdMS_TO_TICKS(50));
      return ESP_OK;
    }
  }

  return ESP_ERR_NOT_FINISHED;
}