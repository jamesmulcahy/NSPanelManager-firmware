#include <BuzzerManager.hpp>
#include <ConfigManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <NSPM_ConfigManager_event.hpp>
#include <Nextion_event.hpp>
#include <RtttlParser.hpp>
#include <WiFiManager.hpp>
#include <esp_log.h>

void BuzzerManager::init() {
  esp_log_level_set("BuzzerManager", ConfigManager::log_level);

#if defined(BOARD_SONOFF)
  BuzzerManager::_playback_mutex = xSemaphoreCreateMutex();
  if (BuzzerManager::_playback_mutex == NULL) [[unlikely]] {
    ESP_LOGE("BuzzerManager", "Failed to create buzzer playback mutex.");
    return;
  }

  BuzzerManager::_sounds_mutex = xSemaphoreCreateMutex();
  if (BuzzerManager::_sounds_mutex == NULL) [[unlikely]] {
    ESP_LOGE("BuzzerManager", "Failed to create buzzer sounds mutex.");
    return;
  }

  ledc_timer_config_t timer_config = {};
  timer_config.speed_mode = BuzzerManager::_ledc_mode;
  timer_config.duty_resolution = BuzzerManager::_ledc_resolution;
  timer_config.timer_num = BuzzerManager::_ledc_timer;
  timer_config.freq_hz = BuzzerManager::_ledc_initial_frequency_hz;
  timer_config.clk_cfg = LEDC_AUTO_CLK;
  esp_err_t result = ledc_timer_config(&timer_config);
  if (result != ESP_OK) [[unlikely]] {
    ESP_LOGE("BuzzerManager", "Failed to configure LEDC timer for buzzer. Got error: %s", esp_err_to_name(result));
    return;
  }

  ledc_channel_config_t channel_config = {};
  channel_config.gpio_num = BuzzerManager::_buzzer_pin;
  channel_config.speed_mode = BuzzerManager::_ledc_mode;
  channel_config.channel = BuzzerManager::_ledc_channel;
  channel_config.intr_type = LEDC_INTR_DISABLE;
  channel_config.timer_sel = BuzzerManager::_ledc_timer;
  channel_config.duty = 0; // Silent until a sound is requested
  channel_config.hpoint = 0;
  result = ledc_channel_config(&channel_config);
  if (result != ESP_OK) [[unlikely]] {
    ESP_LOGE("BuzzerManager", "Failed to configure LEDC channel for buzzer. Got error: %s", esp_err_to_name(result));
    return;
  }

  esp_timer_create_args_t step_timer_args = {};
  step_timer_args.callback = BuzzerManager::_step_timer_callback;
  step_timer_args.dispatch_method = ESP_TIMER_TASK;
  step_timer_args.name = "buzzer_step";
  result = esp_timer_create(&step_timer_args, &BuzzerManager::_step_timer);
  if (result != ESP_OK) [[unlikely]] {
    ESP_LOGE("BuzzerManager", "Failed to create buzzer step timer. Got error: %s", esp_err_to_name(result));
    return;
  }

  BuzzerManager::_initialized = true;
  esp_event_handler_register(NEXTION_EVENT, nextion_event_t::TOUCH_EVENT, BuzzerManager::_nextion_event_handler, NULL);
  esp_event_handler_register(NSPM_CONFIGMANAGER_EVENT, nspm_configmanager_event::CONFIG_LOADED, BuzzerManager::_config_event_handler, NULL);
  ESP_LOGI("BuzzerManager", "Buzzer initialized on GPIO %d.", BuzzerManager::_buzzer_pin);
#else
  // TODO: Enable once it is known whether the custom PCB has a buzzer, see _buzzer_pin in BuzzerManager.hpp
  ESP_LOGI("BuzzerManager", "No buzzer defined for this board, buzzer is disabled.");
#endif
}

void BuzzerManager::init_mqtt() {
  if (!BuzzerManager::_initialized) {
    return;
  }

  BuzzerManager::_raw_command_topic = std::string("nspanel/") + WiFiManager::mac_string() + "/buzzer_raw_command";
  MqttManager::register_handler(MQTT_EVENT_DATA, &BuzzerManager::_mqtt_event_handler, NULL);
  MqttManager::subscribe(BuzzerManager::_raw_command_topic);
}

