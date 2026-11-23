#include <BuzzerManager.hpp>
#include <ConfigManager.hpp>
#include <EntitiesPage.hpp>
#include <EntityPage.hpp>
#include <HomePage.hpp>
#include <EntitiesView.hpp>
#include <InterfaceManager.hpp>
#include <LvglText.hpp>
#include <LvglUi.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <RoomManager.hpp>
#include <RoomManager_event.hpp>
#include <esp_log.h>
#include <string>

void EntitiesPage::show(display_type_t display_type) {
  esp_log_level_set("EntitiesPage", ConfigManager::log_level);
  EntitiesPage::_display_type = display_type;

  std::shared_ptr<PageHandlers> handlers = InterfaceManager::begin_page(EntitiesPage::unshow);
  if (LvglUi::active()) {
    // Touches arrive as the view's actions, on the GUI task: hand them to LvglUi's task.
    EntitiesView::on_action = [](EntitiesView::Action action, int32_t slot) {
      LvglUi::defer([action, slot]() { EntitiesPage::_lvgl_action((int)action, slot); });
    };
  }

  // EntitiesPage::_update_display();
  if (handlers->add_to(RoomManager::event_loop(), ROOMMANAGER_EVENT, ESP_EVENT_ANY_ID, EntitiesPage::_handle_roommanager_event) != ESP_OK) [[unlikely]] {
    ESP_LOGE("EntitiesPage", "Failed to register event handler for EntitiesPage to RoomManager. Will return on HomePage.");
    HomePage::show();
  }

  // If current room has an entities page, go to it and if not, show the next available entities page.
  esp_err_t found = ESP_FAIL;
  switch (display_type) {
  case display_type_t::ENTITIES:
    found = RoomManager::go_to_first_entities_page();
    break;

  case display_type_t::SCENES:
    found = RoomManager::go_to_first_scenes_page();
    break;

  case display_type_t::GLOBAL_SCENES:
    found = RoomManager::go_to_first_global_scenes_page();
    break;

  default:
    ESP_LOGE("EntitiesPage", "Unknown display type! Will return to HomePage.");
    HomePage::show();
    return;
  }

  if (found != ESP_OK) {
    // Nothing to show (a room without scenes, say): no page data will arrive. Say so, rather than leave
    // the last page on screen with no page behind it to take its touches.
    const char *what = display_type == display_type_t::ENTITIES ? "No entities" : "No scenes";
    ESP_LOGI("EntitiesPage", "%s to show.", what);
    if (LvglUi::active()) {
      EntitiesState state;
      state.header = what;
      state.can_prev = false;
      state.can_next = false;
      LvglUi::show(LvglUi::View::ENTITIES, EntitiesView::build, [state]() { EntitiesView::show(state); });
    }
  }

  // if (RoomManager::get_current_room_entities_page_status(&EntitiesPage::_current_entities_page) != ESP_OK) [[unlikely]] {
  //   ESP_LOGE("EntitiesPage", "Failed to get current entities page. Will return to HomePage.");
  //   HomePage::show();
  //   return;
  // }
}

void EntitiesPage::unshow() {
  // Event handlers stop with the page: InterfaceManager only dispatches to the current page.

  EntitiesPage::_current_entities_page.set(nullptr);
  EntitiesPage::_currently_showing_page_type = 0;
  EntitiesPage::_lvgl_save_progress = -1;
  EntitiesPage::_lvgl_header_override.set("");
}

