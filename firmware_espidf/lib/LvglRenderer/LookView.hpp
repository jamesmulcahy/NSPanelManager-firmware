#pragma once
#include <lvgl.h>

// The look picker, reached from the swipe-down menu: the standard looks as one choice each (a page
// background with the palette made for it, or a plain palette), rather than a background picker and
// a theme picker. The firmware's LookPage applies a pick and reports it to the manager (LvglLook).

// A look the panel offers: a background preset and a palette, by the names the lvgl_background and
// lvgl_theme commands and NSPanelConfig use.
struct Look {
  const char *name;       // "Ocean", shown under its preview
  const char *background; // Background preset (Background.hpp)
  const char *theme;      // Palette (Theme.hpp); "auto" is the background's own
};

namespace Looks {
constexpr int COUNT = 8;
extern const Look ALL[COUNT];
// The look that `background` and `theme` draw (compared as drawn: the preset, and the palette after
// "auto" and "light" are resolved), or -1 for a combination none of them is (set from the manager).
int find(const char *background, const char *theme);
} // namespace Looks

struct LookState {
  int selected = -1; // Looks::ALL index, -1 for none
};

class LookView {
public:
  enum class Action { BACK, PICK };
  // PICK: `look` is the Looks::ALL index. GUI task.
  static inline void (*on_action)(Action action, int32_t look) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const LookState &state);

  // Each preview's size on a portrait screen, where it shows the background's art.
  static constexpr int32_t PREVIEW_W = 132;
  static constexpr int32_t PREVIEW_H = 60;

private:
  static inline lv_obj_t *_tiles[Looks::COUNT] = {};
};
