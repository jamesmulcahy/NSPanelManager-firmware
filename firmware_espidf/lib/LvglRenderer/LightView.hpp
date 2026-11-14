#pragma once
#include <lvgl.h>
#include <string>

// LVGL replacement for the Nextion `lights` page (lib/Pages/EntityPage, light entities):
// brightness, and colour temperature or (in colour mode) saturation and hue.

struct LightState {
  std::string name;
  bool color_mode = false;    // Saturation + hue instead of colour temperature
  bool can_color = true;      // The mode button is shown only if the light does both
  bool can_temperature = true;
  int brightness = 0;         // 0-100
  int color_temperature = 50; // 0-100, warm to cool (the manager's scale)
  int saturation = 100;       // 0-100
  int hue = 0;                // 0-359
};

class LightView {
public:
  enum class Action { BACK, SWITCH_MODE, BRIGHTNESS, COLOR_TEMPERATURE, SATURATION, HUE };
  static inline void (*on_action)(Action action, int32_t value) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const LightState &state);

private:
  static inline bool _landscape = false;
  static inline lv_obj_t *_title = NULL;
  static inline lv_obj_t *_mode = NULL;
  static inline lv_obj_t *_brightness = NULL;
  static inline lv_obj_t *_brightness_value = NULL;
  static inline lv_obj_t *_kelvin_card = NULL;
  static inline lv_obj_t *_kelvin = NULL;
  static inline lv_obj_t *_saturation_card = NULL;
  static inline lv_obj_t *_saturation = NULL;
  static inline lv_obj_t *_hue_card = NULL;
  static inline lv_obj_t *_hue = NULL;
  static inline lv_obj_t *_hue_value = NULL;
};
