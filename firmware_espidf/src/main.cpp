#include <BuzzerManager.hpp>
#include <ButtonManager.hpp>
#include <ConfigManager.hpp>
#include <InterfaceManager.hpp>
#include <LittleFS.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_version.hpp>
#include <Nextion.hpp>
#include <PanelClock.hpp>
#include <RoomManager.hpp>
#include <StatusUpdateManager.hpp>
#include <UpdateManager.hpp>
#include <WebManager.hpp>
#include <WiFiManager.hpp>
#include <esp_log.h>
#include <nvs_flash.h>
#include <string>

// Topic on MQTT to send log messages to
std::string mqtt_log_topic;
TaskHandle_t task_publish_mqtt_log_message_handle = NULL;
QueueHandle_t publish_mqtt_log_messages_queue = NULL;

void task_print_mem_usage(void *param) {
  TaskStatus_t *taskStatusArray;
  UBaseType_t taskCount = 0;
  uint32_t totalRAM = 0;

  for (;;) {
    taskCount = uxTaskGetNumberOfTasks();
    taskStatusArray = (TaskStatus_t *)pvPortMalloc(taskCount * sizeof(TaskStatus_t));

    if (taskStatusArray != NULL) {
      taskCount = uxTaskGetSystemState(taskStatusArray, taskCount, NULL);
      printf("Task Name\t\t\tRAM Usage\n");
      printf("----------------------------------------\n");

      for (int i = 0; i < taskCount; i++) {
        printf("%s\t\t\t%lu bytes\n", taskStatusArray[i].pcTaskName,
               (taskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t)));
        totalRAM += (taskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t));
      }

      printf("Total RAM used by tasks: %lu bytes\n", totalRAM);
      vPortFree(taskStatusArray);
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void task_publish_mqtt_log_message(void *param) {
  char *log_message;
  uint16_t message_len = 0;

  for (;;) {
    if (xQueueReceive(publish_mqtt_log_messages_queue, &log_message, portMAX_DELAY) == pdTRUE) {
      message_len = strlen(log_message);
      if (message_len <= 0) [[unlikely]] {
        continue;
      }

      if (publish_mqtt_log_messages_queue != NULL && task_publish_mqtt_log_message_handle != NULL && MqttManager::connected()) {
        MqttManager::publish(mqtt_log_topic, log_message, message_len, false);
      }
      free(log_message);
    }
  }
}

int custom_log_vprintf(const char *fmt, va_list args) {
  // vprintf(fmt, args); // Keep default behavior, print to UART

  char *buffer = NULL;
  int len = vasprintf(&buffer, fmt, args);
  if (len != -1) {
    printf(buffer);
    if (xQueueSend(publish_mqtt_log_messages_queue, &buffer, pdMS_TO_TICKS(100)) != pdTRUE) {
      free(buffer);
    }
  }

  return len;
}

static_assert(INCLUDE_vTaskSuspend == 1, "portMAX_DELAY is not an indefinite block; audit all mutex/queue waits using portMAX_DELAY");

extern "C" void app_main() {
  ESP_LOGI("Main", "Starting NSPanel Manager firmware. Version " NSPM_VERSION ". Marking boot as successful.");
  UpdateManager::mark_boot_successful();

  esp_event_loop_create_default();

  // xTaskCreatePinnedToCore(task_print_mem_usage, "task_mem_debug", 4096, NULL, 3, NULL, 1);

  esp_err_t nvs_flash_init_res = nvs_flash_init();
  if (nvs_flash_init_res != ESP_OK) {
    ESP_LOGE("Main", "Failed to init NVS! Error %s", esp_err_to_name(nvs_flash_init_res));
  }
  ESP_ERROR_CHECK(nvs_flash_init_res);

  if (LittleFS::mount() != ESP_OK) {
    ESP_LOGE("Main", "Failed to mount LittleFS!");
  }

  ConfigManager::create_default(); // Set default values on all config entities

  WiFiManager::init();
  if (ConfigManager::load_config() != ESP_OK) {
    ESP_LOGE("Main", "Failed to load config from LittleFS. If this is the first time running the panel this is normal as no config has been saved yet.");
    ESP_LOGI("Main", "Default config values has been applied, will save to create a config file.");
    esp_err_t config_save_result = ConfigManager::save_config();
    if (config_save_result != ESP_OK) {
      ESP_LOGE("Main", "Failed to save config to LittleFS, got error %s!", esp_err_to_name(config_save_result));
    }

    WiFiManager::start_ap(&ConfigManager::wifi_hostname);
  } else {
    // Start task that handles WiFi connection
    WiFiManager::start_client(&ConfigManager::wifi_ssid, &ConfigManager::wifi_psk, &ConfigManager::wifi_hostname, &ConfigManager::wifi_hostname);
  }

  switch (ConfigManager::log_level) {
  case esp_log_level_t::ESP_LOG_DEBUG:
    ESP_LOGI("Main", "Setting log level to DEBUG.");
    break;

  case esp_log_level_t::ESP_LOG_INFO:
    ESP_LOGI("Main", "Setting log level to INFO.");
    break;

  case esp_log_level_t::ESP_LOG_WARN:
    ESP_LOGI("Main", "Setting log level to WARN.");
    break;

  case esp_log_level_t::ESP_LOG_ERROR:
    ESP_LOGI("Main", "Setting log level to ERROR.");
    break;

  default:
    ESP_LOGI("Main", "Unknown log level: %d.", static_cast<uint8_t>(ConfigManager::log_level));
    break;
  }

  // Setup ButtonManager to handle physical buttons and relays
  ButtonManager::init();

  // Setup BuzzerManager to drive the buzzer and track panel events that can have sounds
  BuzzerManager::init();

  // Only start managers for actual functionality if MQTT is configured.
  if (!ConfigManager::mqtt_server.empty()) {
    // Start task that handles MQTT connection
    MqttManager::start(&ConfigManager::mqtt_server, &ConfigManager::mqtt_port, &ConfigManager::mqtt_username, &ConfigManager::mqtt_password);

    // Now that we have created the MQTT client we can register callbacks from it, register ButtonManager
    ButtonManager::init_mqtt();
    BuzzerManager::init_mqtt();

    // Set the wall clock and timezone from what the manager sends
    PanelClock::init();

    // MQTT is now setup, enable custom logging through MQTT
    publish_mqtt_log_messages_queue = xQueueCreate(16, sizeof(char *));
    xTaskCreatePinnedToCore(task_publish_mqtt_log_message, "pub_mqtt_log", 4096, NULL, 3, &task_publish_mqtt_log_message_handle, 1);
    mqtt_log_topic = std::string("nspanel/") + WiFiManager::mac_string() + "/log";
    esp_log_set_vprintf(custom_log_vprintf);

    // Start RoomManager
    RoomManager::init();

    // Start manager that handles temperature and status updates
    StatusUpdateManager::init();
  }

  // Start task that handles the HTTP server
  WebManager::start();

  // Start the interface and load config
  InterfaceManager::init();

  // Wait until we have been accepted by a manager and received both address and port
  while (NSPM_ConfigManager::get_manager_address().empty() && NSPM_ConfigManager::get_manager_port() == 0) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  // Hook into update manager
  UpdateManager::init();

  // We have been accepted, update internal stored checksum of installed software if needed
  UpdateManager::update_internal_firmware_checksum();

  ESP_LOGI("Main", "Task setup OK. Exiting main.");
}