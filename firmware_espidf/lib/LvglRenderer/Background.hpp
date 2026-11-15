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
enum class Preset : uint8_t { NONE, OCEAN, DUSK, AURORA, PEAKS };

// "none", "ocean", "dusk", "aurora", "peaks"; unknown names are NONE.
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
} // namespace Background
