#include <algorithm>
#include <ConfigManager.hpp>
#include <CoverPage.hpp>
#include <EntitiesPage.hpp>
#include <EntityPage.hpp>
#include <GUI_data.hpp>
#include <InterfaceManager.hpp>
#include <LightView.hpp>
#include <LvglText.hpp>
#include <LvglUi.hpp>
#include <ThermostatView.hpp>
#include <MediaPlayerPage.hpp>
#include <MqttManager.hpp>
#include <PanelMetrics.hpp>
#include <NSPM_ConfigManager.hpp>
#include <PsramProtobuf.hpp>
#include <Nextion.hpp>
#include <Nextion_event.hpp>
#include <StatusUpdateManager.hpp>
#include <esp_log.h>
#include <protobuf_nspanel_entity.pb-c.h>

void EntityPage::show(std::string state_topic) {
  esp_log_level_set("EntityPage", ConfigManager::log_level);
  esp_log_level_set("MediaPlayerPage", ConfigManager::log_level);

  if (EntityPage::_current_state_mutex == NULL) {
    EntityPage::_current_state_mutex = xSemaphoreCreateMutex();
  }

  // Reset variables:
  EntityPage::_selected_thermostat_option_index = 0;
  EntityPage::_is_currently_editing = false;

  if (LvglUi::active()) {
    LightView::on_action = [](LightView::Action action, int32_t value) {
      LvglUi::defer([action, value]() { EntityPage::_lvgl_light_action((int)action, value); });
    };
    ThermostatView::on_action = [](ThermostatView::Action action, int32_t option) {
      LvglUi::defer([action, option]() { EntityPage::_lvgl_thermostat_action((int)action, option); });
    };
  }

  // Last, once the page is ready for it: a cached state is delivered straight away, on mqtt_dispatch.
  // Taking the new hold before the old one is released keeps a re-shown entity's topic subscribed.
  std::lock_guard<std::mutex> lock(EntityPage::_subscription_mutex);
  uint32_t generation = ++EntityPage::_generation;
  EntityPage::_current_entity_mqtt_topic = state_topic;
  EntityPage::_subscription = mqtt::subscribe(std::move(state_topic), mqtt::Kind::STATE, &EntityPage::_handle_state,
                                              reinterpret_cast<void *>(static_cast<uintptr_t>(generation)), PanelMetrics::HANDLER_ENTITY_PAGE);
  if (!EntityPage::_subscription.active()) [[unlikely]] {
    ESP_LOGE("EntityPage", "Failed to subscribe to entity state topic %s.", EntityPage::_current_entity_mqtt_topic.c_str());
  }
}

void EntityPage::unshow() {
  CoverPage::unshow();
  // The Nextion handler stops with the page: InterfaceManager only dispatches to the current page.
  // Releasing the hold lets the topic linger, so going back to the same entity shows its state at once.
  {
    std::lock_guard<std::mutex> lock(EntityPage::_subscription_mutex);
    ++EntityPage::_generation;
    EntityPage::_subscription.reset();
  }
  EntityPage::_currently_showing = false;
  MediaPlayerPage::unshow();
}

void EntityPage::_handle_state(const mqtt::Message &message, void *context) {
  // An empty payload is a cleared retained state. A callback from a hold released since (another
  // entity shown, or the page left) carries an older generation.
  if (message.payload.empty() || static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context)) != EntityPage::_generation.load()) {
    return;
  }
  NSPanelEntityState *new_state = nspanel_entity_state__unpack(psram_protobuf_allocator(), message.payload.size(), message.payload.data());
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
        MediaPlayerPage::handle_state(state->media_player);
      } else if (state->entity_case == NSPanelEntityState__EntityCase::NSPANEL_ENTITY_STATE__ENTITY_COVER) {
        EntityPage::_current_mode = _entity_page_modes::COVER;
      } else {
        ESP_LOGE("EntityPage", "Unknown entity state case!");
      }

      EntityPage::_update_display();
    } else {
      ESP_LOGE("EntityPage", "Failed to take mutex to update current state.");
      nspanel_entity_state__free_unpacked(new_state, psram_protobuf_allocator());
    }
  } else {
    ESP_LOGE("EntityPage", "Received new state but failed to parse into protobuf object.");
  }
}

