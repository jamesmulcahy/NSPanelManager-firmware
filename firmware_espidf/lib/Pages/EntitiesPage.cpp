#include <BuzzerManager.hpp>
#include <ConfigManager.hpp>
#include <EntitiesPage.hpp>
#include <EntityPage.hpp>
#include <GUI_data.hpp>
#include <HomePage.hpp>
#include <InterfaceManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <Nextion.hpp>
#include <Nextion_event.hpp>
#include <RoomManager.hpp>
#include <RoomManager_event.hpp>
#include <esp_log.h>
#include <string>

void EntitiesPage::show(display_type_t display_type) {
  esp_log_level_set("EntitiesPage", ConfigManager::log_level);
  EntitiesPage::_display_type = display_type;

  std::shared_ptr<PageHandlers> handlers = InterfaceManager::begin_page(EntitiesPage::unshow);

  // EntitiesPage::_update_display();
  if (handlers->add_to(RoomManager::event_loop(), ROOMMANAGER_EVENT, ESP_EVENT_ANY_ID, EntitiesPage::_handle_roommanager_event) != ESP_OK) [[unlikely]] {
    ESP_LOGE("EntitiesPage", "Failed to register event handler for EntitiesPage to RoomManager. Will return on HomePage.");
    HomePage::show();
  }

  if (handlers->add(NEXTION_EVENT, ESP_EVENT_ANY_ID, EntitiesPage::_handle_nextion_event) != ESP_OK) [[unlikely]] {
    ESP_LOGE("EntitiesPage", "Failed to register event handler for EntitiesPage to Nextion. Will return on HomePage.");
    HomePage::show();
  }

  // If current room has an entities page, go to it and if not, show the next available entities page.
  switch (display_type) {
  case display_type_t::ENTITIES:
    RoomManager::go_to_first_entities_page();
    break;

  case display_type_t::SCENES:
    RoomManager::go_to_first_scenes_page();
    break;

  case display_type_t::GLOBAL_SCENES:
    RoomManager::go_to_first_global_scenes_page();
    break;

  default:
    ESP_LOGE("EntitiesPage", "Unknown display type! Will return to HomePage.");
    HomePage::show();
    break;
  }

  // if (RoomManager::get_current_room_entities_page_status(&EntitiesPage::_current_entities_page) != ESP_OK) [[unlikely]] {
  //   ESP_LOGE("EntitiesPage", "Failed to get current entities page. Will return to HomePage.");
  //   HomePage::show();
  //   return;
  // }
}

void EntitiesPage::unshow() {
  // Event handlers are removed by InterfaceManager (PageHandlers::retire()).

  EntitiesPage::_current_entities_page.set(nullptr);
  EntitiesPage::_currently_showing_page_type = 0;       // Set to 0 to force a page navigation on Nextion display when showing EntitiesPage next time.
  EntitiesPage::_currently_showing_header_text.set(""); // Clear stored string so that header text is always updated when showing EntitiesPage.
}

