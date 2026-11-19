#include <LvglArt.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mutex>
#include <strings.h>

static const char *TAG = "LvglArt";

LvglArt::_Slot LvglArt::_slots[LvglArt::SLOTS];
std::vector<LvglArt::_Request> LvglArt::_requests;

// --- Handle -------------------------------------------------------------------------------------

LvglArt::Handle::Handle(const Handle &other) : _slot(other._slot), _image(other._image) {
  if (this->_slot >= 0) {
    LvglArt::_retain(this->_slot);
  }
}

LvglArt::Handle::Handle(Handle &&other) noexcept : _slot(other._slot), _image(other._image) {
  other._slot = -1;
  other._image = nullptr;
}

LvglArt::Handle &LvglArt::Handle::operator=(const Handle &other) {
  if (this != &other) {
    // Hold the new image before letting go of the old one: they may be the same slot.
    if (other._slot >= 0) {
      LvglArt::_retain(other._slot);
    }
    this->reset();
    this->_slot = other._slot;
    this->_image = other._image;
  }
  return *this;
}

LvglArt::Handle &LvglArt::Handle::operator=(Handle &&other) noexcept {
  if (this != &other) {
    this->reset();
    this->_slot = other._slot;
    this->_image = other._image;
    other._slot = -1;
    other._image = nullptr;
  }
  return *this;
}

LvglArt::Handle::~Handle() {
  this->reset();
}

void LvglArt::Handle::reset() {
  if (this->_slot >= 0) {
    LvglArt::_release(this->_slot);
  }
  this->_slot = -1;
  this->_image = nullptr;
}

// --- Bookkeeping (mutex held unless stated) -------------------------------------------------------

bool LvglArt::_setup() {
  static std::once_flag once;
  std::call_once(once, []() {
    LvglArt::_mutex = xSemaphoreCreateMutex();
    // Internal RAM stack: callbacks redraw pages from here, as they did before the cache.
    if (LvglArt::_mutex == NULL || xTaskCreatePinnedToCore(LvglArt::_task, "lvgl_art", 4096, NULL, 2, &LvglArt::_task_handle, 1) != pdPASS) {
      LvglArt::_task_handle = NULL;
      ESP_LOGE(TAG, "Failed to set up album art fetching; no art will be shown.");
    }
  });
  return LvglArt::_task_handle != NULL;
}

void LvglArt::_retain(int slot) {
  // Only from an existing hold, so refs is already > 0 and the slot can't be evicted meanwhile.
  xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
  LvglArt::_slots[slot].refs++;
  xSemaphoreGive(LvglArt::_mutex);
}

void LvglArt::_release(int slot) {
  xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
  _Slot &entry = LvglArt::_slots[slot];
  if (entry.refs > 0 && --entry.refs == 0) {
    entry.last_used = ++LvglArt::_clock; // Just on screen: the last to be evicted.
  }
  xSemaphoreGive(LvglArt::_mutex);
}

LvglArt::Handle LvglArt::_hold(int slot) {
  _Slot &entry = LvglArt::_slots[slot];
  entry.refs++;
  entry.last_used = ++LvglArt::_clock;
  return Handle(slot, &entry.dsc);
}

int LvglArt::_find(const std::string &url, uint16_t size) {
  for (int i = 0; i < SLOTS; i++) {
    const _Slot &entry = LvglArt::_slots[i];
    if (entry.state == _SlotState::READY && entry.size == size && entry.url == url) {
      return i;
    }
  }
  return -1;
}

int LvglArt::_reserve(const std::string &url, uint16_t size) {
  const size_t bytes = (size_t)size * size * 2;
  int chosen = -1;
  // An empty slot already the right size, then any empty slot, then the least recently used entry nobody holds.
  for (int i = 0; i < SLOTS && chosen < 0; i++) {
    if (LvglArt::_slots[i].state == _SlotState::EMPTY && LvglArt::_slots[i].capacity == bytes) {
      chosen = i;
    }
  }
  for (int i = 0; i < SLOTS && chosen < 0; i++) {
    if (LvglArt::_slots[i].state == _SlotState::EMPTY) {
      chosen = i;
    }
  }
  if (chosen < 0) {
    for (int i = 0; i < SLOTS; i++) {
      const _Slot &entry = LvglArt::_slots[i];
      if (entry.state == _SlotState::READY && entry.refs == 0 && (chosen < 0 || entry.last_used < LvglArt::_slots[chosen].last_used)) {
        chosen = i;
      }
    }
  }
  if (chosen >= 0) {
    _Slot &entry = LvglArt::_slots[chosen];
    entry.state = _SlotState::FETCHING;
    entry.url = url;
    entry.size = size;
    entry.dsc = {};
  }
  return chosen;
}

