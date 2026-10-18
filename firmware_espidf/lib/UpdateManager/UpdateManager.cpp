#include <PanelRestart.hpp>
#include <ConfigManager.hpp>
#include <algorithm>
#include <InterfaceManager.hpp>
#include <EventLoopWatchdog.hpp>
#include <LvglLook.hpp>
#include <LvglUi.hpp>
#include <NextionTerminal.hpp>
#include <LvglRenderer.hpp>
#include <LoadingPage.hpp>
#include <MqttSubscriptions.hpp>
#include <PanelMetrics.hpp>
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

  std::string command_topic = "nspanel/";
  command_topic.append(WiFiManager::mac_string());
  command_topic.append("/command");
  UpdateManager::_command_subscription = mqtt::subscribe(command_topic, mqtt::Kind::COMMAND, &UpdateManager::_mqtt_message_handler, NULL, PanelMetrics::HANDLER_UPDATE_MANAGER);
}

void UpdateManager::update_gui(void *param) {
  // Keep the radio awake for the whole update, even if a reconnect brings the screensaver back.
  WiFiManager::set_update_in_progress(true);
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    esp_event_handler_register(NEXTION_EVENT, ESP_EVENT_ANY_ID, UpdateManager::_nextion_event_handler, NULL);

    std::string tft_file_url = "http://";
    tft_file_url.append(NSPM_ConfigManager::get_manager_address());
    tft_file_url.append(":");
    tft_file_url.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
    tft_file_url.append("/download_tft/");
    tft_file_url.append(std::to_string(config->nspanel_id));

    size_t remote_tft_file_size;
    while (UpdateManager::_get_remote_file_size(tft_file_url, &remote_tft_file_size) != ESP_OK) {
      ESP_LOGW("UpdateManager", "Failed to get remote TFT file size. Will try again in 500ms");
      vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI("UpdateManager", "Will start updating GUI. Remote file size: %zu bytes", remote_tft_file_size);
    // With LVGL as the UI the terminal holds the display's UART for good: end it first. The panel
    // reboots after the update, so it is never resumed.
    if (!LvglUi::suspend(10000)) {
      ESP_LOGW("UpdateManager", "The terminal still holds the display; the update will wait for it.");
    }
    while (Nextion::start_update(ConfigManager::nextion_upload_baudrate, ConfigManager::use_latest_nextion_upload_protocol, remote_tft_file_size) != ESP_OK) {
      ESP_LOGW("UpdateManager", "Failed to init update process with Nextion display. Will try again in 5000ms");
      vTaskDelay(pdMS_TO_TICKS(5000));
    }

    esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::NEXTION_UPDATE_STARTED, NULL, 0, pdMS_TO_TICKS(500));

    std::vector<uint8_t> tft_data_buffer;
    uint8_t consecutive_errors = 0;
    esp_http_client_handle_t http_client;
    while (UpdateManager::_open_tft_stream(tft_file_url, UpdateManager::_nextion_update_current_offset, remote_tft_file_size, &http_client) != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(2000));
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

          while (UpdateManager::_open_tft_stream(tft_file_url, UpdateManager::_nextion_update_current_offset, remote_tft_file_size, &http_client) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5000));
          }
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

            while (UpdateManager::_open_tft_stream(tft_file_url, UpdateManager::_nextion_update_current_offset + downloaded_bytes, remote_tft_file_size, &http_client) != ESP_OK) {
              vTaskDelay(pdMS_TO_TICKS(2000));
            }
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

  WiFiManager::set_update_in_progress(false); // The update ended without a reboot.
  // Give the screen back. Timeout 0 and back off here: never wait on the default loop's queue.
  while (esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::UPDATE_ENDED_WITHOUT_REBOOT, NULL, 0, 0) != ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  UpdateManager::_current_update_task = NULL;

  vTaskDelete(NULL);
}

