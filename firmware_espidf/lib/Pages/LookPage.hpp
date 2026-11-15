#pragma once

/**
 * The look picker (LookView), from the swipe-down menu's "Look" item; LVGL UI only. A pick is applied
 * at once, saved, and reported to the manager (LvglLook); the page stays, with the new look selected.
 */
class LookPage {
public:
  static void show();
  static void unshow();

private:
  static void _render();
  static void _action(int action, int32_t look);
};
