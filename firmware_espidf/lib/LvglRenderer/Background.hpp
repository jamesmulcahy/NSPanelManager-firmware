#pragma once
#include <cstdint>
#include <lvgl.h>

#include "Theme.hpp"

// Built-in page backgrounds: generated art, drawn with LVGL fills behind a screen's children, with
// the cards made see-through over it. The art changes colour only down the screen or at a few edges
// per row, so over the terminal link (which compresses runs along rows) it costs a few percent more
// than a plain screen; a photo costs 3-5 times as much.
//
// The art is computed when a preset is chosen, as solid rectangles in LVGL's heap (PSRAM; 7-17 KB):
// runs of one colour per row, merged down the screen. Nothing is stored in flash.
// Portrait (US) screens only; on a landscape screen nothing is drawn.
namespace Background {
enum class Preset : uint8_t { NONE, OCEAN, DUSK, AURORA, PEAKS, VESUVIUS, PARTHENON };

// "none", "ocean", "dusk", "aurora", "peaks", "vesuvius", "parthenon"; unknown names are NONE.
Preset from_name(const char *name);
const char *name(Preset preset);

// GUI task: draw the current preset behind `screen`'s children from now on (once per screen).
void attach(lv_obj_t *screen);

// GUI task: the preset drawn, the card opacity Theme gives cards built after this (see-through
// with a background, solid without), and the palette (palette_for()). Views built before keep
// theirs: rebuild them.
void set(Preset preset, const Theme::Palette *theme = nullptr);
// The palette set() applies: `theme`, or for nullptr ("auto") the one `preset` carries, DARK for
// NONE. Touches nothing, so it may run before LVGL starts.
const Theme::Palette &palette_for(Preset preset, const Theme::Palette *theme);
// The preset drawn: NONE under a light palette, whatever was asked for.
Preset current();

// The art's pixels in row y (0-479) of a 320x480 screen, RGB565, into row[320]. For tests and the
// simulator; drawing uses the rectangles.
void render_row(Preset preset, int y, uint16_t *row);

/**
 * With NSPM_HD_ART, on a 720x720 screen in HD (Theme::hd()): set() also draws the preset's HD art
 * (BackgroundHd), which replaces the rectangles once it is ready. Drawing it takes a while, so the device
 * sets this to run `work(arg)` on a task of its own and then `done(arg)` on the GUI task; unset (the
 * simulator), set() runs both at once.
 */
inline void (*run_off_gui)(void (*work)(void *), void (*done)(void *), void *arg) = nullptr;

/**
 * GUI task: `obj` is a see-through card (Theme::card() or Theme::on_tint() at Theme::card_opa()). Over the
 * HD art, its background is then drawn from the art as blended under that colour when the art was drawn
 * (smooth, dithered) rather than blended by LVGL (RGB565 at 32 levels, no dither: the art bands and its
 * dither breaks up under the card). Without NSPM_HD_ART, or not in HD, nothing.
 */
void see_through(lv_obj_t *obj);
} // namespace Background
