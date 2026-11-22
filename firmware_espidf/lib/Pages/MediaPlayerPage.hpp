#pragma once
#include <lvgl.h>

#include <LvglArt.hpp>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <mutex>
#include <protobuf_nspanel.pb-c.h>
#include <protobuf_nspanel_entity.pb-c.h>
#include <string>

/*
 * The media player page: title and artist, playback and volume controls, and the position bar,
 * with the album art drawn by LvglArt, which opens MediaSourcePage when pressed.
 *
 * EntityPage follows the player's state topic and holds its state, as for every entity, and
 * switches to the page. This class draws the page from that state and handles its touches.
 */
class MediaPlayerPage {
public:
  /**
   * Handle a new state for the media player: log it, and confirm the pending volume if it carries it.
   */
  static void handle_state(const NSPanelEntityState__MediaPlayer *media_player);

  /**
   * Draw the page from the given state.
   * @param page_just_opened: EntityPage has just switched to the page, so set up what lasts while it is open.
   */
  static void update_display(const NSPanelEntityState__MediaPlayer *media_player, bool page_just_opened);

  /**
   * The page has closed.
   */
  static void unshow();

  /**
   * Show the media player page again after a page opened from it, such as MediaSourcePage. Unlike
   * EntityPage::show(), this keeps what the volume controls and source list act on, the player or its source.
   */
  static void return_to_media_player(std::string state_topic);

  /**
   * Select a source, by name, for the media player last shown, from MediaSourcePage.
   * @param source_player: Select it on the source player feeding the media player rather than on the player itself.
   */
  static void select_source(const std::string &source, bool source_player);

private:
  /**
   * Draw the page again from the current state, if it is showing.
   */
  static void _redraw();

  /*
   * Human readable name for a playback state, for the log.
   */
  static const char *_playback_state_name(NSPanelEntityState__MediaPlayer__PlaybackState state);

  /*
   * Log a decoded media player state at debug level.
   */
  static void _log_state(const NSPanelEntityState__MediaPlayer *media_player);

  /*
   * Commands for the currently displayed media player. Decisions that depend on the
   * current state (play or pause, mute or unmute) are made from the last state received.
   */
  static void _play_pause();
  static void _next_track();
  static void _previous_track();
  static void _toggle_mute();
  static void _set_volume(int32_t volume);
  static void _set_source_volume(int32_t volume);

  /*
   * Volume up (direction 1) or down (-1) by the player's volume_step, from the volume -/+ buttons.
   * Sets a pending volume that the page shows, and steps from, until the state confirms it.
   */
  static void _step_volume(int32_t direction);

  /*
   * The volume to show and step from for the player, or for its source: the pending volume if one is
   * outstanding for that target, otherwise the state's.
   */
  static int32_t _volume(const NSPanelEntityState__MediaPlayer *media_player, bool source);

  /*
   * Clear the pending volume if the given state carries it.
   */
  static void _confirm_pending_volume(const NSPanelEntityState__MediaPlayer *media_player);

  /*
   * The source list the album art opens: the source player's while the controls act on it, otherwise
   * the player's own. NULL if that list is missing, empty or can't be selected from.
   */
  static const NSPanelEntityState__MediaPlayer__SourceSelection *_selectable_sources(const NSPanelEntityState__MediaPlayer *media_player, bool controlling_source);

  /*
   * Switch the volume controls between the player and the source feeding it, from b_srcswitch.
   */
  static void _toggle_source();

  /*
   * The position bar's value, 0-100, for the media player's position now, or -1 if there is none to
   * show: no position, an unknown duration, or a playing player and no wall clock to advance it by.
   */
  static int32_t _position_percent(const NSPanelEntityState__MediaPlayer *media_player);

  /*
   * Advances the position bar about once a second while the media player page is showing and the
   * player is playing. Sleeps otherwise, until update_display() wakes it.
   */
  static void _task_position(void *param);

  // Send a command for the currently displayed media player to the manager. Fills in the media player ID.
  static void _send_command(NSPanelMQTTManagerCommand__MediaPlayerCommand *command);

  /**
   * With LVGL as the UI (LvglUi): draw the page with MediaPlayerView. Any task.
   */
  static void _lvgl_render(const NSPanelEntityState__MediaPlayer *media_player, bool controlling_source);

  /**
   * A MediaPlayerView action (as int, keeping LVGL out of this header), on LvglUi's task: what a
   * press of the matching button does.
   */
  static void _lvgl_action(int action);

