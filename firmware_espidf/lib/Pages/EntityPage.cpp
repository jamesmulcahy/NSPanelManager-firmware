#include <ConfigManager.hpp>
#include <EntitiesPage.hpp>
#include <EntityPage.hpp>
#include <GUI_data.hpp>
#include <InterfaceManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <Nextion.hpp>
#include <Nextion_event.hpp>
#include <StatusUpdateManager.hpp>
#include <esp_log.h>
#include <protobuf_nspanel_entity.pb-c.h>

void EntityPage::show(std::string state_topic) {
  esp_log_level_set("EntityPage", ConfigManager::log_level);

  if (EntityPage::_current_state_mutex == NULL) {
    EntityPage::_current_state_mutex = xSemaphoreCreateMutex();
  }

  MqttManager::register_handler(MQTT_EVENT_ANY, &EntityPage::_handle_mqtt_event, NULL);

  EntityPage::_current_entity_mqtt_topic = state_topic;
  MqttManager::subscribe(EntityPage::_current_entity_mqtt_topic);

  // Reset variables:
  EntityPage::_selected_thermostat_option_index = 0;
  EntityPage::_is_currently_editing = false;
}

void EntityPage::unshow() {
  MqttManager::unregister_handler(MQTT_EVENT_ANY, &EntityPage::_handle_mqtt_event);
  esp_event_handler_unregister(NEXTION_EVENT, ESP_EVENT_ANY_ID, &EntityPage::_handle_nextion_event);
  if (!EntityPage::_current_entity_mqtt_topic.empty()) {
    MqttManager::unsubscribe(EntityPage::_current_entity_mqtt_topic);
  }
  EntityPage::_currently_showing = false;
}

void EntityPage::_handle_mqtt_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == MQTT_EVENT_DATA) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event->topic_len == 0 || event->data_len == 0) [[unlikely]] {
      return;
    }
    std::string topic_string = std::string(event->topic, event->topic_len);
    if (topic_string.compare(EntityPage::_current_entity_mqtt_topic) == 0) {
      NSPanelEntityState *new_state = nspanel_entity_state__unpack(NULL, event->data_len, (const uint8_t *)event->data);
      if (new_state != NULL) [[likely]] {
        if (xSemaphoreTake(EntityPage::_current_state_mutex, pdMS_TO_TICKS(500)) == pdPASS) [[likely]] {
          EntityPage::_current_state = std::shared_ptr<NSPanelEntityState>(new_state, &EntityPage::_delete_nspanel_entity_state_object);
          xSemaphoreGive(EntityPage::_current_state_mutex);
          auto state = EntityPage::_get_current_state();

          // Set current display mode (RGB/Color temp) from what mode the light state is in
          if (state->entity_case == NSPanelEntityState__EntityCase::NSPANEL_ENTITY_STATE__ENTITY_LIGHT) {
            switch (state->light->current_light_mode) {
            case NSPANEL_ENTITY_STATE__LIGHT__LIGHT_MODE__COLOR_TEMP:
              EntityPage::_current_mode = _entity_page_modes::LIGHT_COLOR_TEMPERATURE;
              break;

            case NSPANEL_ENTITY_STATE__LIGHT__LIGHT_MODE__RGB:
              EntityPage::_current_mode = _entity_page_modes::LIGHT_RGB;
              break;

            default:
              ESP_LOGW("EntityPage", "Unknown light mode!");
              break;
            }
          } else if (state->entity_case == NSPanelEntityState__EntityCase::NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT) {
            EntityPage::_current_mode = _entity_page_modes::THERMOSTAT;
          } else if (state->entity_case == NSPanelEntityState__EntityCase::NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) {
            EntityPage::_current_mode = _entity_page_modes::MEDIA_PLAYER;
          } else {
            ESP_LOGE("EntityPage", "Unknown entity state case!");
          }

          EntityPage::_update_display();
        } else {
          ESP_LOGE("EntityPage", "Failed to take mutex to update current state.");
          nspanel_entity_state__free_unpacked(new_state, NULL);
        }
      } else {
        ESP_LOGE("EntityPage", "Received new state but failed to parse into protobuf object.");
      }
    }
  }
}

