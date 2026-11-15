#include <ConfigManager.hpp>
#include <Nextion.hpp>
#include <NextionWindow.hpp>
#include <PanelMetrics.hpp>
#include <Nextion_constans.hpp>
#include <Nextion_event.hpp>
#include <driver/uart.h>
#include <esp_log.h>
#include <algorithm>
#include <esp_timer.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

// Replies that mean "this instruction failed". Each one answers exactly one command, as 0x01 does.
static bool is_error_code(uint8_t code) {
  switch (code) {
  case 0x00: // invalid instruction
  case 0x02: // invalid component id
  case 0x03: // invalid page id
  case 0x04: // invalid picture id
  case 0x05: // invalid font id
  case 0x06: // invalid file operation
  case 0x09: // invalid CRC
  case 0x11: // invalid baud rate
  case 0x12: // invalid waveform id or channel
  case 0x1A: // invalid variable name or attribute
  case 0x1B: // invalid variable operation
  case 0x1C: // assignment failed
  case 0x1D: // EEPROM operation failed
  case 0x1E: // invalid quantity of parameters
  case 0x1F: // IO operation failed
  case 0x20: // escape character invalid
  case 0x23: // variable name too long
    return true;
  default:
    return false;
  }
}

static const uint8_t command_end_sequence[3] = {0xFF, 0xFF, 0xFF};

void NextionWindow::init() {
  esp_log_level_set("NextionWindow", ConfigManager::log_level);
  NextionWindow::_room_sem = xSemaphoreCreateBinary();
  // 4 KB: besides accounting, it runs Nextion::recover_link() when the display goes silent.
  xTaskCreatePinnedToCore(NextionWindow::_task_monitor, "nextion_window", 4096, NULL, 3, NULL, 1);
}

uint32_t NextionWindow::_wire_ms(uint32_t bytes, uint32_t baud) {
  // 8N1: ten bits on the wire per byte.
  return (uint32_t)(((uint64_t)bytes * 10 * 1000 + baud - 1) / baud);
}

void NextionWindow::_configure_for_baud(uint32_t baud) {
  if (baud == 0) {
    baud = MEASURED_BAUD;
  }
  uint32_t window = (uint32_t)((uint64_t)2 * MEASURED_BULK_WINDOW_BYTES * baud / MEASURED_BAUD);
  window = std::clamp(window, MIN_WINDOW_BYTES, MAX_WINDOW_BYTES);
  uint32_t bulk_window = window / 2;
  // Waits and stalls cover a redraw plus the time to drain what is queued ahead of it, with room to spare.
  uint32_t drain_ms = 2 * NextionWindow::_wire_ms(window, baud);
  uint32_t stall_ms = std::max(MEASURED_STALL_MS, REDRAW_MS + drain_ms);
  NextionWindow::_window_bytes = window;
  NextionWindow::_bulk_window_bytes = bulk_window;
  NextionWindow::_max_wait_ms = std::max(MEASURED_MAX_WAIT_MS, REDRAW_MS + drain_ms);
  NextionWindow::_stall_ms = stall_ms;
  NextionWindow::_probe_timeout_ms = std::max(MEASURED_PROBE_TIMEOUT_MS, stall_ms + NextionWindow::_wire_ms(MARKER_INTERVAL, baud));
  ESP_LOGI("NextionWindow", "Write window for %lu baud: %lu B (bulk %lu B), max wait %lu ms, stall %lu ms, probe timeout %lu ms.",
           baud, window, bulk_window, NextionWindow::_max_wait_ms.load(), stall_ms, NextionWindow::_probe_timeout_ms.load());
}

void NextionWindow::start_locked() {
  NextionWindow::_accounting = false;
  NextionWindow::_reset_locked();
  NextionWindow::_configure_for_baud(Nextion::get_link_baud());

  uart_write_bytes(UART_NUM_2, "bkcmd=3", strlen("bkcmd=3"));
  uart_write_bytes(UART_NUM_2, command_end_sequence, sizeof(command_end_sequence));
  taskENTER_CRITICAL(&NextionWindow::_lock);
  NextionWindow::_settling = true;
  taskEXIT_CRITICAL(&NextionWindow::_lock);

  NextionWindow::_accounting = true;
  NextionWindow::_send_marker_locked(false);
}

void NextionWindow::stop_locked() {
  NextionWindow::_accounting = false;
  NextionWindow::_reset_locked();
}

