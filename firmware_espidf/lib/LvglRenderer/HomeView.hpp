#pragma once
#include <lvgl.h>
#include <string>

// LVGL replacement for the Nextion `home` page (lib/Pages/HomePage). Pure LVGL, no firmware
// headers: HomePage would fill a HomeState from RoomManager and handle the actions.

/**
 * What the home page shows: the room's (or every room's) ceiling and table lights.
 */
struct HomeState {
  enum class Edit { ALL, CEILING, TABLE }; // Which lights the sliders change (a long press on a tile)

  std::string room;         // "Kitchen", or "All" in all-rooms mode
  std::string mode;         // "Room lights" / "All lights"
  std::string time;         // The clock in the corner ("12:34 PM")
  bool all_rooms = false;   // Scenes button shows the all-rooms picture
  int ceiling = 0;          // Brightness 0-100, 0 = off
  int table = 0;
  Edit edit = Edit::ALL;
  int brightness = 0;       // The slider: the average, or the edited group's
  int color_temperature = 0; // 0-100, warm to cool
  bool entities = true;      // The room's entities button (room mode only, as on the Nextion page)
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
    NEXT_ROOM,       // b_nextroom
    NEXT_MODE,       // mode
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
  };
  static inline void (*on_action)(Action action, int32_t value) = nullptr;
  // Whether the press being released was a swipe (the host detects them): a tile then cancels.
  static inline bool (*is_swipe)() = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const HomeState &state);

private:
  static inline lv_obj_t *_time = NULL;
  static inline lv_obj_t *_scenes = NULL;
  static inline lv_obj_t *_entities = NULL;
  static inline lv_obj_t *_ceiling = NULL;
  static inline lv_obj_t *_table = NULL;
  static inline lv_obj_t *_ceiling_value = NULL;
  static inline lv_obj_t *_table_value = NULL;
  static inline lv_obj_t *_brightness = NULL;
  static inline lv_obj_t *_brightness_value = NULL;
  static inline lv_obj_t *_kelvin = NULL;
  static inline lv_obj_t *_kelvin_value = NULL;
  static inline lv_obj_t *_room = NULL;
  static inline lv_obj_t *_mode = NULL;
};
