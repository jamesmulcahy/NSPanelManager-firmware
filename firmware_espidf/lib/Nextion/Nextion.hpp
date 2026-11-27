#pragma once
#include <atomic>
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
  UPDATING,
  // NextionTerminal owns the UART: raw bytes in both directions, no commands from anyone else.
  TERMINAL
};

static constexpr uint16_t NEXTION_UART_EVENT_LOOP_STACK_SIZE = 3096;
static constexpr uint16_t NEXTION_UART_EVENT_LOOP_QUEUE_SIZE = 6;

class Nextion {
  friend class NextionWindow;

public:
  /**
   * Initialize the Nextion display and gather any facts about it.
   *
   * NSPM's TFT announces itself with an "NSPM" banner once, when it starts, and nothing says it again:
   * a banner that is missed would leave the panel treating NSPM's TFT as an unknown display. So when a
   * display sent no banner and `recognised` (given the `connect` reply, "" if there was none) does not
   * know it as the TFT the firmware drives, init() power-cycles it, which makes NSPM's TFT print the
   * banner again, and listens again, up to BANNER_RETRIES more times. A display that is recognised
   * (the terminal TFT, which prints no banner) is never power-cycled: one that answers `connect` is recognised at once (one whose comok is lost is retried, which is right). Without `recognised` there are no retries.
   */
  static esp_err_t init(bool (*recognised)(const char *identity) = nullptr);

  /**
   * Whether the board has a Nextion display. The custom PCB (Waveshare ESP32-P4-WIFI6-Touch-LCD-4B) has
   * a MIPI-DSI LCD instead, and its GPIO 14-19 are the ESP32-C6's SDIO bus. Without one, init() only
   * creates the mutexes and claims no UART or GPIO, the state stays INITIALIZING (so no terminal is ever
   * started), commands are dropped and a TFT update is refused (ESP_ERR_NOT_SUPPORTED).
   */
  static constexpr bool present() {
#if defined(BOARD_CUSTOM)
    return false;
#else
    return true;
#endif
  }

  /**
   * @brief Go to the specified page
   * @param page: The name of the Nextion page to go to.
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t go_to_page(const char *page, uint16_t mutex_timeout);

  /**
   * When set, set_brightness_level() hands the level to `hook` instead of sending dim= (LvglUi:
   * the terminal receiver sets the backlight while it owns the display; a no-op where nothing
   * may be sent to the display at all).
   */
  static void set_brightness_redirect(void (*hook)(uint8_t brightness)) { Nextion::_brightness_redirect = hook; }

  /**
   * The display's last `connect` reply ("comok 2,30614-0,NX4832F035_011C,94,61744,..."), empty
   * if it never answered.
   */
  static std::string identity();

  /**
   * Whether the TFT sent NSPanel Manager's "NSPM" flag since boot. A latch: once seen it stays set.
   */
  static bool nspm_flag_received() { return Nextion::_has_received_nspm_flag; }

  /**
   * @brief Set the text on the given component (the legacy TFT's boot screen is all this is used for)
   * @param component_id: The component to change
   * @param text: What text is to be displayed
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_component_text(const char *component_id, const char *text, uint16_t mutex_timeout);

  /**
   * Set the display's brightness (backlight level).
   * @param brightness: The new brightness for the screen
   * @param mutex_timeout: Timeout (in ms) of how long to wait for UART mutex
   * @return ESP_OK or ESP_ERR_TIMEOUT
   */
  static esp_err_t set_brightness_level(uint8_t brightness, uint16_t mutex_timeout);

  /**
   * Get the current Nextion display state
   */
  static nextion_state_t get_current_state();

  /**
   * The baud rate the display and the UART are running at right now: the operating baud rate if the
   * switch to it succeeded, otherwise the communication baud rate. Not the configured operating rate.
   */
  static uint32_t get_link_baud();

  /**
   * Log the link baud rate and how it was reached. Nextion::init() runs before panel logs reach MQTT,
   * so its own lines are usually lost; call this once they do.
   */
  static void log_link_baud();

  /**
   * Text for a status report warning when the display is not at the operating baud rate it was set to,
   * because it did not answer there or the rate is not one it supports. Empty when there is nothing to report.
   */
  static std::string get_link_baud_warning();

  /**
   * Power-cycle the display and connect to it again, for when it stopped answering while the link
   * ran above the communication baud rate. If the display reset by itself it came back at its default
   * rate, and nothing it sends can be read at the operating rate. Switches to the operating baud rate
   * again the first time only; after that the link stays at the communication baud rate until the
   * panel reboots. Writers are turned away while this runs (a few seconds).
   * Blocks: call from a task of its own, never from an event handler. Takes _uart_write_mutex.
   */
  static void recover_link();

  /**
   * Restart the Nextion display via GPIO pins (power off and power on. Not a soft reboot via commands!)
   * The display comes back at its stored default baud rate, so the UART is set back to
   * ConfigManager::communication_baud_rate as well.
   */
  static void restart();