void NextionWindow::_reset_locked() {
  taskENTER_CRITICAL(&NextionWindow::_lock);
  NextionWindow::_fifo_head = 0;
  NextionWindow::_fifo_count = 0;
  NextionWindow::_inflight_bytes = 0;
  NextionWindow::_probe_seq = -1;
  NextionWindow::_settling = false;
  NextionWindow::_resync_requested = false;
  NextionWindow::_restart_requested = false;
  taskEXIT_CRITICAL(&NextionWindow::_lock);
  NextionWindow::_bytes_since_marker = 0;
  if (NextionWindow::_room_sem != NULL) {
    xSemaphoreGive(NextionWindow::_room_sem);
  }
}

void NextionWindow::wait_for_room(write_class kind) {
  if (!NextionWindow::_accounting) {
    return;
  }

  const std::atomic<uint32_t> &window = kind == write_class::BULK ? NextionWindow::_bulk_window_bytes : NextionWindow::_window_bytes;
  int64_t start = esp_timer_get_time();
  bool waited = false;
  for (;;) {
    taskENTER_CRITICAL(&NextionWindow::_lock);
    uint32_t inflight = NextionWindow::_inflight_bytes;
    taskEXIT_CRITICAL(&NextionWindow::_lock);
    if (inflight <= window || !NextionWindow::_accounting) {
      break;
    }
    if (esp_timer_get_time() - start >= (int64_t)NextionWindow::_max_wait_ms * 1000) {
      // Go ahead anyway. The display may still have room; if not, it reports 0x24 and the count resyncs.
      taskENTER_CRITICAL(&NextionWindow::_lock);
      NextionWindow::_stats.wait_timeouts++;
      taskEXIT_CRITICAL(&NextionWindow::_lock);
      break;
    }
    waited = true;
    // Several writers can wait at once and a give wakes only one, so poll as well.
    xSemaphoreTake(NextionWindow::_room_sem, 1);
  }

  int64_t waited_us = esp_timer_get_time() - start;
  // Every write, waited or not, so the histogram's count is the writes and its buckets their waits.
  PanelMetrics::nextion_window_wait[kind == write_class::BULK ? PanelMetrics::NEXTION_WRITE_BULK : PanelMetrics::NEXTION_WRITE_NORMAL].observe_us(waited ? waited_us : 0);
  if (waited) {
    uint32_t waited_ms = (uint32_t)(waited_us / 1000);
    taskENTER_CRITICAL(&NextionWindow::_lock);
    NextionWindow::_stats.waits++;
    if (waited_ms > NextionWindow::_stats.wait_max_ms) {
      NextionWindow::_stats.wait_max_ms = waited_ms;
    }
    taskEXIT_CRITICAL(&NextionWindow::_lock);
  }
}

NextionWindow::stats_t NextionWindow::stats() {
  taskENTER_CRITICAL(&NextionWindow::_lock);
  stats_t stats = NextionWindow::_stats;
  taskEXIT_CRITICAL(&NextionWindow::_lock);
  return stats;
}

uint32_t NextionWindow::inflight_bytes() {
  taskENTER_CRITICAL(&NextionWindow::_lock);
  uint32_t inflight = NextionWindow::_inflight_bytes;
  taskEXIT_CRITICAL(&NextionWindow::_lock);
  return inflight;
}

void NextionWindow::write_locked(const std::string &command) {
  if (NextionWindow::_accounting) {
    uint16_t size = command.size() + sizeof(command_end_sequence);
    // printh is never answered, so there is nothing to wait for. Its bytes count towards the next marker.
    if (command.compare(0, strlen("printh"), "printh") != 0) {
      char prefix[sizeof(entry_t::prefix)];
      snprintf(prefix, sizeof(prefix), "%s", command.c_str());
      // Account before writing: the UART task runs at a higher priority than any writer, and the reply can
      // arrive before uart_write_bytes() returns.
      NextionWindow::_push(entry_kind::COMMAND, size, prefix, 0);
    }
    NextionWindow::_bytes_since_marker += size;
  }

  uart_write_bytes(UART_NUM_2, command.data(), command.size());
  uart_write_bytes(UART_NUM_2, command_end_sequence, sizeof(command_end_sequence));

  if (NextionWindow::_accounting && NextionWindow::_bytes_since_marker >= MARKER_INTERVAL) {
    NextionWindow::_send_marker_locked(false);
  }
}

