#include <PanelMetrics.hpp>
#ifdef NSPM_HEAP_DIAG
#include <HeapDiag.hpp>
#endif
#include <PanelRestart.hpp>
#include <ConfigManager.hpp>
#include <LvglRenderer.hpp>
#include <LvglScreenshot.hpp>
#include <LvglUi.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <Nextion.hpp>
#include <NSPM_version.hpp>
#include <WebManager.hpp>
#include <WiFiManager.hpp>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nlohmann/json.hpp>
#include <generated/web_assets.hpp>

// How long /screenshot waits for the GUI task: one refresh of what is due, then one full render. On a
// panel that is well under a second. Under QEMU the display link runs at ~8 KB/s, so whatever is
// still due may take many seconds to send first.
#ifdef NSPM_QEMU
static constexpr uint32_t SCREENSHOT_TIMEOUT_MS = 60000;
#else
static constexpr uint32_t SCREENSHOT_TIMEOUT_MS = 2000;
#endif

namespace {
// A chunked response, with small writes (the PNG framing) gathered into one chunk.
class HttpSink : public LvglScreenshot::Sink {
public:
  explicit HttpSink(httpd_req_t *req) : _req(req) {
    this->_buffer = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    this->_ok = this->_buffer != nullptr;
  }
  ~HttpSink() { heap_caps_free(this->_buffer); }

  bool write(const void *data, size_t size) override {
    if (this->_ok && this->_used + size > BUFFER_SIZE) {
      this->flush();
    }
    if (this->_ok && size <= BUFFER_SIZE) {
      memcpy(this->_buffer + this->_used, data, size);
      this->_used += size;
    } else if (this->_ok) {
      this->_ok = this->send(data, size);
    }
    this->sent += size;
    return this->_ok;
  }

  // Send what is gathered, then end the response. False if any of it failed.
  bool finish() {
    this->flush();
    return this->send(NULL, 0) && this->_ok;
  }

  size_t sent = 0;
  // Time spent sending, so that the log can tell encoding from the network.
  int64_t send_us = 0;

private:
  static constexpr size_t BUFFER_SIZE = 1024;
  void flush() {
    if (this->_ok && this->_used > 0) {
      this->_ok = this->send(this->_buffer, this->_used);
    }
    this->_used = 0;
  }
  bool send(const void *data, size_t size) {
    int64_t start = esp_timer_get_time();
    bool ok = httpd_resp_send_chunk(this->_req, static_cast<const char *>(data), size) == ESP_OK;
    this->send_us += esp_timer_get_time() - start;
    return ok;
  }
  httpd_req_t *_req;
  uint8_t *_buffer = nullptr;
  size_t _used = 0;
  bool _ok = false;
};
} // namespace

