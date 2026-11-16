#pragma once
#ifdef NSPM_HEAP_DIAG

#include <Metrics.hpp>
#include <esp_http_server.h>

// Diagnostic build only (-DNSPM_HEAP_DIAG): what the internal heap holds, and why it dips.
//
//  - Allocation accounting (esp_heap_trace_alloc_hook / _free_hook, CONFIG_HEAP_USE_HOOKS): every live
//    allocation by size bucket and by whether a lower CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL could move
//    it, and what a threshold would have taken off the internal heap at its low point.
//  - GET /debug/heap and /debug/heap.bin: a dump of the internal heap. THEY CONTAIN SECRETS (Wi-Fi PSK,
//    MQTT password, all of RAM) and exist only in this build. Decode with tools/heap_decode.py.
namespace HeapDiag {

/** Close the per-scrape low-point window (HeapMetrics::scrape_begin() calls this). */
void scrape_begin();

/** Write nspanel_heap_alloc_* and friends. */
void write_metrics(Metrics::TextWriter &out);

/** Register /debug/heap and /debug/heap.bin on @param server. */
void register_uris(httpd_handle_t server);

} // namespace HeapDiag

#endif
