// The voice recorder's pure parts (lib/VoiceRecorder/VoiceRecorderPure.hpp). Plain C++:
// test/host/voice_recorder/build.sh compiles and runs it.

#include "VoiceRecorderPure.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace VoiceRecorderPure;

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static const char *T = "3f9c0a1e7b2d4c6e8f00112233445566";

static bool parses(const std::string &uri) {
  char token[TOKEN_CHARS + 1];
  return parse_token_uri(uri.c_str(), token);
}

static void token_parsing() {
  char token[TOKEN_CHARS + 1];
  CHECK(parse_token_uri("/rec/3f9c0a1e7b2d4c6e8f00112233445566.wav", token));
  CHECK(std::string(token) == T);
  CHECK(parse_token_uri("/rec/3f9c0a1e7b2d4c6e8f00112233445566.wav?x=1", token));
  CHECK(std::string(token) == T);
  CHECK(!parses("/rec/3f9c0a1e7b2d4c6e8f0011223344556.wav"));   // 31 chars
  CHECK(!parses("/rec/3f9c0a1e7b2d4c6e8f001122334455667.wav")); // 33 chars
  CHECK(!parses("/rec/3F9C0A1E7B2D4C6E8F00112233445566.wav"));  // Uppercase
  CHECK(!parses("/rec/3f9c0a1e7b2d4c6e8f0011223344556g.wav"));  // Not hex
  CHECK(!parses("/rec/3f9c0a1e7b2d4c6e8f00112233445566.mp3"));
  CHECK(!parses("/rec/3f9c0a1e7b2d4c6e8f00112233445566.wavx"));
  CHECK(!parses("/rec/3f9c0a1e7b2d4c6e8f00112233445566"));
  CHECK(!parses("/rec/3f9c0a1e7b2d4c6e8f00112233445566.wav/"));
  CHECK(!parses("/rec/../3f9c0a1e7b2d4c6e8f00112233445566.wav"));
  CHECK(!parses("/rec/test.wav"));
  CHECK(!parses("/rec/"));
  CHECK(!parses(""));
  CHECK(!parses("/screenshot"));
  CHECK(!parse_token_uri(nullptr, token));

  uint8_t bytes[16] = {0x00, 0x01, 0x0a, 0xff, 0xab, 0xcd, 0xef, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90};
  hex_encode(bytes, token);
  CHECK(std::string(token) == "00010affabcdef102030405060708090");
  CHECK(parses(std::string("/rec/") + token + ".wav"));

  char a[TOKEN_CHARS + 1], b[TOKEN_CHARS + 1];
  memcpy(a, T, sizeof(a));
  memcpy(b, T, sizeof(b));
  CHECK(token_equal(a, b));
  b[31] = '7';
  CHECK(!token_equal(a, b));
  b[31] = a[31];
  b[0] = 'e';
  CHECK(!token_equal(a, b));
  char empty[TOKEN_CHARS + 1] = {};
  CHECK(!token_equal(empty, a));
}

static void expect_range(const char *header, size_t total, Range::Kind kind, size_t first = 0, size_t last = 0) {
  Range r = parse_range(header, total);
  if (r.kind != kind || (kind == Range::PARTIAL && (r.first != first || r.last != last))) {
    std::printf("range \"%s\" of %zu: got kind %d %zu-%zu, expected kind %d %zu-%zu\n", header ? header : "(null)", total, (int)r.kind, r.first,
                r.last, (int)kind, first, last);
    failures++;
  }
}

