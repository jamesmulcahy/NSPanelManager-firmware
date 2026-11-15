#include <BootScreen.hpp>
#include <ConfigManager.hpp>
#include <LvglRenderer.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <NSPM_version.hpp>
#include <NextionTerminal.hpp>
#include <PanelClock.hpp>
#include <Theme.hpp>
#include <WiFiManager.hpp>
#include <cstring>
#include <ctime>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

// Lines of the screen LVGL renders at a time, in internal RAM: 12.8 KB at 320 wide. PSRAM shares the
// cache with flash at 40 MHz, and drawing into it took a full repaint from ~160 to ~300 ms on Office;
// taller bands don't help, as the terminal sends a band while LVGL renders the next.
static constexpr int DRAW_LINES = 20;
// The longest the GUI task sleeps, so the clock and the status rows stay current.
static constexpr uint32_t MAX_SLEEP_MS = 500;

// The GUI task's stack, in internal RAM. The deepest page leaves ~5 KB unused; see the
// "GUI task stack" log line.
static constexpr uint32_t GUI_TASK_STACK = 12288;
// How often the GUI task logs how much of its stack it has used, at debug.
static constexpr int64_t STACK_LOG_INTERVAL_US = 60 * 1000 * 1000;

static const char *TAG = "LvglRenderer";

esp_err_t LvglRenderer::start() {
  static std::atomic<bool> started = false;
  if (started.exchange(true)) {
    return LvglRenderer::_enabled ? ESP_OK : ESP_FAIL;
  }
  esp_log_level_set(TAG, ConfigManager::log_level);
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    ESP_LOGE(TAG, "No PSRAM for LVGL's heap.");
    return ESP_ERR_NO_MEM;
  }

  LvglRenderer::_us_layout = ConfigManager::lvgl_us_layout;
  // The widest the screen can be, so a region change needs no new buffer.
  const size_t buffer_bytes = 480 * DRAW_LINES * sizeof(uint16_t);
  uint8_t *draw_buffer = static_cast<uint8_t *>(heap_caps_malloc(buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (draw_buffer == NULL) {
    ESP_LOGW(TAG, "No internal RAM for the draw buffer; drawing into PSRAM, slower.");
    draw_buffer = static_cast<uint8_t *>(heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM));
  }
  LvglRenderer::_text_mutex = xSemaphoreCreateMutex();
  LvglRenderer::_touches = xQueueCreate(16, sizeof(Touch));
  LvglRenderer::_work = xQueueCreate(32, sizeof(std::function<void()> *));
  if (draw_buffer == NULL || LvglRenderer::_text_mutex == NULL || LvglRenderer::_touches == NULL || LvglRenderer::_work == NULL) {
    ESP_LOGE(TAG, "Failed to allocate LVGL's buffers.");
    heap_caps_free(draw_buffer);
    return ESP_ERR_NO_MEM;
  }

  lv_init();
  lv_tick_set_cb(LvglRenderer::_tick);
  LvglRenderer::_display = lv_display_create(LvglRenderer::width(), LvglRenderer::height());
  lv_display_set_color_format(LvglRenderer::_display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(LvglRenderer::_display, draw_buffer, NULL, buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(LvglRenderer::_display, LvglRenderer::_flush);
  lv_indev_t *pointer = lv_indev_create();
  lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pointer, LvglRenderer::_read_pointer);

  // The terminal's touches, and a full repaint whenever it (re)starts: the display lost the picture.
  NextionTerminal::on_touch = LvglRenderer::inject_touch;
  NextionTerminal::on_streaming_changed = [](bool streaming) {
    if (streaming) {
      LvglRenderer::_repaint = true;
    }
    LvglRenderer::notify();
  };
  LvglRenderer::_boot_screen = lv_screen_active();
  LvglRenderer::_pages_screen = lv_obj_create(NULL);
  LvglRenderer::_build_screens();

  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, LvglRenderer::_config_event_handler, NULL);
  // Started after the manager's config may have arrived: lay out for this panel from the start.
  LvglRenderer::_config_event_handler(NULL, NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, NULL);
  // Below the pages' tasks, so a tap's handling overtakes a long redraw. The stack must be internal
  // RAM: on the ESP32 a task whose stack is in PSRAM may not call ROM code, and this one logs.
  if (xTaskCreatePinnedToCore(LvglRenderer::_task, "lvgl", GUI_TASK_STACK, NULL, 2, &LvglRenderer::_task_handle, 1) != pdPASS) {
    ESP_LOGE(TAG, "Failed to start the LVGL task.");
    return ESP_ERR_NO_MEM;
  }
  LvglRenderer::_enabled = true;
  ESP_LOGI(TAG, "LVGL %d.%d.%d drawing %ux%u.", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LvglRenderer::width(), LvglRenderer::height());
  return ESP_OK;
}

