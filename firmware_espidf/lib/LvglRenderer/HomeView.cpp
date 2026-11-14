#include "HomeView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

#include <cstdio>

static void emit(HomeView::Action action, int32_t value) {
  if (HomeView::on_action != nullptr) {
    HomeView::on_action(action, value);
  }
}

static void clicked(lv_event_t *event) {
  emit((HomeView::Action)(intptr_t)lv_event_get_user_data(event), 0);
}

static void slider_released(lv_event_t *event) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(event);
  emit((HomeView::Action)(intptr_t)lv_event_get_user_data(event), lv_slider_get_value(slider));
}

// A tile's release: UP, unless the press became a swipe (CANCEL). user_data: the UP action.
static void tile_released(lv_event_t *event) {
  HomeView::Action up = (HomeView::Action)(intptr_t)lv_event_get_user_data(event);
  lv_indev_t *indev = lv_indev_active();
  bool swiped = lv_event_get_code(event) == LV_EVENT_PRESS_LOST || (indev != NULL && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) ||
                (HomeView::is_swipe != nullptr && HomeView::is_swipe());
  if (swiped) {
    emit(up == HomeView::Action::CEILING_UP ? HomeView::Action::CEILING_CANCEL : HomeView::Action::TABLE_CANCEL, 0);
  } else {
    emit(up, 0);
  }
}

static void on_click(lv_obj_t *obj, HomeView::Action action, lv_event_code_t code = LV_EVENT_SHORT_CLICKED) {
  lv_obj_add_event_cb(obj, clicked, code, (void *)(intptr_t)action);
}

// A light group's tile: icon, name and brightness. Outlined while it is the one being edited.
static lv_obj_t *light_tile(lv_obj_t *parent, const char *glyph, const char *name, lv_obj_t **value) {
  lv_obj_t *tile = Ui::tile(parent, glyph, 40, name, false);
  lv_obj_set_flex_grow(tile, 1);
  lv_obj_set_style_border_color(tile, Theme::on(), 0);
  lv_obj_set_style_border_width(tile, 0, 0);
  lv_obj_set_style_text_color(Ui::tile_label(tile), Theme::muted(), 0);
  *value = Theme::label(tile, "", Theme::title_font(), Theme::text());
  return tile;
}

