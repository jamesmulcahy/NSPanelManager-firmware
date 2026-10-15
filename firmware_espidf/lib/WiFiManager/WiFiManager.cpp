#include <ConfigManager.hpp>
#include <WiFiManager.hpp>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <string>

#if defined(NSPM_QEMU)
#include <esp_eth.h>
#include <esp_eth_mac_openeth.h>
#endif

// Helper
#define min(a, b) ((a) < (b) ? (a) : (b))

void WiFiManager::init() {
  esp_netif_init();

#if defined(NSPM_QEMU)
  // QEMU emulates no Wi-Fi radio, so there is no Wi-Fi driver to initialise.
  // start_client() brings the network up over Ethernet instead.
  uint8_t qemu_mac[6];
  esp_read_mac(qemu_mac, ESP_MAC_WIFI_STA);
  snprintf(WiFiManager::_mac_address, sizeof(WiFiManager::_mac_address), "%02X:%02X:%02X:%02X:%02X:%02X", qemu_mac[0], qemu_mac[1], qemu_mac[2], qemu_mac[3], qemu_mac[4], qemu_mac[5]);
  return;
#endif

  esp_netif_create_default_wifi_sta();

  // Create AP netif now; it gets attached when APSTA mode starts.
  // (Safe to create up front — it just isn't used in STA-only mode.)
  esp_netif_create_default_wifi_ap();

  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::_event_handler, NULL);
  esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::_event_handler, NULL);

  WiFiManager::_init_config = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t wifi_init_res = esp_wifi_init(&WiFiManager::_init_config);
  if (wifi_init_res != ESP_OK) {
    ESP_LOGE("WiFiManager", "Failed to init WiFi, error: %s", esp_err_to_name(wifi_init_res));
  }
  ESP_ERROR_CHECK(wifi_init_res);

  // Load MAC address from ESP32 into memory
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA); // Read MAC address for Wi-Fi Station
  snprintf(WiFiManager::_mac_address, sizeof(WiFiManager::_mac_address), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void WiFiManager::start_client(std::string *ssid, std::string *psk, std::string *hostname, std::string *fallback_ssid) {
  esp_log_level_set("WiFiManager", ConfigManager::log_level);
  ESP_LOGI("WiFiManager", "Configuring fallback ssid to %s", fallback_ssid->c_str());
  WiFiManager::_fallback_ssid = *fallback_ssid;

  WiFiManager::_connected = false;
  WiFiManager::_ip_info.set({});

#if defined(NSPM_QEMU)
  // Report the emulated Ethernet link through the same connected()/ip_info()
  // state, so everything downstream of WiFiManager runs unmodified. There is
  // no fallback AP: it needs Wi-Fi.
  WiFiManager::_start_qemu_ethernet(hostname);
  return;
#endif

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (esp_netif_set_hostname(netif, hostname->c_str()) != ESP_OK) {
    ESP_LOGW("WiFiManager", "Failed to set hostname!");
  }

  // Set SSID
  if (ssid->size() > sizeof(WiFiManager::_config.sta.ssid)) {
    ESP_LOGE("WiFiManager", "SSID too long, max length: %d", sizeof(WiFiManager::_config.sta.ssid));
    return;
  }
  ssid->copy((char *)WiFiManager::_config.sta.ssid, ssid->size(), 0); // Set WiFi SSID
  ESP_LOGI("WiFiManager", "Will connect to WiFi %s", WiFiManager::_config.sta.ssid);

  // Set passwork/PSK
  if (psk->size() > sizeof(WiFiManager::_config.sta.password)) {
    ESP_LOGE("WiFiManager", "PSK To long, max length: %d", sizeof(WiFiManager::_config.sta.password));
    return;
  }
  psk->copy((char *)WiFiManager::_config.sta.password, psk->size(), 0);
  WiFiManager::_has_wifi_config = true;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &WiFiManager::_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Start fallback timer and start an AP if we have not connected to WiFi within threshold
  ESP_LOGI("WiFiManager", "Starting fallback WiFi AP timeout.");
  WiFiManager::_fallback_timer = xTimerCreate("wifi_fallback", pdMS_TO_TICKS(WIFI_TIMEOUT_MS), pdFALSE, NULL, WiFiManager::_fallback_timer_callback);
  xTimerStart(WiFiManager::_fallback_timer, 0);
}

void WiFiManager::start_ap(std::string *ssid) {
  esp_log_level_set("WiFiManager", ConfigManager::log_level);
  WiFiManager::_connected = false;
  WiFiManager::_ip_info.set({});

  esp_wifi_stop();

  wifi_config_t ap_config = {};

  // Set SSID
  if (ssid->size() > sizeof(ap_config.ap.ssid)) {
    ESP_LOGE("WiFiManager", "SSID To long, max length: %d", sizeof(ap_config.ap.ssid));
    return;
  }
  ssid->copy((char *)ap_config.ap.ssid, ssid->size(), 0); // Set WiFi SSID
  ap_config.ap.ssid_len = ssid->length();
  ESP_LOGI("WiFiManager", "Will setup AP with SSID: %s", ap_config.ap.ssid);

  // Setup other default AP parameters
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = wifi_auth_mode_t::WIFI_AUTH_OPEN;

  esp_err_t wifi_mode_res = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (wifi_mode_res != ESP_OK) {
    ESP_LOGE("WiFiManager", "Failed to set WiFi mode, error: %s", esp_err_to_name(wifi_mode_res));
  }
  ESP_ERROR_CHECK(wifi_mode_res);

  esp_err_t wifi_config_res = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  if (wifi_config_res != ESP_OK) {
    ESP_LOGE("WiFiManager", "Failed to config WiFi, error: %s", esp_err_to_name(wifi_config_res));
  }
  ESP_ERROR_CHECK(wifi_config_res);

  esp_err_t wifi_start_res = esp_wifi_start();
  if (wifi_start_res != ESP_OK) {
    ESP_LOGE("WiFiManager", "Failed to start WiFi, error: %s", esp_err_to_name(wifi_start_res));
  }
  ESP_ERROR_CHECK(wifi_start_res);

  // WiFi started, start DHCP-server
  // esp_netif_dhcps_start(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"));
  esp_netif_ip_info_t ip_info;
  esp_netif_t *netif_handle = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  esp_netif_get_ip_info(netif_handle, &ip_info);
  WiFiManager::_ip_info.set(ip_info);
  char ip_addr_str[IP4ADDR_STRLEN_MAX];
  sprintf(ip_addr_str, IPSTR, IP2STR(&ip_info.ip));
  ESP_LOGI("WiFiManager", "SoftAP started with IP %s", ip_addr_str);

  if (WiFiManager::_dns_server == nullptr) {
    espp::DnsServer::Config dns_config{
        .ip_address = "192.168.4.1",
        .log_level = espp::Logger::Verbosity::INFO,
    };

    WiFiManager::_dns_server = new espp::DnsServer(dns_config);
    std::error_code ec;
    if (WiFiManager::_dns_server->start(ec)) {
      ESP_LOGI("WiFiManager", "DNS Server for captive portal popup started.");
    } else {
      ESP_LOGE("WiFiManager", "Failed to start DNS server. Captive portal popup will not work in client device. Error code: %d", ec.value());
    }
  }

  std::string captive_portal_url = "http://";
  captive_portal_url.append(ip_addr_str);

  esp_netif_dhcps_stop(netif_handle);
  esp_netif_dhcps_option(netif_handle, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void *)captive_portal_url.c_str(), captive_portal_url.length() + 1);
  esp_netif_dhcps_start(netif_handle);

  if (WiFiManager::_has_wifi_config) {
    // We are starting the fallback AP. Keep trying to reconnect to WiFi in the background.
    esp_wifi_set_config(WIFI_IF_STA, &WiFiManager::_config);
    esp_wifi_connect();
  }

  WiFiManager::_ap_active = true;
}