void EntityPage::_update_display() {
  ESP_LOGD("EntityPage", "Updating displayed value via delegator.");
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr) [[unlikely]] {
    return;
  }

  switch (state->entity_case) {
  case NSPANEL_ENTITY_STATE__ENTITY_LIGHT:
    EntityPage::_update_display_light();
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT:
    EntityPage::_update_display_thermostat();
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER:
    EntityPage::_update_display_media_player();
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_COVER:
    EntityPage::_update_display_cover();
    break;

  default:
    ESP_LOGE("EntityPage", "Unknown state type. Can't call appropriate update display function.");
    break;
  }
}

void EntityPage::_handle_nextion_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // Take our own reference for the whole event. If we went through _current_state directly, nothing would
  // keep the state alive when a new state arrives on the MQTT task and replaces it mid-handler.
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr) { // No state received yet. Nothing to act on.
    return;
  }

  if (event_id == nextion_event_t::TOUCH_EVENT) {
    nextion_event_touch_t *touch_data = (nextion_event_touch_t *)event_data;
    EntityPage::_handle_touch_event(state, touch_data->component_id, touch_data->pressed);
  } else if (event_id == nextion_event_t::STRING_EVENT) {
    switch (state->entity_case) {
    case NSPANEL_ENTITY_STATE__ENTITY_LIGHT:
      break;

    case NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT:
      EntityPage::_handle_string_event_thermostat(state, (char *)event_data);
      break;

    case NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER:
      break;

    case NSPANEL_ENTITY_STATE__ENTITY_COVER:
      CoverPage::handle_string_event(state, (char *)event_data);
      break;

    default:
      ESP_LOGE("EntityPage", "Unknown state type. Can't call appropriate string event function.");
      break;
    }
  }
}

void EntityPage::_handle_config_update(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
}