void EntityPage::_update_display() {
  ESP_LOGD("EntityPage", "Updating displayed value via delegator.");
  switch (EntityPage::_current_state->entity_case) {
  case NSPANEL_ENTITY_STATE__ENTITY_LIGHT:
    EntityPage::_update_display_light();
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT:
    EntityPage::_update_display_thermostat();
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER:
    EntityPage::_update_display_media_player();
    break;

  default:
    ESP_LOGE("EntityPage", "Unknown state type. Can't call appropriate update display function.");
    break;
  }
}

void EntityPage::_handle_nextion_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == nextion_event_t::TOUCH_EVENT) {
    nextion_event_touch_t *touch_data = (nextion_event_touch_t *)event_data;
    EntityPage::_handle_touch_event(touch_data->component_id, touch_data->pressed);
  } else if (event_id == nextion_event_t::STRING_EVENT) {
    switch (EntityPage::_current_state->entity_case) {
    case NSPANEL_ENTITY_STATE__ENTITY_LIGHT:
      break;

    case NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT:
      EntityPage::_handle_string_event_thermostat((char *)event_data);
      break;

    case NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER:
      break;

    default:
      ESP_LOGE("EntityPage", "Unknown state type. Can't call appropriate string event function.");
      break;
    }
  }
}

void EntityPage::_handle_config_update(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
}

void EntityPage::_handle_touch_event(uint16_t component_id, bool pressed) {
  switch (EntityPage::_current_state->entity_case) {
  case NSPANEL_ENTITY_STATE__ENTITY_LIGHT:
    EntityPage::_handle_touch_event_light(component_id, pressed);
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT:
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER:
    EntityPage::_handle_touch_event_media_player(component_id, pressed);
    break;

  default:
    ESP_LOGE("EntityPage", "Unknown state type. Can't call appropriate touch function.");
    break;
  }
}

