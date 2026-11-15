#pragma once
#include <lvgl.h>
#include <string>

// Kept free of ESP-IDF headers, like the rest of the LVGL views, so test/host/lvgl_sim can draw
// them on a host. LvglRenderer gathers the state they show.

/**
 * What the boot screen shows. Every field is display text.
 */
struct BootStatus {
  std::string time = "--:--";
  std::string date = "Waiting for the time";
  std::string wifi;
  std::string ip;
  std::string mqtt;
  std::string loading;
  std::string secondary;
  std::string version;
};

/**
 * The boot status screen: a clock, and the panel's progress towards the manager.
 *
 * One layout for both panels: a flex container whose direction follows the screen's shape. On a
 * landscape (EU) screen the clock and the status sit side by side; on a portrait (US) one the
 * status sits under the clock. Sizes are in percent and content, never coordinates.
 */
class BootScreen {
public:
  /**
   * Build the screen's widgets in `parent`, replacing what was there.
   */
  static void build(lv_obj_t *parent, bool landscape);

  /**
   * Show `status`. Only labels whose text changed are redrawn.
   */
  static void show(const BootStatus &status);

private:
  static lv_obj_t *_row(lv_obj_t *parent, const char *symbol);

  static inline lv_obj_t *_time = NULL;
  static inline lv_obj_t *_date = NULL;
  static inline lv_obj_t *_wifi = NULL;
  static inline lv_obj_t *_ip = NULL;
  static inline lv_obj_t *_mqtt = NULL;
  static inline lv_obj_t *_loading = NULL;
  static inline lv_obj_t *_secondary = NULL;
  static inline lv_obj_t *_version = NULL;
};
