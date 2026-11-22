#include <Background.hpp>
#include <BuzzerManager.hpp>
#include <ConfigManager.hpp>
#include <InterfaceManager.hpp>
#include <LoadingPage.hpp>
#include <MenuPage.hpp>
#include <LvglRenderer.hpp>
#include <LvglUi.hpp>
#include <NSPM_ConfigManager.hpp>
#include <Nextion.hpp>
#include <NextionTerminal.hpp>
#include <Nextion_event.hpp>
#include <PlaceholderView.hpp>
#include <ScreensaverPage.hpp>
#include <Theme.hpp>
#include <Ui.hpp>
#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <ctime>
#include <esp_timer.h>

static const char *TAG = "LvglUi";
// How often the task checks the screensaver timeout and that the terminal runs.
static constexpr int64_t TICK_US = 1000 * 1000;
// Between attempts to bring the terminal back after it gave up (3 failed sessions in a row).
static constexpr int64_t RESTART_INTERVAL_US = 10 * 1000 * 1000;
// A swipe: at least this far, within this long.
static constexpr int SWIPE_PX = 80;
static constexpr int64_t SWIPE_MAX_US = 1000 * 1000;
// A swipe-down that starts this near the top (the header bar is 8..52) opens the menu, from any page,
// after EDGE_SWIPE_PX. Only from there: elsewhere a drag may be meant for what is under the finger
// (a vertical slider on a landscape panel, a list).
static constexpr int EDGE_ZONE_PX = 72;
static constexpr int EDGE_SWIPE_PX = 48;

// The display's identity, up to the reply's 0xFF terminator.
static std::string printable_identity() {
  std::string identity = Nextion::identity();
  size_t end = identity.find('\xFF');
  return end == std::string::npos ? identity : identity.substr(0, end);
}

bool LvglUi::init() {
  esp_log_level_set(TAG, ConfigManager::log_level);
  std::string identity = printable_identity();
  const char *shown_identity = identity.empty() ? "no connect reply" : identity.c_str();
  // The TFT decides the UI. There are no HMI pages to fall back to: without LVGL the panel shows
  // no pages at all (InterfaceManager), and keeps working as a switch, a sensor and an update target.
  if (Nextion::nspm_flag_received()) {
    // NSPM's own (legacy) TFT: only its boot screen is used, to say that the display needs the GUI update.
    ESP_LOGW(TAG, "The display runs NSPM's legacy TFT (%s): no UI until the GUI is updated in NSPanel Manager.", shown_identity);
    LvglUi::_display_mode = DisplayMode::NEXTION;
    return false;
  }
  if (!NextionTerminal::is_receiver(identity.c_str())) {
    // Neither a terminal TFT nor NSPM's: nothing the firmware knows how to write on.
    ESP_LOGW(TAG, "No UI: the display runs no terminal TFT and sent no NSPM flag (%s).", shown_identity);
    LvglUi::_display_mode = DisplayMode::TERMINAL_IDLE;
    Nextion::set_commands_blocked(true);
    return false;
  }
  // The palette before the first screen is built; Background::set() below applies the same one.
  Background::Preset preset = Background::from_name(ConfigManager::lvgl_background.c_str());
  const Theme::Palette *theme = Theme::palette_from_name(ConfigManager::lvgl_theme.c_str());
  Theme::set_palette(Background::palette_for(preset, theme));
  esp_err_t started = LvglRenderer::start();
  if (started != ESP_OK) {
    // The TFT has no Nextion pages either: the display will show its "waiting for connection".
    ESP_LOGE(TAG, "The display runs a terminal TFT, but LVGL did not start: %s.", esp_err_to_name(started));
    LvglUi::_display_mode = DisplayMode::TERMINAL_IDLE;
    Nextion::set_commands_blocked(true);
    return false;
  }

  Nextion::set_commands_blocked(true);
  Nextion::set_brightness_redirect([](uint8_t brightness) { NextionTerminal::set_brightness(brightness); });
  LvglRenderer::touch_filter = LvglUi::_touch;
  // GUI task, after a region change rebuilt the screens: build the last view again.
  LvglRenderer::on_pages_rebuilt = LvglUi::_rebuild;
  // The page background (Background.hpp), drawn behind the pages screen's children.
  LvglRenderer::post([preset, theme]() {
    Background::attach(LvglRenderer::pages_screen());
    Background::set(preset, theme);
  });
  PlaceholderView::on_action = [](PlaceholderView::Action action) {
    LvglUi::defer([]() { InterfaceManager::show_default_page(); });
  };

  LvglUi::_work = xQueueCreate(16, sizeof(std::function<void()> *));
  // Internal RAM stack: page code logs and publishes from here.
  if (LvglUi::_work == NULL || xTaskCreatePinnedToCore(LvglUi::_task, "lvgl_ui", 6144, NULL, 5, NULL, 1) != pdPASS) {
    ESP_LOGE(TAG, "Failed to start LvglUi's task.");
  }
  LvglUi::_last_activity_us = esp_timer_get_time();
  LvglUi::_active = true;
  LvglUi::_display_mode = DisplayMode::LVGL;
  LvglUi::_start_terminal();
  ESP_LOGI(TAG, "LVGL is the UI: the display runs a terminal TFT (%s).", identity.c_str());
  return true;
}