void EntityPage::_handle_touch_event(const std::shared_ptr<NSPanelEntityState> &state, uint16_t component_id, bool pressed) {
  switch (state->entity_case) {
  case NSPANEL_ENTITY_STATE__ENTITY_LIGHT:
    EntityPage::_handle_touch_event_light(state, component_id, pressed);
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT:
    break;

  case NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER:
    MediaPlayerPage::handle_touch(component_id, pressed);
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

    std::shared_ptr<PageHandlers> handlers = InterfaceManager::begin_page(EntityPage::unshow);

    handlers->add(NEXTION_EVENT, ESP_EVENT_ANY_ID, &EntityPage::_handle_nextion_event);
  }

  if (LvglUi::active()) {
    EntityPage::_lvgl_render_light();
    return;
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

void EntityPage::_handle_touch_event_light(const std::shared_ptr<NSPanelEntityState> &state, uint16_t component_id, bool pressed) {
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
    int32_t light_id = state->light->light_id;
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
    int32_t light_id = state->light->light_id;
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
    int32_t light_id = state->light->light_id;
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

void EntityPage::_update_display_cover() {
  ESP_LOGD("EntityPage", "Updating EntityPage with cover state.");
  bool page_just_opened = false;
  if (!EntityPage::_currently_showing) {
    ESP_LOGD("EntityPage", "Switching page to %s", GUI_COVER_CONTROL_PAGE::page_name);
    EntityPage::_currently_showing = true;
    if (Nextion::go_to_page(GUI_COVER_CONTROL_PAGE::page_name, 1000) != ESP_OK) [[unlikely]] {
      // A stock TFT has no cover page yet. Clear the flag before backing out, otherwise the next
      // state update would take the "already showing" path and write cover components onto
      // whatever page is actually displayed.
      ESP_LOGE("EntityPage", "Failed to navigate to the cover page. Will go back.");
      EntityPage::_currently_showing = false;
      EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
      return;
    }

    std::shared_ptr<PageHandlers> handlers = InterfaceManager::begin_page(EntityPage::unshow);
    handlers->add(NEXTION_EVENT, ESP_EVENT_ANY_ID, &EntityPage::_handle_nextion_event);
    page_just_opened = true;
  }

  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  CoverPage::update_display(state->cover, page_just_opened);
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

    std::shared_ptr<PageHandlers> handlers = InterfaceManager::begin_page(EntityPage::unshow);

    handlers->add(NEXTION_EVENT, ESP_EVENT_ANY_ID, &EntityPage::_handle_nextion_event);
  }

  if (LvglUi::active()) {
    EntityPage::_lvgl_render_thermostat();
    return;
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
    if (StatusUpdateManager::has_valid_temperature()) {
      chars_written = snprintf(buf, sizeof(buf), "%.1f°", StatusUpdateManager::current_temperature());
    } else {
      chars_written = snprintf(buf, sizeof(buf), "--");
    }
  }

  if (chars_written > 0) {
    Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::temperature_label_name, buf, 1000);
  } else {
    ESP_LOGE("EntityPage", "Failed to snprintf current temp to temperature buffer.");
  }
}

void EntityPage::_handle_string_event_thermostat(const std::shared_ptr<NSPanelEntityState> &state, char *data) {
  ESP_LOGD("EntityPage", "Thermostat page received string event data: %s", data);

  if (strcmp(data, "activate:set1") == 0) {
    if (state->thermostat->n_options >= 1) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set1");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 0;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, state->thermostat->options[0]->current_value, 1000);
    }
  } else if (strcmp(data, "activate:set2") == 0) {
    if (state->thermostat->n_options >= 2) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set2");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 1;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, state->thermostat->options[1]->current_value, 1000);
    }
  } else if (strcmp(data, "activate:set3") == 0) {
    if (state->thermostat->n_options >= 3) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set3");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 2;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, state->thermostat->options[2]->current_value, 1000);
    }
  } else if (strcmp(data, "activate:set4") == 0) {
    if (state->thermostat->n_options >= 4) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set4");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 3;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, state->thermostat->options[3]->current_value, 1000);
    }
  } else if (strcmp(data, "activate:set5") == 0) {
    if (state->thermostat->n_options >= 5) {
      EntityPage::_send_thermostat_option_command();
      ESP_LOGD("EntityPage", "Activating thermostat options set5");
      EntityPage::_is_currently_editing = true;
      EntityPage::_selected_thermostat_option_index = 4;
      Nextion::set_component_text(GUI_THERMOSTAT_CONTROL_PAGE::set_label_name, state->thermostat->options[4]->current_value, 1000);
    }
  } else if (strcmp(data, "deactivate") == 0) { // We've edited an option. Send updated state to manager.
    EntityPage::_send_thermostat_option_command();
    EntityPage::_is_currently_editing = false;
    EntityPage::_update_display_thermostat();
  } else if (strcmp(data, "deactivatetemp") == 0) { // We've changed the temperature. Send updated temperature to manager.
    EntityPage::_send_thermostat_setpoint_command();
  } else if (strcmp(data, "back") == 0) {
    // Send what is pending before leaving: sending redraws this page, and once the entities page has
    // begun, EntityPage's unshow has cleared _currently_showing, so that redraw would open the
    // thermostat page again (on Office: "back" did nothing).
    if (EntityPage::_is_currently_editing) [[unlikely]] { // We are currently in editing mode, send the current option.
      EntityPage::_send_thermostat_option_command();
    } else {
      EntityPage::_send_thermostat_setpoint_command();
    }
    EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
  } else if (strcmp(data, "tempup") == 0) {
    if (EntityPage::_is_currently_editing) {
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
      state->thermostat->set_temperature += state->thermostat->step_size;
      EntityPage::_update_display_thermostat();
    }
  } else if (strcmp(data, "tempdown") == 0) {
    if (EntityPage::_is_currently_editing) {
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
      state->thermostat->set_temperature -= state->thermostat->step_size;
      EntityPage::_update_display_thermostat();
    }
  } else {
    ESP_LOGW("EntityPage", "Unknown string event on thermostat page. Received string: %s", data);
  }
}

void EntityPage::_delete_nspanel_entity_state_object(NSPanelEntityState *object) {
  nspanel_entity_state__free_unpacked(object, psram_protobuf_allocator());
}

std::shared_ptr<NSPanelEntityState> EntityPage::_get_current_state() {
  if (xSemaphoreTake(EntityPage::_current_state_mutex, pdMS_TO_TICKS(5000)) == pdPASS) [[likely]] {
    std::shared_ptr<NSPanelEntityState> ret = EntityPage::_current_state;
    xSemaphoreGive(EntityPage::_current_state_mutex);
    return ret;
  }
  return nullptr;
}

