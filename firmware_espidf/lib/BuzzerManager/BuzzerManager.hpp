#pragma once
#include <array>
#include <cstddef>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <memory>
#include <string>
#include <vector>

struct buzzer_tone_step_t {
  // Tone frequency, 0 for a pause
  uint32_t frequency_hz;

  // How long to play the tone or pause
  uint32_t duration_ms;
};

// Panel events that can have a sound attached to them. NSPanelManager configures the sound for each event,
// see NSPanelConfig.sounds. Keep COUNT last.
enum class buzzer_event_t {
  TOUCH,           // The screen was touched
  SCENE_ACTIVATED, // A scene was triggered from the panel
  SCENE_SAVED,     // A scene was saved from the panel
  COUNT,
};

class BuzzerManager {
public:
  /**
   * @brief Configure the buzzer output, register for Nextion touch events so that screen touches are
   * tracked as buzzer events, and register for config updates to load the sound for each event.
   */
  static void init();

  /**
   * @brief Subscribe to the raw buzzer command topic, nspanel/<mac>/buzzer_raw_command. Publishing an RTTTL
   * string to it plays that sound, an empty message stops playback. Retained messages are ignored so a
   * sound is not replayed on every reconnect. Call after MqttManager::start().
   */
  static void init_mqtt();

  /**
   * @brief Play a sequence of tones and pauses. Does not block, each step is advanced from an
   * esp_timer callback. Calling this while a sequence is playing restarts with the new one.
   * @param steps: The sequence to play. Playback keeps its own reference until it finishes.
   */
  static void play(std::shared_ptr<const std::vector<buzzer_tone_step_t>> steps);

  /**
   * @brief Stop any sound that is playing.
   */
  static void stop();

  /**
   * @brief Raise an event that is not tied to a screen touch, ie. a scene being saved after a long press.
   * Safe to call from any task.
   */
  static void raise_event(buzzer_event_t event);

  /**
   * @brief Raise an event for the screen touch being handled, in place of the TOUCH event it would otherwise
   * raise, so a touch that does something specific can get its own sound instead of the generic touch sound.
   * If the event has no sound the TOUCH event is raised as usual.
   * Call from a NEXTION_EVENT handler registered with ESP_EVENT_ANY_ID, ie. a page's touch handler. ESP-IDF runs
   * those before handlers registered for a specific event id, like BuzzerManager's TOUCH_EVENT handler, which
   * then skips the TOUCH event for this touch.
   */
  static void raise_event_for_touch(buzzer_event_t event);

private:
  /**
   * @brief Handle RTTTL strings published to the raw buzzer command topic.
   */
  static void _mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle touch events from the Nextion display. Raise a TOUCH event on press, or on release
   * for components that only send release events. A release following a press of the same component
   * is ignored so components that send both only raise one event.
   */
  static void _nextion_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Load the sound for each event from the NSPanelConfig when a new config is loaded.
   */
  static void _config_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Handle a panel event that may have a sound attached to it. Called from any task, see raise_event().
   * @return true if the event has a sound, which is now playing
   */
  static bool _handle_event(buzzer_event_t event);

  /**
   * @brief esp_timer callback that advances to the next step once the current one has finished.
   */
  static void _step_timer_callback(void *arg);

  /**
   * @brief Start output for the step at _current_step and arm the timer for its end.
   * Silences the buzzer when there are no steps left. Must hold _playback_mutex.
   */
  static void _start_current_step();

  /**
   * @brief Set the buzzer output. A frequency of 0 silences it. Must hold _playback_mutex.
   */
  static void _set_output(uint32_t frequency_hz);

  static inline bool _initialized = false;
  static inline esp_timer_handle_t _step_timer = NULL;

  // Playback state, only accessed while holding _playback_mutex
  static inline SemaphoreHandle_t _playback_mutex = NULL;
  static inline std::shared_ptr<const std::vector<buzzer_tone_step_t>> _steps;
  static inline size_t _current_step = 0;
  static inline int64_t _current_step_end_us = 0; // Lets a callback that raced with play() detect it is stale

  // Sound for each event, indexed by buzzer_event_t. nullptr if the event has no sound. Replaced as a whole when a
  // new config is loaded, playback keeps its own reference to the steps. Only accessed while holding _sounds_mutex.
  static inline SemaphoreHandle_t _sounds_mutex = NULL;
  static inline std::array<std::shared_ptr<const std::vector<buzzer_tone_step_t>>, static_cast<size_t>(buzzer_event_t::COUNT)> _sounds;

  // Component that last raised a TOUCH event on press and has not yet been released. Only accessed
  // from the default event loop task.
  static inline bool _awaiting_release = false;
  static inline uint8_t _pressed_page = 0;
  static inline uint8_t _pressed_component = 0;

  // Set by raise_event_for_touch() when the touch being handled already raised its own event. Only accessed
  // from the default event loop task.
  static inline bool _touch_event_replaced = false;

  static inline std::string _raw_command_topic;

  static constexpr const ledc_mode_t _ledc_mode = LEDC_LOW_SPEED_MODE;
  static constexpr const ledc_timer_t _ledc_timer = LEDC_TIMER_1;
  static constexpr const ledc_channel_t _ledc_channel = LEDC_CHANNEL_1;
  static constexpr const ledc_timer_bit_t _ledc_resolution = LEDC_TIMER_10_BIT;
  static constexpr const uint32_t _ledc_initial_frequency_hz = 4000;
  static constexpr const uint32_t _ledc_duty_on = 1 << (BuzzerManager::_ledc_resolution - 1); // 50% duty

#if defined(BOARD_SONOFF)
  static constexpr const gpio_num_t _buzzer_pin = gpio_num_t::GPIO_NUM_21;
#endif
  // TODO: Open question: does the custom PCB (BOARD_CUSTOM) have a buzzer, and on which pin? Until that is known
  // the buzzer is only enabled for BOARD_SONOFF. On other boards init() leaves it disabled, so play() does nothing
  // and init_mqtt() does not subscribe to the raw command topic.
};