void EntitiesPage::_update_display(const std::shared_ptr<NSPanelRoomEntitiesPage> &page) {
  if (page == nullptr) {
    ESP_LOGE("EntitiesPage", "Tried to show RoomEntitiesPage but no _current_room_status has been set. Returning to HomePage.");
    InterfaceManager::request_show_home_page(); // May run on the RoomManager loop; see InterfaceManager::_task_page_transitions.
    return;
  }

  // go_to_page resets every component, including the header label, to its designed default, so any time we
  // navigate the cached header text no longer reflects what is on screen.
  switch (page->page_type) {
  case 4: {
    if (EntitiesPage::_currently_showing_page_type != 4) {
      Nextion::go_to_page(GUI_ITEMS4_PAGE::page_name, 1000);
      EntitiesPage::_currently_showing_page_type = 4;
      EntitiesPage::_currently_showing_header_text.set("");
    }
    break;
  }

  case 8: {
    if (EntitiesPage::_currently_showing_page_type != 8) {
      Nextion::go_to_page(GUI_ITEMS8_PAGE::page_name, 1000);
      EntitiesPage::_currently_showing_page_type = 8;
      EntitiesPage::_currently_showing_header_text.set("");
    }
    break;
  }

  case 12: {
    if (EntitiesPage::_currently_showing_page_type != 12) {
      Nextion::go_to_page(GUI_ITEMS12_PAGE::page_name, 1000);
      EntitiesPage::_currently_showing_page_type = 12;
      EntitiesPage::_currently_showing_header_text.set("");
    }
    break;
  }

  default:
    ESP_LOGE("EntitiesPage", "Unknown entities page type! Type: %ld. Will navigate to home page.", page->page_type);
    InterfaceManager::request_show_home_page(); // May run on the RoomManager loop; see InterfaceManager::_task_page_transitions.
    return;
  }

  // Header first: it is a single command with no delay, whereas the slots below are paced at 50 ms each.
  if (std::string(page->header_text).compare(EntitiesPage::_currently_showing_header_text.get()) != 0) {
    Nextion::set_component_text(GUI_ITEMS_PAGE_COMMON::page_header_label, page->header_text, 250);
    EntitiesPage::_currently_showing_header_text.set(page->header_text);
  }

  EntitiesPage::_update_displayed_items(page);
}

bool EntitiesPage::_render_is_stale(const std::shared_ptr<NSPanelRoomEntitiesPage> &page) {
  if (EntitiesPage::_currently_showing_page_type == 0) {
    return true; // unshow() ran, we have left the page.
  }

  std::shared_ptr<NSPanelRoomEntitiesPage> latest;
  if (RoomManager::get_current_room_entities_page_status(&latest) != ESP_OK) {
    return false; // Can't tell, so finish what we started.
  }
  // RoomManager swaps in a new object for every state publish and then posts ROOM_ENTITIES_PAGE_UPDATED, which is
  // queued behind this render on the same loop. A different object means that event will redraw the page anyway.
  return latest != page;
}

void EntitiesPage::_update_displayed_items(const std::shared_ptr<NSPanelRoomEntitiesPage> &page) {
  for (int i = 0; i < page->page_type; i++) {
    // Every render redraws every slot, so abandoning this one part-way leaves nothing behind once the next completes.
    if (EntitiesPage::_render_is_stale(page)) {
      ESP_LOGD("EntitiesPage", "Newer entities page data available, abandoning render at slot %d.", i);
      return;
    }

    NSPanelRoomEntitiesPage__EntitySlot *slot = nullptr;
    for (int j = 0; j < page->n_entities; j++) {
      if (page->entities[j]->room_view_position == i) {
        slot = page->entities[j];
        break;
      }
    }

    if (page->page_type == 12) {
      EntitiesPage::_update_displayed_item_in_slot(slot, GUI_ITEMS12_PAGE::item_slots[i]);
    } else if (page->page_type == 8) {
      EntitiesPage::_update_displayed_item_in_slot(slot, GUI_ITEMS8_PAGE::item_slots[i]);
    } else if (page->page_type == 4) {
      EntitiesPage::_update_displayed_item_in_slot(slot, GUI_ITEMS4_PAGE::item_slots[i]);
    } else {
      ESP_LOGE("EntitiesPage", "Unknown entities page type: %ld. Will not update displayed entity slot.", page->page_type);
    }
  }
}

void EntitiesPage::_update_displayed_item_in_slot(NSPanelRoomEntitiesPage__EntitySlot *slot_data, const GUI_ITEMS_PAGE_ITEM_DATA page_slot) {
  if (slot_data != nullptr) {
    Nextion::set_component_text(page_slot.label_name, (std::string("   ") + slot_data->name).c_str(), 250);
    Nextion::set_component_text(page_slot.button_name, slot_data->icon, 250);
    Nextion::set_component_foreground(page_slot.button_name, slot_data->pco, 250);
    Nextion::set_component_pco2(page_slot.button_name, slot_data->pco2, 250);

    Nextion::set_component_visibility(page_slot.button_name, true, 250);
    Nextion::set_component_visibility(page_slot.label_name, true, 250);
    vTaskDelay(pdMS_TO_TICKS(50));
  } else {
    // No entity was found for page slot. Blank page slot.
    Nextion::set_component_visibility(page_slot.button_name, false, 250);
    Nextion::set_component_visibility(page_slot.label_name, false, 250);
  }
}

