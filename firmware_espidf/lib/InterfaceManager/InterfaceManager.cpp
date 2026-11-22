#include <ConfigManager.hpp>
#include <EntitiesPage.hpp>
#include <EntityPage.hpp>
#include <GUI_data.hpp>
#include <HomePage.hpp>
#include <InterfaceManager.hpp>
#include <LoadingPage.hpp>
#include <LvglUi.hpp>
#include <MqttManager.hpp>
#include <MqttSubscriptions.hpp>
#include <iterator>
#include <PanelMetrics.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <Nextion.hpp>
#include <Nextion_event.hpp>
#include <RoomManager.hpp>
#include <RoomManager_event.hpp>
#include <ScreensaverPage.hpp>
#include <TimerPage.hpp>
#include <UpdateManager.hpp>
#include <UpdateManager_event.hpp>
#include <WiFiManager.hpp>
#include <cmath>
#include <cstring>
#include <esp_log.h>
#include <protobuf_nspanel.pb-c.h>

void InterfaceManager::init() {
  esp_log_level_set("InterfaceManager", ConfigManager::log_level);
  InterfaceManager::_unshow_queue = xQueueCreate(8, sizeof(ShownPage *));
  xTaskCreatePinnedToCore(InterfaceManager::_task_unshow_page, "unshow_task", 4096, NULL, 6, NULL, 1);
  // Must exist before any handler below can notify it.
  xTaskCreatePinnedToCore(InterfaceManager::_task_page_transitions, "page_transitions", 8192, NULL, 6, &InterfaceManager::_page_transitions_task, 1);
  esp_err_t nextion_init_result = Nextion::init();
  if (nextion_init_result != ESP_OK) {
    ESP_LOGE("InterfaceManager", "Failed to initialize Nextion display. Will not continue with InterfaceManager!");
    return;
  }
  // On an nspanel-terminal TFT, LVGL draws every page (before the first one is shown). On any other
  // display there is no UI, and no page but the loading page is ever shown (see begin_page()).
  LvglUi::init();

  std::string base_topic = "nspanel/";
  base_topic.append(WiFiManager::mac_string());
  InterfaceManager::_screen_on_off_command_topic = base_topic;
  InterfaceManager::_screen_on_off_state_topic = base_topic;
  InterfaceManager::_screen_brightness_command_topic = base_topic;
  InterfaceManager::_screen_brightness_state_topic = base_topic;
  InterfaceManager::_screensaver_brightness_command_topic = base_topic;
  InterfaceManager::_screensaver_brightness_state_topic = base_topic;
  InterfaceManager::_screensaver_mode_command_topic = base_topic;
  InterfaceManager::_screensaver_mode_state_topic = base_topic;

  InterfaceManager::_screen_on_off_command_topic.append("/screen_cmd");
  InterfaceManager::_screen_on_off_state_topic.append("/screen_state");
  InterfaceManager::_screen_brightness_command_topic.append("/brightness_cmd");
  InterfaceManager::_screen_brightness_state_topic.append("/brightness_state");
  InterfaceManager::_screensaver_brightness_command_topic.append("/brightness_screensaver_cmd");
  InterfaceManager::_screensaver_brightness_state_topic.append("/brightness_screensaver_state");
  InterfaceManager::_screensaver_mode_command_topic.append("/screensaver_mode_cmd");
  InterfaceManager::_screensaver_mode_state_topic.append("/screensaver_mode_state");

  InterfaceManager::_subscribe_to_relevant_mqtt_topics();

  esp_event_handler_register(NEXTION_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_nextion_event_handler, NULL);
  esp_event_handler_register(UPDATEMANAGER_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_update_manager_event_handler, NULL);
  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_nspm_configmanager_event_handler, NULL);
  // Page handlers: registered once, after InterfaceManager's own so they keep
  // running after them, and never unregistered. Keep is_dispatched() in step.
  esp_event_handler_register(NEXTION_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_page_event_dispatcher, NULL);
  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_page_event_dispatcher, NULL);
  if (RoomManager::event_loop() != NULL) {
    esp_event_handler_register_with(RoomManager::event_loop(), ROOMMANAGER_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_page_event_dispatcher, RoomManager::event_loop());
  }
  RoomManager::register_handler(ESP_EVENT_ANY_ID, InterfaceManager::_room_manager_event_handler, NULL);

  // Show boot page
  LoadingPage::show();

  // No SSID configured, the access point has been started through WiFiManager
  // Show text to connect to AP and wait indefinefly. Panel will reboot once settings has been saved.
  // TODO: Perhaps implement flag in WiFi manager for which mode the we are currently in
  // instead of relying on empty WiFi SSID.
  if (ConfigManager::wifi_ssid.empty()) {
    for (;;) {
      std::string primary_text = "Connect to ";
      primary_text.append(ConfigManager::wifi_hostname);
      LoadingPage::set_loading_text(primary_text);

      char ip_address_str[IP4ADDR_STRLEN_MAX];
      esp_netif_ip_info_t ip_info = WiFiManager::ip_info();
      sprintf(ip_address_str, IPSTR, IP2STR(&ip_info.ip));
      LoadingPage::set_secondary_text(ip_address_str);

      vTaskDelay(pdMS_TO_TICKS(1000)); // Update every second
    }
  }

  // Wait for WiFi
  std::string append_string = "";
  while (!WiFiManager::connected()) {
    if (!LvglUi::active()) {
      InterfaceManager::_show_legacy_screen();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    std::string connection_text = "Connecting to ";
    connection_text.append(ConfigManager::wifi_ssid);

    std::string set_string = connection_text;
    set_string.append(append_string);
    LoadingPage::set_loading_text(set_string);
    LoadingPage::set_secondary_text("");

    if (append_string.size() < 3) {
      append_string.append(".");
    } else {
      append_string.clear();
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  // Wait for MQTT
  append_string.clear();
  while (!MqttManager::connected()) {
    if (!LvglUi::active()) {
      InterfaceManager::_show_legacy_screen();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    std::string connection_text = "Connecting to MQTT";
    char ip_address_str[IP4ADDR_STRLEN_MAX];
    esp_netif_ip_info_t ip_info = WiFiManager::ip_info();
    sprintf(ip_address_str, IPSTR, IP2STR(&ip_info.ip));

    std::string set_string = connection_text;
    set_string.append(append_string);
    LoadingPage::set_loading_text(set_string);
    LoadingPage::set_secondary_text(ip_address_str);

    if (append_string.size() < 3) {
      append_string.append(".");
    } else {
      append_string.clear();
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  if (LvglUi::active()) {
    LoadingPage::set_loading_text("Loading config and status");
  } else {
    InterfaceManager::_show_legacy_screen();
  }
  // Listen for commands on nspanel/<mac>/command (reboot, updates, look, ...) before registering: the
  // manager may send one as soon as it accepts the panel, and a command is never replayed. The display
  // and the loading page are up by now, which the reboot and update commands use.
  UpdateManager::init();
  NSPM_ConfigManager::init(); // Register to manager and load all config

  ESP_LOGI("InterfaceManager", "Interface manager init complete. Waiting for config and status to load.");

  // WiFi and MQTT connected.
  // RoomManager will take over and load the config, once the config has been
  // successfully loaded the event handler for RoomManager will take over and send the Nextion display to the correct page
}

std::shared_ptr<PageHandlers> InterfaceManager::begin_page(void (*unshow)()) {
  if (!LvglUi::active() && unshow != LoadingPage::unshow) [[unlikely]] {
    // Every way to a page is closed without a UI (show_default_page(), _task_page_transitions(), the
    // display's events); reaching this is a bug. The page does not become current, so no event reaches it.
    ESP_LOGE("InterfaceManager", "BUG: a page was shown with no UI; it gets no events.");
    std::shared_ptr<PageHandlers> handlers = std::make_shared<PageHandlers>();
    handlers->retire();
    return handlers;
  }
  ShownPage next;
  next.unshow = unshow;
  next.handlers = std::make_shared<PageHandlers>();
  ShownPage *previous = new ShownPage();

  // Only reference counts change in here: the old record stays referenced by
  // *previous, so nothing is freed inside the critical section.
  portENTER_CRITICAL(&InterfaceManager::_current_page_lock);
  *previous = InterfaceManager::_current_page;
  InterfaceManager::_current_page = next;
  portEXIT_CRITICAL(&InterfaceManager::_current_page_lock);

  if (InterfaceManager::_page_transitions_task != NULL) [[likely]] {
    xTaskNotify(InterfaceManager::_page_transitions_task, PAGE_TRANSITION_WIFI_POWER_SAVE, eSetBits);
  }

  if (previous->unshow == nullptr) {
    delete previous; // First page since boot
  } else if (xQueueSend(InterfaceManager::_unshow_queue, &previous, 0) != pdTRUE) {
    // Never block the caller, which is usually an event handler.
    ESP_LOGW("InterfaceManager", "Unshow queue full, unshowing previous page inline.");
    InterfaceManager::_finish_unshow(previous);
  }
  return next.handlers;
}

bool InterfaceManager::is_current_page(void (*unshow)()) {
  portENTER_CRITICAL(&InterfaceManager::_current_page_lock);
  bool current = InterfaceManager::_current_page.unshow == unshow;
  portEXIT_CRITICAL(&InterfaceManager::_current_page_lock);
  return current;
}

bool InterfaceManager::is_dispatched(esp_event_loop_handle_t event_loop, esp_event_base_t event_base) {
  if (event_loop == NULL) {
    return event_base == NEXTION_EVENT || event_base == NSPM_CONFIGMANAGER_EVENT;
  }
  return event_loop == RoomManager::event_loop() && event_base == ROOMMANAGER_EVENT;
}

void InterfaceManager::_page_event_dispatcher(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // Only reference counts change in here, as in begin_page(); the copy keeps
  // the page's handlers alive while they run, even if another page is shown.
  std::shared_ptr<PageHandlers> handlers;
  portENTER_CRITICAL(&InterfaceManager::_current_page_lock);
  handlers = InterfaceManager::_current_page.handlers;
  portEXIT_CRITICAL(&InterfaceManager::_current_page_lock);

  if (handlers) {
    handlers->dispatch(static_cast<esp_event_loop_handle_t>(arg), event_base, event_id, event_data);
  }
}

void InterfaceManager::_finish_unshow(ShownPage *page) {
  // Stops the page accepting handlers from a show() still running; the
  // dispatchers already pass events only to the current page.
  page->handlers->retire();

  if (InterfaceManager::is_current_page(page->unshow)) {
    ESP_LOGD("InterfaceManager", "Page was shown again before its unshow ran; keeping its state.");
  } else {
    page->unshow();
  }
  delete page;
}

void InterfaceManager::hold_screen_for_reboot() {
  InterfaceManager::_update_holds_screen = true;
}

void InterfaceManager::show_default_page() {
  ESP_LOGD("InterfaceManager", "show_default_page called.");
  if (InterfaceManager::_update_holds_screen) {
    ESP_LOGD("InterfaceManager", "An update holds the screen; staying on the loading page.");
    return;
  }
  if (!LvglUi::active()) {
    // No UI: the loading page is the only page, and on the legacy TFT it says so.
    InterfaceManager::_show_legacy_screen();
    return;
  }
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
    switch (config->default_page) {
    case NSPanelConfig__NSPanelDefaultPage::NSPANEL_CONFIG__NSPANEL_DEFAULT_PAGE__HOME: {
      HomePage::show();
      break;
    }

    case NSPanelConfig__NSPanelDefaultPage::NSPANEL_CONFIG__NSPANEL_DEFAULT_PAGE__SCENES: {
      EntitiesPage::show(EntitiesPage::display_type_t::SCENES);
      break;
    }

    case NSPanelConfig__NSPanelDefaultPage::NSPANEL_CONFIG__NSPANEL_DEFAULT_PAGE__ENTITIES: {
      EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
      break;
    }

    default: {
      ESP_LOGE("InterfaceManager", "Unknown default page %ld, will default to home page!", static_cast<uint32_t>(config->default_page));
      HomePage::show();
      break;
    }
    }
  } else {
    ESP_LOGE("InterfaceManager", "Failed to get config while trying to display the default page.");
  }
}

esp_err_t InterfaceManager::go_to_hmi_page(const char *page, uint16_t mutex_timeout) {
  if (LvglUi::active() || LvglUi::display_mode() != LvglUi::DisplayMode::NEXTION) {
    return ESP_OK; // No HMI pages on this display.
  }
  if (strcmp(page, GUI_LOADING_PAGE::page_name) != 0) [[unlikely]] {
    // The legacy TFT's boot screen is the only HMI page this firmware shows.
    ESP_LOGE("InterfaceManager", "BUG: HMI page %s requested; only %s is shown.", page, GUI_LOADING_PAGE::page_name);
    return ESP_ERR_NOT_SUPPORTED;
  }
  return Nextion::go_to_page(page, mutex_timeout);
}

void InterfaceManager::_show_legacy_screen() {
  if (InterfaceManager::_update_holds_screen) {
    return; // "Updating firmware.", "Rebooting" and the like stay.
  }
  std::string name;
  std::shared_ptr<NSPanelConfig> config;
  if (InterfaceManager::_nspm_config_loaded && NSPM_ConfigManager::get_config(&config) == ESP_OK && config && config->name != nullptr) {
    name = config->name;
  }
  std::string ip = WiFiManager::connected() ? WiFiManager::ip_string() : "";
  LoadingPage::set_loading_text(LoadingPage::LEGACY_TEXT);
  LoadingPage::set_secondary_text(LoadingPage::legacy_secondary_text(name, ip));
}

void InterfaceManager::request_show_home_page() {
  InterfaceManager::_request_page(PageRequest::HOME);
}

void InterfaceManager::request_show_entity_page(const std::string &state_topic) {
  // Set before the request, so the page-transition task always sees the topic of the latest request.
  InterfaceManager::_requested_entity_page_topic.set(std::make_shared<const std::string>(state_topic));
  InterfaceManager::_request_page(PageRequest::ENTITY_PAGE);
}

void InterfaceManager::request_timer_alarm() {
  InterfaceManager::_request_page(PageRequest::TIMER_ALARM);
}

void InterfaceManager::set_alarm_blocks_screensaver(bool blocked) {
  InterfaceManager::_alarm_blocks_screensaver = blocked;
}

void InterfaceManager::_show_requested_entity_page() {
  std::shared_ptr<const std::string> topic = InterfaceManager::_requested_entity_page_topic.get();
  if (topic == nullptr || topic->empty()) [[unlikely]] {
    ESP_LOGE("InterfaceManager", "Entity page requested without a state topic.");
    return;
  }
  if (LoadingPage::showing()) {
    // Still loading, updating, or waiting for the manager to come back: the loading page must stay.
    ESP_LOGW("InterfaceManager", "Not opening the entity page for %s while the loading page is shown.", topic->c_str());
    return;
  }

  if (ScreensaverPage::showing()) {
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
      Nextion::set_brightness_level(config->screen_dim_level, 1000);
    } else {
      ESP_LOGE("InterfaceManager", "Failed to get config while waking the screen for an entity page.");
    }
    MqttManager::publish(InterfaceManager::_screen_on_off_state_topic, "1", strlen("1"), true);
  }

  if (InterfaceManager::is_current_page(EntityPage::unshow)) {
    if (EntityPage::get_current_state_topic() == *topic) {
      ESP_LOGD("InterfaceManager", "Entity page for %s is already showing.", topic->c_str());
      return;
    }
    // EntityPage only calls begin_page() when the new entity's state arrives. The page is still current
    // then, so begin_page() would skip the old entity's unshow: unshow it here instead.
    EntityPage::unshow();
  }
  EntityPage::show(*topic);
}

void InterfaceManager::_request_page(PageRequest page) {
  InterfaceManager::_requested_page = page;
  xTaskNotify(InterfaceManager::_page_transitions_task, PAGE_TRANSITION_SHOW_PAGE, eSetBits);
}

void InterfaceManager::_task_page_transitions(void *param) {
  for (;;) {
    uint32_t requested = 0;
    xTaskNotifyWait(0, UINT32_MAX, &requested, portMAX_DELAY);
    if (requested & PAGE_TRANSITION_ASSOCIATE) {
      InterfaceManager::_associate_with_manager_if_ready();
    }
    if (requested & PAGE_TRANSITION_SHOW_PAGE && InterfaceManager::_update_holds_screen) {
      ESP_LOGD("InterfaceManager", "An update holds the screen; dropping page request %d.", static_cast<int>(InterfaceManager::_requested_page.exchange(PageRequest::NONE)));
    } else if (requested & PAGE_TRANSITION_SHOW_PAGE && !LvglUi::active()) {
      // No UI, so no pages to show. Screen on (also after the display restarted) brings the loading page back.
      PageRequest request = InterfaceManager::_requested_page.exchange(PageRequest::NONE);
      if (request == PageRequest::SCREEN_ON) {
        std::shared_ptr<NSPanelConfig> config;
        if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
          Nextion::set_brightness_level(config->screen_dim_level, 1000);
        }
        LoadingPage::redraw();
        InterfaceManager::show_default_page();
      } else if (request != PageRequest::NONE) {
        ESP_LOGD("InterfaceManager", "No UI; dropping page request %d.", static_cast<int>(request));
      }
    } else if (requested & PAGE_TRANSITION_SHOW_PAGE) {
      switch (InterfaceManager::_requested_page.exchange(PageRequest::NONE)) {
      case PageRequest::HOME:
        HomePage::show();
        break;

      case PageRequest::SCREEN_ON: {
        std::shared_ptr<NSPanelConfig> config;
        if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
          Nextion::set_brightness_level(config->screen_dim_level, 1000);
          InterfaceManager::show_default_page();
        } else {
          ESP_LOGE("InterfaceManager", "Failed to get config while trying to turn screen on.");
        }
        break;
      }

      case PageRequest::SCREENSAVER:
        ScreensaverPage::show();
        break;

      case PageRequest::ENTITY_PAGE:
        InterfaceManager::_show_requested_entity_page();
        break;

      case PageRequest::TIMER_ALARM: {
        std::shared_ptr<NSPanelConfig> config;
        if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
          Nextion::set_brightness_level(config->screen_dim_level, 1000);
        } else {
          ESP_LOGE("InterfaceManager", "Failed to get config while trying to turn the screen on for the timer alarm.");
        }
        // TimerManager's lists belong to LvglUi's task, which is not this one.
        LvglUi::defer([]() { TimerPage::show_alarm(); });
        break;
      }

      case PageRequest::NONE:
        break; // Already handled by an earlier wake-up
      }
    }
    if (requested & PAGE_TRANSITION_WIFI_POWER_SAVE) {
      // Keep the radio awake while someone may be using the panel, and let it sleep between
      // beacons while the screensaver is shown. Read the current page here rather than trusting
      // the page that sent the notification: a later switch may already have replaced it.
      WiFiManager::set_low_latency(!ScreensaverPage::showing());
    }
  }
}