void WebManager::start() {
  if (WebManager::_server == NULL) {
    WebManager::_config = HTTPD_DEFAULT_CONFIG();
    WebManager::_config.server_port = WebManager::HTTP_PORT;
    WebManager::_config.max_uri_handlers = 16; // 12 registered (/screenshot and /metrics included; 14 with NSPM_HEAP_DIAG), with room to spare
    // The default 4 KB overflowed while /screenshot ran ROM tdefl under the PNG framing and a
    // chunked send, and 8 KB left only 412 bytes unused on a panel (PNG). See the
    // "Screenshot ..." log line for what is left unused.
    WebManager::_config.stack_size = 12288;
    // The stock config keeps an idle client connection open forever: the 5 s timeouts only cover
    // reading/sending a request. When Wi-Fi drops while a client (e.g. a periodic scraper) has a
    // connection open, its FIN never arrives and the session holds an lwIP socket indefinitely.
    // With CONFIG_LWIP_MAX_SOCKETS=10 a few such sessions exhaust the pool, after which MQTT
    // reconnects, OTA downloads and accept() itself fail with ENFILE.
    // TCP keepalive closes dead sessions after ~30 s idle; capping sessions at 4 (plus the 3 the
    // server uses internally) leaves sockets free for MQTT and HTTP clients, and LRU purge drops
    // the oldest session instead of refusing new clients when the cap is reached.
    WebManager::_config.max_open_sockets = 4;
    WebManager::_config.lru_purge_enable = true;
    WebManager::_config.keep_alive_enable = true;
    WebManager::_config.keep_alive_idle = 15;
    WebManager::_config.keep_alive_interval = 5;
    WebManager::_config.keep_alive_count = 3;
    PanelMetrics::init();

    if (httpd_start(&WebManager::_server, &WebManager::_config) == ESP_OK) {
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_index);                  // Register index view
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_save_config);            // Register URI for saving config data
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_factory_reset);          // Register URI to factory reset device
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_reboot);                 // Register URI to reboot
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_config_data);            // Register URI for config data
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_status_data);            // Register URI for status data
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_get_available_networks); // Register URI for getting list of available networks
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_static_css);             // Register static CSS uri
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_handle_generate_204);    // Handle /generate_204 for Android captive portal popup
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_handle_generate204);     // Handle /generate204 for Android captive portal popup
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_screenshot);             // The LVGL screen as an image
      httpd_register_uri_handler(WebManager::_server, &WebManager::_uri_metrics);                // Prometheus scrape endpoint
#ifdef NSPM_HEAP_DIAG
      HeapDiag::register_uris(WebManager::_server); // 2 more, 14 in all
#endif
    } else {
      ESP_LOGE("Web", "Failed to start web server!");
    }
  }
}

esp_err_t WebManager::_handle_uri_index(httpd_req_t *req) {
  return WebManager::_send_embedded_gzip(req, "text/html", web_assets::INDEX_HTML_GZ, web_assets::INDEX_HTML_GZ_LEN);
}

esp_err_t WebManager::_handle_uri_config_data(httpd_req_t *req) {
  nlohmann::json json;
  json["version"] = NSPM_VERSION;
  json["log_level"] = static_cast<int>(ConfigManager::log_level);
  json["serial_log"] = static_cast<int>(ConfigManager::serial_log);
  json["wifi_hostname"] = ConfigManager::wifi_hostname.c_str();
  json["wifi_ssid"] = ConfigManager::wifi_ssid.c_str();
  json["wifi_psk_set"] = !ConfigManager::wifi_psk.empty();
  json["mqtt_server"] = ConfigManager::mqtt_server.c_str();
  json["mqtt_port"] = ConfigManager::mqtt_port;
  json["mqtt_username"] = ConfigManager::mqtt_username.c_str();
  json["mqtt_psk_set"] = !ConfigManager::mqtt_password.empty();
  json["use_latest_nextion_upload_protocol"] = ConfigManager::use_latest_nextion_upload_protocol;
  json["nextion_upload_baudrate"] = ConfigManager::nextion_upload_baudrate;
  json["communication_baud_rate"] = ConfigManager::communication_baud_rate;
  json["operating_baud_rate"] = ConfigManager::operating_baud_rate;
  json["nextion_link_baud"] = Nextion::get_link_baud();

  if (httpd_resp_set_type(req, "application/json") != ESP_OK) {
    ESP_LOGE("Web", "Failed to set content type for response!");
  }

  std::string json_string = json.dump();
  httpd_resp_send_chunk(req, json_string.c_str(), json_string.length());
  httpd_resp_send_chunk(req, NULL, 0);

  return ESP_OK;
}

