#pragma once
#if defined(NSPM_HD_ART)
#include <cstdint>

#include "Background.hpp"

/**
 * The page backgrounds at the custom PCB's 720x720 (NSPM_HD_ART: custom_pcb and the simulator), in place
 * of the NSPanel's flat art scaled up. The NSPanel's art is made of long runs of one colour, as every frame
 * crosses a 4 Mbaud link to its display; the P4 draws into its own frame buffers, so these are drawn the way
 * they look best: smooth gradients, soft edges, lit ridges and stars, dithered (8x8 Bayer) to RGB565 so
 * the dark skies don't band. Each preset keeps its scene and colours, so Theme's palettes still fit it.
 *
 * Drawn once per preset (Background::set()), into a SIZE x SIZE buffer that Background::draw() copies
 * from, as it costs far too much per redraw. Pure computation: no LVGL, any task.
 */
namespace BackgroundHd {
constexpr int SIZE = 720;

// Whether `preset` has HD art (all but NONE).
bool has(Background::Preset preset);

// Draw `preset` into `out`: SIZE x SIZE RGB565 pixels, row by row. False for a preset without HD art.
bool render(Background::Preset preset, uint16_t *out);
} // namespace BackgroundHd
#endif
