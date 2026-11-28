// The voice recorder's pure parts (lib/VoiceRecorder/VoiceRecorderPure.hpp). Plain C++:
// test/host/voice_recorder/build.sh compiles and runs it.

#include "VoiceRecorderPure.hpp"

#include <cstdio>
#include <string>

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
  level();
  if (failures == 0) {
    std::printf("voice_recorder: all passed\n");
    return 0;
  }
  std::printf("voice_recorder: %d failure(s)\n", failures);
  return 1;
}
