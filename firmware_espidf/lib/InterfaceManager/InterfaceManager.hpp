#pragma once
#include <MqttSubscriptions.hpp>
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
   * called from inside one of that page's own event handlers. Add the new
   * page's event handlers to the returned PageHandlers, never register them
   * with esp_event directly: InterfaceManager's dispatchers deliver each event
   * to the current page's handlers only, and nothing is ever unregistered (see
   * PageHandlers for why unregistering can deadlock the default event loop).
   *
   * Without a UI (!LvglUi::active()) the loading page is the only page: every other one is refused
   * (logged as a bug; it does not become current and gets no events), and nothing in InterfaceManager
   * asks for one. The HMI page code therefore never runs.
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
   * @brief Does init() register a page dispatcher for this loop (NULL for the default loop) and event base?
   */
  static bool is_dispatched(esp_event_loop_handle_t event_loop, esp_event_base_t event_base);

  /**
   * Show the user selected default page
   */
  static void show_default_page();

  /**
   * Brings up the HMI page `page` (a GUI_data.hpp name) on the Nextion. The only HMI page this
   * firmware shows is the legacy TFT's boot screen (GUI_LOADING_PAGE, for LoadingPage): any other
   * page is refused with ESP_ERR_NOT_SUPPORTED and logged as a bug. When LVGL is the UI, or the
   * display has no HMI pages, there is nothing to go to: the page shows its view itself with
   * LvglUi::show() (or LoadingPage activates the boot screen), so this does nothing and reports
   * success.
   */
  static esp_err_t go_to_hmi_page(const char *page, uint16_t mutex_timeout);

  /**
   * @brief Keep the loading page up until the panel restarts. Call before LoadingPage::show()
   * when about to reboot, so that a page request arriving meanwhile (the first status at boot,
   * a manager coming back online) cannot cover "Rebooting" or write to a page without t_loading.
   * Never released: the restart ends it.
   */
  static void hold_screen_for_reboot();

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

  /**
   * @brief Show the timer alarm from the page-transition task: the screen goes on at its normal brightness,
   * then TimerPage::show_alarm(). Dropped (the sound still plays) while an update holds the screen.
   * Safe to call from any task; TimerManager::on_fired is set to it in main().
   */
  static void request_timer_alarm();

  /**
   * @brief Keep the screensaver from taking over while the timer alarm is showing. Set by TimerPage when the
   * alarm is shown and cleared when it is dismissed or unshown. Kept apart from _screensaver_blocked, which
   * updates and manager outages also set and clear.
   */
  static void set_alarm_blocks_screensaver(bool blocked);

private:
  // Pages that can be requested from the page-transition task. Only the latest
  // request is shown, so a quick "screen off, screen on" ends with the screen on.
  enum class PageRequest : uint8_t {
    NONE,
    HOME,
    SCREEN_ON, // Screen brightness, then the default page
    SCREENSAVER,
    ENTITY_PAGE, // The entity page for _requested_entity_page_topic
    TIMER_ALARM, // Screen brightness, then TimerPage::show_alarm()
  };

  static void _request_page(PageRequest page);

  // No UI (!LvglUi::active()): the loading page's text, "Display update needed" and what the panel
  // knows (LoadingPage::legacy_secondary_text()). Written only on NSPM's legacy TFT; not while an update holds the screen.
  static void _show_legacy_screen();

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
   * @brief Handle a command received over MQTT and apply new interface manager settings if applicable.
   * Runs on the mqtt_dispatch task.
   */
  static void _mqtt_message_handler(const mqtt::Message &message, void *context);

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

  /**
   * Registered once in init() for every loop and event base pages use (arg is
   * the loop, NULL for the default loop). Passes the event to the current
   * page's PageHandlers.
   */
  static void _page_event_dispatcher(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  // Page transitions requested from event-loop handlers run here instead.
  //
  // This task was added because showing or hiding a page used to register and
  // unregister handlers on both the default event loop and RoomManager's loop,
  // taking each loop's mutex while a handler on the other loop (or the MQTT
  // client task, holding esp-mqtt's API lock) did the same: a lock-order
  // deadlock that froze the UI. Pages no longer touch esp_event when shown
  // (see PageHandlers), so that cycle is gone; the task stays because a page's
  // show() still draws and waits on the display, which is better done off the
  // loop tasks. Handlers notify it with xTaskNotify, which never blocks.
  static void _task_page_transitions(void *param);
  static void _associate_with_manager_if_ready();
  static inline TaskHandle_t _page_transitions_task = NULL;
  static inline std::atomic<PageRequest> _requested_page = PageRequest::NONE;
  // Held by shared_ptr so that MutexWrapped copies it inside its critical section without allocating.
  static inline MutexWrapped<std::shared_ptr<const std::string>> _requested_entity_page_topic;
  static constexpr uint32_t PAGE_TRANSITION_ASSOCIATE = 1 << 0;
  static constexpr uint32_t PAGE_TRANSITION_SHOW_PAGE = 1 << 1;
  // The shown page changed: pick the Wi-Fi power save mode for it (see WiFiManager::set_low_latency()).
  // Done on this task so that every caller of begin_page() stays non-blocking and the mode is always
  // decided from the page that is current when it runs, however the transitions interleave.
  static constexpr uint32_t PAGE_TRANSITION_WIFI_POWER_SAVE = 1 << 2;

  // Vars
  // Queue of unshow function pointer to call from _task_unshow_page
  static inline QueueHandle_t _unshow_queue;

  // In the case of a Nextion "sleep" event, ie. trigger screensaver, should we
  // or is the screensaver blocked?
  static inline MutexWrapped<bool> _screensaver_blocked = false;

  // The timer alarm is showing: the screensaver stays away like it does when _screensaver_blocked is set.
  static inline std::atomic<bool> _alarm_blocks_screensaver = false;

  // A firmware or GUI update is showing its progress on the loading page. Nothing else may
  // take the screen until UPDATE_ENDED_WITHOUT_REBOOT (or FIRMWARE_UPDATE_FAILED) gives it back.
  // Without this, an update that starts while the panel first leaves the loading page at boot
  // ends up behind the default page, with its progress written to a page that has no t_ip.
  // A reboot sets it too (hold_screen_for_reboot()), and nothing clears it then.
  static inline std::atomic<bool> _update_holds_screen = false;

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
  // One COMMAND hold per topic above, in that order (screen_cmd, brightness, screensaver brightness, mode).
  static inline mqtt::Subscription _command_subscriptions[4];
};