// LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC): everything in PSRAM.
//
// The default C library malloc would put LVGL's objects and styles, which are all small, in
// internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL keeps allocations under 16 KB there), and
// internal RAM is the panels' scarce resource. PSRAM is 2 MiB and nearly unused.
#include <esp_heap_caps.h>
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define LV_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
static void *alloc_psram(size_t size) {
  return heap_caps_malloc(size, LV_PSRAM_CAPS);
}

void lv_mem_init(void) {
}

void lv_mem_deinit(void) {
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
  LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size) {
  return alloc_psram(size);
}

void *lv_realloc_core(void *p, size_t new_size) {
  if (p == NULL) {
    return alloc_psram(new_size);
  }
  return heap_caps_realloc(p, new_size, LV_PSRAM_CAPS);
}

void lv_free_core(void *p) {
  heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  multi_heap_info_t info;
  heap_caps_get_info(&info, LV_PSRAM_CAPS);
  mon_p->total_size = heap_caps_get_total_size(LV_PSRAM_CAPS);
  mon_p->free_size = info.total_free_bytes;
  mon_p->free_biggest_size = info.largest_free_block;
  mon_p->free_cnt = info.free_blocks;
  mon_p->max_used = mon_p->total_size - info.minimum_free_bytes;
  mon_p->used_pct = mon_p->total_size ? 100 - (100 * info.total_free_bytes) / mon_p->total_size : 0;
  mon_p->frag_pct = info.total_free_bytes ? 100 - (100 * info.largest_free_block) / info.total_free_bytes : 0;
}

lv_result_t lv_mem_test_core(void) {
  return heap_caps_check_integrity(LV_PSRAM_CAPS, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

#endif
