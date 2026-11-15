#include <AlbumArt.hpp>
#include <EntitiesPage.hpp>
#include <EntityPage.hpp>
#include <GUI_data.hpp>
#include <InterfaceManager.hpp>
#include <LvglArt.hpp>
#include <LvglText.hpp>
#include <LvglUi.hpp>
#include <MediaPlayerView.hpp>
#include <MediaPlayerPage.hpp>
#include <MediaSourcePage.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <Nextion.hpp>
#include <algorithm>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/time.h>

MediaPlayerPage::_component_t MediaPlayerPage::_components[] = {
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::back_button_name, _action::BACK, GUI_MEDIA_PLAYER_CONTROL_PAGE::back_button_id},
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::previous_track_button_name, _action::PREVIOUS_TRACK, GUI_MEDIA_PLAYER_CONTROL_PAGE::previous_track_button_id},
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::next_track_button_name, _action::NEXT_TRACK, GUI_MEDIA_PLAYER_CONTROL_PAGE::next_track_button_id},
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::play_pause_button_name, _action::PLAY_PAUSE, GUI_MEDIA_PLAYER_CONTROL_PAGE::play_pause_button_id},
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::mute_button_name, _action::MUTE, GUI_MEDIA_PLAYER_CONTROL_PAGE::mute_button_id},
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::source_switch_button_name, _action::SOURCE_SWITCH, GUI_MEDIA_PLAYER_CONTROL_PAGE::source_switch_button_id},
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_up_button_name, _action::VOLUME_UP, GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_up_button_id},
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_down_button_name, _action::VOLUME_DOWN, GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_down_button_id},
    {GUI_MEDIA_PLAYER_CONTROL_PAGE::album_art_hotspot_name, _action::ALBUM_ART, GUI_MEDIA_PLAYER_CONTROL_PAGE::album_art_hotspot_id},
};

void MediaPlayerPage::return_to_media_player(std::string state_topic) {
  MediaPlayerPage::_returning = true;
  // Subscribing to the state topic again delivers the retained state, which opens the media player page.
  EntityPage::show(state_topic);
}

void MediaPlayerPage::handle_state(const NSPanelEntityState__MediaPlayer *media_player) {
  MediaPlayerPage::_log_state(media_player);
  MediaPlayerPage::_confirm_pending_volume(media_player);
}

void MediaPlayerPage::unshow() {
  MediaPlayerPage::_showing = false;
  // A full render takes seconds and outlives this page, so stop it rather than let it paint
  // over whatever comes next.
  AlbumArt::cancel();
}

bool MediaPlayerPage::_on_screen() {
  // The media player is shown as an entity page, so its page is recorded as EntityPage's.
  return MediaPlayerPage::_showing && InterfaceManager::is_current_page(EntityPage::unshow);
}

void MediaPlayerPage::_redraw() {
  if (!MediaPlayerPage::_on_screen()) {
    return;
  }
  std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
  if (state != nullptr && state->entity_case == NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) {
    MediaPlayerPage::update_display(state->media_player, false);
  }
}

const char *MediaPlayerPage::_playback_state_name(NSPanelEntityState__MediaPlayer__PlaybackState state) {
  switch (state) {
  case NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__OFF:
    return "Off";
  case NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__ON:
    return "On";
  case NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__IDLE:
    return "Idle";
  case NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PLAYING:
    return "Playing";
  case NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PAUSED:
    return "Paused";
  case NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__BUFFERING:
    return "Buffering";
  default:
    return "";
  }
}