void UpdateManager::update_firmware(void *param) {
  // Keep the radio awake for the whole update, even if a reconnect brings the screensaver back.
  WiFiManager::set_update_in_progress(true);
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
        ESP_LOGI("UpdateManager", "This is a force update, clearing stored checksum for firmware.");
        ConfigManager::md5_firmware = "";
        ConfigManager::save_config();
      }
      ESP_LOGI("UpdateManager", "New firmware available. Will update OTA.");
      if (UpdateManager::_update_firmware_ota() == ESP_OK) {
        ConfigManager::has_updated = true;
        ConfigManager::md5_firmware_pending = md5_string;
        ConfigManager::save_config();

        ESP_LOGI("UpdateManager", "Firmware update complete. Stored firmware checksum will be updated on next successful boot.");

        ESP_LOGI("UpdateManager", "Update complete. Will restart in 2 seconds");
        vTaskDelay(pdMS_TO_TICKS(2000));
        StatusUpdateManager::reboot();
      }
    } else {
      ESP_LOGI("UpdateManager", "Firmware already up to date.");
    }
  } else {
    ESP_LOGE("UpdateManager", "Failed to new firmware MD5 checksum.");
  }

  WiFiManager::set_update_in_progress(false); // The update ended without a reboot.
  // Give the screen back. Timeout 0 and back off here: never wait on the default loop's queue.
  while (esp_event_post(UPDATEMANAGER_EVENT, updatemanager_event_t::UPDATE_ENDED_WITHOUT_REBOOT, NULL, 0, 0) != ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  UpdateManager::_current_update_task = NULL;
  vTaskDelete(NULL);
}