bool LvglArt::_allocate(int slot, size_t bytes) {
  _Slot &entry = LvglArt::_slots[slot];
  if (entry.pixels != nullptr && entry.capacity == bytes) {
    return true;
  }
  heap_caps_free(entry.pixels);
  entry.pixels = nullptr;
  entry.capacity = 0;

  size_t total = 0;
  for (const _Slot &other : LvglArt::_slots) {
    total += other.capacity;
  }
  while (total + bytes > BUDGET_BYTES) {
    // Free the memory of an empty slot first, then of the least recently used entry nobody holds.
    int victim = -1;
    for (int i = 0; i < SLOTS && victim < 0; i++) {
      if (i != slot && LvglArt::_slots[i].state == _SlotState::EMPTY && LvglArt::_slots[i].capacity > 0) {
        victim = i;
      }
    }
    if (victim < 0) {
      for (int i = 0; i < SLOTS; i++) {
        const _Slot &other = LvglArt::_slots[i];
        if (i != slot && other.state == _SlotState::READY && other.refs == 0 && (victim < 0 || other.last_used < LvglArt::_slots[victim].last_used)) {
          victim = i;
        }
      }
    }
    if (victim < 0) {
      return false; // Everything else is held or being fetched.
    }
    _Slot &freed = LvglArt::_slots[victim];
    total -= freed.capacity;
    heap_caps_free(freed.pixels);
    freed.pixels = nullptr;
    freed.capacity = 0;
    freed.state = _SlotState::EMPTY;
    freed.url.clear();
    freed.dsc = {};
  }
  entry.pixels = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  if (entry.pixels == nullptr) {
    return false;
  }
  entry.capacity = bytes;
  return true;
}

// --- API --------------------------------------------------------------------------------------------

LvglArt::Handle LvglArt::lookup(const std::string &url, uint16_t size) {
  if (url.empty() || !LvglArt::_setup()) {
    return Handle();
  }
  xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
  int slot = LvglArt::_find(url, size);
  Handle handle = slot >= 0 ? LvglArt::_hold(slot) : Handle();
  xSemaphoreGive(LvglArt::_mutex);
  if (slot >= 0) {
    ESP_LOGD(TAG, "cache hit: %ux%u in slot %d for %s", size, size, slot, url.c_str());
  }
  return handle;
}

void LvglArt::fetch(const std::string &url, uint16_t size, Done done) {
  if (url.empty() || size == 0 || size > MAX_SIZE) {
    ESP_LOGE(TAG, "Not fetching album art at %u px for '%s'.", size, url.c_str());
    return;
  }
  if (!LvglArt::_setup()) {
    return;
  }
  bool joined = false;
  std::string dropped;
  xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
  auto request = std::find_if(LvglArt::_requests.begin(), LvglArt::_requests.end(), [&](const _Request &r) { return r.size == size && r.url == url; });
  if (request != LvglArt::_requests.end()) {
    joined = true;
    if (done != nullptr && std::find(request->done.begin(), request->done.end(), done) == request->done.end()) {
      request->done.push_back(done);
    }
  } else {
    if (LvglArt::_requests.size() >= SLOTS) {
      auto oldest = std::find_if(LvglArt::_requests.begin(), LvglArt::_requests.end(), [](const _Request &r) { return !r.started; });
      if (oldest != LvglArt::_requests.end()) {
        dropped = oldest->url;
        LvglArt::_requests.erase(oldest);
      }
    }
    _Request added = {.url = url, .size = size, .done = {}, .started = false};
    if (done != nullptr) {
      added.done.push_back(done);
    }
    LvglArt::_requests.push_back(std::move(added));
  }
  xSemaphoreGive(LvglArt::_mutex);
  if (joined) {
    ESP_LOGD(TAG, "Joined the request for %ux%u: %s", size, size, url.c_str());
  } else {
    if (!dropped.empty()) {
      ESP_LOGW(TAG, "Too many album art requests; dropped the oldest: %s", dropped.c_str());
    }
    xTaskNotifyGive(LvglArt::_task_handle);
  }
}

// --- The task ---------------------------------------------------------------------------------------

namespace {
struct ImageHeaders {
  int width = 0;
  int height = 0;
};
} // namespace

static esp_err_t on_http_event(esp_http_client_event_t *event) {
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->user_data != NULL && event->header_key != NULL && event->header_value != NULL) {
    ImageHeaders *headers = static_cast<ImageHeaders *>(event->user_data);
    if (strcasecmp(event->header_key, "X-Image-Width") == 0) {
      headers->width = atoi(event->header_value);
    } else if (strcasecmp(event->header_key, "X-Image-Height") == 0) {
      headers->height = atoi(event->header_value);
    }
  }
  return ESP_OK;
}

