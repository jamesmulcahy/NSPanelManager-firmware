#pragma once
#include <atomic>
#include <lvgl.h>
#include <string>

#include "Theme.hpp"
#include "nspm_icons.h"

// The building blocks the NSPM pages share: a header bar, icon buttons, tiles, slider cards, and
// setters that only touch LVGL when a value really changed. Kept free of ESP-IDF headers (see
// BootScreen.hpp), like everything a page uses.
//
// Why the "only if changed" setters matter: every lv_*_set_* call invalidates its object, and
// every invalidated area is redrawn and sent to the display. A page's show() is called with the
// whole state every time anything changes, so it must cost nothing for what didn't.

namespace Ui {

// A pop-up (the room list) is open and may scroll: the host must not take a drag for a swipe.
// Set on the GUI task, read from any.
inline std::atomic<bool> popup_open{false};

void set_text(lv_obj_t *label, const std::string &text);
void set_text(lv_obj_t *label, const char *text);
void set_visible(lv_obj_t *obj, bool visible);
void set_text_color(lv_obj_t *obj, lv_color_t color);
void set_bg_color(lv_obj_t *obj, lv_color_t color);
void set_font(lv_obj_t *obj, const lv_font_t *font);
void set_checked(lv_obj_t *obj, bool checked);
void set_slider(lv_obj_t *slider, int32_t value);
void set_bar(lv_obj_t *bar, int32_t value);

/**
 * A transparent flex container that takes the space left along its parent's flow and all of the
 * other axis: what a page's body is made of.
 */
lv_obj_t *fill(lv_obj_t *parent, lv_flex_flow_t flow);

/**
 * An icon (an NSPM_ICON_* string) at 24, 40 or 64 px.
 */
lv_obj_t *icon(lv_obj_t *parent, const char *glyph, int size, lv_color_t color);

/**
 * A flat button holding an icon (its first child), white, amber on the pressed colour while
 * pressed, grey when disabled (LV_STATE_DISABLED), as the Nextion pages' buttons are.
 */
lv_obj_t *icon_button(lv_obj_t *parent, const char *glyph, int size);

/**
 * Make `obj` answer touches out to the screen's edge on each side where it is within `reach` px of
 * it. Fingers land past controls near an edge (Lounge's mute: x 2-6 and y 459-477 for a button at
 * 10-54, 420-458), and nothing a finger means to press lies between a control and the edge. Sides
 * farther from an edge keep the control's own bounds, plus `margin` all round (only where no
 * neighbour is that close, as in the header), so neighbours never lose touches to it. Calling it
 * again replaces the earlier reach and margin.
 * Every icon button and tile has this; call it for other controls near an edge.
 */
void reach_screen_edges(lv_obj_t *obj, int32_t reach = Theme::px(24), int32_t margin = 0);
void set_icon(lv_obj_t *button, const char *glyph);
void set_enabled(lv_obj_t *button, bool enabled);

/**
 * The bar across the top of every sub-page: back, then optionally previous / title / next, and
 * a group on the right for page-specific buttons. Any of the buttons may be NULL.
 */
struct Header {
  lv_obj_t *bar = NULL;
  lv_obj_t *back = NULL;
  lv_obj_t *prev = NULL;
  lv_obj_t *title = NULL;
  lv_obj_t *next = NULL;
  lv_obj_t *right = NULL;
};
Header header(lv_obj_t *parent, bool back, bool paging);

/**
 * A card that can be pressed, with a lip: an icon over (or, if `row`, beside) a label. With
 * set_tile_on() the icon goes amber and the card takes the on tint; set_tile_outlined() swaps the
 * lip for a 2 px amber outline (the control being edited). Returns the card; its children are the
 * icon then the label.
 */
lv_obj_t *tile(lv_obj_t *parent, const char *glyph, int icon_size, const char *text, bool row);
lv_obj_t *tile_icon(lv_obj_t *tile);
lv_obj_t *tile_label(lv_obj_t *tile);
void set_tile_on(lv_obj_t *tile, bool on);
void set_tile_outlined(lv_obj_t *tile, bool outlined);

/**
 * Mark a widget (and so its children) as one whose tap raises its own sound, so a press on it does
 * not play the generic touch sound as well (LvglUi's press hook asks has_own_sound()).
 */
void set_own_sound(lv_obj_t *obj, bool own_sound);
bool has_own_sound(lv_obj_t *obj);

/**
 * A slider in a card, with an icon and the value: a row (icon, slider, value) when `vertical` is
 * false, a column (value, slider, icon) when true. The card grows along its parent's flow.
 */
struct SliderCard {
  lv_obj_t *card = NULL;
  lv_obj_t *icon = NULL;
  lv_obj_t *slider = NULL; // Thick, no knob; the indicator is the value
  lv_obj_t *value = NULL;
  lv_obj_t *track = NULL; // What the slider is drawn over: itself, or the hue strip behind it
};
SliderCard slider_card(lv_obj_t *parent, const char *glyph, bool vertical);

/**
 * Turn a slider card's slider into a picker over a gradient: the track is the gradient, the
 * indicator is invisible, and a ring knob marks the value.
 */
void gradient_slider(SliderCard &card, lv_color_t from, lv_color_t to, bool vertical);

/**
 * Like gradient_slider(), over the hue circle (six gradients, as LVGL is built with two stops).
 */
void hue_slider(SliderCard &card, bool vertical);

} // namespace Ui