void HomeView::build(lv_obj_t *parent, bool landscape) {
  // Header: scenes on the left, the clock on the right.
  Ui::Header header = Ui::header(parent, false, false);
  lv_obj_set_style_pad_left(header.bar, 2, 0);
  HomeView::_scenes = Ui::icon_button(header.bar, NSPM_ICON_PALETTE_SWATCH, 24);
  lv_obj_move_to_index(HomeView::_scenes, 0);
  on_click(HomeView::_scenes, Action::SCENES, LV_EVENT_CLICKED);
  HomeView::_time = header.title;
  HomeView::_entities = Ui::icon_button(header.bar, NSPM_ICON_DOTS_HORIZONTAL, 24);
  on_click(HomeView::_entities, Action::ENTITIES, LV_EVENT_CLICKED);
  // Into the screen's corners, as the header's own buttons (Ui::header()).
  lv_obj_set_ext_click_area(HomeView::_scenes, 10);
  lv_obj_set_ext_click_area(HomeView::_entities, 10);
  lv_obj_set_style_text_align(HomeView::_time, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_pad_right(HomeView::_time, 10, 0);

  // Body: the light groups and the sliders, side by side on a landscape screen, stacked on a
  // portrait one; each slider runs along its card's long side.
  lv_obj_t *body = Ui::fill(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_t *lights = Theme::group(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_grow(lights, landscape ? 1 : 2);
  if (landscape) {
    lv_obj_set_height(lights, lv_pct(100));
  } else {
    lv_obj_set_width(lights, lv_pct(100));
  }
  HomeView::_ceiling = light_tile(lights, NSPM_ICON_CEILING_LIGHT, "Ceiling", &HomeView::_ceiling_value);
  HomeView::_table = light_tile(lights, NSPM_ICON_LAMP, "Table", &HomeView::_table_value);
  for (lv_obj_t *tile : {HomeView::_ceiling, HomeView::_table}) {
    if (landscape) {
      lv_obj_set_height(tile, lv_pct(100));
    } else {
      lv_obj_set_height(tile, lv_pct(100));
    }
  }
  on_click(HomeView::_ceiling, Action::CEILING);
  on_click(HomeView::_table, Action::TABLE);
  on_click(HomeView::_ceiling, Action::CEILING_LONG, LV_EVENT_LONG_PRESSED);
  on_click(HomeView::_table, Action::TABLE_LONG, LV_EVENT_LONG_PRESSED);
  on_click(HomeView::_ceiling, Action::CEILING_DOWN, LV_EVENT_PRESSED);
  on_click(HomeView::_table, Action::TABLE_DOWN, LV_EVENT_PRESSED);
  for (lv_event_code_t code : {LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
    lv_obj_add_event_cb(HomeView::_ceiling, tile_released, code, (void *)(intptr_t)Action::CEILING_UP);
    lv_obj_add_event_cb(HomeView::_table, tile_released, code, (void *)(intptr_t)Action::TABLE_UP);
  }

  lv_obj_t *sliders = Theme::group(body, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_grow(sliders, landscape ? 1 : 3);
  if (landscape) {
    lv_obj_set_height(sliders, lv_pct(100));
  } else {
    lv_obj_set_width(sliders, lv_pct(100));
  }
  Ui::SliderCard brightness = Ui::slider_card(sliders, NSPM_ICON_BRIGHTNESS_6, landscape);
  Ui::SliderCard kelvin = Ui::slider_card(sliders, NSPM_ICON_THERMOMETER, landscape);
  Ui::gradient_slider(kelvin, lv_color_hex(0xF8A038), lv_color_hex(0xC8E0F8), landscape);
  HomeView::_brightness = brightness.slider;
  HomeView::_brightness_value = brightness.value;
  HomeView::_kelvin = kelvin.slider;
  HomeView::_kelvin_value = kelvin.value;
  lv_obj_add_event_cb(brightness.slider, slider_released, LV_EVENT_RELEASED, (void *)(intptr_t)Action::BRIGHTNESS);
  lv_obj_add_event_cb(kelvin.slider, slider_released, LV_EVENT_RELEASED, (void *)(intptr_t)Action::COLOR_TEMPERATURE);

  // Footer: the room and the mode, each tapped to step to the next.
  lv_obj_t *footer = Theme::group(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_size(footer, lv_pct(100), 48);
  for (int i = 0; i < 2; i++) {
    lv_obj_t *button = Ui::tile(footer, NSPM_ICON_CHEVRON_DOWN, 24, "", true);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_height(button, lv_pct(100));
    lv_obj_set_style_pad_left(button, 8, 0);
    lv_obj_set_style_pad_gap(button, 6, 0);
    lv_obj_set_style_text_color(Ui::tile_icon(button), Theme::muted(), 0);
    on_click(button, i == 0 ? Action::NEXT_ROOM : Action::NEXT_MODE, LV_EVENT_CLICKED);
    (i == 0 ? HomeView::_room : HomeView::_mode) = Ui::tile_label(button);
  }
}

static void show_tile(lv_obj_t *tile, lv_obj_t *value, int level, bool editing) {
  char text[8];
  snprintf(text, sizeof(text), level > 0 ? "%d%%" : "Off", level);
  Ui::set_text(value, text);
  Ui::set_tile_on(tile, level > 0);
  int32_t border = editing ? 2 : 0;
  if (lv_obj_get_style_border_width(tile, LV_PART_MAIN) != border) {
    lv_obj_set_style_border_width(tile, border, 0);
  }
}

void HomeView::show(const HomeState &state) {
  Ui::set_text(HomeView::_time, state.time);
  Ui::set_icon(HomeView::_scenes, state.all_rooms ? NSPM_ICON_LIGHTBULB_GROUP : NSPM_ICON_PALETTE_SWATCH);
  Ui::set_visible(HomeView::_entities, state.entities);
  show_tile(HomeView::_ceiling, HomeView::_ceiling_value, state.ceiling, state.edit == HomeState::Edit::CEILING);
  show_tile(HomeView::_table, HomeView::_table_value, state.table, state.edit == HomeState::Edit::TABLE);

  // The brightness slider goes amber while it changes only one group, as on the Nextion page.
  lv_color_t indicator = state.edit == HomeState::Edit::ALL ? Theme::text() : Theme::on();
  if (!lv_color_eq(lv_obj_get_style_bg_color(HomeView::_brightness, LV_PART_INDICATOR), indicator)) {
    lv_obj_set_style_bg_color(HomeView::_brightness, indicator, LV_PART_INDICATOR);
  }
  Ui::set_slider(HomeView::_brightness, state.brightness);
  Ui::set_slider(HomeView::_kelvin, state.color_temperature);
  char text[8];
  snprintf(text, sizeof(text), "%d%%", state.brightness);
  Ui::set_text(HomeView::_brightness_value, text);
  snprintf(text, sizeof(text), "%d%%", state.color_temperature);
  Ui::set_text(HomeView::_kelvin_value, text);
  Ui::set_text(HomeView::_room, state.room);
  Ui::set_text(HomeView::_mode, state.mode);
}
