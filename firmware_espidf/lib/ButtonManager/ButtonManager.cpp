
#include <ButtonManager.hpp>
#include <ConfigManager.hpp>
#include <InterfaceManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <StatusUpdateManager_events.hpp>
#include <WiFiManager.hpp>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <vector>

void ButtonManager::init() {
  esp_log_level_set("ButtonManager", ConfigManager::log_level);

  std::string topic_base = std::string("nspanel/") + WiFiManager::mac_string();
  ButtonManager::_relay1_cmd_topic = topic_base + "/relay1_cmd";
  ButtonManager::_relay2_cmd_topic = topic_base + "/relay2_cmd";
  ButtonManager::_relay1_state_topic = topic_base + "/relay1_state";
  ButtonManager::_relay2_state_topic = topic_base + "/relay2_state";

  ButtonManager::_interrupt_queue = xQueueCreate(4, sizeof(uint32_t));

  ButtonManager::_reverse_relays = ConfigManager::reverse_relays;
  ButtonManager::_relay1_default_mode = ConfigManager::relay1_default_mode;
  ButtonManager::_relay2_default_mode = ConfigManager::relay2_default_mode;
  ButtonManager::_min_button_push_time = ConfigManager::min_button_push_time;
  ButtonManager::_min_button_long_push_time = ConfigManager::min_button_long_push_time;

  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, ButtonManager::_nspm_configmanager_event_handler, NULL);
  esp_event_handler_register(STATUSUPDATEMANAGER_EVENT, statusupdatemanagerevent_t::AVERAGE_TEMP_UPDATE, ButtonManager::_new_temperature_event, NULL);

  ButtonManager::_relay_io_config = {};
  ButtonManager::_relay_io_config.intr_type = GPIO_INTR_DISABLE;
  ButtonManager::_relay_io_config.pin_bit_mask = ButtonManager::_relay_pin_mask;
  ButtonManager::_relay_io_config.mode = GPIO_MODE_OUTPUT;
  ButtonManager::_relay_io_config.pull_up_en = gpio_pullup_t::GPIO_PULLUP_DISABLE;
  if (gpio_config(&ButtonManager::_relay_io_config) != ESP_OK) [[unlikely]] {
    ESP_LOGE("ButtonManager", "Failed to configure IO for relays.");
  }

  ButtonManager::_set_relay_state(1, ButtonManager::_relay1_default_mode, false); // Do not set MQTT update as MQTT is not connected or initialized when this is called
  ButtonManager::_set_relay_state(2, ButtonManager::_relay2_default_mode, false); // Do not set MQTT update as MQTT is not connected or initialized when this is called

  // Init button1
  if (ButtonManager::_button1 != nullptr) {
    delete ButtonManager::_button1;
    ButtonManager::_button1 = nullptr;
  }
  ButtonManager::_button1 = new InterruptButton(ButtonManager::_button1_pin, 0, GPIO_MODE_INPUT, ButtonManager::_min_button_long_push_time, 250, ButtonManager::_min_button_push_time);
  ButtonManager::_button1->bind(Event_KeyDown, &ButtonManager::_button1_key_down);
  ButtonManager::_button1->bind(Event_KeyUp, &ButtonManager::_button1_key_up);
  ButtonManager::_button1->bind(Event_KeyPress, &ButtonManager::_button1_press);

  // Init button2
  if (ButtonManager::_button2 != nullptr) {
    delete ButtonManager::_button2;
    ButtonManager::_button2 = nullptr;
  }
  ButtonManager::_button2 = new InterruptButton(ButtonManager::_button2_pin, 0, GPIO_MODE_INPUT, ButtonManager::_min_button_long_push_time, 250, ButtonManager::_min_button_push_time);
  ButtonManager::_button2->bind(Event_KeyDown, &ButtonManager::_button2_key_down);
  ButtonManager::_button2->bind(Event_KeyUp, &ButtonManager::_button2_key_up);
  ButtonManager::_button2->bind(Event_KeyPress, &ButtonManager::_button2_press);

  InterruptButton::setMenuCount(0);
  InterruptButton::setMenuLevel(0);
  InterruptButton::setMode(Mode_Asynchronous);
}

