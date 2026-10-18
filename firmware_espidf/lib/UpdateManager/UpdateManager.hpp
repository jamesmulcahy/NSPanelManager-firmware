#pragma once
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <freertos/semphr.h>
#include <MqttSubscriptions.hpp>
#include <vector>

class UpdateManager {
public:
  /**
   * Initialize the UpdateManager. Attach event hooks and so on.
   */
  static void init();

  /**
   * Will start downloading the GUI file from the manager and set the Nextion display in update mode.
   * Will then transfer chunk by chunk from manager into the screen.
   */
  static void update_gui(void *param);

  /**
   * Update firmware OTA from manager, then reboot into it. The setup web UI is part of the firmware
   * image, so nothing else is downloaded or written.
   * Will compare stored MD5 checksum of installed firmware with the checksum in the manager.
   */
  static void update_firmware(void *param);

  /*
   * When updating the ESP32 firmware it will update one of the partitions and switch to that partition for the next boot.
   * If it then fails to boot or the boot is not marked as successful it will revert to the old firmware.
   * This function will mark the current boot as successful. For more information please see https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html#app-rollback
   */
  static void mark_boot_successful();

  /*
   * Update the stored checksum in config for what firmware is installed. This will pull the checksum from the manager.
   */
  static void update_internal_firmware_checksum();

private:
  /**
   * @brief Handle commands on the panel's command topic. Runs on the mqtt_dispatch task.
   */
  static void _mqtt_message_handler(const mqtt::Message &message, void *context);

  static inline mqtt::Subscription _command_subscription;

  /**
   * Carries out the "reboot" command. It shows a page and waits for the status report to go out, so
   * it runs on its own task rather than in the MQTT handler that received the command.
   */
  static void _task_reboot(void *param);

  /**
   * @brief Handle events triggered from Nextion display.
   */
  static void _nextion_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Update the firmware of the ESP32 chip OTA from manager
   */
  static esp_err_t _update_firmware_ota();

  /**
   * Setup an HTTP Client with the correct headers. esp_http_client_cleanup is handled in case of errors but not in case of ESP_OK.
   * @param client: Pointer to HTTP Client handle to use
   * @param return_data: Where to save received data. Mutex for _download_data_store has to be manually handled.
   * @param download_url: The URL to try to download data from
   * @param offset: Start downloading data from an offset from the remote file. -1 will not set header.
   * @param length: Number of bytes to download from the remote file. -1 will not set header.
   * @return Will return ESP_OK in case everything was OK, otherwise ESP_ERR_NOT_FINISHED.
   */
  static esp_err_t _setup_http_client(esp_http_client_handle_t *client, std::vector<uint8_t> *return_data, const char *download_url, int64_t offset, int64_t length);

  /**
   * Attempt to download data from a URL into a vector.
   * @param return_data: Where to save received data.
   * @param download_url: The URL to try to download data from
   * @param offset: Start downloading data from an offset from the remote file. -1 will not set header.
   * @param length: Number of bytes to download from the remote file. -1 will not set header.
   * @return Will return ESP_OK in case everything was OK, otherwise ESP_ERR_NOT_FINISHED.
   */
  static esp_err_t _download_data(std::vector<uint8_t> *return_data, const char *download_url, int64_t offset, int64_t length);

  /**
   * Get remote file size
   * @param remote_file_url: URL to remote file to check size of.
   * @param file_size: Pointer to where to store file size
   * @return Will return ESP_OK in case everything was OK, otherwise ESP_ERR_NOT_FINISHED.
   */
  static esp_err_t _get_remote_file_size(std::string remote_file_url, size_t *file_size);

  /**
   * @brief Request the TFT file from `offset` to its end and check the manager answered with exactly those bytes.
   * @param url: The TFT download URL.
   * @param offset: First byte to download.
   * @param file_size: Size of the whole TFT file, as found by _get_remote_file_size.
   * @param client: Set to the open client, ready for esp_http_client_read(), on success.
   * @return ESP_OK on success, otherwise an error with the client already cleaned up.
   */
  static esp_err_t _open_tft_stream(const std::string &url, uint64_t offset, uint64_t file_size, esp_http_client_handle_t *client);

  /**
   * Handle event from ESP HTTP client
   */
  static inline esp_err_t _http_event_handler(esp_http_client_event_t *event);

  // Vars:
  // Pointer to where to save downloaded data.
  static inline std::vector<uint8_t> *_download_data_store = nullptr;

  // Max number of bytes to download from remote source. Set to 0 for no limit.
  static inline uint64_t _download_data_max_bytes = 0;

  // Mutex so that _download_data_store is only accessed from one task at the time.
  static inline SemaphoreHandle_t _download_data_store_mutex;

  // If an update task is running, this is a handle for that task.
  static inline TaskHandle_t _current_update_task = NULL;

  // Next Nextion update TFT update offset.
  static inline uint64_t _nextion_update_current_offset = 0;

  // How many bytes were last written to the Nextion display
  static inline uint64_t _nextion_update_next_chunk_size = 0;

  // After the next interrupt, should we reset the HTTP client?
  static inline bool _nextion_update_reset_http_client = false;

  // When performing an update, should we force the update, ie. skip the check for if we are already up to date? If true, then yes, skip check.
  static inline bool _force_update = false;
};