void InterfaceManager::_associate_with_manager_if_ready() {
  // First time both the home page status and the config are loaded: leave the
  // loading page for the default page. Re-checked here because several
  // notifications can be coalesced into one wake-up.
  if (InterfaceManager::_home_page_status_loaded && InterfaceManager::_nspm_config_loaded && LoadingPage::showing() && !InterfaceManager::_has_been_associated_with_manager) {
    if (!ConfigManager::has_updated && !LvglUi::active()) {
      // No UI: the panel keeps the loading page; the screensaver, its tasks and the rooms' pages never start.
      ESP_LOGI("InterfaceManager", "Home page status and base config loaded; no UI on this display.");
      InterfaceManager::show_default_page();
      InterfaceManager::_has_been_associated_with_manager = true;
    } else if (!ConfigManager::has_updated) [[likely]] {
      ESP_LOGI("InterfaceManager", "Home page status and base config loaded.");
      RoomManager::go_to_default_room();
      RoomManager::go_to_first_entities_page(); // Also select the first entities page for default room as this is the first time and no room is currently selected.

      if (!ScreensaverPage::showing()) {
        ESP_LOGD("InterfaceManager", "Screensaver is not active and new status received. Show default page.");
        InterfaceManager::show_default_page();
      }

      // Initialize Screensaver page so that it's ready when it's time to show it.
      ScreensaverPage::init();
      InterfaceManager::_has_been_associated_with_manager = true;
    } else {
      LoadingPage::set_loading_text("Updated checksums, rebooting.");
    }
  }
}

