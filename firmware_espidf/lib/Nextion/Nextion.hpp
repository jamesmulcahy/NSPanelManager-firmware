#pragma once
#include <cstdint>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_err.h>
#include <esp_event.h>
#include <freertos/semphr.h>
#include <string>
#include <vector>

/**
 * Different available states of the Nextion display
 */
enum nextion_state_t {
  INITIALIZING,
  RUNNING,
  INITIALIZING_UPDATE,
  UPDATING
};

static constexpr uint16_t NEXTION_UART_EVENT_LOOP_STACK_SIZE = 3096;
static constexpr uint16_t NEXTION_UART_EVENT_LOOP_QUEUE_SIZE = 6;

class Nextion {
  friend class NextionWindow;

public:
  /**
   * Initialize the Nextion display and gather any facts about it.
   */
  static esp_err_t init();

  /**
   * @brief Go to the specified page
   * @param page: The name of the Nextion page to go to.
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t go_to_page(const char *page, uint16_t mutex_timeout);

  /**
   * @brief Send command to set Nextion display in "sleep mode"
   */
  static void sleep();

  /**
   * @brief Send command to wake Nextion display from "sleep mode"
   */
  static void wake();

  /**
   * @brief Send raw command data to the Nextion display.
   * @param command: The raw command to send
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t send_raw_command(const char *command, uint16_t mutex_timeout);

  /**
   * @brief Send raw command data to the Nextion display.
   * @param x: Start X of rectangle
   * @param y: Start Y of rectangle
   * @param width: The width of the rectangle
   * @param height: The height of the rectangle
   * @param color: The 565 color value to fill the rectangle with
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t fill(const uint32_t x, const uint32_t y, const uint32_t width, const uint32_t height, const uint16_t color, uint16_t mutex_timeout);

  /**
   * @brief Set the text on the given component
   * @param component_id: The component to change
   * @param text: What text is to be displayed
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_text(const char *component_id, const char *text, uint16_t mutex_timeout);

  /**
   * @brief Set the integer value of the given component
   * @param component_id: The component to change
   * @param value: The new value for the component
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_value(const char *component_id, int16_t value, uint16_t mutex_timeout);

  /**
   * @brief Set the timeout value of the given timer, ie. how long should the timer run before triggering
   * @param timer_id: The timer to change
   * @param value: The new timeout for the timer
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_timer_value(const char *timer_id, uint16_t value, uint16_t mutex_timeout);

  /**
   * @brief Set the picture value of the given component (ie. the "pic" field in Nextion editor)
   * @param component_id: The component to change
   * @param value: The new value for the component
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_pic(const char *component_id, uint8_t value, uint16_t mutex_timeout);

  /**
   * @brief Set the second picture value of the given component (ie. the "pic1" field in Nextion editor)
   * @param component_id: The component to change
   * @param value: The new value for the component
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_pic1(const char *component_id, uint8_t value, uint16_t mutex_timeout);

  /**
   * @brief Set the "pressed" picture value of the given component (ie. the "pic2" field in Nextion editor)
   * @param component_id: The component to change
   * @param value: The new value for the component
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_pic2(const char *component_id, uint8_t value, uint16_t mutex_timeout);

  /**
   * @brief Set the component foreground color
   * @param component_id: The component to change
   * @param value: The new foreground color
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_foreground(const char *component_id, uint16_t value, uint16_t mutex_timeout);

  /**
   * @brief Set the component pco2 color
   * @param component_id: The component to change
   * @param value: The new pco2 color
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_pco2(const char *component_id, uint16_t value, uint16_t mutex_timeout);

  /**
   * @brief Set a component visibility
   * @param component_id: The component to show or hide
   * @param visible: "true" for shown, "false" for hidden
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_visibility(const char *component_id, bool visible, uint16_t mutex_timeout);

  /**
   * Request the current value of a component on the Nextion display
   * @param component_id: The ID of the component to request a value from
   * @param result: Pointer to where to save the value
   * @param timeout: The amount of time to wait for an answer in ms
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return: Return ESP_ERR_TIMEOUT if no value was returned within the given timeout
   */
  static esp_err_t get_component_integer_value(const char *component_id, int32_t *result, uint16_t timeout, uint16_t mutex_timeout);

  /**
   * Request the current value of a component on the Nextion display
   * @param brightness: The new brightness for the screen
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return: Return ESP_ERR_TIMEOUT if no value was returned within the given timeout
   */
  static esp_err_t set_brightness_level(uint8_t brightness, uint16_t mutex_timeout);

