#include <ConfigManager.hpp>
#include <EntitiesPage.hpp>
#include <GUI_data.hpp>
#include <HomePage.hpp>
#include <InterfaceManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <Nextion.hpp>
#include <Nextion_event.hpp>
#include <RoomManager.hpp>
#include <RoomManager_event.hpp>
#include <esp_log.h>
#include <memory>
#include <protobuf_nspanel.pb-c.h>
#include <vector>

void HomePage::show() {
  esp_log_level_set("HomePage", ConfigManager::log_level);

  std::shared_ptr<PageHandlers> handlers = InterfaceManager::begin_page(HomePage::unshow);

  Nextion::go_to_page(GUI_HOME_PAGE::page_name, 250);

  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    HomePage::_nspm_cur_config = config;
  } else {
    ESP_LOGW("HomePage", "Failed to get new config. Can not update HomePage local 'cur config'.");
  }

  HomePage::_update_display();
  handlers->add_to(RoomManager::event_loop(), ROOMMANAGER_EVENT, ESP_EVENT_ANY_ID, HomePage::_handle_roommanager_event);
  handlers->add(NEXTION_EVENT, ESP_EVENT_ANY_ID, HomePage::_handle_nextion_event);
  handlers->add(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, HomePage::_handle_config_update);

  if (HomePage::_special_mode_timer_handle == NULL) {
    // This is the first time this page is shown, create timers
    HomePage::_special_mode_create_timer_args = {};
    HomePage::_special_mode_create_timer_args.callback = &HomePage::_special_mode_timer_callback;
    HomePage::_special_mode_create_timer_args.name = "special_mode_timer";
    esp_err_t err = esp_timer_create(&HomePage::_special_mode_create_timer_args, &HomePage::_special_mode_timer_handle);
    if (err != ESP_OK) {
      ESP_LOGE("HomePage", "Failed to create 'Special mode timer'. Special mode will not be functional! Got error: %s", esp_err_to_name(err));
    }

    HomePage::_special_mode_timeout_create_timer_args = {};
    HomePage::_special_mode_timeout_create_timer_args.callback = &HomePage::_special_mode_timeout_timer_callback;
    HomePage::_special_mode_timeout_create_timer_args.name = "special_mode_timeout_timer";
    err = esp_timer_create(&HomePage::_special_mode_timeout_create_timer_args, &HomePage::_special_mode_timeout_timer_handle);
    if (err != ESP_OK) {
      ESP_LOGE("HomePage", "Failed to create 'Special mode timeout timer'. Special mode will not be functional! Got error: %s", esp_err_to_name(err));
    }
  }
}

void HomePage::unshow() {
  // Event handlers are removed by InterfaceManager (PageHandlers::retire()).
  HomePage::_current_affect_mode = HomePageAffectMode::ROOM;
  HomePage::_current_edit_mode = HomePageEditMode::ALL_LIGHTS;
}

void HomePage::set_current_affect_mode(HomePageAffectMode mode) {
  HomePage::_current_affect_mode = mode;
  HomePage::_update_display();
}

void HomePage::set_current_edit_mode(HomePageEditMode mode) {
  HomePage::_current_edit_mode = mode;
  HomePage::_update_display();
}

