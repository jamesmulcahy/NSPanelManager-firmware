#pragma once
#include <lvgl.h>
#include <memory>
#include <string>

// LVGL replacement for the Nextion `mediaplayer` page (lib/Pages/MediaPlayerPage, NSPanelManager
// PR #408): album art, title and artist, position, transport, volume, and the switch between this
// room's player and the source it is grouped with.

struct MediaPlayerState {
  std::string name;                    // The player: "Lounge"
  std::string title;
  std::string artist;
  std::string album;                   // Not on the Nextion page; shown if the manager sends it
  const lv_image_dsc_t *art = NULL;    // RGB565, ART_SIZE square (the manager scales it); NULL: placeholder
  // Keeps art's pixels valid (an LvglArt::Handle on the panel). The view holds it for as long as it
  // draws art: until a show() with other art, or until the view is deleted.
  std::shared_ptr<const void> art_hold;
  bool playing = false;
  bool muted = false;
  int volume = 0;                      // 0-100, of this player or (controlling_source) the source
  int position = -1;                   // 0-100, -1 when the player reports no position
  std::string elapsed;                 // "1:42", "" if unknown
  std::string duration;                // "4:05"
  bool has_source = false;             // Grouped with a source: show the source switch
  bool controlling_source = false;     // Volume and transport act on the source
  bool can_previous = true, can_next = true, can_play = true, can_volume = true, can_mute = true;
};

class MediaPlayerView {
public:
  // The album art side, as the manager scales it for the panel.
  static constexpr int ART_SIZE = 180;
  enum class Action { BACK, ART /* opens the sources page */, PREVIOUS, PLAY_PAUSE, NEXT, MUTE, VOLUME_DOWN, VOLUME_UP, SOURCE_SWITCH };
  static inline void (*on_action)(Action action) = nullptr;

  static void build(lv_obj_t *parent, bool landscape);
  static void show(const MediaPlayerState &state);

private:
  static inline lv_obj_t *_title = NULL;
  static inline lv_obj_t *_art_box = NULL;
  static inline lv_obj_t *_art = NULL;
  static inline lv_obj_t *_art_placeholder = NULL;
  static inline std::shared_ptr<const void> _art_hold; // Whatever keeps _art's source valid (GUI task)
  static inline lv_obj_t *_song = NULL;
  static inline lv_obj_t *_artist = NULL;
  static inline lv_obj_t *_album = NULL;
  static inline lv_obj_t *_position = NULL;
  static inline lv_obj_t *_elapsed = NULL;
  static inline lv_obj_t *_duration = NULL;
  static inline lv_obj_t *_previous = NULL;
  static inline lv_obj_t *_play = NULL;
  static inline lv_obj_t *_next = NULL;
  static inline lv_obj_t *_source = NULL;
  static inline lv_obj_t *_mute = NULL;
  static inline lv_obj_t *_down = NULL;
  static inline lv_obj_t *_volume = NULL;
  static inline lv_obj_t *_up = NULL;
};
