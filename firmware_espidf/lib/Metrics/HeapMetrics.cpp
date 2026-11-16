#include <HeapMetrics.hpp>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef NSPM_HEAP_DIAG
#include <HeapDiag.hpp>
#endif

namespace HeapMetrics {

namespace {

constexpr uint32_t INTERNAL = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t PSRAM = MALLOC_CAP_SPIRAM;

// Only touched under PanelMetrics' scrape lock (scrape_begin() and write() both run under it).
bool monitoring = false;
bool boot_min_captured = false;
size_t boot_min_internal = 0;
size_t boot_min_psram = 0;
size_t window_min_internal = 0;

} // namespace

void scrape_begin() {
  // heap_caps_monitor_local_minimum_free_size_start() resets every heap's minimum to its current free
  // size and keeps the old one on the side; _stop() merges them back (min of the two). While the
  // monitor runs, heap_caps_get_minimum_free_size() therefore reports the window's minimum, not the
  // one since boot (IDF 6.1, heap_caps.c). _start() on a running monitor re-arms it without losing the
  // old minima, but the since-boot value is only readable after _stop(). So each scrape does:
  //   read (the window) -> _stop() -> read (since boot) -> _start() (the next window).
  // The first scrape has no monitor running, so its "window" is the time since boot.
  // _start() allocates a few bytes (the saved minima) and asserts if that fails. The response has not
  // allocated yet, which is the point of doing this first; a heap that cannot spare 24 bytes is lost anyway.
  window_min_internal = heap_caps_get_minimum_free_size(INTERNAL);
  if (monitoring) {
    heap_caps_monitor_local_minimum_free_size_stop();
  }
  boot_min_internal = heap_caps_get_minimum_free_size(INTERNAL);
  boot_min_psram = heap_caps_get_minimum_free_size(PSRAM);
  boot_min_captured = true;
  monitoring = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
#ifdef NSPM_HEAP_DIAG
  HeapDiag::scrape_begin();
#endif
}

bool min_free_since_boot(uint32_t caps, int64_t *value) {
  if (heap_caps_get_total_size(caps) == 0) {
    return false;
  }
  if (boot_min_captured && caps == INTERNAL) {
    *value = boot_min_internal;
  } else if (boot_min_captured && caps == PSRAM) {
    *value = boot_min_psram;
  } else {
    *value = heap_caps_get_minimum_free_size(caps);
  }
  return true;
}

namespace {

void write_window_min(Metrics::TextWriter &out) {
  out.put("# HELP nspanel_heap_window_min_free_bytes Lowest free internal heap since the previous scrape (since boot for the first). "
          "It includes the previous scrape's own allocations, not this one's. Taken from heap_caps_monitor_local_minimum_free_size_*, "
          "and like nspanel_heap_min_free_bytes it is the sum of each internal region's own minimum, so it can be a little below the lowest total.\n");
  out.put("# TYPE nspanel_heap_window_min_free_bytes gauge\n");
  out.put("nspanel_heap_window_min_free_bytes{pool=\"internal\"} ");
  out.put_u64(window_min_internal);
  out.put('\n');
}

// Whether any task already used this name in this scrape, so that every series is unique.
bool name_seen(const TaskStatus_t *tasks, UBaseType_t before, const char *name) {
  for (UBaseType_t i = 0; i < before; i++) {
    if (strcmp(tasks[i].pcTaskName, name) == 0) {
      return true;
    }
  }
  return false;
}

void write_tasks(Metrics::TextWriter &out) {
  UBaseType_t capacity = uxTaskGetNumberOfTasks() + 8; // Tasks can start between the two calls.
  // From PSRAM when there is some: the array is ~1 KB, but the point of this scrape is to not cost
  // internal heap. Fall back to any 8-bit memory on a board without PSRAM.
  TaskStatus_t *tasks = static_cast<TaskStatus_t *>(heap_caps_malloc(capacity * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
  if (tasks == nullptr) {
    tasks = static_cast<TaskStatus_t *>(heap_caps_malloc(capacity * sizeof(TaskStatus_t), MALLOC_CAP_8BIT));
  }
  if (tasks == nullptr) {
    return;
  }
  int64_t started_us = esp_timer_get_time();
  UBaseType_t n = uxTaskGetSystemState(tasks, capacity, nullptr);
  int64_t scan_us = esp_timer_get_time() - started_us;

  out.put("# HELP nspanel_tasks Number of FreeRTOS tasks, ours and ESP-IDF's.\n# TYPE nspanel_tasks gauge\nnspanel_tasks ");
  out.put_u64(n);
  out.put('\n');
  out.put("# HELP nspanel_task_scan_seconds How long uxTaskGetSystemState() took in this scrape. It scans every stack for its "
          "high-water mark with the scheduler locked (interrupts masked on this core), and the scan is longer the more of a stack is unused.\n");
  out.put("# TYPE nspanel_task_scan_seconds gauge\nnspanel_task_scan_seconds ");
  out.put_seconds(scan_us < 0 ? 0 : scan_us);
  out.put('\n');
  // StackType_t is a byte on this port, so usStackHighWaterMark is in bytes, not words.
  out.put("# HELP nspanel_task_stack_unused_bytes Bytes of a task's stack that have never been used (its high-water mark, in bytes). "
          "mem is where the stack lives. A task that no longer exists is not listed; a second task with the same name gets a #2 suffix.\n");
  out.put("# TYPE nspanel_task_stack_unused_bytes gauge\n");
  for (UBaseType_t i = 0; i < n; i++) {
    const char *name = tasks[i].pcTaskName;
    out.put("nspanel_task_stack_unused_bytes{task=\"");
    out.put_escaped(name);
    if (name_seen(tasks, i, name)) {
      uint32_t copy = 1;
      for (UBaseType_t j = 0; j < i; j++) {
        if (strcmp(tasks[j].pcTaskName, name) == 0) {
          copy++;
        }
      }
      out.put('#');
      out.put_u64(copy);
    }
    out.put("\",mem=\"");
    out.put(esp_ptr_external_ram(tasks[i].pxStackBase) ? "psram" : "internal");
    out.put("\"} ");
    out.put_u64(tasks[i].usStackHighWaterMark);
    out.put('\n');
  }
  heap_caps_free(tasks);
}

} // namespace

void write(Metrics::TextWriter &out) {
  write_window_min(out);
  write_tasks(out);
#ifdef NSPM_HEAP_DIAG
  HeapDiag::write_metrics(out);
#endif
}

} // namespace HeapMetrics