void EntitiesPage::_handle_roommanager_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == roommanager_event_t::ROOM_ENTITIES_PAGE_UPDATED) {
    ESP_LOGD("EntitiesPage", "Received new data. Will update display.");
    std::shared_ptr<NSPanelRoomEntitiesPage> page;
    if (RoomManager::get_current_room_entities_page_status(&page) == ESP_OK) [[likely]] {
      EntitiesPage::_current_entities_page.set(page);
      EntitiesPage::_update_display(page);
    } else {
      ESP_LOGE("EntitiesPage", "Got new entity page data event but couldn't get new entity page data!");
      if (EntitiesPage::_current_entities_page.get() == nullptr) {
        ESP_LOGE("EntitiesPage", "Currently not showing entities page and we failed to get data. Will go back to HomePage.");
        // Not HomePage::show(): this runs on the RoomManager loop, see InterfaceManager::_task_page_transitions.
        InterfaceManager::request_show_home_page();
      }
    }
  }
}

void EntitiesPage::_handle_nextion_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // Take our own reference for the whole event. If we went through _current_entities_page directly,
  // nothing would keep the page alive when unshow() or a page update on another task replaces it mid-handler.
  std::shared_ptr<NSPanelRoomEntitiesPage> page = EntitiesPage::_current_entities_page.get();
  if (page == nullptr || EntitiesPage::_currently_showing_page_type == 0) { // We are currently not displaying. Ignore Nextion event.
    return;
  }

  switch (event_id) {
  case nextion_event_t::TOUCH_EVENT: {
    nextion_event_touch_t *data = (nextion_event_touch_t *)event_data;
    switch (page->page_type) {
    case 4:
      EntitiesPage::_handle_items4_touch_event(page, data);
      break;
    case 8:
      EntitiesPage::_handle_items8_touch_event(page, data);
      break;
    case 12:
      EntitiesPage::_handle_items12_touch_event(page, data);
      break;

    default:
      ESP_LOGE("EntitiesPage", "Unknown entities page type! Type: %ld while handling page touch event!", page->page_type);
      HomePage::show();
      return;
    }

    break;
  }

  default:
    break;
  }
}