static void ranges() {
  const size_t N = 1000;
  expect_range(nullptr, N, Range::NONE);
  expect_range("", N, Range::NONE);
  expect_range("bytes=0-99", N, Range::PARTIAL, 0, 99);
  expect_range("bytes=100-", N, Range::PARTIAL, 100, 999);
  expect_range("bytes=0-", N, Range::PARTIAL, 0, 999);
  expect_range("bytes=-100", N, Range::PARTIAL, 900, 999);
  expect_range("bytes=-1", N, Range::PARTIAL, 999, 999);
  expect_range("bytes=-5000", N, Range::PARTIAL, 0, 999);
  expect_range("bytes=-0", N, Range::UNSATISFIABLE);
  expect_range("bytes=500-5000", N, Range::PARTIAL, 500, 999);
  expect_range("bytes=999-999", N, Range::PARTIAL, 999, 999);
  expect_range("bytes=1000-", N, Range::UNSATISFIABLE);
  expect_range("bytes=1000-1005", N, Range::UNSATISFIABLE);
  expect_range("bytes=5000-", N, Range::UNSATISFIABLE);
  expect_range("bytes=50-10", N, Range::NONE);   // Backwards: invalid, ignored
  expect_range("bytes=0-1,5-9", N, Range::NONE); // Several
  expect_range("bytes=abc", N, Range::NONE);
  expect_range("bytes=", N, Range::NONE);
  expect_range("bytes=-", N, Range::NONE);
  expect_range("bytes=1-2-3", N, Range::NONE);
  expect_range("items=0-5", N, Range::NONE);
  expect_range("0-5", N, Range::NONE);
  expect_range("BYTES=0-9", N, Range::PARTIAL, 0, 9);
  expect_range("bytes = 0-9", N, Range::PARTIAL, 0, 9);
  expect_range("bytes=99999999999999999999999-", N, Range::NONE); // Overflow
  expect_range("bytes=0-99", 0, Range::NONE);
}

static void wav_header() {
  uint8_t h[WAV_HEADER_BYTES];
  write_wav_header(h, 480000, 16000);
  CHECK(memcmp(h, "RIFF", 4) == 0);
  CHECK(h[4] == ((480036) & 0xff) && h[5] == ((480036 >> 8) & 0xff) && h[6] == ((480036 >> 16) & 0xff) && h[7] == 0);
  CHECK(memcmp(h + 8, "WAVEfmt ", 8) == 0);
  CHECK(h[16] == 16 && h[17] == 0);
  CHECK(h[20] == 1 && h[22] == 1);                                   // PCM, mono
  CHECK(h[24] == 0x80 && h[25] == 0x3e && h[26] == 0 && h[27] == 0); // 16000
  CHECK(h[28] == 0x00 && h[29] == 0x7d && h[30] == 0);               // 32000
  CHECK(h[32] == 2 && h[34] == 16);
  CHECK(memcmp(h + 36, "data", 4) == 0);
  CHECK(h[40] == (480000 & 0xff) && h[41] == ((480000 >> 8) & 0xff) && h[42] == ((480000 >> 16) & 0xff) && h[43] == 0);
}

static SlotMeta slot(SlotState state, uint32_t sequence = 0, int64_t expires_us = 0, uint32_t readers = 0) {
  SlotMeta s;
  s.state = state;
  s.sequence = sequence;
  s.expires_us = expires_us;
  s.readers = readers;
  return s;
}

static void slot_choice() {
  const int64_t now = 1000000;
  SlotMeta s[2];
  s[0] = slot(SlotState::FREE);
  s[1] = slot(SlotState::FREE);
  CHECK(choose_slot(s, 2, now) == 0);
  // Prefers FREE over a live READY
  s[0] = slot(SlotState::READY, 5, now + 1000);
  CHECK(choose_slot(s, 2, now) == 1);
  // Prefers an expired READY over a live one
  s[1] = slot(SlotState::READY, 6, now - 1);
  CHECK(choose_slot(s, 2, now) == 1);
  s[0] = slot(SlotState::READY, 5, now - 1);
  s[1] = slot(SlotState::READY, 6, now + 100);
  CHECK(choose_slot(s, 2, now) == 0);
  // Two live READY: the oldest
  s[0] = slot(SlotState::READY, 9, now + 50);
  s[1] = slot(SlotState::READY, 8, now + 40);
  CHECK(choose_slot(s, 2, now) == 1);
  // Never one with readers
  s[1] = slot(SlotState::READY, 8, now + 40, 1);
  CHECK(choose_slot(s, 2, now) == 0);
  s[0] = slot(SlotState::READY, 9, now + 50, 2);
  CHECK(choose_slot(s, 2, now) == -1);
  // Never one RECORDING
  s[0] = slot(SlotState::RECORDING);
  s[1] = slot(SlotState::READY, 3, now + 10);
  CHECK(choose_slot(s, 2, now) == 1);
  s[1] = slot(SlotState::READY, 3, now + 10, 1);
  CHECK(choose_slot(s, 2, now) == -1);
}

