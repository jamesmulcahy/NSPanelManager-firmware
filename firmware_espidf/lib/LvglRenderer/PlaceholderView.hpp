#pragma once
#include <lvgl.h>
#include <string>

// Stands in for a Nextion page that has no LVGL version yet, when LVGL is the panel's whole UI
// (LvglUi on a terminal TFT, which has no Nextion pages): says which page it is, and offers a way
// back, so the panel never lands on a page that cannot be left.
class PlaceholderView {
public:
  enum class Action { BACK };
  static inline void (*on_action)(Action action) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  // `page`: the Nextion page's name ("home", "items8", ...).
  static void show(const std::string &page);

private:
  static inline lv_obj_t *_title = NULL;
};