void EntitiesPage::_handle_items4_touch_event(const std::shared_ptr<NSPanelRoomEntitiesPage> &page, nextion_event_touch_t *touch_data) {
  // First check if we actually pressed an entity toggle button or an entity name
  if (EntitiesPage::_display_type == display_type_t::SCENES || EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
    // When displaying scenes we don't want to send the toggle command when pressing the icon above/beside the button.
    // We want to send the toggle command when pressing the text itself as the icon is for saving applicable scenes.
    for (int i = 0; i < 4; i++) {
      if (touch_data->component_id == GUI_ITEMS4_PAGE::item_slots[i].label_id) {
        if (touch_data->pressed) {
          BuzzerManager::raise_event_for_touch(buzzer_event_t::SCENE_ACTIVATED);
          EntitiesPage::_send_entity_toggle_command_to_manager(page->id, i);
        }
        return; // Found match, no need to continue.
      }
    }

    // Handle scene save buttons
    for (int i = 0; i < 4; i++) {
      if (touch_data->component_id == GUI_ITEMS4_PAGE::item_slots[i].button_id) {
        if (touch_data->pressed) {
          for (int j = 0; j < page->n_entities; j++) {
            if (page->entities[j] != nullptr) {
              if (page->entities[j]->room_view_position == i && page->entities[j]->can_save_scene && EntitiesPage::_save_scene_task_handle == NULL) {
                // This is a scene we can actually save. Start save scene process.
                ESP_LOGD("EntitiesPage", "Entity in slot %ld can save scene.", page->entities[j]->room_view_position);
                EntitiesPage::_save_scene = true;
                int32_t *scene_slot = new int32_t(page->entities[j]->room_view_position);
                xTaskCreatePinnedToCore(EntitiesPage::_task_save_scene_progress, "save_scene", 4096, (void *)scene_slot, 1, &EntitiesPage::_save_scene_task_handle, 1);
              }
            }
          }
        } else {
          // User released the button
          EntitiesPage::_save_scene = false;
        }
        return; // Found match, no need to continue.
      }
    }
  } else {
    // Handle entities and not scenes
    for (int i = 0; i < 4; i++) {
      if (touch_data->component_id == GUI_ITEMS4_PAGE::item_slots[i].button_id) {
        if (touch_data->pressed) {
          EntitiesPage::_send_entity_toggle_command_to_manager(page->id, i);
        }
        return; // Found match, no need to continue.
      }
    }

    for (int i = 0; i < 4; i++) {
      if (touch_data->component_id == GUI_ITEMS4_PAGE::item_slots[i].label_id) {
        if (touch_data->pressed) {
          for (int j = 0; j < page->n_entities; j++) {
            if (page->entities[j] != nullptr) [[likely]] {
              if (page->entities[j]->room_view_position == i) {
                std::string topic_string = page->entities[j]->mqtt_state_topic;
                if (!topic_string.empty()) [[likely]] {
                  EntityPage::show(topic_string);
                }
              }
            }
          }
        }
        return; // Found match, no need to continue.
      }
    }
  }

  // We did not press an entity, check if we pressed any other button
  if (touch_data->pressed) {
    switch (touch_data->component_id) {
    case GUI_ITEMS4_PAGE::button_back_id:
      HomePage::show();
      break;

    case GUI_ITEMS4_PAGE::button_previous_page_id: {
      if (EntitiesPage::_display_type == display_type_t::SCENES) {
        RoomManager::go_to_previous_scenes_page();
      } else if (EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
        RoomManager::go_to_previous_global_scenes_page();
      } else {
        RoomManager::go_to_previous_entities_page();
      }
      break;
    }

    case GUI_ITEMS4_PAGE::button_next_page_id: {
      if (EntitiesPage::_display_type == display_type_t::SCENES) {
        RoomManager::go_to_next_scenes_page();
      } else if (EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
        RoomManager::go_to_next_global_scenes_page();
      } else {
        RoomManager::go_to_next_entities_page();
      }
      break;
    }

    default:
      ESP_LOGD("EntitiesPage", "Unknown component ID of touch event in items4. ID: %u", touch_data->component_id);
      break;
    }
  }
}

