#pragma once
#include <MutexWrapper.hpp>
#include <atomic>
#include <dns_server.hpp>
#include <esp_wifi.h>
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

private:
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
};