void MediaPlayerPage::_log_state(const NSPanelEntityState__MediaPlayer *media_player) {
  if (media_player == NULL) [[unlikely]] {
    ESP_LOGE("MediaPlayerPage", "Media player state case set but no media player payload.");
    return;
  }

  // The whole decode path runs before anything touches the display, so this verifies the protobuf
  // round trip even on a panel whose TFT has no media player page.
  ESP_LOGD("MediaPlayerPage", "Media player %ld '%s': %s", media_player->media_player_id, media_player->name != NULL ? media_player->name : "(no name)", MediaPlayerPage::_playback_state_name(media_player->state));
  ESP_LOGD("MediaPlayerPage", "  title '%s' artist '%s'", media_player->media_title != NULL ? media_player->media_title : "", media_player->media_artist != NULL ? media_player->media_artist : "");
  ESP_LOGD("MediaPlayerPage", "  volume %ld, muted %s, source volume %ld (present: %s)", media_player->volume, media_player->is_muted ? "yes" : "no", media_player->source_volume, media_player->has_source_volume ? "yes" : "no");
  ESP_LOGD("MediaPlayerPage", "  can: play %d pause %d next %d prev %d set_volume %d mute %d", media_player->can_play, media_player->can_pause, media_player->can_next_track, media_player->can_previous_track, media_player->can_set_volume, media_player->can_mute);
  ESP_LOGD("MediaPlayerPage", "  album art: %s", media_player->album_art_url != NULL && media_player->album_art_url[0] != '\0' ? media_player->album_art_url : "(none)");
  if (media_player->player_sources != NULL) {
    ESP_LOGD("MediaPlayerPage", "  player sources: %u, current '%s', can select %d", (unsigned)media_player->player_sources->n_sources, media_player->player_sources->current_source, media_player->player_sources->can_select);
  }
  if (media_player->source_player_sources != NULL) {
    ESP_LOGD("MediaPlayerPage", "  source player sources: %u, current '%s', can select %d", (unsigned)media_player->source_player_sources->n_sources, media_player->source_player_sources->current_source, media_player->source_player_sources->can_select);
  }
  if (media_player->has_media_position) {
    ESP_LOGD("MediaPlayerPage", "  position %lds of %lds at %lld", media_player->media_position, media_player->media_duration, media_player->media_position_updated_at);
  }
}