void ButtonManager::init_mqtt() {
  // Setup and subscribe to MQTT
  MqttManager::register_handler(MQTT_EVENT_ANY, &ButtonManager::_mqtt_event_handler, NULL);

  MqttManager::subscribe(ButtonManager::_relay1_cmd_topic);
  MqttManager::subscribe(ButtonManager::_relay2_cmd_topic);
}

void ButtonManager::_button1_key_down(void) {
  if (ButtonManager::_button1_mode == NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__FOLLOW) {
    ButtonManager::_set_relay_state(1, !ButtonManager::_get_relay_state(1), true); // When button is pressed, activate relay
  }
}

void ButtonManager::_button1_key_up(void) {
  if (ButtonManager::_button1_mode == NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__FOLLOW) {
    ButtonManager::_set_relay_state(1, !ButtonManager::_get_relay_state(1), true); // When button is pressed, activate relay
  }
}

void ButtonManager::_button2_key_down(void) {
  if (ButtonManager::_button2_mode == NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__FOLLOW) {
    ButtonManager::_set_relay_state(2, !ButtonManager::_get_relay_state(2), true); // When button is pressed, activate relay
  }
}

void ButtonManager::_button2_key_up(void) {
  if (ButtonManager::_button2_mode == NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__FOLLOW) {
    ButtonManager::_set_relay_state(2, !ButtonManager::_get_relay_state(2), true); // When button is pressed, activate relay
  }
}

void ButtonManager::_button1_press(void) {
  ESP_LOGD("ButtonManager", "Button 1 pressed.");

  switch (ButtonManager::_button1_mode) {
  case NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__DIRECT: {
    ButtonManager::_set_relay_state(1, !ButtonManager::_get_relay_state(1), true); // Toggle output
    break;
  }

  case NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__NOTIFY_MANAGER: {
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
      NSPanelMQTTManagerCommand command = NSPANEL_MQTTMANAGER_COMMAND__INIT;
      NSPanelMQTTManagerCommand__ButtonPressed pressed_command = NSPANEL_MQTTMANAGER_COMMAND__BUTTON_PRESSED__INIT;
      pressed_command.button_id = 1;
      command.button_pressed = &pressed_command;
      command.nspanel_id = config->nspanel_id;
      command.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_BUTTON_PRESSED;

      uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&command);
      std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
      size_t packed_data_size = nspanel_mqttmanager_command__pack(&command, buffer.data());
      if (packed_data_size == packed_length) [[likely]] {
        if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) [[unlikely]] {
          ESP_LOGE("ButtonManager", "Failed to publish command that button was pressed.");
        }
      }
    } else {
      ESP_LOGE("ButtonManager", "Failed to get config while trying to process button press event and as such could not sent event to manager for further handling.");
    }
    break;
  }

  case NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__FOLLOW:
    break; // Simply do nothing, this is handled in "key down" and "key up" events.

  case NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__OPEN_ENTITY_PAGE:
    ButtonManager::_open_entity_page(1);
    break;

  default:
    ESP_LOGE("ButtonManager", "Unknown button action. Will not perform any action.");
    break;
  }
}

void ButtonManager::_button2_press(void) {
  ESP_LOGD("ButtonManager", "Button 2 pressed.");

  switch (ButtonManager::_button2_mode) {
  case NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__DIRECT: {
    ButtonManager::_set_relay_state(2, !ButtonManager::_get_relay_state(2), true); // Toggle output
    break;
  }

  case NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__NOTIFY_MANAGER: {
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
      NSPanelMQTTManagerCommand command = NSPANEL_MQTTMANAGER_COMMAND__INIT;
      NSPanelMQTTManagerCommand__ButtonPressed pressed_command = NSPANEL_MQTTMANAGER_COMMAND__BUTTON_PRESSED__INIT;
      pressed_command.button_id = 2;
      command.button_pressed = &pressed_command;
      command.nspanel_id = config->nspanel_id;
      command.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_BUTTON_PRESSED;

      uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&command);
      std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
      size_t packed_data_size = nspanel_mqttmanager_command__pack(&command, buffer.data());
      if (packed_data_size == packed_length) [[likely]] {
        if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) [[unlikely]] {
          ESP_LOGE("ButtonManager", "Failed to publish command that button was pressed.");
        }
      }
    } else {
      ESP_LOGE("ButtonManager", "Failed to get config while trying to process button press event and as such could not sent event to manager for further handling.");
    }

    break;
  }

  case NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__FOLLOW:
    break; // Simply do nothing, this is handled in "key down" and "key up" events.

  case NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__OPEN_ENTITY_PAGE:
    ButtonManager::_open_entity_page(2);
    break;

  default:
    ESP_LOGE("ButtonManager", "Unknown button action. Will not perform any action.");
    break;
  }
}

