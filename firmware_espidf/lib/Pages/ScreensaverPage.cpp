#include <ConfigManager.hpp>
#include <GUI_data.hpp>
#include <HomePage.hpp>
#include <InterfaceManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <Nextion.hpp>
#include <RoomManager.hpp>
#include <ScreensaverPage.hpp>
#include <StatusUpdateManager_events.hpp>
// #include <cJSON.h>
#include <cstdio>
#include <esp_http_client.h>
#include <esp_log.h>

void ScreensaverPage::init() {
  // Only register handlers once. init() used to run on every CONFIG_LOADED,
  // re-registering every handler and re-subscribing every topic each time.
  if (ScreensaverPage::_weather_update_data_mutex != NULL) {
    return;
  }
  esp_log_level_set("ScreensaverPage", ConfigManager::log_level);
  ScreensaverPage::_weather_update_data_mutex = xSemaphoreCreateMutex();

  MqttManager::register_handler(MQTT_EVENT_ANY, ScreensaverPage::_mqtt_event_handler, NULL);
  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, ESP_EVENT_ANY_ID, ScreensaverPage::_nspm_config_event_handler, NULL);
  esp_event_handler_register(STATUSUPDATEMANAGER_EVENT, statusupdatemanagerevent_t::AVERAGE_TEMP_UPDATE, ScreensaverPage::_new_temperature_event, NULL);

  ScreensaverPage::_subscribe_to_mqtt_topics();
}

std::vector<uint8_t> _download_data_store;
esp_err_t HttpEventHandler(esp_http_client_event_t *event) {
  switch (event->event_id) {
  case HTTP_EVENT_ON_DATA: {
    if (!esp_http_client_is_chunked_response(event->client)) {
      _download_data_store.insert(_download_data_store.end(), (uint8_t *)event->data, (uint8_t *)event->data + event->data_len);
    } else {
      ESP_LOGE("ScreensaverPage", "Download data is chunked. Not supported!");
    }
    break;
  }

  default:
    break;
  }
  return ESP_OK;
}

