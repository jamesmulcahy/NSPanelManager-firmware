// Ported from nspanel-terminal f0c1394 (components/nspanel_terminal/gap_splitter.h), MIT licence,
// Copyright (c) 2026 NSPanel public contributors. Changed: the namespace and include names only.
// See lib/NextionTerminal/README.md.
#pragma once
#include <algorithm>
#include <cstdint>

namespace nspanel_terminal {
// Incremental gap cost, calibrated against the protocol-7 sparse sweep:
// 0.65 us / additional LCD pixel + 3.2 us / encoded byte. The effective
// split budget grows from 0.6 ms at 4 rows to 2 ms at 16 rows; short tails
// cannot amortise a wide gap as well. Units below are 1/20 microsecond.
// This is a throughput estimate, not a wire-time or electrical timing claim.
struct GapSplitter {
  static uint32_t split_cost(unsigned height) { return std::max(10000u, (7000u * height + 8000u) / 3); }
  static uint32_t cost(unsigned pixels, unsigned encoded_bytes) {
    return 13u * pixels + 64u * encoded_bytes;
  }
  static bool merge(const uint8_t *frame, unsigned stride, unsigned x, unsigned y,
                    unsigned width, unsigned height) {
    const unsigned pixels = width * height;
    const uint32_t budget = split_cost(height);
    const unsigned repeat_bytes = 3 * ((pixels + 127) / 128);
    // Bounds avoid sampling both tiny gaps and gaps whose pixels alone cost
    // more than another rectangle. No fixed gap width is used.
    if (cost(pixels, 2 * pixels + repeat_bytes) <= budget) return true;
    if (cost(pixels, repeat_bytes) > budget) return false;
    unsigned density = 0;
    const unsigned count = std::min(width, 8u);
    // Visit every row, with a moving block, plus left/centre/right blocks on
    // three rows. At most 176 pixel reads for a 16-row strip. Each row has
    // equal weight. Horizontal detail can still fall between sampled blocks.
    for (unsigned r = 0; r < height; ++r) {
      const unsigned blocks = (r == 0 || r == height / 2 || r == height - 1) ? std::min(3u, (width + 7) / 8) : 1;
      unsigned changes = 0, comparisons = 0;
      for (unsigned b = 0; b < blocks; ++b) {
        const unsigned offset = blocks > 1 ? b * (width - count) / (blocks - 1)
                                             : (height > 1 ? r * (width - count) / (height - 1) : 0);
        const uint8_t *p = frame + 2 * ((y + r) * stride + x + offset);
        uint8_t previous_lo = p[0], previous_hi = p[1];
        for (unsigned i = 1; i < count; ++i) {
          const uint8_t lo = p[2*i], hi = p[2*i+1];
          changes += lo != previous_lo || hi != previous_hi;
          previous_lo = lo;
          previous_hi = hi;
          ++comparisons;
        }
      }
      density += changes * 256 / comparisons;
    }
    const unsigned bytes = repeat_bytes + (2 * pixels * density + height * 256 - 1) / (height * 256);
    return cost(pixels, bytes) <= budget;
  }
};
}  // namespace nspanel_terminal