static void lookup() {
  const int64_t now = 5000000;
  SlotMeta s[2];
  memcpy(s[0].token, T, TOKEN_CHARS);
  s[0].state = SlotState::READY;
  s[0].expires_us = now + 1;
  const char *other = "ffffffffffffffffffffffffffffffff";
  memcpy(s[1].token, other, TOKEN_CHARS);
  s[1].state = SlotState::READY;
  s[1].expires_us = now - 1;
  int index = -1;
  CHECK(find_token(s, 2, T, now, &index) == Lookup::FOUND && index == 0);
  CHECK(find_token(s, 2, T, now + 1, &index) == Lookup::EXPIRED); // At expires_us: expired
  CHECK(find_token(s, 2, other, now, &index) == Lookup::EXPIRED);
  CHECK(find_token(s, 2, "00000000000000000000000000000000", now, &index) == Lookup::UNKNOWN);
  // A slot being recorded again no longer answers to its old token
  s[0].state = SlotState::RECORDING;
  memset(s[0].token, 0, sizeof(s[0].token));
  CHECK(find_token(s, 2, T, now, &index) == Lookup::UNKNOWN);
}

// A mutex that counts, to check the lease's locking as well as its count.
struct CountingMutex {
  int locks = 0, unlocks = 0;
  void lock() { locks++; }
  void unlock() { unlocks++; }
};

static void readers() {
  CountingMutex m;
  SlotMeta s[2];
  s[0].state = SlotState::READY;
  s[0].expires_us = 100;
  s[1].state = SlotState::READY;
  s[1].expires_us = 100;
  s[1].sequence = 1;
  {
    ReaderLease<CountingMutex> lease(m);
    lease.acquire(s[0]);
    CHECK(s[0].readers == 1);
    CHECK(m.locks == 0);                // acquire() runs under the caller's lock
    CHECK(choose_slot(s, 2, 200) == 1); // The slot being read is not picked
    lease.acquire(s[1]);                // A second acquire on one lease is ignored
    CHECK(s[1].readers == 0 && s[0].readers == 1);
  }
  CHECK(s[0].readers == 0); // The destructor let go, under the lock
  CHECK(m.locks == 1 && m.unlocks == 1);
  CHECK(choose_slot(s, 2, 200) == 0);

  // Leases opened and abandoned in scopes that end in different ways
  for (int i = 0; i < 100; i++) {
    ReaderLease<CountingMutex> a(m), b(m);
    a.acquire(s[0]);
    b.acquire(s[0]);
    CHECK(s[0].readers == 2);
    if (i % 3 == 0) {
      a.release();
      CHECK(s[0].readers == 1);
      a.release(); // Idempotent
      CHECK(s[0].readers == 1);
    }
    if (i % 2 == 0) {
      continue;
    }
  }
  CHECK(s[0].readers == 0);

  // Never below zero, even if the count was reset under the lease
  {
    ReaderLease<CountingMutex> lease(m);
    lease.acquire(s[1]);
    s[1].readers = 0;
  }
  CHECK(s[1].readers == 0);

  // What two leaked readers look like: no slot to record into (start() answers ESP_ERR_NOT_FINISHED)
  s[0].readers = 1;
  s[1].readers = 1;
  CHECK(choose_slot(s, 2, 200) == -1);
}

static void auto_stops() {
  AutoStopInput in;
  in.idle = false;
  in.now_us = 1000;
  CHECK(auto_stop_reason(in) == StopReason::NONE);
  in.idle = true;
  in.streaming = false;
  CHECK(auto_stop_reason(in) == StopReason::NONE); // Idle: never
  in.idle = false;
  CHECK(auto_stop_reason(in) == StopReason::CAPTURE_ENDED);
  in.full = true;
  CHECK(auto_stop_reason(in) == StopReason::CAPTURE_ENDED); // Ranks first
  in.streaming = true;
  CHECK(auto_stop_reason(in) == StopReason::LIMIT);
  in.auto_send_at_limit = false;
  CHECK(auto_stop_reason(in) == StopReason::NONE); // Stays FULL for the caller
  in.stop_deadline_us = 1000;
  CHECK(auto_stop_reason(in) == StopReason::TIMED); // At the deadline, not before
  in.stop_deadline_us = 1001;
  CHECK(auto_stop_reason(in) == StopReason::NONE);
  in.auto_send_at_limit = true;
  CHECK(auto_stop_reason(in) == StopReason::LIMIT); // The limit before the time
  in.full = false;
  in.stop_deadline_us = 0; // None
  in.hold_until_us = 1000;
  CHECK(auto_stop_reason(in) == StopReason::NONE); // Not a hold_lease recording
  in.hold_lease = true;
  CHECK(auto_stop_reason(in) == StopReason::HOLD_ENDED);
  in.hold_until_us = 1001;
  CHECK(auto_stop_reason(in) == StopReason::NONE); // hold() pushed it on
  in.stop_deadline_us = 500;
  CHECK(auto_stop_reason(in) == StopReason::TIMED); // The time before the lease
  CHECK(std::string(stop_reason_text(StopReason::LIMIT)) == "the limit");
  CHECK(stop_reason_text(StopReason::NONE) == nullptr);
}