void HomePage::_update_display() {
  // Update displayed data
  ESP_LOGD("HomePage", "Updating displayed values on HomePage.");
  std::shared_ptr<NSPanelRoomStatus> status;
  if (HomePage::_current_affect_mode == HomePageAffectMode::ROOM) {
    if (RoomManager::get_home_page_status(&status) == ESP_OK) [[likely]] {
      Nextion::set_component_text(GUI_HOME_PAGE::mode_label_name, "Room lights", 100);
      Nextion::set_component_text(GUI_HOME_PAGE::room_label_name, status->name, 100);
      Nextion::set_component_pic(GUI_HOME_PAGE::button_scenes_name, GUI_HOME_PAGE::button_scenes_room_pic, 250);
      Nextion::set_component_pic2(GUI_HOME_PAGE::button_scenes_name, GUI_HOME_PAGE::button_scenes_room_pic2, 250);
    } else {
      ESP_LOGE("HomePage", "Failed to get status object for home page. Will abort update.");
      return;
    }
  } else if (HomePage::_current_affect_mode == HomePageAffectMode::ALL) {
    Nextion::set_component_text(GUI_HOME_PAGE::mode_label_name, "All lights", 100);
    Nextion::set_component_text(GUI_HOME_PAGE::room_label_name, "All", 100);
    Nextion::set_component_pic(GUI_HOME_PAGE::button_scenes_name, GUI_HOME_PAGE::button_scenes_all_rooms_pic, 250);
    Nextion::set_component_pic2(GUI_HOME_PAGE::button_scenes_name, GUI_HOME_PAGE::button_scenes_all_rooms_pic2, 250);
    if (RoomManager::get_home_page_status_all_rooms(&status) != ESP_OK) [[unlikely]] {
      ESP_LOGW("HomePage", "All-rooms status not yet available; slider updates deferred until MQTT data arrives.");
      return;
    }
  } else {
    ESP_LOGE("HomePage", "Unknown HomePageAffectMode, will abort update!");
    return;
  }

  if (HomePage::_current_edit_mode == HomePageEditMode::ALL_LIGHTS) [[likely]] {
    if (HomePage::_cache_ceiling_light_brightness != status->ceiling_lights_dim_level) {
      Nextion::set_component_value(GUI_HOME_PAGE::label_ceiling_name, status->ceiling_lights_dim_level, 100);
      Nextion::set_component_value(GUI_HOME_PAGE::button_ceiling_name, status->ceiling_lights_dim_level > 0, 100);
      HomePage::_cache_ceiling_light_brightness = status->ceiling_lights_dim_level;
    }

    if (HomePage::_cache_table_light_brightness != status->table_lights_dim_level) {
      Nextion::set_component_value(GUI_HOME_PAGE::label_table_name, status->table_lights_dim_level, 100);
      Nextion::set_component_value(GUI_HOME_PAGE::button_table_name, status->table_lights_dim_level > 0, 100);
      HomePage::_cache_table_light_brightness = status->table_lights_dim_level;
    }

    // Update brightness slider if it has changed and:
    // * It is set to 0
    // * It is set to the max of 100
    // * It has changed by 3 or more
    // The cases above is the only scenarios when we need to update the slider and we want to do so as little as possible.
    // This is because when the slider is updated it flickers the screen.
    uint8_t brightness_diff = abs(HomePage::_cache_brightness_slider - status->average_dim_level);
    if ((status->average_dim_level == 100 && brightness_diff != 0) || (status->average_dim_level == 0 && brightness_diff != 0) || brightness_diff >= 3) {
      Nextion::set_component_value(GUI_HOME_PAGE::dimmer_slider_name, status->average_dim_level, 100);
      HomePage::_cache_brightness_slider = status->average_dim_level;
    }

    // Update color temp slider if it has changed and:
    // * It is set to 0
    // * It is set to the max of 100
    // * It has changed by 3 or more
    // The cases above is the only scenarios when we need to update the slider and we want to do so as little as possible.
    // This is because when the slider is updated it flickers the screen.
    uint8_t color_temp_diff = abs(HomePage::_cache_color_temperature_slider - status->average_color_temperature);
    if ((status->average_color_temperature == 100 && color_temp_diff != 0) || (status->average_color_temperature == 0 && color_temp_diff != 0) || color_temp_diff >= 3) {
      Nextion::set_component_value(GUI_HOME_PAGE::color_temperature_slider_name, status->average_color_temperature, 100);
      HomePage::_cache_color_temperature_slider = status->average_color_temperature;
    }
  } else if (HomePage::_current_edit_mode == HomePageEditMode::CEILING_LIGHTS) {
    if (HomePage::_cache_ceiling_light_brightness != status->ceiling_lights_dim_level) {
      Nextion::set_component_value(GUI_HOME_PAGE::label_ceiling_name, status->ceiling_lights_dim_level, 100);
      Nextion::set_component_value(GUI_HOME_PAGE::button_ceiling_name, status->ceiling_lights_dim_level > 0, 100);
      HomePage::_cache_ceiling_light_brightness = status->ceiling_lights_dim_level;
    }

    // Update brightness slider if it has changed and:
    // * It is set to 0
    // * It is set to the max of 100
    // * It has changed by 3 or more
    // The cases above is the only scenarios when we need to update the slider and we want to do so as little as possible.
    // This is because when the slider is updated it flickers the screen.
    uint8_t brightness_diff = abs(HomePage::_cache_brightness_slider - status->ceiling_lights_dim_level);
    if ((status->ceiling_lights_dim_level == 100 && brightness_diff != 0) || (status->ceiling_lights_dim_level == 0 && brightness_diff != 0) || brightness_diff >= 3) {
      Nextion::set_component_value(GUI_HOME_PAGE::dimmer_slider_name, status->ceiling_lights_dim_level, 100);
      HomePage::_cache_brightness_slider = status->ceiling_lights_dim_level;
    }

    // Update color temp slider if it has changed and:
    // * It is set to 0
    // * It is set to the max of 100
    // * It has changed by 3 or more
    // The cases above is the only scenarios when we need to update the slider and we want to do so as little as possible.
    // This is because when the slider is updated it flickers the screen.
    uint8_t color_temp_diff = abs(HomePage::_cache_color_temperature_slider - status->ceiling_lights_color_temperature_value);
    if ((status->ceiling_lights_color_temperature_value == 100 && color_temp_diff != 0) || (status->ceiling_lights_color_temperature_value == 0 && color_temp_diff != 0) || color_temp_diff >= 3) {
      Nextion::set_component_value(GUI_HOME_PAGE::color_temperature_slider_name, status->ceiling_lights_color_temperature_value, 100);
      HomePage::_cache_color_temperature_slider = status->ceiling_lights_color_temperature_value;
    }
  } else if (HomePage::_current_edit_mode == HomePageEditMode::TABLE_LIGHTS) {
    if (HomePage::_cache_table_light_brightness != status->table_lights_dim_level) {
      Nextion::set_component_value(GUI_HOME_PAGE::label_table_name, status->table_lights_dim_level, 100);
      Nextion::set_component_value(GUI_HOME_PAGE::button_table_name, status->table_lights_dim_level > 0, 100);
      HomePage::_cache_table_light_brightness = status->table_lights_dim_level;
    }

    // Update brightness slider if it has changed and :
    // * It is set to 0
    // * It is set to the max of 100
    // * It has changed by 3 or more
    // The cases above is the only scenarios when we need to update the slider and we want to do so as little as possible.
    // This is because when the slider is updated it flickers the screen.
    uint8_t brightness_diff = abs(HomePage::_cache_brightness_slider - status->table_lights_dim_level);
    if ((status->table_lights_dim_level == 100 && brightness_diff != 0) || (status->table_lights_dim_level == 0 && brightness_diff != 0) || brightness_diff >= 3) {
      Nextion::set_component_value(GUI_HOME_PAGE::dimmer_slider_name, status->table_lights_dim_level, 100);
      HomePage::_cache_brightness_slider = status->table_lights_dim_level;
    }

    // Update color temp slider if it has changed and:
    // * It is set to 0
    // * It is set to the max of 100
    // * It has changed by 3 or more
    // The cases above is the only scenarios when we need to update the slider and we want to do so as little as possible.
    // This is because when the slider is updated it flickers the screen.
    uint8_t color_temp_diff = abs(HomePage::_cache_color_temperature_slider - status->table_lights_color_temperature_value);
    if ((status->table_lights_color_temperature_value == 100 && color_temp_diff != 0) || (status->table_lights_color_temperature_value == 0 && color_temp_diff != 0) || color_temp_diff >= 3) {
      Nextion::set_component_value(GUI_HOME_PAGE::color_temperature_slider_name, status->table_lights_color_temperature_value, 100);
      HomePage::_cache_color_temperature_slider = status->table_lights_color_temperature_value;
    }
  } else {
    ESP_LOGE("HomePage", "Unknown HomePageEditMode!");
  }
}

void HomePage::_handle_roommanager_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == roommanager_event_t::HOME_PAGE_UPDATED && HomePage::_current_affect_mode == HomePageAffectMode::ROOM) {
    HomePage::_update_display();
  } else if (event_id == roommanager_event_t::HOME_PAGE_ALL_ROOMS_UPDATED && HomePage::_current_affect_mode == HomePageAffectMode::ALL) {
    HomePage::_update_display();
  }
}

