#pragma once
// The parts of the voice recorder that are plain computation: the token's form, the Range header, the WAV
// header, which slot a new recording takes, the readers' accounting, the automatic stops, the announcement's
// retry schedule and the level meter's scale. No ESP-IDF, no FreeRTOS, so
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

// --- Readers -------------------------------------------------------------------------------------------------

/**
 * One serve() call's claim on a slot: counts it in SlotMeta::readers while the claim lives, and always lets
 * it go, whatever way the request ends (return, early return, an error from the socket). A reader that is
 * never let go would keep choose_slot() away from the slot for good, and after two such leaks start() would
 * refuse every recording until a reboot.
 *
 * `Mutex` has lock() and unlock(); it is the one that guards the slots' metadata. acquire() is called WITH
 * that mutex held, in the same critical section as the find_token() that picked the slot, so that the slot
 * cannot be chosen for a new recording between the lookup and the count. The destructor takes the mutex
 * itself, so it must run without it held (serve() holds it nowhere else).
 */
template <class Mutex>
class ReaderLease {
public:
  explicit ReaderLease(Mutex &mutex) : _mutex(mutex) {}
  ~ReaderLease() { release(); }
  ReaderLease(const ReaderLease &) = delete;
  ReaderLease &operator=(const ReaderLease &) = delete;

  void acquire(SlotMeta &meta) { // Mutex held by the caller
    if (_meta == nullptr) {
      _meta = &meta;
      meta.readers++;
    }
  }
  void release() { // Idempotent; the count never goes below zero
    if (_meta != nullptr) {
      _mutex.lock();
      if (_meta->readers > 0) {
        _meta->readers--;
      }
      _mutex.unlock();
      _meta = nullptr;
    }
  }

private:
  Mutex &_mutex;
  SlotMeta *_meta = nullptr;
};

// --- Automatic stops -----------------------------------------------------------------------------------------

enum class StopReason : uint8_t { NONE, CAPTURE_ENDED, LIMIT, TIMED, HOLD_ENDED };

inline const char *stop_reason_text(StopReason reason) {
  switch (reason) {
  case StopReason::CAPTURE_ENDED: return "the capture ended";
  case StopReason::LIMIT: return "the limit";
  case StopReason::TIMED: return "its time";
  case StopReason::HOLD_ENDED: return "the hold ended";
  case StopReason::NONE: break;
  }
  return nullptr;
}

// What the worker knows about the recording under way when it asks whether a stop is due.
struct AutoStopInput {
  bool idle = true;                // Nothing is being recorded: never a stop
  bool streaming = true;           // VoiceCapture is still delivering audio
  bool full = false;               // MAX_FRAMES reached
  bool auto_send_at_limit = true;  // Options::auto_send_at_limit
  int64_t now_us = 0;
  int64_t stop_deadline_us = 0;    // Options::stop_after_ms as a time; 0: none
  bool hold_lease = false;         // Options::hold_lease
  int64_t hold_until_us = 0;       // When the lease runs out unless hold() pushes it on
};

/**
 * Whether the recording under way is to be stopped now, and why: in this order, the capture having ended on
 * its own (an I2S failure: the stop discards), the limit (only with auto_send_at_limit; otherwise the
 * recording stays FULL for the caller), the timed stop, the hold lease having lapsed.
 */
inline StopReason auto_stop_reason(const AutoStopInput &in) {
  if (in.idle) {
    return StopReason::NONE;
  }
  if (!in.streaming) {
    return StopReason::CAPTURE_ENDED;
  }
  if (in.full && in.auto_send_at_limit) {
    return StopReason::LIMIT;
  }
  if (in.stop_deadline_us != 0 && in.now_us >= in.stop_deadline_us) {
    return StopReason::TIMED;
  }
  if (in.hold_lease && in.now_us >= in.hold_until_us) {
    return StopReason::HOLD_ENDED;
  }
  return StopReason::NONE;
}

// --- Announcement retries ------------------------------------------------------------------------------------

// The URL lives 60 s from stop(). While MQTT (or the Wi-Fi under it) is down, the announcement is tried again
// for this long, then given up and reported as failed. The UI shows nothing until it is reported, so this is
// also the longest a release waits for its toast.
inline constexpr uint32_t ANNOUNCE_BUDGET_MS = 15000;
inline constexpr uint32_t ANNOUNCE_FIRST_DELAY_MS = 250;
inline constexpr uint32_t ANNOUNCE_MAX_DELAY_MS = 2000;

enum class AnnounceAttempt : uint8_t {
  PUBLISHED,     // Handed to the MQTT client
  NOT_SERVABLE,  // The recording's URL is gone (overwritten or expired): nothing worth announcing
  NO_IP,         // The panel has no IP address, so there is no URL to give out
  PUBLISH_FAILED // MQTT is down, or the client refused the message
};

struct AnnounceStep {
  enum Kind : uint8_t { SENT, FAIL, RETRY } kind = FAIL;
  uint32_t delay_ms = 0; // RETRY: wait this long, then try again
};

/**
 * What to do after an attempt. `failed_before` is how many attempts have already failed, not counting this one;
 * `elapsed_ms` the time since the first attempt. Retries back off 250, 500, 1000, 2000, 2000 ... ms, the last
 * one cut to what is left of ANNOUNCE_BUDGET_MS. No IP address at the very first attempt fails at once (start()
 * refuses without one, so the Wi-Fi went away during the recording, and a release into nothing should not
 * wait); once the panel has retried, no IP is waited out like MQTT being down.
 */
inline AnnounceStep announce_step(AnnounceAttempt attempt, uint32_t failed_before, uint32_t elapsed_ms) {
  AnnounceStep step;
  switch (attempt) {
  case AnnounceAttempt::PUBLISHED:
    step.kind = AnnounceStep::SENT;
    return step;
  case AnnounceAttempt::NOT_SERVABLE:
    return step; // FAIL
  case AnnounceAttempt::NO_IP:
    if (failed_before == 0) {
      return step; // FAIL
    }
    break;
  case AnnounceAttempt::PUBLISH_FAILED:
    break;
  }
  if (elapsed_ms >= ANNOUNCE_BUDGET_MS) {
    return step; // FAIL
  }
  uint32_t delay = ANNOUNCE_FIRST_DELAY_MS;
  for (uint32_t i = 0; i < failed_before && delay < ANNOUNCE_MAX_DELAY_MS; i++) {
    delay *= 2;
  }
  if (delay > ANNOUNCE_MAX_DELAY_MS) {
    delay = ANNOUNCE_MAX_DELAY_MS;
  }
  const uint32_t left = ANNOUNCE_BUDGET_MS - elapsed_ms;
  step.kind = AnnounceStep::RETRY;
  step.delay_ms = delay < left ? delay : left;
  return step;
}

// --- Downloads -----------------------------------------------------------------------------------------------

// A download of a recording (480 KB at most) is cut off after this long in all, however slowly the client takes
// it: the web server has one task, and a client that drains a byte per timeout would otherwise hold it, and the
// slot, for minutes. 250 KB/s measured; this allows a floor of 16 KB/s.
inline constexpr uint32_t DOWNLOAD_BUDGET_MS = 30000;

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
