#pragma once
#include <MutexWrapper.hpp>
#include <atomic>
#include <esp_event.h>
#include <list>
#include <memory>
#include <protobuf_nspanel.pb-c.h>

class RoomManager {
public:
  /**
   * Load and initialize RoomManager.
   */
  static void init();

  /**
   * @brief Get the home page status
   * @param status: Reference to where to save NSPanelRoomStatus to
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t get_home_page_status(std::shared_ptr<NSPanelRoomStatus> *status);

  /**
   * @brief Get the home page status for all rooms
   * @param status: Reference to where to save NSPanelRoomStatus to
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t get_home_page_status_all_rooms(std::shared_ptr<NSPanelRoomStatus> *status);

  /**
   * @brief Get a room status pointer to a __unpacked protobuf
   * @param status: Reference to where to save NSPanelRoomStatus to
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t get_home_page_status_mutable(std::shared_ptr<NSPanelRoomStatus> *status);

  /**
   * @brief Get a room status pointer to a __unpacked protobuf for status for all rooms
   * @param status: Reference to where to save NSPanelRoomStatus to
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t get_home_page_status_mutable_all_rooms(std::shared_ptr<NSPanelRoomStatus> *status);

  /**
   * @brief Go to the specified room
   * @param room_id: The ID of the room to go to.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_room_id(uint32_t room_id);

  /**
   * @brief Navigate to the previous room in order
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_previous_room();

  /**
   * @brief Navigate to the next room in order
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_next_room();

  /**
   * @brief Navigate to the default room
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_default_room();

  /**
   * @brief Return the ID of the currently displayed room.
   * @return ID of currently displayed room.
   */
  static uint32_t get_current_room_id();

  /**
   * @brief Will replace current room status with the one provided
   * @param status: The new room status to be replaced into the list of rooms.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t replace_home_page_status(std::shared_ptr<NSPanelRoomStatus> status);

  /**
   * @brief Will replace room status object respresenting all rooms status with the one provided
   * @param status: The new room status to be replaced into the list of rooms.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t replace_home_page_status_all_rooms(std::shared_ptr<NSPanelRoomStatus> status);

  /**
   * @brief Get an NSPanelRoomEntitiesPage status object for the currently selected room that contains all entities to be displayed and their values
   * @param status: Reference to where to save NSPanelRoomEntitiesPage to
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t get_current_room_entities_page_status(std::shared_ptr<NSPanelRoomEntitiesPage> *status);

  /**
   * @brief Go to the specified entities page
   * @param room_id: The ID of the page to go to.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_entities_page_id(uint32_t page_id);

  /**
   * @brief Go to the first entities page for the currently selected room.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t go_to_first_entities_page();

  /**
   * @brief Navigate to the previous entities page in order and if needed, change to previous room with an entity page defined.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_previous_entities_page();

  /**
   * @brief Navigate to the next entities page in order and if needed, change to next room with an entity page defined.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_next_entities_page();

  /**
   * @brief Go to the first scenes page for the currently selected room.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t go_to_first_scenes_page();

  /**
   * @brief Navigate to the previous scenes page in order and if needed, change to previous room with an entity page defined.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_previous_scenes_page();

  /**
   * @brief Navigate to the next scenes page in order and if needed, change to next room with an entity page defined.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_next_scenes_page();

  /**
   * @brief Go to the first global scenes page.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED
   */
  static esp_err_t go_to_first_global_scenes_page();

  /**
   * @brief Navigate to the previous global scenes page in order and if needed wrap around to end.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_previous_global_scenes_page();

  /**
   * @brief Navigate to the next scenes page in order and if needed, wrap around to beginning.
   * @return ESP_OK if operation was successful, otherwise ESP_ERR_NOT_FINISHED or ESP_ERR_NOT_FOUND
   */
  static esp_err_t go_to_next_global_scenes_page();

  /**
   * @brief Register event handler for events.
   * @param event_id: The event id to subscribe to.
   * @param event_handler: The event handler callback to register
   * @param event_handler_arg: Any argument to the event handler.
   */
  static esp_err_t register_handler(int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg);

  /**
   * @brief The loop ROOMMANAGER_EVENTs are posted on, for PageHandlers::add_to().
   */
  static esp_event_loop_handle_t event_loop() { return RoomManager::_local_event_loop; }

  /**
   * @brief Unregister event handler for events.
   * @param event_id: The event id to unsubscribe from.
   * @param event_handler: The event handler callback to register
   */
  static esp_err_t unregister_handler(int32_t event_id, esp_event_handler_t event_handler);

private:
  /**
   * @brief Handle events triggered from MQTT places.
   */
  static void _mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle events triggered from MQTT when it is connected to MQTT.
   */
  static void _mqtt_event_handler_connected(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle events triggered from other places.
   */
  static void _event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Point a stored state topic at @param new_topic, unsubscribing from whatever it
   * held before. Does nothing if the topic is unchanged.
   * @param current_topic: The stored topic to update in place.
   * @param new_topic: The topic to subscribe to instead.
   * @return True if the topic was moved, false if it was already correct.
   */
  static bool _move_topic(std::string *current_topic, std::string new_topic);

  /**
   * Function used when shared_ptr goes out of scope. Used to delete the underlying object
   * using the nspanel_room_status__free_unpacked function
   */
  static void _nspanel_room_status_shared_ptr_deleter(NSPanelRoomStatus *status);

  /**
   * Function used when shared_ptr goes out of scope. Used to delete the underlying object
   * using the nspanel_room_entities_page__free_unpacked function
   */
  static void _nspanel_room_entities_page_shared_ptr_deleter(NSPanelRoomEntitiesPage *status);

  // Vars:
  // Status of home page for currently selected room
  static inline std::shared_ptr<NSPanelRoomStatus> _home_page = nullptr;

  // The MQTT topic to receive the NSPanelRoomStatus protobuf object from.
  static inline MutexWrapped<std::string> _current_home_page_status_topic;

  // The currently selected room ID
  static inline std::atomic<uint32_t> _current_room_id;

  // The MQTT topic for the "All roms" mode state topic.
  static inline std::string _all_rooms_state_topic;

  // Status of home page for all rooms
  static inline std::shared_ptr<NSPanelRoomStatus> _home_page_all_rooms = nullptr;

  // Mutex to only allow one task at the time to access the status of the home page
  static inline SemaphoreHandle_t _home_page_mutex;

  // Current entities page that is showing
  static inline std::shared_ptr<NSPanelRoomEntitiesPage> _entities_page;

  // The MQTT topic to receive EntitiesPage protobuf object from.
  static inline MutexWrapped<std::string> _current_entities_page_status_topic;

  // The currently selected entity page ID
  static inline std::atomic<uint32_t> _current_entities_page_id;

  // Mutex to allow only one task at the time to access the current entities page shared_ptr
  static inline SemaphoreHandle_t _entities_page_mutex;

  // Task handle to task that loads all rooms via MQTT
  static inline TaskHandle_t _load_all_rooms_task_handle;

  // Arguments used to initialize the local event loop for RoomManager events
  static inline esp_event_loop_args_t _local_event_loop_args;

  // Event loop used for RoomManager events
  static inline esp_event_loop_handle_t _local_event_loop = NULL;
};