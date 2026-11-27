#include "EntitiesView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

#include <cstring>

// Whether every word of `text` is at most `width` wide in `font`, so a label wraps only between words.
static bool words_fit(const char *text, const lv_font_t *font, int32_t width) {
  char word[48];
  while (*text != '\0') {
    size_t length = strcspn(text, " \n");
    if (length > 0) {
      size_t n = length < sizeof(word) - 1 ? length : sizeof(word) - 1;
      memcpy(word, text, n);
      word[n] = '\0';
      lv_point_t size;
      lv_text_get_size(&size, word, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      if (size.x > width) {
        return false;
      }
    }
    text += length;
    if (*text != '\0') {
      text++;
    }
  }
  return true;
}

static void emit(EntitiesView::Action action, int32_t slot) {
  if (EntitiesView::on_action != nullptr) {
    EntitiesView::on_action(action, slot);
  }
}

static void header_clicked(lv_event_t *event) { emit((EntitiesView::Action)(intptr_t)lv_event_get_user_data(event), -1); }
static void slot_clicked(lv_event_t *event) { emit(EntitiesView::Action::SLOT, (int32_t)(intptr_t)lv_event_get_user_data(event)); }
static void slot_held(lv_event_t *event) { emit(EntitiesView::Action::SLOT_LONG, (int32_t)(intptr_t)lv_event_get_user_data(event)); }
static void slot_released(lv_event_t *event) { emit(EntitiesView::Action::SLOT_RELEASED, (int32_t)(intptr_t)lv_event_get_user_data(event)); }

void EntitiesView::build(lv_obj_t *parent, bool landscape) {
  EntitiesView::_landscape = landscape;
  EntitiesView::_size = 0;
  Ui::Header header = Ui::header(parent, true, true);
  EntitiesView::_title = header.title;
  EntitiesView::_prev = header.prev;
  EntitiesView::_next = header.next;
  lv_obj_add_event_cb(header.back, header_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)Action::BACK);
  lv_obj_add_event_cb(header.prev, header_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)Action::PREV);
  lv_obj_add_event_cb(header.next, header_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)Action::NEXT);

  EntitiesView::_grid = Ui::fill(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_layout(EntitiesView::_grid, LV_LAYOUT_GRID);
  for (int i = 0; i < EntitiesState::MAX_SLOTS; i++) {
    lv_obj_t *tile = Ui::tile(EntitiesView::_grid, "", 40, "", false);
    lv_obj_add_event_cb(tile, slot_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(tile, slot_held, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(tile, slot_released, LV_EVENT_RELEASED, (void *)(intptr_t)i);
    EntitiesView::_tiles[i] = tile;
  }

  // Shown while a scene is held down to be saved (the Nextion page's slider_save).
  EntitiesView::_save = lv_bar_create(parent);
  lv_obj_remove_style_all(EntitiesView::_save);
  lv_obj_set_size(EntitiesView::_save, lv_pct(100), Theme::px(6));
  lv_obj_set_style_radius(EntitiesView::_save, Theme::RADIUS_BAR, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(EntitiesView::_save, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(EntitiesView::_save, Theme::track(), LV_PART_MAIN);
  lv_obj_set_style_radius(EntitiesView::_save, Theme::RADIUS_BAR, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(EntitiesView::_save, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(EntitiesView::_save, Theme::on(), LV_PART_INDICATOR);
  // Always in the layout, invisible while idle: hiding it would resize the grid, and every tile
  // would be sent again when a scene is saved.
  lv_obj_set_style_bg_opa(EntitiesView::_save, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_bar_set_value(EntitiesView::_save, 0, LV_ANIM_OFF);
}

// Columns x rows for each page size. Portrait keeps the Nextion pages' grids (1x4, 2x4, 2x6);
// landscape uses its width (2x2, 4x2, 4x3) instead of the EU HMI's 2-column art.
void EntitiesView::_layout(int size) {
  if (size == EntitiesView::_size) {
    return;
  }
  EntitiesView::_size = size;
  int columns, rows;
  if (EntitiesView::_landscape) {
    columns = size <= 4 ? 2 : 4;
  } else {
    columns = size <= 4 ? 1 : 2;
  }
  rows = (size + columns - 1) / columns;
  EntitiesView::_columns = columns;

  static int32_t column_dsc[5], row_dsc[7];
  for (int i = 0; i < columns; i++) {
    column_dsc[i] = LV_GRID_FR(1);
  }
  column_dsc[columns] = LV_GRID_TEMPLATE_LAST;
  for (int i = 0; i < rows; i++) {
    row_dsc[i] = LV_GRID_FR(1);
  }
  row_dsc[rows] = LV_GRID_TEMPLATE_LAST;
  lv_obj_set_grid_dsc_array(EntitiesView::_grid, column_dsc, row_dsc);

  // A slot as wide as the screen puts its icon beside the name; small ones use the small icons.
  bool wide = columns == 1;
  const lv_font_t *icon_font = Theme::icon_font(size >= 12 ? 24 : 40);
  for (int i = 0; i < EntitiesState::MAX_SLOTS; i++) {
    lv_obj_t *tile = EntitiesView::_tiles[i];
    if (i < size) {
      lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, i % columns, 1, LV_GRID_ALIGN_STRETCH, i / columns, 1);
    }
    lv_obj_set_flex_flow(tile, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, wide ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(tile, Theme::px(wide ? 20 : 6), 0);
    lv_obj_set_style_pad_gap(tile, wide ? Theme::SPACE_XL : Theme::SPACE_XS, 0);
    lv_obj_t *label = Ui::tile_label(tile);
    if (wide) {
      lv_obj_set_width(label, LV_SIZE_CONTENT);
      lv_obj_set_flex_grow(label, 1);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    } else {
      lv_obj_set_flex_grow(label, 0);
      lv_obj_set_width(label, lv_pct(100));
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    Ui::set_font(Ui::tile_icon(tile), icon_font);
  }
}

void EntitiesView::show(const EntitiesState &state) {
  Ui::set_text(EntitiesView::_title, state.header);
  Ui::set_enabled(EntitiesView::_prev, state.can_prev);
  Ui::set_enabled(EntitiesView::_next, state.can_next);
  int size = state.size <= 4 ? 4 : state.size <= 8 ? 8 : 12;
  EntitiesView::_layout(size);
  for (int i = 0; i < EntitiesState::MAX_SLOTS; i++) {
    lv_obj_t *tile = EntitiesView::_tiles[i];
    const EntitySlot &slot = state.slots[i];
    bool shown = i < size && !slot.empty;
    Ui::set_own_sound(tile, state.own_sound);
    // Empty slots keep their cell (the grid doesn't reflow) but draw nothing.
    Ui::set_visible(tile, shown);
    if (!shown) {
      continue;
    }
    lv_obj_t *icon = Ui::tile_icon(tile);
    lv_obj_t *label = Ui::tile_label(tile);
    Ui::set_visible(icon, slot.icon != NULL);
    Ui::set_text(icon, slot.icon != NULL ? slot.icon : "");
    Ui::set_text(label, slot.name);
    Ui::set_tile_on(tile, slot.on);
    // Text-only slots (media sources) mark the current one as any tile is marked: the tint.
    Ui::set_text_color(label, Theme::text());
    // Text-only slots use the larger font where the tiles are wide enough for it, and every word
    // fits on a line ("Disconnected" would otherwise break inside the word).
    const lv_font_t *font = Theme::body_font();
    if (slot.icon == NULL && EntitiesView::_columns <= 2) {
      lv_obj_update_layout(tile);
      if (words_fit(slot.name.c_str(), Theme::title_font(), lv_obj_get_content_width(tile))) {
        font = Theme::title_font();
      }
    }
    Ui::set_font(label, font);
  }
  lv_opa_t track = state.save_progress >= 0 ? LV_OPA_COVER : LV_OPA_TRANSP;
  if (lv_obj_get_style_bg_opa(EntitiesView::_save, LV_PART_MAIN) != track) {
    lv_obj_set_style_bg_opa(EntitiesView::_save, track, LV_PART_MAIN);
  }
  Ui::set_bar(EntitiesView::_save, state.save_progress >= 0 ? state.save_progress : 0);
}