void EntityPage::_update_display_light() {
  ESP_LOGI("EntityPage", "Updating EntityPage with light state.");
  if (!EntityPage::_currently_showing) {
    ESP_LOGD("EntityPage", "Switching page to %s", GUI_LIGHT_CONTROL_PAGE::page_name);
    EntityPage::_currently_showing = true;
    if (Nextion::go_to_page(GUI_LIGHT_CONTROL_PAGE::page_name, 1000) != ESP_OK) [[unlikely]] {
      ESP_LOGE("EntityPage", "Failed to navigate Nextion to page. Will go back.");
      EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
      return;
    }

    InterfaceManager::call_unshow_callback();
    InterfaceManager::current_page_unshow_callback.set(EntityPage::unshow);

    esp_event_handler_register(NEXTION_EVENT, ESP_EVENT_ANY_ID, &EntityPage::_handle_nextion_event, NULL);
  }

  auto state = EntityPage::_get_current_state();

  // Show button to switch modes IF light can both color and color temp
  if (state->light->can_color && state->light->can_color_temp) {
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::switch_mode_button_name, true, 1000);
  } else {
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::switch_mode_button_name, false, 1000);
  }

  // Update name if it has changed
  if (EntityPage::_last_displayed_name.compare(state->light->name) != 0) {
    Nextion::set_component_text(GUI_LIGHT_CONTROL_PAGE::light_label_name, state->light->name, 500);
    EntityPage::_last_displayed_name = state->light->name;
  }

  // Update brightness if it has changed
  if (EntityPage::_last_brightness != state->light->brightness) {
    Nextion::set_component_value(GUI_LIGHT_CONTROL_PAGE::brightness_slider_name, state->light->brightness, 250);
    EntityPage::_last_brightness = state->light->brightness;
  }

  // Update hue if it has changed
  if (EntityPage::_last_hue != state->light->hue) {
    Nextion::set_component_value(GUI_LIGHT_CONTROL_PAGE::hue_slider_name, state->light->hue, 250);
    EntityPage::_last_hue = state->light->hue;
  }

  // Set component icons and visibility
  if (EntityPage::_current_mode == EntityPage::_entity_page_modes::LIGHT_COLOR_TEMPERATURE) {
    Nextion::set_component_pic(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, GUI_LIGHT_CONTROL_PAGE::kelvin_slider_pic, 250);
    Nextion::set_component_pic1(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, GUI_LIGHT_CONTROL_PAGE::kelvin_slider_pic1, 250);

    Nextion::set_component_pic(GUI_LIGHT_CONTROL_PAGE::switch_mode_button_name, GUI_LIGHT_CONTROL_PAGE::rgb_mode_pic, 250); // Indicate that the user can switch to RGB-mode by pressing the button
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::hue_slider_name, false, 250);
  } else if (EntityPage::_current_mode == EntityPage::_entity_page_modes::LIGHT_RGB) {
    Nextion::set_component_pic(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, GUI_LIGHT_CONTROL_PAGE::saturation_slider_pic, 250);
    Nextion::set_component_pic1(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, GUI_LIGHT_CONTROL_PAGE::saturation_slider_pic1, 250);

    Nextion::set_component_pic(GUI_LIGHT_CONTROL_PAGE::switch_mode_button_name, GUI_LIGHT_CONTROL_PAGE::kelvin_mode_pic, 250); // Indicate that the user can switch to color temperature mode by pressing the button
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::hue_slider_name, true, 250);
  } else {
    ESP_LOGE("EntityPage", "Unknown light mode!");
  }

  if (state->light->can_color && state->light->can_color_temp) {
    if (EntityPage::_current_mode == _entity_page_modes::LIGHT_COLOR_TEMPERATURE) {
      if (EntityPage::_last_kelvin_pct != state->light->can_color_temp) {
        Nextion::set_component_value(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, state->light->color_temp, 250);
        EntityPage::_last_saturation_pct = state->light->color_temp;
      }
      Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::hue_slider_name, false, 250);
      Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, true, 250);
    } else if (EntityPage::_current_mode == _entity_page_modes::LIGHT_RGB) {
      if (EntityPage::_last_saturation_pct != state->light->saturation) {
        Nextion::set_component_value(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, state->light->saturation, 250);
        EntityPage::_last_saturation_pct = state->light->saturation;
      }
      Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::hue_slider_name, true, 250);
      Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, true, 250);
    } else {
      ESP_LOGE("EntityPage", "Unknown color mode for light!");
    }
  } else if (state->light->can_color_temp) {
    EntityPage::_current_mode = _entity_page_modes::LIGHT_COLOR_TEMPERATURE;
    Nextion::set_component_value(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, state->light->color_temp, 250);
    EntityPage::_last_saturation_pct = state->light->color_temp;
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::hue_slider_name, false, 250);
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, true, 250);
  } else if (state->light->can_color) {
    EntityPage::_current_mode = _entity_page_modes::LIGHT_RGB;
    Nextion::set_component_value(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, state->light->saturation, 250);
    EntityPage::_last_saturation_pct = state->light->saturation;
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, true, 250);
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::hue_slider_name, true, 250);
  } else {
    ESP_LOGD("EntityPage", "Light is not capable of color or color temp, will hide kelvin/saturation slider.");
    Nextion::set_component_visibility(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, false, 250);
  }
}

