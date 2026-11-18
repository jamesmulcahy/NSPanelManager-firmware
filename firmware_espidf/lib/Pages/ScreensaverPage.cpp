#include <ConfigManager.hpp>
#include <GUI_data.hpp>
#include <HomePage.hpp>
#include <InterfaceManager.hpp>
#include <LvglText.hpp>
#include <LvglUi.hpp>
#include <ScreensaverView.hpp>
#include <MqttManager.hpp>
#include <PanelMetrics.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <Nextion.hpp>
#include <RoomManager.hpp>
#include <PanelClock.hpp>
#include <ScreensaverPage.hpp>
#include <StatusUpdateManager_events.hpp>
// #include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
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
  ScreensaverPage::_calendar_mutex = xSemaphoreCreateMutex();
  ScreensaverPage::_energy_mutex = xSemaphoreCreateMutex();
  // The body turned to another card (GUI task): what the QEMU test and a log reader see of the rotation.
  ScreensaverView::on_card = [](const char *card, uint32_t seconds) { ESP_LOGI("ScreensaverPage", "Screensaver card: %s (%u s)", card, (unsigned)seconds); };

  // No inside temperature until the sensor (or the room's temperature sensor topic) has produced one.
  ScreensaverPage::_current_temperature.set("--");

  MqttManager::register_handler<ScreensaverPage::_mqtt_event_handler, PanelMetrics::HANDLER_SCREENSAVER_PAGE>(MQTT_EVENT_ANY, NULL);
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
  ESP_LOGD("ScreensaverPage", "Showing screensaver page. Currently shown? %s", ScreensaverPage::showing() ? "Yes" : "No");
  if (ScreensaverPage::showing()) {
    // Do not "show" page again when it's already showing.
    return;
  }

  if (ScreensaverPage::_weather_update_data_mutex == NULL) {
    // Page has not been initialized, do that first.
    ScreensaverPage::init();
  }

  InterfaceManager::begin_page(ScreensaverPage::unshow);

  // A calendar update that arrived while another page was shown, or whose deferred decode was dropped.
  ScreensaverPage::_decode_calendar_if_stale();
  ScreensaverPage::_decode_energy_if_stale();
  ScreensaverPage::_go_to_nextion_page();
  ScreensaverPage::_update_displayed_date();
  ScreensaverPage::_update_displayed_time();
  ScreensaverPage::_update_displayed_temperature();

  // Weather is only rendered while the screensaver is shown, so bring it up to date now.
  // The minimal (datetime) screensaver shows the outside temperature too.
  if (ScreensaverPage::_current_screensaver_mode.get() != NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER) {
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
  // Nothing to reset; InterfaceManager tracks whether this page is shown.
}

bool ScreensaverPage::showing() {
  return InterfaceManager::is_current_page(ScreensaverPage::unshow);
}