void ButtonManager::_open_entity_page(uint8_t button) {
  // Copy the topic out of the config at press time, as a new config may replace it at any moment.
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK) [[unlikely]] {
    ESP_LOGE("ButtonManager", "Failed to get config while trying to open the entity page for button %d.", button);
    return;
  }
  const char *topic = button == 1 ? config->button1_entity_page_topic : config->button2_entity_page_topic;
  if (topic == NULL || topic[0] == '\0') [[unlikely]] {
    ESP_LOGE("ButtonManager", "Button %d opens an entity page, but the config gives no entity for it.", button);
    return;
  }
  ESP_LOGD("ButtonManager", "Button %d opens the entity page for %s.", button, topic);
  InterfaceManager::request_show_entity_page(topic);
}

void ButtonManager::_set_relay_state(uint8_t relay, bool state, bool send_mqtt_update) {
  if (relay == 1) {
    // Only allow relay state to be changed once every 250ms if we have actually been running for more than 250ms.  This will not allow default relay mode to be set otherwise.
    if (esp_timer_get_time() >= 250000 && esp_timer_get_time() - ButtonManager::_last_relay1_change < 250000) {
      return;
    }

    if (!ButtonManager::_reverse_relays) {
      ESP_LOGD("ButtonManager", "Setting output of relay 1 (left) relay to %s", state ? "ON" : " OFF");
      ButtonManager::_relay1_current_state = state;
      gpio_set_level(ButtonManager::_relay1_pin, state ? 1 : 0);
    } else {
      ESP_LOGD("ButtonManager", "Setting output of relay 2 (right|reversed) relay to %s", state ? "ON" : " OFF");
      ButtonManager::_relay2_current_state = state;
      gpio_set_level(ButtonManager::_relay2_pin, state ? 1 : 0);
    }

    if (send_mqtt_update) {
      MqttManager::publish(ButtonManager::_relay1_state_topic, state ? "1" : "0", strlen(state ? "1" : "0"), true);

      std::shared_ptr<NSPanelConfig> config;
      if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
        std::string relay_group_base_topic = "nspanel/mqttmanager_";
        relay_group_base_topic.append(NSPM_ConfigManager::get_manager_address());
        relay_group_base_topic.append("/relay_groups/");
        std::string relay_group_topic;
        for (int i = 0; i < config->n_relay1_relay_group; i++) {
          relay_group_topic = relay_group_base_topic;
          relay_group_topic.append(std::to_string(config->relay1_relay_group[i]));
          relay_group_topic.append("/state");

          MqttManager::publish(relay_group_topic, state ? "1" : "0", strlen(state ? "1" : "0"), true);
        }
      }
    }
    ButtonManager::_last_relay1_change = esp_timer_get_time();
  } else if (relay == 2) {
    // Only allow relay state to be changed once every 250ms if we have actually been running for more than 250ms. This will not allow default relay mode to be set otherwise.
    if (esp_timer_get_time() >= 250000 && esp_timer_get_time() - ButtonManager::_last_relay2_change < 250000) {
      return;
    }

    if (!ButtonManager::_reverse_relays) {
      ESP_LOGD("ButtonManager", "Setting output of relay 2 (right) relay to %s", state ? "ON" : " OFF");
      ButtonManager::_relay2_current_state = state;
      gpio_set_level(ButtonManager::_relay2_pin, state ? 1 : 0);
    } else {
      ESP_LOGD("ButtonManager", "Setting output of relay 1 (left|reversed) relay to %s", state ? "ON" : " OFF");
      ButtonManager::_relay1_current_state = state;
      gpio_set_level(ButtonManager::_relay1_pin, state ? 1 : 0);
    }

    if (send_mqtt_update) {
      MqttManager::publish(ButtonManager::_relay2_state_topic, state ? "1" : "0", strlen(state ? "1" : "0"), true);

      std::shared_ptr<NSPanelConfig> config;
      if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
        std::string relay_group_base_topic = "nspanel/mqttmanager_";
        relay_group_base_topic.append(NSPM_ConfigManager::get_manager_address());
        relay_group_base_topic.append("/relay_groups/");
        std::string relay_group_topic;
        for (int i = 0; i < config->n_relay2_relay_group; i++) {
          relay_group_topic = relay_group_base_topic;
          relay_group_topic.append(std::to_string(config->relay2_relay_group[i]));
          relay_group_topic.append("/state");

          MqttManager::publish(relay_group_topic, state ? "1" : "0", strlen(state ? "1" : "0"), true);
        }
      }
    }

    ButtonManager::_last_relay2_change = esp_timer_get_time();
  }
}