void BuzzerManager::play(std::shared_ptr<const std::vector<buzzer_tone_step_t>> steps) {
  if (!BuzzerManager::_initialized) [[unlikely]] {
    return;
  }

  if (xSemaphoreTake(BuzzerManager::_playback_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    esp_timer_stop(BuzzerManager::_step_timer); // ESP_ERR_INVALID_STATE if nothing is playing is fine.
    BuzzerManager::_steps = std::move(steps); // Frees the previous sequence unless someone else still holds it
    BuzzerManager::_current_step = 0;
    BuzzerManager::_start_current_step();
    xSemaphoreGive(BuzzerManager::_playback_mutex);
  } else {
    ESP_LOGW("BuzzerManager", "Failed to take playback mutex, skipping sound.");
  }
}

void BuzzerManager::stop() {
  BuzzerManager::play(nullptr);
}

void BuzzerManager::_step_timer_callback(void *arg) {
  if (xSemaphoreTake(BuzzerManager::_playback_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    // play() may have restarted playback while this callback waited for the mutex
    if (esp_timer_get_time() >= BuzzerManager::_current_step_end_us) {
      BuzzerManager::_current_step++;
      BuzzerManager::_start_current_step();
    }
    xSemaphoreGive(BuzzerManager::_playback_mutex);
  } else {
    ESP_LOGW("BuzzerManager", "Failed to take playback mutex while advancing sound.");
  }
}

void BuzzerManager::_start_current_step() {
  if (!BuzzerManager::_steps || BuzzerManager::_current_step >= BuzzerManager::_steps->size()) {
    BuzzerManager::_set_output(0);
    BuzzerManager::_steps.reset();
    return;
  }

  const buzzer_tone_step_t &step = (*BuzzerManager::_steps)[BuzzerManager::_current_step];
  BuzzerManager::_set_output(step.frequency_hz);
  uint64_t duration_us = static_cast<uint64_t>(step.duration_ms) * 1000;
  BuzzerManager::_current_step_end_us = esp_timer_get_time() + duration_us;
  esp_timer_start_once(BuzzerManager::_step_timer, duration_us);
}

void BuzzerManager::_set_output(uint32_t frequency_hz) {
  if (frequency_hz > 0) {
    esp_err_t result = ledc_set_freq(BuzzerManager::_ledc_mode, BuzzerManager::_ledc_timer, frequency_hz);
    if (result != ESP_OK) [[unlikely]] {
      ESP_LOGE("BuzzerManager", "Failed to set buzzer frequency to %lu Hz. Got error: %s", frequency_hz, esp_err_to_name(result));
      frequency_hz = 0; // Stay silent for this step rather than hold the previous tone
    }
  }
  ledc_set_duty(BuzzerManager::_ledc_mode, BuzzerManager::_ledc_channel, frequency_hz > 0 ? BuzzerManager::_ledc_duty_on : 0);
  ledc_update_duty(BuzzerManager::_ledc_mode, BuzzerManager::_ledc_channel);
}

void BuzzerManager::raise_event(buzzer_event_t event) {
  if (!BuzzerManager::_initialized) [[unlikely]] {
    return;
  }

  BuzzerManager::_handle_event(event);
}

void BuzzerManager::raise_event_for_touch(buzzer_event_t event) {
  if (!BuzzerManager::_initialized) [[unlikely]] {
    return; // The TOUCH_EVENT handler that clears _touch_event_replaced is not registered
  }

  BuzzerManager::_touch_event_replaced = BuzzerManager::_handle_event(event);
}

void BuzzerManager::_nextion_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  nextion_event_touch_t *data = (nextion_event_touch_t *)event_data;
  bool raise_touch_event = false;
  if (data->pressed) {
    BuzzerManager::_awaiting_release = true;
    BuzzerManager::_pressed_page = data->page_number;
    BuzzerManager::_pressed_component = data->component_id;
    raise_touch_event = true;
  } else if (BuzzerManager::_awaiting_release && BuzzerManager::_pressed_page == data->page_number && BuzzerManager::_pressed_component == data->component_id) {
    BuzzerManager::_awaiting_release = false; // Already raised when this component was pressed
  } else {
    // Most components in the NSPanel Manager TFT only send release events
    BuzzerManager::_awaiting_release = false;
    raise_touch_event = true;
  }

  // A page handler already raised a more specific event for this touch, see raise_event_for_touch()
  if (raise_touch_event && !BuzzerManager::_touch_event_replaced) {
    BuzzerManager::_handle_event(buzzer_event_t::TOUCH);
  }
  BuzzerManager::_touch_event_replaced = false;
}

bool BuzzerManager::_handle_event(buzzer_event_t event) {
  switch (event) {
  case buzzer_event_t::TOUCH:
    ESP_LOGD("BuzzerManager", "Got TOUCH event.");
    break;
  case buzzer_event_t::SCENE_ACTIVATED:
    ESP_LOGD("BuzzerManager", "Got SCENE_ACTIVATED event.");
    break;
  case buzzer_event_t::SCENE_SAVED:
    ESP_LOGD("BuzzerManager", "Got SCENE_SAVED event.");
    break;
  default:
    return false;
  }

  std::shared_ptr<const std::vector<buzzer_tone_step_t>> steps;
  if (xSemaphoreTake(BuzzerManager::_sounds_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    steps = BuzzerManager::_sounds[static_cast<size_t>(event)];
    xSemaphoreGive(BuzzerManager::_sounds_mutex);
  } else {
    ESP_LOGW("BuzzerManager", "Failed to take sounds mutex, skipping sound.");
    return false;
  }

  if (!steps) {
    return false;
  }
  BuzzerManager::play(std::move(steps));
  return true;
}

void BuzzerManager::_config_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  std::shared_ptr<NSPanelConfig> config;
  if (NSPM_ConfigManager::get_config(&config) != ESP_OK) {
    ESP_LOGE("BuzzerManager", "Failed to get config, keeping the current sounds.");
    return;
  }

  // Parse into a new table outside the mutex so events raised meanwhile still find the current sounds
  decltype(BuzzerManager::_sounds) sounds;
  for (size_t i = 0; i < config->n_sounds; i++) {
    const NSPanelConfig__Sound *sound = config->sounds[i];
    buzzer_event_t event;
    switch (sound->event) {
    case NSPANEL_CONFIG__NSPANEL_SOUND_EVENT__TOUCH:
      event = buzzer_event_t::TOUCH;
      break;
    case NSPANEL_CONFIG__NSPANEL_SOUND_EVENT__SCENE_ACTIVATED:
      event = buzzer_event_t::SCENE_ACTIVATED;
      break;
    case NSPANEL_CONFIG__NSPANEL_SOUND_EVENT__SCENE_SAVED:
      event = buzzer_event_t::SCENE_SAVED;
      break;
    default:
      ESP_LOGD("BuzzerManager", "Ignoring sound for unknown event %d.", sound->event);
      continue;
    }

    if (sound->rtttl == NULL || sound->rtttl[0] == '\0') {
      sounds[static_cast<size_t>(event)].reset(); // No sound for this event
      continue;
    }

    auto steps = std::make_shared<std::vector<buzzer_tone_step_t>>();
    esp_err_t result = RtttlParser::parse(sound->rtttl, steps.get());
    if (result == ESP_OK) {
      sounds[static_cast<size_t>(event)] = std::move(steps);
    } else if (result == ESP_ERR_INVALID_SIZE) {
      ESP_LOGW("BuzzerManager", "RTTTL string for event %d is longer than %zu steps or %lu ms, it will have no sound: %s", sound->event, RtttlParser::max_steps, RtttlParser::max_total_duration_ms, sound->rtttl);
      sounds[static_cast<size_t>(event)].reset();
    } else {
      ESP_LOGW("BuzzerManager", "Failed to parse RTTTL string for event %d, it will have no sound: %s", sound->event, sound->rtttl);
      sounds[static_cast<size_t>(event)].reset();
    }
  }

  if (xSemaphoreTake(BuzzerManager::_sounds_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    BuzzerManager::_sounds = std::move(sounds);
    xSemaphoreGive(BuzzerManager::_sounds_mutex);
    ESP_LOGI("BuzzerManager", "Updated buzzer sounds, config has %zu sound entries.", config->n_sounds);
  } else {
    ESP_LOGE("BuzzerManager", "Failed to take sounds mutex, keeping the current sounds.");
  }
}

void BuzzerManager::_mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  if (event->topic_len <= 0 || BuzzerManager::_raw_command_topic.compare(0, std::string::npos, event->topic, event->topic_len) != 0) {
    return;
  }

  if (event->retain) {
    ESP_LOGW("BuzzerManager", "Ignoring retained message on %s. Publish sounds without the retain flag.", BuzzerManager::_raw_command_topic.c_str());
    return;
  }

  if (event->total_data_len != event->data_len) {
    ESP_LOGE("BuzzerManager", "RTTTL string of %d bytes is too long to receive in one MQTT message, ignoring it.", event->total_data_len);
    return;
  }

  if (event->data_len == 0) {
    ESP_LOGD("BuzzerManager", "Got empty raw buzzer command, stopping playback.");
    BuzzerManager::stop();
    return;
  }

  // Parsing is quick and play() does not block on playback, so this is safe in the MQTT event handler
  std::string rtttl(event->data, event->data_len);
  auto steps = std::make_shared<std::vector<buzzer_tone_step_t>>();
  esp_err_t result = RtttlParser::parse(rtttl, steps.get());
  if (result == ESP_OK) {
    ESP_LOGD("BuzzerManager", "Playing %zu steps from raw buzzer command.", steps->size());
    BuzzerManager::play(std::move(steps));
  } else if (result == ESP_ERR_INVALID_SIZE) {
    ESP_LOGE("BuzzerManager", "RTTTL string is longer than %zu steps or %lu ms, ignoring it: %s", RtttlParser::max_steps, RtttlParser::max_total_duration_ms, rtttl.c_str());
  } else {
    ESP_LOGE("BuzzerManager", "Failed to parse RTTTL string, ignoring it: %s", rtttl.c_str());
  }
}