void ScreensaverPage::show() {
  ESP_LOGD("ScreensaverPage", "Showing screensaver page. Currently shown? %s", ScreensaverPage::_currently_shown ? "Yes" : "No");
  if (ScreensaverPage::_currently_shown) {
    // Do not "show" page again when it's already showing.
    return;
  }

  if (ScreensaverPage::_weather_update_data_mutex == NULL) {
    // Page has not been initialized, do that first.
    ScreensaverPage::init();
  }

  InterfaceManager::begin_page(ScreensaverPage::unshow);

  ScreensaverPage::_go_to_nextion_page();
  ScreensaverPage::_update_displayed_date();
  ScreensaverPage::_update_displayed_time();
  ScreensaverPage::_update_displayed_temperature();

  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    if (config->screensaver_mode == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITH_BACKGROUND || config->screensaver_mode == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITHOUT_BACKGROUND) {
      xTaskCreatePinnedToCore(ScreensaverPage::_task_update_displayed_weather_data, "update_weather_data", 4096, NULL, 2, NULL, 1);
    }
  } else {
    ESP_LOGE("ScreensaverPage", "Failed to get current config while trying to display screensaver page. Will update weather data just in case screensaver mode is with weather data.");
    xTaskCreatePinnedToCore(ScreensaverPage::_task_update_displayed_weather_data, "update_weather_data", 4096, NULL, 2, NULL, 1);
  }

  RoomManager::go_to_default_room(); // Go to default room so that it is the room that is shown when the screensaver is hidden.

  // Test code for album cover follows:
  // vTaskDelay(pdMS_TO_TICKS(2000)); // Wait for screen to finish refreshing

  // _download_data_store.clear();
  // std::string album_cover_url = "http://";
  // album_cover_url.append(NSPM_ConfigManager::get_manager_address());
  // album_cover_url.append(":");
  // album_cover_url.append(std::to_string(NSPM_ConfigManager::get_manager_port()));
  // album_cover_url.append("/api/get_album_cover");
  // ESP_LOGD("ScreensaverPage", "Downloading data from %s", album_cover_url.c_str());

  // esp_http_client_handle_t client;
  // esp_http_client_config_t config = {
  //     .url = album_cover_url.c_str(),
  //     .event_handler = HttpEventHandler,
  // };

  // client = esp_http_client_init(&config);
  // // Perform the actual HTTP request to get data
  // esp_err_t err = esp_http_client_perform(client);
  // if (err == ESP_OK) {
  //   esp_http_client_cleanup(client);
  // } else {
  //   ESP_LOGE("ScreensaverPage", "Failed to download data from %s. Got error: %s. HTTP Status code: %d.", album_cover_url, esp_err_to_name(err), esp_http_client_get_status_code(client));
  //   esp_http_client_cleanup(client);
  // }

  // cJSON *json = cJSON_ParseWithLength((const char *)_download_data_store.data(), _download_data_store.size());
  // if (json != NULL) {
  //   Nextion::go_to_page("0", 5000); // Navigate to empty page
  //   vTaskDelay(pdMS_TO_TICKS(1000));
  //   Nextion::send_raw_command("cls BLACK", 5000); // Clear screen
  //   vTaskDelay(pdMS_TO_TICKS(1000));

  //   // Assume album cover is always 100x100 pixels.
  //   int x = 0;
  //   int y = 0;
  //   const cJSON *pixel = NULL;
  //   const cJSON *pixels = cJSON_GetObjectItem(json, "pixels");
  //   ESP_LOGD("ScreensaverPage", "Starting to draw album cover. Total num pixels: %d", cJSON_GetArraySize(pixels));
  //   cJSON_ArrayForEach(pixel, pixels) {
  //     Nextion::fill(50 + x, 50 + y, 1, 1, pixel->valueint, 5000);

  //     x++;
  //     if (x == 100) {
  //       x = 0;
  //       y++;
  //     }
  //   }
  // } else {
  //   ESP_LOGE("ScreensaverPage", "Failed to parse JSON data.");
  // }
  // cJSON_Delete(json);
}

void ScreensaverPage::unshow() {
  ScreensaverPage::_currently_shown = false;
}

bool ScreensaverPage::showing() {
  return ScreensaverPage::_currently_shown;
}

void ScreensaverPage::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == MQTT_EVENT_DATA) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event->topic_len == 0 || event->data_len == 0) {
      return; // Do not process empty messages
    }

    std::string topic_string = std::string(event->topic, event->topic_len);

    std::string manager_address = NSPM_ConfigManager::get_manager_address();
    std::string mqtt_base_topic = "nspanel/mqttmanager_";
    mqtt_base_topic.append(manager_address);

    std::string time_topic = mqtt_base_topic;
    time_topic.append("/status/time");
    std::string date_topic = mqtt_base_topic;
    date_topic.append("/status/date");
    std::string ampm_topic = mqtt_base_topic;
    ampm_topic.append("/status/ampm");
    std::string weather_topic = mqtt_base_topic;
    weather_topic.append("/status/weather");

    std::string inside_temperature_sensor_state_topic = ScreensaverPage::_inside_temperature_sensor_state_topic.get();
    if (topic_string.compare(inside_temperature_sensor_state_topic) == 0) {
      // Got new temperature from MQTT, update display.
      std::string new_temperature_string = std::string(event->data, event->data_len).c_str();
      ScreensaverPage::_current_temperature.set(new_temperature_string);
      ScreensaverPage::_update_displayed_temperature();
    } else if (topic_string.compare(time_topic) == 0) {
      ScreensaverPage::_current_time = std::string(event->data, event->data_len);
      ScreensaverPage::_update_displayed_time();
    } else if (topic_string.compare(date_topic) == 0) {
      // We got new date, update display:
      ScreensaverPage::_current_date = std::string(event->data, event->data_len);
      ScreensaverPage::_update_displayed_date();
    } else if (topic_string.compare(ampm_topic) == 0) {
      // We got new AM/PM, update display:
      ScreensaverPage::_am_pm_string = std::string(event->data, event->data_len);
      ScreensaverPage::_update_displayed_time();
    } else if (topic_string.compare(weather_topic) == 0) {
      if (ScreensaverPage::_weather_update_data_mutex != NULL) {
        if (xSemaphoreTake(ScreensaverPage::_weather_update_data_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
          ScreensaverPage::_weather_update_mqtt_data.clear();
          ScreensaverPage::_weather_update_mqtt_data.insert(ScreensaverPage::_weather_update_mqtt_data.end(), event->data, event->data + event->data_len);
          xSemaphoreGive(ScreensaverPage::_weather_update_data_mutex);
          // New weather data loaded, update display.
          xTaskCreatePinnedToCore(ScreensaverPage::_task_update_displayed_weather_data, "update_weather_data", 4096, NULL, 2, NULL, 1);
        } else {
          ESP_LOGW("ScreensaverPage", "Failed to take weather data mutex while processing new data from MQTT. Will wait for next forecast.");

          // TaskHandle_t holder = xSemaphoreGetMutexHolder(ScreensaverPage::_weather_update_data_mutex);
          // if (holder != NULL) {
          //   TaskStatus_t status;
          //   vTaskGetInfo(/* The handle of the task being queried. */
          //                holder,
          //                /* The TaskStatus_t structure to complete with information
          //                on xTask. */
          //                &status,
          //                /* Include the stack high water mark value in the
          //                TaskStatus_t structure. */
          //                pdTRUE,
          //                /* Include the task state in the TaskStatus_t structure. */
          //                eInvalid);

          //   ESP_LOGE("ScreensaverPage", "Holding task: %s", status.pcTaskName);
          // }
        }
      } else {
        ESP_LOGW("ScreensaverPage", "Weather update data mutex is NULL. Will wait for next forecast.");
      }
    }
  }
}

