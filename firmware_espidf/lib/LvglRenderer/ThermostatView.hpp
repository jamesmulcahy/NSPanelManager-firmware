#pragma once
#include <lvgl.h>
#include <string>

// The thermostat view (lib/Pages/EntityPage, thermostats): the
// room temperature, the setpoint with down/up, and up to five options (HVAC mode, fan, preset,
// swing, ...) each showing its current value. Selecting an option points the down/up buttons at
// it.

struct ThermostatOption {
  std::string name;        // "Mode", "Fan", "Preset"
  std::string value;       // "Heat", "Auto", "Eco"
  const char *icon = NULL; // NSPM_ICON_* for the current value; NULL: option not shown
};

struct ThermostatState {
  static constexpr int MAX_OPTIONS = 5;
  std::string name;              // "Lounge"
  std::string current;           // "71.5°F"
  std::string set_label = "Set"; // "Heat to", or the selected option's name
  std::string set_value;         // "68°F", or the selected option's value
  int selected = -1;             // Option the down/up buttons change; -1: the setpoint
  bool can_page = false;         // More than one thermostat: prev/next in the header
  ThermostatOption options[MAX_OPTIONS];
};

class ThermostatView {
public:
  enum class Action { BACK, PREV, NEXT, DOWN, UP, OPTION };
  static inline void (*on_action)(Action action, int32_t option) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const ThermostatState &state);

private:
  static inline lv_obj_t *_title = NULL;
  static inline lv_obj_t *_prev = NULL;
  static inline lv_obj_t *_next = NULL;
  static inline lv_obj_t *_current = NULL;
  static inline lv_obj_t *_set_label = NULL;
  static inline lv_obj_t *_set_value = NULL;
  static inline lv_obj_t *_options[ThermostatState::MAX_OPTIONS] = {};
};