void UpdateManager::mark_boot_successful() {
  esp_ota_mark_app_valid_cancel_rollback();
  ESP_LOGI("UpdateManager", "Current boot marked as successful, will not rollback on reboot. Updating stored FW checksum in config.json.");
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
    return_data->clear(); // Callers retry with the same vector; a failed attempt must not leave its body in it.
    if (UpdateManager::_setup_http_client(&client, return_data, download_url, offset, length) == ESP_OK) {
      // Perform the actual HTTP request to get data
      esp_err_t err = esp_http_client_perform(client);
      // perform() succeeds for any complete response, an error page included, and the caller stores
      // what it gets (a checksum, for instance), so only a 200 counts.
      if (err == ESP_OK && esp_http_client_get_status_code(client) != 200) {
        err = ESP_ERR_INVALID_RESPONSE;
      }

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
  esp_http_client_config_t http_config = {};
  http_config.url = url.c_str();
  http_config.cert_pem = NULL;
  http_config.keep_alive_enable = true;

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

  // Only the headers are read; the size is the Content-Length of a successful response. An error
  // page has a length too, and a response without one (chunked, or a failed fetch) reports -1.
  int64_t length = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (status != 200 || length <= 0) {
    ESP_LOGE("UpdateManager", "Manager answered %s with status %d and Content-Length %lld.", url.c_str(), status, length);
    return ESP_ERR_INVALID_RESPONSE;
  }

  *file_size = length;
  return ESP_OK;
}

esp_err_t UpdateManager::_open_tft_stream(const std::string &url, uint64_t offset, uint64_t file_size, esp_http_client_handle_t *client) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.cert_pem = NULL;
  config.keep_alive_enable = true;

  *client = esp_http_client_init(&config);
  if (*client == NULL) {
    ESP_LOGE("UpdateManager", "Failed to init HTTP client for %s.", url.c_str());
    return ESP_ERR_NOT_FINISHED;
  }

  // NOTE: RFC 7233 byte ranges are inclusive, so the correct end would be
  // (file_size - 1). However, the server-side implementation uses Python slice
  // notation data[start:end] (exclusive end), so it interprets the range end as
  // exclusive too. Both sides use the same non-standard convention and the
  // protocol works correctly as a result. Do not change this without a
  // coordinated fix to the server. See PR #12 for context.
  std::string range_header = "bytes=" + std::to_string(offset) + "-" + std::to_string(file_size);
  esp_err_t err = esp_http_client_set_header(*client, "Range", range_header.c_str());
  if (err == ESP_OK) {
    err = esp_http_client_open(*client, 0);
  }
  if (err != ESP_OK) {
    ESP_LOGE("UpdateManager", "Failed to request TFT %s: %s.", range_header.c_str(), esp_err_to_name(err));
    esp_http_client_cleanup(*client);
    return ESP_ERR_NOT_FINISHED;
  }

  // Whatever comes back is written to the display as TFT data from `offset` on, so it must be
  // exactly the rest of the file. The manager answers a range with 200 and just those bytes; a
  // standard server would send 206. Anything else (an error page, or the whole file from a
  // server that ignored Range) has the wrong status or length.
  int64_t length = esp_http_client_fetch_headers(*client);
  int status = esp_http_client_get_status_code(*client);
  if ((status != 200 && status != 206) || length != (int64_t)(file_size - offset)) {
    ESP_LOGE("UpdateManager", "Manager answered TFT %s with status %d and %lld bytes, expected %llu bytes.", range_header.c_str(), status, length, file_size - offset);
    esp_http_client_cleanup(*client);
    return ESP_ERR_INVALID_RESPONSE;
  }

  ESP_LOGI("UpdateManager", "Downloading TFT from offset %llu, %lld bytes.", offset, length);
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

void UpdateManager::_mqtt_message_handler(const mqtt::Message &message, void *context) {
  // TODO: Move all "command topic listening" to a separate "CommandManager" or something like that.
  if (message.payload.empty()) {
    return;
  }

  // As const char *, like every other parse() in the firmware: a uint8_t iterator would instantiate a
  // second copy of nlohmann's parser (about 12 KB of flash).
  const char *text = reinterpret_cast<const char *>(message.payload.data());
  nlohmann::json json = nlohmann::json::parse(text, text + message.payload.size(), nullptr, false);
  if (json.is_discarded()) {
    ESP_LOGW("UpdateManager", "Failed to parse payload as JSON.");
    // Failed to parse as JSON
    return;
  }

  if (json.contains("command") && json["command"].is_string()) {
    std::string command_string = json["command"];

    if (command_string.compare("reboot") == 0) { // TODO: Move to some place more fitting.
      ESP_LOGI("UpdateManager", "Received command to reboot. Will reboot NSPanel.");
      if (xTaskCreatePinnedToCore(UpdateManager::_task_reboot, "reboot", 4096, NULL, 2, NULL, 1) != pdPASS) [[unlikely]] {
        ESP_LOGE("UpdateManager", "Failed to create the reboot task. Restarting without the status report.");
        PanelRestart::restart();
      }
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
    } else if (command_string.compare("lvgl_tap") == 0) {
      // {"command": "lvgl_tap", "x": 160, "y": 20, "hold_ms": 100}: a tap at x,y (screen coordinates)
      // through the LVGL touch path, as if the display reported it. For testing without a finger.
      int x = json.contains("x") && json["x"].is_number_integer() ? json["x"].get<int>() : -1;
      int y = json.contains("y") && json["y"].is_number_integer() ? json["y"].get<int>() : -1;
      int hold = json.contains("hold_ms") && json["hold_ms"].is_number_integer() ? json["hold_ms"].get<int>() : 100;
      if (x >= 0 && y >= 0 && x < LvglRenderer::width() && y < LvglRenderer::height()) {
        ESP_LOGI("UpdateManager", "Tap at %d,%d for %d ms.", x, y, hold);
        // "x2", "y2": a swipe to there instead, in ten moves over hold_ms.
        int x2 = json.contains("x2") && json["x2"].is_number_integer() ? json["x2"].get<int>() : x;
        int y2 = json.contains("y2") && json["y2"].is_number_integer() ? json["y2"].get<int>() : y;
        x2 = std::clamp(x2, 0, LvglRenderer::width() - 1);
        y2 = std::clamp(y2, 0, LvglRenderer::height() - 1);
        hold = std::clamp(hold, 20, 5000);
        LvglRenderer::inject_touch(true, x, y);
        for (int step = 1; step <= 10; step++) {
          vTaskDelay(pdMS_TO_TICKS(hold / 10));
          if (x2 != x || y2 != y) {
            LvglRenderer::inject_touch(true, x + (x2 - x) * step / 10, y + (y2 - y) * step / 10);
          }
        }
        LvglRenderer::inject_touch(false, x2, y2);
      } else {
        ESP_LOGW("UpdateManager", "lvgl_tap needs x and y on the screen.");
      }
    } else if (command_string.compare("lvgl_background") == 0) {
      // {"command": "lvgl_background", "preset": "ocean"}: the page background with LVGL as the UI
      // ("none" for the plain look). Saved, applied at once and reported (LvglLook); a repeat does nothing.
      if (json.contains("preset") && json["preset"].is_string()) {
        LvglLook::command_background(json["preset"].get<std::string>());
      }
    } else if (command_string.compare("lvgl_theme") == 0) {
      // {"command": "lvgl_theme", "theme": "grey"}: the colours with LVGL as the UI: "auto" (the
      // background preset's own, dark without one), "dark", "grey" ("light" too), "ocean", "dusk", "aurora" or
      // "peaks". Saved, applied at once and reported (LvglLook); a repeat does nothing.
      if (json.contains("theme") && json["theme"].is_string()) {
        LvglLook::command_theme(json["theme"].get<std::string>());
      }
    } else if (command_string.compare("lvgl_ui") == 0) {
      // {"command": "lvgl_ui", "enable": false}: keep the Nextion pages whatever TFT the display runs
      // (true: LVGL on a terminal TFT, the default). Saved, then the panel reboots to apply it.
      if (json.contains("enable") && json["enable"].is_boolean()) {
        ConfigManager::lvgl_ui = json["enable"].get<bool>();
        ESP_LOGI("UpdateManager", "lvgl_ui set to %s; rebooting.", ConfigManager::lvgl_ui ? "true" : "false");
        ConfigManager::save_config();
        xTaskCreatePinnedToCore(UpdateManager::_task_reboot, "reboot", 4096, NULL, 2, NULL, 1);
      } else {
        ESP_LOGI("UpdateManager", "lvgl_ui is %s; LVGL is %sthe UI.", ConfigManager::lvgl_ui ? "true" : "false", LvglUi::active() ? "" : "not ");
      }
    } else if (command_string.compare("loop_watchdog") == 0) {
      // {"command": "loop_watchdog", "enable": true}: log every task's backtrace if the default event loop
      // stops dispatching (EventLoopWatchdog). Off by default. Saved and applied at once, no reboot; a repeat
      // of the current state does nothing. Without "enable" it logs the current state.
      if (json.contains("enable") && json["enable"].is_boolean()) {
        bool enable = json["enable"].get<bool>();
        if (enable != ConfigManager::loop_watchdog) {
          ConfigManager::loop_watchdog = enable;
          ESP_LOGI("UpdateManager", "loop_watchdog set to %s.", enable ? "true" : "false");
          ConfigManager::save_config();
          EventLoopWatchdog::set_enabled(enable);
        } else {
          ESP_LOGI("UpdateManager", "loop_watchdog is already %s.", enable ? "true" : "false");
        }
      } else {
        ESP_LOGI("UpdateManager", "loop_watchdog is %s.", ConfigManager::loop_watchdog ? "true" : "false");
      }
    } else if (command_string.compare("register_accept") == 0) {
      // Handled by NSPM_ConfigManager, which also listens on this topic.
    } else {
      ESP_LOGW("UpdateManager", "Unknown command: %s", command_string.c_str());
    }
  }
}

void UpdateManager::_task_reboot(void *param) {
  InterfaceManager::hold_screen_for_reboot();
  LoadingPage::show();
  LoadingPage::set_loading_text("Rebooting");
  LoadingPage::set_secondary_text("");
  StatusUpdateManager::reboot(); // Does not return.
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