void LvglUi::_start_terminal() {
  LvglUi::_last_start_us = esp_timer_get_time();
  NextionTerminal::Options options = NextionTerminal::options_for(!ConfigManager::lvgl_us_layout, LvglRenderer::width(), LvglRenderer::height());
  esp_err_t result = NextionTerminal::start(options);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Terminal did not start: %s", esp_err_to_name(result));
  }
}

void LvglUi::show(View view, void (*build)(lv_obj_t *parent, bool landscape), std::function<void()> apply) {
  LvglRenderer::post([view, build, apply]() {
    if (LvglUi::_view != view) {
      bool landscape = LvglRenderer::landscape();
      build(Theme::page_root(LvglRenderer::pages_screen(), landscape), landscape);
      LvglUi::_view = view;
      LvglUi::_last_view = view;
      LvglUi::_build = build;
    }
    LvglUi::_apply = apply;
    apply();
  });
  LvglRenderer::activate(LvglRenderer::Screen::PAGES);
}

void LvglUi::_rebuild() {
  LvglUi::_view = View::NONE;
  if (LvglUi::_build != nullptr) {
    LvglUi::_build(Theme::page_root(LvglRenderer::pages_screen(), LvglRenderer::landscape()), LvglRenderer::landscape());
    LvglUi::_view = LvglUi::_last_view;
    if (LvglUi::_apply) {
      LvglUi::_apply();
    }
  }
}

void LvglUi::set_look(const std::string &background, const std::string &theme) {
  Background::Preset preset = Background::from_name(background.c_str());
  const Theme::Palette *palette = Theme::palette_from_name(theme.c_str());
  ESP_LOGI(TAG, "Page background: %s; theme: %s.", Background::name(preset), palette != nullptr ? palette->name : "auto");
  LvglRenderer::post([preset, palette]() {
    Background::set(preset, palette);
    // Every screen takes its colours when it is built: the boot screen as well as the page.
    LvglRenderer::rebuild_screens();
    lv_obj_invalidate(lv_screen_active());
  });
}

void LvglUi::show_placeholder(const char *page) {
  ESP_LOGD(TAG, "No LVGL version of page '%s' yet: showing the placeholder.", page);
  std::string name = page;
  LvglUi::show(View::PLACEHOLDER, PlaceholderView::build, [name]() { PlaceholderView::show(name); });
}

void LvglUi::defer(std::function<void()> work) {
  if (LvglUi::_work == NULL) {
    return;
  }
  auto *item = new std::function<void()>(std::move(work));
  if (xQueueSend(LvglUi::_work, &item, 0) != pdTRUE) {
    delete item;
    ESP_LOGW(TAG, "Action queue full; dropped an action.");
  }
}

bool LvglUi::suspend(uint32_t timeout_ms) {
  if (!LvglUi::_active) {
    return true;
  }
  LvglUi::_suspended = true;
  NextionTerminal::stop();
  for (uint32_t waited = 0; NextionTerminal::owns_display() && waited < timeout_ms; waited += 50) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  bool free = !NextionTerminal::owns_display();
  ESP_LOGI(TAG, "Terminal %s for the TFT update.", free ? "ended" : "still running");
  return free;
}

bool LvglUi::_touch(bool down, uint16_t x, uint16_t y) {
  // The terminal's task: no waiting here. A press is activity; so is the finger still being down.
  if (down) {
    LvglUi::_last_activity_us = esp_timer_get_time();
    LvglUi::_sleep_posted = false;
  }
  if (LvglUi::_swallowing) {
    if (!down) {
      LvglUi::_swallowing = false;
    }
    return true;
  }
  if (LvglUi::_screensaver_press || (down && ScreensaverPage::showing())) {
    return LvglUi::_screensaver_touch(down, x, y);
  }
  static bool was_down = false;
  // The press began over an open pop-up (the room list): a drag scrolls it, it is not a swipe.
  static bool in_popup = false;
  // The press has become a swipe-down: LVGL was told to drop it, the menu opens on the release.
  static bool swipe_down = false;
  const int64_t now = esp_timer_get_time();
  if (down && !was_down) {
    in_popup = Ui::popup_open;
    swipe_down = false;
    LvglUi::_press_x = x;
    LvglUi::_press_y = y;
    LvglUi::_press_us = now;
    LvglUi::_swiping = false;
    if (LvglUi::touch_sound) {
      // The touch sound, which BuzzerManager plays for the Nextion's touch events. Not on this task.
      LvglUi::defer([]() { BuzzerManager::raise_event(buzzer_event_t::TOUCH); });
    }
  }
  // A swipe: mostly one way, 80 px or more (LVGL's own gesture detection missed them on Office).
  const int dx = (int)x - LvglUi::_press_x, dy = (int)y - LvglUi::_press_y;
  if (was_down && (abs(dy) >= SWIPE_PX || abs(dx) >= SWIPE_PX)) {
    LvglUi::_swiping = true;
  }
  // Decided while the finger is still down, from the moves (not the release's coordinates, which a
  // receiver may report as nonsense: 255,255 on Kitchen), so LVGL can drop the press before the
  // release clicks the header button the swipe started on.
  if (down && was_down && !swipe_down && !in_popup && now - LvglUi::_press_us < SWIPE_MAX_US && abs(dx) * 2 <= dy) {
    if (LvglUi::_press_y < EDGE_ZONE_PX && dy >= EDGE_SWIPE_PX) {
      swipe_down = true;
      LvglUi::_swiping = true; // A light tile the swipe started on cancels (HomeView::is_swipe).
      LvglRenderer::cancel_press = true;
    }
  }
  if (!down && was_down && swipe_down) {
    swipe_down = false;
    LvglUi::defer([]() { MenuPage::swipe_down(); });
  }
  was_down = down;
  return false;
}

