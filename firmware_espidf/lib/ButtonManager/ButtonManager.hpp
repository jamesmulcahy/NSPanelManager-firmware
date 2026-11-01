#ifndef BUTTON_MANAGER_HPP
#define BUTTON_MANAGER_HPP

#include <InterruptButton.h>
#include <atomic>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <memory>
#include <protobuf_nspanel.pb-c.h>
#include <string>

class ButtonManager {
public:
  static void init();

  static void init_mqtt();

private:
  /**
   * Handle key down event. This is only used for "Follow mode".
   */
  static void _button1_key_down(void);

  /**
   * Handle key down event. This is only used for "Follow mode".
   */
  static void _button1_key_up(void);

  /**
   * Handle key down event. This is only used for "Follow mode".
   */
  static void _button2_key_down(void);

  /**
   * Handle key down event. This is only used for "Follow mode".
   */
  static void _button2_key_up(void);

  /**
   * Handle press event of button1. This is triggered on release of button.
   */
  static void _button1_press(void);

  /**
   * Handle press event of button2. This is triggered on release of button.
   */
  static void _button2_press(void);

  /**
   * Open the entity page that the config gives for a button in OPEN_ENTITY_PAGE mode.
   */
  static void _open_entity_page(uint8_t button);

  /**
   * Set the state of a relay and send state update to MQTT
   */
  static void _set_relay_state(uint8_t relay, bool state, bool send_mqtt_update);

  /**
   * Get the current output state of a relay
   */
  static bool _get_relay_state(uint8_t relay);

  /**
   * Subscribe/Unsubscribe from relay groups for relays
   */
  static void _handle_mqtt_relay_group_topics();

  /**
   * @brief Handle any event trigger from the NSPM_ConfigManager
   */
  static void _nspm_configmanager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle new temperature and if in thermostat mode check for any changes and set relay state accordingly.
   */
  static void _new_temperature_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle data received over MQTT
   */
  static void _mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  // Settings
  static inline std::atomic<bool> _reverse_relays = false;      // Should relays be reversed/flipped?
  static inline std::atomic<bool> _relay1_default_mode = false; // Relay 1 on by default?
  static inline std::atomic<bool> _relay2_default_mode = false; // Relay 2 on by default?
  static inline std::atomic<bool> _relay1_current_state = false;
  static inline std::atomic<bool> _relay2_current_state = false;
  static inline NSPanelConfig__NSPanelButtonMode _button1_mode = NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__DIRECT; // Start by using default direct mode
  static inline NSPanelConfig__NSPanelButtonMode _button2_mode = NSPanelConfig__NSPanelButtonMode::NSPANEL_CONFIG__NSPANEL_BUTTON_MODE__DIRECT; // Start by using default direct mode;

  // Queue of interrupt events to handle
  static inline QueueHandle_t _interrupt_queue;

  // Button data
  static inline std::atomic<uint32_t> _min_button_push_time;
  static inline std::atomic<uint32_t> _min_button_long_push_time;
#if defined(BOARD_SONOFF)
  static constexpr const gpio_num_t _button1_pin = gpio_num_t::GPIO_NUM_14;
  static constexpr const gpio_num_t _button2_pin = gpio_num_t::GPIO_NUM_27;
#elif defined(BOARD_CUSTOM)
  static constexpr const gpio_num_t _button1_pin = gpio_num_t::GPIO_NUM_14;
  static constexpr const gpio_num_t _button2_pin = gpio_num_t::GPIO_NUM_15;
#endif

  static inline InterruptButton *_button1 = nullptr;
  static inline InterruptButton *_button2 = nullptr;

  // Relay data
  static inline gpio_config_t _relay_io_config;
#if defined(BOARD_SONOFF)
  static constexpr const gpio_num_t _relay1_pin = gpio_num_t::GPIO_NUM_22;
  static constexpr const gpio_num_t _relay2_pin = gpio_num_t::GPIO_NUM_19;
#elif defined(BOARD_CUSTOM)
  static constexpr const gpio_num_t _relay1_pin = gpio_num_t::GPIO_NUM_16;
  static constexpr const gpio_num_t _relay2_pin = gpio_num_t::GPIO_NUM_17;
#endif
  static constexpr const uint32_t _relay_pin_mask = ((1ULL << _relay1_pin) | (1ULL << _relay2_pin));

  static inline uint64_t _last_relay1_change = 0;
  static inline uint64_t _last_relay2_change = 0;

  // MQTT topics for relay commands and state. Built once in init().
  static inline std::string _relay1_cmd_topic;
  static inline std::string _relay2_cmd_topic;
  static inline std::string _relay1_state_topic;
  static inline std::string _relay2_state_topic;

  // The current/previous config. Used to compare and check for changes.
  static inline std::shared_ptr<NSPanelConfig> _current_config = nullptr;
};

#endif