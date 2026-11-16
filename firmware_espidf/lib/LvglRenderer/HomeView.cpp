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
  lv_obj_set_style_text_color(Ui::tile_label(tile), Theme::muted(), 0);
  *value = Theme::label(tile, "", Theme::title_font(), Theme::text());
  return tile;
}

void HomeView::build(lv_obj_t *parent, bool landscape) {
  // Header: scenes on the left, the clock on the right.
  Ui::Header header = Ui::header(parent, false, false);
  lv_obj_set_style_pad_left(header.bar, Theme::SPACE_XS, 0);
  HomeView::_scenes = Ui::icon_button(header.bar, NSPM_ICON_PALETTE_SWATCH, 24);
  lv_obj_move_to_index(HomeView::_scenes, 0);
  on_click(HomeView::_scenes, Action::SCENES, LV_EVENT_CLICKED);
  HomeView::_time = header.title;
  HomeView::_entities = Ui::icon_button(header.bar, NSPM_ICON_DOTS_HORIZONTAL, 24);
  on_click(HomeView::_entities, Action::ENTITIES, LV_EVENT_CLICKED);
  // Into the screen's corners, as the header's own buttons (Ui::header()).
  Ui::reach_screen_edges(HomeView::_scenes, 24, 10);
  Ui::reach_screen_edges(HomeView::_entities, 24, 10);
  lv_obj_set_style_text_align(HomeView::_time, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_pad_right(HomeView::_time, 10, 0);

  // Body: the light groups and the sliders, side by side on a landscape screen, stacked on a
  // portrait one; each slider runs along its card's long side.
  // The groups' shares of the body are set by _show_controls(), from which controls the room needs.
  HomeView::_landscape = landscape;
  lv_obj_t *body = Ui::fill(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_t *lights = Theme::group(body, LV_FLEX_FLOW_ROW);
  HomeView::_lights = lights;
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
  HomeView::_sliders = sliders;
  if (landscape) {
    lv_obj_set_height(sliders, lv_pct(100));
  } else {
    lv_obj_set_width(sliders, lv_pct(100));
  }
  Ui::SliderCard brightness = Ui::slider_card(sliders, NSPM_ICON_BRIGHTNESS_6, landscape);
  Ui::SliderCard kelvin = Ui::slider_card(sliders, NSPM_ICON_THERMOMETER, landscape);
  Ui::gradient_slider(kelvin, lv_color_hex(0xF8A038), lv_color_hex(0xC8E0F8), landscape);
  HomeView::_brightness_card = brightness.card;
  HomeView::_kelvin_card = kelvin.card;
  HomeView::_brightness = brightness.slider;
  HomeView::_brightness_value = brightness.value;
  HomeView::_kelvin = kelvin.slider;
  HomeView::_kelvin_value = kelvin.value;
  lv_obj_add_event_cb(brightness.slider, slider_released, LV_EVENT_RELEASED, (void *)(intptr_t)Action::BRIGHTNESS);
  lv_obj_add_event_cb(kelvin.slider, slider_released, LV_EVENT_RELEASED, (void *)(intptr_t)Action::COLOR_TEMPERATURE);

  // A room without main-page lights: nothing for the tiles or the sliders to change.
  HomeView::_no_lights = Theme::card(body);
  lv_obj_set_flex_grow(HomeView::_no_lights, 1);
  lv_obj_set_size(HomeView::_no_lights, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_flow(HomeView::_no_lights, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(HomeView::_no_lights, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(HomeView::_no_lights, Theme::SPACE_M, 0);
  Ui::icon(HomeView::_no_lights, NSPM_ICON_LIGHTBULB_OUTLINE, 40, Theme::muted()); // In the 40 px font (gen_fonts.sh)
  HomeView::_no_lights_text = Theme::label(HomeView::_no_lights, "", Theme::body_font(), Theme::muted());
  lv_obj_add_flag(HomeView::_no_lights, LV_OBJ_FLAG_HIDDEN);
  HomeView::_show_controls(HomeState());

  // Footer: the room, picked from a list that opens upwards, and the mode, a two-way switch.
  lv_obj_t *footer = Theme::group(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_size(footer, lv_pct(100), 48);

  lv_obj_t *room = lv_dropdown_create(footer);
  HomeView::_room = room;
  HomeView::_options.clear();
  HomeView::_room_text.clear();
  HomeView::_catcher = NULL;
  Ui::popup_open = false;
  // Styled from scratch, as Ui's buttons are: the default theme would animate the press, darken it
  // with a recolor and clip the list's corners (see Ui.cpp, flat()).
  lv_obj_remove_style_all(room);
  lv_obj_set_flex_grow(room, 3);
  lv_obj_set_height(room, lv_pct(100));
  lv_obj_set_style_radius(room, Theme::RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(room, Theme::card(), 0);
  lv_obj_set_style_bg_opa(room, Theme::card_opa(), 0);
  Theme::bottom_lip(room);
  lv_obj_set_style_bg_color(room, Theme::pressed(), LV_STATE_PRESSED);
  lv_obj_set_style_text_font(room, Theme::body_font(), 0);
  lv_obj_set_style_text_color(room, Theme::text(), 0);
  lv_obj_set_style_pad_left(room, Theme::SPACE_XL, 0);
  lv_obj_set_style_pad_right(room, 10, 0);
  lv_obj_set_style_pad_ver(room, 0, 0);
  // The label sits mid-height: LVGL draws the button text from the top padding.
  lv_obj_set_style_pad_top(room, (48 - lv_font_get_line_height(Theme::body_font())) / 2, 0);
  lv_dropdown_set_dir(room, LV_DIR_TOP);
  lv_dropdown_set_symbol(room, NSPM_ICON_CHEVRON_UP);
  lv_obj_set_style_text_font(room, Theme::icon_font(24), LV_PART_INDICATOR);
  lv_obj_set_style_text_color(room, Theme::muted(), LV_PART_INDICATOR);
  lv_obj_t *list = lv_dropdown_get_list(room);
  lv_obj_remove_style_all(list);
  lv_obj_set_style_bg_color(list, Theme::card(), 0);
  lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(list, Theme::track(), 0);
  lv_obj_set_style_border_width(list, 1, 0);
  // The border over the row highlights, which span the list's width: without clip_corner (a mask
  // on every pixel the list draws) the highlights take the list's radius instead.
  lv_obj_set_style_border_post(list, true, 0);
  lv_obj_set_style_radius(list, Theme::RADIUS_CARD, 0);
  lv_obj_set_style_text_font(list, Theme::body_font(), 0);
  lv_obj_set_style_text_color(list, Theme::text(), 0);
  // Rows a finger can hit: 14 px text 26 px apart, six rows before the list scrolls.
  lv_obj_set_style_text_line_space(list, 26, 0);
  lv_obj_set_style_pad_ver(list, 14, 0);
  lv_obj_set_style_pad_hor(list, Theme::SPACE_XL, 0);
  lv_obj_set_style_max_height(list, 6 * (lv_font_get_line_height(Theme::body_font()) + 26) + 2, 0);
  // The selected room's row, and the row under a finger.
  lv_obj_set_style_radius(list, Theme::RADIUS_CARD, LV_PART_SELECTED);
  lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
  lv_obj_set_style_text_color(list, Theme::text(), LV_PART_SELECTED);
  lv_obj_set_style_bg_color(list, Theme::track(), (lv_style_selector_t)LV_PART_SELECTED | (lv_style_selector_t)LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(list, Theme::pressed(), (lv_style_selector_t)LV_PART_SELECTED | (lv_style_selector_t)LV_STATE_PRESSED);
  lv_obj_set_style_text_color(list, Theme::on(), (lv_style_selector_t)LV_PART_SELECTED | (lv_style_selector_t)LV_STATE_CHECKED);
  // A thin pill along the right edge while the list scrolls, as the default theme's, but without
  // its fade.
  lv_obj_set_style_bg_color(list, Theme::muted(), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_40, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_COVER, (lv_style_selector_t)LV_PART_SCROLLBAR | (lv_style_selector_t)LV_STATE_SCROLLED);
  lv_obj_set_style_radius(list, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_all(list, 6, LV_PART_SCROLLBAR);
  for (lv_event_code_t code : {LV_EVENT_READY, LV_EVENT_CANCEL, LV_EVENT_VALUE_CHANGED, LV_EVENT_DELETE}) {
    lv_obj_add_event_cb(room, HomeView::_room_event, code, NULL);
  }
  Ui::reach_screen_edges(room); // The footer is at the bottom edge

  lv_obj_t *mode = Theme::group(footer, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_grow(mode, 2);
  lv_obj_set_height(mode, lv_pct(100));
  lv_obj_set_style_bg_color(mode, Theme::card(), 0);
  lv_obj_set_style_bg_opa(mode, Theme::card_opa(), 0);
  lv_obj_set_style_radius(mode, Theme::RADIUS_CARD, 0);
  Theme::top_highlight(mode);
  lv_obj_set_style_pad_all(mode, Theme::SPACE_S, 0);
  lv_obj_set_style_pad_gap(mode, Theme::SPACE_S, 0);
  for (int i = 0; i < 2; i++) {
    lv_obj_t *half = lv_button_create(mode);
    // No default theme: it would grow the half and animate the press (see Ui.cpp, flat()).
    lv_obj_remove_style_all(half);
    lv_obj_set_flex_grow(half, 1);
    lv_obj_set_height(half, lv_pct(100));
    lv_obj_set_style_radius(half, Theme::RADIUS_INSET, 0);
    lv_obj_set_style_bg_opa(half, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(half, Theme::track(), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(half, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(half, Theme::pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(half, Theme::muted(), 0);
    lv_obj_set_style_text_color(half, Theme::text(), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(half, Theme::muted(), LV_STATE_DISABLED);
    // A plain label, so its colour follows the button's state.
    lv_obj_t *label = lv_label_create(half);
    lv_label_set_text_static(label, i == 0 ? "Room" : "All");
    lv_obj_set_style_text_font(label, Theme::body_font(), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(half, [](lv_event_t *event) { emit(Action::SET_MODE, (int32_t)(intptr_t)lv_event_get_user_data(event)); },
                        LV_EVENT_CLICKED, (void *)(intptr_t)i);
    (i == 0 ? HomeView::_mode_room : HomeView::_mode_all) = half;
    // To the screen edges below and beside, and over the switch's own padding (Ui::reach_screen_edges).
    Ui::reach_screen_edges(half, 24, Theme::SPACE_S);
  }
}

void HomeView::_room_event(lv_event_t *event) {
  lv_obj_t *room = (lv_obj_t *)lv_event_get_current_target(event);
  switch (lv_event_get_code(event)) {
  case LV_EVENT_READY: {
    // Opened. A transparent layer under the list takes any tap outside it, so that tap only closes
    // the list (the dropdown closes on defocus, which needs input groups this UI does not use).
    lv_obj_t *screen = lv_obj_get_screen(room);
    lv_obj_t *catcher = lv_obj_create(screen);
    lv_obj_remove_style_all(catcher);
    lv_obj_add_flag(catcher, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
    lv_obj_set_size(catcher, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(catcher, [](lv_event_t *e) { lv_dropdown_close((lv_obj_t *)lv_event_get_user_data(e)); }, LV_EVENT_CLICKED, room);
    lv_obj_move_foreground(lv_dropdown_get_list(room));
    HomeView::_catcher = catcher;
    Ui::popup_open = true; // No swipe-down menu while the list scrolls
    break;
  }
  case LV_EVENT_CANCEL:
  case LV_EVENT_DELETE:
    // Closed, or the page is going (the catcher is the screen's, not the page's).
    if (HomeView::_catcher != NULL) {
      lv_obj_delete_async(HomeView::_catcher);
      HomeView::_catcher = NULL;
    }
    Ui::popup_open = false;
    break;
  case LV_EVENT_VALUE_CHANGED:
    emit(Action::SELECT_ROOM, (int32_t)lv_dropdown_get_selected(room));
    break;
  default:
    break;
  }
}

static void show_tile(lv_obj_t *tile, lv_obj_t *value, int level, bool editing) {
  char text[8];
  snprintf(text, sizeof(text), level > 0 ? "%d%%" : "Off", level);
  Ui::set_text(value, text);
  Ui::set_tile_on(tile, level > 0);
  Ui::set_tile_outlined(tile, editing);
}

// Only the controls the room's lights need, and the space shared out between them.
void HomeView::_show_controls(const HomeState &state) {
  const bool any = state.has_ceiling || state.has_table;
  // A room with no ceiling or table lights has no main-page lights at all (every light is one or
  // the other), so the brightness slider would change nothing either.
  Ui::set_visible(HomeView::_ceiling, state.has_ceiling);
  Ui::set_visible(HomeView::_table, state.has_table);
  Ui::set_visible(HomeView::_lights, any);
  Ui::set_visible(HomeView::_sliders, any);
  Ui::set_visible(HomeView::_kelvin_card, any && state.has_color_temperature);
  Ui::set_visible(HomeView::_no_lights, !any);
  Ui::set_text(HomeView::_no_lights_text, state.all_rooms ? "No lights" : "No lights in this room");
  // Each slider card keeps the size it has with both: without the colour-temperature card, the
  // sliders give its share to the tiles, rather than stretch one card over two cards' space.
  // Portrait: tiles 2 : sliders 3 (1.5 per card); landscape: 1 : 1 (0.5 per card). In halves.
  const int cards = state.has_color_temperature ? 2 : 1;
  const int lights_grow = HomeView::_landscape ? 2 : 4;
  const int sliders_grow = (HomeView::_landscape ? 1 : 3) * cards;
  if (lv_obj_get_style_flex_grow(HomeView::_lights, LV_PART_MAIN) != lights_grow) {
    lv_obj_set_flex_grow(HomeView::_lights, lights_grow);
  }
  if (lv_obj_get_style_flex_grow(HomeView::_sliders, LV_PART_MAIN) != sliders_grow) {
    lv_obj_set_flex_grow(HomeView::_sliders, sliders_grow);
  }
}

void HomeView::show(const HomeState &state) {
  HomeView::_show_controls(state);
  Ui::set_text(HomeView::_time, state.time);
  Ui::set_icon(HomeView::_scenes, state.all_rooms ? NSPM_ICON_LIGHTBULB_GROUP : NSPM_ICON_PALETTE_SWATCH);
  Ui::set_visible(HomeView::_entities, state.entities);
  show_tile(HomeView::_ceiling, HomeView::_ceiling_value, state.ceiling, state.edit == HomeState::Edit::CEILING);
  show_tile(HomeView::_table, HomeView::_table_value, state.table, state.edit == HomeState::Edit::TABLE);

  // The brightness slider goes amber while it changes only one group, as on the Nextion page.
  lv_color_t indicator = state.edit == HomeState::Edit::ALL ? Theme::indicator() : Theme::on();
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

  // The room list, and what the button reads: the selected room, or the given text (all-rooms
  // mode, or a room not in the list).
  std::string options;
  for (const std::string &name : state.rooms) {
    options += (options.empty() ? "" : "\n") + name;
  }
  if (options.empty()) {
    options = state.room;
  }
  if (options != HomeView::_options) {
    HomeView::_options = options;
    lv_dropdown_set_options(HomeView::_room, options.c_str());
  }
  const bool in_list = state.room_index >= 0 && state.room_index < (int)state.rooms.size();
  if (in_list) {
    if (lv_dropdown_get_selected(HomeView::_room) != (uint32_t)state.room_index) {
      lv_dropdown_set_selected(HomeView::_room, state.room_index);
    }
    if (lv_dropdown_get_text(HomeView::_room) != NULL) {
      lv_dropdown_set_text(HomeView::_room, NULL);
    }
  } else if (state.room != HomeView::_room_text || lv_dropdown_get_text(HomeView::_room) == NULL) {
    HomeView::_room_text = state.room;
    lv_dropdown_set_text(HomeView::_room, HomeView::_room_text.c_str());
  }
  Ui::set_checked(HomeView::_mode_room, !state.all_rooms);
  Ui::set_checked(HomeView::_mode_all, state.all_rooms);
  Ui::set_enabled(HomeView::_mode_all, state.all_allowed || state.all_rooms);
}