void EntityPage::_handle_touch_event_light(uint16_t component_id, bool pressed) {
  ESP_LOGD("EntityPage", "Touch component %d, pressed %s", component_id, pressed ? "Yes" : "No");
  switch (component_id) {
  case GUI_LIGHT_CONTROL_PAGE::back_button_id:
    ESP_LOGD("EntityPage", "Received touch event to go back.");
    EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
    break;

  case GUI_LIGHT_CONTROL_PAGE::switch_mode_button_id: {
    if (EntityPage::_current_mode == EntityPage::_entity_page_modes::LIGHT_COLOR_TEMPERATURE) {
      EntityPage::_current_mode = EntityPage::_entity_page_modes::LIGHT_RGB;
    } else {
      EntityPage::_current_mode = EntityPage::_entity_page_modes::LIGHT_COLOR_TEMPERATURE;
    }
    EntityPage::_update_display_light();
    break;
  }

  case GUI_LIGHT_CONTROL_PAGE::brightness_slider_id: {
    int32_t new_brightness;
    int32_t light_id = EntityPage::_current_state->light->light_id;
    if (Nextion::get_component_integer_value(GUI_LIGHT_CONTROL_PAGE::brightness_slider_name, &new_brightness, 250, 250) == ESP_OK) [[likely]] {
      NSPanelMQTTManagerCommand__LightCommand light_command = NSPANEL_MQTTMANAGER_COMMAND__LIGHT_COMMAND__INIT;
      light_command.light_ids = &light_id;
      light_command.n_light_ids = 1;
      light_command.has_brightness = true;
      light_command.brightness = new_brightness;

      NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
      cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_LIGHT_COMMAND;
      cmd.light_command = &light_command;
      cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

      uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
      std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
      size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
      if (packed_data_size == packed_length) [[likely]] {
        if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) [[likely]] {
          EntityPage::_last_brightness = new_brightness;
        } else {
          ESP_LOGE("EntityPage", "Failed to send MQTT message with command payload.");
          EntityPage::_update_display_light(); // Update display to reset values to those stored
        }
      } else {
        ESP_LOGE("EntityPage", "Failed to pack protobuf command.");
        EntityPage::_update_display_light(); // Update display to reset values to those stored
      }

    } else {
      ESP_LOGE("EntityPage", "Failed to get new brightness value from Nextion. Will not send update command.");
      EntityPage::_update_display_light(); // Update display to reset values to those stored
    }
    break;
  }

  case GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_id: {
    int32_t new_kelvin_saturation;
    int32_t light_id = EntityPage::_current_state->light->light_id;
    if (Nextion::get_component_integer_value(GUI_LIGHT_CONTROL_PAGE::kelvin_saturation_slider_name, &new_kelvin_saturation, 250, 250) == ESP_OK) [[likely]] {
      NSPanelMQTTManagerCommand__LightCommand light_command = NSPANEL_MQTTMANAGER_COMMAND__LIGHT_COMMAND__INIT;
      light_command.light_ids = &light_id;
      light_command.n_light_ids = 1;
      if (EntityPage::_current_mode == EntityPage::_entity_page_modes::LIGHT_COLOR_TEMPERATURE) {
        light_command.has_color_temperature = true;
        light_command.color_temperature = new_kelvin_saturation;
      } else if (EntityPage::_current_mode == EntityPage::_entity_page_modes::LIGHT_RGB) {
        light_command.has_saturation = true;
        light_command.saturation = new_kelvin_saturation;
      }

      NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
      cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_LIGHT_COMMAND;
      cmd.light_command = &light_command;
      cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

      uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
      std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
      size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
      if (packed_data_size == packed_length) [[likely]] {
        if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) [[likely]] {
          if (EntityPage::_current_mode == EntityPage::_entity_page_modes::LIGHT_COLOR_TEMPERATURE) {
            EntityPage::_last_kelvin_pct = new_kelvin_saturation;
          } else if (EntityPage::_current_mode == EntityPage::_entity_page_modes::LIGHT_RGB) {
            EntityPage::_last_saturation_pct = new_kelvin_saturation;
          }
        } else {
          ESP_LOGE("EntityPage", "Failed to send MQTT message with command payload.");
          EntityPage::_update_display_light(); // Update display to reset values to those stored
        }
      } else {
        ESP_LOGE("EntityPage", "Failed to pack protobuf command.");
        EntityPage::_update_display_light(); // Update display to reset values to those stored
      }

    } else {
      ESP_LOGE("EntityPage", "Failed to get new brightness value from Nextion. Will not send update command.");
      EntityPage::_update_display_light(); // Update display to reset values to those stored
    }
    break;
  }

  case GUI_LIGHT_CONTROL_PAGE::hue_slider_id: {
    int32_t new_hue;
    int32_t light_id = EntityPage::_current_state->light->light_id;
    if (Nextion::get_component_integer_value(GUI_LIGHT_CONTROL_PAGE::hue_slider_name, &new_hue, 250, 250) == ESP_OK) [[likely]] {
      NSPanelMQTTManagerCommand__LightCommand light_command = NSPANEL_MQTTMANAGER_COMMAND__LIGHT_COMMAND__INIT;
      light_command.light_ids = &light_id;
      light_command.n_light_ids = 1;
      light_command.has_hue = true;
      light_command.hue = new_hue;

      NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
      cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_LIGHT_COMMAND;
      cmd.light_command = &light_command;
      cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

      uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
      std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
      size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
      if (packed_data_size == packed_length) [[likely]] {
        if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) [[likely]] {
          EntityPage::_last_hue = new_hue;
        } else {
          ESP_LOGE("EntityPage", "Failed to send MQTT message with command payload.");
          EntityPage::_update_display_light(); // Update display to reset values to those stored
        }
      } else {
        ESP_LOGE("EntityPage", "Failed to pack protobuf command.");
        EntityPage::_update_display_light(); // Update display to reset values to those stored
      }

    } else {
      ESP_LOGE("EntityPage", "Failed to get new brightness value from Nextion. Will not send update command.");
      EntityPage::_update_display_light(); // Update display to reset values to those stored
    }
    break;
  }

  default:
    break;
  }
}