void InterfaceManager::_task_unshow_page(void *param) {
  ShownPage *page;
  for (;;) {
    if (xQueueReceive(InterfaceManager::_unshow_queue, &page, portMAX_DELAY) == pdTRUE) {
      InterfaceManager::_finish_unshow(page);
    }
  }
}

void InterfaceManager::_nextion_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (!LvglUi::active() && (event_id == nextion_event_t::SLEEP_EVENT || event_id == nextion_event_t::WAKE_EVENT)) {
    // Only the legacy TFT's own code sends these, and there is no screensaver or page to go to.
    ESP_LOGD("InterfaceManager", "No UI; ignoring the display's %s event.", event_id == nextion_event_t::SLEEP_EVENT ? "sleep" : "wake");
    return;
  }
  switch (event_id) {
  case nextion_event_t::SLEEP_EVENT: {
    if (InterfaceManager::_alarm_blocks_screensaver) {
      ESP_LOGD("InterfaceManager", "Got sleep event while the timer alarm is showing; the screensaver stays away.");
    } else if (!InterfaceManager::_screensaver_blocked.get()) {
      ESP_LOGD("InterfaceManager", "Got sleep event from InterfaceManager and screensaver is not blocked. Will switch to screensaver page.");
      ScreensaverPage::show();
    }
    if (!InterfaceManager::_alarm_blocks_screensaver) { // The screen stays on for the alarm
      MqttManager::publish(InterfaceManager::_screen_on_off_state_topic, "0", strlen("0"), true);
    }
    break;
  }

  case nextion_event_t::WAKE_EVENT: {
    // Someone touched the screensaver, unshow it and go to the default page, whatever is selected in the manager
    InterfaceManager::show_default_page();
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
      Nextion::set_brightness_level(config->screen_dim_level, 1000);
    } else {
      ESP_LOGE("InterfaceManager", "Failed to get NSPanel Config when unshowing screensaver page!");
    }
    MqttManager::publish(InterfaceManager::_screen_on_off_state_topic, "1", strlen("1"), true);
    break;
  }

  case nextion_event_t::DISPLAY_RESTARTED: {
    // The display is back on its startup page at its default brightness. Restore both through the
    // page-transition task, which may switch pages; this handler must not.
    ESP_LOGW("InterfaceManager", "Display restarted, showing the default page again.");
    InterfaceManager::_request_page(PageRequest::SCREEN_ON);
    break;
  }

  default:
    break;
  }
}

