#pragma once
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <freertos/semphr.h>
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
   * Update firmware OTA from manager. This will also call update_littlefs.
   * Will compare stored MD5 checksum of installed firmware with the checksum in the manager.
   */
  static void update_firmware(void *param);

  /**
   * Update LittleFS OTA from manager.
   * Will compare stored MD5 checksum of installed LittleFS with the checksum in the manager.
   * The panel's settings (config.json) live on the LittleFS partition, so an update that fails
   * after the partition was erased is retried, and if it still fails the partition is formatted
   * and the settings written back before rebooting. Reboots after a successful update.
   * @param force_update: Force an update even though the stored checksum is the same.
   * @return ESP_OK if the settings are on flash, so a reboot is safe. ESP_FAIL if they could not
   *         be written back and only the copy in RAM remains: the caller must not reboot.
   */
  static esp_err_t update_littlefs(void *param, bool force_update);

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
   * @brief Handle events triggered from MQTT places.
   */
  static void _mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle events triggered from Nextion display.
   */
  static void _nextion_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Update the firmware of the ESP32 chip OTA from manager
   */
  static esp_err_t _update_firmware_ota();

  /**
   * Download the LittleFS image from the manager straight into the LittleFS partition.
   * @param erased: Set to true once the partition has been erased, i.e. once the old filesystem,
   *                and the settings in it, are gone. Left untouched if the attempt failed before that.
   * @return ESP_OK if the whole image was written.
   */
  static esp_err_t _update_littlefs_ota(bool *erased);

  /**
   * After a LittleFS update failed with the partition already erased: format it and write the
   * settings held in RAM back, so the panel keeps its Wi-Fi and MQTT settings across the reboot.
   * The web UI files are lost until the next successful LittleFS update; the stored LittleFS
   * checksum is cleared so that the manager offers one.
   */
  static esp_err_t _restore_config_after_failed_littlefs_update();

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

  // How many times to try the LittleFS download once the partition has been erased.
  static constexpr uint8_t LITTLEFS_UPDATE_ATTEMPTS = 5;

  // When performing an update, should we force the update, ie. skip the check for if we are already up to date? If true, then yes, skip check.
  static inline bool _force_update = false;
};