void ScreensaverPage::_nspm_config_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case nspm_configmanager_event::CONFIG_LOADED: { // New config loaded while showing screensaver. Update screen
    // Only touches MQTT if the manager address or inside temperature topic changed.
    ScreensaverPage::_subscribe_to_mqtt_topics();
    if (ScreensaverPage::_currently_shown) {
      std::shared_ptr<NSPanelConfig> new_config;
      if (NSPM_ConfigManager::get_config(&new_config) == ESP_OK) [[likely]] {
        if (ScreensaverPage::_current_screensaver_mode.get() != new_config->screensaver_mode) {
          ScreensaverPage::_current_screensaver_mode.set(new_config->screensaver_mode);

          ScreensaverPage::_go_to_nextion_page();
          ScreensaverPage::_update_displayed_date();
          ScreensaverPage::_update_displayed_time();
          xTaskCreatePinnedToCore(ScreensaverPage::_task_update_displayed_weather_data, "update_weather_data", 4096, NULL, 2, NULL, 1);
        }

        if (ScreensaverPage::_screensaver_brightness != new_config->screensaver_dim_level) {
          ScreensaverPage::_update_display_brightness();
        }
      } else {
        ESP_LOGE("ScreensaverPage", "Failed to get config while processing 'new config event'. May become out of sync with manager until next config update.");
      }

      RoomManager::go_to_default_room(); // Go to default room so that it is the room that is shown when the screensaver is hidden.
    }
    break;
  }

  default:
    break;
  }
}

void ScreensaverPage::_new_temperature_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  switch (event_id) {
  case statusupdatemanagerevent_t::AVERAGE_TEMP_UPDATE: {
    // So that no inside temperature MQTT topic is configured. If it is it means that the panel is configured to get inside temperature from
    // a sensor via MQTTManger over MQTT. In that case, simply exit this function.
    if (!ScreensaverPage::_inside_temperature_sensor_state_topic.get().empty()) {
      return;
    }
    char temperature_str[16];
    snprintf(temperature_str, sizeof(temperature_str), "%.1f", *((double *)event_data));
    ScreensaverPage::_current_temperature.set(temperature_str);
    ScreensaverPage::_update_displayed_temperature();

    break;
  }

  default:
    break;
  }
}

