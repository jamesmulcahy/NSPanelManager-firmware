// The timer scheduler's pure logic (lib/TimerManager/TimerSchedule).
// Plain C++: test/host/timer_schedule/build.sh compiles and runs it.

#include "TimerSchedule.hpp"

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

static std::string show(const std::string &s) { return "\"" + s + "\""; }
static std::string show(const char *s) { return show(std::string(s)); }
static std::string show(bool b) { return b ? "true" : "false"; }
static std::string show(int v) { return std::to_string(v); }
static std::string show(long v) { return std::to_string(v); }
static std::string show(TimerSchedule::RestoreAction a) { return std::to_string(static_cast<int>(a)); }

constexpr int64_t S = 1'000'000;

static void next_wake() {
  CHECK_EQ(TimerSchedule::next_wake_us(0), (int64_t)0);
  CHECK_EQ(TimerSchedule::next_wake_us(-5 * S), (int64_t)0);
  CHECK_EQ(TimerSchedule::next_wake_us(1), (int64_t)1);
  CHECK_EQ(TimerSchedule::next_wake_us(2 * S), 2 * S);             // Exactly 2 s: wake at the deadline
  CHECK_EQ(TimerSchedule::next_wake_us(2 * S + 1), (2 * S + 1) / 2); // Just over: halve
  CHECK_EQ(TimerSchedule::next_wake_us(3600 * S), 1800 * S);
  CHECK_EQ(TimerSchedule::next_wake_us(7200 * S), 3600 * S);       // Exactly the cap
  CHECK_EQ(TimerSchedule::next_wake_us(36000 * S), 3600 * S);      // 10 h: capped at 1 h
}

// Follow a timer from start to deadline, waking when next_wake_us says to.
static int wakeups(int64_t duration_us, int64_t *last_wake_at = nullptr) {
  int64_t now = 0;
  int count = 0;
  while (now < duration_us && count < 100000) {
    now += TimerSchedule::next_wake_us(duration_us - now);
    count++;
  }
  if (last_wake_at) *last_wake_at = now;
  return count;
}

static void wakeup_counts() {
  int64_t last = 0;
  int n = wakeups(3600 * S, &last);
  CHECK_EQ(n <= 14, true);
  CHECK_EQ(n, 12);
  CHECK_EQ(last, 3600 * S); // The last wakeup lands exactly on the deadline

  n = wakeups(1 * S, &last);
  CHECK_EQ(n, 1);
  CHECK_EQ(last, 1 * S);

  n = wakeups(86399 * S, &last); // 23:59:59 is capped at 1 h sleeps, so about 24 + the halving tail
  CHECK_EQ(n < 50, true);
  CHECK_EQ(last, 86399 * S);
}

static void format() {
  CHECK_EQ(TimerSchedule::format_remaining(0), "0:00");
  CHECK_EQ(TimerSchedule::format_remaining(-3 * S), "0:00");
  CHECK_EQ(TimerSchedule::format_remaining(1), "0:01"); // Rounded up
  CHECK_EQ(TimerSchedule::format_remaining(1 * S), "0:01");
  CHECK_EQ(TimerSchedule::format_remaining(1 * S + 1), "0:02");
  CHECK_EQ(TimerSchedule::format_remaining(245 * S), "4:05");
  CHECK_EQ(TimerSchedule::format_remaining(599 * S + 500000), "10:00");
  CHECK_EQ(TimerSchedule::format_remaining(3599 * S), "59:59");
  CHECK_EQ(TimerSchedule::format_remaining(3600 * S), "1:00:00");
  CHECK_EQ(TimerSchedule::format_remaining(3723 * S), "1:02:03");
  CHECK_EQ(TimerSchedule::format_remaining(86399 * S), "23:59:59");
}

static void restore() {
  using A = TimerSchedule::RestoreAction;
  int64_t now = 1'800'000'000;
  int64_t amount = -1;

  CHECK_EQ(TimerSchedule::restore_decision(now + 600, now, amount), A::RESTORE);
  CHECK_EQ(amount, (int64_t)600);
  CHECK_EQ(TimerSchedule::restore_decision(now + 1, now, amount), A::RESTORE);
  CHECK_EQ(amount, (int64_t)1);

  CHECK_EQ(TimerSchedule::restore_decision(now, now, amount), A::FIRE_LATE); // Expired this very second
  CHECK_EQ(amount, (int64_t)0);
  CHECK_EQ(TimerSchedule::restore_decision(now - 180, now, amount), A::FIRE_LATE);
  CHECK_EQ(amount, (int64_t)180);
  CHECK_EQ(TimerSchedule::restore_decision(now - 900, now, amount), A::FIRE_LATE); // Exactly 15 min
  CHECK_EQ(amount, (int64_t)900);

  CHECK_EQ(TimerSchedule::restore_decision(now - 901, now, amount), A::DROP);
  CHECK_EQ(amount, (int64_t)901);
  CHECK_EQ(TimerSchedule::restore_decision(now - 86400, now, amount), A::DROP);
}

static void durations() {
  CHECK_EQ(TimerSchedule::duration_valid(0), false);
  CHECK_EQ(TimerSchedule::duration_valid(1), true);
  CHECK_EQ(TimerSchedule::duration_valid(86399), true);
  CHECK_EQ(TimerSchedule::duration_valid(86400), false);
  CHECK_EQ(TimerSchedule::duration_valid(-1), false);
}

int main() {
  next_wake();
  wakeup_counts();
  format();
  restore();
  durations();
  if (failures > 0) {
    std::printf("%d of %d checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("timer_schedule: all %d checks passed\n", checks);
  return 0;
}