void NextionWindow::_push(entry_kind kind, uint16_t size, const char *prefix, uint8_t seq) {
  int64_t now = esp_timer_get_time();
  taskENTER_CRITICAL(&NextionWindow::_lock);
  if (NextionWindow::_fifo_count == FIFO_SIZE) {
    // Nothing has been answered for a long time. Drop the oldest; the next marker puts the count right.
    NextionWindow::_inflight_bytes -= NextionWindow::_fifo[NextionWindow::_fifo_head].size;
    NextionWindow::_fifo_head = (NextionWindow::_fifo_head + 1) % FIFO_SIZE;
    NextionWindow::_fifo_count--;
  }
  entry_t &entry = NextionWindow::_fifo[(NextionWindow::_fifo_head + NextionWindow::_fifo_count) % FIFO_SIZE];
  entry.sent_us = now;
  entry.size = size;
  entry.kind = kind;
  entry.seq = seq;
  entry.overflowed = false;
  strncpy(entry.prefix, prefix, sizeof(entry.prefix) - 1);
  entry.prefix[sizeof(entry.prefix) - 1] = '\0';
  NextionWindow::_fifo_count++;
  NextionWindow::_inflight_bytes += size;
  if (kind == entry_kind::COMMAND) {
    NextionWindow::_stats.commands++;
  }
  taskEXIT_CRITICAL(&NextionWindow::_lock);
}

void NextionWindow::_send_marker_locked(bool probe) {
  uint8_t seq = NextionWindow::_next_seq;
  NextionWindow::_next_seq = NextionWindow::_next_seq >= 250 ? 1 : NextionWindow::_next_seq + 1;

  // The display echoes the bytes, ending in its usual terminator: a frame of MARKER_HEAD, seq.
  char marker[32];
  int length = snprintf(marker, sizeof(marker), "printh %02x %02x ff ff ff", MARKER_HEAD, seq);
  NextionWindow::_push(entry_kind::MARKER, length + sizeof(command_end_sequence), "printh", seq);
  if (probe) {
    taskENTER_CRITICAL(&NextionWindow::_lock);
    NextionWindow::_probe_seq = seq;
    NextionWindow::_probe_sent_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&NextionWindow::_lock);
  }
  NextionWindow::_bytes_since_marker = 0;
  uart_write_bytes(UART_NUM_2, marker, length);
  uart_write_bytes(UART_NUM_2, command_end_sequence, sizeof(command_end_sequence));
}

bool NextionWindow::on_frame(const uint8_t *data, size_t size) {
  if (!NextionWindow::_accounting || size == 0) {
    return false;
  }

  int64_t now = esp_timer_get_time();
  uint8_t head = data[0];
  if (size == 2 && head == MARKER_HEAD) {
    NextionWindow::_on_marker_echo(data[1], now);
    return true;
  }
  if (size == 1 && head == NEX_OUT_BUFFER_OVERFLOW) {
    taskENTER_CRITICAL(&NextionWindow::_lock);
    for (size_t i = 0; i < NextionWindow::_fifo_count; i++) {
      NextionWindow::_fifo[(NextionWindow::_fifo_head + i) % FIFO_SIZE].overflowed = true;
    }
    NextionWindow::_resync_requested = true;
    taskEXIT_CRITICAL(&NextionWindow::_lock);
    return false; // Nextion logs it.
  }
  if ((size == 1 && head == NEX_OUT_READY) || (size == 3 && data[0] == 0 && data[1] == 0 && data[2] == 0)) {
    // The display restarted, and with it bkcmd went back to 2.
    taskENTER_CRITICAL(&NextionWindow::_lock);
    NextionWindow::_restart_requested = true;
    taskEXIT_CRITICAL(&NextionWindow::_lock);
    return false;
  }
  if (size == 1 && (head == NEX_RET_CMD_FINISHED || is_error_code(head))) {
    NextionWindow::_on_reply(head, now);
    return true;
  }
  if ((head == NEX_RET_NUMBER_HEAD && size == 5) || head == NEX_RET_STRING_HEAD) {
    // A get answers with its value instead of 0x01. Whoever asked still needs it.
    NextionWindow::_on_reply(head, now);
    return false;
  }
  return false;
}

