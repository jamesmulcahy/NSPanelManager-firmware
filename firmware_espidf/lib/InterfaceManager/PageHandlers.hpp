#pragma once
#include <array>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>

/**
 * The event handlers one page registered during one show().
 *
 * InterfaceManager::begin_page() hands each show() a fresh PageHandlers and
 * retires the previous page's when that page is unshown. Retiring removes only
 * the registrations this object made, so a late unshow can never remove the
 * handlers of a newer show(), whatever order the two happen to run in.
 *
 * Registrations use the esp_event *instance* API on purpose. The legacy
 * esp_event_handler_register() de-duplicates by function pointer: registering a
 * handler that is still pending removal logs "handler already registered,
 * overwriting", returns ESP_OK, and leaves it to be removed, so a page shown
 * again quickly ends up with no handlers at all. An instance registration is
 * always a new, live entry.
 *
 * No lock is held while calling into esp_event, so this cannot take part in a
 * lock-order cycle with the event loops' own mutexes.
 */
class PageHandlers {
public:
  /**
   * @brief Register a handler on the default event loop for as long as this page is shown.
   */
  esp_err_t add(esp_event_base_t event_base, int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg = NULL);

  /**
   * @brief Register a handler on another event loop (e.g. RoomManager::event_loop()) for as long as this page is shown.
   */
  esp_err_t add_to(esp_event_loop_handle_t event_loop, esp_event_base_t event_base, int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg = NULL);

  /**
   * @brief Unregister everything added. Anything added after this is unregistered immediately.
   */
  void retire();

private:
  struct Registration {
    esp_event_loop_handle_t event_loop; // NULL for the default loop
    esp_event_base_t event_base;
    int32_t event_id;
    esp_event_handler_instance_t instance;
  };

  static void _unregister(const Registration &registration);

  // Pages register a handful of handlers; a fixed array keeps allocation out of the critical section.
  static constexpr size_t MAX_REGISTRATIONS = 8;
  std::array<Registration, MAX_REGISTRATIONS> _registrations = {};
  size_t _count = 0;
  bool _retired = false;
  portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;
};