void MediaPlayerPage::update_display(const NSPanelEntityState__MediaPlayer *media_player, bool page_just_opened) {
  if (!InterfaceManager::is_current_page(EntityPage::unshow)) {
    // Another page has been shown, but EntityPage still receives state until its unshow() runs.
    // Every write would land on the other page, the album art included.
    ESP_LOGD("MediaPlayerPage", "Not updating the display: the media player page is no longer shown.");
    return;
  }

  if (page_just_opened) {
    MediaPlayerPage::_showing = true;
    if (LvglUi::active()) {
      MediaPlayerView::on_action = [](MediaPlayerView::Action action) {
        LvglUi::defer([action]() { MediaPlayerPage::_lvgl_action((int)action); });
      };
    } else {
      MediaPlayerPage::_resolve_component_ids();
    }

    MediaPlayerPage::_position_bar_value = -2;
    if (MediaPlayerPage::_position_task_handle == NULL) {
      if (xTaskCreatePinnedToCore(MediaPlayerPage::_task_position, "media_position", 3072, NULL, 2, &MediaPlayerPage::_position_task_handle, 1) != pdPASS) [[unlikely]] {
        ESP_LOGE("MediaPlayerPage", "Failed to create the media player position task. The position bar won't advance while playing.");
        MediaPlayerPage::_position_task_handle = NULL;
      }
    }
    Nextion::set_timer_value(GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_bar_timer_name, MediaPlayerPage::_volume_bar_timeout_ms, 250);
    if (!MediaPlayerPage::_returning.exchange(false)) {
      MediaPlayerPage::_controlling_source = false;
    }
    taskENTER_CRITICAL(&MediaPlayerPage::_spinlock);
    MediaPlayerPage::_pending_volume.volume = -1; // A press from the last time the page was open no longer counts.
    taskEXIT_CRITICAL(&MediaPlayerPage::_spinlock);
  }

  bool is_playing = media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PLAYING ||
                    media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__BUFFERING;
  // Fall back to the player if the source has gone away while it was selected.
  bool controlling_source = MediaPlayerPage::_controlling_source && media_player->has_source_volume;

  Nextion::set_component_text(GUI_MEDIA_PLAYER_CONTROL_PAGE::title_label_name, media_player->media_title, 1000);
  Nextion::set_component_text(GUI_MEDIA_PLAYER_CONTROL_PAGE::artist_label_name, media_player->media_artist, 1000);

  // Icons follow the player's state rather than the press, so the page stays right when something
  // else controls the player.
  Nextion::set_component_text(GUI_MEDIA_PLAYER_CONTROL_PAGE::play_pause_button_name, is_playing ? GUI_MEDIA_ICONS::pause : GUI_MEDIA_ICONS::play, 250);
  // Buttons the player can't use now are greyed out rather than hidden, so the layout stays put.
  for (const _component_t &component : MediaPlayerPage::_components) {
    switch (component.action) {
    case _action::PREVIOUS_TRACK:
    case _action::NEXT_TRACK:
    case _action::PLAY_PAUSE:
    case _action::MUTE:
    case _action::VOLUME_UP:
    case _action::VOLUME_DOWN:
      MediaPlayerPage::_set_button_enabled(component.name, MediaPlayerPage::_action_enabled(component.action, media_player, controlling_source));
      break;
    default:
      break;
    }
  }

  // The switch shows what the volume controls act on now: this room's player, or the source feeding it.
  Nextion::set_component_text(GUI_MEDIA_PLAYER_CONTROL_PAGE::source_switch_button_name, controlling_source ? GUI_MEDIA_ICONS::speaker_multiple : GUI_MEDIA_ICONS::speaker, 250);
  Nextion::set_component_visibility(GUI_MEDIA_PLAYER_CONTROL_PAGE::source_switch_button_name, media_player->has_source_volume, 250);

  Nextion::set_component_text(GUI_MEDIA_PLAYER_CONTROL_PAGE::mute_button_name, media_player->is_muted ? GUI_MEDIA_ICONS::volume_mute : GUI_MEDIA_ICONS::volume_high, 250);
  // Only the value: the bar is shown briefly when the volume buttons are pressed, not on every update.
  Nextion::set_component_value(GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_bar_name, MediaPlayerPage::_volume(media_player, controlling_source), 250);

  MediaPlayerPage::_update_position_bar(media_player);
  // The task advances the bar while playing, and sleeps otherwise, so let it look at the new state.
  if (MediaPlayerPage::_position_task_handle != NULL) {
    xTaskNotifyGive(MediaPlayerPage::_position_task_handle);
  }

  // Draw the art when the page opens and whenever it changes, for example on the next track. A new
  // render replaces one still in progress, and unshow() cancels one that outlives the page.
  // Start it last: a render streams thousands of fills, and the writes above, made on the MQTT task,
  // would otherwise wait their turn behind them on every track change.
  {
    std::string album_art_url = media_player->album_art_url != NULL ? media_player->album_art_url : "";
    std::lock_guard<std::mutex> lock_guard(MediaPlayerPage::_album_art_url_mutex);
    if (LvglUi::active()) {
      // Whole, in the background (LvglArt); the page shows the placeholder meanwhile.
      MediaPlayerPage::_album_art_url = album_art_url;
      if (album_art_url != MediaPlayerPage::_lvgl_art_url) {
        MediaPlayerPage::_lvgl_art = nullptr;
        MediaPlayerPage::_lvgl_art_url.clear();
        if (!album_art_url.empty()) {
          LvglArt::fetch(album_art_url, MediaPlayerPage::_lvgl_art_done);
        } else {
          LvglArt::cancel();
        }
      }
    } else if (page_just_opened || album_art_url != MediaPlayerPage::_album_art_url) {
      MediaPlayerPage::_album_art_url = album_art_url;
      if (!album_art_url.empty()) {
        AlbumArt::render(album_art_url, MediaPlayerPage::_album_art_x, MediaPlayerPage::_album_art_y, MediaPlayerPage::_on_screen);
      } else if (page_just_opened) {
        AlbumArt::cancel(); // Nothing drawn on this page yet.
      } else {
        AlbumArt::clear(); // Remove the last track's art, or what was drawn of it.
      }
    }
  }
  MediaPlayerPage::_lvgl_render(media_player, controlling_source);
}