void NextionWindow::_on_reply(uint8_t code, int64_t now) {
  bool popped = false;
  bool failed = false;
  char prefix[sizeof(entry_t::prefix)] = "";
  taskENTER_CRITICAL(&NextionWindow::_lock);
  NextionWindow::_last_reply_us = now;
  if (NextionWindow::_fifo_count == 0 || NextionWindow::_fifo[NextionWindow::_fifo_head].kind == entry_kind::MARKER) {
    // Markers are never answered, so nothing before the next one is waiting: this reply is for no command we know of.
    if (!NextionWindow::_settling) {
      NextionWindow::_stats.unsolicited++;
    }
  } else {
    entry_t &entry = NextionWindow::_fifo[NextionWindow::_fifo_head];
    if (is_error_code(code)) {
      failed = true;
      NextionWindow::_stats.errors++;
      memcpy(prefix, entry.prefix, sizeof(prefix));
    }
    NextionWindow::_inflight_bytes -= entry.size;
    NextionWindow::_fifo_head = (NextionWindow::_fifo_head + 1) % FIFO_SIZE;
    NextionWindow::_fifo_count--;
    popped = true;
  }
  taskEXIT_CRITICAL(&NextionWindow::_lock);

  if (popped) {
    xSemaphoreGive(NextionWindow::_room_sem);
  }
  if (failed) {
    if (code == NEX_RET_INVALID_PICTURE_ID) {
      ESP_LOGW("NextionWindow", "Display rejected '%s': 0x%02x, invalid picture ID.", prefix, code);
    } else {
      ESP_LOGD("NextionWindow", "Display rejected '%s': 0x%02x.", prefix, code);
    }
  }
}

void NextionWindow::_on_marker_echo(uint8_t seq, int64_t now) {
  taskENTER_CRITICAL(&NextionWindow::_lock);
  NextionWindow::_last_reply_us = now;
  size_t index = SIZE_MAX;
  for (size_t i = 0; i < NextionWindow::_fifo_count; i++) {
    const entry_t &entry = NextionWindow::_fifo[(NextionWindow::_fifo_head + i) % FIFO_SIZE];
    if (entry.kind == entry_kind::MARKER && entry.seq == seq) {
      index = i;
      break;
    }
  }

  // Not found: a marker from before the last reset. Nothing to do.
  if (index != SIZE_MAX) {
    // The display handles commands in order, so it has processed everything ahead of the marker.
    // Whatever is still waiting will never be answered.
    for (size_t i = 0; i <= index; i++) {
      const entry_t &entry = NextionWindow::_fifo[NextionWindow::_fifo_head];
      if (entry.kind == entry_kind::COMMAND && !NextionWindow::_settling) {
        if (entry.overflowed) {
          NextionWindow::_stats.lost_in_overflow++;
        } else {
          NextionWindow::_stats.missing++;
        }
      }
      NextionWindow::_inflight_bytes -= entry.size;
      NextionWindow::_fifo_head = (NextionWindow::_fifo_head + 1) % FIFO_SIZE;
      NextionWindow::_fifo_count--;
    }
    if (NextionWindow::_probe_seq == seq) {
      NextionWindow::_probe_seq = -1;
    }
    NextionWindow::_settling = false;
  }
  taskEXIT_CRITICAL(&NextionWindow::_lock);
  xSemaphoreGive(NextionWindow::_room_sem);
}

