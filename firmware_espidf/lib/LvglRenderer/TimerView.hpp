#pragma once
#include <lvgl.h>
#include <string>
#include <vector>

// The timers pages, reached from the swipe-down menu's "Timers" item (TimerPage drives them):
//
// - the list of running timers, each with a cancel button, and "New timer";
// - the set page: hours, minutes and seconds rollers, quick picks, Start and Cancel;
// - the alarm, shown when a timer finishes: "Timer done" and a large Dismiss button.
//
// Like the other views, kept free of ESP-IDF headers: the page hands over text already formatted.

struct TimerRow {
  uint32_t id = 0;
  std::string remaining; // "4:32", "1:02:03"
  std::string total;     // "10:00", shown as "of 10:00"
};

struct TimersState {
  std::vector<TimerRow> rows; // At most TimerView::MAX_ROWS, soonest first
  bool can_add = true;        // False while the most timers are running
};

struct TimerSetState {
  int hours = 0;
  int minutes = 5;
  int seconds = 0;
};

struct TimerAlarmState {
  std::string title = "Timer done"; // "2 timers done"
  std::string duration;             // "10:00"
  std::string note;                 // "timer", or "+1 more"
};

class TimerView {
public:
  enum class Action {
    BACK,         // Timers page: back to the menu
    NEW_TIMER,    // Timers page: open the set page
    CANCEL_TIMER, // Timers page: `value` is the timer's id
    START,        // Set page: `value` is the duration in seconds (never 0)
    CANCEL_SET,   // Set page: Cancel, or back
    DISMISS,      // Alarm
  };
  // GUI task.
  static inline void (*on_action)(Action action, int32_t value) = nullptr;

  // The most rows the Timers page has (TimerManager::MAX_TIMERS).
  static constexpr int MAX_ROWS = 4;

  static void build_timers(lv_obj_t *parent, bool landscape);
  static void build_set(lv_obj_t *parent, bool landscape);
  static void build_alarm(lv_obj_t *parent, bool landscape);
  static void show(const TimersState &state);
  static void show(const TimerSetState &state);
  static void show(const TimerAlarmState &state);
};
