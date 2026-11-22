#include <ConfigManager.hpp>
#include <GUI_data.hpp>
#include <EntitiesView.hpp>
#include <InterfaceManager.hpp>
#include <LvglText.hpp>
#include <LvglUi.hpp>
#include <MediaPlayerPage.hpp>
#include <MediaSourcePage.hpp>
#include <Nextion.hpp>
#include <Nextion_event.hpp>
#include <esp_log.h>

void MediaSourcePage::show(std::string media_player_state_topic, bool source_player, const NSPanelEntityState__MediaPlayer__SourceSelection *sources) {
  esp_log_level_set("MediaSourcePage", ConfigManager::log_level);

  // Checked before begin_page(), so a call with nothing to list leaves the current page as it is.
  if (sources == NULL || sources->n_sources == 0 || sources->sources == NULL) [[unlikely]] {
    ESP_LOGW("MediaSourcePage", "No sources to show.");
    return;
  }

  std::shared_ptr<PageHandlers> handlers = InterfaceManager::begin_page(MediaSourcePage::unshow);

  MediaSourcePage::_media_player_state_topic = media_player_state_topic;
  MediaSourcePage::_showing_source_player = source_player;
  MediaSourcePage::_sources.assign(sources->sources, sources->sources + sources->n_sources);
  MediaSourcePage::_current_source = sources->current_source != NULL ? sources->current_source : "";
  MediaSourcePage::_first_source_index = 0;

  // items12 is never used: its labels are only 97 px wide, two columns, so source names are cut short.
  // More than eight sources page through items8 instead.
  MediaSourcePage::_page_type = MediaSourcePage::_sources.size() <= 4 ? 4 : 8;

  handlers->add(NEXTION_EVENT, ESP_EVENT_ANY_ID, &MediaSourcePage::_handle_nextion_event);
  if (LvglUi::active()) {
    EntitiesView::on_action = [](EntitiesView::Action action, int32_t slot) {
      LvglUi::defer([action, slot]() { MediaSourcePage::_lvgl_action((int)action, slot); });
    };
  }
  MediaSourcePage::_update_display();
}

void MediaSourcePage::unshow() {
  // The Nextion handler is removed by InterfaceManager (PageHandlers::retire()).
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
    return;
  }
  switch (MediaSourcePage::_page_type) {
  case 4:
    InterfaceManager::go_to_hmi_page(GUI_ITEMS4_PAGE::page_name, 1000);
    break;
  case 8:
    InterfaceManager::go_to_hmi_page(GUI_ITEMS8_PAGE::page_name, 1000);
    break;
  case 12:
    InterfaceManager::go_to_hmi_page(GUI_ITEMS12_PAGE::page_name, 1000);
    break;
  default:
    return;
  }

  Nextion::set_component_text(GUI_ITEMS_PAGE_COMMON::page_header_label, MediaSourcePage::_showing_source_player ? "Favorites" : "Input", 250);
  for (size_t slot = 0; slot < MediaSourcePage::_page_type; slot++) {
    size_t source_index = MediaSourcePage::_first_source_index + slot;
    if (MediaSourcePage::_page_type == 12) {
      MediaSourcePage::_update_displayed_source_in_slot(source_index, GUI_ITEMS12_PAGE::item_slots[slot]);
    } else if (MediaSourcePage::_page_type == 8) {
      MediaSourcePage::_update_displayed_source_in_slot(source_index, GUI_ITEMS8_PAGE::item_slots[slot]);
    } else {
      MediaSourcePage::_update_displayed_source_in_slot(source_index, GUI_ITEMS4_PAGE::item_slots[slot]);
    }
  }
}

