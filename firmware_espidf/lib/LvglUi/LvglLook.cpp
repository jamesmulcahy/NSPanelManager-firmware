#include <Background.hpp>
#include <ConfigManager.hpp>
#include <LvglLook.hpp>
#include <LvglUi.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <Theme.hpp>
#include <esp_log.h>
#include <esp_timer.h>
#include <protobuf_nspanel.pb-c.h>
#include <vector>

static const char *TAG = "LvglLook";

void LvglLook::init() {
  esp_log_level_set(TAG, ConfigManager::log_level);
  {
    // Names saved by older firmware ("light") or by hand: compare and report them as they are drawn.
    std::lock_guard<std::mutex> lock(LvglLook::_mutex);
    ConfigManager::lvgl_theme = LvglLook::canonical_theme(ConfigManager::lvgl_theme);
    ConfigManager::lvgl_background = LvglLook::canonical_background(ConfigManager::lvgl_background);
    if (ConfigManager::lvgl_look_unconfirmed) {
      ESP_LOGI(TAG, "Look %s/%s is not confirmed by the manager yet: will report it.", ConfigManager::lvgl_background.c_str(), ConfigManager::lvgl_theme.c_str());
    }
  }
  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, LvglLook::_config_loaded, NULL);
  // A config that arrived before the handler was registered.
  LvglLook::_config_loaded(NULL, NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, NULL);
}

std::string LvglLook::canonical_theme(const std::string &theme) {
  const Theme::Palette *palette = Theme::palette_from_name(theme.c_str());
  return palette != nullptr ? palette->name : "auto";
}

std::string LvglLook::canonical_background(const std::string &background) {
  return Background::name(Background::from_name(background.c_str()));
}

void LvglLook::current(std::string *background, std::string *theme) {
  std::lock_guard<std::mutex> lock(LvglLook::_mutex);
  *background = ConfigManager::lvgl_background;
  *theme = ConfigManager::lvgl_theme;
}

void LvglLook::choose(const std::string &background, const std::string &theme) {
  std::lock_guard<std::mutex> lock(LvglLook::_mutex);
  ESP_LOGI(TAG, "Look chosen on the panel: %s/%s.", background.c_str(), theme.c_str());
  LvglLook::_set(background, theme, true);
}

void LvglLook::command_theme(const std::string &theme) {
  std::lock_guard<std::mutex> lock(LvglLook::_mutex);
  LvglLook::_set(ConfigManager::lvgl_background, theme, true);
}

void LvglLook::command_background(const std::string &background) {
  std::lock_guard<std::mutex> lock(LvglLook::_mutex);
  LvglLook::_set(background, ConfigManager::lvgl_theme, true);
}

void LvglLook::_set(const std::string &background, const std::string &theme, bool report) {
  std::string new_background = LvglLook::canonical_background(background);
  std::string new_theme = LvglLook::canonical_theme(theme);
  bool changed = new_background != ConfigManager::lvgl_background || new_theme != ConfigManager::lvgl_theme;
  if (!changed) {
    // A repeat (the same command again, or a tap on the look in use): no flash write, no repaint, and
    // nothing new for the manager.
    return;
  }
  // Only a panel that draws with LVGL reports (and so waits for a confirmation): on NSPM's TFT the
  // look is only kept for later, and the manager's config may replace it.
  report = report && LvglUi::active();
  ConfigManager::change(CONFIG_SAVE_BY_LOOK, [&]() {
    ConfigManager::lvgl_background = new_background;
    ConfigManager::lvgl_theme = new_theme;
    if (report) {
      ConfigManager::lvgl_look_unconfirmed = true;
    }
  });
  if (report) {
    LvglLook::_last_report_us = 0; // A new look is reported at once.
  }
  if (LvglUi::active()) {
    LvglUi::set_look(new_background, new_theme);
  }
  if (report) {
    LvglLook::_report();
  }
}

void LvglLook::_report() {
  if (!MqttManager::connected()) {
    return;
  }
  // A command, like the panel's other actions: the manager acts on it for the panel whose id it carries.
  // Before registration there is no id; the first config after it disagrees with an unconfirmed look and
  // brings the report back (_config_loaded).
  int32_t nspanel_id = NSPM_ConfigManager::get_nspanel_id();
  if (nspanel_id <= 0) {
    return;
  }
  std::string background = ConfigManager::lvgl_background;
  std::string theme = ConfigManager::lvgl_theme;
  NSPanelMQTTManagerCommand__LvglLook look = NSPANEL_MQTTMANAGER_COMMAND__LVGL_LOOK__INIT;
  look.theme = const_cast<char *>(theme.c_str());
  look.background = const_cast<char *>(background.c_str());
  NSPanelMQTTManagerCommand command = NSPANEL_MQTTMANAGER_COMMAND__INIT;
  command.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_LVGL_LOOK;
  command.lvgl_look = &look;
  command.nspanel_id = nspanel_id;

  std::vector<uint8_t> buffer(nspanel_mqttmanager_command__get_packed_size(&command));
  if (nspanel_mqttmanager_command__pack(&command, buffer.data()) != buffer.size()) [[unlikely]] {
    ESP_LOGE(TAG, "Failed to pack the look report.");
    return;
  }
  if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), buffer.size(), false) == ESP_OK) {
    LvglLook::_last_report_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Reported look %s/%s to the manager.", background.c_str(), theme.c_str());
  }
}

void LvglLook::_config_loaded(void *arg, esp_event_base_t base, int32_t id, void *data) {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK || !config) {
    return;
  }
  // Empty: the manager does not manage that part of the look (or does not know the fields at all).
  std::string theme = config->lvgl_theme != nullptr ? config->lvgl_theme : "";
  std::string background = config->lvgl_background != nullptr ? config->lvgl_background : "";

  std::lock_guard<std::mutex> lock(LvglLook::_mutex);
  if (ConfigManager::lvgl_look_unconfirmed) {
    if (!theme.empty() && !background.empty() && LvglLook::canonical_theme(theme) == ConfigManager::lvgl_theme &&
        LvglLook::canonical_background(background) == ConfigManager::lvgl_background) {
      ESP_LOGI(TAG, "The manager confirmed look %s/%s.", ConfigManager::lvgl_background.c_str(), ConfigManager::lvgl_theme.c_str());
      ConfigManager::lvgl_look_unconfirmed = false;
      ConfigManager::save_config(CONFIG_SAVE_BY_LOOK_CONFIRMED);
      return;
    }
    // A config from before the manager had the report: keep the panel's look, and say it again (the
    // report may have been lost, or the panel registered since). Spaced out, so a manager that answers
    // a report with another look cannot make the two go back and forth.
    ESP_LOGI(TAG, "Config has look '%s'/'%s', not the panel's unconfirmed %s/%s: keeping the panel's.", background.c_str(), theme.c_str(),
             ConfigManager::lvgl_background.c_str(), ConfigManager::lvgl_theme.c_str());
    if (LvglLook::_last_report_us == 0 || esp_timer_get_time() - LvglLook::_last_report_us >= LvglLook::REPORT_INTERVAL_US) {
      LvglLook::_report();
    }
    return;
  }
  if (theme.empty() && background.empty()) {
    return;
  }
  LvglLook::_set(background.empty() ? ConfigManager::lvgl_background : background, theme.empty() ? ConfigManager::lvgl_theme : theme, false);
}
