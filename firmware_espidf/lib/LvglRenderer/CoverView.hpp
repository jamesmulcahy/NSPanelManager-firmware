#pragma once
#include <lvgl.h>
#include <string>

// LVGL replacement for the `cover` page (lib/Pages/CoverPage): state, a position
// slider, and open / stop / close.

struct CoverState {
  std::string name;          // "Lounge blinds"
  std::string state;         // "Open · 75%", "Closing", "Unavailable"
  bool is_open = false;      // Icon: open or closed shutter
  bool has_position = true;  // The slider is shown only if the cover can be positioned
  int position = 0;          // 0 closed - 100 open
  bool can_open = true;
  bool can_stop = true;
  bool can_close = true;
};

class CoverView {
public:
  enum class Action { BACK, OPEN, STOP, CLOSE, POSITION };
  static inline void (*on_action)(Action action, int32_t value) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const CoverState &state);

private:
  static inline lv_obj_t *_title = NULL;
  static inline lv_obj_t *_icon = NULL;
  static inline lv_obj_t *_state = NULL;
  static inline lv_obj_t *_slider_card = NULL;
  static inline lv_obj_t *_slider = NULL;
  static inline lv_obj_t *_value = NULL;
  static inline lv_obj_t *_open = NULL;
  static inline lv_obj_t *_stop = NULL;
  static inline lv_obj_t *_close = NULL;
};