void NextionWindow::_task_monitor(void *param) {
  int64_t last_log_us = esp_timer_get_time();
  uint32_t last_anomalies = 0;
  uint32_t last_commands = 0;
  uint32_t failed_starts = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!NextionWindow::_accounting) {
      continue;
    }

    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&NextionWindow::_lock);
    bool restart = NextionWindow::_restart_requested;
    bool probe_timed_out = NextionWindow::_probe_seq >= 0 && now - NextionWindow::_probe_sent_us > (int64_t)NextionWindow::_probe_timeout_ms * 1000;
    int64_t quiet_since = NextionWindow::_last_reply_us;
    if (NextionWindow::_fifo_count > 0 && NextionWindow::_fifo[NextionWindow::_fifo_head].sent_us > quiet_since) {
      quiet_since = NextionWindow::_fifo[NextionWindow::_fifo_head].sent_us;
    }
    bool stalled = NextionWindow::_fifo_count > 0 && NextionWindow::_probe_seq < 0 && now - quiet_since > (int64_t)NextionWindow::_stall_ms * 1000;
    bool resync = NextionWindow::_resync_requested;
    // No marker echoed since the window last started.
    bool settling = NextionWindow::_settling;
    taskEXIT_CRITICAL(&NextionWindow::_lock);

    if (probe_timed_out && settling && Nextion::get_link_baud() != ConfigManager::communication_baud_rate) {
      // Above the default rate, a display that reset by itself comes back at its default and its
      // restart frames arrive garbled, so a silent display is the only sign. A marker lost in an
      // overflow looks the same at first, so this comes only after starting over at this rate
      // has also gone unanswered (settling): then bring the link up again.
      Nextion::recover_link();
      failed_starts = 0;
      NextionWindow::_post_display_restarted();
    } else if (restart || probe_timed_out) {
      if (xSemaphoreTake(Nextion::_uart_write_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        if (NextionWindow::_accounting) {
          taskENTER_CRITICAL(&NextionWindow::_lock);
          // Still settling: not one marker has been echoed since the last start.
          bool never_echoed = NextionWindow::_settling;
          taskEXIT_CRITICAL(&NextionWindow::_lock);
          failed_starts = (probe_timed_out && never_echoed) ? failed_starts + 1 : 0;
        }
        if (failed_starts >= MAX_FAILED_STARTS) {
          // A display that never echoes would otherwise make every write wait the maximum wait.
          ESP_LOGE("NextionWindow", "Display never echoed a marker after %lu starts. Writes to it will not be paced.", MAX_FAILED_STARTS);
          NextionWindow::stop_locked();
          // Nothing consumes replies any more, so stop the display sending them.
          uart_write_bytes(UART_NUM_2, "bkcmd=0", strlen("bkcmd=0"));
          uart_write_bytes(UART_NUM_2, command_end_sequence, sizeof(command_end_sequence));
        } else if (NextionWindow::_accounting) {
          ESP_LOGW("NextionWindow", "%s, starting over with bkcmd=3.", restart ? "Display restarted" : "Display did not echo a marker");
          NextionWindow::start_locked();
          taskENTER_CRITICAL(&NextionWindow::_lock);
          NextionWindow::_stats.resets++;
          taskEXIT_CRITICAL(&NextionWindow::_lock);
        }
        xSemaphoreGive(Nextion::_uart_write_mutex);
        if (restart) {
          NextionWindow::_post_display_restarted();
        }
      }
    } else if ((resync || stalled) && NextionWindow::_probe_seq < 0) {
      if (xSemaphoreTake(Nextion::_uart_write_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (NextionWindow::_accounting) {
          taskENTER_CRITICAL(&NextionWindow::_lock);
          NextionWindow::_resync_requested = false;
          taskEXIT_CRITICAL(&NextionWindow::_lock);
          NextionWindow::_send_marker_locked(true);
        }
        xSemaphoreGive(Nextion::_uart_write_mutex);
      }
    }

    // Warn at most once a minute when something went wrong, otherwise log at debug every ten minutes while busy.
    taskENTER_CRITICAL(&NextionWindow::_lock);
    uint32_t anomalies = NextionWindow::_stats.missing + NextionWindow::_stats.unsolicited + NextionWindow::_stats.resets + NextionWindow::_stats.wait_timeouts;
    uint32_t commands = NextionWindow::_stats.commands;
    taskEXIT_CRITICAL(&NextionWindow::_lock);
    if (anomalies != last_anomalies && now - last_log_us > 60LL * 1000 * 1000) {
      NextionWindow::_log_stats(true);
      last_anomalies = anomalies;
      last_commands = commands;
      last_log_us = now;
    } else if (commands != last_commands && now - last_log_us > 600LL * 1000 * 1000) {
      NextionWindow::_log_stats(false);
      last_commands = commands;
      last_log_us = now;
    }
  }
}

void NextionWindow::_post_display_restarted() {
  // The display is back on its startup page with its default brightness. Post with no timeout, so
  // this task never waits on the default event loop's queue ahead of Wi-Fi, and back off here instead.
  for (int attempt = 0; attempt < 10; attempt++) {
    if (esp_event_post(NEXTION_EVENT, nextion_event_t::DISPLAY_RESTARTED, NULL, 0, 0) == ESP_OK) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  ESP_LOGW("NextionWindow", "Could not post DISPLAY_RESTARTED; the display stays on its startup page until the next page change.");
}

void NextionWindow::_log_stats(bool anomaly) {
  taskENTER_CRITICAL(&NextionWindow::_lock);
  stats_t stats = NextionWindow::_stats;
  taskEXIT_CRITICAL(&NextionWindow::_lock);

  if (anomaly) {
    ESP_LOGW("NextionWindow", "Write window out of step: commands=%lu errors=%lu missing=%lu lost_in_overflow=%lu unsolicited=%lu resets=%lu waits=%lu wait_timeouts=%lu wait_max=%lums",
             stats.commands, stats.errors, stats.missing, stats.lost_in_overflow, stats.unsolicited, stats.resets, stats.waits, stats.wait_timeouts, stats.wait_max_ms);
  } else {
    ESP_LOGD("NextionWindow", "commands=%lu errors=%lu missing=%lu lost_in_overflow=%lu unsolicited=%lu resets=%lu waits=%lu wait_timeouts=%lu wait_max=%lums",
             stats.commands, stats.errors, stats.missing, stats.lost_in_overflow, stats.unsolicited, stats.resets, stats.waits, stats.wait_timeouts, stats.wait_max_ms);
  }
}
