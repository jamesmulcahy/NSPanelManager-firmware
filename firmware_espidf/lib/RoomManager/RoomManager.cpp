#include "RoomManager.hpp"
#include <ConfigManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <RoomManager_event.hpp>
#include <UpdateManager_event.hpp>
#include <WiFiManager.hpp>
#include <esp_event.h>
#include <esp_log.h>
#include <vector>

ESP_EVENT_DEFINE_BASE(ROOMMANAGER_EVENT);

void RoomManager::init() {
  ESP_LOGI("RoomManager", "Initializing RoomManager.");
  esp_log_level_set("RoomManager", ConfigManager::log_level);
  RoomManager::_home_page_mutex = xSemaphoreCreateMutex();
  RoomManager::_entities_page_mutex = xSemaphoreCreateMutex();
  RoomManager::_load_all_rooms_task_handle = NULL;

  // Initialize local event loop
  RoomManager::_local_event_loop_args = {
      .queue_size = 128,
      .task_name = "roommanager_event_loop",
      .task_priority = 4,
      .task_stack_size = 8192,
      .task_core_id = 1};
  esp_event_loop_create(&RoomManager::_local_event_loop_args, &RoomManager::_local_event_loop);

  // Hook into MQTT events
  MqttManager::register_handler(MQTT_EVENT_ANY, RoomManager::_mqtt_event_handler, NULL);
  // MqttManager::register_handler(MQTT_EVENT_CONNECTED, RoomManager::_mqtt_event_handler_connected, NULL);
  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, &RoomManager::_event_handler, NULL);
}