void EntitiesPage::_handle_items8_touch_event(const std::shared_ptr<NSPanelRoomEntitiesPage> &page, nextion_event_touch_t *touch_data) {
  // First check if we actually pressed an entity toggle button or an entity name
  if (EntitiesPage::_display_type == display_type_t::SCENES || EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
    // When displaying scenes we don't want to send the toggle command when pressing the icon above/beside the button.
    // We want to send the toggle command when pressing the text itself as the icon is for saving applicable scenes.
    for (int i = 0; i < 8; i++) {
      if (touch_data->component_id == GUI_ITEMS8_PAGE::item_slots[i].label_id) {
        if (touch_data->pressed) {
          BuzzerManager::raise_event_for_touch(buzzer_event_t::SCENE_ACTIVATED);
          EntitiesPage::_send_entity_toggle_command_to_manager(page->id, i);
        }
        return; // Found match, no need to continue.
      }
    }

    // Handle scene save buttons
    for (int i = 0; i < 8; i++) {
      if (touch_data->component_id == GUI_ITEMS8_PAGE::item_slots[i].button_id) {
        if (touch_data->pressed) {
          for (int j = 0; j < page->n_entities; j++) {
            if (page->entities[j] != nullptr) {
              if (page->entities[j]->room_view_position == i && page->entities[j]->can_save_scene && EntitiesPage::_save_scene_task_handle == NULL) {
                // This is a scene we can actually save. Start save scene process.
                ESP_LOGD("EntitiesPage", "Entity in slot %ld can save scene.", page->entities[j]->room_view_position);
                EntitiesPage::_save_scene = true;
                int32_t *scene_slot = new int32_t(page->entities[j]->room_view_position);
                xTaskCreatePinnedToCore(EntitiesPage::_task_save_scene_progress, "save_scene", 4096, (void *)scene_slot, 1, &EntitiesPage::_save_scene_task_handle, 1);
              }
            }
          }
        } else {
          // User released the button
          EntitiesPage::_save_scene = false;
        }
        return; // Found match, no need to continue.
      }
    }
  } else {
    // Handle entities and not scenes
    for (int i = 0; i < 8; i++) {
      if (touch_data->component_id == GUI_ITEMS8_PAGE::item_slots[i].button_id) {
        if (touch_data->pressed) {
          EntitiesPage::_send_entity_toggle_command_to_manager(page->id, i);
        }
        return; // Found match, no need to continue.
      }
    }

    for (int i = 0; i < 8; i++) {
      if (touch_data->component_id == GUI_ITEMS8_PAGE::item_slots[i].label_id) {
        if (touch_data->pressed) {
          for (int j = 0; j < page->n_entities; j++) {
            if (page->entities[j] != nullptr) [[likely]] {
              if (page->entities[j]->room_view_position == i) {
                std::string topic_string = page->entities[j]->mqtt_state_topic;

                if (!topic_string.empty()) [[likely]] {
                  EntityPage::show(topic_string);
                }
                return; // Found match, no need to continue.
              }
            }
          }
        }
        return; // Found match, no need to continue.
      }
    }
  }

  // We did not press an entity, check if we pressed any other button
  if (touch_data->pressed) {
    switch (touch_data->component_id) {
    case GUI_ITEMS8_PAGE::button_back_id:
      HomePage::show();
      break;

    case GUI_ITEMS8_PAGE::button_previous_page_id: {
      if (EntitiesPage::_display_type == display_type_t::SCENES) {
        RoomManager::go_to_previous_scenes_page();
      } else if (EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
        RoomManager::go_to_previous_global_scenes_page();
      } else {
        RoomManager::go_to_previous_entities_page();
      }
      break;
    }

    case GUI_ITEMS8_PAGE::button_next_page_id: {
      if (EntitiesPage::_display_type == display_type_t::SCENES) {
        RoomManager::go_to_next_scenes_page();
      } else if (EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
        RoomManager::go_to_next_global_scenes_page();
      } else {
        RoomManager::go_to_next_entities_page();
      }
      break;
    }

    default:
      ESP_LOGD("EntitiesPage", "Unknown component ID of touch event in items8. ID: %u", touch_data->component_id);
      break;
    }
  }
}

