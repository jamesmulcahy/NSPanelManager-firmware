#pragma once
#include <GUI_data.hpp>
#include <MutexWrapper.hpp>
#include <Nextion_event.hpp>
#include <atomic>
#include <esp_event.h>
#include <memory>
#include <protobuf_nspanel.pb-c.h>
#include <string>

class EntitiesPage {
public:
  enum display_type_t {
    ENTITIES,
    SCENES,
    GLOBAL_SCENES
  };

  /**
   * Show the entities page for entity control
   * @param display_type: What should be displayed
   */
  static void show(display_type_t display_type);

  /**
   * Unshow the loading page
   */
  static void unshow();

  /**
   * Update display with new values from _current_entities_page.
   */
  static void _update_display();

  /**
   * Updated displayed items in each slot.
   */
  static void _update_displayed_items();

private:
  /**
   * Update a given items slot with the given EntitySlot data. In case slot_data is nullptr, blank the slot.
   */
  static void _update_displayed_item_in_slot(NSPanelRoomEntitiesPage__EntitySlot *slot_data, const GUI_ITEMS_PAGE_ITEM_DATA page_slot);

  /**
   * Handle RoomManager event
   */
  static void _handle_roommanager_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Handle Nextion events, such as touch
   */
  static void _handle_nextion_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Handle touch event in "items4" page.
   */
  static void _handle_items4_touch_event(nextion_event_touch_t *touch_data);

  /**
   * Handle touch event in "items8" page.
   */
  static void _handle_items8_touch_event(nextion_event_touch_t *touch_data);

  /**
   * Handle touch event in "items12" page.
   */
  static void _handle_items12_touch_event(nextion_event_touch_t *touch_data);

  /**
   * Get the entity in the given slot of the current entities page, or nullptr if the slot is empty.
   */
  static NSPanelRoomEntitiesPage__EntitySlot *_get_entity_in_slot(int32_t slot);

  /**
   * Handle a press on the entity button in the given slot.
   * Media players are opened on the entity page, all other entities are toggled.
   */
  static void _handle_entity_button_pressed(int32_t slot);

  /**
   * Send toggle command to manager
   */
  static void _send_entity_toggle_command_to_manager(uint32_t entities_page_id, uint32_t entity_slot);

  /**
   * Count to three seconds and update progress on screen.
   * If the task has not been stopped it will send the command to save the scene.
   * @param scene_slot: int32_t of which slot to save scene.
   */
  static void _task_save_scene_progress(void *scene_slot);

  // Vars
  static inline std::shared_ptr<NSPanelRoomEntitiesPage> _current_entities_page;

  // Set to 4, 8 or 12. Used to determine if we should send the command to switch page on Nextion display.
  static inline std::atomic<uint8_t> _currently_showing_page_type = 0;

  static inline std::atomic<display_type_t> _display_type = display_type_t::ENTITIES;

  // What is the currently displayed header text. Used to determine if we should update header text.
  static inline MutexWrapped<std::string> _currently_showing_header_text;

  // The MQTT where the current entities page update is sent.
  static inline MutexWrapped<std::string> _current_entities_page_mqtt_topic;

  // Task handle for task that will update the screen and save a scene.
  static inline TaskHandle_t _save_scene_task_handle = NULL;

  // The entities page slot to save the scene.
  static inline std::atomic<bool> _save_scene = false;
};