void ScreensaverPage::_shared_ptr_weather_update_cleanup(NSPanelWeatherUpdate *data) {
  nspanel_weather_update__free_unpacked(data, NULL);
}

void ScreensaverPage::_subscribe_to_mqtt_topics() {
  // MqttManager keeps these subscribed across reconnects, so this only needs to
  // run when the manager address or inside temperature topic changes.
  std::string manager_address = NSPM_ConfigManager::get_manager_address();
  if (manager_address.empty()) {
    ESP_LOGE("ScreensaverPage", "Failed to subscribe to relevant MQTT topics as no manager address is set.");
    return;
  }

  std::string inside_temperature_sensor_mqtt_topic = ScreensaverPage::_inside_temperature_sensor_state_topic.get();
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    inside_temperature_sensor_mqtt_topic = std::string(config->inside_temperature_sensor_mqtt_topic);
  }

  std::string old_manager_address = ScreensaverPage::_subscribed_manager_address.get();
  std::string old_inside_temperature_sensor_mqtt_topic = ScreensaverPage::_inside_temperature_sensor_state_topic.get();
  if (manager_address != old_manager_address) {
    for (const char *suffix : {"/status/time", "/status/date", "/status/ampm", "/status/weather"}) {
      if (!old_manager_address.empty()) {
        MqttManager::unsubscribe("nspanel/mqttmanager_" + old_manager_address + suffix);
      }
      std::string topic = "nspanel/mqttmanager_" + manager_address + suffix;
      if (MqttManager::subscribe(topic) != ESP_OK) {
        ESP_LOGE("ScreensaverPage", "Failed to subscribe to %s for screensaver page.", topic.c_str());
      }
    }
    ScreensaverPage::_subscribed_manager_address.set(manager_address);
  }

  // subscribe() does nothing if the topic is already subscribed, so always call it.
  if (inside_temperature_sensor_mqtt_topic != old_inside_temperature_sensor_mqtt_topic && !old_inside_temperature_sensor_mqtt_topic.empty()) {
    MqttManager::unsubscribe(old_inside_temperature_sensor_mqtt_topic);
  }
  ScreensaverPage::_inside_temperature_sensor_state_topic.set(inside_temperature_sensor_mqtt_topic);
  if (!inside_temperature_sensor_mqtt_topic.empty()) {
    ESP_LOGD("ScreensaverPage", "Subscribing to inside temperature sensor state topic: %s", inside_temperature_sensor_mqtt_topic.c_str());
    if (MqttManager::subscribe(inside_temperature_sensor_mqtt_topic) != ESP_OK) {
      ESP_LOGE("ScreensaverPage", "Failed to subscribe to inside temperature sensor state topic.");
    }
  }
}

void ScreensaverPage::_update_displayed_time() {
  // Screensaver with weather
  Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_time, ScreensaverPage::_current_time.get().c_str(), 250);
  Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_am_pm_name, ScreensaverPage::_am_pm_string.get().c_str(), 250);

  // Screensaver without weather
  Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_time, ScreensaverPage::_current_time.get().c_str(), 250);
  Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_am_pm_name, ScreensaverPage::_am_pm_string.get().c_str(), 250);
}

void ScreensaverPage::_update_displayed_date() {
  if (ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITH_BACKGROUND || ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITHOUT_BACKGROUND) {
    Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_day_name, ScreensaverPage::_current_date.get().c_str(), 250);
  } else if (ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITH_BACKGROUND || ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITHOUT_BACKGROUND) {
    Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_day_name, ScreensaverPage::_current_date.get().c_str(), 250);
  } else if (ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER) {
    // Perform nothing and do not show error message below.
  } else {
    ESP_LOGE("ScreensaverPage", "Unknown screensaver mode %d while processing new date from MQTT.", (int)ScreensaverPage::_current_screensaver_mode.get());
  }
}