void InterfaceManager::_update_manager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case updatemanager_event_t::FIRMWARE_UPDATE_STARTED: {
    InterfaceManager::_update_holds_screen = true;
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
      Nextion::set_brightness_level(config->screen_dim_level, 1000);
    }
    // If screensaver blocked is false we are currently not showing loading page. Show it.
    if (!InterfaceManager::_screensaver_blocked.get()) {
      LoadingPage::show();
      InterfaceManager::_screensaver_blocked.set(true);
    }
    LoadingPage::set_loading_text("Updating firmware.");
    LoadingPage::set_secondary_text("0%");
    break;
  }

  case updatemanager_event_t::FIRMWARE_UPDATE_PROGRESS: {
    float *progress_percentage = (float *)event_data;
    std::string percent_string = std::to_string(static_cast<int>(std::round(*progress_percentage)));
    percent_string.append("%");
    LoadingPage::set_secondary_text(percent_string.c_str());
    break;
  }

  case updatemanager_event_t::FIRMWARE_UPDATE_FINISHED: {
    LoadingPage::set_loading_text("Firmware update complete.");
    LoadingPage::set_secondary_text("100%");
    break;
  }

  case updatemanager_event_t::FIRMWARE_UPDATE_FAILED:
  case updatemanager_event_t::UPDATE_ENDED_WITHOUT_REBOOT: {
    // The running firmware is untouched, go back to normal operation. Only if an update took the
    // screen: the boot-time check that finds nothing to update must not leave the loading page early.
    if (InterfaceManager::_update_holds_screen.exchange(false)) {
      InterfaceManager::_screensaver_blocked = false;
      InterfaceManager::show_default_page();
    }
    break;
  }

  default:
    break;
  }
}