esp_err_t WebManager::_handle_uri_save_config(httpd_req_t *req) {
  std::vector<char> data(req->content_len + 1);
  httpd_req_recv(req, data.data(), req->content_len);
  data[req->content_len] = '\0';

  // Decode form data into result unordered map
  std::unordered_map<std::string, std::string> result;
  if (WebManager::_decode_form_data(data.data(), &result) != ESP_OK) {
    ESP_LOGE("WebManager", "Failed to decode form data into parameters! Form data: %s", data.data());
    return ESP_ERR_INVALID_ARG;
  }

  if (result.find("wifi_hostname") != result.end()) {
    ConfigManager::wifi_hostname = result["wifi_hostname"];
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'wifi_hostname' while saving config from web post request!");
  }

  if (result.find("log_level") != result.end()) {
    ConfigManager::log_level = static_cast<esp_log_level_t>(atoi(result["log_level"].c_str()));
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'log_level' while saving config from web post request!");
  }

  // Missing when the form that posted this is from an older page. Keep the current value then.
  if (result.find("serial_log") != result.end()) {
    int serial_log = atoi(result["serial_log"].c_str());
    if (serial_log >= 0 && serial_log <= static_cast<int>(ConfigManager::SerialLogMode::NEVER)) {
      ConfigManager::serial_log = static_cast<ConfigManager::SerialLogMode>(serial_log);
    } else {
      ESP_LOGW("WebManager", "Ignoring invalid serial_log value '%s' from web post request.", result["serial_log"].c_str());
    }
  } else {
    ESP_LOGD("WebManager", "No 'serial_log' field in web post request, keeping current setting.");
  }

  if (result.find("wifi_ssid") != result.end()) {
    ConfigManager::wifi_ssid = result["wifi_ssid"];
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'wifi_ssid' while saving config from web post request!");
  }

  if (result.find("clear_wifi_password") != result.end()) {
    ConfigManager::wifi_psk = "";
  } else if (result.find("wifi_psk") != result.end()) {
    if (!result["wifi_psk"].empty()) {
      ConfigManager::wifi_psk = result["wifi_psk"];
    }
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'wifi_psk' while saving config from web post request!");
  }

  if (result.find("mqtt_server") != result.end()) {
    ConfigManager::mqtt_server = result["mqtt_server"];
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'mqtt_server' while saving config from web post request!");
  }

  if (result.find("mqtt_port") != result.end()) {
    ConfigManager::mqtt_port = atoi(result["mqtt_port"].c_str());
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'mqtt_port' while saving config from web post request!");
  }

  if (result.find("mqtt_username") != result.end()) {
    ConfigManager::mqtt_username = result["mqtt_username"];
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'mqtt_username' while saving config from web post request!");
  }

  if (result.find("clear_mqtt_password") != result.end()) {
    ConfigManager::mqtt_password = "";
  } else if (result.find("mqtt_psk") != result.end()) {
    if (!result["mqtt_psk"].empty()) {
      ConfigManager::mqtt_password = result["mqtt_psk"];
    }
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'mqtt_psk' while saving config from web post request!");
  }

  if (result.find("upload_protocol") != result.end()) {
    if (result["upload_protocol"].compare("latest") == 0) {
      ConfigManager::use_latest_nextion_upload_protocol = true;
    } else {
      ConfigManager::use_latest_nextion_upload_protocol = false;
    }
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'upload_protocol' while saving config from web post request!");
  }

  if (result.find("upload_baud_rate") != result.end()) {
    ConfigManager::nextion_upload_baudrate = atoi(result["upload_baud_rate"].c_str());
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'upload_baud_rate' while saving config from web post request!");
  }

  if (result.find("communication_baud_rate") != result.end()) {
    ConfigManager::communication_baud_rate = atoi(result["communication_baud_rate"].c_str());
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'communication_baud_rate' while saving config from web post request!");
  }

  if (result.find("operating_baud_rate") != result.end()) {
    ConfigManager::operating_baud_rate = atoi(result["operating_baud_rate"].c_str());
  } else {
    ESP_LOGE("WebManager", "Could not find field name 'operating_baud_rate' while saving config from web post request!");
  }

  httpd_resp_set_status(req, "302 Found");  // Set the status code
  httpd_resp_set_hdr(req, "Location", "/"); // Set the Location header
  httpd_resp_send(req, NULL, 0);            // Send the response

  ConfigManager::save_config();
  PanelRestart::restart();
  return ESP_OK;
}

