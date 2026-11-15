#include <LvglScreenshot.hpp>
#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <miniz.h>

static const char *TAG = "LvglScreenshot";

static void put_le16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

static void put_le32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    p[i] = (v >> (8 * i)) & 0xFF;
  }
}

static void put_be32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    p[i] = (v >> (24 - 8 * i)) & 0xFF;
  }
}

bool LvglScreenshot::write_bmp(const uint16_t *pixels, uint16_t width, uint16_t height, Sink &sink) {
  uint8_t header[BMP_HEADER_SIZE] = {'B', 'M'};
  uint32_t pixel_bytes = (uint32_t)width * height * 2;
  // BITMAPFILEHEADER
  put_le32(header + 2, BMP_HEADER_SIZE + pixel_bytes);
  put_le32(header + 10, BMP_HEADER_SIZE);
  // BITMAPINFOHEADER: a negative height is top-down, as the pixels are.
  put_le32(header + 14, 40);
  put_le32(header + 18, width);
  put_le32(header + 22, (uint32_t)(-(int32_t)height));
  put_le16(header + 26, 1);  // planes
  put_le16(header + 28, 16); // bits per pixel
  put_le32(header + 30, 3);  // BI_BITFIELDS
  put_le32(header + 34, pixel_bytes);
  put_le32(header + 38, 2835); // 72 dpi
  put_le32(header + 42, 2835);
  // The colour masks: RGB565.
  put_le32(header + 54, 0xF800);
  put_le32(header + 58, 0x07E0);
  put_le32(header + 62, 0x001F);
  // Rows of 640 or 960 bytes need no padding to 4.
  return sink.write(header, sizeof(header)) && sink.write(pixels, pixel_bytes);
}

namespace {
struct PngOutput {
  LvglScreenshot::Sink *sink;
  bool ok;
};

bool png_chunk(LvglScreenshot::Sink &sink, const char *type, const void *data, size_t size) {
  uint8_t head[8];
  put_be32(head, size);
  memcpy(head + 4, type, 4);
  uint32_t crc = esp_rom_crc32_le(0, head + 4, 4);
  if (size > 0) {
    crc = esp_rom_crc32_le(crc, static_cast<const uint8_t *>(data), size);
  }
  uint8_t tail[4];
  put_be32(tail, crc);
  return sink.write(head, sizeof(head)) && (size == 0 || sink.write(data, size)) && sink.write(tail, sizeof(tail));
}

// tdefl's output, one block at a time (up to TDEFL_OUT_BUF_SIZE): an IDAT chunk each.
mz_bool png_put_idat(const void *data, int size, void *user) {
  PngOutput *out = static_cast<PngOutput *>(user);
  out->ok = out->ok && png_chunk(*out->sink, "IDAT", data, size);
  return out->ok;
}
} // namespace

bool LvglScreenshot::write_png(const uint16_t *pixels, uint16_t width, uint16_t height, Sink &sink) {
  // State in PSRAM (the ROM code may read and write data there; only its stack must be internal).
  tdefl_compressor *compressor = static_cast<tdefl_compressor *>(heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM));
  // One row: the filter byte (0, none), then RGB888. In internal RAM, as it is read twice per pixel.
  size_t row_bytes = 1 + (size_t)width * 3;
  uint8_t *row = static_cast<uint8_t *>(heap_caps_malloc(row_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (compressor == nullptr || row == nullptr) {
    ESP_LOGW(TAG, "No memory for the PNG encoder.");
    heap_caps_free(compressor);
    heap_caps_free(row);
    return false;
  }

  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  uint8_t ihdr[13];
  put_be32(ihdr, width);
  put_be32(ihdr + 4, height);
  ihdr[8] = 8;  // bits per channel
  ihdr[9] = 2;  // RGB
  ihdr[10] = 0; // deflate
  ihdr[11] = 0; // adaptive filtering (each row says which)
  ihdr[12] = 0; // not interlaced
  PngOutput out = {&sink, sink.write(signature, sizeof(signature)) && png_chunk(sink, "IHDR", ihdr, sizeof(ihdr))};

  // 1 probe with greedy parsing: tdefl's fastest. Filtering rows (Sub) made this flat UI larger.
  if (out.ok && tdefl_init(compressor, png_put_idat, &out, 1 | TDEFL_GREEDY_PARSING_FLAG | TDEFL_WRITE_ZLIB_HEADER) != TDEFL_STATUS_OKAY) {
    out.ok = false;
  }
  row[0] = 0;
  for (uint16_t y = 0; out.ok && y < height; y++) {
    const uint16_t *src = pixels + (size_t)y * width;
    uint8_t *dst = row + 1;
    for (uint16_t x = 0; x < width; x++) {
      uint16_t v = src[x];
      uint8_t r = v >> 11, g = (v >> 5) & 0x3F, b = v & 0x1F;
      // Widened by repeating the top bits, so that full scale stays full scale.
      *dst++ = (r << 3) | (r >> 2);
      *dst++ = (g << 2) | (g >> 4);
      *dst++ = (b << 3) | (b >> 2);
    }
    if (tdefl_compress_buffer(compressor, row, row_bytes, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY) {
      out.ok = false;
    }
  }
  if (out.ok && tdefl_compress_buffer(compressor, NULL, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE) {
    out.ok = false;
  }
  heap_caps_free(compressor);
  heap_caps_free(row);
  return out.ok && png_chunk(sink, "IEND", NULL, 0);
}
