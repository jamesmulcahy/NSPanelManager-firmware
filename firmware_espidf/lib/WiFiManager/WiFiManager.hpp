#pragma once
#include <MutexWrapper.hpp>
#include <atomic>
#include <dns_server.hpp>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string>
#include <vector>

#define WIFI_TIMEOUT_MS (1 * 60 * 1000)

class WiFiManager {
public:
  // Initialize WiFi
  static void init();

  /**
   * @brief Start the WiFi client in STA-mode and try to connect to given SSID.
   * @param ssid: The name of the WiFi to connect to.
   * @param psk: The password to connect to the WiFi.
   * @param hostname: The hostname of this device
   */
  static void start_client(std::string *ssid, std::string *psk, std::string *hostname, std::string *fallback_ssid);

  /**
   * @brief Start the WiFi access point for initial configuration.
   * @param ssid: The name of the WiFi to broadcast.
   * @param psk: The password of the WiFi.
   */
  static void start_ap(std::string *ssid);

  /**
   * @brief Get a list of available networks
   * @return A std::vector with entities of type wifi_ap_record_t that contains information about the network
   */
  static std::vector<wifi_ap_record_t> search_available_networks();

  /**
   * @brief Check if WiFi is connected.
   * @return True if connected, otherwise false.
   */
  static bool connected();

  /**
   * @brief Get the last esp_netif_ip_info_t that was handed to the ESP32
   * @return Latest network IP information handed to the ESP32.
   */
  static esp_netif_ip_info_t ip_info();

  /**
   * @brief Get current IP address formatted as a string
   * @return Current IP formatted as string in format 111.222.333.444
   */
  static std::string ip_string();

  /**
   * @brief Get MAC address formatted as a string
   * @return MAC formatted as string in format AA:BB:CC:DD:EE:FF
   */
  static const char *mac_string();

  /**
   * @brief Say whether the page now shown wants the Wi-Fi radio kept awake.
   * With the IDF default (WIFI_PS_MIN_MODEM) the radio sleeps between the AP's beacons and the AP
   * holds every packet for the panel until the next one, which adds up to a few hundred ms to each
   * incoming packet. Low latency (WIFI_PS_NONE) keeps the radio awake, at the cost of power and heat.
   * An update in progress (see set_update_in_progress()) keeps the radio awake whatever is asked here.
   * Only calls into the Wi-Fi driver when the resulting mode changes.
   * @param low_latency: True for WIFI_PS_NONE, false for WIFI_PS_MIN_MODEM.
   */
  static void set_low_latency(bool low_latency);

  /**
   * @brief Keep the Wi-Fi radio awake for the length of a firmware, LittleFS or GUI update.
   * The download runs for minutes, and in modem sleep every downlink packet waits for a beacon.
   * A reconnect during the update shows the screensaver again, which would otherwise put the radio
   * back into modem sleep halfway through the download.
   * Call with true when the update task starts and false when it ends without rebooting.
   */
  static void set_update_in_progress(bool in_progress);

private:
  // Apply the power save mode that the page and any update in progress call for.
  static void _apply_power_save();

  static void _event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  // This callback is run if the WiFi has not been connected within set limit and start the AP as a "backup"
  // to get back into the NSPanel.
  static void _fallback_timer_callback(TimerHandle_t timer);

#if defined(NSPM_QEMU)
  static void _start_qemu_ethernet(std::string *hostname);
  static void _qemu_ethernet_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
#endif

  // Timer to start the fallback AP if wifi is not connected within WIFI_TIMEOUT_MS threshold
  static inline TimerHandle_t _fallback_timer;

  // DNS Server to handle captive portal
  static inline espp::DnsServer *_dns_server = nullptr;

  // WiFi initialization config
  static inline wifi_init_config_t _init_config;

  // WiFi connection config
  static inline wifi_config_t _config;

  // Is the WiFi connected?
  static inline std::atomic<bool> _connected = false;

  // Is the WiFi connected?
  static inline std::atomic<bool> _ap_active = false;

  // Current IP address
  static inline MutexWrapped<esp_netif_ip_info_t> _ip_info;

  // Connection default for WiFi to connect to
  static inline std::string _fallback_ssid;

  // Flag to indicate to start_ap function that the _config variable is in fact configured and we are starting a fallback AP.
  static inline bool _has_wifi_config = false;

  // MAC address of ESP32 WiFi
  static inline char _mac_address[18];

  // Whether the shown page wants the radio awake (set_low_latency()).
  static inline std::atomic<bool> _page_wants_low_latency = true;
  // Whether an update is running (set_update_in_progress()). Overrides the page.
  static inline std::atomic<bool> _update_in_progress = false;
  // Serializes _apply_power_save(), which is called from both the page_transitions task and the update tasks.
  static inline SemaphoreHandle_t _power_save_mutex = NULL;
  // Power save mode last applied, under _power_save_mutex: 1 low latency, 0 power saving, -1 not set (or the last attempt failed).
  static inline int8_t _applied_low_latency = -1;
};