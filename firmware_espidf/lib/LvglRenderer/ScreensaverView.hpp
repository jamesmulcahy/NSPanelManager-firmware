#pragma once
#include <cstdint>
#include <lvgl.h>
#include <string>
#include <vector>

// LVGL replacement for the Nextion `screensaver` (weather) and `screensaver2` (clock only) pages
// (lib/Pages/ScreensaverPage). The manager's screensaver modes map onto `weather` and
// `background`; "no screensaver" turns the display off and draws nothing.
//
// The screen is a fixed clock header (time, AM/PM, date, the room's temperature) over a body. Only
// the body changes: it rotates through its cards (the weather, then the extra cards such as the
// calendar and the energy card), each for its own time. The clock never moves.
//
//   mode       header                       body cards
//   WEATHER_*  clock, date, room            weather (base_seconds), then `cards`
//   DATETIME_* clock, date, outside, room   `cards` only; with none, the centred clock as before

struct ForecastDay {
  std::string day;         // "Sat"
  const char *icon = NULL; // NSPM_ICON_WEATHER_*
  std::string max_min;     // "58°/44°"
  std::string rain;        // "10%"
  std::string wind;        // "6 mph"
};

// The extra cards. The values are NSPanelConfig.NSPanelScreensaverCardType's (the view has no
// protobuf); the page skips the values it does not know.
enum class ScreensaverCardType : uint8_t {
  CALENDAR = 1,
  ENERGY = 2,
};

// One event: the time column and the title. `now`: under way (drawn in the accent colour).
struct CalendarRow {
  std::string time;    // "3:00 PM", "All day", "Now"
  std::string summary; // The event's title
  bool now = false;
};

// The events of one day, under its heading.
struct CalendarDay {
  std::string heading; // "Today", "Tomorrow", "Wed 18"
  std::vector<CalendarRow> rows;
};

// The energy card: the house's power draw as a cost per day (or as power, with no price) over the
// number of lights on. The page formats it (ScreensaverPage::format_energy).
struct EnergyState {
  bool power_shown = false;  // The power part: hidden when the manager has no power source
  bool lights_shown = false; // The lights part: hidden when the manager counts no lights
  // The large figure: the cost per day with a price, else the power.
  std::string hero_symbol;  // "$" before the number (a smaller font); empty for power
  std::string hero_number;  // "9.38", "3.6"; "--" when unavailable
  std::string hero_unit;    // "per day", "kW now"
  std::string power_line;   // "3.6 kW now" under the cost; empty: not shown (the hero is the power)
  bool exporting = false;   // The power line reads "Exporting 0.4 kW" (another icon, in the accent colour)
  std::string lights_number; // "6"; "--" when unavailable
  std::string lights_text;   // "lights on", "light on", "lights off"
  bool lights_none = false;  // No light on: the outline bulb, muted
  bool lights_alert = false; // At or over the manager's threshold: the alert colour
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

  // The body's rotation.
  struct Card {
    ScreensaverCardType type;
    uint16_t seconds; // How long it shows; 0: 10 s
  };
  uint16_t base_seconds = 10;        // The weather card's time (WEATHER_* modes); 0: 10 s
  std::vector<Card> cards;           // Extra cards after the weather, in order; empty: weather alone
  std::vector<CalendarDay> calendar; // Sorted; empty: the calendar card shows empty_text
  std::string empty_text;            // "No events in the next 48 hours"
  EnergyState energy;
};

class ScreensaverView {
public:
  // Any touch wakes the panel.
  static inline void (*on_touch)() = nullptr;

  // The body turned to a card (also when it first shows one): `weather`, `calendar` or `energy`, and how long
  // it stays, in seconds. On the GUI task. For the page's log; the simulator leaves it unset.
  static inline void (*on_card)(const char *card, uint32_t seconds) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const ScreensaverState &state);

  /**
   * Show the next body card now, as the rotation timer does when a card's time is up. For the
   * simulator; nothing on the panel needs it.
   */
  static void next_card();

