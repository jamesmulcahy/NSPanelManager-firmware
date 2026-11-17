#include <TimerSchedule.hpp>
#include <algorithm>
#include <cstdio>

int64_t TimerSchedule::next_wake_us(int64_t remaining_us) {
  if (remaining_us <= 0) {
    return 0;
  }
  if (remaining_us <= EXACT_BELOW_US) {
    return remaining_us;
  }
  return std::min(remaining_us / 2, MAX_SLEEP_US);
}

std::string TimerSchedule::format_remaining(int64_t remaining_us) {
  int64_t total_s = remaining_us <= 0 ? 0 : (remaining_us + 999'999) / 1'000'000;
  int64_t h = total_s / 3600;
  int64_t m = (total_s / 60) % 60;
  int64_t s = total_s % 60;
  char buf[24];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", static_cast<long long>(h), static_cast<long long>(m), static_cast<long long>(s));
  } else {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(m), static_cast<long long>(s));
  }
  return buf;
}

TimerSchedule::RestoreAction TimerSchedule::restore_decision(int64_t deadline_epoch_s, int64_t now_epoch_s, int64_t &remaining_s) {
  int64_t left = deadline_epoch_s - now_epoch_s;
  if (left > 0) {
    remaining_s = left;
    return RestoreAction::RESTORE;
  }
  remaining_s = -left;
  return remaining_s <= LATE_WINDOW_S ? RestoreAction::FIRE_LATE : RestoreAction::DROP;
}