void ScreensaverPage::_update_displayed_temperature() {
  if (ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITH_BACKGROUND || ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITHOUT_BACKGROUND) {
    Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_room_temperature_name, ScreensaverPage::_current_temperature.get().c_str(), 1000);
  } else if (ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITH_BACKGROUND || ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITHOUT_BACKGROUND) {
    Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_room_temperature_name, ScreensaverPage::_current_temperature.get().c_str(), 1000);
  } else if (ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER) {
    // Perform nothing and do not show error message below.
  } else {
    ESP_LOGE("ScreensaverPage", "Unknown screensaver mode %d while processing new date from MQTT.", (int)ScreensaverPage::_current_screensaver_mode.get());
  }
}

void ScreensaverPage::_task_update_displayed_weather_data(void *param) {
  if (_weather_update_data_mutex != NULL && xSemaphoreTake(ScreensaverPage::_weather_update_data_mutex, pdMS_TO_TICKS(1000)) == pdPASS) [[likely]] {
    if (ScreensaverPage::_weather_update_mqtt_data.size() > 0) [[likely]] {
      NSPanelWeatherUpdate *new_weather_data = nspanel_weather_update__unpack(NULL, ScreensaverPage::_weather_update_mqtt_data.size(), ScreensaverPage::_weather_update_mqtt_data.data());
      if (new_weather_data != NULL) [[likely]] {
        ESP_LOGD("ScreensaverPage", "Updating screensaver page data.");
        ScreensaverPage::_weather_update_data = std::shared_ptr<NSPanelWeatherUpdate>(new_weather_data, &ScreensaverPage::_shared_ptr_weather_update_cleanup);
        Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_weather_icon_name, ScreensaverPage::_weather_update_data->current_weather_icon, 250);
        Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_temperature_name, ScreensaverPage::_weather_update_data->current_temperature_string, 250);
        Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_temperature_name, ScreensaverPage::_weather_update_data->current_temperature_string, 250); // Outside temperature on minimal screensaver.
        Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_wind_name, ScreensaverPage::_weather_update_data->current_wind_string, 250);
        Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_sunrise_name, ScreensaverPage::_weather_update_data->sunrise_string, 250);
        Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_sunset_name, ScreensaverPage::_weather_update_data->sunset_string, 250);
        Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_max_min_temperature_name, ScreensaverPage::_weather_update_data->current_maxmin_temperature, 250);
        Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_rain_name, ScreensaverPage::_weather_update_data->current_precipitation_string, 250);

        ESP_LOGD("ScreensaverPage", "Updating forecast. Forecast items: %zu, new weather forecast items: %zu", ScreensaverPage::_weather_update_data->n_forecast_items, new_weather_data->n_forecast_items);
        for (int i = 0; i < ScreensaverPage::_weather_update_data->n_forecast_items && i < 5; i++) { // Update all available forecasts but no more than 5 as that's how many forecasts are displayed on the page
          auto item = ScreensaverPage::_weather_update_data->forecast_items[i];
          Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_forecast_day_names[i], item->display_string, 250);
          Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_forecast_day_icon_names[i], item->weather_icon, 250);
          Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_forecast_day_max_min_names[i], item->temperature_maxmin_string, 250);
          Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_forecast_day_rain_names[i], item->precipitation_string, 250);
          Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_forecast_day_wind_names[i], item->wind_string, 250);
        }
        ESP_LOGD("ScreensaverPage", "Forecast updated. Successfully updated screensaver page with new weather data.");
      } else {
        ESP_LOGE("ScreensaverPage", "Got new weather data but failed to decode it into protobuf object.");
      }
    } else {
      ESP_LOGE("ScreensaverPage", "Trying to update screensaver page but no data to decode protobuf is available.");
    }

    xSemaphoreGive(ScreensaverPage::_weather_update_data_mutex);
  } else {
    ESP_LOGE("ScreensaverPage", "Failed to get _weather_update_data_mutex when updating display.");
  }

  vTaskDelete(NULL);         // Stop this task without causing about.
  vTaskDelay(portMAX_DELAY); // Wait for task to be deleted.
}