int32_t MediaPlayerPage::_position_percent(const NSPanelEntityState__MediaPlayer *media_player) {
  if (!media_player->has_media_position || media_player->media_duration <= 0) {
    return -1; // A duration of 0 is unknown, as for a live stream.
  }

  int64_t position = media_player->media_position;
  // The manager only sends the position when playback starts, stops or seeks, with the time it was
  // taken. While playing, advance it by the time since then. The state is retained, so it may have
  // arrived long after it was sent, and the time since it arrived would not do.
  if (media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PLAYING) {
    struct timeval now;
    gettimeofday(&now, NULL);
    if (now.tv_sec < MediaPlayerPage::_min_wall_clock) {
      return -1;
    }
    // Clocks a little apart could put the update in the future.
    position += std::max<int64_t>(0, now.tv_sec - media_player->media_position_updated_at);
  }

  position = std::clamp<int64_t>(position, 0, media_player->media_duration);
  return position * 100 / media_player->media_duration;
}

void MediaPlayerPage::_update_position_bar(const NSPanelEntityState__MediaPlayer *media_player) {
  int32_t value = MediaPlayerPage::_position_percent(media_player);
  int32_t shown = MediaPlayerPage::_position_bar_value.exchange(value);
  if (value == shown) {
    return;
  }

  if (value < 0) {
    Nextion::set_component_visibility(GUI_MEDIA_PLAYER_CONTROL_PAGE::position_bar_name, false, 250);
    return;
  }
  // Set the value before showing it, so a hidden bar doesn't flash its old value.
  Nextion::set_component_value(GUI_MEDIA_PLAYER_CONTROL_PAGE::position_bar_name, value, 250);
  if (shown < 0) {
    Nextion::set_component_visibility(GUI_MEDIA_PLAYER_CONTROL_PAGE::position_bar_name, true, 250);
  }
}

void MediaPlayerPage::_task_position(void *param) {
  for (;;) {
    bool playing = false;
    if (MediaPlayerPage::_on_screen()) {
      std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
      if (state != nullptr && state->entity_case == NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) {
        playing = state->media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PLAYING;
        if (playing && LvglUi::active()) {
          MediaPlayerPage::_lvgl_render(state->media_player, MediaPlayerPage::_controlling_source && state->media_player->has_source_volume);
        } else if (playing) {
          MediaPlayerPage::_update_position_bar(state->media_player);
        }
      }
    }
    // The bar moves 1% at a time, so it only reaches the display every few seconds for most tracks.
    ulTaskNotifyTake(pdTRUE, playing ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
  }
}

bool MediaPlayerPage::_action_enabled(_action action, const NSPanelEntityState__MediaPlayer *media_player, bool controlling_source) {
  switch (action) {
  case _action::PREVIOUS_TRACK:
    return media_player->can_previous_track;
  case _action::NEXT_TRACK:
    return media_player->can_next_track;
  case _action::PLAY_PAUSE: {
    bool is_playing = media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PLAYING ||
                      media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__BUFFERING;
    return is_playing ? media_player->can_pause : media_player->can_play;
  }
  case _action::MUTE:
    // The command can mute only the player, so there is no mute while controlling the source.
    return !controlling_source && media_player->can_mute;
  case _action::VOLUME_UP:
  case _action::VOLUME_DOWN:
    return controlling_source || media_player->can_set_volume;
  default:
    return true;
  }
}

void MediaPlayerPage::_set_button_enabled(const char *button_name, bool enabled) {
  Nextion::set_component_foreground(button_name, enabled ? GUI_MEDIA_PLAYER_CONTROL_PAGE::button_pco : GUI_MEDIA_PLAYER_CONTROL_PAGE::button_disabled_pco, 250);
  Nextion::set_component_pco2(button_name, enabled ? GUI_MEDIA_PLAYER_CONTROL_PAGE::button_pressed_pco2 : GUI_MEDIA_PLAYER_CONTROL_PAGE::button_disabled_pco, 250);
}

void MediaPlayerPage::handle_touch(uint16_t component_id, bool pressed) {
  ESP_LOGD("MediaPlayerPage", "Touch component %d, pressed %s", component_id, pressed ? "Yes" : "No");
  if (pressed) {
    return; // The page sends release events only.
  }

  for (const _component_t &component : MediaPlayerPage::_components) {
    if (component.id != component_id) {
      continue;
    }

    // The button is greyed out, but the Nextion still reports presses.
    std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
    if (state != nullptr && state->entity_case == NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) {
      bool controlling_source = MediaPlayerPage::_controlling_source && state->media_player->has_source_volume;
      if (!MediaPlayerPage::_action_enabled(component.action, state->media_player, controlling_source)) {
        ESP_LOGD("MediaPlayerPage", "Ignoring a press on %s, which the media player can't do now.", component.name);
        return;
      }
    }

    switch (component.action) {
    case _action::BACK:
      ESP_LOGD("MediaPlayerPage", "Received touch event to go back.");
      EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
      break;

    case _action::PLAY_PAUSE:
      MediaPlayerPage::_play_pause();
      break;

    case _action::PREVIOUS_TRACK:
      MediaPlayerPage::_previous_track();
      break;

    case _action::NEXT_TRACK:
      MediaPlayerPage::_next_track();
      break;

    case _action::MUTE:
      MediaPlayerPage::_toggle_mute();
      break;

    case _action::SOURCE_SWITCH:
      MediaPlayerPage::_toggle_source();
      break;

    case _action::VOLUME_UP:
      MediaPlayerPage::_step_volume(1);
      break;

    case _action::VOLUME_DOWN:
      MediaPlayerPage::_step_volume(-1);
      break;

    case _action::ALBUM_ART: {
      if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
        break;
      }
      // The source list follows the source switch, like the volume buttons.
      bool source_player = MediaPlayerPage::_controlling_source && state->media_player->has_source_volume;
      const NSPanelEntityState__MediaPlayer__SourceSelection *sources = MediaPlayerPage::_selectable_sources(state->media_player, source_player);
      if (sources == NULL) {
        ESP_LOGD("MediaPlayerPage", "No %s sources to select from.", source_player ? "source player" : "player");
        break;
      }
      MediaSourcePage::show(EntityPage::get_current_state_topic(), source_player, sources);
      break;
    }
    }
    return;
  }
}

