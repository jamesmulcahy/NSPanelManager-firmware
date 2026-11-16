#ifdef NSPM_HEAP_DIAG

#include <HeapDiag.hpp>
#include <cstdio>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_ipc_isr.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_private/cache_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#ifndef CONFIG_HEAP_USE_HOOKS
#error "NSPM_HEAP_DIAG needs CONFIG_HEAP_USE_HOOKS=y"
#endif

// ======================================================================================================
// Allocation accounting
//
// esp_heap_trace_alloc_hook() / esp_heap_trace_free_hook() run on the allocating task after every
// allocation and free, outside the heap's lock, on either core, from ISRs, and with the flash cache
// disabled during a flash write. So they:
//  - are IRAM (IRAM_ATTR), and call nothing that is not, except heap_caps_get_free_size() which is
//    only reached once spi_flash_cache_enabled() has said both caches are on;
//  - never allocate, log or block (a short spinlock, held for a table probe);
//  - never touch PSRAM while the cache is off: they count the call and return.
//
// The table of live allocations (pointer -> size, class) lives in PSRAM: 16 Ki entries of 8 bytes. It is
// allocated by a static constructor, which runs before every other C++ static and before app_main; only
// allocations made earlier (ESP-IDF's core init) are untracked, and are counted.
//
// realloc: IDF reports an in-place realloc as one alloc hook call (same or new pointer) and no free hook.
// A hook call for a pointer that is already in the table replaces the entry, which is right when the block
// stayed put. When a realloc moved the block inside its heap, the old pointer is never reported, so its
// entry stays until that address is allocated again ("stale"). /debug/heap counts the stale entries by
// comparing the table with a heap walk, so the error is known.