  /**
   * Get the current Nextion display state
   */
  static nextion_state_t get_current_state();

  /**
   * Restart the Nextion display via GPIO pins (power off and power on. Not a soft reboot via commands!)
   */
  static void restart();

  /**
   * Restart the Nextion display and start the update process.
   * @param comms_baudrate: What serial baud rate to use when sending command to display to start update.
   * @param upload_baudrate: What serial baud rate to use when sending upload data to the screen.
   * @param use_new_upload_protocol: False: use upload protocol "v1.0". True: use upload protocol "v1.2".
   * @param upload_file_size: Size of the new TFT file to be uploaded to the Nextion display.
   * @return ESP_OK on success or ESP_ERR_NOT_FINISHED if failed to init update.
   */
  static esp_err_t start_update(uint32_t upload_baudrate, bool use_new_upload_protocol, uint64_t upload_file_size);

  /**
   * Write update data to the Nextion display.
   * @param data: Pointer to buffer from where to read upload data from.
   * @param size: The number of bytes to read from buffer.
   * @return ESP_OK if successful or ESP_ERR_NOT_FINISHED if failed to write bytes to uart.
   */
  static esp_err_t write_update_bytes(uint8_t *data, uint16_t size);

private:
  // Handle all events from UART
  static void _task_uart_event(void *param);

  // Post each complete frame in pending with _post_received_data, and leave only a trailing partial frame in it
  static void _post_received_frames(std::vector<char> &pending);
  // Post data read from UART to _uart_data_handler on the UART data event loop
  static void _post_received_data(const std::vector<char> &data);

  // Process UART events
  static void _uart_data_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   *  @brief Wait for a specific event to occur
   * @param event_base: The event base to listen for events from
   * @param event_id: The specific ID within the event base to wait for.
   * @param timeout: The amount of time to wait for event to happen, timeout in ms.
   * @param mutex_timeout: The amount of time to wait for mutex to be taken, timeout in ms.
   * @return ESP_OK on success, ESP_ERR_TIMEOUT if event did not occur within specified timeout or failed to take mutex.
   */
  static esp_err_t _wait_for_event(esp_event_base_t event_base, int32_t event_id, uint16_t timeout, uint16_t mutex_timeout);

  /**
   *  @brief When waiting for events, this is the function that will handle and trigger the event is done.
   */
  static void _wait_for_event_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * @brief Will attempt to write data onto serial.
   * @return ESP_OK on success, otherwise return ESP_ERR_NOT_FINISHED.
   */
  static esp_err_t _write_command(const char *data);

  /**
   * @brief Send one command, if the display is running. Every public command goes through here.
   * @param command: The command, without its 0xFF 0xFF 0xFF terminator.
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t _send_command(const std::string &command, uint16_t mutex_timeout);

  /**
   * @brief Write one command and its terminator. The caller holds _uart_write_mutex.
   */
  static void _write_locked(const std::string &command);

private:
  // VARIABLES:
  // Event loop handle for custom events that happens from UART read -> handle mqtt message
  static inline esp_event_loop_handle_t _handle_uart_data_event_loop;

  // Loop task arguments for custom event loop for UARt data
  static inline esp_event_loop_args_t _handle_uart_data_event_loop_args;

  // Mutex to only allow one task at the time to write to UART
  static inline SemaphoreHandle_t _uart_write_mutex;

  // Mutex to only allow one task at the time to wait for events
  static inline SemaphoreHandle_t _event_wait_mutex;

  // The Task that is waiting for an event to happen
  static inline TaskHandle_t _event_wait_task;

  // Mutex to only allow one task at the time to access the variables for "event_wait_integer"
  static inline SemaphoreHandle_t _event_wait_integer_mutex;

  // The task that requested an integer value from the Nextion display and is currently waiting
  static inline TaskHandle_t _event_wait_integer;

  // The value that was received from the display
  static inline int32_t *_event_wait_integer_result;

  // Configuration used to initialize uart
  static inline uart_config_t _uart_config;

  // Event queue for uart events
  static inline QueueHandle_t _uart_event_queue;

  // Mutex to only allow one task at the time to read/write the _current_nextion_state variable
  static inline SemaphoreHandle_t _nextion_state_mutex;

  // The current state of the display according to the ESP32 firmware
  static inline nextion_state_t _current_nextion_state;

  // When starting the Nextion display, did we receive the "NSPM" flag indicating that our NSPanel Manager firmware is running on the Nextion display?
  static inline bool _has_received_nspm_flag;
};