bool ButtonManager::_get_relay_state(uint8_t relay) {
  if (relay == 1) {
    if (!ButtonManager::_reverse_relays) {
      return ButtonManager::_relay1_current_state;
    } else {
      return ButtonManager::_relay2_current_state;
    }
  } else if (relay == 2) {
    if (!ButtonManager::_reverse_relays) {
      return ButtonManager::_relay2_current_state;
    } else {
      return ButtonManager::_relay1_current_state;
    }
  }
  return false; // Should never be reached
}

void ButtonManager::_handle_mqtt_relay_group_topics() {
  std::vector<int32_t> current_relay1_group_ids;
  std::vector<int32_t> current_relay2_group_ids;
  if (ButtonManager::_current_config != nullptr) {
    for (int i = 0; i < ButtonManager::_current_config->n_relay1_relay_group; i++) {
      current_relay1_group_ids.push_back(ButtonManager::_current_config->relay1_relay_group[i]);
    }
    for (int i = 0; i < ButtonManager::_current_config->n_relay2_relay_group; i++) {
      current_relay2_group_ids.push_back(ButtonManager::_current_config->relay2_relay_group[i]);
    }
  }

  std::vector<int32_t> new_relay1_group_ids;
  std::vector<int32_t> new_relay2_group_ids;
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
    for (int i = 0; i < config->n_relay1_relay_group; i++) {
      new_relay1_group_ids.push_back(config->relay1_relay_group[i]);
    }
    for (int i = 0; i < config->n_relay2_relay_group; i++) {
      new_relay2_group_ids.push_back(config->relay2_relay_group[i]);
    }
  } else {
    ESP_LOGE("ButtonManager", "Failed to get current config while trying to subscribe/unsubscribe from relay group topics.");
    return;
  }

  // Unsubscribe from any removed relay1 group
  for (int i = 0; i < current_relay1_group_ids.size(); i++) {
    bool group_found = false;
    for (int j = 0; j < new_relay1_group_ids.size(); j++) {
      if (new_relay1_group_ids[j] == current_relay1_group_ids[i]) {
        group_found = true;
        break;
      }
    }

    if (!group_found) {
      ESP_LOGI("ButtonManager", "Removing relay group %ld from relay1 binding.", current_relay1_group_ids[i]);
      std::string relay_group_topic = "nspanel/mqttmanager_";
      relay_group_topic.append(NSPM_ConfigManager::get_manager_address());
      relay_group_topic.append("/relay_groups/");
      relay_group_topic.append(std::to_string(current_relay1_group_ids[i]));
      relay_group_topic.append("/state");
      MqttManager::unsubscribe(relay_group_topic);
    }
  }

  // Unsubscribe from any removed relay2 group
  for (int i = 0; i < current_relay2_group_ids.size(); i++) {
    bool group_found = false;
    for (int j = 0; j < new_relay2_group_ids.size(); j++) {
      if (new_relay2_group_ids[j] == current_relay2_group_ids[i]) {
        group_found = true;
        break;
      }
    }

    if (!group_found) {
      ESP_LOGI("ButtonManager", "Removing relay group %ld from relay2 binding.", current_relay2_group_ids[i]);
      std::string relay_group_topic = "nspanel/mqttmanager_";
      relay_group_topic.append(NSPM_ConfigManager::get_manager_address());
      relay_group_topic.append("/relay_groups/");
      relay_group_topic.append(std::to_string(current_relay2_group_ids[i]));
      relay_group_topic.append("/state");
      MqttManager::unsubscribe(relay_group_topic);
    }
  }

  // Subscribe to any added relay1 group
  for (int i = 0; i < new_relay1_group_ids.size(); i++) {
    ESP_LOGI("ButtonManager", "Adding relay1 to relay group ID %ld.", new_relay1_group_ids[i]);
    std::string relay_group_topic = "nspanel/mqttmanager_";
    relay_group_topic.append(NSPM_ConfigManager::get_manager_address());
    relay_group_topic.append("/relay_groups/");
    relay_group_topic.append(std::to_string(new_relay1_group_ids[i]));
    relay_group_topic.append("/state");
    MqttManager::subscribe(relay_group_topic);
  }

  // Subscribe to any added relay2 group
  for (int i = 0; i < new_relay2_group_ids.size(); i++) {
    ESP_LOGI("ButtonManager", "Adding relay2 to relay group ID %ld.", new_relay2_group_ids[i]);
    std::string relay_group_topic = "nspanel/mqttmanager_";
    relay_group_topic.append(NSPM_ConfigManager::get_manager_address());
    relay_group_topic.append("/relay_groups/");
    relay_group_topic.append(std::to_string(new_relay2_group_ids[i]));
    relay_group_topic.append("/state");
    MqttManager::subscribe(relay_group_topic);
  }
}