esp_err_t WebManager::_handle_uri_reboot(httpd_req_t *req) {
  ESP_LOGI("WebManager", "Reboot requested from web interface.");

  httpd_resp_set_status(req, "302 Found");  // Set the status code
  httpd_resp_set_hdr(req, "Location", "/"); // Set the Location header
  httpd_resp_send(req, NULL, 0);            // Send the response

  vTaskDelay(pdMS_TO_TICKS(250));
  PanelRestart::restart();
  return ESP_OK;
}

esp_err_t WebManager::_handle_uri_factory_reset(httpd_req_t *req) {
  ESP_LOGI("WebManager", "Factory reset requested from web interface.");
  vTaskDelay(pdMS_TO_TICKS(250));

  ConfigManager::create_default();
  ConfigManager::save_config();
  PanelRestart::restart();
  return ESP_OK;
}

esp_err_t WebManager::_decode_form_data(char *source, std::unordered_map<std::string, std::string> *dest) {
  // Decode URL form data, example data:
  // wifi_hostname=NSPMPanel-FA5734&log_level=2&wifi_ssid=%C3%A5%C3%A4%C3%B6%21%22%23%C2%A4%25%26%2F%28%29%3D%3F%60%3B%3A_&wifi_psk=password&mqtt_server=10.2.0.4&mqtt_port=1883&mqtt_username=&mqtt_psk=&upload_protocol=latest&upload_buad_rate=115200

  std::string field;         // Currently reading field name
  std::string data;          // Currently reading field data
  bool reading_field = true; // Are we reading field name or data? True for field name

  while (*source) {
    if (*source == '=') {
      reading_field = !reading_field;
      source++;
    } else if (*source == '&') {
      // We've now read a whole field, this is the split between two fields
      reading_field = true; // Reset, the next field name is to be read
      (*dest)[field] = data;
      field.clear();
      data.clear();
      source++;
    } else if (*source == '%') { // We are reading base64 (ie. URI encoded) data
      int value;
      sscanf(source + 1, "%2X", &value); // Read the hex string from the source
      if (reading_field) {
        field.push_back((char)value);
      } else {
        data.push_back((char)value);
      }
      source += 3; // Move pas current hex string
    } else if (*source == '+') {
      if (reading_field) {
        field.push_back(' ');
      } else {
        data.push_back(' ');
      }
      source++;
    } else {
      if (reading_field) {
        field.push_back(*source++);
      } else {
        data.push_back(*source++);
      }
    }
  }
  // The last field doesn't contain an &-sign to separate it from other fields, therefore we add it manually here.
  (*dest)[field] = data;
  field.clear();
  data.clear();

  return ESP_OK;
}

esp_err_t WebManager::_handle_uri_status_data(httpd_req_t *req) {
  nlohmann::json json;
  if (json == NULL) {
    ESP_LOGE("WebManager", "Failed to create JSON-object when handling request for status data!");
    return ESP_ERR_NO_MEM;
  }

  wifi_mode_t current_wifi_mode;
  if (esp_wifi_get_mode(&current_wifi_mode) == ESP_OK) {
    if (current_wifi_mode == wifi_mode_t::WIFI_MODE_STA) {
      if (WiFiManager::connected()) {
        json["wifi_status"] = "Connected";
      } else {
        json["wifi_status"] = "Disconnected";
      }
    } else if (current_wifi_mode == wifi_mode_t::WIFI_MODE_AP) {
      json["wifi_status"] = "Access Point";
    } else if (current_wifi_mode == wifi_mode_t::WIFI_MODE_APSTA) {
      json["wifi_status"] = "Access Point/Station";
    } else {
      json["wifi_status"] = "Unknown WiFi mode!";
    }
  } else {
    json["wifi_status"] = "Failed to get mode!";
  }

  if (MqttManager::connected()) {
    json["mqtt_status"] = "Connected";
  } else {
    json["mqtt_status"] = "Disconnected";
  }

  std::string manager_address = NSPM_ConfigManager::get_manager_address();
  if (MqttManager::connected()) {
    if (manager_address.empty()) {
      json["nspanelmanager_status"] = "Searching";
    } else {
      uint16_t manager_port = NSPM_ConfigManager::get_manager_port();
      std::string ret_string = "Accepted by ";
      ret_string.append(manager_address);
      ret_string.append(":");
      ret_string.append(std::to_string(manager_port));
      json["nspanelmanager_status"] = ret_string.c_str();
    }
  } else {
    json["nspanelmanager_status"] = "Disconnected";
  }

  if (httpd_resp_set_type(req, "application/json") != ESP_OK) {
    ESP_LOGE("Web", "Failed to set content type for response!");
  }

  std::string json_string = json.dump();
  httpd_resp_send_chunk(req, json_string.c_str(), json_string.length());
  httpd_resp_send_chunk(req, NULL, 0);

  return ESP_OK;
}