void HomePage::_handle_nextion_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case nextion_event_t::TOUCH_EVENT: {
    nextion_event_touch_t *data = (nextion_event_touch_t *)event_data;
    if (data->component_id == GUI_HOME_PAGE::button_ceiling_id) {
      HomePage::_handle_nextion_event_master_ceiling_lights_button(data->pressed);
    } else if (data->component_id == GUI_HOME_PAGE::button_table_id) {
      HomePage::_handle_nextion_event_master_table_lights_button(data->pressed);
    } else if (data->component_id == GUI_HOME_PAGE::dimmer_slider_id) {
      HomePage::_update_brightness_slider_cache();
      HomePage::_handle_brightness_slider_event();
    } else if (data->component_id == GUI_HOME_PAGE::color_temperature_slider_id) {
      HomePage::_update_color_temperature_slider_cache();
      HomePage::_handle_color_temperature_slider_event();
    } else if (data->component_id == GUI_HOME_PAGE::button_next_room_id) {
      if (HomePage::_current_affect_mode == HomePage::HomePageAffectMode::ROOM) {
        RoomManager::go_to_next_room();
      }
    } else if (data->component_id == GUI_HOME_PAGE::button_next_mode_id) {
      switch (HomePage::_current_affect_mode) {
      case HomePage::HomePageAffectMode::ROOM: {
        std::shared_ptr<NSPanelConfig> config;
        if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
          if (!config->locked_to_default_room) {
            HomePage::set_current_affect_mode(HomePageAffectMode::ALL);
          } else {
            ESP_LOGW("HomePage", "Tried to enter 'All rooms' mode but panel is not allowed to control other rooms than the default.");
          }
        } else {
          ESP_LOGE("HomePage", "Failed to get config while checking if panel is allowed to access other rooms.");
        }
        break;
      }

      case HomePage::HomePageAffectMode::ALL:
        HomePage::set_current_affect_mode(HomePageAffectMode::ROOM);
        break;

      default:
        ESP_LOGE("HomePage", "Unknown affect mode when processing 'next mode button' event!");
        break;
      }
    } else if (data->component_id == GUI_HOME_PAGE::button_scenes_id) {
      if (HomePage::_current_affect_mode == HomePage::HomePageAffectMode::ROOM && !data->pressed) {
        EntitiesPage::show(EntitiesPage::display_type_t::SCENES);
      } else if (HomePage::_current_affect_mode == HomePage::HomePageAffectMode::ALL && !data->pressed) {
        EntitiesPage::show(EntitiesPage::display_type_t::GLOBAL_SCENES);
      }
    } else if (data->component_id == GUI_HOME_PAGE::button_room_entities_id) {
      if (HomePage::_current_affect_mode == HomePage::HomePageAffectMode::ROOM && !data->pressed) {
        EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
      }
    } else {
      ESP_LOGW("HomePage", "Got touch event from unknown ID: %d", data->component_id);
    }
    break;
  }

  default:
    break;
  }
}

void HomePage::_handle_config_update(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    if (HomePage::_nspm_cur_config != nullptr && config->default_room != HomePage::_nspm_cur_config->default_room) { // Has the default page changed
      if (RoomManager::get_current_room_id() == HomePage::_nspm_cur_config->default_room) {                          // Are we currently displaying the 'default page'? If so, go to new default page.
        ESP_LOGD("HomePage", "Default room page changed. Navigating to new default room.");
        RoomManager::go_to_room_id(config->default_room);
      }
    }
    HomePage::_nspm_cur_config = config;
  } else {
    ESP_LOGW("HomePage", "Failed to get new config. Can not determine if 'default page' has changed.");
  }
}

void HomePage::_handle_nextion_event_master_ceiling_lights_button(bool pressed) {
  if (pressed) {
    if (HomePage::_current_edit_mode == HomePageEditMode::ALL_LIGHTS) {
      // We are currently pressing the "Ceiling lights master button", start timer for special mode. Only activate special mode if
      // we are currently not already in special mode.
      std::shared_ptr<NSPanelConfig> config;
      if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
        esp_timer_stop(HomePage::_special_mode_timer_handle); // Stop special mode timer in case it's running even though it shouldn't be.
        HomePage::_special_mode_timer_activation_mode = HomePageEditMode::CEILING_LIGHTS;
        esp_timer_start_once(HomePage::_special_mode_timer_handle, config->special_mode_trigger_time * 1000);
      } else {
        ESP_LOGE("HomePage", "Failed to get NSPanel Config from NSPM_ConfigManager!");
      }
    } else {
      // We pressed the button and we are currently in a special mode, exit special mode.
      if (esp_timer_is_active(HomePage::_special_mode_timer_handle) == 1) {
        esp_timer_stop(HomePage::_special_mode_timer_handle); // Stop special mode timer in case it's running even though it shouldn't be.
      }
      HomePage::_special_mode_timer_activation_mode = HomePageEditMode::ALL_LIGHTS;
      HomePage::_special_mode_timeout_timer_callback(NULL); // Revert to "normal mode"

      // This button press canceled the "special mode", ignore touch up event that follows this touch down event.
      HomePage::_ignore_next_ceiling_button_touch_up = true;
    }
  } else {
    // Should we ignore this event?
    if (HomePage::_ignore_next_ceiling_button_touch_up) {
      HomePage::_ignore_next_ceiling_button_touch_up = false;
      return;
    }
    // We are in "normal mode", proceed as normal
    if (HomePage::_current_edit_mode == HomePageEditMode::ALL_LIGHTS) {
      // We released the button and are currently not in special mode, therefore the timer
      // hasn't trigger yet so stop it ans the user hasn't held the button long enough
      // for special mode to trigger. This was a normal press of the button.
      if (esp_timer_is_active(HomePage::_special_mode_timer_handle) == 1) {
        esp_timer_stop(HomePage::_special_mode_timer_handle); // Stop special mode timer in case it's running even though it shouldn't be.
        HomePage::_special_mode_timer_activation_mode = HomePageEditMode::ALL_LIGHTS;
      }

      HomePage::_send_ceiling_master_button_command_to_manager();
    } else if (HomePage::_current_edit_mode == HomePage::TABLE_LIGHTS) {
      // We pressed the button and we are currently in a special mode, exit special mode.
      if (esp_timer_is_active(HomePage::_special_mode_timer_handle) == 1) {
        esp_timer_stop(HomePage::_special_mode_timer_handle); // Stop special mode timer in case it's running even though it shouldn't be.
      }
      HomePage::_special_mode_timer_activation_mode = HomePageEditMode::ALL_LIGHTS;
      HomePage::_special_mode_timeout_timer_callback(NULL); // Revert to "normal mode"
    }
  }
}

