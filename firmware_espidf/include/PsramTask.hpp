#pragma once
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <sdkconfig.h>

/**
 * @brief Create a task pinned to a core with its STACK in PSRAM (the TCB stays in internal RAM), falling back to
 *        an internal stack with a warning if PSRAM has none to give.
 *
 * Needs CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM (off by default on ESP32): without it IDF refuses a
 * PSRAM stack.
 *
 * THE RULE: a task created this way, and everything it calls, must never run code that disables the flash
 * cache, because the stack is unreachable then and IDF aborts: no esp_flash/esp_partition read, write or
 * erase, no NVS, no LittleFS/VFS file I/O (ConfigManager::save_config(), load_config()), no OTA, no
 * spi_flash_*. Audit the call graph before using it and say so at the create site.
 * A task that exits must be deleted with vTaskDeleteWithCaps(), not vTaskDelete().
 */
inline BaseType_t create_task_psram_stack(TaskFunction_t code, const char *name, uint32_t stack_bytes, void *param, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core) {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
  if (xTaskCreatePinnedToCoreWithCaps(code, name, stack_bytes, param, priority, handle, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
    return pdPASS;
  }
  ESP_LOGW("PsramTask", "No PSRAM for the %u byte stack of '%s'; using internal RAM.", (unsigned)stack_bytes, name);
#else
  ESP_LOGW("PsramTask", "PSRAM stacks are not enabled in this build; '%s' uses an internal stack.", name);
#endif
  return xTaskCreatePinnedToCore(code, name, stack_bytes, param, priority, handle, core);
}