void LvglRenderer::activate(Screen screen) {
  if (!LvglRenderer::_enabled || (LvglRenderer::_active && LvglRenderer::_wanted_screen == screen)) {
    return; // Not running, or already drawing it: LVGL redraws what changes.
  }
  LvglRenderer::_wanted_screen = screen;
  LvglRenderer::_repaint = true;
  LvglRenderer::_active = true;
  LvglRenderer::notify();
}

bool LvglRenderer::post(std::function<void()> work) {
  if (LvglRenderer::_work == NULL) {
    return false;
  }
  auto *item = new std::function<void()>(std::move(work));
  if (xQueueSend(LvglRenderer::_work, &item, 0) != pdTRUE) {
    delete item;
    ESP_LOGW(TAG, "GUI work queue full; dropped an update.");
    return false;
  }
  LvglRenderer::notify();
  return true;
}

void LvglRenderer::inject_touch(bool down, uint16_t x, uint16_t y) {
  if (LvglRenderer::touch_filter != nullptr && LvglRenderer::touch_filter(down, x, y)) {
    return;
  }
  if (!LvglRenderer::_active || LvglRenderer::_touches == NULL) {
    return;
  }
  static bool was_down = false;
  Touch touch = {x, y, down};
  // A full queue drops moves quietly; an edge lost is worth a warning.
  if (xQueueSend(LvglRenderer::_touches, &touch, 0) != pdTRUE && down != was_down) {
    ESP_LOGW(TAG, "Touch queue full, dropped a %s.", down ? "press" : "release");
  }
  was_down = down;
  LvglRenderer::notify();
}