void ButtonManager::_nspm_configmanager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  ESP_LOGD("ButtonManager", "Received new config. Updating internal values.");
  ButtonManager::_handle_mqtt_relay_group_topics();

  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
    ButtonManager::_reverse_relays = config->reverse_relays;
    ButtonManager::_button1_mode = config->button1_mode;
    ButtonManager::_button2_mode = config->button2_mode;

    bool save = false;
    if (config->button1_mode == NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__THERMOSTAT_HEAT || config->button1_mode == NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__THERMOSTAT_COOL) {
      if (config->button1_mode == NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__THERMOSTAT_HEAT) {
        ConfigManager::relay1_thermostat_cool_mode = false;
        ConfigManager::relay1_thermostat_heat_mode = true;
        ESP_LOGI("ButtonManager", "Relay1 is set to thermostat (heating) mode.");
      } else if (config->button1_mode == NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__THERMOSTAT_COOL) {
        ConfigManager::relay1_thermostat_heat_mode = false;
        ConfigManager::relay1_thermostat_cool_mode = true;
        ESP_LOGI("ButtonManager", "Relay1 is set to thermostat (cooling) mode.");
      }
      ConfigManager::relay1_lower_temperature = config->button1_lower_temperature;
      ConfigManager::relay1_upper_temperature = config->button1_upper_temperature;

      // If we are running in thermostat mode. Always set default to relay is off.
      ConfigManager::relay1_default_mode = false;
      ButtonManager::_relay1_default_mode = false;
      ButtonManager::_set_relay_state(1, false, true);
      save = true;
    } else {
      ConfigManager::relay1_thermostat_heat_mode = false;
      ConfigManager::relay1_thermostat_cool_mode = false;

      if (config->relay1_default_mode != ButtonManager::_relay1_default_mode) {
        ESP_LOGI("ButtonManager", "Relay1 default state changed. Setting new state %s -> %s.", ButtonManager::_relay1_default_mode ? "ON" : "OFF", config->relay1_default_mode ? "ON" : "OFF");
        ConfigManager::relay1_default_mode = config->relay1_default_mode;
        ButtonManager::_relay1_default_mode = config->relay1_default_mode;
        ButtonManager::_set_relay_state(1, config->relay1_default_mode, true);
        save = true;
      }
    }

    if (config->button2_mode == NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__THERMOSTAT_HEAT || config->button2_mode == NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__THERMOSTAT_COOL) {
      if (config->button2_mode == NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__THERMOSTAT_HEAT) {
        ConfigManager::relay2_thermostat_cool_mode = false;
        ConfigManager::relay2_thermostat_heat_mode = true;
        ESP_LOGI("ButtonManager", "Relay2 is set to thermostat (heating) mode.");
      } else if (config->button2_mode == NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__THERMOSTAT_COOL) {
        ConfigManager::relay2_thermostat_heat_mode = false;
        ConfigManager::relay2_thermostat_cool_mode = true;
        ESP_LOGI("ButtonManager", "Relay2 is set to thermostat (cooling) mode.");
      }
      ConfigManager::relay2_lower_temperature = config->button2_lower_temperature;
      ConfigManager::relay2_upper_temperature = config->button2_upper_temperature;

      // If we are running in thermostat mode. Always set default to relay is off.
      ConfigManager::relay2_default_mode = false;
      ButtonManager::_relay2_default_mode = false;
      ButtonManager::_set_relay_state(2, false, true);
      save = true;
    } else {
      ConfigManager::relay2_thermostat_cool_mode = false;
      ConfigManager::relay2_thermostat_heat_mode = false;

      if (config->relay2_default_mode != ButtonManager::_relay2_default_mode) {
        ESP_LOGI("ButtonManager", "Relay2 default state changed. Setting new state %s -> %s.", ButtonManager::_relay2_default_mode ? "ON" : "OFF", config->relay2_default_mode ? "ON" : "OFF");
        ConfigManager::relay2_default_mode = config->relay2_default_mode;
        ButtonManager::_relay2_default_mode = config->relay2_default_mode;
        ButtonManager::_set_relay_state(2, config->relay2_default_mode, true);
        save = true;
      }
    }

    if (config->min_button_push_time != ButtonManager::_min_button_push_time) {
      ButtonManager::_min_button_push_time = config->min_button_push_time;
      ConfigManager::min_button_push_time = ButtonManager::_min_button_push_time;

      ButtonManager::_button1->setDoubleClickInterval(ButtonManager::_min_button_push_time);
      ButtonManager::_button2->setDoubleClickInterval(ButtonManager::_min_button_push_time);
      save = true;
    }

    if (config->button_long_press_time != ButtonManager::_min_button_long_push_time) {
      ButtonManager::_min_button_long_push_time = config->button_long_press_time;
      ConfigManager::min_button_long_push_time = ButtonManager::_min_button_long_push_time;

      ButtonManager::_button1->setLongPressInterval(ButtonManager::_min_button_long_push_time);
      ButtonManager::_button2->setLongPressInterval(ButtonManager::_min_button_long_push_time);
      save = true;
    }

    if (save) {
      ConfigManager::save_config();
    }

    ButtonManager::_current_config = config;
  } else {
    ESP_LOGE("ButtonManager", "Got new config update but ButtonManager failed to read config. Cannot update internal values.");
  }
}