std::shared_ptr<NSPanelEntityState> EntityPage::get_current_state() {
  return EntityPage::_get_current_state();
}

std::string EntityPage::get_current_state_topic() {
  return EntityPage::_current_entity_mqtt_topic;
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
  ESP_LOGI("EntityPage", "Updating EntityPage with media player state.");
  bool page_just_opened = false;
  if (!EntityPage::_currently_showing) {
    ESP_LOGD("EntityPage", "Switching page to %s", GUI_MEDIA_PLAYER_CONTROL_PAGE::page_name);
    EntityPage::_currently_showing = true;
    if (Nextion::go_to_page(GUI_MEDIA_PLAYER_CONTROL_PAGE::page_name, 1000) != ESP_OK) [[unlikely]] {
      // The official HMI has no media player page yet, so on a stock TFT this fails every time
      // rather than never. Clear the flag before backing out: otherwise the next state update
      // takes the "already showing" path, skips the page switch, and writes the media player
      // components onto whatever page is actually displayed.
      ESP_LOGE("EntityPage", "Failed to navigate Nextion to page. Will go back.");
      EntityPage::_currently_showing = false;
      EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
      return;
    }

    std::shared_ptr<PageHandlers> handlers = InterfaceManager::begin_page(EntityPage::unshow);

    handlers->add(NEXTION_EVENT, ESP_EVENT_ANY_ID, &EntityPage::_handle_nextion_event);
    page_just_opened = true;
  }

  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  MediaPlayerPage::update_display(state->media_player, page_just_opened);
}

// --- LVGL -----------------------------------------------------------------------------------------

// The manager's thermostat option icons are characters of the HMI's icon font (MQTTManager's
// entity_icons.hpp); these are the closest Material Design Icons in the 24 px font.
static const char *thermostat_icon(const char *glyph) {
  switch (glyph != nullptr ? glyph[0] : 0) {
  case '!': return NSPM_ICON_FIRE;                  // heating
  case '8': return NSPM_ICON_SNOWFLAKE;             // cooling
  case '#': return NSPM_ICON_SUN_SNOWFLAKE_VARIANT; // heat/cool
  case '$': return NSPM_ICON_HVAC;                  // auto
  case '%': return NSPM_ICON_WATER_PERCENT;         // dry
  case '&': return NSPM_ICON_LEAF;                  // eco
  case '\'': return NSPM_ICON_SHIELD_OFF;           // away
  case '(': return NSPM_ICON_HOME;                  // home
  case ')': return NSPM_ICON_SLEEP;                 // sleep
  case '*': return NSPM_ICON_FIRE;                  // boost
  case '+': return NSPM_ICON_SOFA;                  // comfort
  case ',': return NSPM_ICON_GESTURE_TAP_BUTTON;    // activity
  case '-': return NSPM_ICON_SNOWFLAKE;             // defrosting
  case '.':
  case '/':
  case '0': return NSPM_ICON_ARROW_OSCILLATING;     // swing
  case '1': return NSPM_ICON_POWER;                 // fan off
  case '2': return NSPM_ICON_FAN_AUTO;              // fan auto
  case '3':
  case '4':
  case '5':
  case '6': return NSPM_ICON_FAN;                   // fan, fan speeds
  case 'h': return NSPM_ICON_POWER;                 // off
  default: return NSPM_ICON_THERMOSTAT;
  }
}

static std::string temperature_text(float value) {
  char text[16];
  bool fahrenheit = true;
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK && config) {
    fahrenheit = config->use_fahrenheit;
  }
  snprintf(text, sizeof(text), "%.1f\xC2\xB0%s", value, fahrenheit ? "F" : "C");
  return text;
}

void EntityPage::_lvgl_render_light() {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_LIGHT || !InterfaceManager::is_current_page(EntityPage::unshow)) {
    return;
  }
  const NSPanelEntityState__Light *light = state->light;
  LightState view;
  view.name = LvglText::utf8(light->name);
  view.can_color = light->can_color;
  view.can_temperature = light->can_color_temp;
  // As the Nextion page: a light that does only one shows that one, whatever mode it reports.
  view.color_mode = light->can_color && (!light->can_color_temp || EntityPage::_current_mode == _entity_page_modes::LIGHT_RGB);
  view.brightness = light->brightness;
  view.color_temperature = light->color_temp;
  view.saturation = light->saturation;
  view.hue = light->hue;
  LvglUi::show(LvglUi::View::LIGHT, LightView::build, [view]() { LightView::show(view); });
}

