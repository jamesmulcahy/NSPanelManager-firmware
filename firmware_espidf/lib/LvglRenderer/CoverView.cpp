#include "CoverView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

#include <cstdio>

static void emit(CoverView::Action action, int32_t value) {
  if (CoverView::on_action != nullptr) {
    CoverView::on_action(action, value);
  }
}

static void clicked(lv_event_t *event) { emit((CoverView::Action)(intptr_t)lv_event_get_user_data(event), 0); }

static void released(lv_event_t *event) {
  emit(CoverView::Action::POSITION, lv_slider_get_value((lv_obj_t *)lv_event_get_target(event)));
}

void CoverView::build(lv_obj_t *parent, bool landscape) {
  Ui::Header header = Ui::header(parent, true, false);
  CoverView::_title = header.title;
  lv_obj_add_event_cb(header.back, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)Action::BACK);

  // State and position in one card, the buttons in another: side by side on a landscape screen,
  // stacked on a portrait one. The position slider runs upright on a landscape screen, like the
  // cover itself, and across a portrait one.
  lv_obj_t *body = Ui::fill(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_t *status = Theme::card(body);
  lv_obj_set_flex_grow(status, 1);
  lv_obj_set_flex_flow(status, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(status, Theme::SPACE_L, 0);
  if (landscape) {
    lv_obj_set_height(status, lv_pct(100));
  } else {
    lv_obj_set_width(status, lv_pct(100));
  }
  lv_obj_t *info = Theme::group(status, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_flex_grow(info, 1);
  CoverView::_icon = Ui::icon(info, NSPM_ICON_WINDOW_SHUTTER, 40, Theme::text());
  CoverView::_state = Theme::label(info, "", Theme::title_font(), Theme::text());

  Ui::SliderCard position = Ui::slider_card(status, NSPM_ICON_ARROW_UP, landscape);
  // The slider is part of the status card, not a card of its own.
  lv_obj_set_style_bg_opa(position.card, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(position.card, 0, 0);
  lv_obj_set_style_pad_all(position.card, 0, 0);
  if (landscape) {
    lv_obj_set_flex_grow(position.card, 0);
    lv_obj_set_width(position.card, 60);
  } else {
    lv_obj_set_flex_grow(position.card, 0);
  }
  lv_obj_delete(position.icon);
  CoverView::_slider_card = position.card;
  CoverView::_slider = position.slider;
  CoverView::_value = position.value;
  lv_obj_add_event_cb(position.slider, released, LV_EVENT_RELEASED, NULL);

  lv_obj_t *buttons = Theme::card(body);
  lv_obj_set_flex_flow(buttons, landscape ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (landscape) {
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, lv_pct(100));
  } else {
    lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
  }
  struct {
    const char *icon;
    Action action;
    lv_obj_t **button;
  } controls[] = {
      {NSPM_ICON_ARROW_UP, Action::OPEN, &CoverView::_open},
      {NSPM_ICON_STOP, Action::STOP, &CoverView::_stop},
      {NSPM_ICON_ARROW_DOWN, Action::CLOSE, &CoverView::_close},
  };
  for (auto &control : controls) {
    lv_obj_t *button = Ui::icon_button(buttons, control.icon, 40);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, Theme::track(), 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(button, 68, 68);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)control.action);
    *control.button = button;
  }
}

void CoverView::show(const CoverState &state) {
  Ui::set_text(CoverView::_title, state.name);
  Ui::set_text(CoverView::_icon, state.is_open ? NSPM_ICON_WINDOW_SHUTTER_OPEN : NSPM_ICON_WINDOW_SHUTTER);
  Ui::set_text(CoverView::_state, state.state);
  Ui::set_visible(CoverView::_slider_card, state.has_position);
  Ui::set_slider(CoverView::_slider, state.position);
  char text[8];
  snprintf(text, sizeof(text), "%d%%", state.position);
  Ui::set_text(CoverView::_value, text);
  // Hidden, not greyed, when the cover can't do it (CoverPage.md).
  Ui::set_visible(CoverView::_open, state.can_open);
  Ui::set_visible(CoverView::_stop, state.can_stop);
  Ui::set_visible(CoverView::_close, state.can_close);
}
