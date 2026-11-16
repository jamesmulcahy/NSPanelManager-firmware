#pragma once
#include <atomic>
#include <esp_event.h>
#include <stdint.h>

ESP_EVENT_DECLARE_BASE(EVENTLOOPWATCHDOG_EVENT);

/**
 * @brief Diagnostic: notices when the default event loop stops dispatching, and logs every task's
 *        backtrace so the handler it is stuck in, and what that handler waits on, can be found.
 *
 * A probe event is posted to the default loop every few seconds. If none has been handled for
 * stall_after_ms, every task's backtrace is captured from esp_rom_printf and logged, a line per
 * task, through the normal log path, so it reaches the manager over MQTT. That happens once per
 * stall: it is armed again only after the loop has dispatched a probe.
 *
 * Off by default (ConfigManager::loop_watchdog); the loop_watchdog command switches it at runtime. Only
 * the watch task is started and stopped: the probe handler is registered once at boot whatever the
 * setting, because registering on the default loop after boot can deadlock it (see CLAUDE.md, "Lock order").
 *
 * Decode a line with:
 *   xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/original_sonoff/firmware.elf <pc> <pc> ...
 */
class EventLoopWatchdog {
public:
  /**
   * @brief Register the probe handler. Call once, after the default loop exists. Does not start the task.
   */
  static void init();

  /**
   * @brief Start or stop the watch task. Any task; repeating the current state does nothing. A stopped
   *        watchdog posts no probes. Starting resets the stall clock, so no stale stall is reported.
   */
  static void set_enabled(bool enable);

private:
  static void _probe_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
  static void _task_watch(void *param);
  static void _log_all_backtraces();
  static void _capture_putc(char c);

  static constexpr uint32_t probe_interval_ms = 2000;
  static constexpr uint32_t stall_after_ms = 15000;
  static constexpr int backtrace_depth = 16;
  static constexpr size_t capture_size = 16 * 1024;

  static inline std::atomic<int64_t> _last_handled_us = 0;

  // The state asked for, and whether a watch task exists (it may still be on its way out after a disable).
  static inline std::atomic<bool> _wanted = false;
  static inline std::atomic<bool> _task_alive = false;
  // Set on every enable so a running task forgets a stall it reported before.
  static inline std::atomic<bool> _rearm = false;

  // Written only by _capture_putc() while the scheduler is suspended, read after.
  static inline char *_capture = nullptr;
  static inline size_t _capture_len = 0;
};