void HomePage::_handle_nextion_event_master_table_lights_button(bool pressed) {
  if (pressed) {
    if (HomePage::_current_edit_mode == HomePageEditMode::ALL_LIGHTS) {
      // We are currently pressing the "Table lights master button", start timer for special mode. Only activate special mode if
      // we are currently not already in special mode.
      std::shared_ptr<NSPanelConfig> config;
      if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
        esp_timer_stop(HomePage::_special_mode_timer_handle); // Stop special mode timer in case it's running even though it shouldn't be.
        HomePage::_special_mode_timer_activation_mode = HomePageEditMode::TABLE_LIGHTS;
        esp_timer_start_once(HomePage::_special_mode_timer_handle, config->special_mode_trigger_time * 1000);
      } else {
        ESP_LOGE("HomePage", "Failed to get NSPanel Config from NSPM_ConfigManager!");
      }
    } else {
      // We pressed the button and we are currently in a special mode, exit special mode.
      if (esp_timer_is_active(HomePage::_special_mode_timer_handle) == 1) {
        esp_timer_stop(HomePage::_special_mode_timer_handle); // Stop special mode timer in case it's running even though it shouldn't be.
      }
      HomePage::_special_mode_timer_activation_mode = HomePageEditMode::ALL_LIGHTS;
      HomePage::_special_mode_timeout_timer_callback(NULL); // Revert to "normal mode"

      // This button press canceled the "special mode", ignore touch up event that follows this touch down event.
      HomePage::_ignore_next_table_button_touch_up = true;
    }
  } else {
    // Should we ignore this event?
    if (HomePage::_ignore_next_table_button_touch_up) {
      HomePage::_ignore_next_table_button_touch_up = false;
      return;
    }
    // We are in "normal mode", proceed as normal
    if (HomePage::_current_edit_mode == HomePageEditMode::ALL_LIGHTS) {
      // We released the button and are currently not in special mode, therefore the timer
      // hasn't trigger yet so stop it ans the user hasn't held the button long enough
      // for special mode to trigger. This was a normal press of the button.
      if (esp_timer_is_active(HomePage::_special_mode_timer_handle) == 1) {
        esp_timer_stop(HomePage::_special_mode_timer_handle); // Stop special mode timer in case it's running even though it shouldn't be.
        HomePage::_special_mode_timer_activation_mode = HomePageEditMode::ALL_LIGHTS;
      }

      HomePage::_send_table_master_button_command_to_manager();
    } else if (HomePage::_current_edit_mode == HomePage::CEILING_LIGHTS) {
      // We pressed the button and we are currently in a special mode, exit special mode.
      if (esp_timer_is_active(HomePage::_special_mode_timer_handle) == 1) {
        esp_timer_stop(HomePage::_special_mode_timer_handle); // Stop special mode timer in case it's running even though it shouldn't be.
      }
      HomePage::_special_mode_timer_activation_mode = HomePageEditMode::ALL_LIGHTS;
      HomePage::_special_mode_timeout_timer_callback(NULL); // Revert to "normal mode"
    }
  }
}

