#include <ConfigManager.hpp>
#include <Nextion.hpp>
#include <NextionWindow.hpp>
#include <Nextion_constans.hpp>
#include <Nextion_event.hpp>
#include <esp_event.h>
#include <esp_log.h>
#include <memory.h>
#include <memory>
#include <string>
#include <vector>

ESP_EVENT_DEFINE_BASE(NEXTION_EVENT);

esp_err_t Nextion::init() {
  esp_log_level_set("Nextion", ConfigManager::log_level);
  esp_log_level_set("uart", esp_log_level_t::ESP_LOG_DEBUG);
  ESP_LOGI("Nextion", "Initializing Nextion display.");

  // Setup initial values and create mutexes:
  Nextion::_nextion_state_mutex = xSemaphoreCreateMutex();
  Nextion::_uart_write_mutex = xSemaphoreCreateMutex();
  Nextion::_event_wait_mutex = xSemaphoreCreateMutex();
  Nextion::_event_wait_integer_mutex = xSemaphoreCreateMutex();

  Nextion::_current_nextion_state = nextion_state_t::INITIALIZING;

  // Start custom event loop that handles messages from UART -> data handler
  Nextion::_handle_uart_data_event_loop_args = {
      .queue_size = NEXTION_UART_EVENT_LOOP_QUEUE_SIZE,
      .task_name = "uart_handle_data",
      .task_priority = 8,
      .task_stack_size = NEXTION_UART_EVENT_LOOP_STACK_SIZE,
      .task_core_id = 1,
  };
  esp_err_t create_result = esp_event_loop_create(&Nextion::_handle_uart_data_event_loop_args, &Nextion::_handle_uart_data_event_loop);
  if (create_result != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to create custom event loop to handle uart data. Error: %s", esp_err_to_name(create_result));
    return ESP_ERR_NOT_FINISHED;
  }

  // Configure UART
  Nextion::_uart_config = {};
  Nextion::_uart_config.baud_rate = (int)ConfigManager::communication_baud_rate;
  Nextion::_uart_config.data_bits = UART_DATA_8_BITS;
  Nextion::_uart_config.parity = UART_PARITY_DISABLE;
  Nextion::_uart_config.stop_bits = UART_STOP_BITS_1;
  Nextion::_uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  Nextion::_uart_config.source_clk = UART_SCLK_DEFAULT;

  esp_err_t cmd_err = uart_param_config(UART_NUM_2, &Nextion::_uart_config);
  if (cmd_err != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to set UART parameters! Error: %s", esp_err_to_name(cmd_err));
  }

  cmd_err = uart_driver_install(UART_NUM_2, NEXTION_UART_BUFFER_SIZE * 2, 6000, 8, &Nextion::_uart_event_queue, 0); // Setup UART driver
  if (cmd_err != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to install UART driver! Error: %s", esp_err_to_name(cmd_err));
  }

  // Set RX pin to input mode
  gpio_config_t io_conf = {};
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = 1ULL << GPIO_NUM_17;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cmd_err = gpio_config(&io_conf);
  if (cmd_err != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to clear existing IO config for UART RX pin! Error: %s", esp_err_to_name(cmd_err));
  }

#if defined(BOARD_SONOFF)
  cmd_err = uart_set_pin(UART_NUM_2, GPIO_NUM_16, GPIO_NUM_17, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#elif defined(BOARD_CUSTOM)
  cmd_err = uart_set_pin(UART_NUM_2, GPIO_NUM_3, GPIO_NUM_46, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#endif
  if (cmd_err != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to set UART pins! Error: %s", esp_err_to_name(cmd_err));
  }

  cmd_err = uart_enable_pattern_det_baud_intr(UART_NUM_2, 0xFF, 3, 9, 0, 0); // Setup pattern detection to trigger interrupt when 3 consecutive 0xFF has been received.
  if (cmd_err != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to enable pattern detection on UART! Error: %s", esp_err_to_name(cmd_err));
  }

  cmd_err = uart_pattern_queue_reset(UART_NUM_2, 8);
  if (cmd_err != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to reset UART pattern queue! Error: %s", esp_err_to_name(cmd_err));
  }

  // Register event handler for when uart data is fully read and processed into an Nextion event
  esp_err_t reg_handler_result = esp_event_handler_register_with(Nextion::_handle_uart_data_event_loop, NEXTION_EVENT, nextion_event_t::RECEIVED_DATA, &Nextion::_uart_data_handler, NULL);
  if (reg_handler_result != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to register Nextion UART data processing handler. Got result %s", esp_err_to_name(reg_handler_result));
  }

  // Start the task that is responsible for handling Nextion display data
  xTaskCreatePinnedToCore(Nextion::_task_uart_event, "uart_event_task", 4096, NULL, 12, NULL, 1);

  ESP_LOGD("Nextion", "Turning Nextion display off and on again.");
  gpio_set_direction(NEXTION_ON_OFF_GPIO, GPIO_MODE_OUTPUT);
  Nextion::restart();

  // Wait for 'NSPM' flag, if we receive it.
  if (Nextion::_wait_for_event(NEXTION_EVENT, nextion_event_t::RECEIVED_NSPM_FLAG, 2500, 250) == ESP_OK) {
    ESP_LOGI("Nextion", "Received 'NSPM'-flag. You are running the NSPanel Manager GUI/TFT.");
  } else {
    ESP_LOGW("Nextion", "Did not receive the 'NSPM'-flag on screen startup. Are you running the NSPanel Manager GUI/TFT?");
    Nextion::_has_received_nspm_flag = false;
  }

  ESP_LOGI("Nextion", "Trying to connect to Nextion display.");
  Nextion::_write_command("DRAKJHSUYDGBNCJHGJKSHBDN"); // Send a bunch of jibberish to "reset" the Nextion display from any state it is in currently.
  vTaskDelay(pdMS_TO_TICKS(250));                      // Wait for panel to process
  uart_flush(UART_NUM_2);                              // Clear UART buffers

  ESP_LOGD("Nextion", "Sending connect command to panel.");
  if (Nextion::_write_command("connect") != ESP_OK) {
    ESP_LOGE("Nextion", "Failed to send connect command to Nextion display.");
  }

  if (Nextion::_wait_for_event(NEXTION_EVENT, nextion_event_t::CONNECTED, 500, 32) == ESP_OK) {
    ESP_LOGI("Nextion", "Connected to Nextion display.");
  } else {
    ESP_LOGW("Nextion", "Failed to receive 'comok' from Nextion display, will continue anyway.");
  }

  // Set initial values in Nextion display:
  Nextion::_write_command("sleep=0");
  Nextion::_write_command("sleep=0");

  Nextion::_current_nextion_state = nextion_state_t::RUNNING;

  // From here on the display answers every command, and writers share its input buffer through the window.
  NextionWindow::init();
  if (xSemaphoreTake(Nextion::_uart_write_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    NextionWindow::start_locked();
    xSemaphoreGive(Nextion::_uart_write_mutex);
  } else {
    ESP_LOGE("Nextion", "Failed to take _uart_write_mutex to start the write window. Writes to the display will not be paced.");
  }
  ESP_LOGI("Nextion", "Nextion init complete.");

  return ESP_OK;
}

void Nextion::_task_uart_event(void *param) {
  ESP_LOGI("Nextion", "Started Nextion uart event handler.");

  uart_event_t event;
  std::vector<char> uart_buffer;
  bool read_nextion_jump_instruction = false;
  std::vector<uint8_t> uart_pattern_buffer(3);
  // Bytes read but not yet part of a complete frame, kept for the next read.
  std::vector<char> pending_frames;
  // std::vector<uint8_t> uart_read_buffer(NEXTION_UART_BUFFER_SIZE);
  for (;;) {
    // Wait for UART data, while waiting pause task
    if (xQueueReceive(Nextion::_uart_event_queue, (void *)&event, portMAX_DELAY)) {
      switch (event.type) {
      case UART_BREAK:
        break; // Ignore UART BREAK

      case UART_DATA: {
        if (event.size <= NEXTION_UART_BUFFER_SIZE) {
          if (xSemaphoreTake(Nextion::_nextion_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (Nextion::_current_nextion_state == nextion_state_t::UPDATING) {
              uart_buffer.resize(event.size);
              uart_read_bytes(UART_NUM_2, uart_buffer.data(), event.size, pdMS_TO_TICKS(64));

              // Special handling for 0x08 jump instruction from Nextion display.
              if (uart_buffer[0] == 0x08) {
                read_nextion_jump_instruction = true;
                xSemaphoreGive(Nextion::_nextion_state_mutex); // Give back mutex as we do a continue here and will not reach the xSemaphoreGive at the bottom.
                continue;                                      // Wait for next event after 0x08 to received the 4 bytes of offset data.
              } else if (read_nextion_jump_instruction) {      // Least event read was a 0x08 jump instruction. Read this data as jump dest and reset.
                if (event.size >= 4) {
                  // Copy all data 1 byte forward in memory and then set data[0] = 0x08
                  uart_buffer.resize(event.size + 1);
                  for (int i = event.size; i > 0; i--) {
                    uart_buffer[i] = uart_buffer[i - 1];
                  }
                  uart_buffer[0] = 0x08;
                }
                read_nextion_jump_instruction = false; // Reset special handling
              }

              Nextion::_post_received_data(uart_buffer);
            }
            xSemaphoreGive(Nextion::_nextion_state_mutex);
          } else {
            ESP_LOGE("Nextion", "Failed to take _nextion_state_mutex while checking current state!");
          }
        }
        break;
      }

      case UART_FIFO_OVF:
        ESP_LOGW("Nextion", "UART buffer overflow, will reset buffers. Any data in buffers or event queue will be lost!");
        uart_flush(UART_NUM_2);
        xQueueReset(Nextion::_uart_event_queue);
        break;

      case UART_BUFFER_FULL:
        ESP_LOGW("Nextion", "UART buffer full! Will reset buffers. Any data in buffers or event queue will be lost.");
        uart_flush(UART_NUM_2);
        xQueueReset(Nextion::_uart_event_queue);
        break;

      case UART_PATTERN_DET: {
        bool updating = false;
        if (xSemaphoreTake(Nextion::_nextion_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          updating = Nextion::_current_nextion_state == nextion_state_t::INITIALIZING_UPDATE || Nextion::_current_nextion_state == nextion_state_t::UPDATING;
          xSemaphoreGive(Nextion::_nextion_state_mutex);
        }

        if (!updating) [[likely]] {
          // Don't trust the pattern positions. The driver keeps only 8, and when the display sends frames faster
          // than this task pops them, it drops positions without saying so: the frames then arrive joined, and
          // once the events outnumber the positions, popping one fails. Read everything buffered instead and
          // split it into frames here, keeping a partial frame for the next read. Reading also discards the
          // positions of everything read, so the driver's queue drains.
          size_t buffered = 0;
          if (uart_get_buffered_data_len(UART_NUM_2, &buffered) == ESP_OK) [[likely]] {
            if (buffered > 0) {
              size_t kept = pending_frames.size();
              pending_frames.resize(kept + buffered);
              int read = uart_read_bytes(UART_NUM_2, pending_frames.data() + kept, buffered, pdMS_TO_TICKS(64));
              pending_frames.resize(kept + (read > 0 ? read : 0));
            }
            Nextion::_post_received_frames(pending_frames);
          } else [[unlikely]] {
            ESP_LOGE("Nextion", "Failed to get UART data buffered size!");
          }
          break;
        }

        // A GUI update reads its replies on UART_DATA, so take only the bytes up to this pattern.
        size_t uart_buffered_data_size;
        if (uart_get_buffered_data_len(UART_NUM_2, &uart_buffered_data_size) == ESP_OK) {
          int pattern_position = uart_pattern_pop_pos(UART_NUM_2);
          if (pattern_position == -1) [[unlikely]] {
            ESP_LOGE("Nextion", "No pattern position recorded for a pattern event. Will clear buffers and queue, all data will be lost!");
            uart_flush(UART_NUM_2);
          } else {
            uart_buffer.resize(pattern_position);
            uart_read_bytes(UART_NUM_2, uart_buffer.data(), pattern_position, pdMS_TO_TICKS(64));
            uart_read_bytes(UART_NUM_2, uart_pattern_buffer.data(), 3, pdMS_TO_TICKS(64)); // Read last three 0XFF as well to clear buffer.

            Nextion::_post_received_data(uart_buffer);
          }
        } else [[unlikely]] {
          ESP_LOGE("Nextion", "Failed to get UART data buffered size!");
        }
        break;
      }

      default:
        ESP_LOGW("Nextion", "Unknown UART event type %d!", event.type);
        break;
      }
    }
  }
}

void Nextion::_post_received_frames(std::vector<char> &pending) {
  // Every message from the display ends in FF FF FF, except that a number reply carries 4 raw bytes, which may
  // themselves be 0xFF, before its terminator.
  size_t start = 0;
  for (size_t i = 0; i + 2 < pending.size(); i++) {
    if ((uint8_t)pending[start] == NEX_RET_NUMBER_HEAD && i < start + 5) {
      continue;
    }
    if ((uint8_t)pending[i] == 0xFF && (uint8_t)pending[i + 1] == 0xFF && (uint8_t)pending[i + 2] == 0xFF) {
      if (!NextionWindow::on_frame((const uint8_t *)pending.data() + start, i - start)) {
        Nextion::_post_received_data(std::vector<char>(pending.begin() + start, pending.begin() + i));
      }
      i += 2;
      start = i + 1;
    }
  }
  pending.erase(pending.begin(), pending.begin() + start);

  if (pending.size() > NEXTION_UART_BUFFER_SIZE) [[unlikely]] {
    // No terminator in all that: not a frame, or not one that can be recovered.
    ESP_LOGW("Nextion", "Dropping %zu bytes from the display that never ended a frame.", pending.size());
    pending.clear();
  }
}

void Nextion::_post_received_data(const std::vector<char> &data) {
  if (data.size() > NEXTION_UART_EVENT_LOOP_STACK_SIZE - 512) { // Subtract 512 to allow for internal variables in the event loop
    return;
  }

  // esp_event copies the posted bytes, so post the data itself rather than the vector, which would copy
  // only its pointer to a buffer the UART task reuses for the next message, possibly before the handler
  // has read this one. Handlers are not told the size, so it goes first.
  uint16_t size = data.size();
  std::vector<uint8_t> message(sizeof(size) + size);
  memcpy(message.data(), &size, sizeof(size));
  memcpy(message.data() + sizeof(size), data.data(), size);

  switch (esp_event_post_to(Nextion::_handle_uart_data_event_loop, NEXTION_EVENT, nextion_event_t::RECEIVED_DATA, message.data(), message.size(), pdMS_TO_TICKS(16))) {
  case ESP_OK:
    [[likely]];
    break;

  case ESP_ERR_TIMEOUT:
    ESP_LOGE("Nextion", "Failed to post new event data from UART event!");
    break;

  case ESP_ERR_INVALID_ARG:
    ESP_LOGE("Nextion", "Invalid combination of event base and data when posting event from UART data!");
    break;

  default:
    ESP_LOGE("Nextion", "Unknown error when posting event data from UART event!");
  }
}

void Nextion::_uart_data_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // _post_received_data posts the size, then the data.
  uint16_t size;
  memcpy(&size, event_data, sizeof(size));
  const char *received_data = (const char *)event_data + sizeof(size);
  std::vector<char> received(received_data, received_data + size);
  std::vector<char> *data = &received;
  if (data->size() <= 0) [[unlikely]] {
    return;
  }

  // ESP_LOGD("Nextion", "Read Nextion data: %.*s, size: %d", data->size(), data->data(), data->size());

  if (data->data()[0] == NEX_RET_NUMBER_HEAD) {
    // Got numeric data
    if (data->size() >= 5) [[likely]] {
      if (xSemaphoreTake(Nextion::_event_wait_integer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (Nextion::_event_wait_integer_result != NULL) {
          *Nextion::_event_wait_integer_result = data->data()[1];
          *Nextion::_event_wait_integer_result += data->data()[2] << 8;
          *Nextion::_event_wait_integer_result += data->data()[3] << 16;
          *Nextion::_event_wait_integer_result += data->data()[4] << 24;
        } else {
          ESP_LOGE("Nextion", "Got integer data but _event_wait_integer_result is NULL. No place to store result!");
        }

        if (Nextion::_event_wait_integer != NULL) {
          xTaskNotifyGive(Nextion::_event_wait_integer);
        } else {
          ESP_LOGE("Nextion", "Got integer data but _event_wait_integer is NULL. No task to notify about result!");
        }
        xSemaphoreGive(Nextion::_event_wait_integer_mutex);
      } else {
        ESP_LOGE("Nextion", "Failed to get _event_wait_integer_mutex while processing integer response.");
        TaskHandle_t holding_task = xSemaphoreGetMutexHolder(Nextion::_event_wait_integer_mutex);
        if (holding_task != NULL) {
          char *task_name = pcTaskGetName(holding_task);
          ESP_LOGE("Nextion", "Mutex held by: %s", task_name);
        }
      }
    } else {
      ESP_LOGE("Nextion", "Not enough bytes sent for 0x71 integer response.");
    }
  } else if (data->data()[0] == NEX_RET_EVENT_TOUCH_HEAD) {
    if (data->size() >= 4) [[likely]] {
      nextion_event_touch_t touch_event;
      touch_event.page_number = data->data()[1];
      touch_event.component_id = data->data()[2];
      touch_event.pressed = data->data()[3];
      // A touch that cannot be posted is lost for good, so say so rather than leave a press that did nothing.
      esp_err_t post_result = esp_event_post(NEXTION_EVENT, nextion_event_t::TOUCH_EVENT, &touch_event, sizeof(touch_event), pdMS_TO_TICKS(16));
      if (post_result != ESP_OK) [[unlikely]] {
        ESP_LOGW("Nextion", "Dropped touch event for page %d component %d: %s", touch_event.page_number, touch_event.component_id, esp_err_to_name(post_result));
      }
    } else {
      ESP_LOGE("Nextion", "Not enough bytes sent for 0x65 touch event.");
    }
  } else if (data->data()[0] == NEX_OUT_STRING_DATA) {
    data->erase(data->begin());                // Remove first byte that represents the even type:
    if (data->empty() || data->back() != '\n') [[likely]] { // Add termination char at end of string for proper processing when receiving the event.
      data->push_back('\0');
    }
    esp_event_post(NEXTION_EVENT, nextion_event_t::STRING_EVENT, data->data(), data->size(), pdMS_TO_TICKS(16)); // TODO: Is there a better way than simply sending raw bytes as te length cannot be access when receiving the event
  } else if (data->data()[0] == NEX_OUT_SLEEP) {
    esp_event_post(NEXTION_EVENT, nextion_event_t::SLEEP_EVENT, NULL, 0, pdMS_TO_TICKS(5000));
  } else if (data->data()[0] == NEX_RET_CMD_FINISHED) {
    // A reply to a command. NextionWindow consumes these while it is running; nothing else waits for one.
  } else if (data->data()[0] == NEX_OUT_WAKE) {
    esp_event_post(NEXTION_EVENT, nextion_event_t::WAKE_EVENT, NULL, 0, pdMS_TO_TICKS(5000));
  } else if (data->size() >= strlen("NSPM") && strncmp(data->data() + data->size() - strlen("NSPM"), "NSPM", strlen("NSPM")) == 0) { // TODO: Compare last bytes of message instead of first as there may be garbage data output from the panel before sending NSPM-flag
    esp_event_post(NEXTION_EVENT, nextion_event_t::RECEIVED_NSPM_FLAG, NULL, 0, pdMS_TO_TICKS(16));
  } else if (data->size() >= 5 && strncmp(data->data(), "comok", strlen("comok")) == 0) {
    ESP_LOGD("Nextion", "Connected to Nextion display, comok data: %s", data->data());
    esp_event_post(NEXTION_EVENT, nextion_event_t::CONNECTED, NULL, 0, pdMS_TO_TICKS(16));
  } else if (data->data()[0] == NEX_RET_INVALID_PICTURE_ID) {
    ESP_LOGW("Nextion", "NEX_RET_INVALID_PICTURE_ID (0x04): a command or HMI component references a picture ID that does not exist in the resource list");
  } else if (data->data()[0] == NEX_RET_UPDATE_READY_FOR_NEXT_CHUNK) {
    esp_event_post(NEXTION_EVENT, nextion_event_t::UPDATE_READY_FOR_NEXT_CHUNK, NULL, 0, pdMS_TO_TICKS(1000));
  } else if (data->data()[0] == NEX_RET_UPDATE_JUMP_TO_OFFSET) {
    if (data->size() >= 5) [[likely]] {
      size_t byte_offset;
      byte_offset = data->data()[1];
      byte_offset += data->data()[2] << 8;
      byte_offset += data->data()[3] << 16;
      byte_offset += data->data()[4] << 24;

      esp_event_post(NEXTION_EVENT, nextion_event_t::UPDATE_JUMP_TO_OFFSET, &byte_offset, sizeof(byte_offset), pdMS_TO_TICKS(1000));
    } else {
      ESP_LOGE("Nextion", "Not enough bytes sent for NEX_RET_UPDATE_JUMP_TO_OFFSET (0x08)! Only read %zu bytes.", data->size());
    }
  } else if (data->data()[0] == NEX_OUT_BUFFER_OVERFLOW) {
    ESP_LOGW("Nextion", "NEX_OUT_BUFFER_OVERFLOW (0x24): the display's serial buffer overflowed and the commands waiting in it were lost.");
  } else {
    ESP_LOGW("Nextion", "Unknown event data from Nextion: %.*s", data->size(), data->data());
  }
}

esp_err_t Nextion::_wait_for_event(esp_event_base_t event_base, int32_t event_id, uint16_t timeout, uint16_t mutex_timeout) {
  // Wait for mutex that allows us to manipulate the _event_wait_task task handle
  if (xSemaphoreTake(Nextion::_event_wait_mutex, pdMS_TO_TICKS(mutex_timeout)) == pdTRUE) {
    // Set the waiting task handle to the currently calling task
    if (Nextion::_event_wait_task == NULL) {
      Nextion::_event_wait_task = xTaskGetCurrentTaskHandle();
      esp_event_handler_register(event_base, event_id, Nextion::_wait_for_event_event_handler, NULL);
    }
    xSemaphoreGive(Nextion::_event_wait_mutex);
  } else {
    return ESP_ERR_TIMEOUT;
  }

  // Wait for the notification that the event happened
  BaseType_t result = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout));

  // Reset the waiting task handle to NULL
  if (xSemaphoreTake(Nextion::_event_wait_mutex, portMAX_DELAY) == pdTRUE) {
    Nextion::_event_wait_task = NULL;
    xSemaphoreGive(Nextion::_event_wait_mutex);
  } else {
    return ESP_ERR_TIMEOUT;
  }

  // Unregister event handler for waiting task
  esp_event_handler_unregister(event_base, event_id, Nextion::_wait_for_event_event_handler);

  if (result == pdTRUE) {
    return ESP_OK;
  } else {
    return ESP_ERR_TIMEOUT;
  }
}

void Nextion::_wait_for_event_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // Handler is set to only trigger on the correct EVENT_BASE and EVENT_ID, simply send a notification to task that event happened
  if (xSemaphoreTake(Nextion::_event_wait_mutex, pdMS_TO_TICKS(32)) == pdTRUE) {
    if (Nextion::_event_wait_task != NULL) { // A task is set, send notification
      xTaskNotifyGive(Nextion::_event_wait_task);
    }
    xSemaphoreGive(Nextion::_event_wait_mutex);
  }
}

esp_err_t Nextion::_write_command(const char *data) {
  if (xSemaphoreTake(Nextion::_uart_write_mutex, pdMS_TO_TICKS(32)) == pdTRUE) {
    Nextion::_write_locked(data);
    xSemaphoreGive(Nextion::_uart_write_mutex);
    return ESP_OK;
  }
  return ESP_ERR_NOT_FINISHED;
}

void Nextion::_write_locked(const std::string &command) {
  NextionWindow::write_locked(command);
}

esp_err_t Nextion::_send_command(const std::string &command, uint16_t mutex_timeout) {
  // Verify that the Nextion state is "running" as we don't want to send data that may interrupt other processes such as updating the GUI
  if (xSemaphoreTake(Nextion::_nextion_state_mutex, pdMS_TO_TICKS(mutex_timeout)) == pdTRUE) {
    if (Nextion::_current_nextion_state != nextion_state_t::RUNNING) {
      xSemaphoreGive(Nextion::_nextion_state_mutex);
      return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(Nextion::_nextion_state_mutex);
  }

  // Wait before taking the mutex, so other writers can still queue behind it meanwhile.
  NextionWindow::wait_for_room();
  if (xSemaphoreTake(Nextion::_uart_write_mutex, pdMS_TO_TICKS(mutex_timeout)) == pdTRUE) {
    Nextion::_write_locked(command);
    xSemaphoreGive(Nextion::_uart_write_mutex);
    return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

esp_err_t Nextion::go_to_page(const char *page, uint16_t mutex_timeout) {
  return Nextion::_send_command(std::string("page ") + page, mutex_timeout);
}

esp_err_t Nextion::send_raw_command(const char *command, uint16_t mutex_timeout) {
  return Nextion::_send_command(command, mutex_timeout);
}

esp_err_t Nextion::fill(const uint32_t x, const uint32_t y, const uint32_t width, const uint32_t height, const uint16_t color, uint16_t mutex_timeout) {
  std::string command = "fill ";
  command.append(std::to_string(x));
  command.append(",");
  command.append(std::to_string(y));
  command.append(",");
  command.append(std::to_string(width));
  command.append(",");
  command.append(std::to_string(height));
  command.append(",");
  command.append(std::to_string(color));
  return Nextion::_send_command(command, mutex_timeout);
}

esp_err_t Nextion::set_component_text(const char *component_id, const char *text, uint16_t mutex_timeout) {
  std::string command = component_id;
  command.append(".txt=\"");
  command.append(text);
  command.append("\"");
  return Nextion::_send_command(command, mutex_timeout);
}

esp_err_t Nextion::set_component_value(const char *component_id, int16_t value, uint16_t mutex_timeout) {
  return Nextion::_send_command(std::string(component_id) + ".val=" + std::to_string(value), mutex_timeout);
}

esp_err_t Nextion::set_timer_value(const char *component_id, uint16_t value, uint16_t mutex_timeout) {
  return Nextion::_send_command(std::string(component_id) + ".tim=" + std::to_string(value), mutex_timeout);
}

esp_err_t Nextion::set_component_pic(const char *component_id, uint8_t value, uint16_t mutex_timeout) {
  return Nextion::_send_command(std::string(component_id) + ".pic=" + std::to_string(value), mutex_timeout);
}

esp_err_t Nextion::set_component_pic1(const char *component_id, uint8_t value, uint16_t mutex_timeout) {
  return Nextion::_send_command(std::string(component_id) + ".pic1=" + std::to_string(value), mutex_timeout);
}

esp_err_t Nextion::set_component_pic2(const char *component_id, uint8_t value, uint16_t mutex_timeout) {
  return Nextion::_send_command(std::string(component_id) + ".pic2=" + std::to_string(value), mutex_timeout);
}

esp_err_t Nextion::set_component_foreground(const char *component_id, uint16_t color, uint16_t mutex_timeout) {
  return Nextion::_send_command(std::string(component_id) + ".pco=" + std::to_string(color), mutex_timeout);
}

esp_err_t Nextion::set_component_pco2(const char *component_id, uint16_t color, uint16_t mutex_timeout) {
  return Nextion::_send_command(std::string(component_id) + ".pco2=" + std::to_string(color), mutex_timeout);
}

esp_err_t Nextion::set_component_visibility(const char *component_id, bool visibility, uint16_t mutex_timeout) {
  // Remove page name from beginning of component_id as it doesn't work with "vis"-command.
  std::string raw_component_name = component_id;
  uint16_t dot_position = raw_component_name.find('.');
  if (dot_position != std::string::npos && dot_position + 1 < raw_component_name.length()) { // A dot was found, remove page name.
    raw_component_name = raw_component_name.substr(dot_position + 1);
  }
  return Nextion::_send_command("vis " + raw_component_name + (visibility ? ",1" : ",0"), mutex_timeout);
}

esp_err_t Nextion::get_component_integer_value(const char *component_id, int32_t *result, uint16_t timeout, uint16_t mutex_timeout) {
  // Verify that the Nextion state is "running" as we don't want to send data that may interrupt other processes such as updating the GUI
  if (xSemaphoreTake(Nextion::_nextion_state_mutex, pdMS_TO_TICKS(mutex_timeout)) == pdTRUE) {
    if (Nextion::_current_nextion_state != nextion_state_t::RUNNING) {
      xSemaphoreGive(Nextion::_nextion_state_mutex);
      return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(Nextion::_nextion_state_mutex);
  }

  if (xSemaphoreTake(Nextion::_event_wait_integer_mutex, pdMS_TO_TICKS(mutex_timeout)) == pdTRUE) {
    if (Nextion::_event_wait_integer == NULL) {
      Nextion::_event_wait_integer = xTaskGetCurrentTaskHandle();
      Nextion::_event_wait_integer_result = result;
      xSemaphoreGive(Nextion::_event_wait_integer_mutex);
    } else {
      ESP_LOGE("Nextion", "Got mutex but _event_wait_integer is not NULL while fetching integer, cancel operation.");
      xSemaphoreGive(Nextion::_event_wait_integer_mutex);
      return ESP_ERR_TIMEOUT;
    }
  } else {
    ESP_LOGE("Nextion", "Failed to take _event_wait_integer_mutex while fetching integer value.");
    return ESP_ERR_TIMEOUT;
  }

  NextionWindow::wait_for_room();
  if (xSemaphoreTake(Nextion::_uart_write_mutex, pdMS_TO_TICKS(mutex_timeout)) == pdTRUE) {
    Nextion::_write_locked(std::string("get ") + component_id + ".val");
    xSemaphoreGive(Nextion::_uart_write_mutex);
  } else {
    ESP_LOGE("Nextion", "Failed to take _uart_write_mutex while fetching integer value.");
    Nextion::_event_wait_integer = NULL;
    Nextion::_event_wait_integer_result = NULL;
    return ESP_ERR_TIMEOUT;
  }

  // Wait for result
  BaseType_t wait_result = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout));

  if (xSemaphoreTake(Nextion::_event_wait_integer_mutex, portMAX_DELAY) == pdTRUE) {
    Nextion::_event_wait_integer = NULL;
    Nextion::_event_wait_integer_result = NULL;
    xSemaphoreGive(Nextion::_event_wait_integer_mutex);
  }

  if (wait_result == pdTRUE) {
    return ESP_OK;
  } else {
    return ESP_ERR_TIMEOUT;
  }
}

esp_err_t Nextion::set_brightness_level(uint8_t brightness, uint16_t mutex_timeout) {
  return Nextion::_send_command("dim=" + std::to_string(brightness), mutex_timeout);
}

nextion_state_t Nextion::get_current_state() {
  return Nextion::_current_nextion_state;
}

void Nextion::restart() {
  gpio_set_level(NEXTION_ON_OFF_GPIO, 1); // Turn off power to the display
  vTaskDelay(pdMS_TO_TICKS(250));         // Wait for screen to turn off
  gpio_set_level(NEXTION_ON_OFF_GPIO, 0); // Turn on power to the display
}

esp_err_t Nextion::start_update(uint32_t upload_baudrate, bool use_new_upload_protocol, uint64_t upload_file_size) {
  if (xSemaphoreTake(Nextion::_uart_write_mutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
    if (xSemaphoreTake(Nextion::_nextion_state_mutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
      Nextion::_current_nextion_state = nextion_state_t::UPDATING; // Set state to updating so that other functions won't write to the UART.
      NextionWindow::stop_locked();                                // The update protocol has replies of its own.
      xSemaphoreGive(Nextion::_nextion_state_mutex);
    } else {
      ESP_LOGE("Nextion", "Failed to take _nextion_state_mutex while processing request to start update!");
      xSemaphoreGive(Nextion::_uart_write_mutex);
      return ESP_ERR_NOT_FINISHED;
    }

    Nextion::restart();

    // Wait for panel to start
    vTaskDelay(pdMS_TO_TICKS(10000));

    // Send command sequence to clear NSPanel buffer
    uint8_t command_end_sequence[3] = {0xFF, 0xFF, 0xFF};
    uart_write_bytes(UART_NUM_2, "DRAKJHSUYDGBNCJHGJKSHBDN", strlen("DRAKJHSUYDGBNCJHGJKSHBDN"));
    uart_write_bytes(UART_NUM_2, command_end_sequence, sizeof(command_end_sequence));
    vTaskDelay(pdMS_TO_TICKS(50));

    uart_write_bytes(UART_NUM_2, "connect", strlen("connect"));
    uart_write_bytes(UART_NUM_2, command_end_sequence, sizeof(command_end_sequence));
    vTaskDelay(pdMS_TO_TICKS(50));

    // Wake sleeping displays before whmi-wri — a sleeping display silently ignores whmi-wri.
    // This is a no-op if the display is already awake.
    uart_write_bytes(UART_NUM_2, "sleep=0", strlen("sleep=0"));
    uart_write_bytes(UART_NUM_2, command_end_sequence, sizeof(command_end_sequence));
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI("Nextion", "Will try to init Nextion update process. New TFT file size: %llu", upload_file_size);
    std::string init_upload_command = use_new_upload_protocol ? "whmi-wris " : "whmi-wri ";
    init_upload_command.append(std::to_string(upload_file_size));
    init_upload_command.append(",");
    init_upload_command.append(std::to_string(upload_baudrate));
    init_upload_command.append(",1");
    uart_write_bytes(UART_NUM_2, init_upload_command.c_str(), init_upload_command.length());

    ESP_LOGD("Nextion", "Sending update init command to Nextion display: %s", init_upload_command.c_str());

    uart_write_bytes(UART_NUM_2, command_end_sequence, sizeof(command_end_sequence));

    uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(5000)); // Wait up to 5 second for TX buffer to clear.

    // Switch uart to desired baud
    if (uart_set_baudrate(UART_NUM_2, upload_baudrate) != ESP_OK) {
      ESP_LOGE("Nextion", "Failed to set UART baudrate %lu when initializing Nextion update!", upload_baudrate);
    }

    xSemaphoreGive(Nextion::_uart_write_mutex);
    return ESP_OK;
  } else {
    ESP_LOGE("Nextion", "Failed to take _uart_write_mutex while trying to start update to panel.");
  }
  return ESP_ERR_NOT_FINISHED;
}

esp_err_t Nextion::write_update_bytes(uint8_t *data, uint16_t size) {
  if (xSemaphoreTake(Nextion::_uart_write_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    bool write_success = uart_write_bytes(UART_NUM_2, data, size) >= 0;
    uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(1000)); // Wait up to 1 second for TX buffer to clear.

    xSemaphoreGive(Nextion::_uart_write_mutex);
    return write_success ? ESP_OK : ESP_ERR_NOT_FINISHED;
  } else {
    ESP_LOGE("Nextion", "Failed to take _uart_write_mutex while trying to start update to panel.");
  }
  return ESP_ERR_NOT_FINISHED;
}
