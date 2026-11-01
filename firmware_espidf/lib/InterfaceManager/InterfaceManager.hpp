#pragma once
#include <MutexWrapper.hpp>
#include <PageHandlers.hpp>
#include <atomic>
#include <esp_event.h>
#include <functional>
#include <memory>
#include <protobuf_nspanel.pb-c.h>

class InterfaceManager {
public:
  /**
   * @brief Initialize the InterfaceManager and the Nextion display. Start loading config and enter main interface.
   */
  static void init();

  /**
   * @brief Make the calling page the current page. Every page's show() calls this first.
   *
   * The previous page is unshown on a separate task, as show() is usually
   * called from inside one of that page's own event handlers. Register the new
   * page's event handlers through the returned PageHandlers, never directly:
   * the previous page's late unshow then only removes its own registrations,
   * not the new page's.
   *
   * @param unshow The page's unshow function. It is skipped if the same page has
   *               been shown again by the time it would run, so that it cannot
   *               reset the state of the newer show().
   * @return The handlers owned by this show() of the page.
   */
  static std::shared_ptr<PageHandlers> begin_page(void (*unshow)());

  /**
   * @brief Is the page with this unshow function the current page?
   *
   * Pages must use this rather than a flag cleared in their unshow(): unshow
   * runs later, on another task, so such a flag still reads "shown" for a
   * while after another page has been shown.
   */
  static bool is_current_page(void (*unshow)());

  /**
   * Show the user selected default page
   */
  static void show_default_page();

  /**
   * @brief Show the home page from the page-transition task.
   * Use this instead of HomePage::show() from a handler on the RoomManager
   * event loop; see _task_page_transitions for why.
   */
  static void request_show_home_page();

  /**
   * @brief Open the entity page for an entity's state topic, from the page-transition task.
   * Wakes the screen first if the screensaver is showing. Safe to call from any task.
   */
  static void request_show_entity_page(const std::string &state_topic);

private:
  // Pages that can be requested from the page-transition task. Only the latest
  // request is shown, so a quick "screen off, screen on" ends with the screen on.
  enum class PageRequest : uint8_t {
    NONE,
    HOME,
    SCREEN_ON, // Screen brightness, then the default page
    SCREENSAVER,
    ENTITY_PAGE, // The entity page for _requested_entity_page_topic
  };

  static void _request_page(PageRequest page);

  // Show the entity page for _requested_entity_page_topic. Runs on the page-transition task.
  static void _show_requested_entity_page();

  /**
   * @brief Handle any event trigger from the Nextion display
   */
  static void _nextion_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle any event trigger from the UpdateManager
   */
  static void _update_manager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle any event trigger from the NSPM_ConfigManager
   */
  static void _nspm_configmanager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle any event trigger from the RoomManager
   */
  static void _room_manager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle data received over MQTT and apply new interface manager settings if applicable.
   */
  static void _mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /*
   * @brief Subscribe to relevant MQTT topics for InterfaceManager
   */
  static void _subscribe_to_relevant_mqtt_topics();

  // The page currently shown, as recorded by begin_page().
  struct ShownPage {
    void (*unshow)(); // nullptr until the first page is shown (static storage is zeroed)
    std::shared_ptr<PageHandlers> handlers;
  };
  static inline ShownPage _current_page;
  static inline portMUX_TYPE _current_page_lock = portMUX_INITIALIZER_UNLOCKED;

  /**
   * Unshows pages handed over by begin_page(). Runs on its own task so that a
   * page's unshow never runs inside one of that page's own handlers.
   */
  static void _task_unshow_page(void *param);
  static void _finish_unshow(ShownPage *page);

  // Page transitions requested from event-loop handlers run here instead.
  //
  // Showing or hiding a page registers and unregisters handlers on both the
  // default event loop and RoomManager's loop, and esp_event takes a loop's
  // mutex for that, the same mutex its own task holds while dispatching. If a
  // handler on one loop does this while a handler on the other does the same,
  // each loop task holds its own mutex and waits on the other's: a permanent
  // deadlock that freezes the UI (e.g. the screensaver is touched just as the
  // first room state arrives). A plain task holds no loop mutex, so it cannot
  // be part of that cycle. Handlers notify it with xTaskNotify, which never blocks.
  //
  // The MQTT client task needs the same treatment. esp-mqtt holds the client's
  // API lock while it runs our MQTT handlers, and default-loop handlers publish
  // (which takes that lock) while the loop's mutex is held. A page switch from
  // an MQTT handler (the screen_cmd topic) would wait on the default loop's
  // mutex while holding the API lock: the same cycle, between those two tasks.
  static void _task_page_transitions(void *param);
  static void _associate_with_manager_if_ready();
  static inline TaskHandle_t _page_transitions_task = NULL;
  static inline std::atomic<PageRequest> _requested_page = PageRequest::NONE;
  // Held by shared_ptr so that MutexWrapped copies it inside its critical section without allocating.
  static inline MutexWrapped<std::shared_ptr<const std::string>> _requested_entity_page_topic;
  static constexpr uint32_t PAGE_TRANSITION_ASSOCIATE = 1 << 0;
  static constexpr uint32_t PAGE_TRANSITION_SHOW_PAGE = 1 << 1;

  // Vars
  // Queue of unshow function pointer to call from _task_unshow_page
  static inline QueueHandle_t _unshow_queue;

  // In the case of a Nextion "sleep" event, ie. trigger screensaver, should we
  // or is the screensaver blocked?
  static inline MutexWrapped<bool> _screensaver_blocked = false;

  // Has the config been loaded from NSPanel Manager container yet?
  static inline std::atomic<bool> _nspm_config_loaded = false;

  // Has the status for the home page been loaded from the manager yet?
  static inline std::atomic<bool> _home_page_status_loaded = false;

  // Has the panel been accepted by a manager as of yet?
  static inline std::atomic<bool> _has_been_associated_with_manager = false;

  // Current NSPanelConfig. Primarily used to check if current screensaver timeout has changed and if so update the interval.
  static inline std::shared_ptr<NSPanelConfig> _nspm_cur_config;

  // MQTT Topics
  static inline std::string _screen_on_off_command_topic;
  static inline std::string _screen_on_off_state_topic;
  static inline std::string _screen_brightness_command_topic;
  static inline std::string _screen_brightness_state_topic;
  static inline std::string _screensaver_brightness_command_topic;
  static inline std::string _screensaver_brightness_state_topic;
  static inline std::string _screensaver_mode_command_topic;
  static inline std::string _screensaver_mode_state_topic;
  static inline std::string _screen_raw_commands;
};