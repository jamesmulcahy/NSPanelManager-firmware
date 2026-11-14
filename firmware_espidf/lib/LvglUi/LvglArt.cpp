#include <LvglArt.hpp>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "LvglArt";
static constexpr size_t IMAGE_BYTES = LvglArt::SIZE * LvglArt::SIZE * 2;

void LvglArt::fetch(const std::string &url, void (*done)(const lv_image_dsc_t *image, const std::string &url)) {
  if (LvglArt::_task_handle == NULL) {
    LvglArt::_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; i++) {
      LvglArt::_buffers[i] = static_cast<uint8_t *>(heap_caps_malloc(IMAGE_BYTES, MALLOC_CAP_SPIRAM));
    }
    if (LvglArt::_mutex == NULL || LvglArt::_buffers[0] == NULL || LvglArt::_buffers[1] == NULL ||
        xTaskCreatePinnedToCore(LvglArt::_task, "lvgl_art", 4096, NULL, 2, &LvglArt::_task_handle, 1) != pdPASS) {
      ESP_LOGE(TAG, "Failed to set up album art fetching; no art will be shown.");
      return;
    }
  }
  xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
  LvglArt::_url = url;
  LvglArt::_done = done;
  LvglArt::_generation++;
  xSemaphoreGive(LvglArt::_mutex);
  xTaskNotifyGive(LvglArt::_task_handle);
}

void LvglArt::cancel() {
  LvglArt::_generation++;
}

bool LvglArt::_fetch_into(const std::string &base_url, uint8_t *buffer) {
  // The manager's URL carries ?v=<hash>; RGB565 little endian is its default format.
  std::string url = base_url;
  url.append(url.find('?') == std::string::npos ? "?w=" : "&w=");
  url.append(std::to_string(SIZE)).append("&h=").append(std::to_string(SIZE));

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return false;
  }
  bool ok = false;
  if (esp_http_client_open(client, 0) == ESP_OK) {
    int64_t length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
      ESP_LOGW(TAG, "HTTP %d for %s", status, url.c_str()); // 404: no art for this track.
    } else if (length >= 0 && (size_t)length != IMAGE_BYTES) {
      ESP_LOGE(TAG, "Expected %u bytes of RGB565, the manager sent %lld.", (unsigned)IMAGE_BYTES, length);
    } else {
      size_t have = 0;
      while (have < IMAGE_BYTES) {
        int n = esp_http_client_read(client, reinterpret_cast<char *>(buffer) + have, IMAGE_BYTES - have);
        if (n <= 0) {
          break;
        }
        have += n;
      }
      ok = have == IMAGE_BYTES;
      if (!ok) {
        ESP_LOGE(TAG, "Album art cut short: %u of %u bytes.", (unsigned)have, (unsigned)IMAGE_BYTES);
      }
    }
    esp_http_client_close(client);
  } else {
    ESP_LOGE(TAG, "Failed to open %s", url.c_str());
  }
  esp_http_client_cleanup(client);
  return ok;
}

void LvglArt::_task(void *param) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
    std::string url = LvglArt::_url;
    auto done = LvglArt::_done;
    uint32_t generation = LvglArt::_generation;
    xSemaphoreGive(LvglArt::_mutex);

    const int into = 1 - LvglArt::_shown;
    const int64_t started = esp_timer_get_time();
    bool ok = LvglArt::_fetch_into(url, LvglArt::_buffers[into]);
    if (generation != LvglArt::_generation) {
      continue; // Superseded or cancelled while fetching; the next notification has the new one.
    }
    const lv_image_dsc_t *image = NULL;
    if (ok) {
      lv_image_dsc_t &dsc = LvglArt::_images[into];
      dsc = {};
      dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
      dsc.header.cf = LV_COLOR_FORMAT_RGB565;
      dsc.header.w = SIZE;
      dsc.header.h = SIZE;
      dsc.header.stride = SIZE * 2;
      dsc.data_size = IMAGE_BYTES;
      dsc.data = LvglArt::_buffers[into];
      LvglArt::_shown = into;
      image = &dsc;
      ESP_LOGI(TAG, "Album art fetched in %lld ms.", (esp_timer_get_time() - started) / 1000);
    }
    if (done != nullptr) {
      done(image, url);
    }
  }
}