esp_err_t RoomManager::get_home_page_status(std::shared_ptr<NSPanelRoomStatus> *status) {
  if (xSemaphoreTake(RoomManager::_home_page_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
    if (RoomManager::_home_page == nullptr) [[unlikely]] {
      xSemaphoreGive(RoomManager::_home_page_mutex);
      ESP_LOGE("RoomManager", "Failed to get home page status, is NULL.");
      return ESP_ERR_NOT_FINISHED;
    }

    *status = RoomManager::_home_page;
    xSemaphoreGive(RoomManager::_home_page_mutex);
    return ESP_OK;
  } else {
    ESP_LOGE("RoomManager", "Failed to get _home_page_mutex while trying to get NSPanelRoomStatus!");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::get_home_page_status_all_rooms(std::shared_ptr<NSPanelRoomStatus> *status) {
  if (xSemaphoreTake(RoomManager::_home_page_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
    if (RoomManager::_home_page_all_rooms == nullptr) [[unlikely]] {
      xSemaphoreGive(RoomManager::_home_page_mutex);
      ESP_LOGE("RoomManager", "Failed to get home page status (all rooms), is NULL.");
      return ESP_ERR_NOT_FINISHED;
    }

    *status = RoomManager::_home_page_all_rooms;
    xSemaphoreGive(RoomManager::_home_page_mutex);
    return ESP_OK;
  } else {
    ESP_LOGE("RoomManager", "Failed to get _home_page_mutex while trying to get NSPanelRoomStatus for all rooms!");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::get_home_page_status_mutable(std::shared_ptr<NSPanelRoomStatus> *status) {
  if (xSemaphoreTake(RoomManager::_home_page_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
    if (RoomManager::_home_page == nullptr) [[unlikely]] {
      xSemaphoreGive(RoomManager::_home_page_mutex);
      return ESP_ERR_NOT_FINISHED;
    }

    size_t status_pack_size = nspanel_room_status__get_packed_size(RoomManager::_home_page.get());
    std::vector<uint8_t> buffer(status_pack_size);
    nspanel_room_status__pack(RoomManager::_home_page.get(), buffer.data());
    NSPanelRoomStatus *temp_status;

    temp_status = nspanel_room_status__unpack(NULL, status_pack_size, buffer.data());
    if (temp_status != NULL) [[likely]] {
      xSemaphoreGive(RoomManager::_home_page_mutex);
      (*status) = std::shared_ptr<NSPanelRoomStatus>(temp_status, &RoomManager::_nspanel_room_status_shared_ptr_deleter);
      return ESP_OK;
    } else {
      xSemaphoreGive(RoomManager::_home_page_mutex);
      return ESP_ERR_NOT_FINISHED;
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get _home_page_mutex while trying to get NSPanelRoomStatus!");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::get_home_page_status_mutable_all_rooms(std::shared_ptr<NSPanelRoomStatus> *status) {
  if (xSemaphoreTake(RoomManager::_home_page_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
    if (RoomManager::_home_page_all_rooms == nullptr) [[unlikely]] {
      xSemaphoreGive(RoomManager::_home_page_mutex);
      return ESP_ERR_NOT_FINISHED;
    }

    size_t status_pack_size = nspanel_room_status__get_packed_size(RoomManager::_home_page_all_rooms.get());
    std::vector<uint8_t> buffer(status_pack_size);
    nspanel_room_status__pack(RoomManager::_home_page_all_rooms.get(), buffer.data());
    NSPanelRoomStatus *temp_status;

    temp_status = nspanel_room_status__unpack(NULL, status_pack_size, buffer.data());
    if (temp_status != NULL) [[likely]] {
      xSemaphoreGive(RoomManager::_home_page_mutex);
      (*status) = std::shared_ptr<NSPanelRoomStatus>(temp_status, &RoomManager::_nspanel_room_status_shared_ptr_deleter);
      return ESP_OK;
    } else {
      xSemaphoreGive(RoomManager::_home_page_mutex);
      return ESP_ERR_NOT_FINISHED;
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get _home_page_mutex while trying to get NSPanelRoomStatus for all rooms!");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_room_id(uint32_t room_id) {
  ESP_LOGD("RoomManager", "Request to navigate to room page ID %lu", room_id);

  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    if (config->n_room_infos <= 0) [[unlikely]] {
      ESP_LOGE("RoomManager", "Tried going to room %lu but no rooms exist in current config.", room_id);
      return ESP_ERR_NOT_FINISHED;
    }

    bool valid_room = false;
    for (int i = 0; i < config->n_room_infos; i++) {
      if (config->room_infos[i]->room_id == room_id) {
        valid_room = true;
        break;
      }
    }

    if (!valid_room) [[unlikely]] {
      ESP_LOGW("RoomManager", "Requested to go to room id %lu but not such ID was found in config. Will go to default room instead.", room_id);
      room_id = config->default_room;
    }

    // Unsubscribe from old status topic
    if (!RoomManager::_current_home_page_status_topic.get().empty()) [[likely]] {
      if (MqttManager::unsubscribe(RoomManager::_current_home_page_status_topic.get()) != ESP_OK) [[unlikely]] {
        ESP_LOGW("RoomManager", "Failed to unsubscribe from current room status topic.");
      }
    }

    std::string new_mqtt_room_topic = "nspanel/mqttmanager_";
    new_mqtt_room_topic.append(NSPM_ConfigManager::get_manager_address());
    new_mqtt_room_topic.append("/room/");
    new_mqtt_room_topic.append(std::to_string(room_id));
    new_mqtt_room_topic.append("/state");
    ESP_LOGD("RoomManager", "Subscribing to room topic %s.", new_mqtt_room_topic.c_str());
    if (MqttManager::subscribe(new_mqtt_room_topic) != ESP_OK) {
      ESP_LOGE("RoomManager", "Failed to subscribe to room state topic %s.", new_mqtt_room_topic.c_str());
    }

    RoomManager::_current_room_id = room_id;
    RoomManager::_current_home_page_status_topic.set(new_mqtt_room_topic);
    return ESP_OK;
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_previous_room() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    if (config->n_room_infos <= 0) [[unlikely]] {
      ESP_LOGE("RoomManager", "Tried going to another room but no rooms exist in current config.");
      return ESP_ERR_NOT_FINISHED;
    }

    int32_t current_room_index = -1;
    for (int i = 0; i < config->n_room_infos; i++) {
      if (config->room_infos[i]->room_id == RoomManager::_current_room_id) {
        current_room_index = i;
        break;
      }
    }

    if (current_room_index < 0) [[unlikely]] {
      ESP_LOGW("RoomManager", "Did not find currently selected room in room IDs. Will go to default room.");
      return RoomManager::go_to_room_id(config->default_room);
    } else [[likely]] {
      if (current_room_index == 0) {
        // We are at first room in list, wrap around to end of list.
        return RoomManager::go_to_room_id(config->room_infos[config->n_room_infos - 1]->room_id);
      } else [[likely]] {
        return RoomManager::go_to_room_id(config->room_infos[--current_room_index]->room_id);
      }
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to previous room.");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_next_room() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    if (config->n_room_infos <= 0) [[unlikely]] {
      ESP_LOGE("RoomManager", "Tried going to another room but no rooms exist in current config.");
      return ESP_ERR_NOT_FINISHED;
    }

    int32_t current_room_index = -1;
    for (int i = 0; i < config->n_room_infos; i++) {
      if (config->room_infos[i]->room_id == RoomManager::_current_room_id) {
        current_room_index = i;
        break;
      }
    }

    if (current_room_index < 0) [[unlikely]] {
      ESP_LOGW("RoomManager", "Did not find currently selected room in room IDs. Will go to default room.");
      return RoomManager::go_to_room_id(config->default_room);
    } else [[likely]] {
      if (current_room_index != config->n_room_infos - 1) [[likely]] {
        return RoomManager::go_to_room_id(config->room_infos[++current_room_index]->room_id);
      } else {
        // We have reached end of list of rooms, wrap around to first room.
        return RoomManager::go_to_room_id(config->room_infos[0]->room_id);
      }
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to next room.");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_default_room() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    return RoomManager::go_to_room_id(config->default_room);
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to next room.");
  }
  return ESP_ERR_NOT_FINISHED;
}

uint32_t RoomManager::get_current_room_id() {
  return RoomManager::_current_room_id;
}

esp_err_t RoomManager::replace_home_page_status(std::shared_ptr<NSPanelRoomStatus> status) {
  if (xSemaphoreTake(RoomManager::_home_page_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
    RoomManager::_home_page = status;
    xSemaphoreGive(RoomManager::_home_page_mutex);

    esp_event_post(ROOMMANAGER_EVENT, roommanager_event_t::HOME_PAGE_UPDATED, NULL, 0, pdMS_TO_TICKS(250));
    return ESP_OK;
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::replace_home_page_status_all_rooms(std::shared_ptr<NSPanelRoomStatus> status) {
  if (xSemaphoreTake(RoomManager::_home_page_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
    RoomManager::_home_page_all_rooms = status;
    xSemaphoreGive(RoomManager::_home_page_mutex);

    esp_event_post(ROOMMANAGER_EVENT, roommanager_event_t::HOME_PAGE_UPDATED, NULL, 0, pdMS_TO_TICKS(250));
    return ESP_OK;
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::get_current_room_entities_page_status(std::shared_ptr<NSPanelRoomEntitiesPage> *status) {
  if (xSemaphoreTake(RoomManager::_entities_page_mutex, pdMS_TO_TICKS(500)) == pdPASS) {
    if (RoomManager::_entities_page == nullptr) [[unlikely]] {
      xSemaphoreGive(RoomManager::_entities_page_mutex);
      ESP_LOGE("RoomManager", "Failed to get current room entities page. Current page is NULL.");
      return ESP_ERR_NOT_FINISHED;
    }

    *status = RoomManager::_entities_page;
    xSemaphoreGive(RoomManager::_entities_page_mutex);
    return ESP_OK;
  }
  ESP_LOGE("RoomManager", "Failed to get current room entities page mutex.");

  TaskHandle_t holder_task = xSemaphoreGetMutexHolder(RoomManager::_entities_page_mutex);
  if (holder_task != NULL) {
    ESP_LOGE("RoomManager", "Holding task Name: %s", pcTaskGetName(holder_task));
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_entities_page_id(uint32_t page_id) {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    bool valid_page_id = false;
    for (int i = 0; i < config->n_room_infos && !valid_page_id; i++) {
      for (int j = 0; j < config->room_infos[i]->n_entity_page_ids && !valid_page_id; j++) {
        if (config->room_infos[i]->entity_page_ids[j] == page_id) {
          valid_page_id = true;
        }
      }
      for (int j = 0; j < config->room_infos[i]->n_scene_page_ids && !valid_page_id; j++) {
        if (config->room_infos[i]->scene_page_ids[j] == page_id) {
          valid_page_id = true;
        }
      }
    }
    for (int i = 0; i < config->n_global_scene_entity_page_ids && !valid_page_id; i++) {
      if (config->global_scene_entity_page_ids[i] == page_id) {
        valid_page_id = true;
      }
    }

    if (!valid_page_id) [[unlikely]] {
      ESP_LOGW("RoomManager", "Requested to go to entities page id %lu but not such ID was found in config. Will abort.", page_id);
      return ESP_ERR_NOT_FINISHED;
    }

    std::string new_mqtt_entities_page_status_topic = "nspanel/mqttmanager_";
    new_mqtt_entities_page_status_topic.append(NSPM_ConfigManager::get_manager_address());
    new_mqtt_entities_page_status_topic.append("/entity_pages/");
    new_mqtt_entities_page_status_topic.append(std::to_string(page_id));
    new_mqtt_entities_page_status_topic.append("/state");
    std::string current_topic = std::string(RoomManager::_current_entities_page_status_topic.get());

    if (xSemaphoreTake(RoomManager::_entities_page_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
      ESP_LOGD("RoomManager", "Request to navigate to entities page ID %lu", page_id);

      if (new_mqtt_entities_page_status_topic.compare(current_topic) == 0) {
        ESP_LOGD("RoomManager", "Already on correct topic, send event.");
        // We are navigating to the same already selected page, simply trigger the event.
        if (xSemaphoreGive(RoomManager::_entities_page_mutex) != pdTRUE) [[unlikely]] {
          ESP_LOGE("RoomManager", "Failed to give back _entities_page_mutex!");
        }

        vTaskDelay(pdMS_TO_TICKS(25)); // Wait 25ms for semaphore to actually give way and reset as this seems to be the cause of a race-condition otherwise.
        if (esp_event_post_to(RoomManager::_local_event_loop, ROOMMANAGER_EVENT, roommanager_event_t::ROOM_ENTITIES_PAGE_UPDATED, NULL, 0, pdMS_TO_TICKS(250)) != ESP_OK) [[unlikely]] {
          ESP_LOGE("RoomManager", "Failed to trigger event that entities page has changed (was already on correct entities page ID).");
        }
      } else {
        if (xSemaphoreGive(RoomManager::_entities_page_mutex) != pdTRUE) [[unlikely]] {
          ESP_LOGE("RoomManager", "Failed to give back _entities_page_mutex!");
        }

        ESP_LOGD("RoomManager", "Navigate to new topic.");
        if (!current_topic.empty()) [[likely]] {
          if (MqttManager::unsubscribe(current_topic) != ESP_OK) [[unlikely]] {
            ESP_LOGW("RoomManager", "Failed to unsubscribe from current room status topic.");
          }
        }

        RoomManager::_current_entities_page_status_topic.set(new_mqtt_entities_page_status_topic);
        if (MqttManager::subscribe(new_mqtt_entities_page_status_topic) != ESP_OK) {
          ESP_LOGE("RoomManager", "Failed to subscribe to new MQTT topic for entity page state updates.");
        }
      }
      RoomManager::_current_entities_page_id = page_id;
      return ESP_OK;
    }
    return ESP_ERR_NOT_FINISHED;
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_first_entities_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
    for (int i = 0; i < config->n_room_infos; i++) {
      if (config->room_infos[i]->room_id == RoomManager::_current_room_id) {
        if (config->room_infos[i]->n_entity_page_ids > 0) [[likely]] {
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->entity_page_ids[0]);
          break;
        } else {
          ESP_LOGE("RoomManager", "Requested to go to first entity page in room but room has not entity pages. Will abort.");
          return ESP_ERR_NOT_FINISHED;
        }
      }
    }
    ESP_LOGE("RoomManager", "Failed to find currently selected room while trying to go to first entities page.");
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to first entities page for room.");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_previous_entities_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    bool select_next_entity_page = false;
    for (size_t i = config->n_room_infos; i-- > 0;) {
      for (size_t j = config->room_infos[i]->n_entity_page_ids; j-- > 0;) {
        if (config->room_infos[i]->entity_page_ids[j] == RoomManager::_current_entities_page_id) {
          select_next_entity_page = true;
        } else if (select_next_entity_page) {
          // We need to go to another room for this page, switch.
          if (config->room_infos[i]->room_id != RoomManager::_current_room_id) {
            RoomManager::go_to_room_id(config->room_infos[i]->room_id); //
          }
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->entity_page_ids[j]);
        }
      }
    }

    if (select_next_entity_page) {
      // Did not find any entity page after currently selected, try from beginning ie. "wrap" around
      for (size_t i = config->n_room_infos; i-- > 0;) {
        for (size_t j = config->room_infos[i]->n_entity_page_ids; j-- > 0;) {
          // We need to go to another room for this page, switch.
          if (config->room_infos[i]->room_id != RoomManager::_current_room_id) {
            RoomManager::go_to_room_id(config->room_infos[i]->room_id); //
          }
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->entity_page_ids[j]);
        }
      }
    } else {
      ESP_LOGE("RoomManager", "Did not find currently selected page within config. Aborting.");
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to previous entities page.");
  }

  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_next_entities_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    bool select_next_entity_page = false;
    for (int i = 0; i < config->n_room_infos; i++) {
      for (int j = 0; j < config->room_infos[i]->n_entity_page_ids; j++) {
        if (config->room_infos[i]->entity_page_ids[j] == RoomManager::_current_entities_page_id) {
          select_next_entity_page = true;
        } else if (select_next_entity_page) {
          // We need to go to another room for this page, switch.
          if (config->room_infos[i]->room_id != RoomManager::_current_room_id) {
            RoomManager::go_to_room_id(config->room_infos[i]->room_id); //
          }
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->entity_page_ids[j]);
        }
      }
    }

    if (select_next_entity_page) {
      // Did not find any entity page after currently selected, try from beginning ie. "wrap" around
      for (int i = 0; i < config->n_room_infos; i++) {
        for (int j = 0; j < config->room_infos[i]->n_entity_page_ids; j++) {
          // We need to go to another room for this page, switch.
          if (config->room_infos[i]->room_id != RoomManager::_current_room_id) {
            RoomManager::go_to_room_id(config->room_infos[i]->room_id); //
          }
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->entity_page_ids[j]);
        }
      }
    } else {
      ESP_LOGE("RoomManager", "Did not find currently selected page within config. Aborting.");
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to next entities page.");
  }

  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_first_scenes_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
    for (int i = 0; i < config->n_room_infos; i++) {
      if (config->room_infos[i]->room_id == RoomManager::_current_room_id) {
        if (config->room_infos[i]->n_scene_page_ids > 0) [[likely]] {
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->scene_page_ids[0]);
          break;
        } else {
          ESP_LOGE("RoomManager", "Requested to go to first entity page in room but room has not entity pages. Will abort.");
          return ESP_ERR_NOT_FINISHED;
        }
      }
    }
    ESP_LOGE("RoomManager", "Failed to find currently selected room while trying to go to first entities page.");
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to first entities page for room.");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_previous_scenes_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    bool select_next_entity_page = false;
    for (size_t i = config->n_room_infos; i-- > 0;) {
      for (size_t j = config->room_infos[i]->n_scene_page_ids; j-- > 0;) {
        if (config->room_infos[i]->scene_page_ids[j] == RoomManager::_current_entities_page_id) {
          select_next_entity_page = true;
        } else if (select_next_entity_page) {
          // We need to go to another room for this page, switch.
          if (config->room_infos[i]->room_id != RoomManager::_current_room_id) {
            RoomManager::go_to_room_id(config->room_infos[i]->room_id); //
          }
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->scene_page_ids[j]);
        }
      }
    }

    if (select_next_entity_page) {
      // Did not find any entity page after currently selected, try from beginning ie. "wrap" around
      for (size_t i = config->n_room_infos; i-- > 0;) {
        for (size_t j = config->room_infos[i]->n_scene_page_ids; j-- > 0;) {
          // We need to go to another room for this page, switch.
          if (config->room_infos[i]->room_id != RoomManager::_current_room_id) {
            RoomManager::go_to_room_id(config->room_infos[i]->room_id); //
          }
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->scene_page_ids[j]);
        }
      }
    } else {
      ESP_LOGE("RoomManager", "Did not find currently selected page within config. Aborting.");
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to previous entities page.");
  }

  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_next_scenes_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    bool select_next_entity_page = false;
    for (int i = 0; i < config->n_room_infos; i++) {
      for (int j = 0; j < config->room_infos[i]->n_scene_page_ids; j++) {
        if (config->room_infos[i]->scene_page_ids[j] == RoomManager::_current_entities_page_id) {
          select_next_entity_page = true;
        } else if (select_next_entity_page) {
          // We need to go to another room for this page, switch.
          if (config->room_infos[i]->room_id != RoomManager::_current_room_id) {
            RoomManager::go_to_room_id(config->room_infos[i]->room_id); //
          }
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->scene_page_ids[j]);
        }
      }
    }

    if (select_next_entity_page) {
      // Did not find any entity page after currently selected, try from beginning ie. "wrap" around
      for (int i = 0; i < config->n_room_infos; i++) {
        for (int j = 0; j < config->room_infos[i]->n_scene_page_ids; j++) {
          // We need to go to another room for this page, switch.
          if (config->room_infos[i]->room_id != RoomManager::_current_room_id) {
            RoomManager::go_to_room_id(config->room_infos[i]->room_id); //
          }
          return RoomManager::go_to_entities_page_id(config->room_infos[i]->scene_page_ids[j]);
        }
      }
    } else {
      ESP_LOGE("RoomManager", "Did not find currently selected page within config. Aborting.");
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to next entities page.");
  }

  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_first_global_scenes_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) [[likely]] {
    if (config->n_global_scene_entity_page_ids > 0) {
      return RoomManager::go_to_entities_page_id(config->global_scene_entity_page_ids[0]);
    } else {
      ESP_LOGE("RoomManager", "Failed to navigate to first global scenes page, no global scenes pages loaded.");
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to first entities page for room.");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_previous_global_scenes_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    bool select_next_entity_page = false;
    for (size_t i = config->n_global_scene_entity_page_ids; i-- > 0;) {
      if (config->global_scene_entity_page_ids[i] == RoomManager::_current_entities_page_id) {
        select_next_entity_page = true;
      } else if (select_next_entity_page) {
        return RoomManager::go_to_entities_page_id(config->global_scene_entity_page_ids[i]);
      }
    }

    if (select_next_entity_page) {
      // Did not find any entity page after currently selected, try from end ie. "wrap" around
      if (config->n_global_scene_entity_page_ids > 0) {
        return RoomManager::go_to_entities_page_id(config->global_scene_entity_page_ids[config->n_global_scene_entity_page_ids - 1]);
      } else {
        ESP_LOGE("RoomManager", "No global entity pages loaded. Aborting.");
      }
    } else {
      ESP_LOGE("RoomManager", "Did not find currently selected page within config. Aborting.");
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to previous entities page.");
  }

  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::go_to_next_global_scenes_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    bool select_next_entity_page = false;
    for (int i = 0; i < config->n_global_scene_entity_page_ids; i++) {
      if (config->global_scene_entity_page_ids[i] == RoomManager::_current_entities_page_id) {
        select_next_entity_page = true;
      } else if (select_next_entity_page) {
        return RoomManager::go_to_entities_page_id(config->global_scene_entity_page_ids[i]);
      }
    }

    ESP_LOGD("RoomManager", "Num loaded global scenes: %zu", config->n_global_scene_entity_page_ids);

    if (select_next_entity_page) {
      // Did not find any entity page after currently selected, try from beginning ie. "wrap" around
      if (config->n_global_scene_entity_page_ids > 0) {
        return RoomManager::go_to_entities_page_id(config->global_scene_entity_page_ids[0]);
      } else {
        ESP_LOGE("RoomManager", "No global entity pages loaded. Aborting.");
      }
    } else {
      ESP_LOGE("RoomManager", "Did not find currently selected page within config. Aborting.");
    }
  } else {
    ESP_LOGE("RoomManager", "Failed to get config while trying to go to next entities page.");
  }

  return ESP_ERR_NOT_FINISHED;
}

esp_err_t RoomManager::register_handler(int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg) {
  if (RoomManager::_local_event_loop != NULL) {
    return esp_event_handler_register_with(RoomManager::_local_event_loop, ROOMMANAGER_EVENT, event_id, event_handler, event_handler_arg);
  } else {
    return ESP_ERR_NOT_FINISHED;
  }
}

esp_err_t RoomManager::unregister_handler(int32_t event_id, esp_event_handler_t event_handler) {
  if (RoomManager::_local_event_loop != NULL) {
    return esp_event_handler_unregister_with(RoomManager::_local_event_loop, ROOMMANAGER_EVENT, event_id, event_handler);
  } else {
    return ESP_ERR_NOT_FINISHED;
  }
}

void RoomManager::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == MQTT_EVENT_DATA) {
    // This function is only registered for MQTT_EVENT_DATA, handle data config data:
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    std::string topic_string = std::string(event->topic, event->topic_len);

    if (topic_string.compare(RoomManager::_current_home_page_status_topic.get()) == 0) {
      NSPanelRoomStatus *room_status = nspanel_room_status__unpack(NULL, event->data_len, (const uint8_t *)event->data);
      if (room_status != NULL) {
        if (xSemaphoreTake(RoomManager::_home_page_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
          ESP_LOGD("RoomManager", "Received new home page state update.");
          RoomManager::_home_page = std::shared_ptr<NSPanelRoomStatus>(room_status, &RoomManager::_nspanel_room_status_shared_ptr_deleter);
          xSemaphoreGive(RoomManager::_home_page_mutex);

          if (esp_event_post_to(RoomManager::_local_event_loop, ROOMMANAGER_EVENT, roommanager_event_t::HOME_PAGE_UPDATED, NULL, 0, pdMS_TO_TICKS(250)) != ESP_OK) {
            ESP_LOGW("RoomManager", "Failed to publish event that new home page data is available.");
          }
        } else {
          ESP_LOGE("RoomManager", "Got new status for home page but couldn't take mutex to update it! Will free new state.");
          nspanel_room_status__free_unpacked(room_status, NULL);
        }
      } else {
        ESP_LOGE("RoomManager", "Got new status for home page but failed to unpack it.");
      }
    } else if (topic_string.compare(RoomManager::_current_entities_page_status_topic.get()) == 0) {
      NSPanelRoomEntitiesPage *entities_page = nspanel_room_entities_page__unpack(NULL, event->data_len, (const uint8_t *)event->data);
      if (entities_page != NULL) {
        if (xSemaphoreTake(RoomManager::_entities_page_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
          ESP_LOGD("RoomManager", "Received new entities page state update.");
          RoomManager::_entities_page = std::shared_ptr<NSPanelRoomEntitiesPage>(entities_page, &RoomManager::_nspanel_room_entities_page_shared_ptr_deleter);
          if (xSemaphoreGive(RoomManager::_entities_page_mutex) != pdTRUE) [[unlikely]] {
            ESP_LOGE("RoomManager", "Failed to give back _entities_page_mutex after updating shared_ptr to new NSPanelRoomEntitiesPage object.");
          }

          if (esp_event_post_to(RoomManager::_local_event_loop, ROOMMANAGER_EVENT, roommanager_event_t::ROOM_ENTITIES_PAGE_UPDATED, NULL, 0, pdMS_TO_TICKS(250)) != ESP_OK) {
            ESP_LOGW("RoomManager", "Failed to publish event that new entities page is available.");
          }
        } else {
          ESP_LOGE("RoomManager", "Got new status for entities page but couldn't take mutex to update it! Will free unpacked data.");
          nspanel_room_entities_page__free_unpacked(entities_page, NULL);
        }
      } else {
        ESP_LOGE("RoomManager", "Got new status for entities page but failed to unpack it.");
      }
    } else if (topic_string.compare(RoomManager::_all_rooms_state_topic) == 0) {
      NSPanelRoomStatus *all_room_status = nspanel_room_status__unpack(NULL, event->data_len, (const uint8_t *)event->data);
      if (all_room_status != NULL) {
        if (xSemaphoreTake(RoomManager::_home_page_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
          ESP_LOGD("RoomManager", "Received new home page state update for 'All rooms' mode.");
          RoomManager::_home_page_all_rooms = std::shared_ptr<NSPanelRoomStatus>(all_room_status, &RoomManager::_nspanel_room_status_shared_ptr_deleter);
          xSemaphoreGive(RoomManager::_home_page_mutex);

          if (esp_event_post_to(RoomManager::_local_event_loop, ROOMMANAGER_EVENT, roommanager_event_t::HOME_PAGE_ALL_ROOMS_UPDATED, NULL, 0, pdMS_TO_TICKS(250)) != ESP_OK) {
            ESP_LOGW("RoomManager", "Failed to publish event that new home page data is available when receiving 'All rooms' data.");
          }
        } else {
          ESP_LOGE("RoomManager", "Got new status for 'All rooms' for home page but couldn't take mutex to update it! Will free new state.");
          nspanel_room_status__free_unpacked(all_room_status, NULL);
        }
      } else {
        ESP_LOGE("RoomManager", "Got new status for 'All rooms' for home page but failed to unpack it.");
      }
    }
  }
}

void RoomManager::_mqtt_event_handler_connected(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // ESP_LOGI("RoomManager", "MQTT connected. Subscribing to room status topics.");
}

bool RoomManager::_move_topic(std::string *current_topic, std::string new_topic) {
  if (new_topic.compare(*current_topic) == 0) {
    return false;
  }
  if (!current_topic->empty()) {
    MqttManager::unsubscribe(*current_topic);
  }
  *current_topic = new_topic;
  if (MqttManager::subscribe(new_topic) != ESP_OK) {
    ESP_LOGE("RoomManager", "Failed to subscribe to state topic %s.", new_topic.c_str());
  }
  return true;
}

void RoomManager::_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base != NSPM_CONFIGMANAGER_EVENT || event_id != nspm_configmanager_event::CONFIG_LOADED) {
    return;
  }

  std::string manager_address = NSPM_ConfigManager::get_manager_address();
  if (manager_address.empty()) [[unlikely]] {
    ESP_LOGE("RoomManager", "Handled CONFIG_LOADED with no manager address set.");
    return;
  }
  std::string topic_prefix = "nspanel/mqttmanager_";
  topic_prefix.append(manager_address);

  // Every topic below embeds the manager address, so a manager that has moved leaves them
  // all pointing at somewhere that will never publish again. MqttManager keeps resubscribing
  // whatever it was last told, so nothing else notices; the topics have to be moved here.
  RoomManager::_move_topic(&RoomManager::_all_rooms_state_topic, topic_prefix + "/all_rooms_status");

  // First config: we have not picked a room yet, so go to the default one. go_to_room_id()
  // builds the room topic from the current manager address itself.
  if (RoomManager::_current_home_page_status_topic.get().empty()) {
    std::shared_ptr<NSPanelConfig> config;
    if (NSPM_ConfigManager::get_config(&config) != ESP_OK) {
      ESP_LOGE("RoomManager", "Failed to get current config while handling CONFIG_LOADED.");
      return;
    }
    RoomManager::go_to_default_room();
    return;
  }

  std::string room_topic = RoomManager::_current_home_page_status_topic.get();
  if (RoomManager::_move_topic(&room_topic, topic_prefix + "/room/" + std::to_string(RoomManager::_current_room_id) + "/state")) {
    RoomManager::_current_home_page_status_topic.set(room_topic);
  }

  std::string entities_topic = RoomManager::_current_entities_page_status_topic.get();
  if (!entities_topic.empty() && RoomManager::_move_topic(&entities_topic, topic_prefix + "/entity_pages/" + std::to_string(RoomManager::_current_entities_page_id) + "/state")) {
    RoomManager::_current_entities_page_status_topic.set(entities_topic);
  }
}

void RoomManager::_nspanel_room_status_shared_ptr_deleter(NSPanelRoomStatus *status) {
  nspanel_room_status__free_unpacked(status, NULL);
}

void RoomManager::_nspanel_room_entities_page_shared_ptr_deleter(NSPanelRoomEntitiesPage *status) {
  nspanel_room_entities_page__free_unpacked(status, NULL);
}