void HomePage::_send_ceiling_master_button_command_to_manager() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK) [[unlikely]] {
    ESP_LOGE("HomePage", "Failed to get config, cannot proceed with sending command to manager.");
    return;
  }

  ESP_LOGD("HomePage", "Send ceiling master button event.");
  if (HomePage::_current_affect_mode == HomePageAffectMode::ROOM) {
    std::shared_ptr<NSPanelRoomStatus> status;
    if (RoomManager::get_home_page_status(&status) == ESP_OK) {
      if (status->num_ceiling_lights > 0) {
        NSPanelMQTTManagerCommand__FirstPageTurnLightOn turn_light_on_cmd = NSPANEL_MQTTMANAGER_COMMAND__FIRST_PAGE_TURN_LIGHT_ON__INIT;
        turn_light_on_cmd.has_brightness_value = true;
        if (status->ceiling_lights_dim_level > 0) {
          turn_light_on_cmd.brightness_slider_value = 0;
        } else {
          turn_light_on_cmd.brightness_slider_value = HomePage::_cache_brightness_slider;
          if (turn_light_on_cmd.brightness_slider_value == 0) [[unlikely]] {
            turn_light_on_cmd.brightness_slider_value = config->default_light_brightess;
          }
        }
        turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__CEILING_LIGHTS;
        turn_light_on_cmd.has_kelvin_value = true;
        turn_light_on_cmd.kelvin_slider_value = HomePage::_cache_color_temperature_slider;
        turn_light_on_cmd.global = false;
        turn_light_on_cmd.selected_room = status->id;

        NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
        cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_FIRST_PAGE_TURN_ON;
        cmd.first_page_turn_on = &turn_light_on_cmd;
        cmd.nspanel_id = config->nspanel_id;

        uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
        std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
        size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
        if (packed_data_size == packed_length) [[likely]] {
          if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) [[likely]] {
            std::shared_ptr<NSPanelConfig> config;
            if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
              std::shared_ptr<NSPanelRoomStatus> mut_status;
              if (RoomManager::get_home_page_status_mutable(&mut_status) == ESP_OK) [[likely]] {
                // Does room contain ceiling lights?
                // If all lights were off, they are now all on
                if (mut_status->ceiling_lights_dim_level == 0) {
                  mut_status->ceiling_lights_dim_level = HomePage::_cache_brightness_slider;
                  mut_status->num_ceiling_lights_on = mut_status->num_ceiling_lights;
                } else {
                  mut_status->ceiling_lights_dim_level = 0;
                  mut_status->num_ceiling_lights_on = 0;
                }
                // Calculate new average of total color temperature:
                uint16_t total_light_level = mut_status->ceiling_lights_dim_level * mut_status->num_ceiling_lights_on;
                uint16_t number_of_lights_on = mut_status->num_ceiling_lights_on;
                total_light_level += mut_status->table_lights_dim_level * mut_status->num_table_lights_on;
                number_of_lights_on += mut_status->num_table_lights_on;

                if (number_of_lights_on > 0) {
                  mut_status->average_dim_level = total_light_level / number_of_lights_on;
                }
                RoomManager::replace_home_page_status(mut_status);
              } else {
                ESP_LOGW("HomePage", "Failed to get config when checking if optimistic mode is active. Will now update optimistically.");
              }
            } else {
              ESP_LOGW("HomePage", "Failed to get config when checking if optimistic mode is active. Will now update optimistically.");
            }
          } else {
            ESP_LOGE("HomePage", "Failed to publish data while sending command to manager from 'ceiling master button'!");
          }
        } else {
          ESP_LOGE("HomePage", "Failed to pack data while sending command to manager from 'ceiling master button'!");
        }
      }
    } else {
      ESP_LOGE("HomePage", "Failed to get current page room status when trying to send 'ceiling master button' command to manager!");
    }
  } else if (HomePage::_current_affect_mode == HomePageAffectMode::ALL) {
    std::shared_ptr<NSPanelRoomStatus> status;
    if (RoomManager::get_home_page_status_all_rooms(&status) == ESP_OK) [[likely]] {
      NSPanelMQTTManagerCommand__FirstPageTurnLightOn turn_light_on_cmd = NSPANEL_MQTTMANAGER_COMMAND__FIRST_PAGE_TURN_LIGHT_ON__INIT;
      turn_light_on_cmd.has_brightness_value = true;
      if (status->ceiling_lights_dim_level > 0) {
        turn_light_on_cmd.brightness_slider_value = 0;
      } else {
        turn_light_on_cmd.brightness_slider_value = HomePage::_cache_brightness_slider;
        if (turn_light_on_cmd.brightness_slider_value == 0) [[unlikely]] {
          turn_light_on_cmd.brightness_slider_value = config->default_light_brightess;
        }
      }
      turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__CEILING_LIGHTS;
      turn_light_on_cmd.has_kelvin_value = true;
      turn_light_on_cmd.kelvin_slider_value = HomePage::_cache_color_temperature_slider;
      turn_light_on_cmd.global = true;     // Affect all rooms
      turn_light_on_cmd.selected_room = 0; // Set to 0 as it doesn't matter when "global" is set to true

      NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
      cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_FIRST_PAGE_TURN_ON;
      cmd.first_page_turn_on = &turn_light_on_cmd;
      cmd.nspanel_id = config->nspanel_id;

      uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
      std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
      size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
      if (packed_data_size == packed_length) [[likely]] {
        if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) [[likely]] {
          std::shared_ptr<NSPanelConfig> config;
          if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
            if (config->optimistic_mode) [[likely]] {
              std::shared_ptr<NSPanelRoomStatus> mutable_status;

              // We are in optimistic mode, update all rooms with new values
              if (RoomManager::get_home_page_status_mutable_all_rooms(&mutable_status) == ESP_OK) [[likely]] {
                if (status->average_dim_level == 0) {
                  mutable_status->ceiling_lights_dim_level = HomePage::_cache_brightness_slider;
                  mutable_status->num_ceiling_lights_on = mutable_status->num_ceiling_lights;
                } else {
                  mutable_status->ceiling_lights_dim_level = 0;
                  mutable_status->num_ceiling_lights_on = 0;
                }
                RoomManager::replace_home_page_status_all_rooms(mutable_status);
              } else {
                ESP_LOGW("HomePage", "Failed to get mutable room status. Will wait for return result instead.");
              }
            }
          }
        } else {
          ESP_LOGW("HomePage", "Failed to get NSPM config when checking if panel is optimistic. Will wait for return result instead.");
        }
      } else {
        ESP_LOGE("HomePage", "Failed to publish data while sending command to manager from 'ceiling master button'!");
      }
    } else {
      ESP_LOGE("HomePage", "Failed to get home page status for all rooms. Will abort!");
    }
  } else {
    ESP_LOGE("HomePage", "Failed to pack data while sending command to manager from 'ceiling master button'!");
  }
}

