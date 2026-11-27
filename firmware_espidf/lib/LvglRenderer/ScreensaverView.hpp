#pragma once
#include "EnergyFormat.hpp"
#include "Theme.hpp"
#include <atomic>
#include <cstdint>
#include <lvgl.h>
#include <memory>
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
//
// A card is built whenever it is configured, but takes its turn only while it has something to show:
// the now-playing card joins the rotation while its player plays (ScreensaverState::now_playing_eligible).
// That changes the rotation and nothing else, so playback starting or stopping never rebuilds the page.
// The one exception is a DATETIME_* mode whose only card is now-playing: with nothing playing it is the
// centred clock, and the header and card are built when playback starts.

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
  NOW_PLAYING = 3,
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

// The now-playing card: the player's album art over its title and artist. Only filled in while the
// player is eligible.
struct NowPlayingState {
  std::string title;
  std::string artist;
  std::string player;               // "Lounge"
  const lv_image_dsc_t *art = NULL; // RGB565, ScreensaverView::art_size() square; NULL: placeholder
  // Keeps art's pixels valid (an LvglArt::Handle on the panel). The view holds it for as long as it
  // draws art: until a show() with other art, or until the view is deleted.
  std::shared_ptr<const void> art_hold;
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
  // The now-playing card (a Card of type NOW_PLAYING says it is configured): in the rotation only while
  // eligible, that is while its player is playing and not muted.
  bool now_playing_eligible = false;
  NowPlayingState now_playing;
};

class ScreensaverView {
public:
  // Any touch wakes the panel.
  static inline void (*on_touch)() = nullptr;

  // The body turned to a card (also when it first shows one): `weather`, `calendar` or `energy`, and how long
  // it stays, in seconds. On the GUI task. For the page's log; the simulator leaves it unset.
  static inline void (*on_card)(const char *card, uint32_t seconds) = nullptr;

  // The now-playing card's art is a square this many pixels a side: the largest that leaves room for
  // its text, a multiple of 8 and at most MAX_ART_SIZE. Worked out from the layout when the card is
  // built (0 until then), and the size to ask the manager for. on_art_size runs on the GUI task each
  // time the card is built.
  static constexpr Theme::Px MAX_ART_SIZE{256};
  static inline void (*on_art_size)(uint16_t size) = nullptr;
  static uint16_t art_size() { return ScreensaverView::_art_size.load(); }

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const ScreensaverState &state);

  /**
   * Show the next body card now, as the rotation timer does when a card's time is up. For the
   * simulator.
   */
  static void next_card();

  /**
   * GUI task: a swipe turned the body by `step` cards (1 the next, -1 the one before), wrapping round.
   * The card turned to stays twice its time, so it doesn't move on while it is being read.
   */
  static void turn(int step);

  /**
   * Any task: how many cards the body turns between (0 or 1: it doesn't turn).
   */
  static size_t card_count() { return ScreensaverView::_card_count.load(); }

private:
  struct Day {
    lv_obj_t *day, *icon, *max_min, *rain, *wind;
  };
  // The body's cards, each built once and shown in turn.
  enum Body : uint8_t { WEATHER, CALENDAR, ENERGY, NOW_PLAYING, BODY_COUNT };
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
  static lv_obj_t *_build_now_playing(bool landscape);
  static void _build_clock();
  static void _show_calendar(const ScreensaverState &state);
  static void _show_energy(const EnergyState &energy);
  static void _show_now_playing(const NowPlayingState &now_playing);
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
  static inline std::atomic<size_t> _card_count = 0; // _sequence.size(), for other tasks
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
  // Now-playing card: the art over the title and artist (and the player's name, if it fits).
  static inline std::atomic<uint16_t> _art_size = 0;
  static inline lv_obj_t *_np_art = NULL;
  static inline lv_obj_t *_np_placeholder = NULL;
  static inline std::shared_ptr<const void> _np_art_hold; // Whatever keeps _np_art's source valid (GUI task)
  static inline lv_obj_t *_np_title = NULL;
  static inline lv_obj_t *_np_artist = NULL;
  static inline lv_obj_t *_np_player_row = NULL;
  static inline lv_obj_t *_np_player = NULL;
};
