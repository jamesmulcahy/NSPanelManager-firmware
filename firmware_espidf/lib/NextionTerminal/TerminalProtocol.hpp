// Ported from nspanel-terminal f0c1394 (components/nspanel_terminal/protocol.h), MIT licence,
// Copyright (c) 2026 NSPanel public contributors. Changed: the namespace and include names only.
// See lib/NextionTerminal/README.md.
#pragma once
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#if defined(__GNUC__) && !defined(__clang__)
// Keep byte loops inlined even when the firmware is built for size (-Os).
#define NSPANEL_PROTOCOL_HOT __attribute__((optimize("O3")))
#else
#define NSPANEL_PROTOCOL_HOT
#endif
namespace nspanel_terminal::protocol {
enum { RECT = 1, DATA, END, PING, EXIT, SYNC, BRIGHTNESS };
enum { READY = 128, BYE, PROGRESS, ERROR, SYNCED, DONE, READY_BRIGHTNESS, BRIGHTNESS_DONE };
constexpr unsigned VERSION = 8, WINDOW = 512, PAYLOAD = 512;
inline unsigned get16(const uint8_t *p) { return unsigned(p[0]) | (unsigned(p[1]) << 8); }
inline uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t(get16(p + 2)) << 16); }
inline void put16(std::vector<uint8_t> &p, unsigned at, unsigned n) {
  p[at] = uint8_t(n);
  p[at + 1] = uint8_t(n >> 8);
}
inline uint16_t crc_step(uint16_t c, uint8_t b) {
  static constexpr auto table = []() {
    std::array<uint16_t, 256> t{};
    for (unsigned i = 0; i < 256; i++) {
      unsigned c = i << 8;
      for (unsigned j = 0; j < 8; j++) c = (c << 1) ^ ((c & 32768) ? 0x1021 : 0);
      t[i] = uint16_t(c);
    }
    return t;
  }();
  return uint16_t((c << 8) ^ table[(c >> 8) ^ b]);
}
NSPANEL_PROTOCOL_HOT inline uint16_t crc(const uint8_t *p, size_t n) {
  uint16_t c = 65535;
  while (n--) c = crc_step(c, *p++);
  return c;
}
inline std::vector<uint8_t> cobs(const std::vector<uint8_t> &raw) {
  std::vector<uint8_t> out;
  out.reserve(raw.size() + raw.size() / 254 + 2);
  out.push_back(0);
  size_t mark = 0;
  unsigned code = 1;
  for (uint8_t b : raw) {
    if (!b) {
      out[mark] = uint8_t(code);
      mark = out.size();
      out.push_back(0);
      code = 1;
    } else {
      out.push_back(b);
      if (++code == 255) {
        out[mark] = 255;
        mark = out.size();
        out.push_back(0);
        code = 1;
      }
    }
  }
  out[mark] = uint8_t(code);
  out.push_back(0);
  return out;
}
inline std::vector<uint8_t> record(unsigned session, unsigned seq, unsigned kind, unsigned x = 0, unsigned y = 0,
                                   unsigned w = 0, unsigned h = 0, const std::vector<uint8_t> &body = {}) {
  if (body.size() > PAYLOAD) return {};
  std::vector<uint8_t> b(20);
  b[0] = 'R';
  b[1] = 'S';
  b[2] = VERSION;
  b[3] = uint8_t(kind);
  put16(b, 4, session);
  put16(b, 6, seq);
  put16(b, 8, body.size());
  put16(b, 10, x);
  put16(b, 12, y);
  put16(b, 14, w);
  put16(b, 16, h);
  put16(b, 18, crc(b.data(), 18));
  b.insert(b.end(), body.begin(), body.end());
  return cobs(b);
}
/* A sequential source is read exactly once per pixel. Two-pixel lookahead
 * survives record boundaries; output buffers are reused by the caller. */
template <class T, class = void> struct HasRepeatScan : std::false_type {};
template <class T>
struct HasRepeatScan<T, std::void_t<decltype(std::declval<T &>().repeat_scan(0u, 0u, uint16_t{}))>>
    : std::true_type {};
template <class T, class = void> struct HasLiteralSpan : std::false_type {};
template <class T>
struct HasLiteralSpan<T, std::void_t<decltype(std::declval<T &>().literal_span(0u))>>
    : std::true_type {};
template <class Pixel>
class Encoder {
  Pixel pixel;
  unsigned total, generated = 0, used = 0, have = 0;
  uint16_t look[2]{};
  void fill() {
    while (have < 2 && generated < total) look[have++] = uint16_t(pixel(generated++));
  }
  uint16_t take() {
    auto v = look[0];
    if (have == 2) look[0] = look[1];
    --have;
    ++used;
    fill();
    return v;
  }