void LvglRenderer::set_loading_text(const std::string &text) {
  if (LvglRenderer::_text_mutex == NULL || xSemaphoreTake(LvglRenderer::_text_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }
  LvglRenderer::_loading_text = text;
  xSemaphoreGive(LvglRenderer::_text_mutex);
  LvglRenderer::notify();
}

void LvglRenderer::set_secondary_text(const std::string &text) {
  if (LvglRenderer::_text_mutex == NULL || xSemaphoreTake(LvglRenderer::_text_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }
  LvglRenderer::_secondary_text = text;
  xSemaphoreGive(LvglRenderer::_text_mutex);
  LvglRenderer::notify();
}

void LvglRenderer::notify() {
  if (LvglRenderer::_task_handle != NULL) {
    xTaskNotifyGive(LvglRenderer::_task_handle);
  }
}

uint32_t LvglRenderer::_tick() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

void LvglRenderer::_build_screens() {
  bool landscape = !LvglRenderer::_us_layout;
  lv_obj_t *screen = LvglRenderer::_boot_screen;
  lv_obj_clean(screen);
  Theme::screen(screen);
  lv_obj_t *root = Theme::group(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_size(root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_gap(root, Theme::SPACE_L, 0);
  BootScreen::build(root, landscape);
  if (LvglRenderer::on_pages_rebuilt != nullptr) {
    LvglRenderer::on_pages_rebuilt();
  }
}

void LvglRenderer::_show_status() {
  BootStatus status;
  if (xSemaphoreTake(LvglRenderer::_text_mutex, 0) == pdTRUE) {
    status.loading = LvglRenderer::_loading_text;
    status.secondary = LvglRenderer::_secondary_text;
    xSemaphoreGive(LvglRenderer::_text_mutex);
  } else {
    return; // Next tick.
  }
  bool wifi = WiFiManager::connected();
  status.wifi = wifi ? ConfigManager::wifi_ssid : "Connecting to " + ConfigManager::wifi_ssid;
  status.ip = wifi ? WiFiManager::ip_string() : "No address yet";
  status.mqtt = MqttManager::connected() ? "MQTT connected" : "MQTT not connected";
  status.version = "Firmware " NSPM_VERSION;

  if (PanelClock::is_set()) {
    bool us_style = false;
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK && config) {
      us_style = config->clock_us_style;
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char text[32];
    strftime(text, sizeof(text), us_style ? "%l:%M %p" : "%H:%M", &local);
    status.time = text[0] == ' ' ? text + 1 : text; // %l pads one-digit hours with a space
    strftime(text, sizeof(text), "%A %e %B", &local);
    status.date = text;
  }
  BootScreen::show(status);
}

void LvglRenderer::_task(void *param) {
  for (;;) {
    // Other tasks' changes to LVGL objects (post()), whether or not anything is drawn yet.
    std::function<void()> *work;
    while (xQueueReceive(LvglRenderer::_work, &work, 0) == pdTRUE) {
      (*work)();
      delete work;
    }

    int wanted = LvglRenderer::_wanted_region.exchange(-1);
    if (wanted != -1 && (wanted == 1) != LvglRenderer::_us_layout) {
      ESP_LOGI(TAG, "Laying out for a%s panel.", wanted == 1 ? " US" : "n EU");
      LvglRenderer::_us_layout = wanted == 1;
      lv_display_set_resolution(LvglRenderer::_display, LvglRenderer::width(), LvglRenderer::height());
      LvglRenderer::_build_screens();
      LvglRenderer::_repaint = true;
    }

    uint32_t sleep_ms = MAX_SLEEP_MS;
    // Nothing reaches the display until the receiver streams: wait, and repaint everything then.
    if (LvglRenderer::_active && !NextionTerminal::streaming()) {
      LvglRenderer::_repaint = true;
    } else if (LvglRenderer::_active) {
      if (LvglRenderer::_wanted_screen != LvglRenderer::_loaded_screen) {
        LvglRenderer::_loaded_screen = LvglRenderer::_wanted_screen;
        lv_screen_load(LvglRenderer::_loaded_screen == Screen::PAGES ? LvglRenderer::_pages_screen : LvglRenderer::_boot_screen);
      }
      if (LvglRenderer::_repaint.exchange(false)) {
        lv_obj_invalidate(lv_screen_active());
      }
      if (LvglRenderer::_loaded_screen == Screen::BOOT) {
        LvglRenderer::_show_status();
      }
      uint32_t next = lv_timer_handler();
      if (next < sleep_ms) {
        sleep_ms = next;
      }
    }
    static int64_t last_stack_log = 0;
    if (esp_timer_get_time() - last_stack_log > STACK_LOG_INTERVAL_US) {
      last_stack_log = esp_timer_get_time();
      ESP_LOGD(TAG, "GUI task stack: %lu of %lu bytes never used.", (unsigned long)uxTaskGetStackHighWaterMark(NULL), (unsigned long)GUI_TASK_STACK);
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sleep_ms < 10 ? 10 : sleep_ms));
  }
}

void LvglRenderer::_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  // The band is copied before draw() returns, while the previous one may still be on its way, so
  // LVGL renders the next band meanwhile. A band that fails means the display no longer shows what
  // LVGL drew: repaint once the receiver is back.
  if (NextionTerminal::draw(area->x1, area->y1, area->x2, area->y2, reinterpret_cast<const uint16_t *>(px_map), lv_display_flush_is_last(display)) !=
      ESP_OK) {
    LvglRenderer::_repaint = true;
  }
  lv_display_flush_ready(display);
}

void LvglRenderer::_read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
  Touch touch;
  if (xQueueReceive(LvglRenderer::_touches, &touch, 0) == pdTRUE) {
    LvglRenderer::_point.x = touch.x;
    LvglRenderer::_point.y = touch.y;
    LvglRenderer::_pressed = touch.pressed;
    // Read again at once if more edges are waiting, so none is merged into the next.
    data->continue_reading = uxQueueMessagesWaiting(LvglRenderer::_touches) > 0;
  }
  data->point = LvglRenderer::_point;
  data->state = LvglRenderer::_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void LvglRenderer::_config_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK) {
    return;
  }
  bool us_layout = config->is_us_panel;
  if (us_layout != ConfigManager::lvgl_us_layout) {
    // Remembered so that the next boot lays out its first screen for this panel.
    ConfigManager::lvgl_us_layout = us_layout;
    ConfigManager::save_config();
  }
  LvglRenderer::_wanted_region = us_layout ? 1 : 0;
  LvglRenderer::notify();
}