void HomePage::_send_table_master_button_command_to_manager() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK) [[unlikely]] {
    ESP_LOGE("HomePage", "Failed to get config, cannot proceed with sending command to manager.");
    return;
  }

  ESP_LOGD("HomePage", "Send table master button event.");
  if (HomePage::_current_affect_mode == HomePageAffectMode::ROOM) {
    std::shared_ptr<NSPanelRoomStatus> status;
    if (RoomManager::get_home_page_status(&status) == ESP_OK) {
      if (status->num_table_lights > 0) {
        // Build and send command to turn on/off table lights
        NSPanelMQTTManagerCommand__FirstPageTurnLightOn turn_light_on_cmd = NSPANEL_MQTTMANAGER_COMMAND__FIRST_PAGE_TURN_LIGHT_ON__INIT;
        turn_light_on_cmd.has_brightness_value = true;
        if (status->table_lights_dim_level > 0) {
          turn_light_on_cmd.brightness_slider_value = 0;
        } else {
          turn_light_on_cmd.brightness_slider_value = HomePage::_cache_brightness_slider;
          if (turn_light_on_cmd.brightness_slider_value == 0) [[unlikely]] {
            turn_light_on_cmd.brightness_slider_value = config->default_light_brightess;
          }
        }
        turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__TABLE_LIGHTS;
        turn_light_on_cmd.has_kelvin_value = true;
        turn_light_on_cmd.kelvin_slider_value = HomePage::_cache_color_temperature_slider;
        turn_light_on_cmd.global = false;
        turn_light_on_cmd.selected_room = status->id;

        NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
        cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_FIRST_PAGE_TURN_ON;
        cmd.first_page_turn_on = &turn_light_on_cmd;
        cmd.nspanel_id = config->nspanel_id;

        uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
        std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
        size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
        if (packed_data_size == packed_length) [[likely]] {
          if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) [[likely]] {
            std::shared_ptr<NSPanelConfig> config;
            if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
              std::shared_ptr<NSPanelRoomStatus> mut_status;
              if (RoomManager::get_home_page_status_mutable(&mut_status) == ESP_OK) [[likely]] {
                // Does room contain table lights?
                // If all lights were off, they are now all on
                if (mut_status->table_lights_dim_level == 0) {
                  mut_status->table_lights_dim_level = HomePage::_cache_brightness_slider;
                  mut_status->num_table_lights_on = mut_status->num_table_lights;
                } else {
                  mut_status->table_lights_dim_level = 0;
                  mut_status->num_table_lights_on = 0;
                }
                // Calculate new average of total color temperature:
                uint16_t total_light_level = mut_status->ceiling_lights_dim_level * mut_status->num_ceiling_lights_on;
                uint16_t number_of_lights_on = mut_status->num_ceiling_lights_on;
                total_light_level += mut_status->table_lights_dim_level * mut_status->num_table_lights_on;
                number_of_lights_on += mut_status->num_table_lights_on;

                if (number_of_lights_on > 0) {
                  mut_status->average_dim_level = total_light_level / number_of_lights_on;
                }
                RoomManager::replace_home_page_status(mut_status);
              } else {
                ESP_LOGW("HomePage", "Failed to get config when checking if optimistic mode is active. Will now update optimistically.");
              }
            } else {
              ESP_LOGW("HomePage", "Failed to get config when checking if optimistic mode is active. Will now update optimistically.");
            }
          } else {
            ESP_LOGE("HomePage", "Failed to publish data while sending command to manager from 'table master button'!");
          }
        } else {
          ESP_LOGE("HomePage", "Failed to pack data while sending command to manager from 'table master button'!");
        }
      }
    } else {
      ESP_LOGE("HomePage", "Failed to get current page room status when trying to send 'table master button' command to manager!");
    }
  } else if (HomePage::_current_affect_mode == HomePageAffectMode::ALL) {
    std::shared_ptr<NSPanelRoomStatus> status;
    if (RoomManager::get_home_page_status_all_rooms(&status) == ESP_OK) [[likely]] {
      NSPanelMQTTManagerCommand__FirstPageTurnLightOn turn_light_on_cmd = NSPANEL_MQTTMANAGER_COMMAND__FIRST_PAGE_TURN_LIGHT_ON__INIT;
      turn_light_on_cmd.has_brightness_value = true;
      if (status->table_lights_dim_level > 0) {
        turn_light_on_cmd.brightness_slider_value = 0;
      } else {
        turn_light_on_cmd.brightness_slider_value = HomePage::_cache_brightness_slider;
        if (turn_light_on_cmd.brightness_slider_value == 0) [[unlikely]] {
          turn_light_on_cmd.brightness_slider_value = config->default_light_brightess;
        }
      }
      turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__TABLE_LIGHTS;
      turn_light_on_cmd.has_kelvin_value = true;
      turn_light_on_cmd.kelvin_slider_value = HomePage::_cache_color_temperature_slider;
      turn_light_on_cmd.global = true;     // Affect all rooms
      turn_light_on_cmd.selected_room = 0; // Set to 0 as it doesn't matter when "global" is set to true

      NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
      cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_FIRST_PAGE_TURN_ON;
      cmd.first_page_turn_on = &turn_light_on_cmd;
      cmd.nspanel_id = config->nspanel_id;

      uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
      std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
      size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
      if (packed_data_size == packed_length) [[likely]] {
        if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) [[likely]] {
          std::shared_ptr<NSPanelConfig> config;
          if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
            if (config->optimistic_mode) [[likely]] {
              std::shared_ptr<NSPanelRoomStatus> mutable_status;

              // We are in optimistic mode, update all rooms with new values
              if (RoomManager::get_home_page_status_mutable_all_rooms(&mutable_status) == ESP_OK) [[likely]] {
                if (status->average_dim_level == 0) {
                  mutable_status->table_lights_dim_level = HomePage::_cache_brightness_slider;
                  mutable_status->num_table_lights_on = mutable_status->num_table_lights;
                } else {
                  mutable_status->table_lights_dim_level = 0;
                  mutable_status->num_table_lights_on = 0;
                }
                RoomManager::replace_home_page_status_all_rooms(mutable_status);
              } else {
                ESP_LOGW("HomePage", "Failed to get mutable room status. Will wait for return result instead.");
              }
            }
          }
        } else {
          ESP_LOGW("HomePage", "Failed to get NSPM config when checking if panel is optimistic. Will wait for return result instead.");
        }
      } else {
        ESP_LOGE("HomePage", "Failed to publish data while sending command to manager from 'ceiling master button'!");
      }
    } else {
      ESP_LOGE("HomePage", "Failed to get home page status for all rooms. Will abort!");
    }
  }
}

void HomePage::_update_brightness_slider_cache() {
  int32_t dimmer_slider_value;
  if (Nextion::get_component_integer_value(GUI_HOME_PAGE::dimmer_slider_name, &dimmer_slider_value, 1000, 250) == ESP_OK) {
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
      if (dimmer_slider_value >= config->raise_light_level_to_100_above) {
        HomePage::_cache_brightness_slider = 100;

        // In the case were we actually raise light level while reading it, update the slider to new value:
        Nextion::set_component_value(GUI_HOME_PAGE::dimmer_slider_name, 100, 250);
      } else {
        HomePage::_cache_brightness_slider = dimmer_slider_value;
      }
    } else {
      HomePage::_cache_brightness_slider = dimmer_slider_value;
    }
  } else {
    ESP_LOGE("HomePage", "Failed to get current color temperature value to update cache!");
  }
}

void HomePage::_update_color_temperature_slider_cache() {
  int32_t color_temperature;
  if (Nextion::get_component_integer_value(GUI_HOME_PAGE::color_temperature_slider_name, &color_temperature, 1000, 250) == ESP_OK) {
    HomePage::_cache_color_temperature_slider = color_temperature;
  } else {
    ESP_LOGE("HomePage", "Failed to get current color temperature slider value to update cache!");
  }
}