void ScreensaverPage::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == MQTT_EVENT_DATA) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event->topic_len == 0 || event->data_len == 0) {
      return; // Do not process empty messages
    }
    if (event->data_len != event->total_data_len) [[unlikely]] {
      return; // A fragment of a message larger than the MQTT buffer, e.g. a truncated forecast. MqttManager logs the drop.
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

    std::string calendar_topic = mqtt_base_topic;
    calendar_topic.append("/status/calendar");
    std::string energy_topic = mqtt_base_topic;
    energy_topic.append("/status/energy");

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
    } else if (topic_string.compare(calendar_topic) == 0) {
      // Store the newest bytes and nothing more: decoding and formatting happen on LvglUi's task.
      if (ScreensaverPage::_calendar_mutex != NULL && xSemaphoreTake(ScreensaverPage::_calendar_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
        ScreensaverPage::_calendar_mqtt_data.assign(event->data, event->data + event->data_len);
        ScreensaverPage::_calendar_stale = true;
        xSemaphoreGive(ScreensaverPage::_calendar_mutex);
        // Only LVGL panels draw the calendar. One job at a time covers any number of updates: it decodes the newest.
        if (LvglUi::active() && !ScreensaverPage::_calendar_job_queued.exchange(true)) {
          LvglUi::defer([]() {
            ScreensaverPage::_calendar_job_queued = false;
            ScreensaverPage::_decode_calendar_if_stale();
            if (ScreensaverPage::showing()) {
              ScreensaverPage::_lvgl_render();
            }
          });
        }
      } else {
        ESP_LOGW("ScreensaverPage", "Failed to take calendar mutex while processing new data from MQTT. Will wait for next update.");
      }
    } else if (topic_string.compare(energy_topic) == 0) {
      // As the calendar: store the newest bytes, decode and format on LvglUi's task.
      if (ScreensaverPage::_energy_mutex != NULL && xSemaphoreTake(ScreensaverPage::_energy_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
        ScreensaverPage::_energy_mqtt_data.assign(event->data, event->data + event->data_len);
        ScreensaverPage::_energy_stale = true;
        xSemaphoreGive(ScreensaverPage::_energy_mutex);
        if (LvglUi::active() && !ScreensaverPage::_energy_job_queued.exchange(true)) {
          LvglUi::defer([]() {
            ScreensaverPage::_energy_job_queued = false;
            ScreensaverPage::_decode_energy_if_stale();
            if (ScreensaverPage::showing()) {
              ScreensaverPage::_lvgl_render();
            }
          });
        }
      } else {
        ESP_LOGW("ScreensaverPage", "Failed to take energy mutex while processing new data from MQTT. Will wait for next update.");
      }
    } else if (topic_string.compare(weather_topic) == 0) {
      if (ScreensaverPage::_weather_update_data_mutex != NULL) {
        if (xSemaphoreTake(ScreensaverPage::_weather_update_data_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
          ScreensaverPage::_weather_update_mqtt_data.clear();
          ScreensaverPage::_weather_update_mqtt_data.insert(ScreensaverPage::_weather_update_mqtt_data.end(), event->data, event->data + event->data_len);
          xSemaphoreGive(ScreensaverPage::_weather_update_data_mutex);
          // New weather data loaded. Update the display only if the screensaver is shown; otherwise show() renders it.
          if (ScreensaverPage::showing()) {
            xTaskCreatePinnedToCore(ScreensaverPage::_task_update_displayed_weather_data, "update_weather_data", 4096, NULL, 2, NULL, 1);
          }
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
    {
      // The rotation's cards or times changed: draw the screensaver again, on LvglUi's task. Cheap (a string
      // compare) and kept up to date while another page is shown, so the next load does not draw for nothing.
      std::shared_ptr<NSPanelConfig> cards_config;
      if (NSPM_ConfigManager::get_config(&cards_config) == ESP_OK && cards_config) {
        std::string signature = ScreensaverPage::_cards_signature(*cards_config);
        if (signature != ScreensaverPage::_cards_signature_seen.get()) {
          ScreensaverPage::_cards_signature_seen.set(signature);
          if (ScreensaverPage::showing() && LvglUi::active()) {
            LvglUi::defer([]() { ScreensaverPage::_lvgl_render(); });
          }
        }
      }
    }
    if (ScreensaverPage::showing()) {
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
    for (const char *suffix : {"/status/time", "/status/date", "/status/ampm", "/status/weather", "/status/calendar", "/status/energy"}) {
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
  ScreensaverPage::_lvgl_render();
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
  ScreensaverPage::_lvgl_render();
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
  ScreensaverPage::_lvgl_render();
}

void ScreensaverPage::_task_update_displayed_weather_data(void *param) {
  if (!ScreensaverPage::showing()) {
    // Another page took over before this task ran, e.g. at boot, where the home page follows the screensaver within
    // 80 ms. The screensaver's labels are not shown, so do not write them. show() renders the weather next time.
    ESP_LOGD("ScreensaverPage", "Screensaver no longer shown, skipping weather update.");
  } else if (_weather_update_data_mutex != NULL && xSemaphoreTake(ScreensaverPage::_weather_update_data_mutex, pdMS_TO_TICKS(1000)) == pdPASS) [[likely]] {
    if (ScreensaverPage::_weather_update_mqtt_data.size() > 0) [[likely]] {
      NSPanelWeatherUpdate *new_weather_data = nspanel_weather_update__unpack(NULL, ScreensaverPage::_weather_update_mqtt_data.size(), ScreensaverPage::_weather_update_mqtt_data.data());
      if (new_weather_data != NULL) [[likely]] {
        ESP_LOGD("ScreensaverPage", "Updating screensaver page data.");
        ScreensaverPage::_weather_update_data = std::shared_ptr<NSPanelWeatherUpdate>(new_weather_data, &ScreensaverPage::_shared_ptr_weather_update_cleanup);
        NSPanelConfig__NSPanelScreensaverMode mode = ScreensaverPage::_current_screensaver_mode.get();
        if (mode == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITH_BACKGROUND || mode == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITHOUT_BACKGROUND) {
          // The minimal screensaver shows only the outside temperature.
          Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_temperature_name, ScreensaverPage::_weather_update_data->current_temperature_string, 250);
          ESP_LOGD("ScreensaverPage", "Updated outside temperature on minimal screensaver.");
        } else {
          Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_weather_icon_name, ScreensaverPage::_weather_update_data->current_weather_icon, 250);
          Nextion::set_component_text(GUI_SCREENSAVER_PAGE::label_current_temperature_name, ScreensaverPage::_weather_update_data->current_temperature_string, 250);
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
        }
      } else {
        ESP_LOGE("ScreensaverPage", "Got new weather data but failed to decode it into protobuf object.");
      }
    } else {
      // Expected at boot: show() runs before the retained weather message has arrived (the topic name needs the manager
      // address from the config). The MQTT handler renders the weather as soon as it does arrive.
      ESP_LOGD("ScreensaverPage", "No weather data received yet, skipping weather update.");
    }

    xSemaphoreGive(ScreensaverPage::_weather_update_data_mutex);
    ScreensaverPage::_lvgl_render();
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
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_am_pm_name_raw, config->clock_us_style, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_current_room_temperature_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_current_room_temperature_icon_name, config->show_screensaver_inside_temperature, 250);
    break;
  }

  case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITHOUT_BACKGROUND: {
    ESP_LOGD("ScreensaverPage", "Showing screensaver: weather without background");
    Nextion::set_component_value(GUI_SCREENSAVER_PAGE::screensaver_background_control_variable_name, 0, 250);
    Nextion::go_to_page(GUI_SCREENSAVER_PAGE::page_name, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_am_pm_name_raw, config->clock_us_style, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_current_room_temperature_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_current_room_temperature_icon_name, config->show_screensaver_inside_temperature, 250);
    break;
  }

  case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITH_BACKGROUND: {
    ESP_LOGD("ScreensaverPage", "Showing screensaver: minimal with background");
    Nextion::set_component_value(GUI_SCREENSAVER_PAGE::screensaver_minimal_background_control_variable_name, 1, 250);
    Nextion::go_to_page(GUI_SCREENSAVER_PAGE::screensaver_minimal_page_name, 250);
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
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_am_pm_name_raw, config->clock_us_style, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_room_temperature_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_room_temperature_icon_name, config->show_screensaver_inside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_temperature_name, config->show_screensaver_outside_temperature, 250);
    Nextion::set_component_visibility(GUI_SCREENSAVER_PAGE::label_screensaver_minimal_current_weather_icon_name, config->show_screensaver_outside_temperature, 250);
    break;

  case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER:
    ESP_LOGD("ScreensaverPage", "Showing screensaver: no screensaver");
    Nextion::go_to_page(GUI_SCREENSAVER_PAGE::screensaver_minimal_page_name, 250);
    break;
  }

  default:
    ESP_LOGE("ScreensaverPage", "Unknown screensaver mode when showing screensaver!");
    break;
  }

  if (!LvglUi::active()) {
    vTaskDelay(pdMS_TO_TICKS(50)); // Wait for screen to go to page.
  }
  ScreensaverPage::_update_display_brightness();
  ScreensaverPage::_lvgl_render();
}

void ScreensaverPage::_lvgl_render() {
  if (!LvglUi::active() || !ScreensaverPage::showing()) {
    return;
  }
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK || !config) [[unlikely]] {
    return;
  }
  NSPanelConfig__NSPanelScreensaverMode mode = ScreensaverPage::_current_screensaver_mode.get();
  ScreensaverState state;
  state.weather = mode == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITH_BACKGROUND || mode == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITHOUT_BACKGROUND;
  state.background = mode == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__WEATHER_WITH_BACKGROUND || mode == NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__DATETIME_WITH_BACKGROUND;
  // "No screensaver" turns the backlight off (_update_display_brightness()); draw the plain clock behind it.
  state.time = LvglText::utf8(ScreensaverPage::_current_time.get());
  state.am_pm = config->clock_us_style ? LvglText::utf8(ScreensaverPage::_am_pm_string.get()) : "";
  state.date = LvglText::utf8(ScreensaverPage::_current_date.get());
  state.show_inside = config->show_screensaver_inside_temperature;
  // The weather screensaver always shows the outside temperature; the date-time one when asked.
  state.show_outside = state.weather || config->show_screensaver_outside_temperature;
  std::string inside = LvglText::utf8(ScreensaverPage::_current_temperature.get());
  if (!inside.empty() && inside != "--" && inside.find("\xC2\xB0") == std::string::npos) {
    inside += config->use_fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C"; // The HMI drew the unit as an icon.
  }
  state.inside = inside;

  std::shared_ptr<NSPanelWeatherUpdate> weather;
  if (ScreensaverPage::_weather_update_data_mutex != NULL && xSemaphoreTake(ScreensaverPage::_weather_update_data_mutex, pdMS_TO_TICKS(250)) == pdPASS) {
    weather = ScreensaverPage::_weather_update_data;
    xSemaphoreGive(ScreensaverPage::_weather_update_data_mutex);
  }
  if (weather) {
    state.outside_icon = LvglText::weather_icon(weather->current_weather_icon);
    state.outside = LvglText::utf8(weather->current_temperature_string);
    state.max_min = LvglText::utf8(weather->current_maxmin_temperature);
    state.rain = LvglText::utf8(weather->current_precipitation_string);
    state.wind = LvglText::utf8(weather->current_wind_string);
    state.sunrise = LvglText::utf8(weather->sunrise_string);
    state.sunset = LvglText::utf8(weather->sunset_string);
    for (size_t i = 0; i < weather->n_forecast_items && i < (size_t)ScreensaverState::FORECAST_DAYS; i++) {
      const NSPanelWeatherUpdate__ForecastItem *item = weather->forecast_items[i];
      state.forecast[i].day = LvglText::utf8(item->display_string);
      state.forecast[i].icon = LvglText::weather_icon(item->weather_icon);
      state.forecast[i].max_min = LvglText::utf8(item->temperature_maxmin_string);
      state.forecast[i].rain = LvglText::utf8(item->precipitation_string);
      state.forecast[i].wind = LvglText::utf8(item->wind_string);
    }
  }

  // The body's rotation (the clock header stays). Card types this firmware doesn't know are skipped.
  state.base_seconds = config->screensaver_base_card_seconds > 0 ? config->screensaver_base_card_seconds : 10;
  bool calendar_card = false;
  bool energy_card = false;
  // "No screensaver" shows no cards: the backlight is off and nothing rotates.
  for (size_t i = 0; mode != NSPANEL_CONFIG__NSPANEL_SCREENSAVER_MODE__NO_SCREENSAVER && i < config->n_screensaver_cards; i++) {
    const NSPanelConfig__ScreensaverCard *card = config->screensaver_cards[i];
    if (card == NULL) {
      continue;
    }
    uint16_t seconds = card->seconds > 0 ? (uint16_t)std::min<int32_t>(card->seconds, UINT16_MAX) : 10;
    switch (card->type) {
    case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_CARD_TYPE__SCREENSAVER_CARD_CALENDAR:
      state.cards.push_back({ScreensaverCardType::CALENDAR, seconds});
      calendar_card = true;
      break;
    case NSPANEL_CONFIG__NSPANEL_SCREENSAVER_CARD_TYPE__SCREENSAVER_CARD_ENERGY:
      state.cards.push_back({ScreensaverCardType::ENERGY, seconds});
      energy_card = true;
      break;
    default:
      break;
    }
  }
  if (calendar_card) {
    // Decoded already (the MQTT handler's deferred job, or show()): this only formats a locked copy.
    std::shared_ptr<NSPanelCalendarUpdate> calendar = ScreensaverPage::_calendar.get();
    ScreensaverPage::_format_calendar(calendar.get(), config->clock_us_style, state);
  }
  if (energy_card) {
    std::shared_ptr<NSPanelEnergyUpdate> energy = ScreensaverPage::_energy.get();
    state.energy = EnergyFormat::format(ScreensaverPage::_energy_values(energy.get()));
  }
  LvglUi::show(LvglUi::View::SCREENSAVER, ScreensaverView::build, [state]() { ScreensaverView::show(state); });
}

std::string ScreensaverPage::_cards_signature(const NSPanelConfig &config) {
  std::string signature = std::to_string(config.screensaver_base_card_seconds);
  for (size_t i = 0; i < config.n_screensaver_cards; i++) {
    if (config.screensaver_cards[i] != NULL) {
      signature += "," + std::to_string((int)config.screensaver_cards[i]->type) + ":" + std::to_string(config.screensaver_cards[i]->seconds);
    }
  }
  return signature;
}

void ScreensaverPage::_decode_calendar_if_stale() {
  if (ScreensaverPage::_calendar_mutex == NULL || xSemaphoreTake(ScreensaverPage::_calendar_mutex, pdMS_TO_TICKS(1000)) != pdPASS) {
    ESP_LOGW("ScreensaverPage", "Failed to take calendar mutex to decode an update. Will retry on the next one.");
    return;
  }
  // Held through the unpack (a few ms for the <= 4 KB the MQTT buffer allows) so two decoders cannot store
  // an older update after a newer one.
  if (ScreensaverPage::_calendar_stale) {
    ScreensaverPage::_calendar_stale = false;
    NSPanelCalendarUpdate *update = nspanel_calendar_update__unpack(NULL, ScreensaverPage::_calendar_mqtt_data.size(), ScreensaverPage::_calendar_mqtt_data.data());
    if (update != NULL) {
      ESP_LOGI("ScreensaverPage", "Calendar update: %zu event(s), looking %d h ahead.", update->n_events, (int)update->lookahead_hours);
      ScreensaverPage::_calendar.set(std::shared_ptr<NSPanelCalendarUpdate>(update, [](NSPanelCalendarUpdate *data) { nspanel_calendar_update__free_unpacked(data, NULL); }));
    } else {
      ESP_LOGE("ScreensaverPage", "Got a calendar update but failed to decode it. Keeping the previous one.");
    }
  }
  xSemaphoreGive(ScreensaverPage::_calendar_mutex);
}

void ScreensaverPage::_decode_energy_if_stale() {
  if (ScreensaverPage::_energy_mutex == NULL || xSemaphoreTake(ScreensaverPage::_energy_mutex, pdMS_TO_TICKS(1000)) != pdPASS) {
    ESP_LOGW("ScreensaverPage", "Failed to take energy mutex to decode an update. Will retry on the next one.");
    return;
  }
  if (ScreensaverPage::_energy_stale) {
    ScreensaverPage::_energy_stale = false;
    NSPanelEnergyUpdate *update = nspanel_energy_update__unpack(NULL, ScreensaverPage::_energy_mqtt_data.size(), ScreensaverPage::_energy_mqtt_data.data());
    if (update != NULL) {
      // The cost in cents: no float formatting in the log.
      ESP_LOGI("ScreensaverPage", "Energy update: power %d (%ld W), cost %d (%ld cents %s/day), lights %d (%ld, alert: %s).", (int)update->power, (long)update->power_watts, (int)update->cost,
               std::lround(update->cost_per_day * 100), update->currency != NULL ? update->currency : "", (int)update->lights, (long)update->lights_on, update->lights_alert ? "yes" : "no");
      ScreensaverPage::_energy.set(std::shared_ptr<NSPanelEnergyUpdate>(update, [](NSPanelEnergyUpdate *data) { nspanel_energy_update__free_unpacked(data, NULL); }));
    } else {
      ESP_LOGE("ScreensaverPage", "Got an energy update but failed to decode it. Keeping the previous one.");
    }
  }
  xSemaphoreGive(ScreensaverPage::_energy_mutex);
}

EnergyValues ScreensaverPage::_energy_values(const NSPanelEnergyUpdate *update) {
  EnergyValues values;
  if (update == NULL) {
    return values; // Nothing has arrived yet
  }
  values.received = true;
  values.power = (EnergyValues::Availability)update->power;
  values.power_watts = update->power_watts;
  values.cost = (EnergyValues::Availability)update->cost;
  values.cost_per_day = update->cost_per_day;
  values.currency = LvglText::drawable(update->currency != NULL ? update->currency : "");
  std::string symbol = LvglText::utf8(update->currency_symbol != NULL ? update->currency_symbol : "");
  values.currency_symbol = LvglText::drawable(symbol) == symbol ? symbol : "";
  values.lights = (EnergyValues::Availability)update->lights;
  values.lights_on = update->lights_on;
  values.lights_alert = update->lights_alert;
  // A value from a newer manager this firmware doesn't know: treat the part as not set up.
  for (EnergyValues::Availability *availability : {&values.power, &values.cost, &values.lights}) {
    if (*availability > EnergyValues::AVAILABLE) {
      *availability = EnergyValues::NOT_CONFIGURED;
    }
  }
  return values;
}

static bool same_day(const struct tm &a, const struct tm &b) { return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday; }

void ScreensaverPage::_format_calendar(const NSPanelCalendarUpdate *update, bool us_clock, ScreensaverState &state) {
  static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  state.calendar.clear();
  state.empty_text = "No events";
  if (update == NULL) {
    return; // Nothing has arrived yet
  }
  if (update->lookahead_hours == 1) {
    state.empty_text = "No events in the next hour";
  } else if (update->lookahead_hours > 1) {
    state.empty_text = "No events in the next " + std::to_string(update->lookahead_hours) + " hours";
  }

  if (!PanelClock::is_set()) {
    // No wall clock: the manager's own wording, in one group with no heading.
    CalendarDay day;
    for (size_t i = 0; i < update->n_events; i++) {
      const NSPanelCalendarUpdate__Event *event = update->events[i];
      if (event != NULL) {
        day.rows.push_back({LvglText::drawable(event->when_string), LvglText::drawable(event->summary), false});
      }
    }
    if (!day.rows.empty()) {
      state.calendar.push_back(std::move(day));
    }
    return;
  }

  time_t now = time(NULL);
  struct tm today;
  localtime_r(&now, &today);
  struct tm midnight_tm = today;
  midnight_tm.tm_hour = midnight_tm.tm_min = midnight_tm.tm_sec = 0;
  midnight_tm.tm_isdst = -1;
  time_t midnight = mktime(&midnight_tm);
  struct tm tomorrow = midnight_tm;
  tomorrow.tm_mday += 1;
  tomorrow.tm_isdst = -1;
  mktime(&tomorrow); // Normalises the date

  struct tm group_tm = {};
  for (size_t i = 0; i < update->n_events; i++) {
    const NSPanelCalendarUpdate__Event *event = update->events[i];
    if (event == NULL || event->end <= (int64_t)now) {
      continue; // Over
    }
    time_t shown_on = (time_t)std::max<int64_t>(event->start, (int64_t)midnight);
    struct tm day_tm;
    localtime_r(&shown_on, &day_tm);

    CalendarRow row;
    row.summary = LvglText::drawable(event->summary);
    if (event->all_day) {
      row.time = "All day"; // Under way all day: not "Now"
    } else if (event->start <= (int64_t)now) {
      row.time = "Now";
      row.now = true;
    } else {
      struct tm start_tm;
      time_t start = (time_t)event->start;
      localtime_r(&start, &start_tm);
      char buffer[16];
      if (us_clock) {
        snprintf(buffer, sizeof(buffer), "%d:%02d %s", start_tm.tm_hour % 12 == 0 ? 12 : start_tm.tm_hour % 12, start_tm.tm_min, start_tm.tm_hour < 12 ? "AM" : "PM");
      } else {
        snprintf(buffer, sizeof(buffer), "%02d:%02d", start_tm.tm_hour, start_tm.tm_min);
      }
      row.time = buffer;
    }

    if (state.calendar.empty() || !same_day(group_tm, day_tm)) {
      CalendarDay day;
      if (same_day(day_tm, today)) {
        day.heading = "Today";
      } else if (same_day(day_tm, tomorrow)) {
        day.heading = "Tomorrow";
      } else {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%s %d", DAYS[day_tm.tm_wday % 7], day_tm.tm_mday);
        day.heading = buffer;
      }
      state.calendar.push_back(std::move(day));
      group_tm = day_tm;
    }
    state.calendar.back().rows.push_back(std::move(row));
  }
}