void ButtonManager::_new_temperature_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id != statusupdatemanagerevent_t::AVERAGE_TEMP_UPDATE) [[unlikely]] {
    return;
  }
  double *new_avg_temp = ((double *)event_data);

  if (ConfigManager::relay1_thermostat_heat_mode) {
    if (*new_avg_temp >= ConfigManager::relay1_upper_temperature) {
      if (_get_relay_state(1)) {
        ESP_LOGI("ButtonManager", "Relay1 is set to heat mode and new temperature %f has gone above (>=) set threshold (%d). Stop heating.", *new_avg_temp, ConfigManager::relay1_upper_temperature);
        _set_relay_state(1, false, true); // We are in heat mode and the temperature has risen above set threshold. Stop heating.
      }
    } else if (*new_avg_temp <= ConfigManager::relay1_lower_temperature) {
      if (!_get_relay_state(1)) {
        ESP_LOGI("ButtonManager", "Relay1 is set to heat mode and new temperature %f has gone below (<=) set threshold (%d). Start heating.", *new_avg_temp, ConfigManager::relay1_lower_temperature);
        _set_relay_state(1, true, true); // We are in heat mode and the temperature has gone below set threshold. Start heating.
      }
    }
  } else if (ConfigManager::relay1_thermostat_cool_mode) {
    if (*new_avg_temp >= ConfigManager::relay1_upper_temperature) {
      if (!_get_relay_state(1)) {
        ESP_LOGI("ButtonManager", "Relay1 is set to cool mode and new temperature %f has gone above (>=) set threshold (%d). Start cooling.", *new_avg_temp, ConfigManager::relay1_upper_temperature);
        _set_relay_state(1, true, true); // We are in cool mode and the temperature has risen above set threshold. Start cooling.
      }
    } else if (*new_avg_temp <= ConfigManager::relay1_lower_temperature) {
      if (_get_relay_state(1)) {
        ESP_LOGI("ButtonManager", "Relay1 is set to cool mode and new temperature %f has gone below (<=) set threshold (%d). Stop cooling.", *new_avg_temp, ConfigManager::relay1_upper_temperature);
        _set_relay_state(1, false, true); // We are in cool mode and the temperature has gone below set threshold. Stop cooling.
      }
    }
  }

  if (ConfigManager::relay2_thermostat_heat_mode) {
    if (*new_avg_temp >= ConfigManager::relay2_upper_temperature) {
      if (_get_relay_state(2)) {
        ESP_LOGI("ButtonManager", "Relay2 is set to heat mode and new temperature %f has gone above (>=) set threshold (%d). Stop heating.", *new_avg_temp, ConfigManager::relay2_upper_temperature);
        _set_relay_state(2, false, true); // We are in heat mode and the temperature has risen above set threshold. Stop heating.
      }
    } else if (*new_avg_temp <= ConfigManager::relay2_lower_temperature) {
      if (!_get_relay_state(2)) {
        ESP_LOGI("ButtonManager", "Relay2 is set to heat mode and new temperature %f has gone below (<=) set threshold (%d). Start heating.", *new_avg_temp, ConfigManager::relay2_lower_temperature);
        _set_relay_state(2, true, true); // We are in heat mode and the temperature has gone below set threshold. Start heating.
      }
    }
  } else if (ConfigManager::relay2_thermostat_cool_mode) {
    if (*new_avg_temp >= ConfigManager::relay2_upper_temperature) {
      if (!_get_relay_state(2)) {
        ESP_LOGI("ButtonManager", "Relay2 is set to cool mode and new temperature %f has gone above (>=) set threshold (%d). Start cooling.", *new_avg_temp, ConfigManager::relay2_upper_temperature);
        _set_relay_state(2, true, true); // We are in cool mode and the temperature has risen above set threshold. Start cooling.
      }
    } else if (*new_avg_temp <= ConfigManager::relay2_lower_temperature) {
      if (_get_relay_state(2)) {
        ESP_LOGI("ButtonManager", "Relay2 is set to heat mode and new temperature %f has gone below (<=) set threshold (%d). Start heating.", *new_avg_temp, ConfigManager::relay2_lower_temperature);
        _set_relay_state(2, false, true); // We are in cool mode and the temperature has gone below set threshold. Stop cooling.
      }
    }
  }
}