bool LvglUi::_screensaver_touch(bool down, uint16_t x, uint16_t y) {
  // The terminal's task. With fewer than two cards (or the backlight off) a press wakes the panel at
  // once, and the rest of the touch goes nowhere, as the HMI's screensaver did. With cards to turn
  // between, the release decides: a swipe sideways turns the card and the panel sleeps on; anything
  // else wakes it. Nothing reaches LVGL either way.
  const int64_t now = esp_timer_get_time();
  if (!LvglUi::_screensaver_press) {
    if (!ScreensaverPage::can_turn_cards()) {
      LvglUi::_swallowing = true;
      esp_event_post(NEXTION_EVENT, nextion_event_t::WAKE_EVENT, NULL, 0, 0);
      return true;
    }
    LvglUi::_screensaver_press = true;
    LvglUi::_press_x = x;
    LvglUi::_press_y = y;
    LvglUi::_press_us = now;
    return true;
  }
  if (down) {
    if (!ScreensaverPage::showing()) {
      // The screensaver went under the finger (screen_on over MQTT): the rest of the touch is not a tap.
      LvglUi::_screensaver_press = false;
      LvglUi::_swallowing = true;
    }
    return true;
  }
  LvglUi::_screensaver_press = false;
  if (!ScreensaverPage::showing()) {
    return true; // Gone while the finger was down: nothing to wake or turn.
  }
  // The release comes at the last point that mapped (NextionTerminal), so this is the finger's travel.
  const int dx = (int)x - LvglUi::_press_x, dy = (int)y - LvglUi::_press_y;
  if (abs(dx) >= SWIPE_PX && abs(dy) * 2 <= abs(dx) && now - LvglUi::_press_us < SWIPE_MAX_US) {
    // Left brings in the next card, right the one before, as the finger pushes them.
    ScreensaverPage::turn_card(dx < 0 ? 1 : -1);
  } else {
    esp_event_post(NEXTION_EVENT, nextion_event_t::WAKE_EVENT, NULL, 0, 0);
  }
  return true;
}

void LvglUi::_tick() {
  if (LvglUi::_suspended) {
    return;
  }
  const int64_t now = esp_timer_get_time();
  // The terminal gives up after three failed sessions in a row; this TFT has nothing else to show.
  if (!NextionTerminal::owns_display() && Nextion::get_current_state() == nextion_state_t::RUNNING &&
      now - LvglUi::_last_start_us > RESTART_INTERVAL_US) {
    ESP_LOGW(TAG, "The terminal is not running; starting it again.");
    LvglUi::_start_terminal();
  }

  const int64_t minute = (int64_t)time(NULL) / 60;
  if (minute != LvglUi::_last_minute) {
    LvglUi::_last_minute = minute;
    if (LvglUi::on_minute != nullptr) {
      LvglUi::on_minute();
    }
  }

  // The screensaver timeout, which the HMI's home.sleep_display timer kept.
  const bool screensaver = ScreensaverPage::showing();
  if (LvglUi::_was_screensaver && !screensaver) {
    LvglUi::_last_activity_us = now; // Woken, by a touch or otherwise: the timeout starts again.
    LvglUi::_sleep_posted = false;
  }
  LvglUi::_was_screensaver = screensaver;
  if (screensaver || LoadingPage::showing() || LvglUi::_sleep_posted) {
    return;
  }
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK || !config || config->screensaver_activation_timeout <= 0) {
    return;
  }
  if (now - LvglUi::_last_activity_us >= (int64_t)config->screensaver_activation_timeout * 1000) {
    ESP_LOGD(TAG, "No touch for %ld ms: screensaver.", (long)config->screensaver_activation_timeout);
    LvglUi::_sleep_posted = true;
    esp_event_post(NEXTION_EVENT, nextion_event_t::SLEEP_EVENT, NULL, 0, pdMS_TO_TICKS(100));
  }
}

void LvglUi::_task(void *param) {
  int64_t last_tick = 0;
  for (;;) {
    std::function<void()> *work = NULL;
    if (xQueueReceive(LvglUi::_work, &work, pdMS_TO_TICKS(250)) == pdTRUE) {
      (*work)();
      delete work;
    }
    int64_t now = esp_timer_get_time();
    if (now - last_tick >= TICK_US) {
      last_tick = now;
      LvglUi::_tick();
    }
  }
}