  /**
   * Hand the UART to the calling task alone, for NextionTerminal. Waits for the writer in flight,
   * then turns every other writer away (state TERMINAL) and stops reading frames: the caller reads
   * and writes raw bytes with terminal_read()/terminal_write() until terminal_release().
   * Only while the display is RUNNING.
   * @return ESP_OK, or ESP_ERR_INVALID_STATE / ESP_ERR_TIMEOUT.
   */
  static esp_err_t terminal_acquire(uint16_t mutex_timeout);

  /**
   * Raw UART access while acquired. terminal_read() returns as soon as `size` bytes arrived, or
   * after `timeout` ticks with what there is.
   */
  static int terminal_write(const uint8_t *data, size_t size);
  static int terminal_read(uint8_t *data, size_t size, TickType_t timeout);
  static void terminal_drain();
  // Wait for everything written to leave, then move the UART (not the display) to `baud`.
  static esp_err_t terminal_set_uart_baud(uint32_t baud);

  /**
   * Give the UART back, from the task that acquired it. With `restart_display`, whatever state the
   * display was left in, it is power-cycled and connected again exactly as at boot (operating baud
   * rate included); that blocks for a few seconds. Without, the caller vouches that the display
   * is still at the link baud rate in its normal command mode (nothing but queries were sent).
   */
  static void terminal_release(bool restart_display);

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

  // While updating: split what the display sent into replies and post each one. Partial replies are
  // kept for the next read.
  static void _split_update_replies(const std::vector<char> &data);

  // Bytes received while updating that do not yet make a whole reply. UART task only, except for
  // the clear in start_update(), which holds _nextion_state_mutex like the UART task does here.
  static inline std::vector<char> _update_reply_buffer;

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
   * @brief Wait for the display to boot after a restart() and connect to it at its default baud rate.
   */
  static void _connect_at_default_baud(uint16_t banner_wait_ms = 2500);

  /**
   * @brief Move the display and the UART to ConfigManager::operating_baud_rate, if one is set.
   * Uses `baud=`, which lasts only until the display is next power-cycled, never `bauds=`, which
   * would change the display's stored default. If the display does not answer at the new rate,
   * it is power-cycled back to its default and the UART follows.
   * @return ESP_OK if running at the operating baud rate or none is set, otherwise ESP_FAIL.
   */
  static esp_err_t _switch_to_operating_baud();

  /**
   * @brief Drop anything received so far, including queued pattern positions that would no longer
   * match the bytes in the buffer.
   */
  static void _flush_uart_input();

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
  // Runs _handle_uart_data_event_loop (created without a task, so this one can have its stack in PSRAM).
  static void _task_uart_frame_loop(void *param);

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

  // The NEXTION_EVENT id _event_wait_task is waiting for
  static inline int32_t _event_wait_id;

  // Set when the event _event_wait_task waits for has arrived (under _event_wait_mutex)
  static inline std::atomic<bool> _event_wait_seen = false;

  // Waits for this task's notification until `seen` is set or timeout_ms passes. The waits above
  // share the task's one notification slot with whatever else the task waits for (the terminal
  // task's draw() wake-ups, say), so a notification alone proves nothing: one left pending would
  // end the wait at once. Notifications that were not ours are given back. True if `seen`.
  static bool _wait_for_flag(const std::atomic<bool> &seen, uint32_t timeout_ms);

  // Configuration used to initialize uart
  static inline uart_config_t _uart_config;

  // Event queue for uart events
  static inline QueueHandle_t _uart_event_queue;

  // Mutex to only allow one task at the time to read/write the _current_nextion_state variable
  static inline SemaphoreHandle_t _nextion_state_mutex;

  // The current state of the display according to the ESP32 firmware
  static inline nextion_state_t _current_nextion_state;

  // When starting the Nextion display, did we receive the "NSPM" flag indicating that our NSPanel Manager firmware is running on the Nextion display?
  static inline std::atomic<bool> _has_received_nspm_flag = false;
  // The last comok, for identity(); guarded by _identity_lock.
  static inline char _identity[96] = {};
  static inline portMUX_TYPE _identity_lock = portMUX_INITIALIZER_UNLOCKED;

  // How the link reached _link_baud.
  enum class link_baud_result_t : uint8_t {
    NOT_REQUESTED, // No operating baud rate set.
    SWITCHED,      // Running at the operating baud rate.
    FELL_BACK,     // The display did not answer at the operating baud rate; back at the default.
    UNSUPPORTED,   // The operating baud rate is not one the display accepts.
  };

  static inline std::atomic<uint32_t> _link_baud = 0;
  static inline std::atomic<link_baud_result_t> _link_baud_result = link_baud_result_t::NOT_REQUESTED;
  static inline void (*_brightness_redirect)(uint8_t brightness) = nullptr;
};
