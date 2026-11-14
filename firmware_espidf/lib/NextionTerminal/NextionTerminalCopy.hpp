#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace nspanel_terminal {

// The receiver's short side; portrait screens are this wide (NextionTerminal::start() checks).
static constexpr int COPY_T_HEIGHT = 320;

/**
 * Copy `rows` rows of a `tw`-wide rectangle in the receiver's coordinates, starting at receiver
 * column tx0 and row ty, from `pixels` (the screen area x1,y1.. with `stride` pixels per row) into
 * `out`, row by row. Screen to receiver:
 *   90:  tx = y - crop,                 ty = 319 - x
 *   270: tx = t_width - 1 - (y - crop), ty = x
 *   180: tx = t_width - 1 - (x - crop), ty = t_height - 1 - y
 *   0:   tx = x - crop,                 ty = y
 * Turned copies go in 16x16 tiles: one step along a receiver row is a whole screen row in the
 * source, so a tile keeps reads within 16 source rows' cache lines and writes within 16 out rows.
 */
template <int ROTATION>
inline void copy_rotated_t(uint16_t *out, int tw, int rows, int tx0, int ty, const uint16_t *pixels, int x1, int y1, int stride, int crop,
                           int t_width, int t_height) {
  if constexpr (ROTATION == 0) {
    for (int r = 0; r < rows; r++) {
      memcpy(out + r * tw, pixels + (tx0 + crop - x1) + (ty + r - y1) * stride, tw * sizeof(uint16_t));
    }
    return;
  }
  if constexpr (ROTATION == 180) {
    // A receiver row is a screen row read backwards, from the bottom of the area up.
    for (int r = 0; r < rows; r++) {
      const uint16_t *src = pixels + (t_width - 1 - tx0 + crop - x1) + (t_height - 1 - (ty + r) - y1) * stride;
      uint16_t *dst = out + r * tw;
      for (int c = 0; c < tw; c++) {
        dst[c] = src[-c];
      }
    }
    return;
  }
  constexpr int TILE = 16;
  for (int r0 = 0; r0 < rows; r0 += TILE) {
    const int r1 = std::min(rows, r0 + TILE);
    for (int c0 = 0; c0 < tw; c0 += TILE) {
      const int c1 = std::min(tw, c0 + TILE);
      for (int r = r0; r < r1; r++) {
        uint16_t *dst = out + r * tw;
        if constexpr (ROTATION == 90) {
          // x = 319 - ty, fixed for this row; y = tx + crop, one source row per column.
          const uint16_t *src = pixels + (COPY_T_HEIGHT - 1 - (ty + r) - x1) + (tx0 + crop - y1) * stride;
          for (int c = c0; c < c1; c++) {
            dst[c] = src[c * stride];
          }
        } else {
          // x = ty, fixed for this row; y = t_width - 1 - tx + crop, going up a row per column.
          const uint16_t *src = pixels + ((ty + r) - x1) + (t_width - 1 - tx0 + crop - y1) * stride;
          for (int c = c0; c < c1; c++) {
            dst[c] = src[-c * stride];
          }
        }
      }
    }
  }
}

inline void copy_rotated(int rotation, uint16_t *out, int tw, int rows, int tx0, int ty, const uint16_t *pixels, int x1, int y1, int stride, int crop,
                         int t_width, int t_height) {
  switch (rotation) {
  case 90:
    copy_rotated_t<90>(out, tw, rows, tx0, ty, pixels, x1, y1, stride, crop, t_width, t_height);
    break;
  case 270:
    copy_rotated_t<270>(out, tw, rows, tx0, ty, pixels, x1, y1, stride, crop, t_width, t_height);
    break;
  case 180:
    copy_rotated_t<180>(out, tw, rows, tx0, ty, pixels, x1, y1, stride, crop, t_width, t_height);
    break;
  default:
    copy_rotated_t<0>(out, tw, rows, tx0, ty, pixels, x1, y1, stride, crop, t_width, t_height);
    break;
  }
}

} // namespace nspanel_terminal