void InterfaceManager::_nspm_configmanager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case nspm_configmanager_event::CONFIG_LOADED: {
    InterfaceManager::_nspm_config_loaded = true;
    xTaskNotify(InterfaceManager::_page_transitions_task, PAGE_TRANSITION_ASSOCIATE, eSetBits);

    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
      // LvglUi keeps the screensaver timeout itself, from the config.
      if (!LvglUi::active()) {
        InterfaceManager::show_default_page(); // The legacy screen, with the panel's name now known.
      }

      if (!ScreensaverPage::showing()) {
        Nextion::set_brightness_level(config->screen_dim_level, 1000);
      }

      // Update MQTT state topics
      std::string screen_brightness = std::to_string(config->screen_dim_level);
      std::string screensaver_brightness = std::to_string(config->screensaver_dim_level);
      MqttManager::publish(std::string(InterfaceManager::_screen_brightness_state_topic), screen_brightness.c_str(), screen_brightness.size(), true);
      MqttManager::publish(std::string(InterfaceManager::_screensaver_brightness_state_topic), screensaver_brightness.c_str(), screensaver_brightness.size(), true);

      switch (config->screensaver_mode) {
      case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITH_BACKGROUND:
        MqttManager::publish(InterfaceManager::_screensaver_mode_state_topic, "with_background", strlen("with_background"), true);
        break;

      case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITHOUT_BACKGROUND:
        MqttManager::publish(InterfaceManager::_screensaver_mode_state_topic, "without_background", strlen("without_background"), true);
        break;

      case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITH_BACKGROUND:
        MqttManager::publish(InterfaceManager::_screensaver_mode_state_topic, "datetime_with_background", strlen("datetime_with_background"), true);
        break;

      case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITHOUT_BACKGROUND:
        MqttManager::publish(InterfaceManager::_screensaver_mode_state_topic, "datetime_without_background", strlen("datetime_without_background"), true);
        break;

      case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER:
        MqttManager::publish(InterfaceManager::_screensaver_mode_state_topic, "no_screensaver", strlen("no_screensaver"), true);
        break;

      default:
        ESP_LOGE("InterfaceManager", "Unknown screensaver mode. Will not send state update.");
        break;
      }

      // Go to new default page if it has changed
      if (InterfaceManager::_nspm_cur_config != nullptr) {
        if (InterfaceManager::_nspm_cur_config->default_page != config->default_page && !ScreensaverPage::showing()) {
          ESP_LOGD("InterfaceManager", "Navigating to new default page.");
          InterfaceManager::show_default_page();
        }
      }

      InterfaceManager::_nspm_cur_config = config;
    } else {
      ESP_LOGW("InterfaceManager", "Failed to get config when received new config. Will not be able to update screensaver timeout!");
    }
    break;
  }

  case nspm_configmanager_event::MANAGER_STATE_CHANGE: {
    if (InterfaceManager::_update_holds_screen) {
      ESP_LOGD("InterfaceManager", "Manager state changed during an update; the update keeps the screen.");
    } else if (!LvglUi::active()) {
      InterfaceManager::show_default_page(); // No UI: the loading page stays, whatever the manager does.
    } else if (NSPM_ConfigManager::get_manager_online()) {
      // Manager became online again after being offline.
      InterfaceManager::_screensaver_blocked = false;

      std::shared_ptr<NSPanelConfig> config;
      if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
        ESP_LOGD("InterfaceManager", "Manager became online and valid config exists. Showing screensaver page.");
        ScreensaverPage::show();
      } else {
        ESP_LOGE("InterfaceManager", "Failed to get NSPanel Config when unshowing screensaver page!");
      }
    } else {
      LoadingPage::show();
      LoadingPage::set_loading_text("Lost connection to manager.");
      LoadingPage::set_secondary_text("");
      InterfaceManager::_screensaver_blocked = true;
    }
    break;
  }

  default:
    break;
  }
}