namespace {

constexpr uint32_t INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

constexpr uint32_t TABLE_BITS = 14;
constexpr uint32_t TABLE_ENTRIES = 1u << TABLE_BITS;
constexpr uint32_t TABLE_MAX_LOAD = TABLE_ENTRIES - TABLE_ENTRIES / 12; // ~92%: past it, probes get long

constexpr uint32_t SIZE_MASK = 0x0FFFFFFF;
constexpr uint32_t CLASS_SHIFT = 28;
constexpr uint32_t SEEN_BIT = 1u << 31; // Used by the dump to find entries no heap block matches.

enum : uint32_t { CLASS_MOVABLE, CLASS_PINNED, CLASS_PSRAM, CLASS_COUNT };
constexpr int BUCKETS = 12; // <=16, 32, 64, 128, 256, 512, 1K, 2K, 4K, 8K, 16K, >16K
constexpr int THRESHOLDS = 8; // 64, 128, ... 8K; threshold k is the upper edge of bucket k + 2

struct Entry {
  uint32_t ptr; // 0 = empty
  uint32_t meta;
};

struct Low {
  uint32_t free; // UINT32_MAX until the first internal allocation after the window opened
  uint32_t above[THRESHOLDS];
};

Entry *table = nullptr;
uint32_t table_count = 0;
portMUX_TYPE hook_lock = portMUX_INITIALIZER_UNLOCKED;

// All under `lock`. Sizes are the sizes asked for, not the blocks' sizes (the allocator adds a header
// and rounds up).
uint32_t live_bytes[CLASS_COUNT][BUCKETS];
uint32_t live_count[CLASS_COUNT][BUCKETS];
uint64_t total_allocs[CLASS_COUNT][BUCKETS];
Low low_boot = {UINT32_MAX, {}};
Low low_window = {UINT32_MAX, {}};
Low low_window_closed = {UINT32_MAX, {}};
uint32_t overflow_total;
uint32_t unknown_free_total;
uint32_t skipped_cache_off_total;
uint32_t early_total; // Hook calls before the table existed.
uint32_t reuse_total; // Alloc hook calls for a pointer already in the table.

inline __attribute__((always_inline)) uint32_t bucket_of(uint32_t size) {
  if (size <= 16) {
    return 0;
  }
  uint32_t b = 32 - __builtin_clz(size - 1) - 4; // 17..32 -> 1, 33..64 -> 2, ...
  return b > BUCKETS - 1 ? BUCKETS - 1 : b;
}

inline __attribute__((always_inline)) uint32_t slot_of(uint32_t ptr) { return ((ptr >> 2) * 2654435761u) >> (32 - TABLE_BITS); }

// All of these run under `lock`, with `table` set.
void IRAM_ATTR counters_add(uint32_t cls, uint32_t size, bool add) {
  uint32_t b = bucket_of(size);
  if (add) {
    live_bytes[cls][b] += size;
    live_count[cls][b]++;
    total_allocs[cls][b]++;
  } else {
    live_bytes[cls][b] -= size < live_bytes[cls][b] ? size : live_bytes[cls][b];
    if (live_count[cls][b] > 0) {
      live_count[cls][b]--;
    }
  }
}

void IRAM_ATTR table_insert(uint32_t ptr, uint32_t size, uint32_t cls, bool *inserted) {
  uint32_t i = slot_of(ptr);
  *inserted = false;
  for (uint32_t probes = 0; probes < TABLE_ENTRIES; probes++, i = (i + 1) & (TABLE_ENTRIES - 1)) {
    if (table[i].ptr == ptr) {
      reuse_total++;
      counters_add((table[i].meta >> CLASS_SHIFT) & 3, table[i].meta & SIZE_MASK, false);
      table[i].meta = size | (cls << CLASS_SHIFT);
      *inserted = true;
      return;
    }
    if (table[i].ptr == 0) {
      if (table_count >= TABLE_MAX_LOAD) {
        overflow_total++;
        return;
      }
      table[i].ptr = ptr;
      table[i].meta = size | (cls << CLASS_SHIFT);
      table_count++;
      *inserted = true;
      return;
    }
  }
  overflow_total++;
}

// Linear probing with backward-shift deletion, so there are no tombstones.
bool IRAM_ATTR table_remove(uint32_t ptr, uint32_t *size, uint32_t *cls) {
  uint32_t i = slot_of(ptr);
  uint32_t probes = 0;
  while (table[i].ptr != ptr) {
    if (table[i].ptr == 0 || ++probes >= TABLE_ENTRIES) {
      return false;
    }
    i = (i + 1) & (TABLE_ENTRIES - 1);
  }
  *size = table[i].meta & SIZE_MASK;
  *cls = (table[i].meta >> CLASS_SHIFT) & 3;
  uint32_t j = i;
  for (;;) {
    j = (j + 1) & (TABLE_ENTRIES - 1);
    if (table[j].ptr == 0) {
      break;
    }
    uint32_t k = slot_of(table[j].ptr);
    // Move j back to i unless its home slot k lies cyclically in (i, j].
    bool in_range = i <= j ? (i < k && k <= j) : (i < k || k <= j);
    if (!in_range) {
      table[i] = table[j];
      i = j;
    }
  }
  table[i].ptr = 0;
  table[i].meta = 0;
  table_count--;
  return true;
}

// Called after an internal allocation, with the free internal heap measured just after it.
void IRAM_ATTR note_low(uint32_t free_now) {
  if (free_now >= low_boot.free && free_now >= low_window.free) {
    return;
  }
  Low snapshot;
  snapshot.free = free_now;
  uint32_t above = 0; // Live movable bytes in buckets above threshold k, up to 16 KB.
  for (int k = THRESHOLDS - 1; k >= 0; k--) {
    above += live_bytes[CLASS_MOVABLE][k + 3]; // threshold k = bucket k + 2's edge, so bucket k + 3 is above it
    snapshot.above[k] = above;
  }
  if (free_now < low_boot.free) {
    low_boot = snapshot;
  }
  if (free_now < low_window.free) {
    low_window = snapshot;
  }
}

__attribute__((constructor(101))) void init_table() {
  Entry *t = static_cast<Entry *>(heap_caps_calloc(TABLE_ENTRIES, sizeof(Entry), MALLOC_CAP_SPIRAM));
  portENTER_CRITICAL(&hook_lock);
  table = t; // nullptr on a board without PSRAM: the hooks then only count what they missed
  portEXIT_CRITICAL(&hook_lock);
}

} // namespace

