#pragma once
#include <lvgl.h>
#include <string>

// LVGL replacements for the HMI's own pages, which no firmware page drives today:
//
// - MenuView: the `dropdown` page (swipe down on the home page): alarm, night mode, clean screen,
//   reboot, most of them "Coming soon...".
// - CleanScreenView: `cleanscreen`: touch is ignored for a while so the glass can be wiped; a
//   touch restarts the countdown, which returns to the home page when it runs out.
// - DialogView: a confirmation over any page (there is no Nextion equivalent; e.g. "Reboot?").
//
// On the Nextion these run in the display's own code (page events, timers); with LVGL the
// firmware runs them.

struct MenuItem {
  const char *icon = NULL; // NULL: an empty slot
  std::string label;       // "Night mode", "ON", "Disarmed"
  std::string note;        // "Coming soon...", shown above in red
  bool on = false;
};

struct MenuState {
  static constexpr int ITEMS = 6;
  std::string title = "Panel";
  MenuItem items[ITEMS];
};

class MenuView {
public:
  enum class Action { BACK, ITEM };
  static inline void (*on_action)(Action action, int32_t item) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const MenuState &state);

private:
  static inline lv_obj_t *_title = NULL;
  static inline lv_obj_t *_tiles[MenuState::ITEMS] = {};
  static inline lv_obj_t *_notes[MenuState::ITEMS] = {};
};

struct CleanScreenState {
  int remaining = 100;      // Countdown, 100 -> 0
  std::string text = "Screen locked for cleaning";
};

class CleanScreenView {
public:
  static inline void (*on_touch)() = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const CleanScreenState &state);

private:
  static inline lv_obj_t *_text = NULL;
  static inline lv_obj_t *_bar = NULL;
};

struct DialogState {
  bool visible = false;
  std::string title;    // "Reboot the panel?"
  std::string message;
  std::string confirm;  // "Reboot"
  std::string cancel = "Cancel";
  bool danger = false;  // Confirm in red
};

class DialogView {
public:
  static inline void (*on_answer)(bool confirmed) = nullptr;

  /**
   * Build the (hidden) dialog over everything else in `parent`.
   */
  static void build(lv_obj_t *parent, bool landscape);
  static void show(const DialogState &state);

private:
  static inline lv_obj_t *_overlay = NULL;
  static inline lv_obj_t *_title = NULL;
  static inline lv_obj_t *_message = NULL;
  static inline lv_obj_t *_confirm = NULL;
  static inline lv_obj_t *_cancel = NULL;
};
