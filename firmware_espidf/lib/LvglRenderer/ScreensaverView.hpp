#pragma once
#include <lvgl.h>
#include <string>

// LVGL replacement for the Nextion `screensaver` (weather) and `screensaver2` (clock only) pages
// (lib/Pages/ScreensaverPage). The manager's screensaver modes map onto `weather` and
// `background`; "no screensaver" turns the display off and draws nothing.

struct ForecastDay {
  std::string day;         // "Sat"
  const char *icon = NULL; // NSPM_ICON_WEATHER_*
  std::string max_min;     // "58°/44°"
  std::string rain;        // "10%"
  std::string wind;        // "6 mph"
};

struct ScreensaverState {
  static constexpr int FORECAST_DAYS = 5;
  bool weather = true;        // WEATHER_* modes: forecast; DATETIME_* modes: the clock alone
  bool background = true;     // *_WITH_BACKGROUND: the themed background; else black
  std::string time;           // "12:34"
  std::string am_pm;          // "PM", "" for a 24-hour clock
  std::string date;           // "Friday 13 November"
  bool show_outside = true;   // show_screensaver_outside_temperature
  bool show_inside = true;    // show_screensaver_inside_temperature
  const char *outside_icon = NULL;
  std::string outside;        // "54°F"
  std::string inside;         // "71°F", the room
  std::string max_min;        // "61°/47°"
  std::string rain;           // "20%"
  std::string wind;           // "8 mph"
  std::string sunrise;        // "6:42 AM"
  std::string sunset;
  ForecastDay forecast[FORECAST_DAYS];
};

class ScreensaverView {
public:
  // Any touch wakes the panel.
  static inline void (*on_touch)() = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const ScreensaverState &state);

private:
  struct Day {
    lv_obj_t *day, *icon, *max_min, *rain, *wind;
  };
  static void _build_weather(bool landscape);
  static void _build_clock();

  static inline lv_obj_t *_parent = NULL;
  static inline bool _landscape = false;
  static inline int _mode = -1; // weather, as built; -1 before the first show()
  static inline bool _background = true;
  // Both modes
  static inline lv_obj_t *_time = NULL;
  static inline lv_obj_t *_am_pm = NULL;
  static inline lv_obj_t *_date = NULL;
  static inline lv_obj_t *_outside_icon = NULL;
  static inline lv_obj_t *_outside = NULL;
  static inline lv_obj_t *_inside_group = NULL;
  static inline lv_obj_t *_outside_group = NULL;
  static inline lv_obj_t *_inside = NULL;
  // Weather mode
  static inline lv_obj_t *_max_min = NULL;
  static inline lv_obj_t *_rain = NULL;
  static inline lv_obj_t *_wind = NULL;
  static inline lv_obj_t *_sunrise = NULL;
  static inline lv_obj_t *_sunset = NULL;
  static inline Day _days[ScreensaverState::FORECAST_DAYS] = {};
};
