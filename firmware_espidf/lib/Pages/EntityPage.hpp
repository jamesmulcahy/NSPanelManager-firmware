#pragma once

#include <atomic>
#include <memory>
#include <protobuf_nspanel_entity.pb-c.h>
#include <string>

/*
 * The page that will handle control of individual entities
 */
class EntityPage {
public:
  /**
   * Show the loading page
   */
  static void show(std::string state_topic);

  /**
   * Unshow the loading page
   */
  static void unshow();

private:
  /**
   * Handle events from MQTT, such as new state events:
   */
  static void _handle_mqtt_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Handle events from Nextion, such as touch events:
   */
  static void _handle_nextion_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Handle event for a new config from MQTTManager:
   */
  static void _handle_config_update(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Delegate to the correct update method
   */
  static void _update_display();

  /**
   * Delegate to the correct touch method
   */
  static void _handle_touch_event(const std::shared_ptr<NSPanelEntityState> &state, uint16_t component_id, bool pressed);

  /*
   * Update displayed page and value of "Light" page.
   */
  static void _update_display_light();

  /*
   * Handle touch event for "Light" page.
   */
  static void _handle_touch_event_light(const std::shared_ptr<NSPanelEntityState> &state, uint16_t component_id, bool pressed);

  /*
   * Update displayed page and value of "Thermostat" page.
   */
  static void _update_display_thermostat();

  /*
   * Handle Nextion string event.
   */
  static void _handle_string_event_thermostat(const std::shared_ptr<NSPanelEntityState> &state, char *data);

  /**
   * properly delete pointer and clear old data when shared_ptr expires
   */
  static void _delete_nspanel_entity_state_object(NSPanelEntityState *object);

  // Get a shared_ptr to the current state of the entity.
  static inline std::shared_ptr<NSPanelEntityState> _get_current_state();

  // If currently editing, check what has changed and sent the new setting to the manager to apply setting.
  static void _send_thermostat_option_command();

  // Send new set temp for a thermostat
  static void _send_thermostat_setpoint_command();

  // What is the page currently showing
  enum _entity_page_modes {
    LIGHT_COLOR_TEMPERATURE,
    LIGHT_RGB,
    THERMOSTAT,
  };
  static inline std::atomic<_entity_page_modes> _current_mode;

  // Vars
  // The state topic of the given entity
  static inline std::string _current_entity_mqtt_topic;

  // Are we currently displaying the page?
  static inline std::atomic<bool> _currently_showing = false;

  // The last name to be updated on the display.
  static inline std::string _last_displayed_name = "";

  // Light specific variables
  static inline uint8_t _last_brightness = 0;
  static inline uint8_t _last_kelvin_pct = 0;
  static inline uint8_t _last_hue = 0;
  static inline uint8_t _last_saturation_pct = 0;

  // Thermostat specific variables
  static inline std::atomic<bool> _is_currently_editing = false;
  static inline uint8_t _selected_thermostat_option_index = 0;

  // Written by the MQTT handler, read by the Nextion event loop. Never dereference _current_state directly:
  // `->` on a shared_ptr returns a raw pointer and holds no reference, so a new state arriving mid-read can
  // free the one you are reading. Call _get_current_state() once per entry point, keep that copy for the
  // whole operation, and pass it down by const&. It returns nullptr until the first state has arrived.
  static inline SemaphoreHandle_t _current_state_mutex = NULL;
  static inline std::shared_ptr<NSPanelEntityState> _current_state;

  static inline portMUX_TYPE _entity_page_spinlock = portMUX_INITIALIZER_UNLOCKED;
};