void MediaPlayerPage::_resolve_component_ids() {
  if (MediaPlayerPage::_component_ids_resolved) {
    return;
  }

  bool all_resolved = true;
  std::string resolved_ids;
  for (_component_t &component : MediaPlayerPage::_components) {
    int32_t id = -1;
    if (Nextion::get_component_integer_attribute(component.name, "id", &id, 250, 250) == ESP_OK && id >= 0 && id <= UINT8_MAX) {
      component.id = id;
    } else {
      // Keep the ID from GUI_data.hpp and try again the next time the page opens.
      ESP_LOGE("MediaPlayerPage", "Failed to look up the ID of media player component %s, will use %d.", component.name, component.id.load());
      all_resolved = false;
    }
    resolved_ids.append(" ").append(component.name).append("=").append(std::to_string(component.id.load()));
  }
  ESP_LOGI("MediaPlayerPage", "Media player component IDs:%s", resolved_ids.c_str());

  int32_t x = -1;
  int32_t y = -1;
  if (Nextion::get_component_integer_attribute(GUI_MEDIA_PLAYER_CONTROL_PAGE::album_art_hotspot_name, "x", &x, 250, 250) == ESP_OK &&
      Nextion::get_component_integer_attribute(GUI_MEDIA_PLAYER_CONTROL_PAGE::album_art_hotspot_name, "y", &y, 250, 250) == ESP_OK &&
      x >= 0 && x <= UINT16_MAX && y >= 0 && y <= UINT16_MAX) {
    MediaPlayerPage::_album_art_x = x;
    MediaPlayerPage::_album_art_y = y;
    ESP_LOGI("MediaPlayerPage", "Album art at %ld,%ld.", x, y);
  } else {
    ESP_LOGE("MediaPlayerPage", "Failed to read the position of %s, will draw the album art at %d,%d.", GUI_MEDIA_PLAYER_CONTROL_PAGE::album_art_hotspot_name,
             MediaPlayerPage::_album_art_x.load(), MediaPlayerPage::_album_art_y.load());
    all_resolved = false;
  }
  MediaPlayerPage::_component_ids_resolved = all_resolved;
}