static bool send_metrics_chunk(void *req, const char *data, size_t length) {
  return httpd_resp_send_chunk(static_cast<httpd_req_t *>(req), data, length) == ESP_OK;
}

esp_err_t WebManager::_handle_uri_metrics(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
  esp_err_t result = PanelMetrics::write(send_metrics_chunk, req);
  if (result == ESP_ERR_TIMEOUT) {
    // Nothing has been sent yet, so the status can still be set.
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, NULL, 0);
  }
  if (result != ESP_OK) {
    return ESP_FAIL; // The client went away mid-response: have the server close the socket.
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t WebManager::_handle_uri_main_css(httpd_req_t *req) {
  return WebManager::_send_embedded_gzip(req, "text/css", web_assets::MAIN_CSS_GZ, web_assets::MAIN_CSS_GZ_LEN);
}

esp_err_t WebManager::_handle_uri_generate_204(httpd_req_t *req) {
  ESP_LOGI("WebManager", "Responding to /generate_204 request.");
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  return httpd_resp_send(req, NULL, 0); // empty body, sets Content-Length: 0
}

esp_err_t WebManager::_send_embedded_gzip(httpd_req_t *req, const char *content_type, const uint8_t *data, size_t length) {
  if (httpd_resp_set_type(req, content_type) != ESP_OK) {
    ESP_LOGE("Web", "Failed to set content type for response!");
  }
  // The bytes are sent as stored and the browser decompresses them, so the panel needs no zlib.
  if (httpd_resp_set_hdr(req, "Content-Encoding", "gzip") != ESP_OK) {
    ESP_LOGE("Web", "Failed to set content encoding for response!");
  }
  return httpd_resp_send(req, reinterpret_cast<const char *>(data), length);
}

esp_err_t WebManager::_handle_uri_get_available_networks(httpd_req_t *req) {
  nlohmann::json json = nlohmann::json::array();

  // Get list of available networks
  std::vector<wifi_ap_record_t> networks = WiFiManager::search_available_networks();
  for (int i = 0; i < networks.size(); i++) {
    // Skip networks without names
    if (strlen((const char *)networks[i].ssid) == 0) {
      continue;
    }

    nlohmann::json network_item = nlohmann::json();

    network_item["ssid"] = (const char *)networks[i].ssid;
    network_item["security"] = static_cast<int>(networks[i].authmode);
    network_item["rssi"] = networks[i].rssi;
    json.push_back(network_item);
  }

  if (httpd_resp_set_type(req, "application/json") != ESP_OK) {
    ESP_LOGE("Web", "Failed to set content type for response!");
  }

  std::string json_string = json.dump();
  httpd_resp_send_chunk(req, json_string.c_str(), json_string.length());
  httpd_resp_send_chunk(req, NULL, 0);

  return ESP_OK;
}

esp_err_t WebManager::_handle_uri_screenshot(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (!LvglUi::active()) {
    // On NSPM's TFT the Nextion draws its own pages: the pixels never pass through the ESP32.
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "No screenshot: this panel shows the Nextion display's own pages, which the firmware cannot read back. "
                                   "Screenshots need the LVGL UI (an nspanel-terminal TFT).\n");
  }

  char query[64];
  char format[8] = "png";
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char value[sizeof(format)];
    if (httpd_query_key_value(query, "format", value, sizeof(value)) == ESP_OK) {
      strcpy(format, value);
    }
  }
  bool png = strcmp(format, "png") == 0, bmp = strcmp(format, "bmp") == 0, raw = strcmp(format, "raw") == 0;
  if (!png && !bmp && !raw) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "format must be png, bmp or raw.\n");
  }

  int64_t start = esp_timer_get_time();
  std::shared_ptr<const LvglRenderer::Screenshot> shot;
  esp_err_t captured = LvglRenderer::capture(SCREENSHOT_TIMEOUT_MS, &shot);
  if (captured != ESP_OK) {
    const char *why = captured == ESP_ERR_NOT_FINISHED ? "another screenshot is under way"
                      : captured == ESP_ERR_NO_MEM     ? "no memory for it"
                      : captured == ESP_ERR_TIMEOUT    ? "the UI did not get to it in time"
                                                       : "the UI is busy";
    ESP_LOGW("WebManager", "No screenshot: %s.", why);
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Retry-After", "1");
    httpd_resp_set_type(req, "text/plain");
    std::string text = std::string("No screenshot: ") + why + ".\n";
    return httpd_resp_sendstr(req, text.c_str());
  }
  int64_t captured_at = esp_timer_get_time();

  // Header values must stay valid until the response is sent.
  char width[8], height[8], capture_ms[12], disposition[80];
  snprintf(width, sizeof(width), "%u", shot->width);
  snprintf(height, sizeof(height), "%u", shot->height);
  snprintf(capture_ms, sizeof(capture_ms), "%lu", (unsigned long)shot->capture_ms);
  std::string name;
  for (char c : ConfigManager::wifi_hostname) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_') {
      name.push_back(c);
    }
  }
  snprintf(disposition, sizeof(disposition), "inline; filename=\"%.40s.%s\"", name.empty() ? "nspanel" : name.c_str(), format);
  httpd_resp_set_type(req, png ? "image/png" : bmp ? "image/bmp" : "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", disposition);
  httpd_resp_set_hdr(req, "X-Width", width);
  httpd_resp_set_hdr(req, "X-Height", height);
  httpd_resp_set_hdr(req, "X-Display-Streaming", shot->in_sync ? "1" : "0");
  httpd_resp_set_hdr(req, "X-Capture-Ms", capture_ms);

  bool sent;
  size_t bytes;
  int64_t send_us;
  if (raw) {
    httpd_resp_set_hdr(req, "X-Pixel-Format", "RGB565LE");
    bytes = (size_t)shot->width * shot->height * 2;
    sent = httpd_resp_send(req, reinterpret_cast<const char *>(shot->pixels), bytes) == ESP_OK;
    send_us = esp_timer_get_time() - captured_at;
  } else {
    HttpSink sink(req);
    bool written = png ? LvglScreenshot::write_png(shot->pixels, shot->width, shot->height, sink)
                       : LvglScreenshot::write_bmp(shot->pixels, shot->width, shot->height, sink);
    sent = sink.finish() && written;
    bytes = sink.sent;
    send_us = sink.send_us;
  }
  int64_t end = esp_timer_get_time();
  ESP_LOGI("WebManager", "Screenshot %s %ux%u, %u bytes%s: capture %lu ms (GUI task %lu ms), encode %lu ms, send %lu ms. Stack: %u bytes never used.",
           format, shot->width, shot->height, (unsigned)bytes, sent ? "" : " (send failed)", (unsigned long)((captured_at - start) / 1000),
           (unsigned long)shot->capture_ms, (unsigned long)((end - captured_at - send_us) / 1000), (unsigned long)(send_us / 1000),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  return sent ? ESP_OK : ESP_FAIL;
}