void EntitiesPage::_handle_items12_touch_event(const std::shared_ptr<NSPanelRoomEntitiesPage> &page, nextion_event_touch_t *touch_data) {
  // First check if we actually pressed an entity toggle button or an entity name
  if (EntitiesPage::_display_type == display_type_t::SCENES || EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
    // When displaying scenes we don't want to send the toggle command when pressing the icon above/beside the button.
    // We want to send the toggle command when pressing the text itself as the icon is for saving applicable scenes.
    for (int i = 0; i < 12; i++) {
      if (touch_data->component_id == GUI_ITEMS12_PAGE::item_slots[i].label_id) {
        if (touch_data->pressed) {
          BuzzerManager::raise_event_for_touch(buzzer_event_t::SCENE_ACTIVATED);
          EntitiesPage::_send_entity_toggle_command_to_manager(page->id, i);
        }
        return; // Found match, no need to continue.
      }
    }

    // Handle scene save buttons
    for (int i = 0; i < 12; i++) {
      if (touch_data->component_id == GUI_ITEMS12_PAGE::item_slots[i].button_id) {
        if (touch_data->pressed) {
          for (int j = 0; j < page->n_entities; j++) {
            if (page->entities[j] != nullptr) {
              if (page->entities[j]->room_view_position == i && page->entities[j]->can_save_scene && EntitiesPage::_save_scene_task_handle == NULL) {
                // This is a scene we can actually save. Start save scene process.
                ESP_LOGD("EntitiesPage", "Entity in slot %ld can save scene.", page->entities[j]->room_view_position);
                EntitiesPage::_save_scene = true;
                int32_t *scene_slot = new int32_t(page->entities[j]->room_view_position);
                xTaskCreatePinnedToCore(EntitiesPage::_task_save_scene_progress, "save_scene", 4096, (void *)scene_slot, 1, &EntitiesPage::_save_scene_task_handle, 1);
              }
            }
          }
        } else {
          // User released the button
          EntitiesPage::_save_scene = false;
        }
        return; // Found match, no need to continue.
      }
    }
  } else {
    // Handle entities and not scenes
    for (int i = 0; i < 12; i++) {
      if (touch_data->component_id == GUI_ITEMS12_PAGE::item_slots[i].button_id) {
        if (touch_data->pressed) {
          EntitiesPage::_send_entity_toggle_command_to_manager(page->id, i);
        }
        return; // Found match, no need to continue.
      }
    }

    for (int i = 0; i < 12; i++) {
      if (touch_data->component_id == GUI_ITEMS12_PAGE::item_slots[i].label_id) {
        if (touch_data->pressed) {
          for (int j = 0; j < page->n_entities; j++) {
            if (page->entities[j] != nullptr) [[likely]] {
              if (page->entities[j]->room_view_position == i) {
                std::string topic_string = page->entities[j]->mqtt_state_topic;
                if (!topic_string.empty()) [[likely]] {
                  EntityPage::show(topic_string);
                }
              }
            }
          }
        }
        return; // Found match, no need to continue.
      }
    }
  }

  // We did not press an entity, check if we pressed any other button
  if (touch_data->pressed) {
    switch (touch_data->component_id) {
    case GUI_ITEMS12_PAGE::button_back_id:
      HomePage::show();
      break;

    case GUI_ITEMS12_PAGE::button_previous_page_id: {
      if (EntitiesPage::_display_type == display_type_t::SCENES) {
        RoomManager::go_to_previous_scenes_page();
      } else if (EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
        RoomManager::go_to_previous_global_scenes_page();
      } else {
        RoomManager::go_to_previous_entities_page();
      }
      break;
    }

    case GUI_ITEMS12_PAGE::button_next_page_id: {
      if (EntitiesPage::_display_type == display_type_t::SCENES) {
        RoomManager::go_to_next_scenes_page();
      } else if (EntitiesPage::_display_type == display_type_t::GLOBAL_SCENES) {
        RoomManager::go_to_next_global_scenes_page();
      } else {
        RoomManager::go_to_next_entities_page();
      }
      break;
    }

    default:
      ESP_LOGD("EntitiesPage", "Unknown component ID of touch event in items12. ID: %u", touch_data->component_id);
      break;
    }
  }
}

void EntitiesPage::_send_entity_toggle_command_to_manager(uint32_t entity_page_id, uint32_t entity_slot) {
  NSPanelMQTTManagerCommand__ToggleEntityFromEntitiesPage toggle_cmd = NSPANEL_MQTTMANAGER_COMMAND__TOGGLE_ENTITY_FROM_ENTITIES_PAGE__INIT;
  toggle_cmd.entity_page_id = entity_page_id;
  toggle_cmd.entity_slot = entity_slot;

  ESP_LOGD("EntitiesPage", "Sending command to toggle entity in slot %ld from entity page with ID %ld.", entity_slot, entity_page_id);

  NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
  cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_TOGGLE_ENTITY_FROM_ENTITIES_PAGE;
  cmd.toggle_entity_from_entities_page = &toggle_cmd;
  cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

  uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
  std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
  size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
  if (packed_data_size == packed_length) {
    if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) {
      ESP_LOGE("EntitiesPage", "Failed to send toggle command!");
    }
  } else {
    ESP_LOGE("EntitiesPage", "Failed to serialize toggle command!");
  }
}

