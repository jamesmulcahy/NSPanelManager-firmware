#pragma once
#include <lvgl.h>

// The LVGL screens' shared look: colours, fonts and the card container. Kept free of ESP-IDF
// headers (see BootScreen.hpp).

namespace Theme {
// Colours, chosen to be exact in RGB565 so what the planner sends is what was meant.
inline lv_color_t background() { return lv_color_hex(0x10141C); }
inline lv_color_t card() { return lv_color_hex(0x1C2230); }
inline lv_color_t text() { return lv_color_hex(0xF0F0F0); }
inline lv_color_t muted() { return lv_color_hex(0x8890A0); }
inline lv_color_t accent() { return lv_color_hex(0x38A8F8); }
inline lv_color_t warm() { return lv_color_hex(0xF8A038); }
inline lv_color_t good() { return lv_color_hex(0x48C878); }
// "On", as the Nextion pages' amber (RGB565 65024): lights that are on, the control being edited.
inline lv_color_t on() { return lv_color_hex(0xF8C000); }
// Slider and bar tracks, pressed buttons, and controls the device can't use now.
inline lv_color_t track() { return lv_color_hex(0x283448); }
inline lv_color_t pressed() { return lv_color_hex(0x303C50); }
inline lv_color_t disabled() { return lv_color_hex(0x586070); }
inline lv_color_t danger() { return lv_color_hex(0xF85048); }

/**
 * The part of the screen a page may use. The EU HMI's art is 456 px wide on its 480x320
 * controller: the rightmost 24 columns sit under the bezel, so pages leave them alone.
 */
inline int32_t usable_width(bool landscape) { return landscape ? 456 : 320; }
inline int32_t usable_height(bool landscape) { return landscape ? 320 : 480; }

// The clock's digits (tools/lv_font_subset.py), and the text sizes.
const lv_font_t *clock_font();
const lv_font_t *title_font();
const lv_font_t *body_font();
// Montserrat 32, digits and what temperatures need ("72°F", "-3.5°C", "45%").
const lv_font_t *large_font();
// Material Design Icons (nspm_icons.h) at 24, 40 or 64 px; 40 and 64 hold only the icons drawn
// that big (tools/gen_fonts.sh).
const lv_font_t *icon_font(int size);

/**
 * Style `obj` as the screen behind everything: background colour, padding, no scrolling.
 */
void screen(lv_obj_t *obj);

/**
 * A rounded card, a flex column. The default theme's dark text colour is overridden here, as
 * every child would inherit it.
 */
lv_obj_t *card(lv_obj_t *parent);

/**
 * A transparent flex container, for grouping without drawing anything.
 */
lv_obj_t *group(lv_obj_t *parent, lv_flex_flow_t flow);

/**
 * A label in `font` and `color`.
 */
lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

/**
 * Prepare `screen` for a full-screen page and return the area the page builds in: the usable
 * part of the screen (usable_width()), a flex column with the pages' margins.
 */
lv_obj_t *page_root(lv_obj_t *screen, bool landscape);
} // namespace Theme
