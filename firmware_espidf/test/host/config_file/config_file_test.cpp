// The decisions behind loading and saving config.json (lib/ConfigManager/ConfigFileState).
// Plain C++: test/host/config_file/build.sh compiles and runs it.

#include "ConfigFileState.hpp"

#include <cstdio>
#include <string>

static int failures = 0;
static int checks = 0;

#define CHECK_EQ(actual, expected)                                                                                                    \
  do {                                                                                                                                \
    checks++;                                                                                                                         \
    auto a_ = (actual);                                                                                                               \
    auto e_ = (expected);                                                                                                             \
    if (!(a_ == e_)) {                                                                                                                \
      std::printf("%s:%d: %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #actual, show(a_).c_str(), show(e_).c_str()); \
      failures++;                                                                                                                     \
    }                                                                                                                                 \
  } while (0)

using State = ConfigFileState;
using Save = ConfigFileState::SaveAction;
using Boot = ConfigFileState::BootAction;
using Load = ConfigFileState::LoadOutcome;

static std::string show(bool b) { return b ? "true" : "false"; }
static std::string show(Save a) {
  switch (a) {
  case Save::WRITE:
    return "WRITE";
  case Save::SKIP_UNCHANGED:
    return "SKIP_UNCHANGED";
  case Save::REFUSE_UNREADABLE:
    return "REFUSE_UNREADABLE";
  }
  return "?";
}
static std::string show(Boot a) {
  switch (a) {
  case Boot::CONNECT:
    return "CONNECT";
  case Boot::SAVE_DEFAULTS_AND_START_AP:
    return "SAVE_DEFAULTS_AND_START_AP";
  case Boot::START_AP_WITHOUT_SAVING:
    return "START_AP_WITHOUT_SAVING";
  }
  return "?";
}

static const std::string A = R"({"wifi_ssid":"home","mqtt_port":1883})";
static const std::string B = R"({"wifi_ssid":"home","mqtt_port":1884})";

// Only a missing file is replaced by the defaults; any other failure leaves the file alone.
static void boot_decision() {
  CHECK_EQ(State::boot_action(Load::LOADED), Boot::CONNECT);
  CHECK_EQ(State::boot_action(Load::MISSING), Boot::SAVE_DEFAULTS_AND_START_AP);
  CHECK_EQ(State::boot_action(Load::UNREADABLE), Boot::START_AP_WITHOUT_SAVING);
}

// After a load, saving the same content is skipped and a change is written.
static void skip_identical_after_load() {
  State s;
  s.loaded(A);
  CHECK_EQ(s.save_action(A, false), Save::SKIP_UNCHANGED);
  CHECK_EQ(s.save_action(A, true), Save::SKIP_UNCHANGED); // The web UI's save of the same values is a no-op too
  CHECK_EQ(s.save_action(B, false), Save::WRITE);
}

// The last successful save becomes the reference; a failed one (saved() not called) does not.
static void skip_identical_after_save() {
  State s;
  s.loaded(A);
  CHECK_EQ(s.save_action(B, false), Save::WRITE);
  // The write of B failed: nothing recorded, so B is still written next time and A is still skipped.
  CHECK_EQ(s.save_action(B, false), Save::WRITE);
  CHECK_EQ(s.save_action(A, false), Save::SKIP_UNCHANGED);
  s.saved(B);
  CHECK_EQ(s.save_action(B, false), Save::SKIP_UNCHANGED);
  CHECK_EQ(s.save_action(A, false), Save::WRITE);
}

// No file: the first save (the defaults at boot) writes, and repeats of it are skipped.
static void missing_file() {
  State s;
  s.missing();
  CHECK_EQ(s.is_unreadable(), false);
  CHECK_EQ(s.save_action(A, false), Save::WRITE);
  s.saved(A);
  CHECK_EQ(s.save_action(A, false), Save::SKIP_UNCHANGED);
}

// A file that could not be read is protected from every save but the user's own, which then always writes.
static void unreadable_file() {
  State s;
  s.unreadable();
  CHECK_EQ(s.is_unreadable(), true);
  CHECK_EQ(s.save_action(A, false), Save::REFUSE_UNREADABLE);
  CHECK_EQ(s.save_action(B, false), Save::REFUSE_UNREADABLE);
  CHECK_EQ(s.save_action(A, true), Save::WRITE);
  // A failed replace leaves it protected.
  CHECK_EQ(s.save_action(A, false), Save::REFUSE_UNREADABLE);
  s.saved(A);
  CHECK_EQ(s.is_unreadable(), false);
  CHECK_EQ(s.save_action(A, false), Save::SKIP_UNCHANGED);
  CHECK_EQ(s.save_action(B, false), Save::WRITE);
}

// A fresh state (nothing loaded yet) never skips: what is on flash is unknown.
static void unknown_never_skips() {
  State s;
  CHECK_EQ(s.save_action("", false), Save::WRITE);
  CHECK_EQ(s.save_action(A, false), Save::WRITE);
  s.loaded(A);
  s.unreadable(); // A later failed load forgets what was known
  CHECK_EQ(s.save_action(A, true), Save::WRITE);
}

int main() {
  boot_decision();
  skip_identical_after_load();
  skip_identical_after_save();
  missing_file();
  unreadable_file();
  unknown_never_skips();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
