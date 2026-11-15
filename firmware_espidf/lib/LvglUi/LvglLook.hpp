#pragma once
#include <esp_event.h>
#include <mutex>
#include <string>

/**
 * The panel's LVGL look (page background and palette): where it comes from, where it is saved, and
 * telling the manager when it changed here (docs/LVGL.md, "The look").
 *
 * Three ways to set it, in ConfigManager (saved in config.json, so a reboot keeps it):
 *
 * - The manager's config (NSPanelConfig lvgl_theme / lvgl_background): applied when it differs.
 *   An empty field is "not managed": the panel keeps its own.
 * - The lvgl_theme and lvgl_background commands (UpdateManager): applied at once.
 * - The person at the panel, on the look picker (LookPage).
 *
 * The last two are changes the manager may not know about yet. The panel reports the look on
 * nspanel/<MAC>/lvgl_look and keeps it "unconfirmed" (also saved) until a config arrives whose look
 * is the same. While it is unconfirmed, a config with another look is a stale one (sent before the
 * manager had the report, or retained from before) and is not applied; the report is sent again
 * instead, at most every REPORT_INTERVAL_US. A manager that does not know the report never confirms
 * it: the panel keeps its look, and a command still changes it.
 */
class LvglLook {
public:
  // Once, after MqttManager::start(): follow the config, and with LVGL as the UI, report.
  static void init();

  // The person at the panel chose a look (names as in Looks::ALL). Any task.
  static void choose(const std::string &background, const std::string &theme);
  // The lvgl_theme / lvgl_background commands. Any task.
  static void command_theme(const std::string &theme);
  static void command_background(const std::string &background);

  // The saved look, with names made canonical ("light" is "grey"; unknown names are "auto" and "none").
  static void current(std::string *background, std::string *theme);

  static std::string canonical_theme(const std::string &theme);
  static std::string canonical_background(const std::string &background);

  // Between reports sent again for one unconfirmed look.
  static constexpr int64_t REPORT_INTERVAL_US = 10 * 1000 * 1000;

private:
  static void _config_loaded(void *arg, esp_event_base_t base, int32_t id, void *data);
  // Under _mutex: store, save, apply (LVGL mode) and, when `report`, mark unconfirmed and report.
  static void _set(const std::string &background, const std::string &theme, bool report);
  static void _report(); // Under _mutex

  static inline std::mutex _mutex;
  static inline int64_t _last_report_us = 0;
};
