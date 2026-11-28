#pragma once
// The parts of the voice recorder that are plain computation: the token's form, the Range header, the WAV
// header, which slot a new recording takes, and the level meter's scale. No ESP-IDF, no FreeRTOS, so
// test/host/voice_recorder compiles and tests them on the build machine. VoiceRecorder.cpp uses them as they
// are. Header-only and not behind BOARD_CUSTOM (nothing here includes the board's code).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <strings.h>

namespace VoiceRecorderPure {

inline constexpr size_t TOKEN_CHARS = 32;
inline constexpr size_t WAV_HEADER_BYTES = 44;

// --- Token ---------------------------------------------------------------------------------------------------

// 16 bytes as 32 lowercase hex digits, NUL-terminated: `out` takes 33 bytes.
inline void hex_encode(const uint8_t bytes[TOKEN_CHARS / 2], char out[TOKEN_CHARS + 1]) {
  static const char DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < TOKEN_CHARS / 2; i++) {
    out[i * 2] = DIGITS[bytes[i] >> 4];
    out[i * 2 + 1] = DIGITS[bytes[i] & 0x0f];
  }
  out[TOKEN_CHARS] = '\0';
}

/**
 * The token in a request URI: "/rec/" + exactly 32 lowercase hex digits + ".wav", optionally followed by a
 * query string, which is ignored. Anything else is not a token (the caller answers 404).
 * @param token: 33 bytes, filled on true.
 */
inline bool parse_token_uri(const char *uri, char token[TOKEN_CHARS + 1]) {
  static const char PREFIX[] = "/rec/";
  static const char SUFFIX[] = ".wav";
  if (uri == nullptr || strncmp(uri, PREFIX, sizeof(PREFIX) - 1) != 0) {
    return false;
  }
  const char *p = uri + sizeof(PREFIX) - 1;
  for (size_t i = 0; i < TOKEN_CHARS; i++) {
    const char c = p[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false; // Also catches a string that ends early (NUL)
    }
  }
  p += TOKEN_CHARS;
  if (strncmp(p, SUFFIX, sizeof(SUFFIX) - 1) != 0) {
    return false;
  }
  p += sizeof(SUFFIX) - 1;
  if (*p != '\0' && *p != '?') {
    return false;
  }
  memcpy(token, uri + sizeof(PREFIX) - 1, TOKEN_CHARS);
  token[TOKEN_CHARS] = '\0';
  return true;
}

// Equal in time that does not depend on where they differ. Both are TOKEN_CHARS long (a slot with no token
// holds NULs, which no parsed token matches).
inline bool token_equal(const char *a, const char *b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < TOKEN_CHARS; i++) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}

// --- Range ---------------------------------------------------------------------------------------------------

struct Range {
  enum Kind : uint8_t {
    NONE,          // No usable Range header: send the whole file, 200.
    PARTIAL,       // first..last inclusive: 206.
    UNSATISFIABLE, // Starts at or past the end: 416.
  } kind = NONE;
  size_t first = 0;
  size_t last = 0; // Inclusive
};

namespace detail {
// Digits only, no sign or spaces; false if there are none or it would overflow size_t.
inline bool parse_number(const char *&p, size_t &value) {
  if (*p < '0' || *p > '9') {
    return false;
  }
  size_t v = 0;
  while (*p >= '0' && *p <= '9') {
    const size_t digit = (size_t)(*p - '0');
    if (v > (SIZE_MAX - digit) / 10) {
      return false;
    }
    v = v * 10 + digit;
    p++;
  }
  value = v;
  return true;
}
} // namespace detail

/**
 * A Range request header's value ("bytes=0-99", "bytes=100-", "bytes=-500") against a file of `total` bytes.
 * One range only: several ranges, another unit and bad syntax are ignored (RFC 9110 lets a server send the
 * whole file), as is a first byte after the last. A last byte past the end is cut to the end; "-n" longer
 * than the file is the whole file; "-0" and a first byte at or past the end are UNSATISFIABLE.
 */
inline Range parse_range(const char *header, size_t total) {
  Range none;
  if (header == nullptr || total == 0) {
    return none;
  }
  const char *p = header;
  while (*p == ' ') {
    p++;
  }
  if (strncasecmp(p, "bytes", 5) != 0) {
    return none;
  }
  p += 5;
  while (*p == ' ') {
    p++;
  }
  if (*p != '=') {
    return none;
  }
  p++;
  while (*p == ' ') {
    p++;
  }
  Range range;
  if (*p == '-') { // Suffix: the last n bytes
    p++;
    size_t n = 0;
    if (!detail::parse_number(p, n)) {
      return none;
    }
    while (*p == ' ') {
      p++;
    }
    if (*p != '\0') {
      return none;
    }
    if (n == 0) {
      range.kind = Range::UNSATISFIABLE;
      return range;
    }
    range.kind = Range::PARTIAL;
    range.first = n >= total ? 0 : total - n;
    range.last = total - 1;
    return range;
  }
  size_t first = 0;
  if (!detail::parse_number(p, first) || *p != '-') {
    return none;
  }
  p++;
  size_t last = total - 1;
  bool has_last = false;
  if (*p >= '0' && *p <= '9') {
    if (!detail::parse_number(p, last)) {
      return none;
    }
    has_last = true;
  }
  while (*p == ' ') {
    p++;
  }
  if (*p != '\0') { // A comma (more ranges) or junk
    return none;
  }
  if (has_last && last < first) {
    return none;
  }
  if (first >= total) {
    range.kind = Range::UNSATISFIABLE;
    return range;
  }
  range.kind = Range::PARTIAL;
  range.first = first;
  range.last = last >= total ? total - 1 : last;
  return range;
}