void WiFiManager::_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      esp_wifi_connect(); // WiFi station started, start trying to connect to configured wifi.
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      ESP_LOGI("WiFiManager", "Lost connection to Wifi, will try to reconnect.");
      WiFiManager::_connected = false;
      esp_wifi_connect(); // We lost connection, try to reconnect.
      break;
    case WIFI_EVENT_STA_CONNECTED:
      ESP_LOGI("WiFiManager", "Connected to WiFi.");
      WiFiManager::_connected = true;
      if (WiFiManager::_dns_server != nullptr) {
        WiFiManager::_dns_server->stop();
      }
      break;

    default:
      break;
    }
  } else if (event_base == IP_EVENT) {
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
      ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
      WiFiManager::_ip_info.set(event->ip_info);
      ESP_LOGI("WiFiManager", "Got IP: " IPSTR ", Netmask: " IPSTR ", Gateway: " IPSTR, IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));

      if (!WiFiManager::_ap_active) {
        ESP_LOGI("WiFiManager", "Stopping fallback WiFi AP timeout.");
        xTimerStop(WiFiManager::_fallback_timer, 0); // Stop fallback timer as we connected successfully
      } else {
        ESP_LOGI("WiFiManager", "Connected to WiFi. Closing fallback AP.");
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &WiFiManager::_config);
        esp_wifi_connect();

        WiFiManager::_ap_active = false;
      }
      break;
    }

    default:
      break;
    }
  }
}