// IRAM: esp_heap_caps.h declares both hooks with HEAP_IRAM_ATTR, which the definitions inherit.
extern "C" void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps) {
  if (!spi_flash_cache_enabled()) {
    portENTER_CRITICAL_SAFE(&hook_lock);
    skipped_cache_off_total++;
    portEXIT_CRITICAL_SAFE(&hook_lock);
    return;
  }
  uint32_t cls = esp_ptr_external_ram(ptr) ? CLASS_PSRAM : ((caps & MALLOC_CAP_DEFAULT) ? CLASS_MOVABLE : CLASS_PINNED);
  uint32_t free_now = 0;
  // Read outside `lock`: the heap lock is taken in here, and the only thing that nests the other
  // way round would be a heap walk's callback, which does not take `lock`.
  if (cls != CLASS_PSRAM && table != nullptr) {
    free_now = heap_caps_get_free_size(INTERNAL_CAPS);
  }
  uint32_t asked = size > SIZE_MASK ? SIZE_MASK : size;
  portENTER_CRITICAL_SAFE(&hook_lock);
  if (table == nullptr) {
    early_total++;
  } else {
    bool inserted;
    table_insert(reinterpret_cast<uint32_t>(ptr), asked, cls, &inserted);
    if (inserted) {
      counters_add(cls, asked, true);
      if (cls != CLASS_PSRAM && free_now != 0) {
        note_low(free_now);
      }
    }
  }
  portEXIT_CRITICAL_SAFE(&hook_lock);
}

extern "C" void esp_heap_trace_free_hook(void *ptr) {
  if (!spi_flash_cache_enabled()) {
    portENTER_CRITICAL_SAFE(&hook_lock);
    skipped_cache_off_total++;
    portEXIT_CRITICAL_SAFE(&hook_lock);
    return;
  }
  portENTER_CRITICAL_SAFE(&hook_lock);
  if (table == nullptr) {
    early_total++;
  } else {
    uint32_t size, cls;
    if (table_remove(reinterpret_cast<uint32_t>(ptr), &size, &cls)) {
      counters_add(cls, size, false);
    } else {
      unknown_free_total++;
    }
  }
  portEXIT_CRITICAL_SAFE(&hook_lock);
}