void HomePage::_special_mode_timer_callback(void *arg) {
  ESP_LOGI("HomePage", "Special mode timer triggered!");
  HomePage::_current_edit_mode = HomePage::_special_mode_timer_activation_mode;

  if (HomePage::_current_edit_mode == HomePageEditMode::CEILING_LIGHTS) {
    Nextion::set_component_visibility(GUI_HOME_PAGE::highlight_ceiling_name, true, 100);
    Nextion::set_component_visibility(GUI_HOME_PAGE::highlight_table_name, false, 100);
    Nextion::set_component_foreground(GUI_HOME_PAGE::dimmer_slider_name, GUI_HOME_PAGE::slider_highlight_color, 100);
    Nextion::set_component_foreground(GUI_HOME_PAGE::color_temperature_slider_name, GUI_HOME_PAGE::slider_highlight_color, 100);

    HomePage::_cache_table_light_brightness = -1; // Set cache to -1 so that it updates once special mode ends
    Nextion::set_component_value(GUI_HOME_PAGE::label_table_name, 0, 100);
    Nextion::set_component_value(GUI_HOME_PAGE::button_table_name, false, 100);
    HomePage::_ignore_next_ceiling_button_touch_up = true;
    HomePage::_ignore_next_table_button_touch_up = false;
  } else if (HomePage::_current_edit_mode == HomePageEditMode::TABLE_LIGHTS) {
    Nextion::set_component_visibility(GUI_HOME_PAGE::highlight_ceiling_name, false, 100);
    Nextion::set_component_visibility(GUI_HOME_PAGE::highlight_table_name, true, 100);
    Nextion::set_component_foreground(GUI_HOME_PAGE::dimmer_slider_name, GUI_HOME_PAGE::slider_highlight_color, 100);
    Nextion::set_component_foreground(GUI_HOME_PAGE::color_temperature_slider_name, GUI_HOME_PAGE::slider_highlight_color, 100);

    HomePage::_cache_ceiling_light_brightness = -1; // Set cache to -1 so that it updates once it special mode ends
    Nextion::set_component_value(GUI_HOME_PAGE::label_ceiling_name, 0, 100);
    Nextion::set_component_value(GUI_HOME_PAGE::button_ceiling_name, false, 100);
    HomePage::_ignore_next_ceiling_button_touch_up = false;
    HomePage::_ignore_next_table_button_touch_up = true;
  } else {
    ESP_LOGE("HomePage", "Unknown EditMode when processing special mode timer callback.");
  }

  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    esp_timer_stop(HomePage::_special_mode_timeout_timer_handle);
    esp_err_t result = esp_timer_start_once(HomePage::_special_mode_timeout_timer_handle, config->special_mode_release_time * 1000);
    if (result != ESP_OK) {
      ESP_LOGE("HomePage", "Error while starting timer to exit special mode! Got error: %s", esp_err_to_name(result));
    }
  } else {
    ESP_LOGW("HomePage", "Failed to get NSPanel Config from NSPM_ConfigManager. Will set default special mode timeout of 5 seconds.");
    esp_timer_stop(HomePage::_special_mode_timeout_timer_handle);
    esp_err_t result = esp_timer_start_once(HomePage::_special_mode_timeout_timer_handle, 5000 * 1000);
    if (result != ESP_OK) {
      ESP_LOGE("HomePage", "Error while restarting timer to exit special mode! Got error: %s", esp_err_to_name(result));
    }
  }

  HomePage::_update_display();
}

void HomePage::_special_mode_timeout_timer_callback(void *arg) {
  if (HomePage::_current_edit_mode != HomePageEditMode::ALL_LIGHTS) {
    // Reset to "normal mode"
    HomePage::_current_edit_mode = HomePageEditMode::ALL_LIGHTS; // Reset activation mode
    HomePage::_ignore_next_ceiling_button_touch_up = false;
    HomePage::_ignore_next_table_button_touch_up = false;

    Nextion::set_component_visibility(GUI_HOME_PAGE::highlight_ceiling_name, false, 100);
    Nextion::set_component_visibility(GUI_HOME_PAGE::highlight_table_name, false, 100);
    Nextion::set_component_foreground(GUI_HOME_PAGE::dimmer_slider_name, GUI_HOME_PAGE::slider_normal_color, 100);
    Nextion::set_component_foreground(GUI_HOME_PAGE::color_temperature_slider_name, GUI_HOME_PAGE::slider_normal_color, 100);

    HomePage::_update_display();
  }
}