#if defined(NSPM_QEMU)
void WiFiManager::_start_qemu_ethernet(std::string *hostname) {
  esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t *netif = esp_netif_new(&netif_config);

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.autonego_timeout_ms = 100;
  esp_eth_mac_t *eth_mac = esp_eth_mac_new_openeth(&mac_config);
  // QEMU models a standard 802.3 PHY; IDF 6 keeps only the generic driver in-tree.
  esp_eth_phy_t *eth_phy = esp_eth_phy_new_generic(&phy_config);

  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(eth_mac, eth_phy);
  esp_eth_handle_t eth_handle = NULL;
  ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
  ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handle)));

  if (esp_netif_set_hostname(netif, hostname->c_str()) != ESP_OK) {
    ESP_LOGW("WiFiManager", "Failed to set hostname!");
  }

  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::_qemu_ethernet_event_handler, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &WiFiManager::_qemu_ethernet_event_handler, NULL);

  ESP_LOGI("WiFiManager", "QEMU build: using emulated Ethernet instead of Wi-Fi.");
  ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}

void WiFiManager::_qemu_ethernet_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == ETH_EVENT) {
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
      ESP_LOGI("WiFiManager", "Ethernet link up.");
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      ESP_LOGI("WiFiManager", "Ethernet link down.");
      WiFiManager::_connected = false;
      break;
    default:
      break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    WiFiManager::_ip_info.set(event->ip_info);
    ESP_LOGI("WiFiManager", "Got IP: " IPSTR ", Netmask: " IPSTR ", Gateway: " IPSTR, IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
    // On Wi-Fi, connected() goes true on association, before DHCP. Ethernet
    // has no association step, so report connected once we are addressable.
    WiFiManager::_connected = true;
  }
}
#endif

void WiFiManager::_fallback_timer_callback(TimerHandle_t timer) {
  ESP_LOGE("WiFiManager", "Fallback WiFi AP callback called. Starting fallback AP.");
  if (WiFiManager::_ap_active) {
    return;
  }

  // Start AP in task a to handle a higher requirement of stack depth.
  if (xTaskCreate([](void *arg) {
        WiFiManager::start_ap(&WiFiManager::_fallback_ssid);
        vTaskDelete(NULL); // Start AP then exit task cleanly.
      },
                  "start_ap_fallback", 6000, NULL, 1, NULL) != ESP_OK) {
    ESP_LOGE("WiFiManager", "Failed to start task to start fallback SoftAP as we've failed to connect to WiFi.");
  }
}

std::vector<wifi_ap_record_t> WiFiManager::search_available_networks() {
  std::vector<wifi_ap_record_t> return_vector;

  // Config to scan for all networks, including hidden.
  wifi_scan_config_t scan_config = {};
  scan_config.ssid = NULL;
  scan_config.bssid = NULL;
  scan_config.channel = 0;
  scan_config.show_hidden = true;

  // Scan for networks
  esp_wifi_scan_start(&scan_config, true);

  uint16_t num_networks_found;
  esp_wifi_scan_get_ap_num(&num_networks_found);
  ESP_LOGI("WiFiManager", "Found %d networks in AP scan.", num_networks_found);

  return_vector.resize(num_networks_found);
  esp_wifi_scan_get_ap_records(&num_networks_found, return_vector.data());

  return return_vector;
}

bool WiFiManager::connected() {
  return WiFiManager::_connected;
}

esp_netif_ip_info_t WiFiManager::ip_info() {
  return WiFiManager::_ip_info.get();
}

std::string WiFiManager::ip_string() {
  char ip_str[IP4ADDR_STRLEN_MAX];
  esp_netif_ip_info_t info = WiFiManager::_ip_info.get();
  sprintf(ip_str, IPSTR, IP2STR(&info.ip));
  return std::string(ip_str);
}

const char *WiFiManager::mac_string() {
  return WiFiManager::_mac_address;
}