void MediaPlayerPage::_play_pause() {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
    ESP_LOGE("MediaPlayerPage", "Tried to play/pause without a media player state.");
    return;
  }

  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  if (state->media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PLAYING || state->media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__BUFFERING) {
    command.playback_action = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__PLAYBACK_ACTION__PAUSE;
  } else {
    command.playback_action = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__PLAYBACK_ACTION__PLAY;
  }
  MediaPlayerPage::_send_command(&command);
}

void MediaPlayerPage::_next_track() {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.playback_action = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__PLAYBACK_ACTION__NEXT_TRACK;
  MediaPlayerPage::_send_command(&command);
}

void MediaPlayerPage::_previous_track() {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.playback_action = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__PLAYBACK_ACTION__PREVIOUS_TRACK;
  MediaPlayerPage::_send_command(&command);
}

void MediaPlayerPage::_toggle_mute() {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
    ESP_LOGE("MediaPlayerPage", "Tried to toggle mute without a media player state.");
    return;
  }

  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.has_muted = true;
  command.muted = !state->media_player->is_muted;
  MediaPlayerPage::_send_command(&command);
}

void MediaPlayerPage::_set_volume(int32_t volume) {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.has_volume = true;
  command.volume = volume;
  MediaPlayerPage::_send_command(&command);
}

void MediaPlayerPage::_set_source_volume(int32_t volume) {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  command.has_source_volume = true;
  command.source_volume = volume;
  MediaPlayerPage::_send_command(&command);
}

void MediaPlayerPage::_step_volume(int32_t direction) {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
    ESP_LOGE("MediaPlayerPage", "Tried to change volume without a media player state.");
    return;
  }
  NSPanelEntityState__MediaPlayer *media_player = state->media_player;

  bool controlling_source = MediaPlayerPage::_controlling_source && media_player->has_source_volume;
  // A manager from before volume_step was added sends 0.
  int32_t step = media_player->volume_step > 0 ? media_player->volume_step : MediaPlayerPage::_default_volume_step;
  // Step from the pending volume if there is one, so quick presses compound instead of each
  // starting from a state that has not caught up yet.
  int32_t volume = std::clamp<int32_t>(MediaPlayerPage::_volume(media_player, controlling_source) + direction * step, 0, 100);

  taskENTER_CRITICAL(&MediaPlayerPage::_spinlock);
  MediaPlayerPage::_pending_volume = {
      .volume = volume,
      .source = controlling_source,
      .requested_at = esp_timer_get_time(),
  };
  taskEXIT_CRITICAL(&MediaPlayerPage::_spinlock);

  if (controlling_source) {
    MediaPlayerPage::_set_source_volume(volume);
  } else {
    MediaPlayerPage::_set_volume(volume);
  }
  MediaPlayerPage::_show_volume_bar(volume);
}

int32_t MediaPlayerPage::_volume(const NSPanelEntityState__MediaPlayer *media_player, bool source) {
  int32_t volume = source ? media_player->source_volume : media_player->volume;

  taskENTER_CRITICAL(&MediaPlayerPage::_spinlock);
  const _pending_volume_t pending = MediaPlayerPage::_pending_volume;
  taskEXIT_CRITICAL(&MediaPlayerPage::_spinlock);

  if (pending.volume >= 0 && pending.source == source && esp_timer_get_time() - pending.requested_at < MediaPlayerPage::_pending_volume_timeout_us) {
    volume = pending.volume;
  }
  return volume;
}

void MediaPlayerPage::_confirm_pending_volume(const NSPanelEntityState__MediaPlayer *media_player) {
  if (media_player == NULL) [[unlikely]] {
    return;
  }

  // Only the state for the last press confirms it. States for earlier presses in a quick run arrive
  // in between, and dropping the pending volume on those would make the bar jump back.
  taskENTER_CRITICAL(&MediaPlayerPage::_spinlock);
  _pending_volume_t &pending = MediaPlayerPage::_pending_volume;
  if (pending.volume >= 0 && pending.volume == (pending.source ? media_player->source_volume : media_player->volume)) {
    pending.volume = -1;
  }
  taskEXIT_CRITICAL(&MediaPlayerPage::_spinlock);
}