void MediaSourcePage::_update_displayed_source_in_slot(size_t source_index, const GUI_ITEMS_PAGE_ITEM_DATA page_slot) {
  if (source_index < MediaSourcePage::_sources.size()) {
    const std::string &source = MediaSourcePage::_sources[source_index];
    bool is_current = source == MediaSourcePage::_current_source;
    // No indent, unlike EntitiesPage: the labels take at most 24 characters, and source names are long.
    Nextion::set_component_text(page_slot.label_name, source.c_str(), 250);
    // The name of the current source in the "on" color of the items pages' icons, the others in the labels' white.
    Nextion::set_component_foreground(page_slot.label_name, is_current ? GUI_ITEMS_PAGE_COMMON::items_button_on_pco : GUI_ITEMS_PAGE_COMMON::items_button_off_pco, 250);
    // No icon: the items pages' buttons use an icon font with only entity icons, none of them a source.
    Nextion::set_component_visibility(page_slot.button_name, false, 250);
    Nextion::set_component_visibility(page_slot.label_name, true, 250);
    vTaskDelay(pdMS_TO_TICKS(50)); // As EntitiesPage does between slots, so the display keeps up.
  } else {
    Nextion::set_component_visibility(page_slot.button_name, false, 250);
    Nextion::set_component_visibility(page_slot.label_name, false, 250);
  }
}

void MediaSourcePage::_handle_nextion_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id != nextion_event_t::TOUCH_EVENT) {
    return;
  }

  nextion_event_touch_t *touch_data = (nextion_event_touch_t *)event_data;
  switch (MediaSourcePage::_page_type) {
  case 4:
    MediaSourcePage::_handle_touch_event(touch_data, GUI_ITEMS4_PAGE::button_back_id, GUI_ITEMS4_PAGE::button_previous_page_id, GUI_ITEMS4_PAGE::button_next_page_id, GUI_ITEMS4_PAGE::item_slots);
    break;
  case 8:
    MediaSourcePage::_handle_touch_event(touch_data, GUI_ITEMS8_PAGE::button_back_id, GUI_ITEMS8_PAGE::button_previous_page_id, GUI_ITEMS8_PAGE::button_next_page_id, GUI_ITEMS8_PAGE::item_slots);
    break;
  case 12:
    MediaSourcePage::_handle_touch_event(touch_data, GUI_ITEMS12_PAGE::button_back_id, GUI_ITEMS12_PAGE::button_previous_page_id, GUI_ITEMS12_PAGE::button_next_page_id, GUI_ITEMS12_PAGE::item_slots);
    break;
  default:
    break; // Not showing.
  }
}

void MediaSourcePage::_handle_touch_event(nextion_event_touch_t *touch_data, uint8_t button_back_id, uint8_t button_previous_page_id, uint8_t button_next_page_id, const GUI_ITEMS_PAGE_ITEM_DATA *item_slots) {
  ESP_LOGD("MediaSourcePage", "Touch component %d, pressed %s", touch_data->component_id, touch_data->pressed ? "Yes" : "No");
  if (!touch_data->pressed) {
    return; // The items pages act on press, like EntitiesPage.
  }

  // A source is picked by its name; the slot's button is hidden.
  for (size_t slot = 0; slot < MediaSourcePage::_page_type; slot++) {
    if (touch_data->component_id == item_slots[slot].label_id) {
      size_t source_index = MediaSourcePage::_first_source_index + slot;
      if (source_index < MediaSourcePage::_sources.size()) {
        MediaSourcePage::_select_source(source_index);
      }
      return;
    }
  }

  if (touch_data->component_id == button_back_id) {
    MediaSourcePage::_return_to_media_player();
  } else if (touch_data->component_id == button_previous_page_id) {
    if (MediaSourcePage::_first_source_index >= MediaSourcePage::_page_type) {
      MediaSourcePage::_first_source_index -= MediaSourcePage::_page_type;
      MediaSourcePage::_update_display();
    }
  } else if (touch_data->component_id == button_next_page_id) {
    if (MediaSourcePage::_first_source_index + MediaSourcePage::_page_type < MediaSourcePage::_sources.size()) {
      MediaSourcePage::_first_source_index += MediaSourcePage::_page_type;
      MediaSourcePage::_update_display();
    }
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