void EntityPage::_update_display_thermostat() {
  ESP_LOGI("EntityPage", "Updating EntityPage with thermostat state.");
  if (!EntityPage::_currently_showing) {
    ESP_LOGD("EntityPage", "Switching page to %s", GUI_THERMOSTAT_CONTROL_PAGE::page_name);
    EntityPage::_currently_showing = true;
    if (Nextion::go_to_page(GUI_THERMOSTAT_CONTROL_PAGE::page_name, 1000) != ESP_OK) [[unlikely]] {
      ESP_LOGE("EntityPage", "Failed to navigate Nextion to page. Will go back.");
      EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
    }

    InterfaceManager::call_unshow_callback();
    InterfaceManager::current_page_unshow_callback.set(EntityPage::unshow);

    esp_event_handler_register(NEXTION_EVENT, ESP_EVENT_ANY_ID, &EntityPage::_handle_nextion_event, NULL);
  }

  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();

  // Loop over all options. Set them to the corresponding value if an option in the index
  // is available in the state data. If not, clean it and hide it.
  for (int i = 0; i < sizeof(GUI_THERMOSTAT_CONTROL_PAGE::options) / sizeof(GUI_THERMOSTAT_OPTIONS_MODE_DATA); i++) {
    if (i < state->thermostat->n_options) {
      Nextion::set_component_visibility(GUI_THERMOSTAT_CONTROL_PAGE::options[i].icon_name, true, 1000);
      Nextion::set_component_visibility(GUI_THERMOSTAT_CONTROL_PAGE::options[i].label_name, true, 1000);
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::options[i].icon_name, state->thermostat->options[i]->current_icon, 1000);
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::options[i].label_name, state->thermostat->options[i]->name, 1000);
    } else {
      Nextion::set_component_visibility(GUI_THERMOSTAT_CONTROL_PAGE::options[i].icon_name, false, 1000);
      Nextion::set_component_visibility(GUI_THERMOSTAT_CONTROL_PAGE::options[i].label_name, false, 1000);
    }
  }

  char buf[16];
  uint8_t chars_written = snprintf(buf, sizeof(buf), "%.1f°", state->thermostat->set_temperature);
  if (chars_written > 0) {
    if (!EntityPage::_is_currently_editing) { // Only update displayed temperature in case we are currently not editing any other option.
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, buf, 1000);
    }
  } else {
    ESP_LOGE("EntityPage", "Failed to snprintf set temp to temperature buffer.");
  }

  Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::room_name_label_name, state->thermostat->name, 1000);
  chars_written = 0;
  if (state->thermostat->has_current_temperature) {
    chars_written = snprintf(buf, sizeof(buf), "%.1f°", state->thermostat->current_temperature);
  } else {
    ESP_LOGI("EntityPage", "Thermostat does not have a valid temperature for location. Will use internal sensor.");
    chars_written = snprintf(buf, sizeof(buf), "%.1f°", StatusUpdateManager::current_temperature());
  }

  if (chars_written > 0) {
    Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::temperature_label_name, buf, 1000);
  } else {
    ESP_LOGE("EntityPage", "Failed to snprintf current temp to temperature buffer.");
  }
}

