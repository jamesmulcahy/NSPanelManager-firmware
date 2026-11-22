#pragma once
#include <MutexWrapper.hpp>
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
   * Update display with new values from the given entities page.
   */
  static void _update_display(const std::shared_ptr<NSPanelRoomEntitiesPage> &page);

private:
  /**
   * Handle RoomManager event
   */
  static void _handle_roommanager_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Get the entity in the given slot of the given entities page, or nullptr if the slot is empty.
   */
  static NSPanelRoomEntitiesPage__EntitySlot *_get_entity_in_slot(const std::shared_ptr<NSPanelRoomEntitiesPage> &page, int32_t slot);

  /**
   * Handle a press on the entity button in the given slot.
   * Media players are opened on the entity page, all other entities are toggled.
   */
  static void _handle_entity_button_pressed(const std::shared_ptr<NSPanelRoomEntitiesPage> &page, int32_t slot);

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

  /**
   * With LVGL as the UI (LvglUi): draw `page` with EntitiesView. Any task.
   */
  static void _lvgl_render(const std::shared_ptr<NSPanelRoomEntitiesPage> &page);

  /**
   * _lvgl_render() of the page shown now, if any (after the save progress or header changed).
   */
  static void _lvgl_refresh();

  /**
   * An EntitiesView action (EntitiesView::Action, as int to keep LVGL out of this header), on
   * LvglUi's task.
   */
  static void _lvgl_action(int action, int32_t slot);

  /**
   * Previous or next page of what is shown (entities, scenes or global scenes).
   */
  static void _step_page(bool next);

  // Vars
  // Shared between tasks: written by the RoomManager event loop and unshow(), read by the LVGL
  // action handler and the save-scene task.
  //
  // Never dereference this directly. `->` on a shared_ptr returns a raw pointer and holds no reference,
  // so if another task replaces or clears the page while you are reading through it, the old page can be
  // freed underneath you (this member is often its last owner). Instead, call get() once per entry point,
  // keep that local copy for the whole operation, and pass it down by const&. The copy holds a reference,
  // so the page stays alive until you are done.
  //
  // The mutex protects taking the copy itself: only the refcount inside a shared_ptr is atomic, so copying
  // it while another task assigns it is a data race.
  static inline MutexWrapped<std::shared_ptr<NSPanelRoomEntitiesPage>> _current_entities_page;

  // Set to 4, 8 or 12 while a page is shown, 0 otherwise.
  static inline std::atomic<uint8_t> _currently_showing_page_type = 0;

  static inline std::atomic<display_type_t> _display_type = display_type_t::ENTITIES;

  // The MQTT where the current entities page update is sent.
  static inline MutexWrapped<std::string> _current_entities_page_mqtt_topic;

  // Task handle for task that will update the screen and save a scene.
  static inline TaskHandle_t _save_scene_task_handle = NULL;

  // The entities page slot to save the scene.
  static inline std::atomic<bool> _save_scene = false;
  // LVGL: the save bar (0-100, -1 hidden) and a header shown instead of the page's ("Scene saved").
  static inline std::atomic<int> _lvgl_save_progress = -1;
  static inline MutexWrapped<std::string> _lvgl_header_override;
};