void EntitiesPage::_update_display(const std::shared_ptr<NSPanelRoomEntitiesPage> &page) {
  if (page == nullptr) {
    ESP_LOGE("EntitiesPage", "Tried to show RoomEntitiesPage but no _current_room_status has been set. Returning to HomePage.");
    InterfaceManager::request_show_home_page(); // May run on the RoomManager loop; see InterfaceManager::_task_page_transitions.
    return;
  }

  if (!LvglUi::active()) {
    return;
  }
  if (page->page_type != 4 && page->page_type != 8 && page->page_type != 12) {
    ESP_LOGE("EntitiesPage", "Unknown entities page type! Type: %ld. Will navigate to home page.", page->page_type);
    InterfaceManager::request_show_home_page();
    return;
  }
  EntitiesPage::_currently_showing_page_type = page->page_type;
  EntitiesPage::_lvgl_render(page);
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

NSPanelRoomEntitiesPage__EntitySlot *EntitiesPage::_get_entity_in_slot(const std::shared_ptr<NSPanelRoomEntitiesPage> &page, int32_t slot) {
  for (int i = 0; i < page->n_entities; i++) {
    if (page->entities[i] != nullptr && page->entities[i]->room_view_position == slot) {
      return page->entities[i];
    }
  }
  return nullptr;
}

void EntitiesPage::_handle_entity_button_pressed(const std::shared_ptr<NSPanelRoomEntitiesPage> &page, int32_t slot) {
  NSPanelRoomEntitiesPage__EntitySlot *entity = EntitiesPage::_get_entity_in_slot(page, slot);
  // Media players can't be toggled, open their control page instead. With LVGL, thermostats and covers
  // too: the manager's thermostat toggle does nothing, and LVGL's tiles have no separate name to tap.
  const bool opens_page = entity != nullptr && (entity->type == NSPANEL_ROOM_ENTITIES_PAGE__ENTITY_SLOT__ENTITY_TYPE__ENTITY_TYPE_MEDIA_PLAYER ||
                                                (LvglUi::active() && (entity->type == NSPANEL_ROOM_ENTITIES_PAGE__ENTITY_SLOT__ENTITY_TYPE__ENTITY_TYPE_THERMOSTAT ||
                                                                      entity->type == NSPANEL_ROOM_ENTITIES_PAGE__ENTITY_SLOT__ENTITY_TYPE__ENTITY_TYPE_COVER)));
  if (opens_page && entity->mqtt_state_topic[0] != '\0') {
    EntityPage::show(entity->mqtt_state_topic);
    return;
  }
  EntitiesPage::_send_entity_toggle_command_to_manager(page->id, slot);
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

  for (int i = 0; i < 100 && EntitiesPage::_save_scene; i += 2) {
    if (LvglUi::active()) {
      EntitiesPage::_lvgl_save_progress = i;
      EntitiesPage::_lvgl_refresh();
    }
    vTaskDelay(pdMS_TO_TICKS(3000 / (100 / 2))); // Update in steps of 2 and take 3 seconds (3000ms) to save a scene
  }
  if (LvglUi::active()) {
    EntitiesPage::_lvgl_save_progress = -1;
    EntitiesPage::_lvgl_refresh();
  }

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
        EntitiesPage::_lvgl_header_override.set("Scene saved");
        EntitiesPage::_lvgl_refresh();
        vTaskDelay(pdMS_TO_TICKS(1000));
        EntitiesPage::_lvgl_header_override.set("");
        EntitiesPage::_lvgl_refresh();
      } else {
        ESP_LOGE("EntitiesPage", "Failed to send save command!");
        EntitiesPage::_lvgl_header_override.set("Save failed");
        EntitiesPage::_lvgl_refresh();
        vTaskDelay(pdMS_TO_TICKS(1000));
        EntitiesPage::_lvgl_header_override.set("");
        EntitiesPage::_lvgl_refresh();
      }
    } else {
      ESP_LOGE("EntitiesPage", "Failed to serialize save command!");
      ESP_LOGE("EntitiesPage", "Failed to send save command!");
      EntitiesPage::_lvgl_header_override.set("Save failed");
      EntitiesPage::_lvgl_refresh();
      vTaskDelay(pdMS_TO_TICKS(1000));
      EntitiesPage::_lvgl_header_override.set("");
      EntitiesPage::_lvgl_refresh();
    }
  }

  // Task finished. Reset ptr and exit task.
  EntitiesPage::_save_scene_task_handle = NULL;
  vTaskDelete(NULL);
}
void EntitiesPage::_lvgl_render(const std::shared_ptr<NSPanelRoomEntitiesPage> &page) {
  if (!LvglUi::active() || page == nullptr) {
    return;
  }
  EntitiesState state;
  std::string header = EntitiesPage::_lvgl_header_override.get();
  state.header = LvglText::utf8(header.empty() ? page->header_text : header.c_str());
  state.size = page->page_type;
  state.own_sound = EntitiesPage::_display_type != display_type_t::ENTITIES;
  state.save_progress = EntitiesPage::_lvgl_save_progress;
  for (size_t i = 0; i < page->n_entities; i++) {
    const NSPanelRoomEntitiesPage__EntitySlot *entity = page->entities[i];
    if (entity == nullptr || entity->room_view_position < 0 || entity->room_view_position >= EntitiesState::MAX_SLOTS) {
      continue;
    }
    EntitySlot &slot = state.slots[entity->room_view_position];
    slot.empty = false;
    slot.name = LvglText::utf8(entity->name);
    slot.on = entity->pco == LvglText::ICON_COLOR_ON;
    slot.icon = LvglText::slot_icon((int)entity->type, slot.on);
  }
  LvglUi::show(LvglUi::View::ENTITIES, EntitiesView::build, [state]() { EntitiesView::show(state); });
}

