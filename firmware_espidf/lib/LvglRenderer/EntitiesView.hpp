#pragma once
#include <lvgl.h>
#include <string>
#include <vector>

// LVGL replacement for the Nextion `items4` / `items8` / `items12` pages: a room's entities, its
// scenes (lib/Pages/EntitiesPage) and a media player's sources (lib/Pages/MediaSourcePage, which
// shows text-only slots). One page whatever the size; the grid follows the slot count and the
// screen's shape.

struct EntitySlot {
  std::string name;
  const char *icon = NULL; // An NSPM_ICON_*; NULL for a text-only slot (media sources)
  bool on = false;         // Icon (or, text-only, the name) in amber
  bool empty = true;       // Nothing configured in this slot: not drawn
};

struct EntitiesState {
  static constexpr int MAX_SLOTS = 12;
  std::string header;           // "Kitchen", "Scenes", "Scene saved", "Favorites", ...
  int size = 4;                 // 4, 8 or 12, as the manager's page type
  EntitySlot slots[MAX_SLOTS];
  bool can_prev = true;         // Header buttons
  bool can_next = true;
  bool own_sound = false;       // A tap on a slot raises its own sound (scenes), so it gets no touch sound
  int save_progress = -1;       // 0-100 while a scene is held to be saved, else -1
};

class EntitiesView {
public:
  // SLOT_RELEASED follows every press of a slot, held or not (a scene save stops when let go).
  enum class Action { BACK, PREV, NEXT, SLOT, SLOT_LONG /* hold: save scene */, SLOT_RELEASED };
  static inline void (*on_action)(Action action, int32_t slot) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const EntitiesState &state);

private:
  static void _layout(int size);

  static inline bool _landscape = false;
  static inline int _size = 0;
  static inline int _columns = 1;
  static inline lv_obj_t *_title = NULL;
  static inline lv_obj_t *_prev = NULL;
  static inline lv_obj_t *_next = NULL;
  static inline lv_obj_t *_grid = NULL;
  static inline lv_obj_t *_tiles[EntitiesState::MAX_SLOTS] = {};
  static inline lv_obj_t *_save = NULL;
};