// Send a light command for the light shown, with one field set by `fill`.
static void send_light_command(const std::shared_ptr<NSPanelEntityState> &state, void (*fill)(NSPanelMQTTManagerCommand__LightCommand &, int32_t), int32_t value) {
  int32_t light_id = state->light->light_id;
  NSPanelMQTTManagerCommand__LightCommand light_command = NSPANEL_MQTTMANAGER_COMMAND__LIGHT_COMMAND__INIT;
  light_command.light_ids = &light_id;
  light_command.n_light_ids = 1;
  fill(light_command, value);

  NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
  cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_LIGHT_COMMAND;
  cmd.light_command = &light_command;
  cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();
  uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
  std::vector<uint8_t> buffer(packed_length);
  if (nspanel_mqttmanager_command__pack(&cmd, buffer.data()) != packed_length) [[unlikely]] {
    ESP_LOGE("EntityPage", "Failed to pack protobuf command.");
    return;
  }
  if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) [[unlikely]] {
    ESP_LOGE("EntityPage", "Failed to send MQTT message with command payload.");
  }
}

void EntityPage::_lvgl_light_action(int action, int32_t value) {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_LIGHT || !InterfaceManager::is_current_page(EntityPage::unshow)) {
    return;
  }
  switch ((LightView::Action)action) {
  case LightView::Action::BACK:
    EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
    return;
  case LightView::Action::SWITCH_MODE:
    EntityPage::_current_mode = EntityPage::_current_mode == _entity_page_modes::LIGHT_RGB ? _entity_page_modes::LIGHT_COLOR_TEMPERATURE : _entity_page_modes::LIGHT_RGB;
    break;
  case LightView::Action::BRIGHTNESS:
    send_light_command(state, [](NSPanelMQTTManagerCommand__LightCommand &c, int32_t v) { c.has_brightness = true; c.brightness = v; }, value);
    break;
  case LightView::Action::COLOR_TEMPERATURE:
    send_light_command(state, [](NSPanelMQTTManagerCommand__LightCommand &c, int32_t v) { c.has_color_temperature = true; c.color_temperature = v; }, value);
    break;
  case LightView::Action::SATURATION:
    send_light_command(state, [](NSPanelMQTTManagerCommand__LightCommand &c, int32_t v) { c.has_saturation = true; c.saturation = v; }, value);
    break;
  case LightView::Action::HUE:
    send_light_command(state, [](NSPanelMQTTManagerCommand__LightCommand &c, int32_t v) { c.has_hue = true; c.hue = v; }, value);
    break;
  }
  EntityPage::_lvgl_render_light(); // The slider stays where it was let go until the state follows.
}

void EntityPage::_lvgl_render_thermostat() {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT || !InterfaceManager::is_current_page(EntityPage::unshow)) {
    return;
  }
  const NSPanelEntityState__Thermostat *thermostat = state->thermostat;
  ThermostatState view;
  view.name = LvglText::utf8(thermostat->name);
  if (thermostat->has_current_temperature) {
    view.current = temperature_text(thermostat->current_temperature);
  } else if (StatusUpdateManager::has_valid_temperature()) {
    view.current = temperature_text(StatusUpdateManager::current_temperature());
  } else {
    view.current = "--"; // The internal sensor has no reading yet.
  }
  // tempup/tempdown replace current_value/current_icon under the spinlock: copy them under it too,
  // into fixed buffers (no allocation with interrupts off).
  char names[ThermostatState::MAX_OPTIONS][32] = {}, values[ThermostatState::MAX_OPTIONS][32] = {};
  char icons[ThermostatState::MAX_OPTIONS] = {};
  const size_t count = std::min<size_t>(thermostat->n_options, ThermostatState::MAX_OPTIONS);
  taskENTER_CRITICAL(&EntityPage::_entity_page_spinlock);
  const bool editing = EntityPage::_is_currently_editing && EntityPage::_selected_thermostat_option_index < thermostat->n_options;
  for (size_t i = 0; i < count; i++) {
    const NSPanelEntityState__Thermostat__ThermostatOption *option = thermostat->options[i];
    strncpy(names[i], option->name != NULL ? option->name : "", sizeof(names[i]) - 1);
    strncpy(values[i], option->current_value != NULL ? option->current_value : "", sizeof(values[i]) - 1);
    icons[i] = option->current_icon != NULL ? option->current_icon[0] : 0;
  }
  taskEXIT_CRITICAL(&EntityPage::_entity_page_spinlock);
  for (size_t i = 0; i < count; i++) {
    const char glyph[2] = {icons[i], 0};
    view.options[i].name = LvglText::utf8(names[i]);
    view.options[i].value = LvglText::utf8(values[i]);
    view.options[i].icon = thermostat_icon(glyph);
  }
  if (editing) {
    view.selected = EntityPage::_selected_thermostat_option_index;
    view.set_label = view.options[view.selected].name;
    view.set_value = view.options[view.selected].value;
  } else {
    view.set_label = "Set";
    view.set_value = temperature_text(thermostat->set_temperature);
  }
  LvglUi::show(LvglUi::View::THERMOSTAT, ThermostatView::build, [view]() { ThermostatView::show(view); });
}