void HomePage::_handle_brightness_slider_event() {
  std::shared_ptr<NSPanelRoomStatus> status;
  if (RoomManager::get_home_page_status(&status) != ESP_OK) {
    ESP_LOGE("HomePage", "Failed to get home page status when trying to update brightness slider event.");
    return;
  }

  NSPanelMQTTManagerCommand__FirstPageTurnLightOn turn_light_on_cmd = NSPANEL_MQTTMANAGER_COMMAND__FIRST_PAGE_TURN_LIGHT_ON__INIT;
  turn_light_on_cmd.has_brightness_value = true;
  turn_light_on_cmd.brightness_slider_value = HomePage::_cache_brightness_slider;
  turn_light_on_cmd.has_kelvin_value = false;
  turn_light_on_cmd.kelvin_slider_value = 0;
  turn_light_on_cmd.selected_room = status->id;

  ESP_LOGD("HomePage", "Sending command to request brightness %ld%%", turn_light_on_cmd.brightness_slider_value);

  if (HomePage::_current_edit_mode == HomePageEditMode::ALL_LIGHTS) {
    turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__ALL;
  } else if (HomePage::_current_edit_mode == HomePageEditMode::CEILING_LIGHTS) {
    turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__CEILING_LIGHTS;
  } else if (HomePage::_current_edit_mode == HomePageEditMode::TABLE_LIGHTS) {
    turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__TABLE_LIGHTS;
  } else {
    ESP_LOGE("HomePage", "Unknown edit mode when sending command from slider event, will cancel.");
    return;
  }

  if (HomePage::_current_affect_mode == HomePageAffectMode::ROOM) {
    turn_light_on_cmd.global = false;
  } else if (HomePage::_current_affect_mode == HomePageAffectMode::ALL) {
    turn_light_on_cmd.global = true;
  } else {
    ESP_LOGE("HomePage", "Unknown affect mode when sending command from slider event, will cancel.");
    return;
  }

  NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
  cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_FIRST_PAGE_TURN_ON;
  cmd.first_page_turn_on = &turn_light_on_cmd;
  cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

  uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
  std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
  size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());

  if (packed_data_size == packed_length) [[likely]] {
    if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) [[likely]] {
      std::shared_ptr<NSPanelConfig> config;
      if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
        // Get the correct mutable status object.
        std::shared_ptr<NSPanelRoomStatus> mut_status;
        if (HomePage::_current_affect_mode == HomePageAffectMode::ROOM) {
          if (RoomManager::get_home_page_status_mutable(&mut_status) == ESP_OK) [[likely]] {
          } else {
            ESP_LOGE("HomePage", "Failed to get mutable home page status. Will abort.");
            return;
          }
        } else if (HomePage::_current_affect_mode == HomePageAffectMode::ALL) {
          if (RoomManager::get_home_page_status_mutable_all_rooms(&mut_status) == ESP_OK) [[likely]] {
          } else {
            ESP_LOGE("HomePage", "Failed to get mutable home page status. Will abort.");
            return;
          }
        } else {
          ESP_LOGW("HomePage", "Failed to get config when checking if optimistic mode is active. Will not update optimistically.");
          return;
        }

        if (mut_status->ceiling_lights_dim_level == 0 && mut_status->table_lights_dim_level == 0) {
          mut_status->ceiling_lights_dim_level = HomePage::_cache_brightness_slider;
          mut_status->num_ceiling_lights_on = mut_status->num_ceiling_lights;
          mut_status->table_lights_dim_level = HomePage::_cache_brightness_slider;
          mut_status->num_table_lights_on = mut_status->num_table_lights;
        } else if (mut_status->ceiling_lights_dim_level > 0 && mut_status->table_lights_dim_level == 0) {
          mut_status->ceiling_lights_dim_level = HomePage::_cache_brightness_slider;
          mut_status->num_ceiling_lights_on = mut_status->num_ceiling_lights;
        } else if (mut_status->ceiling_lights_dim_level == 0 && mut_status->table_lights_dim_level > 0) {
          mut_status->table_lights_dim_level = HomePage::_cache_brightness_slider;
          mut_status->num_table_lights_on = mut_status->num_table_lights;
        }
        // Calculate new average of total color temperature:
        uint16_t total_light_level = mut_status->ceiling_lights_dim_level * mut_status->num_ceiling_lights_on;
        uint16_t number_of_lights_on = mut_status->num_ceiling_lights_on;
        total_light_level += mut_status->table_lights_dim_level * mut_status->num_table_lights_on;
        number_of_lights_on += mut_status->num_table_lights_on;

        if (number_of_lights_on > 0) {
          mut_status->average_dim_level = total_light_level / number_of_lights_on;
        }
        if (HomePage::_current_affect_mode == HomePageAffectMode::ROOM) {
          if (RoomManager::replace_home_page_status(mut_status) != ESP_OK) [[unlikely]] {
            ESP_LOGE("HomePage", "Failed to get mutable home page status. Will abort.");
            return;
          }
        } else if (HomePage::_current_affect_mode == HomePageAffectMode::ALL) {
          if (RoomManager::replace_home_page_status_all_rooms(mut_status) != ESP_OK) [[unlikely]] {
            ESP_LOGE("HomePage", "Failed to get mutable home page status. Will abort.");
            return;
          }
        } else {
          ESP_LOGW("HomePage", "Failed to get config when checking if optimistic mode is active. Will not update optimistically.");
          return;
        }

      } else {
        ESP_LOGW("HomePage", "Failed to get config when checking if optimistic mode is active. Will not update optimistically.");
      }
    } else {
      ESP_LOGE("HomePage", "Failed to publish data while sending command to manager from 'brightness slider event'!");
    }
  } else {
    ESP_LOGE("HomePage", "Failed to pack data while sending command to manager from 'brightness slider event'!");
  }
}

void HomePage::_handle_color_temperature_slider_event() {
  std::shared_ptr<NSPanelRoomStatus> status;
  if (RoomManager::get_home_page_status(&status) != ESP_OK) {
    ESP_LOGE("HomePage", "Failed to get home page status when trying to update color temperature slider event.");
    return;
  }

  NSPanelMQTTManagerCommand__FirstPageTurnLightOn turn_light_on_cmd = NSPANEL_MQTTMANAGER_COMMAND__FIRST_PAGE_TURN_LIGHT_ON__INIT;
  turn_light_on_cmd.has_brightness_value = false;
  turn_light_on_cmd.brightness_slider_value = 0;
  turn_light_on_cmd.has_kelvin_value = true;
  turn_light_on_cmd.kelvin_slider_value = HomePage::_cache_color_temperature_slider;
  turn_light_on_cmd.selected_room = status->id;

  ESP_LOGD("HomePage", "Sending command to request color temp %ld%%", turn_light_on_cmd.kelvin_slider_value);

  if (HomePage::_current_edit_mode == HomePageEditMode::ALL_LIGHTS) {
    turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__ALL;
  } else if (HomePage::_current_edit_mode == HomePageEditMode::CEILING_LIGHTS) {
    turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__CEILING_LIGHTS;
  } else if (HomePage::_current_edit_mode == HomePageEditMode::TABLE_LIGHTS) {
    turn_light_on_cmd.affect_lights = NSPANEL_MQTTMANAGER_COMMAND__AFFECT_LIGHTS_OPTIONS__TABLE_LIGHTS;
  } else {
    ESP_LOGE("HomePage", "Unknown edit mode when sending command from slider event, will cancel.");
    return;
  }

  if (HomePage::_current_affect_mode == HomePageAffectMode::ROOM) {
    turn_light_on_cmd.global = false;
  } else if (HomePage::_current_affect_mode == HomePageAffectMode::ALL) {
    turn_light_on_cmd.global = true;
  } else {
    ESP_LOGE("HomePage", "Unknown affect mode when sending command from slider event, will cancel.");
    return;
  }

  NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
  cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_FIRST_PAGE_TURN_ON;
  cmd.first_page_turn_on = &turn_light_on_cmd;
  cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

  uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
  std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
  size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
  if (packed_data_size == packed_length) {
    if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) {
      // TODO: Handle optimistic mode
      // NSPanelConfig config;
      // if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
      //   if (config.optimistic_mode) {
      //     if (status.ceiling_lights_dim_level == 0 && status.table_lights_dim_level == 0) {
      //       status.average_dim_level = HomePage::_cache_brightness_slider;
      //       status.ceiling_lights_dim_level = HomePage::_cache_brightness_slider;
      //       HomePage::_update_display();
      //     }
      //     // TODO: Update total average light with weighted values when average light is not 0
      //   }
      // } else {
      //   ESP_LOGW("HomePage", "Failed to get NSPM config when checking if panel is optimistic. Will wait for return result instead.");
      // }
    } else {
      ESP_LOGE("HomePage", "Failed to publish data while sending command to manager from 'color temperature slider'!");
    }
  } else {
    ESP_LOGE("HomePage", "Failed to pack data while sending command to manager from 'color temperature slider'!");
  }
}