#pragma once
#include <cstddef>
#include <cstdint>

/**
 * Image files from a screen capture (LvglRenderer::capture()): RGB565 pixels, little-endian, row by
 * row. The encoders stream to a Sink, so no file is ever held whole.
 *
 * - BMP: 16 bits per pixel, BI_BITFIELDS with 565 masks, top-down. The pixels go out as they are.
 * - PNG: 8-bit RGB, no filter, deflated by the ESP32 ROM's tdefl (miniz) at its fastest setting,
 *   one IDAT chunk per block tdefl emits. The compressor's ~160 KB of state is in PSRAM for the
 *   call only. A typical page comes to 5-50 KB.
 */
class LvglScreenshot {
public:
  // Where an encoder writes. write() returning false stops it.
  class Sink {
  public:
    virtual bool write(const void *data, size_t size) = 0;
  };

  // The BMP header's size, and the BMP's.
  static constexpr size_t BMP_HEADER_SIZE = 66;
  static size_t bmp_size(uint16_t width, uint16_t height) { return BMP_HEADER_SIZE + (size_t)width * height * 2; }

  /**
   * Write `pixels` as a BMP file. False if the sink failed. `width` must be even (rows are not
   * padded).
   */
  static bool write_bmp(const uint16_t *pixels, uint16_t width, uint16_t height, Sink &sink);

  /**
   * Write `pixels` as a PNG file. False if the sink failed, or there was no memory for the
   * compressor.
   */
  static bool write_png(const uint16_t *pixels, uint16_t width, uint16_t height, Sink &sink);
};