void EntityPage::_handle_string_event_thermostat(char *data) {
  ESP_LOGD("EntityPage", "Thermostat page received string event data: %s", data);

  if (strcmp(data, "activate:set1") == 0) {
    if (EntityPage::_current_state->thermostat->n_options >= 1) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set1");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 0;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, EntityPage::_current_state->thermostat->options[0]->current_value, 1000);
    }
  } else if (strcmp(data, "activate:set2") == 0) {
    if (EntityPage::_current_state->thermostat->n_options >= 2) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set2");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 1;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, EntityPage::_current_state->thermostat->options[1]->current_value, 1000);
    }
  } else if (strcmp(data, "activate:set3") == 0) {
    if (EntityPage::_current_state->thermostat->n_options >= 3) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set3");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 2;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, EntityPage::_current_state->thermostat->options[2]->current_value, 1000);
    }
  } else if (strcmp(data, "activate:set4") == 0) {
    if (EntityPage::_current_state->thermostat->n_options >= 4) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set4");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 3;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, EntityPage::_current_state->thermostat->options[3]->current_value, 1000);
    }
  } else if (strcmp(data, "activate:set5") == 0) {
    if (EntityPage::_current_state->thermostat->n_options >= 5) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set5");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 4;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, EntityPage::_current_state->thermostat->options[4]->current_value, 1000);
    }
  } else if (strcmp(data, "deactivate") == 0) { // We've edited an option. Send updated state to manager.
    EntityPage::_send_thermostat_option_command();
    EntityPage::_is_currently_editing = false;
    EntityPage::_update_display_thermostat();
  } else if (strcmp(data, "deactivatetemp") == 0) { // We've changed the temperature. Send updated temperature to manager.
    EntityPage::_send_thermostat_setpoint_command();
  } else if (strcmp(data, "back") == 0) {
    EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);

    if (EntityPage::_is_currently_editing) [[unlikely]] { // We are currently in editing mode, send the current option.
      EntityPage::_send_thermostat_option_command();
    } else {
      EntityPage::_send_thermostat_setpoint_command();
    }
  } else if (strcmp(data, "tempup") == 0) {
    if (EntityPage::_is_currently_editing) {
      std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
      if (EntityPage::_selected_thermostat_option_index < state->thermostat->n_options) {
        if (state->thermostat->options[EntityPage::_selected_thermostat_option_index]->n_options <= 0) {
          return; // Do nothing, there is no options available.
        }

        // Find current index of currently selected option
        uint8_t current_index = 0;
        for (int i = 0; i < state->thermostat->options[EntityPage::_selected_thermostat_option_index]->n_options; i++) {
          if (strcmp(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[i]->value, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value) == 0) {
            current_index = i;
            break;
          }
        }

        current_index++;
        if (current_index >= state->thermostat->options[EntityPage::_selected_thermostat_option_index]->n_options) {
          current_index = 0; // Reset index to 0 to restart loop of options
        }

        bool alloc_failed = false;
        uint16_t new_value_len = strlen(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[current_index]->value);
        uint16_t new_icon_len = strlen(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[current_index]->icon);

        char *new_value = (char *)malloc(new_value_len + 1);
        char *new_icon = (char *)malloc(new_icon_len + 1);

        if (new_value == NULL) {
          alloc_failed = true;
          ESP_LOGE("EntityPage", "Failed to allocate memory for new value. New value length: %u", new_value_len);
          if (new_icon != NULL) {
            free(new_icon); // Free new icon as it won't be used when alloc failed.
          }
        }

        if (new_icon == NULL) {
          alloc_failed = true;
          ESP_LOGE("EntityPage", "Failed to allocate memory for new icon. New icon length: %u", new_value_len);
          free(new_value); // Free new value as it won't be used when alloc failed.
        }

        if (!alloc_failed) [[likely]] {
          strncpy(new_value, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[current_index]->value, new_value_len);
          strncpy(new_icon, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[current_index]->icon, new_icon_len);
          new_value[new_value_len] = '\0';
          new_icon[new_icon_len] = '\0';

          taskENTER_CRITICAL(&EntityPage::_entity_page_spinlock);
          if (new_value_len != 0) {
            free(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value);
            state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value = new_value;
          }

          if (new_icon_len != 0) {
            free(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_icon);
            state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_icon = new_icon;
          }
          taskEXIT_CRITICAL(&EntityPage::_entity_page_spinlock);
        }

        Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value, 1000);
        Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::options[EntityPage::_selected_thermostat_option_index].icon_name, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_icon, 1000);
      }
    } else {
      std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
      state->thermostat->set_temperature += state->thermostat->step_size;
      EntityPage::_update_display_thermostat();
    }
  } else if (strcmp(data, "tempdown") == 0) {
    if (EntityPage::_is_currently_editing) {
      std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
      if (EntityPage::_selected_thermostat_option_index < state->thermostat->n_options) {
        if (state->thermostat->options[EntityPage::_selected_thermostat_option_index]->n_options <= 0) {
          return; // Do nothing, there is no options available.
        }

        // Find current index of currently selected option
        uint8_t current_index = 0;
        for (int i = state->thermostat->options[EntityPage::_selected_thermostat_option_index]->n_options - 1; i > 0; i--) {
          if (strcmp(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[i]->value, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value) == 0) {
            current_index = i;
            break;
          }
        }

        current_index--;
        if (current_index == 255) {                                                                                 // We've loop all the way around.
          current_index = state->thermostat->options[EntityPage::_selected_thermostat_option_index]->n_options - 1; // Reset index to last item to restart loop of options
        }
        bool alloc_failed = false;
        uint16_t new_value_len = strlen(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[current_index]->value);
        uint16_t new_icon_len = strlen(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[current_index]->icon);
        char *new_value = (char *)malloc(new_value_len + 1);
        char *new_icon = (char *)malloc(new_icon_len + 1);

        if (new_value == NULL) {
          alloc_failed = true;
          ESP_LOGE("EntityPage", "Failed to allocate memory for new value. New value length: %u", new_value_len);
          if (new_icon != NULL) {
            free(new_icon); // Free new icon as it won't be used when alloc failed.
          }
        } else if (new_icon == NULL) {
          alloc_failed = true;
          ESP_LOGE("EntityPage", "Failed to allocate memory for new icon. New icon length: %u", new_value_len);
          free(new_value); // Free new value as it won't be used when alloc failed.
        }

        if (!alloc_failed) [[likely]] {
          strncpy(new_value, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[current_index]->value, new_value_len);
          strncpy(new_icon, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->options[current_index]->icon, new_icon_len);
          new_value[new_value_len] = '\0';
          new_icon[new_icon_len] = '\0';

          taskENTER_CRITICAL(&EntityPage::_entity_page_spinlock);
          if (new_value_len != 0) {
            free(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value);
            state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value = new_value;
          }

          if (new_icon_len != 0) {
            free(state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_icon);
            state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_icon = new_icon;
          }
          taskEXIT_CRITICAL(&EntityPage::_entity_page_spinlock);
        }

        Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value, 1000);
        Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::options[EntityPage::_selected_thermostat_option_index].icon_name, state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_icon, 1000);
      }
    } else {
      std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
      state->thermostat->set_temperature -= state->thermostat->step_size;
      EntityPage::_update_display_thermostat();
    }
  } else {
    ESP_LOGW("EntityPage", "Unknown string event on thermostat page. Received string: %s", data);
  }
}