const NSPanelEntityState__MediaPlayer__SourceSelection *MediaPlayerPage::_selectable_sources(const NSPanelEntityState__MediaPlayer *media_player, bool controlling_source) {
  const NSPanelEntityState__MediaPlayer__SourceSelection *sources = controlling_source ? media_player->source_player_sources : media_player->player_sources;
  if (sources == NULL || sources->n_sources == 0 || !sources->can_select) {
    return NULL;
  }
  return sources;
}

void MediaPlayerPage::select_source(const std::string &source, bool source_player) {
  NSPanelMQTTManagerCommand__MediaPlayerCommand command = NSPANEL_MQTTMANAGER_COMMAND__MEDIA_PLAYER_COMMAND__INIT;
  if (source_player) {
    command.select_source_player_source = (char *)source.c_str();
  } else {
    command.select_player_source = (char *)source.c_str();
  }
  MediaPlayerPage::_send_command(&command);
}

void MediaPlayerPage::_toggle_source() {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
    ESP_LOGE("MediaPlayerPage", "Tried to switch to the source without a media player state.");
    return;
  }
  if (!state->media_player->has_source_volume) {
    return; // The switch is hidden, but a press can race the state update that hid it.
  }

  MediaPlayerPage::_controlling_source = !MediaPlayerPage::_controlling_source;
  ESP_LOGD("MediaPlayerPage", "Volume controls now act on the %s.", MediaPlayerPage::_controlling_source ? "source" : "player");
  MediaPlayerPage::_redraw();
  // Show the volume the controls now act on, as a volume press does.
  MediaPlayerPage::_show_volume_bar(MediaPlayerPage::_volume(state->media_player, MediaPlayerPage::_controlling_source));
}

void MediaPlayerPage::_show_volume_bar(int32_t volume) {
  if (LvglUi::active()) {
    MediaPlayerPage::_redraw(); // The LVGL page always shows the volume.
    return;
  }
  Nextion::set_component_value(GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_bar_name, volume, 250);
  Nextion::set_component_visibility(GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_bar_name, true, 250);

  // The timer hides the bar. Stop it before starting it so that every press restarts the countdown,
  // and holding the buttons keeps the bar up.
  std::string timer = GUI_MEDIA_PLAYER_CONTROL_PAGE::volume_bar_timer_name;
  Nextion::send_raw_command((timer + ".en=0").c_str(), 250);
  Nextion::send_raw_command((timer + ".en=1").c_str(), 250);
}

void MediaPlayerPage::_send_command(NSPanelMQTTManagerCommand__MediaPlayerCommand *command) {
  std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_MEDIA_PLAYER) [[unlikely]] {
    ESP_LOGE("MediaPlayerPage", "Tried to send media player command without a media player state.");
    return;
  }
  command->media_player_id = state->media_player->media_player_id;

  NSPanelMQTTManagerCommand cmd = NSPANEL_MQTTMANAGER_COMMAND__INIT;
  cmd.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_MEDIA_PLAYER_COMMAND;
  cmd.media_player_command = command;
  cmd.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

  uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&cmd);
  std::vector<uint8_t> buffer(packed_length); // Use vector for automatic cleanup of data when going out of scope
  size_t packed_data_size = nspanel_mqttmanager_command__pack(&cmd, buffer.data());
  if (packed_data_size == packed_length) [[likely]] {
    if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) [[unlikely]] {
      ESP_LOGE("MediaPlayerPage", "Failed to send MQTT message with command payload.");
      MediaPlayerPage::_redraw(); // Update display to reset values to those stored
    }
  } else {
    ESP_LOGE("MediaPlayerPage", "Failed to pack protobuf command.");
    MediaPlayerPage::_redraw(); // Update display to reset values to those stored
  }
}

// Seconds as "m:ss", or "h:mm:ss".
static std::string clock_text(int64_t seconds) {
  char text[16];
  if (seconds >= 3600) {
    snprintf(text, sizeof(text), "%d:%02d:%02d", (int)(seconds / 3600), (int)(seconds / 60 % 60), (int)(seconds % 60));
  } else {
    snprintf(text, sizeof(text), "%d:%02d", (int)(seconds / 60), (int)(seconds % 60));
  }
  return text;
}

