#include <EventLoopWatchdog.hpp>
#include <esp_debug_helpers.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <string>

ESP_EVENT_DEFINE_BASE(EVENTLOOPWATCHDOG_EVENT);

void EventLoopWatchdog::init() {
  EventLoopWatchdog::_last_handled_us = esp_timer_get_time();
  // Always registered, enabled or not, and never unregistered: doing either after boot can deadlock the loop.
  if (esp_event_handler_register(EVENTLOOPWATCHDOG_EVENT, 0, &EventLoopWatchdog::_probe_handler, NULL) != ESP_OK) [[unlikely]] {
    ESP_LOGE("LoopWatchdog", "Failed to register the probe handler.");
  }
}

void EventLoopWatchdog::set_enabled(bool enable) {
  if (!enable) {
    if (EventLoopWatchdog::_wanted.exchange(false)) {
      ESP_LOGI("LoopWatchdog", "Disabled; the watch task exits at its next wake-up.");
    }
    return;
  }

  // Order matters: the clock and the rearm flag are set before _wanted, so a task that sees _wanted sees them.
  EventLoopWatchdog::_last_handled_us = esp_timer_get_time();
  EventLoopWatchdog::_rearm = true;
  if (EventLoopWatchdog::_wanted.exchange(true)) {
    return; // Already enabled.
  }
  ESP_LOGI("LoopWatchdog", "Enabled.");
  bool expected = false;
  if (EventLoopWatchdog::_task_alive.compare_exchange_strong(expected, true)) {
    // Low priority, like the other helpers that post to the default loop, so the Wi-Fi task wins any tie.
    if (xTaskCreatePinnedToCore(&EventLoopWatchdog::_task_watch, "loop_watchdog", 4096, NULL, 2, NULL, 1) != pdPASS) [[unlikely]] {
      ESP_LOGE("LoopWatchdog", "Failed to create the watchdog task.");
      EventLoopWatchdog::_task_alive = false;
      EventLoopWatchdog::_wanted = false;
    }
  }
  // Otherwise a task from before a quick disable is still alive: it sees _wanted again and carries on.
}

void EventLoopWatchdog::_probe_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  EventLoopWatchdog::_last_handled_us = esp_timer_get_time();
}

void EventLoopWatchdog::_task_watch(void *param) {
  bool reported = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(EventLoopWatchdog::probe_interval_ms));
    if (!EventLoopWatchdog::_wanted) {
      EventLoopWatchdog::_task_alive = false;
      // An enable between the check above and the line before saw a live task and started none. Take the role back.
      bool expected = false;
      if (EventLoopWatchdog::_wanted && EventLoopWatchdog::_task_alive.compare_exchange_strong(expected, true)) {
        continue;
      }
      ESP_LOGI("LoopWatchdog", "Watch task stopped.");
      vTaskDelete(NULL);
    }
    if (EventLoopWatchdog::_rearm.exchange(false)) {
      reported = false;
      continue; // _last_handled_us was reset by the enable; probe on the next round.
    }
    // Timeout 0: only take a slot that is already free. Waiting on the queue of a stalled loop would
    // stall this task too, and it would never report.
    esp_event_post(EVENTLOOPWATCHDOG_EVENT, 0, NULL, 0, 0);

    int64_t since_ms = (esp_timer_get_time() - EventLoopWatchdog::_last_handled_us) / 1000;
    if (since_ms < EventLoopWatchdog::stall_after_ms) {
      if (reported) {
        ESP_LOGW("LoopWatchdog", "Default event loop is dispatching again.");
        reported = false;
      }
      continue;
    }
    if (reported) {
      continue;
    }
    reported = true;
    ESP_LOGE("LoopWatchdog", "Default event loop has not dispatched for %lld ms. Task backtraces follow.", since_ms);
    EventLoopWatchdog::_log_all_backtraces();
  }
}

void EventLoopWatchdog::_capture_putc(char c) {
  if (EventLoopWatchdog::_capture_len < EventLoopWatchdog::capture_size - 1) {
    EventLoopWatchdog::_capture[EventLoopWatchdog::_capture_len++] = c;
  }
}

void EventLoopWatchdog::_log_all_backtraces() {
#if !CONFIG_IDF_TARGET_ARCH_XTENSA
  // esp_backtrace_print_all_tasks() is Xtensa-only: on RISC-V (the custom PCB's ESP32-P4) ESP-IDF unwinds
  // a backtrace only in the panic handler. The stall itself was logged.
  ESP_LOGW("LoopWatchdog", "Task backtraces are not available on this chip.");
#else
  // PSRAM first: 16 KB of internal RAM is the last thing a stalled panel has to spare. Safe there because
  // _capture_putc() runs in this task's context with the scheduler suspended and the other core held in
  // an IPC call, so no flash operation can have the cache off meanwhile (esp_backtrace_print_all_tasks()).
  EventLoopWatchdog::_capture = (char *)heap_caps_malloc(EventLoopWatchdog::capture_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (EventLoopWatchdog::_capture == nullptr) {
    EventLoopWatchdog::_capture = (char *)heap_caps_malloc(EventLoopWatchdog::capture_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (EventLoopWatchdog::_capture == nullptr) {
    ESP_LOGE("LoopWatchdog", "No memory for the backtrace capture.");
    return;
  }
  EventLoopWatchdog::_capture_len = 0;

  // esp_backtrace_print_all_tasks() writes with esp_rom_printf(), which goes to the UART (channel 1)
  // and to channel 2 if one is installed. Nothing in this firmware installs channel 2, so borrow it.
  esp_rom_install_channel_putc(2, &EventLoopWatchdog::_capture_putc);
  esp_err_t err = esp_backtrace_print_all_tasks(EventLoopWatchdog::backtrace_depth);
  esp_rom_install_channel_putc(2, NULL);
  EventLoopWatchdog::_capture[EventLoopWatchdog::_capture_len] = '\0';

  if (err != ESP_OK) {
    ESP_LOGW("LoopWatchdog", "Some backtraces are incomplete: %s", esp_err_to_name(err));
  }

  // The output is a task name on one line, then "Backtrace: pc:sp pc:sp ..." on the next. Join them.
  std::string task_name;
  char *saveptr = nullptr;
  for (char *line = strtok_r(EventLoopWatchdog::_capture, "\r\n", &saveptr); line != nullptr; line = strtok_r(nullptr, "\r\n", &saveptr)) {
    while (*line == ' ') {
      line++;
    }
    if (*line == '\0') {
      continue;
    }
    if (strncmp(line, "Backtrace:", strlen("Backtrace:")) == 0) {
      ESP_LOGE("LoopWatchdog", "%s %s", task_name.c_str(), line);
      task_name.clear();
      // The log queue is small and drops when full. Let the forwarder keep up.
      vTaskDelay(pdMS_TO_TICKS(150));
    } else {
      task_name = line;
    }
  }
  if (EventLoopWatchdog::_capture_len >= EventLoopWatchdog::capture_size - 1) {
    ESP_LOGW("LoopWatchdog", "Backtrace capture was truncated at %u bytes.", (unsigned)EventLoopWatchdog::capture_size);
  }
  ESP_LOGE("LoopWatchdog", "End of task backtraces.");

  free(EventLoopWatchdog::_capture);
  EventLoopWatchdog::_capture = nullptr;
#endif
}