  /**
   * LvglArt's result for `url`, on its task: keep it if it is still this page's art, and redraw.
   */
  static void _lvgl_art_done(LvglArt::Handle image, const std::string &url);

  // Whether the entity page last opened as this media player: set when it opens and cleared by
  // unshow(). unshow() runs late on the unshow task, and not at all while that is held up, so this
  // alone doesn't say the page is on screen. Use _on_screen() for that.
  static inline std::atomic<bool> _showing = false;

  // Whether this page is what the display shows now, as InterfaceManager records it.
  static bool _on_screen();

  // Whether the volume controls act on the source feeding the player rather than the player itself.
  // Reset to the player whenever the page opens.
  static inline std::atomic<bool> _controlling_source = false;
  // Set by return_to_media_player() so that opening the page keeps _controlling_source.
  static inline std::atomic<bool> _returning = false;
  // Percent per press of the volume -/+ buttons when the manager sends no volume_step. Matches Home
  // Assistant's default.
  static constexpr int32_t _default_volume_step = 10;
  // The volume the last -/+ press asked for. Volume presses are optimistic: the page shows, and steps
  // from, this volume before Home Assistant confirms it, so that quick presses add up. Only the last
  // press is kept, and only for the volume: mute and play/pause decide from the last state.
  //  - _step_volume() sets it and sends the command.
  //  - _volume() returns it in place of the state's volume while it is pending.
  //  - _confirm_pending_volume() clears it when a state carries exactly that volume. States for
  //    earlier presses in a quick run don't, so the bar doesn't jump back while Home Assistant
  //    catches up.
  //  - It lapses after _pending_volume_timeout_us, and opening the page clears it.
  // It is for the player or for its source, whichever the controls acted on, and is ignored while they
  // act on the other.
  // A press that changes nothing, + at 100 say, stays pending until it lapses, as no new state comes;
  // the two volumes are equal, so that is harmless. Nothing redraws when it lapses, but the volume bar
  // has hidden itself by then.
  // Guarded by _spinlock: written from the GUI task, read from MQTT too.
  struct _pending_volume_t {
    int32_t volume;       // -1 when nothing is pending
    bool source;          // Whether it is for the source rather than the player
    int64_t requested_at; // esp_timer_get_time() of the press
  };
  static inline _pending_volume_t _pending_volume = {.volume = -1, .source = false, .requested_at = 0};
  // How long a pending volume waits for its state before the state wins: in case the manager rounds
  // it differently, or the player ignores it.
  static constexpr int64_t _pending_volume_timeout_us = 5 * 1000 * 1000;
  // What a touch on a media player component does.
  enum class _action {
    BACK,
    PREVIOUS_TRACK,
    NEXT_TRACK,
    PLAY_PAUSE,
    MUTE,
    SOURCE_SWITCH,
    VOLUME_UP,
    VOLUME_DOWN,
    ALBUM_ART,
  };
  // Whether the media player can do what action does now. Buttons it can't are greyed out, and
  // their presses ignored.
  static bool _action_enabled(_action action, const NSPanelEntityState__MediaPlayer *media_player, bool controlling_source);
  // Does what a touch on a media player button does. name is only for the log.
  static void _perform(_action action, const char *name);
  static inline TaskHandle_t _position_task_handle = NULL;
  // Wall clock times before this (2026-01-01 UTC) mean the panel's clock hasn't been set yet. A playing
  // player's position can't be advanced without it, so the bar is hidden until it is.
  // TODO: Use PanelClock::is_set() once the panel sets its clock from the manager.
  static constexpr int64_t _min_wall_clock = 1767225600;

  // The album art URL last handed to LvglArt, so that new art is drawn when it changes, for example on
  // the next track. Guarded by _album_art_url_mutex: the page is updated from the MQTT
  // task and from the GUI task.
  static inline std::string _album_art_url;
  static inline std::mutex _album_art_url_mutex;
  // LVGL: a hold on the art for _album_art_url, from LvglArt's cache; empty while it is on its way, or
  // if there is none (placeholder). Shared with the states handed to MediaPlayerView, which keep it
  // until the view draws other art. Guarded by _album_art_url_mutex.
  static inline std::shared_ptr<const LvglArt::Handle> _lvgl_art;

  static inline portMUX_TYPE _spinlock = portMUX_INITIALIZER_UNLOCKED;
};
