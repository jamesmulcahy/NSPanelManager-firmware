#include "MediaPlayerView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

static void clicked(lv_event_t *event) {
  if (MediaPlayerView::on_action != nullptr) {
    MediaPlayerView::on_action((MediaPlayerView::Action)(intptr_t)lv_event_get_user_data(event));
  }
}

static lv_obj_t *button(lv_obj_t *parent, const char *glyph, int size, MediaPlayerView::Action action) {
  lv_obj_t *b = Ui::icon_button(parent, glyph, size);
  lv_obj_add_event_cb(b, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)action);
  return b;
}

static lv_obj_t *bar(lv_obj_t *parent, lv_color_t color) {
  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_height(b, 6);
  lv_obj_set_style_radius(b, Theme::RADIUS_BAR, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, Theme::track(), LV_PART_MAIN);
  lv_obj_set_style_radius(b, Theme::RADIUS_BAR, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
  return b;
}

static lv_obj_t *line(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = Theme::label(parent, "", font, color);
  lv_obj_set_width(label, lv_pct(100));
  // Dots, not LVGL's scrolling modes: a scrolling title redraws (and is sent as fills) forever.
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
  return label;
}

void MediaPlayerView::build(lv_obj_t *parent, bool landscape) {
  Ui::Header header = Ui::header(parent, true, false);
  MediaPlayerView::_title = header.title;
  lv_obj_add_event_cb(header.back, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)Action::BACK);
  MediaPlayerView::_source = button(header.right, NSPM_ICON_SPEAKER, 24, Action::SOURCE_SWITCH);
  Ui::reach_screen_edges(MediaPlayerView::_source, 24, 10); // As the header's own buttons

  // The art beside everything else on a landscape screen, above it on a portrait one.
  lv_obj_t *body = Ui::fill(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(body, landscape ? Theme::SPACE_L : Theme::SPACE_M, 0);

  lv_obj_t *art = lv_obj_create(body);
  lv_obj_remove_style_all(art);
  lv_obj_remove_flag(art, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(art, ART_SIZE, ART_SIZE);
  lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(art, Theme::card(), 0);
  // Rounded as the cards are, behind the placeholder. Art covers the box and stays square: clipping
  // its corners (clip_corner) would mask every pixel of it.
  lv_obj_set_style_radius(art, Theme::RADIUS_CARD, 0);
  lv_obj_add_flag(art, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(art, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)Action::ART);
  MediaPlayerView::_art_box = art;
  MediaPlayerView::_art = lv_image_create(art);
  lv_obj_center(MediaPlayerView::_art);
  // Nothing draws the art once the image is gone, so its pixels may go too.
  MediaPlayerView::_art_hold.reset();
  lv_obj_add_event_cb(MediaPlayerView::_art, [](lv_event_t *) { MediaPlayerView::_art_hold.reset(); }, LV_EVENT_DELETE, NULL);
  MediaPlayerView::_art_placeholder = Ui::icon(art, NSPM_ICON_MUSIC, 64, Theme::muted());
  lv_obj_center(MediaPlayerView::_art_placeholder);

  lv_obj_t *info = Theme::group(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_grow(info, 1);
  lv_obj_set_flex_align(info, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(info, Theme::SPACE_S, 0);
  if (landscape) {
    lv_obj_set_height(info, lv_pct(100));
  } else {
    lv_obj_set_width(info, lv_pct(100));
  }
  lv_obj_t *text = Theme::group(info, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(text, lv_pct(100));
  lv_obj_set_style_pad_gap(text, Theme::SPACE_XS, 0);
  MediaPlayerView::_song = line(text, Theme::title_font(), Theme::text());
  MediaPlayerView::_artist = line(text, Theme::body_font(), Theme::text());
  MediaPlayerView::_album = line(text, Theme::body_font(), Theme::muted());
  if (!landscape) {
    for (lv_obj_t *label : {MediaPlayerView::_song, MediaPlayerView::_artist, MediaPlayerView::_album}) {
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
  }

  lv_obj_t *position = Theme::group(info, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_width(position, lv_pct(100));
  lv_obj_set_style_pad_row(position, Theme::SPACE_XS, 0);
  MediaPlayerView::_position = bar(position, Theme::indicator());
  lv_obj_set_width(MediaPlayerView::_position, lv_pct(100));
  MediaPlayerView::_elapsed = Theme::label(position, "", Theme::body_font(), Theme::muted());
  lv_obj_set_flex_grow(MediaPlayerView::_elapsed, 1);
  MediaPlayerView::_duration = Theme::label(position, "", Theme::body_font(), Theme::muted());

  lv_obj_t *transport = Theme::group(info, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(transport, lv_pct(100));
  lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  MediaPlayerView::_previous = button(transport, NSPM_ICON_SKIP_PREVIOUS, 40, Action::PREVIOUS);
  MediaPlayerView::_play = button(transport, NSPM_ICON_PLAY, 40, Action::PLAY_PAUSE);
  lv_obj_set_size(MediaPlayerView::_play, 64, 64);
  lv_obj_set_style_radius(MediaPlayerView::_play, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(MediaPlayerView::_play, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(MediaPlayerView::_play, Theme::track(), 0);
  MediaPlayerView::_next = button(transport, NSPM_ICON_SKIP_NEXT, 40, Action::NEXT);

  lv_obj_t *volume = Theme::card(info);
  lv_obj_set_size(volume, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(volume, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(volume, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(volume, Theme::SPACE_XS, 0);
  lv_obj_set_style_pad_gap(volume, Theme::SPACE_XS, 0);
  // Big targets: on Lounge, most taps meant for a 44x36 mute landed beside it (2026-11-15). Mute is
  // filled, as play is, so it reads as a button; it and the +/- buttons reach the screen's edges
  // (Ui::reach_screen_edges, from icon_button) where they are near one.
  MediaPlayerView::_mute = button(volume, NSPM_ICON_VOLUME_HIGH, 24, Action::MUTE);
  lv_obj_set_size(MediaPlayerView::_mute, 72, 52);
  lv_obj_set_style_bg_opa(MediaPlayerView::_mute, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(MediaPlayerView::_mute, Theme::track(), 0);
  MediaPlayerView::_down = button(volume, NSPM_ICON_MINUS, 24, Action::VOLUME_DOWN);
  lv_obj_set_size(MediaPlayerView::_down, 56, 52);
  // Always shown, unlike the Nextion page's p_volume, which appears for a few seconds on a change
  // (there is room for it here, and a bar that comes and goes costs two redraws per change).
  MediaPlayerView::_volume = bar(volume, Theme::on());
  lv_obj_set_flex_grow(MediaPlayerView::_volume, 1);
  MediaPlayerView::_up = button(volume, NSPM_ICON_PLUS, 24, Action::VOLUME_UP);
  lv_obj_set_size(MediaPlayerView::_up, 56, 52);
}

void MediaPlayerView::show(const MediaPlayerState &state) {
  Ui::set_text(MediaPlayerView::_title, state.name);
  Ui::set_visible(MediaPlayerView::_source, state.has_source);
  Ui::set_icon(MediaPlayerView::_source, state.controlling_source ? NSPM_ICON_SPEAKER_MULTIPLE : NSPM_ICON_SPEAKER);

  if (lv_image_get_src(MediaPlayerView::_art) != state.art) {
    lv_image_set_src(MediaPlayerView::_art, state.art);
  }
  // Only now that the image draws from the new art can the old art's pixels be let go.
  MediaPlayerView::_art_hold = state.art_hold;
  Ui::set_visible(MediaPlayerView::_art, state.art != NULL);
  Ui::set_visible(MediaPlayerView::_art_placeholder, state.art == NULL);

  Ui::set_text(MediaPlayerView::_song, state.title);
  Ui::set_text(MediaPlayerView::_artist, state.artist);
  Ui::set_text(MediaPlayerView::_album, state.album);
  Ui::set_visible(MediaPlayerView::_album, !state.album.empty());

  bool has_position = state.position >= 0;
  Ui::set_visible(MediaPlayerView::_position, has_position);
  Ui::set_visible(MediaPlayerView::_elapsed, has_position);
  Ui::set_visible(MediaPlayerView::_duration, has_position);
  if (has_position) {
    Ui::set_bar(MediaPlayerView::_position, state.position);
  }
  Ui::set_text(MediaPlayerView::_elapsed, state.elapsed);
  Ui::set_text(MediaPlayerView::_duration, state.duration);

  Ui::set_icon(MediaPlayerView::_play, state.playing ? NSPM_ICON_PAUSE : NSPM_ICON_PLAY);
  Ui::set_icon(MediaPlayerView::_mute, state.muted ? NSPM_ICON_VOLUME_OFF : NSPM_ICON_VOLUME_HIGH);
  // Greyed out when the player can't do it now, as MediaPlayerPage does.
  Ui::set_enabled(MediaPlayerView::_previous, state.can_previous);
  Ui::set_enabled(MediaPlayerView::_next, state.can_next);
  Ui::set_enabled(MediaPlayerView::_play, state.can_play);
  Ui::set_enabled(MediaPlayerView::_mute, state.can_mute);
  Ui::set_enabled(MediaPlayerView::_down, state.can_volume);
  Ui::set_enabled(MediaPlayerView::_up, state.can_volume);
  Ui::set_bar(MediaPlayerView::_volume, state.volume);
}