void InterfaceManager::_room_manager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // Received event that all rooms has been loaded AND we are currently on the loading page.
  // This should only happen on first boot of panel, force navigate to default page.
  if (event_id == roommanager_event_t::HOME_PAGE_UPDATED) {
    InterfaceManager::_home_page_status_loaded = true;
    // Never switch pages here, on the RoomManager loop: see _task_page_transitions.
    xTaskNotify(InterfaceManager::_page_transitions_task, PAGE_TRANSITION_ASSOCIATE, eSetBits);
  }
}

void InterfaceManager::_mqtt_message_handler(const mqtt::Message &message, void *context) {
  if (message.payload.empty()) {
    return;
  }

  std::string topic_string(message.topic);
  std::string data(reinterpret_cast<const char *>(message.payload.data()), message.payload.size());

  if (topic_string.compare(InterfaceManager::_screen_brightness_command_topic) == 0) {
    std::shared_ptr<NSPanelConfig> mutable_config;
    if (NSPM_ConfigManager::get_mutable_config(&mutable_config) == ESP_OK) [[likely]] {
      uint8_t new_brightness = std::stoi(data);
      if (new_brightness > 100) [[unlikely]] { // Clamp value to a max of 100%
        new_brightness = 100;
      }
      mutable_config->screen_dim_level = new_brightness;
      if (!ScreensaverPage::showing()) { // We are not on screensaver, update screen brightness
        Nextion::set_brightness_level(new_brightness, 1000);
      }
      std::string send_data = std::to_string(new_brightness).c_str();
      MqttManager::publish(std::string(InterfaceManager::_screen_brightness_state_topic), data.c_str(), send_data.length(), true);
      NSPM_ConfigManager::replace_config(&mutable_config);
    } else {
      ESP_LOGE("InterfaceManager", "Failed to get mutable config while trying to process config update data from MQTT topic %s.", topic_string.c_str());
    }
  } else if (topic_string.compare(InterfaceManager::_screensaver_brightness_command_topic) == 0) {
    std::shared_ptr<NSPanelConfig> mutable_config;
    if (NSPM_ConfigManager::get_mutable_config(&mutable_config) == ESP_OK) [[likely]] {
      uint8_t new_brightness = std::stoi(data);
      if (new_brightness > 100) [[unlikely]] { // Clamp value to a max of 100%
        new_brightness = 100;
      }
      mutable_config->screensaver_dim_level = new_brightness;
      if (ScreensaverPage::showing()) { // We are not on screensaver, update screen brightness
        Nextion::set_brightness_level(new_brightness, 1000);
      }
      std::string send_data = std::to_string(new_brightness).c_str();
      MqttManager::publish(std::string(InterfaceManager::_screensaver_brightness_state_topic), data.c_str(), send_data.length(), true);
      NSPM_ConfigManager::replace_config(&mutable_config);
    } else {
      ESP_LOGE("InterfaceManager", "Failed to get mutable config while trying to process config update data from MQTT topic %s.", topic_string.c_str());
    }
  } else if (topic_string.compare(InterfaceManager::_screen_on_off_command_topic) == 0) {
    // Never switch pages here, on the mqtt_dispatch task: see _task_page_transitions.
    if (data.compare("0") == 0) {
      InterfaceManager::_request_page(PageRequest::SCREENSAVER);
      MqttManager::publish(InterfaceManager::_screen_on_off_state_topic, "0", strlen("0"), true);
    } else if (data.compare("1") == 0) {
      InterfaceManager::_request_page(PageRequest::SCREEN_ON);
      MqttManager::publish(InterfaceManager::_screen_on_off_state_topic, "1", strlen("1"), true);
    } else {
      ESP_LOGE("InterfaceManager", "Got request to turn screen on/off but got unknown data: '%s'. Valid data is 0 or 1", data.c_str());
    }
  } else if (topic_string.compare(InterfaceManager::_screensaver_mode_command_topic) == 0) {
    std::shared_ptr<NSPanelConfig> mutable_config;
    if (NSPM_ConfigManager::get_mutable_config(&mutable_config) == ESP_OK) [[likely]] {
      if (data.compare("with_background") == 0) {
        mutable_config->screensaver_mode = NSPanelConfig__NSPanelScreensaverMode::NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITH_BACKGROUND;
        NSPM_ConfigManager::replace_config(&mutable_config);
      } else if (data.compare("without_background") == 0) {
        mutable_config->screensaver_mode = NSPanelConfig__NSPanelScreensaverMode::NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITHOUT_BACKGROUND;
        NSPM_ConfigManager::replace_config(&mutable_config);
      } else if (data.compare("datetime_with_background") == 0) {
        mutable_config->screensaver_mode = NSPanelConfig__NSPanelScreensaverMode::NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITH_BACKGROUND;
        NSPM_ConfigManager::replace_config(&mutable_config);
      } else if (data.compare("datetime_without_background") == 0) {
        mutable_config->screensaver_mode = NSPanelConfig__NSPanelScreensaverMode::NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITHOUT_BACKGROUND;
        NSPM_ConfigManager::replace_config(&mutable_config);
      } else if (data.compare("no_screensaver") == 0) {
        mutable_config->screensaver_mode = NSPanelConfig__NSPanelScreensaverMode::NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER;
        NSPM_ConfigManager::replace_config(&mutable_config);
      } else {
        ESP_LOGE("InterfaceManager", "Got request to update screensaver mode to '%s' but that is not a valid screensaver mode!", data.c_str());
      }
    } else {
      ESP_LOGE("InterfaceManager", "Failed to get mutable config while trying to process config update data from MQTT topic %s.", topic_string.c_str());
    }
  }
}

void InterfaceManager::_subscribe_to_relevant_mqtt_topics() {
  // All commands: each is delivered once, in order, and never replayed. One hold per topic, replaced if init runs again.
  const std::string *topics[] = {&InterfaceManager::_screen_on_off_command_topic, &InterfaceManager::_screen_brightness_command_topic, &InterfaceManager::_screensaver_brightness_command_topic,
                                 &InterfaceManager::_screensaver_mode_command_topic};
  static_assert(std::size(topics) == std::size(InterfaceManager::_command_subscriptions));
  for (size_t i = 0; i < std::size(topics); i++) {
    InterfaceManager::_command_subscriptions[i] = mqtt::subscribe(*topics[i], mqtt::Kind::COMMAND, &InterfaceManager::_mqtt_message_handler, NULL, PanelMetrics::HANDLER_INTERFACE_MANAGER);
  }
}
