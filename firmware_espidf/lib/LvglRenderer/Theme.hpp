#pragma once
#include <lvgl.h>

// The LVGL screens' shared look: colours, fonts and the card container. Kept free of ESP-IDF
// headers (see BootScreen.hpp).

namespace Theme {
/**
 * The colours that make a theme. New colours are exact in RGB565, so what the planner sends is
 * what was meant (DARK keeps its original values). What colours mean is not in it and stays the
 * same in every palette: amber "on", warm, good, and the kelvin, saturation and hue gradients.
 */
struct Palette {
  const char *name;
  // Dark text on light cards. The background presets are dark art, so none is drawn under it.
  bool light;
  uint32_t background; // Behind everything
  uint32_t card;       // Cards and tiles
  uint32_t text;
  uint32_t muted;  // Secondary text
  uint32_t accent; // Highlights that are not "on": the boot screen's symbols, dialog buttons
  uint32_t track;  // Slider and bar tracks, the selected option
  uint32_t pressed;
  uint32_t disabled; // Controls the device can't use now
  uint32_t danger;
  uint32_t indicator; // A plain slider's value
};
// The default; a light one; and one per background preset (Background.hpp), tinted to sit on its art.
extern const Palette DARK, LIGHT, OCEAN, DUSK, AURORA, PEAKS;

namespace detail {
extern const Palette *palette;
}
inline const Palette &palette() { return *detail::palette; }
// GUI task (or before LVGL starts). Views built before keep their colours: rebuild them.
void set_palette(const Palette &palette);
// "dark", "light", "ocean", "dusk", "aurora", "peaks"; nullptr for anything else ("auto").
const Palette *palette_from_name(const char *name);

inline lv_color_t background() { return lv_color_hex(palette().background); }
inline lv_color_t card() { return lv_color_hex(palette().card); }
inline lv_color_t text() { return lv_color_hex(palette().text); }
inline lv_color_t muted() { return lv_color_hex(palette().muted); }
inline lv_color_t accent() { return lv_color_hex(palette().accent); }
inline lv_color_t track() { return lv_color_hex(palette().track); }
inline lv_color_t pressed() { return lv_color_hex(palette().pressed); }
inline lv_color_t disabled() { return lv_color_hex(palette().disabled); }
inline lv_color_t danger() { return lv_color_hex(palette().danger); }
inline lv_color_t indicator() { return lv_color_hex(palette().indicator); }
// The same in every palette.
inline lv_color_t warm() { return lv_color_hex(0xF8A038); }
inline lv_color_t good() { return lv_color_hex(0x48C878); }
// "On", as the Nextion pages' amber (RGB565 65024): lights that are on, the control being edited.
inline lv_color_t on() { return lv_color_hex(0xF8C000); }
// Behind the screensaver and the clean screen when they have no background: black, or a light
// palette's background, as its dark text would not show on black.
inline lv_color_t blank() { return palette().light ? background() : lv_color_black(); }
// Corner radii: containers (cards, tiles, the room list), the controls in them (buttons, sliders),
// a control inset in a container's padding (outer radius minus padding), and thin bars.
constexpr int32_t RADIUS_CARD = 12;
constexpr int32_t RADIUS_CONTROL = 10;
constexpr int32_t RADIUS_INSET = 8;
constexpr int32_t RADIUS_BAR = 3;
// Gaps and padding: a 4 px scale and a hairline. Odd values that place one thing (a label's
// indent, a list's row pitch) stay where they are used.
constexpr int32_t SPACE_XS = 2;
constexpr int32_t SPACE_S = 4;
constexpr int32_t SPACE_M = 8;
constexpr int32_t SPACE_L = 12;
constexpr int32_t SPACE_XL = 16;

// How opaque cards and tiles are: solid, or see-through over a page background (Background.hpp).
// Read when a card is built.
lv_opa_t card_opa();
void set_card_opa(lv_opa_t opa);

/**
 * The part of the screen a page may use. The EU HMI's art is 456 px wide on its 480x320
 * controller: the rightmost 24 columns sit under the bezel, so pages leave them alone.
 */
inline int32_t usable_width(bool landscape) { return landscape ? 456 : 320; }
inline int32_t usable_height(bool landscape) { return landscape ? 320 : 480; }

// The clock's digits (tools/lv_font_subset.py), and the text sizes.
const lv_font_t *clock_font();
// The screensaver clock on a portrait screen: digits, colon and dash only.
const lv_font_t *big_clock_font();
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