void EntitiesPage::_task_save_scene_progress(void *scene_slot) {
  int32_t scene_slot_int = *((int32_t *)scene_slot);
  delete (int32_t *)scene_slot;

  ESP_LOGD("EntitiesPage", "Starting save of scene in slot %ld", scene_slot_int);

  Nextion::set_component_visibility(GUI_ITEMS_PAGE_COMMON::slider_save_name, true, 250);
  for (int i = 0; i < 100 && EntitiesPage::_save_scene; i += 2) {
    Nextion::set_component_value(GUI_ITEMS_PAGE_COMMON::slider_save_name, i, 25);
    vTaskDelay(pdMS_TO_TICKS(3000 / (100 / 2))); // Update in steps of 2 and take 3 seconds (3000ms) to save a scene
  }
  Nextion::set_component_visibility(GUI_ITEMS_PAGE_COMMON::slider_save_name, false, 250);

  std::shared_ptr<NSPanelRoomEntitiesPage> page = EntitiesPage::_current_entities_page.get();
  if (EntitiesPage::_save_scene && page == nullptr) {
    ESP_LOGW("EntitiesPage", "Left the entities page while saving scene in slot %ld. Will not save it.", scene_slot_int);
  } else if (EntitiesPage::_save_scene) {
    // Send command to save scene.
    NSPanelMQTTManagerCommand__SaveSceneCommand save_command = NSPANEL_MQTTMANAGER_COMMAND__SAVE_SCENE_COMMAND__INIT;
    save_command.entity_page_id = page->id;
    save_command.entity_slot = scene_slot_int;

    ESP_LOGD("EntitiesPage", "Sending command to save scene in slot %ld from entity page with ID %ld.", scene_slot_int, page->id);

    NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
    cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_SAVE_SCENE_COMMAND;
    cmd.save_scene_command = &save_command;
    cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

    uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
    std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
    size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
    if (packed_data_size == packed_length) {
      if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) == ESP_OK) {
        BuzzerManager::raise_event(buzzer_event_t::SCENE_SAVED);

        // Display text to user to say scene was saved.
        Nextion::set_component_text(GUI_ITEMS_PAGE_COMMON::page_header_label, "Scene saved", 250);
        EntitiesPage::_currently_showing_header_text.set("Scene saved");
        vTaskDelay(pdMS_TO_TICKS(1000));
        Nextion::set_component_text(GUI_ITEMS_PAGE_COMMON::page_header_label, page->header_text, 250);
        EntitiesPage::_currently_showing_header_text.set(page->header_text);
      } else {
        ESP_LOGE("EntitiesPage", "Failed to send save command!");
        Nextion::set_component_text(GUI_ITEMS_PAGE_COMMON::page_header_label, "Save failed", 250);
        EntitiesPage::_currently_showing_header_text.set("Save failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        Nextion::set_component_text(GUI_ITEMS_PAGE_COMMON::page_header_label, page->header_text, 250);
        EntitiesPage::_currently_showing_header_text.set(page->header_text);
      }
    } else {
      ESP_LOGE("EntitiesPage", "Failed to serialize save command!");
      ESP_LOGE("EntitiesPage", "Failed to send save command!");
      Nextion::set_component_text(GUI_ITEMS_PAGE_COMMON::page_header_label, "Save failed", 250);
      EntitiesPage::_currently_showing_header_text.set("Save failed");
      vTaskDelay(pdMS_TO_TICKS(1000));
      Nextion::set_component_text(GUI_ITEMS_PAGE_COMMON::page_header_label, page->header_text, 250);
      EntitiesPage::_currently_showing_header_text.set(page->header_text);
    }
  }

  // Task finished. Reset ptr and exit task.
  EntitiesPage::_save_scene_task_handle = NULL;
  vTaskDelete(NULL);
}