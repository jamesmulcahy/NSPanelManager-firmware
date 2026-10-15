#include <ConfigManager.hpp>
#include <EntitiesPage.hpp>
#include <GUI_data.hpp>
#include <HomePage.hpp>
#include <InterfaceManager.hpp>
#include <LoadingPage.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <Nextion.hpp>
#include <Nextion_event.hpp>
#include <RoomManager.hpp>
#include <RoomManager_event.hpp>
#include <ScreensaverPage.hpp>
#include <UpdateManager_event.hpp>
#include <WiFiManager.hpp>
#include <cmath>
#include <esp_log.h>
#include <protobuf_nspanel.pb-c.h>

void InterfaceManager::init() {
  esp_log_level_set("InterfaceManager", ConfigManager::log_level);
  current_page_unshow_callback.set(NULL);
  InterfaceManager::_unshow_queue = xQueueCreate(4, sizeof(std::function<void()>));
  // Must exist before any handler below can notify it.
  xTaskCreatePinnedToCore(InterfaceManager::_task_page_transitions, "page_transitions", 8192, NULL, 6, &InterfaceManager::_page_transitions_task, 1);
  esp_err_t nextion_init_result = Nextion::init();
  if (nextion_init_result != ESP_OK) {
    ESP_LOGE("InterfaceManager", "Failed to initialize Nextion display. Will not continue with InterfaceManager!");
    return;
  }

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
  InterfaceManager::_screen_raw_commands = base_topic;

  InterfaceManager::_screen_on_off_command_topic.append("/screen_cmd");
  InterfaceManager::_screen_on_off_state_topic.append("/screen_state");
  InterfaceManager::_screen_brightness_command_topic.append("/brightness_cmd");
  InterfaceManager::_screen_brightness_state_topic.append("/brightness_state");
  InterfaceManager::_screensaver_brightness_command_topic.append("/brightness_screensaver_cmd");
  InterfaceManager::_screensaver_brightness_state_topic.append("/brightness_screensaver_state");
  InterfaceManager::_screensaver_mode_command_topic.append("/screensaver_mode_cmd");
  InterfaceManager::_screensaver_mode_state_topic.append("/screensaver_mode_state");
  InterfaceManager::_screen_raw_commands.append("/screen_raw_command");

  InterfaceManager::_subscribe_to_relevant_mqtt_topics();

  esp_event_handler_register(NEXTION_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_nextion_event_handler, NULL);
  esp_event_handler_register(UPDATEMANAGER_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_update_manager_event_handler, NULL);
  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, ESP_EVENT_ANY_ID, InterfaceManager::_nspm_configmanager_event_handler, NULL);
  MqttManager::register_handler(MQTT_EVENT_ANY, &InterfaceManager::_mqtt_event_handler, NULL);
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

  LoadingPage::set_loading_text("Loading config and status");
  NSPM_ConfigManager::init(); // Register to manager and load all config

  ESP_LOGI("InterfaceManager", "Interface manager init complete. Waiting for config and status to load.");

  // WiFi and MQTT connected.
  // RoomManager will take over and load the config, once the config has been
  // successfully loaded the event handler for RoomManager will take over and send the Nextion display to the correct page
}

void InterfaceManager::call_unshow_callback() {
  auto unshow_handle = InterfaceManager::current_page_unshow_callback.get();
  if (unshow_handle != nullptr) {
    xQueueSend(InterfaceManager::_unshow_queue, &unshow_handle, pdMS_TO_TICKS(500));
    xTaskCreatePinnedToCore(InterfaceManager::_task_unshow_page, "unshow_task", 4096, NULL, 6, NULL, 1);
  }
}

void InterfaceManager::show_default_page() {
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

void InterfaceManager::request_show_home_page() {
  xTaskNotify(InterfaceManager::_page_transitions_task, PAGE_TRANSITION_SHOW_HOME, eSetBits);
}

void InterfaceManager::_task_page_transitions(void *param) {
  for (;;) {
    uint32_t requested = 0;
    xTaskNotifyWait(0, UINT32_MAX, &requested, portMAX_DELAY);
    if (requested & PAGE_TRANSITION_ASSOCIATE) {
      InterfaceManager::_associate_with_manager_if_ready();
    }
    if (requested & PAGE_TRANSITION_SHOW_HOME) {
      HomePage::show();
    }
  }
}

void InterfaceManager::_associate_with_manager_if_ready() {
  // First time both the home page status and the config are loaded: leave the
  // loading page for the default page. Re-checked here because several
  // notifications can be coalesced into one wake-up.
  if (InterfaceManager::_home_page_status_loaded && InterfaceManager::_nspm_config_loaded && LoadingPage::showing() && !InterfaceManager::_has_been_associated_with_manager) {
    if (!ConfigManager::has_updated) [[likely]] {
      ESP_LOGI("InterfaceManager", "Home page status and base config loaded. Will go to home page on default room.");
      RoomManager::go_to_default_room();
      RoomManager::go_to_first_entities_page(); // Also select the first entities page for default room as this is the first time and no room is currently selected.
      InterfaceManager::show_default_page();

      // Initialize Screensaver page so that it's ready when it's time to show it.
      ScreensaverPage::init();
      InterfaceManager::_has_been_associated_with_manager = true;
    } else {
      LoadingPage::set_loading_text("Updated checksums, rebooting.");
    }
  }
}

void InterfaceManager::_task_unshow_page(void *param) {
  std::function<void()> unshow_handle;
  while (xQueueReceive(InterfaceManager::_unshow_queue, &unshow_handle, pdMS_TO_TICKS(250)) == pdPASS) {
    unshow_handle();
  }
  vTaskDelete(NULL);
}

void InterfaceManager::_nextion_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case nextion_event_t::SLEEP_EVENT: {
    if (!InterfaceManager::_screensaver_blocked.get()) {
      ESP_LOGD("InterfaceManager", "Got sleep event from InterfaceManager and screensaver is not blocked. Will switch to screensaver page.");
      ScreensaverPage::show();
    }
    MqttManager::publish(InterfaceManager::_screen_on_off_state_topic, "0", strlen("0"), true);
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

  default:
    break;
  }
}

