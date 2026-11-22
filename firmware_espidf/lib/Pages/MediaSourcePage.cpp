#include <ConfigManager.hpp>
#include <EntitiesView.hpp>
#include <InterfaceManager.hpp>
#include <LvglText.hpp>
#include <LvglUi.hpp>
#include <MediaPlayerPage.hpp>
#include <MediaSourcePage.hpp>
#include <esp_log.h>

void MediaSourcePage::show(std::string media_player_state_topic, bool source_player, const NSPanelEntityState__MediaPlayer__SourceSelection *sources) {
  esp_log_level_set("MediaSourcePage", ConfigManager::log_level);

  // Checked before begin_page(), so a call with nothing to list leaves the current page as it is.
  if (sources == NULL || sources->n_sources == 0 || sources->sources == NULL) [[unlikely]] {
    ESP_LOGW("MediaSourcePage", "No sources to show.");
    return;
  }

  InterfaceManager::begin_page(MediaSourcePage::unshow);

  MediaSourcePage::_media_player_state_topic = media_player_state_topic;
  MediaSourcePage::_showing_source_player = source_player;
  MediaSourcePage::_sources.assign(sources->sources, sources->sources + sources->n_sources);
  MediaSourcePage::_current_source = sources->current_source != NULL ? sources->current_source : "";
  MediaSourcePage::_first_source_index = 0;

  // items12 is never used: its labels are only 97 px wide, two columns, so source names are cut short.
  // More than eight sources page through items8 instead.
  MediaSourcePage::_page_type = MediaSourcePage::_sources.size() <= 4 ? 4 : 8;

  if (LvglUi::active()) {
    EntitiesView::on_action = [](EntitiesView::Action action, int32_t slot) {
      LvglUi::defer([action, slot]() { MediaSourcePage::_lvgl_action((int)action, slot); });
    };
  }
  MediaSourcePage::_update_display();
}

void MediaSourcePage::unshow() {
  MediaSourcePage::_page_type = 0;
}

void MediaSourcePage::_update_display() {
  if (LvglUi::active()) {
    if (MediaSourcePage::_page_type == 0) {
      return;
    }
    // EntitiesView with text-only slots, the current source in amber.
    EntitiesState state;
    state.header = MediaSourcePage::_showing_source_player ? "Favorites" : "Input";
    state.size = MediaSourcePage::_page_type;
    state.can_prev = MediaSourcePage::_first_source_index > 0;
    state.can_next = MediaSourcePage::_first_source_index + MediaSourcePage::_page_type < MediaSourcePage::_sources.size();
    for (size_t slot = 0; slot < MediaSourcePage::_page_type && slot < (size_t)EntitiesState::MAX_SLOTS; slot++) {
      size_t index = MediaSourcePage::_first_source_index + slot;
      if (index < MediaSourcePage::_sources.size()) {
        state.slots[slot].empty = false;
        state.slots[slot].name = LvglText::utf8(MediaSourcePage::_sources[index]);
        state.slots[slot].on = MediaSourcePage::_sources[index] == MediaSourcePage::_current_source;
      }
    }
    LvglUi::show(LvglUi::View::ENTITIES, EntitiesView::build, [state]() { EntitiesView::show(state); });
  }
}

void MediaSourcePage::_select_source(size_t source_index) {
  // By name rather than index: the lists change when the player switches source, so an index could
  // select another source than the one pressed by the time the manager acts on it.
  const std::string &source = MediaSourcePage::_sources[source_index];
  ESP_LOGI("MediaSourcePage", "Selected %s source '%s'.", MediaSourcePage::_showing_source_player ? "source player" : "player", source.c_str());
  MediaPlayerPage::select_source(source, MediaSourcePage::_showing_source_player);
  MediaSourcePage::_return_to_media_player();
}

void MediaSourcePage::_return_to_media_player() {
  MediaPlayerPage::return_to_media_player(MediaSourcePage::_media_player_state_topic);
}

void MediaSourcePage::_lvgl_action(int action, int32_t slot) {
  if (MediaSourcePage::_page_type == 0) {
    return; // Left the page before the action got here.
  }
  switch ((EntitiesView::Action)action) {
  case EntitiesView::Action::SLOT: {
    size_t index = MediaSourcePage::_first_source_index + slot;
    if (slot >= 0 && index < MediaSourcePage::_sources.size()) {
      MediaSourcePage::_select_source(index);
    }
    break;
  }
  case EntitiesView::Action::BACK:
    MediaSourcePage::_return_to_media_player();
    break;
  case EntitiesView::Action::PREV:
    if (MediaSourcePage::_first_source_index >= MediaSourcePage::_page_type) {
      MediaSourcePage::_first_source_index -= MediaSourcePage::_page_type;
      MediaSourcePage::_update_display();
    }
    break;
  case EntitiesView::Action::NEXT:
    if (MediaSourcePage::_first_source_index + MediaSourcePage::_page_type < MediaSourcePage::_sources.size()) {
      MediaSourcePage::_first_source_index += MediaSourcePage::_page_type;
      MediaSourcePage::_update_display();
    }
    break;
  default:
    break;
  }
}
