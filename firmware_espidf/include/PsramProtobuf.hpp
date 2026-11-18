#pragma once
#include <esp_heap_caps.h>
#include <protobuf-c/protobuf-c.h>
#include <stdlib.h>

/**
 * @brief Allocator that makes protobuf-c decode messages into PSRAM instead of internal RAM.
 *
 * Pass psram_protobuf_allocator() to every *__unpack() and to the matching *__free_unpacked().
 *
 * WHY: with a NULL allocator protobuf-c uses malloc(), and with CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 every
 * allocation under 16 KB (every decoded message) is served from the internal heap, which is the scarce pool. A burst
 * of retained MQTT messages (for example when the manager restarts) drove the internal heap to near zero. PSRAM has
 * plenty of room. If PSRAM has nothing to give we fall back to plain malloc() so a decode never fails because of
 * this change.
 *
 * FLASH-SAFE: decoded messages are never handed to the flash driver (save_config() copies fields into ConfigManager
 * and writes JSON with fwrite(), and esp_flash_write() bounces non-DRAM sources anyway). Only task STACKS must stay
 * in internal RAM (see PsramTask.hpp).
 *
 * free() works on any ESP-IDF heap, so strings or arrays that code assigns into a decoded message with plain
 * malloc() are still released correctly by __free_unpacked(), which uses heap_caps_free().
 */
inline void *psram_protobuf_alloc(void *, size_t size) {
  void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return ptr != NULL ? ptr : malloc(size);
}

inline void psram_protobuf_free(void *, void *ptr) { heap_caps_free(ptr); }

inline ProtobufCAllocator *psram_protobuf_allocator() {
  static ProtobufCAllocator allocator = {psram_protobuf_alloc, psram_protobuf_free, NULL};
  return &allocator;
}