void InterfaceManager::_update_manager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case updatemanager_event_t::FIRMWARE_UPDATE_STARTED: {
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

  case updatemanager_event_t::LITTLEFS_UPDATE_STARTED: {
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
      Nextion::set_brightness_level(config->screen_dim_level, 1000);
    }
    // If screensaver blocked is false we are currently not showing loading page. Show it.
    if (!InterfaceManager::_screensaver_blocked.get()) {
      LoadingPage::show();
      InterfaceManager::_screensaver_blocked.set(true);
    }
    LoadingPage::set_loading_text("Updating LittleFS.");
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

  case updatemanager_event_t::LITTLEFS_UPDATE_PROGRESS: {
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

  case updatemanager_event_t::FIRMWARE_UPDATE_FAILED: {
    // The running firmware is untouched, go back to normal operation.
    InterfaceManager::_screensaver_blocked = false;
    InterfaceManager::show_default_page();
    break;
  }

  case updatemanager_event_t::LITTLEFS_UPDATE_FINISHED: {
    LoadingPage::set_loading_text("LittleFS update complete.");
    LoadingPage::set_secondary_text("100%");
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
      if (InterfaceManager::_nspm_cur_config == nullptr || InterfaceManager::_nspm_cur_config->screensaver_activation_timeout != config->screensaver_activation_timeout) {
        if (Nextion::set_timer_value(GUI_HOME_PAGE::timer_screensaver_name, config->screensaver_activation_timeout, pdMS_TO_TICKS(250)) != ESP_OK) {
          ESP_LOGE("InterfaceManager", "Failed to update timer value for screensaver timeout.");
        }
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
    if (NSPM_ConfigManager::get_manager_online()) {
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

void InterfaceManager::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == MQTT_EVENT_DATA) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event->data_len == 0) {
      return;
    }

    std::string topic_string = std::string(event->topic, event->topic_len);
    std::string data = std::string(event->data, event->data_len);
    // esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_DATA: {
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
        std::shared_ptr<NSPanelConfig> config;
        if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
          if (data.compare("0") == 0) {
            ScreensaverPage::show();
            MqttManager::publish(InterfaceManager::_screen_on_off_state_topic, "0", strlen("0"), true);
          } else if (data.compare("1") == 0) {
            Nextion::set_brightness_level(config->screen_dim_level, 1000);
            InterfaceManager::show_default_page();
            MqttManager::publish(InterfaceManager::_screen_on_off_state_topic, "1", strlen("1"), true);
          } else {
            ESP_LOGE("InterfaceManager", "Got request to turn screen on/off but got unknown data: '%s'. Valid data is 0 or 1", data.c_str());
          }
        } else {
          ESP_LOGE("InterfaceManager", "Failed to get config while trying to turn screen on or off.");
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
      } else if (topic_string.compare(InterfaceManager::_screen_raw_commands) == 0) {
        ESP_LOGD("InterfaceManager", "Writing raw Nextion command to display. Command: %s", data.c_str());
        Nextion::send_raw_command(data.c_str(), 5000); // Try writing raw command data to the Nextion display.
      }
      break;
    }

    default:
      break;
    }
  }
}

void InterfaceManager::_subscribe_to_relevant_mqtt_topics() {
  MqttManager::subscribe(InterfaceManager::_screen_on_off_command_topic);
  MqttManager::subscribe(InterfaceManager::_screen_brightness_command_topic);
  MqttManager::subscribe(InterfaceManager::_screensaver_brightness_command_topic);
  MqttManager::subscribe(InterfaceManager::_screensaver_mode_command_topic);
  MqttManager::subscribe(InterfaceManager::_screen_raw_commands);
}