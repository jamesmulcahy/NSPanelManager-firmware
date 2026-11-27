#pragma once
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <string>
#include <vector>

/**
 * Album art for LVGL: a small cache of the manager's RGB565 images in PSRAM, keyed by the full
 * album_art_url (it carries ?v=<hash of the source>, so it identifies the track's art) plus the
 * square size asked for. Images are fetched whole on a task of LvglArt's own and handed over as an
 * lv_image_dsc_t that LVGL draws from directly (its own image cache is off).
 *
 * Lifetime: every image is reached through a Handle, a refcounted hold on its slot. The pixels stay
 * valid, and are never written over or freed, until the last Handle on them is gone. Only entries
 * nobody holds are evicted, least recently used first. So a view that keeps a Handle for as long as
 * an lv_image points at its pixels can never draw a torn or freed image, whatever other callers fetch.
 *
 * Tasks: lookup(), fetch() and Handle copies/releases work from any task (a short mutex, never held
 * across the network or a callback). Callbacks run on LvglArt's task; they must hand drawing to the
 * GUI task (LvglUi::show() / LvglRenderer::post()) as usual.
 *
 * Log lines (tag LvglArt) that tests count: "Fetched <w>x<h> album art" for each network fetch, and
 * the DEBUG "cache hit" for each request answered from memory.
 */
class LvglArt {
public:
  // Slots in the cache, and the PSRAM all their pixels may take together. Each slot is allocated at the
  // size of the image it holds, so this fits the media page's 180 px and a screensaver card's ~224 px for
  // the current track, plus the previous track at both sizes.
  // On the custom PCB's 720x720 screen (drawn at 1.5x) the art is 1.5x the size: 270 px and ~336 px.
  static constexpr int SLOTS = 4;
#if defined(BOARD_CUSTOM)
  static constexpr size_t BUDGET_BYTES = 1280 * 1024;
  // The largest side that may be asked for.
  static constexpr uint16_t MAX_SIZE = 384;
#else
  static constexpr size_t BUDGET_BYTES = 512 * 1024;
  // The largest side that may be asked for.
  static constexpr uint16_t MAX_SIZE = 256;
#endif

  /**
   * A hold on a cached image. Copyable (each copy holds too) and movable; the hold ends when the
   * last copy is destroyed or reset(). Empty when default constructed, or when the image could not be had.
   */
  class Handle {
  public:
    Handle() = default;
    Handle(const Handle &other);
    Handle(Handle &&other) noexcept;
    Handle &operator=(const Handle &other);
    Handle &operator=(Handle &&other) noexcept;
    ~Handle();

    // The image, valid for as long as this Handle (or a copy) is held. NULL when empty.
    const lv_image_dsc_t *image() const { return this->_image; }
    explicit operator bool() const { return this->_slot >= 0; }
    // Let go of the image now.
    void reset();

  private:
    friend class LvglArt;
    // Adopts a hold already counted in the slot's refs (made under LvglArt's mutex).
    Handle(int slot, const lv_image_dsc_t *image) : _slot(slot), _image(image) {}

    int _slot = -1;
    const lv_image_dsc_t *_image = nullptr;
  };

  // A fetch's result, on LvglArt's task: a Handle on the image, or an empty one if it could not be had.
  using Done = void (*)(Handle image, const std::string &url);

  /**
   * From any task: the image for `url` at `size`x`size` if it is already in the cache, with no
   * network. Empty if it isn't (or is still being fetched).
   */
  static Handle lookup(const std::string &url, uint16_t size);

  /**
   * From any task: get `url` (a media player's album_art_url) at `size`x`size`, then call `done` on
   * LvglArt's task. A hit in the cache is answered without the network. A request for an image already
   * on its way joins that fetch, and the same `done` joining twice is called once.
   *
   * Requests are served in order. If more than SLOTS are waiting, the oldest that hasn't started is
   * dropped and its callers are not called: callers key on the URL, and ask again when they need to.
   */
  static void fetch(const std::string &url, uint16_t size, Done done);

private:
  enum class _SlotState : uint8_t { EMPTY, FETCHING, READY };
  struct _Slot {
    std::string url; // The key, with size; meaningful while FETCHING or READY
    uint16_t size = 0;
    _SlotState state = _SlotState::EMPTY;
    uint16_t refs = 0;      // Handles on it. Never evicted, written to or freed while > 0
    uint32_t last_used = 0; // _clock when last handed out or let go, for LRU
    uint8_t *pixels = nullptr;
    size_t capacity = 0; // Bytes allocated at pixels (PSRAM); counted against BUDGET_BYTES
    lv_image_dsc_t dsc = {};
  };
  struct _Request {
    std::string url;
    uint16_t size;
    std::vector<Done> done;
    bool started = false;
  };

  static bool _setup();
  static void _task(void *param);
  // Fetch into `slot`, which the task has reserved (FETCHING): nobody holds it, so it may be written.
  static bool _fetch_into(const std::string &url, uint16_t size, int slot);
  // Mutex held: make `bytes` of pixels available to `slot` within the budget, evicting unheld entries.
  static bool _allocate(int slot, size_t bytes);
  // Mutex held: the READY slot for the key, or -1.
  static int _find(const std::string &url, uint16_t size);
  // Mutex held: a slot to fetch the key into (an empty one, else the least recently used unheld entry),
  // now FETCHING; -1 if every slot is held or busy.
  static int _reserve(const std::string &url, uint16_t size);
  // Mutex held: a new hold on READY `slot`.
  static Handle _hold(int slot);
  static void _retain(int slot);
  static void _release(int slot);

  static inline TaskHandle_t _task_handle = NULL;
  static inline SemaphoreHandle_t _mutex = NULL; // Guards everything below; bookkeeping only
  static _Slot _slots[SLOTS];            // Defined in LvglArt.cpp
  static std::vector<_Request> _requests; // Defined in LvglArt.cpp
  static inline uint32_t _clock = 0;
};