void EntityPage::_delete_nspanel_entity_state_object(NSPanelEntityState *object) {
  nspanel_entity_state__free_unpacked(object, NULL);
}

std::shared_ptr<NSPanelEntityState> EntityPage::_get_current_state() {
  if (xSemaphoreTake(EntityPage::_current_state_mutex, pdMS_TO_TICKS(5000)) == pdPASS) [[likely]] {
    std::shared_ptr<NSPanelEntityState> ret = EntityPage::_current_state;
    xSemaphoreGive(EntityPage::_current_state_mutex);
    return ret;
  }
  return nullptr;
}

void EntityPage::_send_thermostat_option_command() {
  if (EntityPage::_is_currently_editing) {
    NSPanelMQTTManagerCommand__ThermostatCommand command = NSPANEL_MQTTMANAGER_COMMAND__THERMOSTAT_COMMAND__INIT;
    std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
    command.thermostat_id = state->thermostat->thermostat_id;
    command.option = state->thermostat->options[EntityPage::_selected_thermostat_option_index]->name;
    command.new_value = state->thermostat->options[EntityPage::_selected_thermostat_option_index]->current_value;

    NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
    cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_THERMOSTAT_COMMAND;
    cmd.thermostat_command = &command;
    cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

    uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
    std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
    size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
    if (packed_data_size == packed_length) [[likely]] {
      if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) [[unlikely]] {
        ESP_LOGE("EntityPage", "Failed to send MQTT message with command payload.");
      }
    } else {
      ESP_LOGE("EntityPage", "Failed to pack protobuf command.");
      EntityPage::_update_display_thermostat(); // Update display to reset values to those stored
    }
  }
}