void ButtonManager::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == MQTT_EVENT_DATA) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event->data_len == 0) {
      return;
    }

    std::string topic_string = std::string(event->topic, event->topic_len);
    std::string data = std::string(event->data, event->data_len);

    if (topic_string.compare(ButtonManager::_relay1_cmd_topic) == 0) {
      if (data.compare("0") == 0) {
        if (ButtonManager::_get_relay_state(1) == true) {
          ButtonManager::_set_relay_state(1, false, true);
        }
      } else if (data.compare("1") == 0) {
        if (ButtonManager::_get_relay_state(1) == false) {
          ButtonManager::_set_relay_state(1, true, true);
        }
      } else if (data.compare("2") == 0) {
        ButtonManager::_set_relay_state(1, !ButtonManager::_get_relay_state(1), true);
      } else {
        ESP_LOGE("ButtonManager", "Got command to set relay1 state but command data was not recognized.");
      }
    } else if (topic_string.compare(ButtonManager::_relay2_cmd_topic) == 0) {
      if (data.compare("0") == 0) {
        if (ButtonManager::_get_relay_state(2) == true) {
          ButtonManager::_set_relay_state(2, false, true);
        }
      } else if (data.compare("1") == 0) {
        if (ButtonManager::_get_relay_state(2) == false) {
          ButtonManager::_set_relay_state(2, true, true);
        }
      } else if (data.compare("2") == 0) {
        ButtonManager::_set_relay_state(2, !ButtonManager::_relay2_current_state, true);
      } else {
        ESP_LOGE("ButtonManager", "Got command to set relay2 state but command data was not recognized.");
      }
    }

    // Check if it is a relay1 bound group
    std::shared_ptr<NSPanelConfig> config = ButtonManager::_current_config;
    if (config != nullptr) [[likely]] {
      std::string relay_group_base_topic = "nspanel/mqttmanager_";
      relay_group_base_topic.append(NSPM_ConfigManager::get_manager_address());
      relay_group_base_topic.append("/relay_groups/");
      std::string relay_group_topic;

      for (int i = 0; i < config->n_relay1_relay_group; i++) {
        relay_group_topic = relay_group_base_topic;
        relay_group_topic.append(std::to_string(config->relay1_relay_group[i]));
        relay_group_topic.append("/state");

        if (topic_string.compare(relay_group_topic) == 0) {
          if (data.compare("0") == 0) {
            if (ButtonManager::_get_relay_state(1)) { // Relay 1 is on, turn off.
              ESP_LOGD("ButtonManager", "Received update from relay group %ld, new state OFF.", config->relay1_relay_group[i]);
              if (ButtonManager::_get_relay_state(1) == true) {
                ButtonManager::_set_relay_state(1, false, true);
              }
            }
          } else if (data.compare("1") == 0) {
            if (!ButtonManager::_get_relay_state(1)) { // Relay 1 is off, turn off.
              ESP_LOGD("ButtonManager", "Received update from relay group %ld, new state ON.", config->relay1_relay_group[i]);
              if (ButtonManager::_get_relay_state(1) == false) {
                ButtonManager::_set_relay_state(1, true, true);
              }
            }
          } else {
            ESP_LOGE("ButtonManager", "Found matching relay group topic but failed to determine state.");
          }
        }
        break;
      }

      // Check if it is a relay2 bound group
      for (int i = 0; i < config->n_relay2_relay_group; i++) {
        relay_group_topic = relay_group_base_topic;
        relay_group_topic.append(std::to_string(config->relay2_relay_group[i]));
        relay_group_topic.append("/state");

        if (topic_string.compare(relay_group_topic) == 0) {
          if (data.compare("0") == 0) {
            if (ButtonManager::_get_relay_state(2)) { // Relay 2 is on, turn off.
              ESP_LOGD("ButtonManager", "Received update from relay group %ld, new state OFF.", config->relay2_relay_group[i]);
              if (ButtonManager::_get_relay_state(2) == true) {
                ButtonManager::_set_relay_state(2, false, true);
              }
            }
          } else if (data.compare("1") == 0) {
            if (!ButtonManager::_get_relay_state(2)) { // Relay 2 is off, turn off.
              ESP_LOGD("ButtonManager", "Received update from relay group %ld, new state ON.", config->relay2_relay_group[i]);
              if (ButtonManager::_get_relay_state(2) == false) {
                ButtonManager::_set_relay_state(2, true, true);
              }
            }
          } else {
            ESP_LOGE("ButtonManager", "Found matching relay group topic but failed to determine state.");
          }
        }
        break;
      }
    } else {
      ESP_LOGW("ButtonManager", "No config currently loaded/set. Unable to determine if received message was from a relay group. Will not change any relay state.");
    }
  } else if (event_id == MQTT_EVENT_CONNECTED) {
    // MqttManager re-subscribes the relay and relay group topics after a reconnect.
    // Publish current state to relay state topics
    bool state = ButtonManager::_get_relay_state(1);
    MqttManager::publish(ButtonManager::_relay1_state_topic, state ? "1" : "0", strlen(state ? "1" : "0"), true);

    state = ButtonManager::_get_relay_state(2);
    MqttManager::publish(ButtonManager::_relay2_state_topic, state ? "1" : "0", strlen(state ? "1" : "0"), true);
  }
}