void ScreensaverPage::_update_display_brightness() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) == ESP_OK) {
    if (ScreensaverPage::_current_screensaver_mode.get() == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER) {
      ESP_LOGD("ScreensaverPage", "Screensaver mode is 'no_screensaver'. Setting display brightness to 0.");
      Nextion::set_brightness_level(0, 1000); // No screensaver is to be shown, simply set brightness to 0
    } else {
      ESP_LOGD("ScreensaverPage", "Setting screensaver display brightness to 0.");
      Nextion::set_brightness_level(config->screensaver_dim_level, 1000);
    }
    ScreensaverPage::_screensaver_brightness = config->screensaver_dim_level;
  } else {
    ESP_LOGE("ScreensaverPage", "Failed to get NSPanel Config when showing screensaver page! Will cancel operation.");
    return;
  }
}

void ScreensaverPage::_go_to_nextion_page() {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK) [[unlikely]] {
    ESP_LOGE("ScreensaverPage", "Failed to get NSPanel Config when showing screensaver page! Will cancel operation.");
    return;
  }
  ScreensaverPage::_current_screensaver_mode = config->screensaver_mode;

  switch (ScreensaverPage::_current_screensaver_mode.get()) {
  case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITH_BACKGROUND: {
    ESP_LOGD("ScreensaverPage", "Showing screensaver: weather with background");
    Nextion::set_component_value(GUI_SCREENSAVER_PAGE::screensaver_background_control_variable_name, 1, 250);
    Nextion::go_to_page(GUI_SCREENSAVER_PAGE::page_name, 250);
    ScreensaverPage::_currently_shown = true;
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_am_pm_name_raw, config->clock_us_style, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_current_room_temperature_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_current_room_temperature_icon_name, config->show_screensaver_inside_temperature, 250);
    break;
  }

  case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITHOUT_BACKGROUND: {
    ESP_LOGD("ScreensaverPage", "Showing screensaver: weather without background");
    Nextion::set_component_value(GUI_SCREENSAVER_PAGE::screensaver_background_control_variable_name, 0, 250);
    Nextion::go_to_page(GUI_SCREENSAVER_PAGE::page_name, 250);
    ScreensaverPage::_currently_shown = true;
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_am_pm_name_raw, config->clock_us_style, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_current_room_temperature_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_current_room_temperature_icon_name, config->show_screensaver_inside_temperature, 250);
    break;
  }

  case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITH_BACKGROUND: {
    ESP_LOGD("ScreensaverPage", "Showing screensaver: minimal with background");
    Nextion::set_component_value(GUI_SCREENSAVER_PAGE::screensaver_minimal_background_control_variable_name, 1, 250);
    Nextion::go_to_page(GUI_SCREENSAVER_PAGE::screensaver_minimal_page_name, 250);
    ScreensaverPage::_currently_shown = true;
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_am_pm_name_raw, config->clock_us_style, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_room_temperature_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_room_temperature_icon_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_temperature_name, config->show_screensaver_outside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_weather_icon_name, config->show_screensaver_outside_temperature, 250);
    break;
  }

  case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITHOUT_BACKGROUND: {
    ESP_LOGD("ScreensaverPage", "Showing screensaver: minimal without background");
    Nextion::set_component_value(GUI_SCREENSAVER_PAGE::screensaver_minimal_background_control_variable_name, 0, 250);
    Nextion::go_to_page(GUI_SCREENSAVER_PAGE::screensaver_minimal_page_name, 250);
    ScreensaverPage::_currently_shown = true;
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_am_pm_name_raw, config->clock_us_style, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_room_temperature_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_room_temperature_icon_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_temperature_name, config->show_screensaver_outside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_weather_icon_name, config->show_screensaver_outside_temperature, 250);
    break;

  case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER:
    ESP_LOGD("ScreensaverPage", "Showing screensaver: no screensaver");
    Nextion::go_to_page(GUI_SCREENSAVER_PAGE::screensaver_minimal_page_name, 250);
    ScreensaverPage::_currently_shown = true;
    break;
  }

  default:
    ESP_LOGE("ScreensaverPage", "Unknown screensaver mode when showing screensaver!");
    break;
  }

  vTaskDelay(pdMS_TO_TICKS(50)); // Wait for screen to go to page.
  ScreensaverPage::_update_display_brightness();
}