bool LvglArt::_fetch_into(const std::string &base_url, uint16_t size, int slot) {
  // The manager's URL carries ?v=<hash>; RGB565 little endian is its default format.
  std::string url = base_url;
  url.append(url.find('?') == std::string::npos ? "?w=" : "&w=");
  url.append(std::to_string(size)).append("&h=").append(std::to_string(size));

  ImageHeaders headers;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 5000;
  config.event_handler = on_http_event;
  config.user_data = &headers;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return false;
  }
  bool ok = false;
  if (esp_http_client_open(client, 0) == ESP_OK) {
    int64_t length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    // The size the manager says it sent, else the size asked for.
    const int width = headers.width > 0 ? headers.width : size;
    const int height = headers.height > 0 ? headers.height : size;
    const size_t bytes = (size_t)width * height * 2;
    if (status != 200) {
      ESP_LOGW(TAG, "HTTP %d for %s", status, url.c_str()); // 404: no art for this track.
    } else if (width > MAX_SIZE || height > MAX_SIZE) {
      ESP_LOGE(TAG, "The manager sent %dx%d album art for %u px; at most %u is drawn.", width, height, size, MAX_SIZE);
    } else if (length >= 0 && (size_t)length != bytes) {
      ESP_LOGE(TAG, "Expected %u bytes of %dx%d RGB565, the manager sent %lld.", (unsigned)bytes, width, height, length);
    } else {
      xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
      bool allocated = LvglArt::_allocate(slot, bytes);
      xSemaphoreGive(LvglArt::_mutex);
      if (!allocated) {
        ESP_LOGW(TAG, "No room for %u bytes of album art (cache of %u KB, every other entry held or no PSRAM).", (unsigned)bytes, (unsigned)(BUDGET_BYTES / 1024));
      } else {
        // The slot is FETCHING: no Handle can reach it, so nothing draws from these pixels meanwhile.
        _Slot &entry = LvglArt::_slots[slot];
        size_t have = 0;
        while (have < bytes) {
          int n = esp_http_client_read(client, reinterpret_cast<char *>(entry.pixels) + have, bytes - have);
          if (n <= 0) {
            break;
          }
          have += n;
        }
        ok = have == bytes;
        if (ok) {
          entry.dsc = {};
          entry.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
          entry.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
          entry.dsc.header.w = width;
          entry.dsc.header.h = height;
          entry.dsc.header.stride = width * 2;
          entry.dsc.data_size = bytes;
          entry.dsc.data = entry.pixels;
        } else {
          ESP_LOGE(TAG, "Album art cut short: %u of %u bytes.", (unsigned)have, (unsigned)bytes);
        }
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
    // Serve every request waiting, oldest first, one at a time.
    for (;;) {
      std::string url;
      uint16_t size = 0;
      int slot = -1;
      bool hit = false;
      std::vector<Done> done;
      std::vector<Handle> handles;

      xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
      auto request = std::find_if(LvglArt::_requests.begin(), LvglArt::_requests.end(), [](const _Request &r) { return !r.started; });
      if (request == LvglArt::_requests.end()) {
        xSemaphoreGive(LvglArt::_mutex);
        break;
      }
      url = request->url;
      size = request->size;
      slot = LvglArt::_find(url, size);
      if (slot >= 0) {
        // Already in memory: answer at once.
        hit = true;
        done = std::move(request->done);
        LvglArt::_requests.erase(request);
        for (size_t i = 0; i < done.size(); i++) {
          handles.push_back(LvglArt::_hold(slot));
        }
      } else {
        request->started = true;
        slot = LvglArt::_reserve(url, size);
      }
      xSemaphoreGive(LvglArt::_mutex);

      if (!hit) {
        bool ok = false;
        if (slot < 0) {
          ESP_LOGW(TAG, "Every album art slot is held or busy; not fetching %s", url.c_str());
        } else {
          const int64_t started = esp_timer_get_time();
          ok = LvglArt::_fetch_into(url, size, slot);
          if (ok) {
            ESP_LOGI(TAG, "Fetched %ux%u album art in %lld ms into slot %d: %s", LvglArt::_slots[slot].dsc.header.w, LvglArt::_slots[slot].dsc.header.h,
                     (esp_timer_get_time() - started) / 1000, slot, url.c_str());
          }
        }
        xSemaphoreTake(LvglArt::_mutex, portMAX_DELAY);
        if (slot >= 0) {
          _Slot &entry = LvglArt::_slots[slot];
          entry.state = ok ? _SlotState::READY : _SlotState::EMPTY;
          if (!ok) {
            entry.url.clear();
          }
        }
        // Callers that joined while it was fetching are in the request too.
        auto finished = std::find_if(LvglArt::_requests.begin(), LvglArt::_requests.end(), [&](const _Request &r) { return r.started && r.size == size && r.url == url; });
        if (finished != LvglArt::_requests.end()) {
          done = std::move(finished->done);
          LvglArt::_requests.erase(finished);
        }
        for (size_t i = 0; i < done.size(); i++) {
          handles.push_back(ok ? LvglArt::_hold(slot) : Handle());
        }
        if (ok && done.empty()) {
          LvglArt::_slots[slot].last_used = ++LvglArt::_clock;
        }
        xSemaphoreGive(LvglArt::_mutex);
      } else {
        ESP_LOGD(TAG, "cache hit: %ux%u in slot %d for %s", size, size, slot, url.c_str());
      }

      // Outside the mutex: callbacks may look up, fetch, copy and release handles.
      for (size_t i = 0; i < done.size(); i++) {
        done[i](std::move(handles[i]), url);
      }
    }
  }
}