void EntitiesPage::_lvgl_refresh() {
  if (LvglUi::active() && EntitiesPage::_currently_showing_page_type != 0) {
    EntitiesPage::_lvgl_render(EntitiesPage::_current_entities_page.get());
  }
}

void EntitiesPage::_step_page(bool next) {
  switch (EntitiesPage::_display_type) {
  case display_type_t::SCENES:
    next ? RoomManager::go_to_next_scenes_page() : RoomManager::go_to_previous_scenes_page();
    break;
  case display_type_t::GLOBAL_SCENES:
    next ? RoomManager::go_to_next_global_scenes_page() : RoomManager::go_to_previous_global_scenes_page();
    break;
  default:
    next ? RoomManager::go_to_next_entities_page() : RoomManager::go_to_previous_entities_page();
    break;
  }
}

void EntitiesPage::_lvgl_action(int action, int32_t slot) {
  if (!InterfaceManager::is_current_page(EntitiesPage::unshow)) {
    return; // Left the page before the action got here.
  }
  if ((EntitiesView::Action)action == EntitiesView::Action::BACK) {
    HomePage::show(); // Also from the "No scenes" page, which has no page data.
    return;
  }
  std::shared_ptr<NSPanelRoomEntitiesPage> page = EntitiesPage::_current_entities_page.get();
  if (page == nullptr || EntitiesPage::_currently_showing_page_type == 0) {
    return;
  }
  const bool scenes = EntitiesPage::_display_type != display_type_t::ENTITIES;
  switch ((EntitiesView::Action)action) {
  case EntitiesView::Action::BACK:
    HomePage::show();
    break;
  case EntitiesView::Action::PREV:
    EntitiesPage::_step_page(false);
    break;
  case EntitiesView::Action::NEXT:
    EntitiesPage::_step_page(true);
    break;
  case EntitiesView::Action::SLOT:
    // A tap: activate a scene, or toggle an entity (media players open their page), .
    if (EntitiesPage::_get_entity_in_slot(page, slot) == nullptr) {
      break;
    }
    if (scenes) {
      // The scene tiles are marked own_sound, so LvglUi played no touch sound for this press.
      if (!BuzzerManager::raise_event(buzzer_event_t::SCENE_ACTIVATED) && LvglUi::touch_sound) {
        BuzzerManager::raise_event(buzzer_event_t::TOUCH); // No scene sound configured: the plain touch sound
      }
      EntitiesPage::_send_entity_toggle_command_to_manager(page->id, slot);
    } else {
      EntitiesPage::_handle_entity_button_pressed(page, slot);
    }
    break;
  case EntitiesView::Action::SLOT_LONG: {
    // A hold: save a scene (until let go, or 3 s), or open an entity's page, .
    NSPanelRoomEntitiesPage__EntitySlot *entity = EntitiesPage::_get_entity_in_slot(page, slot);
    if (entity == nullptr) {
      break;
    }
    if (scenes) {
      if (entity->can_save_scene && EntitiesPage::_save_scene_task_handle == NULL) {
        EntitiesPage::_save_scene = true;
        int32_t *scene_slot = new int32_t(entity->room_view_position);
        xTaskCreatePinnedToCore(EntitiesPage::_task_save_scene_progress, "save_scene", 4096, (void *)scene_slot, 1, &EntitiesPage::_save_scene_task_handle, 1);
      }
    } else if (entity->mqtt_state_topic[0] != '\0') {
      EntityPage::show(entity->mqtt_state_topic);
    }
    break;
  }
  case EntitiesView::Action::SLOT_RELEASED:
    EntitiesPage::_save_scene = false;
    break;
  }
}