void EntityPage::_send_thermostat_setpoint_command() {
  if (!EntityPage::_is_currently_editing) {
    NSPanelMQTTManagerCommand__ThermostatTemperatureCommand command = NSPANEL_MQTTMANAGER_COMMAND__THERMOSTAT_TEMPERATURE_COMMAND__INIT;
    std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
    command.thermostat_id = state->thermostat->thermostat_id;
    command.temperature = state->thermostat->set_temperature;

    NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
    cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_THERMOSTAT_TEMPERATURE_COMMAND;
    cmd.thermostat_temperature_command = &command;
    cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

    uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
    std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
    size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
    if (packed_data_size == packed_length) [[likely]] {
      if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) [[unlikely]] {
        ESP_LOGE("EntityPage", "Failed to send MQTT message with command payload.");
      }
    } else {
      ESP_LOGE("EntityPage", "Failed to pack protobuf command.");
      EntityPage::_update_display_thermostat(); // Update display to reset values to those stored
    }

    EntityPage::_update_display_thermostat();
  }
}

void EntityPage::_update_display_media_player() {
  // There is no media player page in the GUI yet. Stop following the media player, the display stays on the entities page.
  ESP_LOGW("EntityPage", "The GUI has no media player page, can't show media player.");
  EntityPage::unshow();
}

void EntityPage::_handle_touch_event_media_player(uint16_t component_id, bool pressed) {
  // There is no media player page in the GUI yet, see _update_display_media_player.
}

void EntityPage::_media_player_play_pause() {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
    ESP_LOGE("EntityPage", "Tried to play/pause without a media player state.");
    return;
  }

  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  if (state->media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PLAYING || state->media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__BUFFERING) {
    command.playback_action = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__PLAYBACK_ACTION__PAUSE;
  } else {
    command.playback_action = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__PLAYBACK_ACTION__PLAY;
  }
  EntityPage::_send_media_player_command(&command);
}

void EntityPage::_media_player_next_track() {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.playback_action = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__PLAYBACK_ACTION__NEXT_TRACK;
  EntityPage::_send_media_player_command(&command);
}

void EntityPage::_media_player_previous_track() {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.playback_action = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__PLAYBACK_ACTION__PREVIOUS_TRACK;
  EntityPage::_send_media_player_command(&command);
}

void EntityPage::_media_player_toggle_mute() {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
    ESP_LOGE("EntityPage", "Tried to toggle mute without a media player state.");
    return;
  }

  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.has_muted = true;
  command.muted = !state->media_player->is_muted;
  EntityPage::_send_media_player_command(&command);
}

void EntityPage::_media_player_set_volume(int32_t volume) {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.has_volume = true;
  command.volume = volume;
  EntityPage::_send_media_player_command(&command);
}

void EntityPage::_media_player_set_source_volume(int32_t volume) {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.has_source_volume = true;
  command.source_volume = volume;
  EntityPage::_send_media_player_command(&command);
}

void EntityPage::_send_media_player_command(NSPanelMQTTManagerCommand__MediaPlayerCommand *command) {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
    ESP_LOGE("EntityPage", "Tried to send media player command without a media player state.");
    return;
  }
  command->media_player_id = state->media_player->media_player_id;

  NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
  cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_MEDIA_PLAYER_COMMAND;
  cmd.media_player_command = command;
  cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

  uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
  std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
  size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
  if (packed_data_size == packed_length) [[likely]] {
    if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) [[unlikely]] {
      ESP_LOGE("EntityPage", "Failed to send MQTT message with command payload.");
      EntityPage::_update_display_media_player(); // Update display to reset values to those stored
    }
  } else {
    ESP_LOGE("EntityPage", "Failed to pack protobuf command.");
    EntityPage::_update_display_media_player(); // Update display to reset values to those stored
  }
}