 public:
  Encoder(unsigned count, Pixel source) : pixel(source), total(count) { fill(); }
  unsigned consumed() const { return used; }
  NSPANEL_PROTOCOL_HOT bool next(std::vector<uint8_t> &out) {
    // Keep hot counters local: a source callback may alias encoder members,
    // forcing field reloads/stores per pixel on the ESP32 size optimiser.
    unsigned g = generated, u = used, n = have, limit = total;
    uint16_t a = look[0], b = look[1];
    auto refill = [&]() {
      while (n < 2 && g < limit) {
        auto v = uint16_t(pixel(g++));
        if (n == 0)
          a = v;
        else
          b = v;
        ++n;
      }
    };
    auto pop = [&]() {
      auto v = a;
      a = b;
      --n;
      ++u;
      refill();
      return v;
    };
    // Bound the output once; avoid a vector operation for each encoded byte.
    out.resize(PAYLOAD);
    uint8_t *dest = out.data();
    unsigned size = 0;
    while (u < limit && size + 3 <= PAYLOAD) {
      if (n == 2 && a == b) {
        uint16_t v = a;
        unsigned run = 2;
        u += 2;
        n = 0;
        if constexpr (HasRepeatScan<Pixel>::value) {
          const unsigned extra = pixel.repeat_scan(g, std::min(128u - run, limit - g), v);
          g += extra;
          u += extra;
          run += extra;
        } else {
          while (run < 128 && g < limit) {
            auto candidate = uint16_t(pixel(g++));
            if (candidate != v) {
              a = candidate;
              n = 1;
              break;
            }
            ++run;
            ++u;
          }
        }
        refill();
        dest[size++] = uint8_t(127 + run);
        dest[size++] = uint8_t(v >> 8);
        dest[size++] = uint8_t(v);
      } else {
        unsigned room = std::min(128u, unsigned((PAYLOAD - size - 1) / 2));
        size_t mark = size;
        dest[size++] = 0;
        unsigned run = 0;
        if constexpr (HasLiteralSpan<Pixel>::value) {
          // Fuse two literal pixels without refill's per-pixel bookkeeping.
          // Stop before either adjacent pair becomes a repeat, preserving the
          // original two-pixel lookahead and every DATA/token boundary.
          const uint8_t *src = pixel.literal_span(g);
          while (run + 2 <= room && n == 2 && limit - g >= 2) {
            const uint16_t c = uint16_t(src[0]) | (uint16_t(src[1]) << 8);
            src += 2; ++g;
            dest[size++] = uint8_t(a >> 8);
            dest[size++] = uint8_t(a);
            ++run; ++u;
            if (b == c) { a = b; b = c; break; }
            const uint16_t d = uint16_t(src[0]) | (uint16_t(src[1]) << 8);
            src += 2; ++g;
            dest[size++] = uint8_t(b >> 8);
            dest[size++] = uint8_t(b);
            ++run; ++u;
            a = c; b = d;
            if (a == b) break;
          }
          while (run < room && n && !(n == 2 && a == b)) {
            auto v = pop();
            dest[size++] = uint8_t(v >> 8);
            dest[size++] = uint8_t(v);
            ++run;
          }
        } else {
          do {
            auto v = pop();
            dest[size++] = uint8_t(v >> 8);
            dest[size++] = uint8_t(v);
            ++run;
          } while (run < room && n && !(n == 2 && a == b));
        }
        dest[mark] = uint8_t(run - 1);
      }
    }
    generated = g;
    used = u;
    have = n;
    look[0] = a;
    look[1] = b;
    out.resize(size);
    return size != 0;
  }
};
// Stream the checked header and immutable body into COBS output, without a trailer.
NSPANEL_PROTOCOL_HOT inline void data_record(unsigned session, unsigned seq, const uint8_t *body,
                                            size_t body_size, std::vector<uint8_t> &out) {
  if (body_size > PAYLOAD) { out.clear(); return; }
  uint8_t header[20] = {'R', 'S', VERSION, DATA};
  auto put = [&](unsigned at, unsigned value) {
    header[at] = uint8_t(value);
    header[at + 1] = uint8_t(value >> 8);
  };
  put(4, session); put(6, seq); put(8, body_size);
  put(18, crc(header, 18));
  out.resize(536);
  uint8_t *dest = out.data();
  size_t size = 1, mark = 0;
  unsigned code = 1;
  auto emit = [&](uint8_t byte) {
    if (!byte) {
      dest[mark] = uint8_t(code);
      mark = size++;
      code = 1;
    } else {
      dest[size++] = byte;
      if (++code == 255) {
        dest[mark] = 255;
        mark = size++;
        code = 1;
      }
    }
  };
  for (uint8_t byte : header) emit(byte);
  for (size_t i = 0; i < body_size; ++i) emit(body[i]);
  dest[mark] = uint8_t(code);
  dest[size++] = 0;
  out.resize(size);
}
inline void data_record(unsigned session, unsigned seq, const std::vector<uint8_t> &body,
                        std::vector<uint8_t> &out) {
  data_record(session, seq, body.data(), body.size(), out);
}

constexpr unsigned RESPONSE = 32;
struct Reply {
  unsigned version = 0, status = 0, seq = 0, rectangles = 0, flags = 0, mask = 0, freshness = 0, x = 0, y = 0, highwater = 0;
  uint32_t consumed = 0;
};
inline bool reply(const uint8_t *p, unsigned session, Reply &r) {
  if (p[0] != 'S' || p[1] != 'R' || p[2] != VERSION || get16(p + 4) != session ||
      get16(p + 26) != WINDOW ||
      crc(p, 30) != get16(p + 30))
    return false;
  r.version = p[2];
  r.status = p[3];
  r.seq = get16(p + 6);
  r.consumed = get32(p + 8);
  r.rectangles = get16(p + 12);
  r.flags = get16(p + 14);
  r.mask = p[20];
  r.freshness = p[21];
  r.x = get16(p + 22);
  r.y = get16(p + 24);
  r.highwater = get16(p + 28);
  return r.status >= READY && r.status <= BRIGHTNESS_DONE && r.highwater <= WINDOW;
}
}  // namespace nspanel_terminal::protocol

#undef NSPANEL_PROTOCOL_HOT
