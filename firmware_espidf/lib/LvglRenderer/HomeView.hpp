#pragma once
#include <lvgl.h>
#include <string>
#include <vector>

// LVGL replacement for the Nextion `home` page (lib/Pages/HomePage). Pure LVGL, no firmware
// headers: HomePage would fill a HomeState from RoomManager and handle the actions.

/**
 * What the home page shows: the room's (or every room's) ceiling and table lights.
 */
struct HomeState {
  enum class Edit { ALL, CEILING, TABLE }; // Which lights the sliders change (a long press on a tile)

  std::string room;         // "Kitchen", or "All" in all-rooms mode: what the room button reads
  std::vector<std::string> rooms; // The room list, in order
  int room_index = -1;      // The current room's place in `rooms`, -1 for none (all-rooms mode)
  bool all_allowed = true;  // The mode switch may go to "All" (not when locked to the default room)
  std::string time;         // The clock in the corner ("12:34 PM")
  bool all_rooms = false;   // Scenes button shows the all-rooms picture
  int ceiling = 0;          // Brightness 0-100, 0 = off
  int table = 0;
  Edit edit = Edit::ALL;
  int brightness = 0;       // The slider: the average, or the edited group's
  int color_temperature = 0; // 0-100, warm to cool
  bool entities = true;      // The room's entities button (room mode only)
  // What the room's main-page lights have (NSPanelRoomStatus): a control for what it has none of is
  // hidden. All true until known, so that a panel with an older manager shows everything, as before.
  bool has_ceiling = true;          // Ceiling lights: the Ceiling tile
  bool has_table = true;            // Table lights: the Table tile
  bool has_color_temperature = true; // A light that can change colour temperature: its slider
  // The board has microphones: show the hold-to-talk button (custom PCB). The host sets it, as HomeView
  // knows nothing of boards; false everywhere else, where the button is never drawn.
  bool has_mic = false;
};

class HomeView {
public:
  enum class Action {
    SCENES,          // b_scenes
    CEILING,         // tap: toggle; long press: edit only the ceiling lights
    TABLE,
    CEILING_LONG,
    TABLE_LONG,
    BRIGHTNESS,      // value: 0-100, on release
    COLOR_TEMPERATURE,
    SELECT_ROOM,     // value: the room's index in HomeState::rooms
    SET_MODE,        // value: 0 the room's lights, 1 every room's
    // Every press and release of the light tiles, for a host that times the hold itself (HomePage's
    // special_mode_trigger_time), as the Nextion buttons report them.
    CEILING_DOWN,
    CEILING_UP,
    TABLE_DOWN,
    TABLE_UP,
    ENTITIES,        // b_room_entities: the room's entities page
    // A press on a light tile that ended as a swipe, or slid off: neither a tap nor a hold.
    CEILING_CANCEL,
    TABLE_CANCEL,
    // The hold-to-talk button (HomeState::has_mic): pressed, released, or the press became a swipe.
    MIC_DOWN,
    MIC_UP,
    MIC_CANCEL,
  };
  // What a toast says it with: a tick, a cross, or a hint.
  enum class Toast { OK, ERROR, HINT };
  static inline void (*on_action)(Action action, int32_t value) = nullptr;
  // Whether the press being released was a swipe (the host detects them): a tile then cancels.
  static inline bool (*is_swipe)() = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const HomeState &state);

  // The mic's input level, 0-100, polled while recording (show_mic_recording) about every 40 ms. GUI task.
  static inline int (*mic_level)() = nullptr;
  // Called with that poll while the mic button is pressed (the recorder's hold lease); not once it is
  // released, deleted (a rebuild) or hidden. GUI task.
  static inline void (*mic_holding)() = nullptr;
  // A line for the host's log (the button was left out for want of room). GUI task.
  static inline void (*log_warning)(const char *message) = nullptr;
  /**
   * GUI task: the button in its recording state (the meter running, polling mic_level) or idle again.
   * No-ops unless the page is built with the button shown.
   */
  static void show_mic_recording(bool recording);
  // GUI task: the meter, 0-100, without waiting for the poll (the simulator).
  static void show_mic_level(int level);
  // GUI task: a message over the page, above the button, for about two seconds. No-op without the button.
  static void show_toast(const std::string &text, Toast kind);

private:
  static inline lv_obj_t *_time = NULL;
  static inline lv_obj_t *_scenes = NULL;
  static inline lv_obj_t *_entities = NULL;
  static inline bool _landscape = false;
  static inline lv_obj_t *_lights = NULL;  // The tiles' group
  static inline lv_obj_t *_sliders = NULL; // The slider cards' group
  static inline lv_obj_t *_brightness_card = NULL;
  static inline lv_obj_t *_kelvin_card = NULL;
  static inline lv_obj_t *_no_lights = NULL; // In place of all of them, for a room without lights
  static inline lv_obj_t *_no_lights_text = NULL;
  static inline lv_obj_t *_ceiling = NULL;
  static inline lv_obj_t *_table = NULL;
  static inline lv_obj_t *_ceiling_value = NULL;
  static inline lv_obj_t *_table_value = NULL;
  static inline lv_obj_t *_brightness = NULL;
  static inline lv_obj_t *_brightness_value = NULL;
  static inline lv_obj_t *_kelvin = NULL;
  static inline lv_obj_t *_kelvin_value = NULL;
  static inline lv_obj_t *_room = NULL;      // The room dropdown
  static inline lv_obj_t *_mode_room = NULL; // The mode switch's halves
  static inline lv_obj_t *_mode_all = NULL;
  static inline lv_obj_t *_catcher = NULL;   // Behind the open room list: a tap there closes it
  // What the dropdown holds (it keeps a pointer to its button text, not a copy).
  static inline std::string _options;
  static inline std::string _room_text;

  // Hold to talk: a bar between the body and the footer, with the meter drawn behind its icon and text.
  static inline lv_obj_t *_body = NULL;
  static inline lv_obj_t *_mic = NULL;
  static inline lv_obj_t *_mic_icon = NULL;
  static inline lv_obj_t *_mic_label = NULL;
  static inline lv_obj_t *_mic_meter = NULL;
  static inline lv_timer_t *_mic_timer = NULL;
  static inline int _mic_shown = 0;    // The meter as drawn: the level, decaying
  static inline bool _mic_fits = true; // Checked once, when the button first shows
  static inline bool _mic_checked = false;
  static inline lv_obj_t *_toast = NULL;
  static inline lv_obj_t *_toast_icon = NULL;
  static inline lv_obj_t *_toast_text = NULL;
  static inline lv_timer_t *_toast_timer = NULL;

  static void _room_event(lv_event_t *event);
  static void _mic_deleted(lv_event_t *event);
  static void _mic_poll(lv_timer_t *timer);
  static void _show_controls(const HomeState &state);
};