static void announce_schedule() {
  // Published: sent whatever came before
  CHECK(announce_step(AnnounceAttempt::PUBLISHED, 0, 0).kind == AnnounceStep::SENT);
  CHECK(announce_step(AnnounceAttempt::PUBLISHED, 9, 14000).kind == AnnounceStep::SENT);
  // The URL is gone: nothing to retry
  CHECK(announce_step(AnnounceAttempt::NOT_SERVABLE, 0, 0).kind == AnnounceStep::FAIL);
  CHECK(announce_step(AnnounceAttempt::NOT_SERVABLE, 3, 500).kind == AnnounceStep::FAIL);
  // No IP at release: fails at once; after a retry it is waited out
  CHECK(announce_step(AnnounceAttempt::NO_IP, 0, 0).kind == AnnounceStep::FAIL);
  CHECK(announce_step(AnnounceAttempt::NO_IP, 1, 300).kind == AnnounceStep::RETRY);

  // MQTT down all along: the whole schedule, as the worker would run it
  uint32_t elapsed = 0, failed = 0;
  std::vector<uint32_t> delays;
  for (;;) {
    AnnounceStep step = announce_step(AnnounceAttempt::PUBLISH_FAILED, failed, elapsed);
    if (step.kind != AnnounceStep::RETRY) {
      CHECK(step.kind == AnnounceStep::FAIL);
      break;
    }
    delays.push_back(step.delay_ms);
    elapsed += step.delay_ms;
    failed++;
    if (failed >= 100) {
      CHECK(false);
      break;
    }
  }
  CHECK(elapsed == ANNOUNCE_BUDGET_MS); // Gives up exactly at the budget, never past it
  CHECK(delays.size() >= 8 && delays[0] == 250 && delays[1] == 500 && delays[2] == 1000 && delays[3] == 2000 && delays[4] == 2000);
  for (uint32_t d : delays) {
    CHECK(d > 0 && d <= ANNOUNCE_MAX_DELAY_MS);
  }
  // The last wait is cut to what is left
  CHECK(announce_step(AnnounceAttempt::PUBLISH_FAILED, 5, ANNOUNCE_BUDGET_MS - 700).delay_ms == 700);
  CHECK(announce_step(AnnounceAttempt::PUBLISH_FAILED, 5, ANNOUNCE_BUDGET_MS).kind == AnnounceStep::FAIL);
  CHECK(announce_step(AnnounceAttempt::PUBLISH_FAILED, 5, ANNOUNCE_BUDGET_MS + 5000).kind == AnnounceStep::FAIL);
  // A huge attempt count does not overflow the backoff
  CHECK(announce_step(AnnounceAttempt::PUBLISH_FAILED, 4000000000u, 0).delay_ms == ANNOUNCE_MAX_DELAY_MS);
  // The budget leaves most of the URL's 60 s for Home Assistant to fetch it
  CHECK(ANNOUNCE_BUDGET_MS <= 30 * 1000);
}

static void level() {
  CHECK(level_from_peak(0) == 0);
  CHECK(level_from_peak(33) == 0);
  CHECK(level_from_peak(32767) == 100);
  CHECK(level_from_peak(-32768) == 100);
  CHECK(level_from_peak(1000) == 49); // -30.3 dBFS
  CHECK(level_from_peak(100) < level_from_peak(1000));
  CHECK(level_from_peak(16384) == 90);
}

int main() {
  token_parsing();
  ranges();
  wav_header();
  slot_choice();
  lookup();
  readers();
  auto_stops();
  announce_schedule();
  level();
  if (failures == 0) {
    std::printf("voice_recorder: all passed\n");
    return 0;
  }
  std::printf("voice_recorder: %d failure(s)\n", failures);
  return 1;
}