// --- WAV -----------------------------------------------------------------------------------------------------

// The 44-byte header of 16-bit mono PCM with `data_bytes` of samples after it.
inline void write_wav_header(uint8_t out[WAV_HEADER_BYTES], uint32_t data_bytes, uint32_t sample_rate) {
  auto put32 = [&](size_t at, uint32_t v) {
    out[at] = (uint8_t)v;
    out[at + 1] = (uint8_t)(v >> 8);
    out[at + 2] = (uint8_t)(v >> 16);
    out[at + 3] = (uint8_t)(v >> 24);
  };
  auto put16 = [&](size_t at, uint16_t v) {
    out[at] = (uint8_t)v;
    out[at + 1] = (uint8_t)(v >> 8);
  };
  memcpy(out, "RIFF", 4);
  put32(4, 36 + data_bytes);
  memcpy(out + 8, "WAVEfmt ", 8);
  put32(16, 16);          // fmt chunk size
  put16(20, 1);           // PCM
  put16(22, 1);           // mono
  put32(24, sample_rate);
  put32(28, sample_rate * 2); // byte rate
  put16(32, 2);           // block align
  put16(34, 16);          // bits per sample
  memcpy(out + 36, "data", 4);
  put32(40, data_bytes);
}

// --- Slots ---------------------------------------------------------------------------------------------------

enum class SlotState : uint8_t { FREE, RECORDING, READY };

// What the choice of slot, and the lookup of a token, need to know about one slot.
struct SlotMeta {
  SlotState state = SlotState::FREE;
  char token[TOKEN_CHARS + 1] = {}; // All NULs unless READY
  int64_t expires_us = 0;           // READY only
  uint32_t sequence = 0;            // READY only: higher is newer
  uint32_t readers = 0;             // serve() calls sending from it
};

/**
 * The slot a new recording goes into, or -1 if every candidate has a reader. Never one with a reader (its
 * bytes are being sent), never one RECORDING; of the rest, a FREE one, then a READY one that has expired,
 * then the oldest READY. Ties go to the lower index.
 */
inline int choose_slot(const SlotMeta *slots, size_t count, int64_t now_us) {
  int best = -1, best_rank = 3;
  uint32_t best_sequence = 0;
  for (size_t i = 0; i < count; i++) {
    const SlotMeta &s = slots[i];
    if (s.readers != 0 || s.state == SlotState::RECORDING) {
      continue;
    }
    int rank = s.state == SlotState::FREE ? 0 : (now_us >= s.expires_us ? 1 : 2);
    if (best < 0 || rank < best_rank || (rank == best_rank && rank != 0 && s.sequence < best_sequence)) {
      best = (int)i;
      best_rank = rank;
      best_sequence = s.sequence;
    }
  }
  return best;
}

enum class Lookup : uint8_t { UNKNOWN, EXPIRED, FOUND };

/**
 * Which slot holds `token`, and whether it still serves: FOUND (READY and before expires_us, `index` set),
 * EXPIRED (READY, past expires_us: 410) or UNKNOWN (no slot has it: 404). Compares every slot, in constant time.
 */
inline Lookup find_token(const SlotMeta *slots, size_t count, const char *token, int64_t now_us, int *index) {
  int match = -1;
  for (size_t i = 0; i < count; i++) {
    if (token_equal(slots[i].token, token) && slots[i].state == SlotState::READY) {
      match = (int)i;
    }
  }
  if (match < 0) {
    return Lookup::UNKNOWN;
  }
  if (now_us >= slots[match].expires_us) {
    return Lookup::EXPIRED;
  }
  *index = match;
  return Lookup::FOUND;
}

// --- Level ---------------------------------------------------------------------------------------------------

// A peak sample as 0-100: 0 at -60 dBFS and below, 100 at 0 dBFS, linear in dB.
inline uint8_t level_from_peak(int peak) {
  if (peak < 0) {
    peak = -peak;
  }
  if (peak <= 33) { // 32.8 is -60 dBFS
    return 0;
  }
  if (peak >= 32767) {
    return 100;
  }
  const float db = 20.0f * log10f((float)peak / 32768.0f);
  const int level = (int)lroundf((db + 60.0f) * 100.0f / 60.0f);
  return (uint8_t)(level < 0 ? 0 : level > 100 ? 100 : level);
}

} // namespace VoiceRecorderPure