void MediaPlayerPage::_lvgl_render(const NSPanelEntityState__MediaPlayer *media_player, bool controlling_source) {
  if (!LvglUi::active() || media_player == NULL || !MediaPlayerPage::_on_screen()) {
    return;
  }
  MediaPlayerState state;
  state.name = LvglText::utf8(media_player->name);
  state.title = LvglText::utf8(media_player->media_title);
  state.artist = LvglText::utf8(media_player->media_artist);
  {
    std::lock_guard<std::mutex> lock_guard(MediaPlayerPage::_album_art_url_mutex);
    state.art = MediaPlayerPage::_lvgl_art;
  }
  state.playing = media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__PLAYING ||
                  media_player->state == NSPANEL_ENTITY_STATE__MEDIA_PLAYER__PLAYBACK_STATE__BUFFERING;
  state.muted = media_player->is_muted;
  state.volume = MediaPlayerPage::_volume(media_player, controlling_source);
  state.position = MediaPlayerPage::_position_percent(media_player);
  if (state.position >= 0) {
    state.elapsed = clock_text((int64_t)state.position * media_player->media_duration / 100);
    state.duration = clock_text(media_player->media_duration);
  }
  state.has_source = media_player->has_source_volume;
  state.controlling_source = controlling_source;
  state.can_previous = MediaPlayerPage::_action_enabled(_action::PREVIOUS_TRACK, media_player, controlling_source);
  state.can_next = MediaPlayerPage::_action_enabled(_action::NEXT_TRACK, media_player, controlling_source);
  state.can_play = MediaPlayerPage::_action_enabled(_action::PLAY_PAUSE, media_player, controlling_source);
  state.can_volume = MediaPlayerPage::_action_enabled(_action::VOLUME_UP, media_player, controlling_source);
  state.can_mute = MediaPlayerPage::_action_enabled(_action::MUTE, media_player, controlling_source);
  LvglUi::show(LvglUi::View::MEDIA_PLAYER, MediaPlayerView::build, [state]() { MediaPlayerView::show(state); });
}

void MediaPlayerPage::_lvgl_art_done(const lv_image_dsc_t *image, const std::string &url) {
  {
    std::lock_guard<std::mutex> lock_guard(MediaPlayerPage::_album_art_url_mutex);
    if (url != MediaPlayerPage::_album_art_url) {
      return; // The track changed meanwhile; its own fetch is on the way.
    }
    MediaPlayerPage::_lvgl_art = image;
    MediaPlayerPage::_lvgl_art_url = image != NULL ? url : "";
  }
  MediaPlayerPage::_redraw();
}

void MediaPlayerPage::_lvgl_action(int action) {
  if (!MediaPlayerPage::_on_screen()) {
    return;
  }
  _action wanted;
  switch ((MediaPlayerView::Action)action) {
  case MediaPlayerView::Action::BACK: wanted = _action::BACK; break;
  case MediaPlayerView::Action::ART: wanted = _action::ALBUM_ART; break;
  case MediaPlayerView::Action::PREVIOUS: wanted = _action::PREVIOUS_TRACK; break;
  case MediaPlayerView::Action::PLAY_PAUSE: wanted = _action::PLAY_PAUSE; break;
  case MediaPlayerView::Action::NEXT: wanted = _action::NEXT_TRACK; break;
  case MediaPlayerView::Action::MUTE: wanted = _action::MUTE; break;
  case MediaPlayerView::Action::VOLUME_DOWN: wanted = _action::VOLUME_DOWN; break;
  case MediaPlayerView::Action::VOLUME_UP: wanted = _action::VOLUME_UP; break;
  case MediaPlayerView::Action::SOURCE_SWITCH: wanted = _action::SOURCE_SWITCH; break;
  default: return;
  }
  // The same path as a release of the Nextion component (enabled check included).
  for (const _component_t &component : MediaPlayerPage::_components) {
    if (component.action == wanted) {
      MediaPlayerPage::handle_touch(component.id, false);
      return;
    }
  }
}
