#pragma once
#include <Metrics.hpp>
#include <cstdint>

// Heap and task series that are not in PanelMetrics' static registry, because they are not a fixed
// list of atomics: they are read from the allocator and the scheduler at scrape time.
//
//   M2  nspanel_heap_window_min_free_bytes: the low point of internal free heap since the previous
//       scrape (since boot for the first), and the since-boot minima that nspanel_heap_min_free_bytes
//       reports (see scrape_begin() for why those two need care).
//   M1  nspanel_task_stack_unused_bytes{task,mem}, nspanel_tasks, nspanel_task_scan_seconds.
namespace HeapMetrics {

/**
 * Close the current low-point window and open the next. Call once at the start of every scrape,
 * under PanelMetrics' scrape lock and before anything else in the scrape allocates, so the window
 * ends before the response's own allocations.
 */
void scrape_begin();

/**
 * Minimum free bytes since boot for the pools with these @param caps, as of the last scrape_begin().
 * ESP-IDF's local-minimum monitor replaces the minimum it reports with the window's, so the real
 * since-boot value is read in scrape_begin() when the monitor is stopped. Before the first scrape it
 * is the live value. @return false if the pool does not exist.
 */
bool min_free_since_boot(uint32_t caps, int64_t *value);

/** Write the dynamic series (window minimum, tasks, and with NSPM_HEAP_DIAG the allocation tables). */
void write(Metrics::TextWriter &out);

} // namespace HeapMetrics