void EntityPage::_lvgl_thermostat_timer(void *arg) {
  // esp_timer's task: hand over, as the HMI's timers sent their strings to the firmware.
  LvglUi::defer([]() {
    std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
    if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT || !InterfaceManager::is_current_page(EntityPage::unshow)) {
      return;
    }
    char event[16];
    strcpy(event, EntityPage::_is_currently_editing ? "deactivate" : "deactivatetemp");
    EntityPage::_handle_string_event_thermostat(state, event);
    EntityPage::_lvgl_render_thermostat();
  });
}

void EntityPage::_lvgl_thermostat_action(int action, int32_t option) {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::_get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_THERMOSTAT || !InterfaceManager::is_current_page(EntityPage::unshow)) {
    return;
  }
  if (EntityPage::_lvgl_thermostat_timer_handle == NULL) {
    esp_timer_create_args_t args = {};
    args.callback = EntityPage::_lvgl_thermostat_timer;
    args.name = "thermostat_set";
    if (esp_timer_create(&args, &EntityPage::_lvgl_thermostat_timer_handle) != ESP_OK) {
      ESP_LOGE("EntityPage", "Failed to create the thermostat timer; changes are sent only when leaving the page.");
    }
  }
  auto restart_timer = []() {
    if (EntityPage::_lvgl_thermostat_timer_handle != NULL) {
      esp_timer_stop(EntityPage::_lvgl_thermostat_timer_handle);
      esp_timer_start_once(EntityPage::_lvgl_thermostat_timer_handle, 3000 * 1000);
    }
  };
  auto stop_timer = []() {
    if (EntityPage::_lvgl_thermostat_timer_handle != NULL) {
      esp_timer_stop(EntityPage::_lvgl_thermostat_timer_handle);
    }
  };
  // The strings the HMI's thermostat page sends (thermostat.nx), handled as they always were.
  char event[16] = "";
  switch ((ThermostatView::Action)action) {
  case ThermostatView::Action::BACK:
    stop_timer();
    strcpy(event, "back"); // Sends what is pending, then leaves.
    EntityPage::_handle_string_event_thermostat(state, event);
    return;
  case ThermostatView::Action::DOWN:
  case ThermostatView::Action::UP:
    strcpy(event, (ThermostatView::Action)action == ThermostatView::Action::UP ? "tempup" : "tempdown");
    EntityPage::_handle_string_event_thermostat(state, event);
    restart_timer(); // setTimer while an option is active, setTempTimer otherwise: 3 s either way.
    break;
  case ThermostatView::Action::OPTION:
    if (option < 0 || option >= ThermostatState::MAX_OPTIONS) {
      return;
    }
    if (EntityPage::_is_currently_editing && EntityPage::_selected_thermostat_option_index == option) {
      stop_timer(); // The active option tapped again: done with it.
      strcpy(event, "deactivate");
    } else {
      snprintf(event, sizeof(event), "activate:set%d", (int)option + 1);
      restart_timer();
    }
    EntityPage::_handle_string_event_thermostat(state, event);
    break;
  default:
    return; // PREV/NEXT: one thermostat per page, as on the Nextion.
  }
  EntityPage::_lvgl_render_thermostat();
}
