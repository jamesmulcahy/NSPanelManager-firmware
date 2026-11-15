#include "ThermostatView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

static void emit(ThermostatView::Action action, int32_t option) {
  if (ThermostatView::on_action != nullptr) {
    ThermostatView::on_action(action, option);
  }
}

static void clicked(lv_event_t *event) { emit((ThermostatView::Action)(intptr_t)lv_event_get_user_data(event), -1); }
static void option_clicked(lv_event_t *event) {
  emit(ThermostatView::Action::OPTION, (int32_t)(intptr_t)lv_event_get_user_data(event));
}

static void on_click(lv_obj_t *obj, ThermostatView::Action action) {
  lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)action);
}

static lv_obj_t *centred_card(lv_obj_t *parent, bool landscape) {
  lv_obj_t *card = Theme::card(parent);
  lv_obj_set_flex_grow(card, 1);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (landscape) {
    lv_obj_set_height(card, lv_pct(100));
  } else {
    lv_obj_set_width(card, lv_pct(100));
  }
  return card;
}

void ThermostatView::build(lv_obj_t *parent, bool landscape) {
  Ui::Header header = Ui::header(parent, true, true);
  ThermostatView::_title = header.title;
  ThermostatView::_prev = header.prev;
  ThermostatView::_next = header.next;
  on_click(header.back, Action::BACK);
  on_click(header.prev, Action::PREV);
  on_click(header.next, Action::NEXT);

  // The room temperature and the setpoint: side by side on a landscape screen, stacked on a
  // portrait one.
  lv_obj_t *body = Ui::fill(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_t *room = centred_card(body, landscape);
  lv_obj_set_style_pad_row(room, 0, 0);
  Theme::label(room, "Room", Theme::body_font(), Theme::muted());
  ThermostatView::_current = Theme::label(room, "", Theme::clock_font(), Theme::text());

  lv_obj_t *set = centred_card(body, landscape);
  lv_obj_set_flex_flow(set, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(set, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *down = Ui::icon_button(set, NSPM_ICON_CHEVRON_LEFT, 40);
  lv_obj_t *value = Theme::group(set, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(value, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(value, Theme::SPACE_XS, 0);
  ThermostatView::_set_label = Theme::label(value, "", Theme::body_font(), Theme::muted());
  ThermostatView::_set_value = Theme::label(value, "", Theme::large_font(), Theme::on());
  lv_obj_t *up = Ui::icon_button(set, NSPM_ICON_CHEVRON_RIGHT, 40);
  on_click(down, Action::DOWN);
  on_click(up, Action::UP);

  // The options, in a row along the bottom on both screens.
  lv_obj_t *options = Theme::card(parent);
  lv_obj_set_size(options, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(options, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(options, Theme::SPACE_S, 0);
  lv_obj_set_style_pad_gap(options, Theme::SPACE_S, 0);
  for (int i = 0; i < ThermostatState::MAX_OPTIONS; i++) {
    lv_obj_t *option = Ui::tile(options, "", 24, "", false);
    lv_obj_set_flex_grow(option, 1);
    lv_obj_set_style_pad_ver(option, 6, 0);
    lv_obj_set_style_bg_color(option, Theme::track(), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(Ui::tile_label(option), Theme::muted(), 0);
    lv_obj_add_event_cb(option, option_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    // Third line: the option's name, under its value.
    Theme::label(option, "", Theme::body_font(), Theme::muted());
    lv_obj_set_style_text_color(Ui::tile_label(option), Theme::text(), 0);
    ThermostatView::_options[i] = option;
  }
}

void ThermostatView::show(const ThermostatState &state) {
  Ui::set_text(ThermostatView::_title, state.name);
  Ui::set_visible(ThermostatView::_prev, state.can_page);
  Ui::set_visible(ThermostatView::_next, state.can_page);
  Ui::set_text(ThermostatView::_current, state.current);
  Ui::set_text(ThermostatView::_set_label, state.set_label);
  Ui::set_text(ThermostatView::_set_value, state.set_value);
  for (int i = 0; i < ThermostatState::MAX_OPTIONS; i++) {
    const ThermostatOption &option = state.options[i];
    lv_obj_t *tile = ThermostatView::_options[i];
    Ui::set_visible(tile, option.icon != NULL);
    if (option.icon == NULL) {
      continue;
    }
    Ui::set_text(Ui::tile_icon(tile), option.icon);
    Ui::set_text(Ui::tile_label(tile), option.value);
    Ui::set_text(lv_obj_get_child(tile, 2), option.name);
    Ui::set_checked(tile, state.selected == i);
    Ui::set_tile_on(tile, state.selected == i);
  }
}
