#pragma once
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <string>

/**
 * Album art for LVGL: the manager's 180x180 RGB565 image,
 * fetched whole on a task of its own into PSRAM, then handed over as an lv_image_dsc_t. The page
 * draws at once with a placeholder, and only the art's rectangle is sent when it arrives
 * (~65 KB, ~0.2 s over the terminal).
 *
 * Two buffers: a fetch fills the one not shown last, so the image on screen is never written to.
 * A newer fetch() supersedes one in flight.
 */
class LvglArt {
public:
  static constexpr int SIZE = 180;

  /**
   * From any task: fetch `url` (the media player's album_art_url). `done` runs on LvglArt's task with
   * the image, or NULL if it could not be had; not at all if another fetch() or cancel() came first.
   */
  static void fetch(const std::string &url, void (*done)(const lv_image_dsc_t *image, const std::string &url));

  /**
   * Forget the fetch in flight, if any.
   */
  static void cancel();

private:
  static void _task(void *param);
  static bool _fetch_into(const std::string &url, uint8_t *buffer);

  static inline TaskHandle_t _task_handle = NULL;
  static inline SemaphoreHandle_t _mutex = NULL;
  static inline std::string _url; // Guarded by _mutex
  static inline void (*_done)(const lv_image_dsc_t *, const std::string &) = nullptr;
  static inline std::atomic<uint32_t> _generation = 0;
  static inline uint8_t *_buffers[2] = {NULL, NULL};
  static inline lv_image_dsc_t _images[2] = {};
  static inline int _shown = 1; // The buffer handed over last (task only)
};
