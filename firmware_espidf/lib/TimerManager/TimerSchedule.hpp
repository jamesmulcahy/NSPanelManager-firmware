#pragma once
#include <cstdint>
#include <string>

// The pure parts of TimerManager, kept free of ESP-IDF headers so they can be built and tested on a host.
class TimerSchedule {
public:
  // Under this long to the deadline the scheduler wakes exactly at the deadline.
  static constexpr int64_t EXACT_BELOW_US = 2'000'000;
  // The scheduler never sleeps longer than this.
  static constexpr int64_t MAX_SLEEP_US = 3'600'000'000;
  // A timer that expired up to this long before the panel's clock came back fires late; an older one is dropped.
  static constexpr int64_t LATE_WINDOW_S = 15 * 60;
  // Longest timer that can be set: 23:59:59.
  static constexpr uint32_t MAX_DURATION_S = 24 * 3600 - 1;

  /**
   * @brief How long to sleep before the next scheduler wakeup. Halves the gap each time, so a 1 h timer wakes at
   * about 30 m, 15 m, 7.5 m, ... 2 s, then exactly at the deadline.
   * @param remaining_us: Microseconds until the soonest deadline
   * @return 0 when remaining_us <= 0, remaining_us when it is at most EXACT_BELOW_US, otherwise
   * min(remaining_us / 2, MAX_SLEEP_US)
   */
  static int64_t next_wake_us(int64_t remaining_us);

  /**
   * @brief Format a remaining time for the screen, rounded up to whole seconds so a countdown reads 0:01 until
   * it is done: "1:02:03" from an hour up, otherwise "4:05". Zero and negative times read "0:00".
   */
  static std::string format_remaining(int64_t remaining_us);

  enum class RestoreAction {
    RESTORE, // Still running: restore it with the time left
    FIRE_LATE, // Expired no more than LATE_WINDOW_S ago: fire it now
    DROP,    // Expired longer ago than that: drop it
  };

  /**
   * @brief What to do with a timer read from the config once the wall clock is set.
   * @param deadline_epoch_s: The saved wall-clock deadline
   * @param now_epoch_s: The wall clock now
   * @param remaining_s: Set to the seconds left for RESTORE, or the seconds since the deadline for FIRE_LATE and
   * DROP
   */
  static RestoreAction restore_decision(int64_t deadline_epoch_s, int64_t now_epoch_s, int64_t &remaining_s);

  /**
   * @brief Whether a duration can be set on a timer: 1 s to 23:59:59.
   */
  static bool duration_valid(int64_t duration_s) { return duration_s >= 1 && duration_s <= MAX_DURATION_S; }
};