private:
  struct Day {
    lv_obj_t *day, *icon, *max_min, *rain, *wind;
  };
  // The body's cards, each built once and shown in turn.
  enum Body : uint8_t { WEATHER, CALENDAR, ENERGY, BODY_COUNT };
  struct Step {
    Body body;
    uint32_t ms;
    bool operator==(const Step &other) const { return body == other.body && ms == other.ms; }
  };
  // A line of the calendar card: a day's heading, an event, or "+3 more".
  struct Line {
    lv_obj_t *row, *time, *summary;
  };
  static constexpr int CALENDAR_COLUMNS_MAX = 2;

  static void _build_header(bool landscape, bool outside);
  static lv_obj_t *_build_weather(bool landscape);
  static lv_obj_t *_build_calendar(bool landscape);
  static lv_obj_t *_build_energy(bool landscape);
  static void _build_clock();
  static void _show_calendar(const ScreensaverState &state);
  static void _show_energy(const EnergyState &energy);
  static void _set_sequence(const std::vector<Step> &sequence);
  static void _show_current();
  static void _rotate(lv_timer_t *timer);
  static void _forget(lv_event_t *event);

  static inline lv_obj_t *_parent = NULL;
  static inline bool _landscape = false;
  // What is built: weather mode, background, and which body cards (a bit per Body); -1 before the
  // first show(). Any change builds the page again.
  static inline int _layout = -1;
  static inline bool _background = true;
  // The rotation: the body cards in order, the one shown, and the timer that turns to the next
  // (none with fewer than two). The timer is the GUI task's, and is deleted with the header (_forget).
  static inline std::vector<Step> _sequence;
  static inline size_t _current = 0;
  static inline int _announced = -1; // The Body on_card last told of
  static inline lv_timer_t *_timer = NULL;
  static inline lv_obj_t *_bodies[BODY_COUNT] = {};
  // Header (all modes)
  static inline lv_obj_t *_time = NULL;
  static inline lv_obj_t *_am_pm = NULL;
  static inline lv_obj_t *_date = NULL;
  static inline int32_t _date_width = 0; // The landscape header's: a wider date takes a smaller font
  static inline lv_obj_t *_outside_icon = NULL;
  static inline lv_obj_t *_outside = NULL;
  static inline lv_obj_t *_inside_group = NULL;
  static inline lv_obj_t *_outside_group = NULL;
  static inline lv_obj_t *_inside = NULL;
  // Weather card
  static inline lv_obj_t *_max_min = NULL;
  static inline lv_obj_t *_rain = NULL;
  static inline lv_obj_t *_wind = NULL;
  static inline lv_obj_t *_sunrise = NULL;
  static inline lv_obj_t *_sunset = NULL;
  static inline Day _days[ScreensaverState::FORECAST_DAYS] = {};
  // Calendar card: one or two columns of lines, or the empty message.
  static inline int _calendar_columns = 0;
  static inline std::vector<Line> _lines[CALENDAR_COLUMNS_MAX];
  static inline lv_obj_t *_calendar_list = NULL;
  static inline lv_obj_t *_calendar_empty = NULL;
  static inline lv_obj_t *_calendar_empty_text = NULL;
  // Energy card: the power panel (the large figure and the power line) and the lights panel.
  static inline lv_obj_t *_power_panel = NULL;
  static inline lv_obj_t *_hero_symbol = NULL;
  static inline lv_obj_t *_hero_number = NULL;
  static inline lv_obj_t *_hero_unit = NULL;
  static inline lv_obj_t *_power_line = NULL;
  static inline lv_obj_t *_power_line_icon = NULL;
  static inline lv_obj_t *_power_line_text = NULL;
  static inline lv_obj_t *_lights_panel = NULL;
  static inline lv_obj_t *_lights_icon = NULL;
  static inline lv_obj_t *_lights_number = NULL;
  static inline lv_obj_t *_lights_text = NULL;
  static inline lv_obj_t *_lights_alert = NULL;
};