namespace HeapDiag {

void scrape_begin() {
  portENTER_CRITICAL(&hook_lock);
  low_window_closed = low_window;
  low_window.free = UINT32_MAX;
  portEXIT_CRITICAL(&hook_lock);
}

namespace {

const char *CLASS_NAMES[CLASS_COUNT] = {"movable", "pinned", "psram"};
const char *BUCKET_LABELS[BUCKETS] = {"16", "32", "64", "128", "256", "512", "1024", "2048", "4096", "8192", "16384", "+Inf"};
const char *THRESHOLD_LABELS[THRESHOLDS] = {"64", "128", "256", "512", "1024", "2048", "4096", "8192"};

template <typename T>
void write_by_class_and_bucket(Metrics::TextWriter &out, const char *name, const char *type, const char *help, const T (*values)[BUCKETS]) {
  out.put("# HELP ");
  out.put(name);
  out.put(' ');
  out.put(help);
  out.put("\n# TYPE ");
  out.put(name);
  out.put(' ');
  out.put(type);
  out.put('\n');
  for (int c = 0; c < CLASS_COUNT; c++) {
    for (int b = 0; b < BUCKETS; b++) {
      out.put(name);
      out.put("{class=\"");
      out.put(CLASS_NAMES[c]);
      out.put("\",le=\"");
      out.put(BUCKET_LABELS[b]);
      out.put("\"} ");
      out.put_u64(values[c][b]);
      out.put('\n');
    }
  }
}

void write_scalar(Metrics::TextWriter &out, const char *name, const char *type, const char *help, uint64_t value) {
  out.put("# HELP ");
  out.put(name);
  out.put(' ');
  out.put(help);
  out.put("\n# TYPE ");
  out.put(name);
  out.put(' ');
  out.put(type);
  out.put('\n');
  out.put(name);
  out.put(' ');
  out.put_u64(value);
  out.put('\n');
}

} // namespace

void write_metrics(Metrics::TextWriter &out) {
  uint32_t bytes[CLASS_COUNT][BUCKETS];
  uint32_t count[CLASS_COUNT][BUCKETS];
  uint64_t total[CLASS_COUNT][BUCKETS];
  Low window, boot;
  uint32_t overflow, unknown, skipped, early, reuse, entries;
  bool active;
  portENTER_CRITICAL(&hook_lock);
  memcpy(bytes, live_bytes, sizeof(bytes));
  memcpy(count, live_count, sizeof(count));
  memcpy(total, total_allocs, sizeof(total));
  window = low_window_closed;
  boot = low_boot;
  overflow = overflow_total;
  unknown = unknown_free_total;
  skipped = skipped_cache_off_total;
  early = early_total;
  reuse = reuse_total;
  entries = table_count;
  active = table != nullptr;
  portEXIT_CRITICAL(&hook_lock);

  write_by_class_and_bucket(out, "nspanel_heap_alloc_live_bytes", "gauge",
                            "Bytes asked for by live heap allocations, by class and size bucket (le is the bucket's upper edge in bytes; "
                            "NOT cumulative like a Prometheus histogram). movable: malloc()/new, in internal RAM, could go to PSRAM under a lower "
                            "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL; pinned: explicit capabilities, in internal RAM; psram: in PSRAM. Sizes are as "
                            "requested, not block sizes. See nspanel_heap_hook_* for what the tracking missed.",
                            bytes);
  write_by_class_and_bucket(out, "nspanel_heap_alloc_live_count", "gauge", "Number of live heap allocations, by class and size bucket (not cumulative).", count);
  write_by_class_and_bucket(out, "nspanel_heap_alloc_total", "counter",
                            "Allocations made since boot, by class and size bucket (churn; not cumulative across buckets). A realloc counts as one.", total);

  out.put("# HELP nspanel_heap_movable_at_low_bytes Live movable bytes in allocations larger than threshold (and up to 16 KB), "
          "at the moment internal free heap was lowest: what a CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL of that value would have taken off "
          "internal RAM at its worst point. window=scrape: lowest point since the previous scrape (as of the last one), boot: since boot.\n");
  out.put("# TYPE nspanel_heap_movable_at_low_bytes gauge\n");
  const Low *lows[2] = {&window, &boot};
  const char *low_names[2] = {"scrape", "boot"};
  for (int w = 0; w < 2; w++) {
    if (lows[w]->free == UINT32_MAX) {
      continue;
    }
    for (int k = 0; k < THRESHOLDS; k++) {
      out.put("nspanel_heap_movable_at_low_bytes{threshold=\"");
      out.put(THRESHOLD_LABELS[k]);
      out.put("\",window=\"");
      out.put(low_names[w]);
      out.put("\"} ");
      out.put_u64(lows[w]->above[k]);
      out.put('\n');
    }
  }
  out.put("# HELP nspanel_heap_free_at_low_bytes Internal free heap at the low point the nspanel_heap_movable_at_low_bytes of the same window was taken at.\n");
  out.put("# TYPE nspanel_heap_free_at_low_bytes gauge\n");
  for (int w = 0; w < 2; w++) {
    if (lows[w]->free == UINT32_MAX) {
      continue;
    }
    out.put("nspanel_heap_free_at_low_bytes{window=\"");
    out.put(low_names[w]);
    out.put("\"} ");
    out.put_u64(lows[w]->free);
    out.put('\n');
  }

  write_scalar(out, "nspanel_heap_hook_active", "gauge", "1 if the allocation table exists, 0 if it could not be allocated (no PSRAM).", active ? 1 : 0);
  write_scalar(out, "nspanel_heap_hook_table_entries", "gauge", "Allocations currently in the tracking table (includes stale entries, see /debug/heap).", entries);
  write_scalar(out, "nspanel_heap_hook_table_capacity", "gauge", "Entries the table can hold before it overflows.", TABLE_MAX_LOAD);
  write_scalar(out, "nspanel_heap_hook_table_overflow_total", "counter", "Allocations not tracked because the table was full. Live totals miss them.", overflow);
  write_scalar(out, "nspanel_heap_hook_unknown_free_total", "counter",
               "Frees of a pointer the table did not hold: allocated before tracking, while the flash cache was off, or lost to the table being full.", unknown);
  write_scalar(out, "nspanel_heap_hook_skipped_cache_off_total", "counter",
               "Alloc and free hook calls skipped because the flash cache was off (PSRAM cannot be read then). Live totals drift by these.", skipped);
  write_scalar(out, "nspanel_heap_hook_early_total", "counter", "Alloc and free hook calls before the table existed (ESP-IDF core init).", early);
  write_scalar(out, "nspanel_heap_hook_reuse_total", "counter",
               "Alloc hook calls for a pointer already in the table: an in-place realloc, or the address reused after a free the table missed.", reuse);
}

// ======================================================================================================
// Heap dump

namespace {

constexpr uint32_t DRAM_START = 0x3FFAE000;
constexpr uint32_t DRAM_END = 0x40000000;
constexpr uint32_t DRAM_LENGTH = DRAM_END - DRAM_START;
constexpr uint32_t MAX_HEAPS = 8;
constexpr uint32_t MAX_TASKS = 48;
constexpr uint32_t MAX_BLOCKS = 12288;
constexpr const char *TAG = "HeapDiag";

struct __attribute__((packed)) TaskRecord {
  char name[16];
  uint32_t stack_base;
  uint32_t unused_bytes;
};

struct __attribute__((packed)) HeapRecord {
  uint32_t start;
  uint32_t end;
  uint32_t first_block; // pointer to the first block's data: the walk starts here
};

// Layout shared with tools/heap_decode.py (HEADER_FMT, HEAP_FMT, TASK_FMT).
struct __attribute__((packed)) BinHeader {
  char magic[8];
  uint32_t version;
  uint32_t header_size; // The memory snapshot follows at this offset.
  uint32_t flags;
  char elf_sha[72]; // Hex, as esp_app_get_elf_sha256() gives it: the first characters of the ELF's SHA-256.
  uint32_t region_start;
  uint32_t region_length;
  uint32_t uptime_s;
  uint32_t stall_us; // How long the other core was held for the copy.
  uint32_t n_heaps;
  uint32_t n_tasks;
  HeapRecord heaps[MAX_HEAPS];
  TaskRecord tasks[MAX_TASKS];
};

struct BlockRecord {
  uint32_t ptr;
  uint32_t size_used; // size << 1 | used
  uint8_t head[16];
};

bool busy = false; // One dump at a time: httpd runs handlers on one task, but a dump holds ~300 KB of PSRAM.

struct Walk {
  BlockRecord *blocks;
  uint32_t count;
  uint32_t truncated;
};

// Runs with the heap's lock held: no allocating, logging or sending in here.
bool collect_block(walker_heap_into_t, walker_block_info_t block, void *user) {
  Walk *walk = static_cast<Walk *>(user);
  if (walk->count >= MAX_BLOCKS) {
    walk->truncated++;
    return true;
  }
  BlockRecord &r = walk->blocks[walk->count++];
  r.ptr = reinterpret_cast<uint32_t>(block.ptr);
  r.size_used = (static_cast<uint32_t>(block.size) << 1) | (block.used ? 1 : 0);
  memcpy(r.head, block.ptr, block.size < sizeof(r.head) ? block.size : sizeof(r.head));
  if (block.size < sizeof(r.head)) {
    memset(r.head + block.size, 0, sizeof(r.head) - block.size);
  }
  return true;
}

struct HeapWalk {
  HeapRecord *heaps;
  uint32_t count;
};

// The first call for a heap gives its first block, which is all the decoder needs to walk it.
bool collect_heap(walker_heap_into_t heap, walker_block_info_t block, void *user) {
  HeapWalk *walk = static_cast<HeapWalk *>(user);
  if (walk->count < MAX_HEAPS) {
    walk->heaps[walk->count++] = {static_cast<uint32_t>(heap.start), static_cast<uint32_t>(heap.end), reinterpret_cast<uint32_t>(block.ptr)};
  }
  return false; // Done with this heap.
}

uint32_t collect_tasks(TaskRecord *out) {
  UBaseType_t capacity = uxTaskGetNumberOfTasks() + 8;
  TaskStatus_t *tasks = static_cast<TaskStatus_t *>(heap_caps_malloc(capacity * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
  if (tasks == nullptr) {
    return 0;
  }
  UBaseType_t n = uxTaskGetSystemState(tasks, capacity, nullptr);
  uint32_t count = 0;
  for (UBaseType_t i = 0; i < n && count < MAX_TASKS; i++) {
    memset(out[count].name, 0, sizeof(out[count].name));
    strncpy(out[count].name, tasks[i].pcTaskName, sizeof(out[count].name) - 1);
    out[count].stack_base = reinterpret_cast<uint32_t>(tasks[i].pxStackBase);
    out[count].unused_bytes = tasks[i].usStackHighWaterMark;
    count++;
  }
  heap_caps_free(tasks);
  return count;
}

// The class of the allocation at @param ptr as the table has it, marking the entry as matched.
// Not called with the heap's lock held, so taking `lock` here cannot nest the wrong way.
const char *class_of(uint32_t ptr) {
  const char *result = "untracked";
  portENTER_CRITICAL(&hook_lock);
  if (table != nullptr) {
    uint32_t i = slot_of(ptr);
    for (uint32_t probes = 0; probes < TABLE_ENTRIES && table[i].ptr != 0; probes++, i = (i + 1) & (TABLE_ENTRIES - 1)) {
      if (table[i].ptr == ptr) {
        table[i].meta |= SEEN_BIT;
        result = CLASS_NAMES[(table[i].meta >> CLASS_SHIFT) & 3];
        break;
      }
    }
  }
  portEXIT_CRITICAL(&hook_lock);
  return result;
}

// Entries in the table for internal heap memory that no block of the walk matched (a realloc moved
// the block, or a free was missed): the error in the live totals. Clears the matched marks.
void count_stale(uint32_t *entries, uint32_t *bytes) {
  *entries = 0;
  *bytes = 0;
  for (uint32_t base = 0; base < TABLE_ENTRIES; base += 256) {
    portENTER_CRITICAL(&hook_lock);
    if (table != nullptr) {
      for (uint32_t i = base; i < base + 256; i++) {
        if (table[i].ptr == 0) {
          continue;
        }
        uint32_t cls = (table[i].meta >> CLASS_SHIFT) & 3;
        if (!(table[i].meta & SEEN_BIT) && cls != CLASS_PSRAM && esp_ptr_internal(reinterpret_cast<void *>(table[i].ptr)) &&
            esp_ptr_byte_accessible(reinterpret_cast<void *>(table[i].ptr))) {
          (*entries)++;
          *bytes += table[i].meta & SIZE_MASK;
        }
        table[i].meta &= ~SEEN_BIT;
      }
    }
    portEXIT_CRITICAL(&hook_lock);
  }
}

class Sender {
public:
  explicit Sender(httpd_req_t *req) : _req(req) {}
  bool put(const char *data, size_t length) {
    while (length > 0 && _ok) {
      size_t n = sizeof(_buffer) - _used;
      if (n > length) {
        n = length;
      }
      memcpy(_buffer + _used, data, n);
      _used += n;
      data += n;
      length -= n;
      if (_used == sizeof(_buffer)) {
        flush();
      }
    }
    return _ok;
  }
  bool put(const char *text) { return put(text, strlen(text)); }
  bool flush() {
    if (_ok && _used > 0) {
      _ok = httpd_resp_send_chunk(_req, _buffer, _used) == ESP_OK;
    }
    _used = 0;
    return _ok;
  }
  bool ok() const { return _ok; }

private:
  httpd_req_t *_req;
  bool _ok = true;
  size_t _used = 0;
  char _buffer[1024];
};

void hex_bytes(char *dst, const uint8_t *src, size_t n) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    dst[2 * i] = digits[src[i] >> 4];
    dst[2 * i + 1] = digits[src[i] & 15];
  }
}

struct BusyGuard {
  bool acquired;
  BusyGuard() : acquired(!busy) { busy = true; }
  ~BusyGuard() {
    if (acquired) {
      busy = false;
    }
  }
};

esp_err_t handle_heap_text(httpd_req_t *req) {
  BusyGuard guard;
  if (!guard.acquired) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "another dump is running\n", HTTPD_RESP_USE_STRLEN);
  }
  BlockRecord *blocks = static_cast<BlockRecord *>(heap_caps_malloc(MAX_BLOCKS * sizeof(BlockRecord), MALLOC_CAP_SPIRAM));
  if (blocks == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "no PSRAM for the dump\n", HTTPD_RESP_USE_STRLEN);
  }
  TaskRecord tasks[MAX_TASKS];
  HeapRecord heaps[MAX_HEAPS];
  HeapWalk heap_walk = {heaps, 0};
  Walk walk = {blocks, 0, 0};

  uint32_t n_tasks = collect_tasks(tasks);
  heap_caps_walk(INTERNAL_CAPS, collect_heap, &heap_walk);
  int64_t started_us = esp_timer_get_time();
  heap_caps_walk(INTERNAL_CAPS, collect_block, &walk);
  uint32_t walk_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
  ESP_LOGW(TAG, "/debug/heap: %u blocks in %u us (heap lock held)", (unsigned)walk.count, (unsigned)walk_us);

  // Classes and the stale count: after the walk, so `lock` is never taken inside the heap's.
  const char **classes = static_cast<const char **>(heap_caps_malloc(walk.count * sizeof(const char *), MALLOC_CAP_SPIRAM));
  if (classes == nullptr) {
    heap_caps_free(blocks);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "no PSRAM for the dump\n", HTTPD_RESP_USE_STRLEN);
  }
  for (uint32_t i = 0; i < walk.count; i++) {
    classes[i] = (blocks[i].size_used & 1) ? class_of(blocks[i].ptr) : "free";
  }
  uint32_t stale_entries, stale_bytes;
  count_stale(&stale_entries, &stale_bytes);

  char sha[72] = {};
  esp_app_get_elf_sha256(sha, sizeof(sha));
  uint32_t entries, overflow, unknown, skipped, early;
  portENTER_CRITICAL(&hook_lock);
  entries = table_count;
  overflow = overflow_total;
  unknown = unknown_free_total;
  skipped = skipped_cache_off_total;
  early = early_total;
  portEXIT_CRITICAL(&hook_lock);

  httpd_resp_set_type(req, "text/plain");
  Sender out(req);
  char line[160];
  snprintf(line, sizeof(line), "# nspm heap dump v1 (contains secrets)\n# uptime_s=%u elf_sha=%s\n", (unsigned)(esp_timer_get_time() / 1000000), sha);
  out.put(line);
  snprintf(line, sizeof(line), "# internal_free=%u internal_min_free=%u largest_free_block=%u\n", (unsigned)heap_caps_get_free_size(INTERNAL_CAPS),
           (unsigned)heap_caps_get_minimum_free_size(INTERNAL_CAPS), (unsigned)heap_caps_get_largest_free_block(INTERNAL_CAPS));
  out.put(line);
  snprintf(line, sizeof(line), "# table_entries=%u overflow=%u unknown_free=%u skipped_cache_off=%u early=%u stale_entries=%u stale_bytes=%u\n",
           (unsigned)entries, (unsigned)overflow, (unsigned)unknown, (unsigned)skipped, (unsigned)early, (unsigned)stale_entries, (unsigned)stale_bytes);
  out.put(line);
  snprintf(line, sizeof(line), "# blocks=%u truncated=%u walk_us=%u\n", (unsigned)walk.count, (unsigned)walk.truncated, (unsigned)walk_us);
  out.put(line);
  for (uint32_t i = 0; i < heap_walk.count; i++) {
    snprintf(line, sizeof(line), "# heap start=%08x end=%08x\n", (unsigned)heaps[i].start, (unsigned)heaps[i].end);
    out.put(line);
  }
  for (uint32_t i = 0; i < n_tasks; i++) {
    snprintf(line, sizeof(line), "# task %s %08x %u\n", tasks[i].name, (unsigned)tasks[i].stack_base, (unsigned)tasks[i].unused_bytes);
    out.put(line);
  }
  out.put("# columns: address size U|F class first-16-bytes-hex\n");
  for (uint32_t i = 0; i < walk.count && out.ok(); i++) {
    int n = snprintf(line, sizeof(line), "%08x %u %c %s ", (unsigned)blocks[i].ptr, (unsigned)(blocks[i].size_used >> 1), (blocks[i].size_used & 1) ? 'U' : 'F',
                     classes[i]);
    hex_bytes(line + n, blocks[i].head, 16);
    line[n + 32] = '\n';
    out.put(line, n + 33);
  }
  out.flush();
  bool ok = out.ok();
  heap_caps_free(classes);
  heap_caps_free(blocks);
  if (!ok) {
    return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// Both cores are held for the copy: esp_ipc_isr_stall_other_cpu() masks interrupts on this one until
// the release. ~330 KB into PSRAM is tens of ms on hardware; the time is logged and put in the header.
uint32_t snapshot_dram(uint8_t *dst) {
  int64_t started_us = esp_timer_get_time();
  esp_ipc_isr_stall_other_cpu();
  memcpy(dst, reinterpret_cast<const void *>(DRAM_START), DRAM_LENGTH);
  esp_ipc_isr_release_other_cpu();
  return static_cast<uint32_t>(esp_timer_get_time() - started_us);
}

esp_err_t handle_heap_bin(httpd_req_t *req) {
  BusyGuard guard;
  if (!guard.acquired) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "another dump is running\n", HTTPD_RESP_USE_STRLEN);
  }
  uint8_t *snapshot = static_cast<uint8_t *>(heap_caps_malloc(DRAM_LENGTH, MALLOC_CAP_SPIRAM));
  if (snapshot == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "no PSRAM for the dump\n", HTTPD_RESP_USE_STRLEN);
  }
  BinHeader header = {};
  HeapWalk heap_walk = {header.heaps, 0};
  header.n_tasks = collect_tasks(header.tasks);
  heap_caps_walk(INTERNAL_CAPS, collect_heap, &heap_walk);
  header.n_heaps = heap_walk.count;
  memcpy(header.magic, "NSPMHEAP", 8);
  header.version = 1;
  header.header_size = sizeof(BinHeader);
  esp_app_get_elf_sha256(header.elf_sha, sizeof(header.elf_sha));
  header.region_start = DRAM_START;
  header.region_length = DRAM_LENGTH;
  header.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
  header.stall_us = snapshot_dram(snapshot);
  ESP_LOGW(TAG, "/debug/heap.bin: both cores held for %u us", (unsigned)header.stall_us);

  httpd_resp_set_type(req, "application/octet-stream");
  esp_err_t result = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(&header), sizeof(header));
  for (uint32_t offset = 0; result == ESP_OK && offset < DRAM_LENGTH; offset += 8192) {
    uint32_t n = DRAM_LENGTH - offset < 8192 ? DRAM_LENGTH - offset : 8192;
    result = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(snapshot + offset), n);
  }
  heap_caps_free(snapshot);
  if (result != ESP_OK) {
    return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

httpd_uri_t uri_heap_text = {.uri = "/debug/heap", .method = HTTP_GET, .handler = handle_heap_text, .user_ctx = nullptr};
httpd_uri_t uri_heap_bin = {.uri = "/debug/heap.bin", .method = HTTP_GET, .handler = handle_heap_bin, .user_ctx = nullptr};

} // namespace

void register_uris(httpd_handle_t server) {
  httpd_register_uri_handler(server, &uri_heap_text);
  httpd_register_uri_handler(server, &uri_heap_bin);
  ESP_LOGW(TAG, "NSPM_HEAP_DIAG build: /debug/heap and /debug/heap.bin are served, and they contain secrets");
}

} // namespace HeapDiag

#endif // NSPM_HEAP_DIAG
