#include "LightView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

#include <cstdio>

static void emit(LightView::Action action, int32_t value) {
  if (LightView::on_action != nullptr) {
    LightView::on_action(action, value);
  }
}

static void clicked(lv_event_t *event) { emit((LightView::Action)(intptr_t)lv_event_get_user_data(event), 0); }

static void released(lv_event_t *event) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(event);
  emit((LightView::Action)(intptr_t)lv_event_get_user_data(event), lv_slider_get_value(slider));
}

static lv_obj_t *watch(lv_obj_t *slider, LightView::Action action) {
  lv_obj_add_event_cb(slider, released, LV_EVENT_RELEASED, (void *)(intptr_t)action);
  return slider;
}

void LightView::build(lv_obj_t *parent, bool landscape) {
  LightView::_landscape = landscape;
  Ui::Header header = Ui::header(parent, true, false);
  LightView::_title = header.title;
  lv_obj_add_event_cb(header.back, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)Action::BACK);
  LightView::_mode = Ui::icon_button(header.right, NSPM_ICON_PALETTE, 24);
  Ui::reach_screen_edges(LightView::_mode, Theme::px(24), Theme::px(10)); // As the header's own buttons
  lv_obj_add_event_cb(LightView::_mode, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)Action::SWITCH_MODE);

  // Three cards along the screen's long side: sliders across a portrait screen, upright on a
  // landscape one, as the US and EU Nextion pages have them.
  lv_obj_t *body = Ui::fill(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);

  Ui::SliderCard brightness = Ui::slider_card(body, NSPM_ICON_BRIGHTNESS_6, landscape);
  LightView::_brightness = watch(brightness.slider, Action::BRIGHTNESS);
  LightView::_brightness_value = brightness.value;
  lv_slider_set_range(LightView::_brightness, 1, 100);

  Ui::SliderCard kelvin = Ui::slider_card(body, NSPM_ICON_THERMOMETER, landscape);
  Ui::gradient_slider(kelvin, lv_color_hex(0xF8A038), lv_color_hex(0xC8E0F8), landscape);
  LightView::_kelvin_card = kelvin.card;
  LightView::_kelvin = watch(kelvin.slider, Action::COLOR_TEMPERATURE);
  lv_obj_delete(kelvin.value);

  Ui::SliderCard saturation = Ui::slider_card(body, NSPM_ICON_WATER_PERCENT, landscape);
  Ui::gradient_slider(saturation, lv_color_hex(0xF0F0F0), lv_color_hex(0xF80000), landscape);
  LightView::_saturation_card = saturation.card;
  LightView::_saturation = watch(saturation.slider, Action::SATURATION);
  lv_obj_delete(saturation.value);

  Ui::SliderCard hue = Ui::slider_card(body, NSPM_ICON_PALETTE, landscape);
  Ui::hue_slider(hue, landscape);
  LightView::_hue_card = hue.card;
  LightView::_hue = watch(hue.slider, Action::HUE);
  lv_slider_set_range(LightView::_hue, 0, 359);
  LightView::_hue_value = hue.value;
}

void LightView::show(const LightState &state) {
  Ui::set_text(LightView::_title, state.name);
  Ui::set_visible(LightView::_mode, state.can_color && state.can_temperature);
  // The button shows the mode it switches to.
  Ui::set_icon(LightView::_mode, state.color_mode ? NSPM_ICON_THERMOMETER : NSPM_ICON_PALETTE);
  bool color = state.color_mode && state.can_color;
  Ui::set_visible(LightView::_kelvin_card, !color && state.can_temperature);
  Ui::set_visible(LightView::_saturation_card, color);
  Ui::set_visible(LightView::_hue_card, color);

  Ui::set_slider(LightView::_brightness, state.brightness);
  char text[8];
  snprintf(text, sizeof(text), "%d%%", state.brightness);
  Ui::set_text(LightView::_brightness_value, text);
  Ui::set_slider(LightView::_kelvin, state.color_temperature);
  Ui::set_slider(LightView::_saturation, state.saturation);
  Ui::set_slider(LightView::_hue, state.hue);
  snprintf(text, sizeof(text), "%d\xC2\xB0", state.hue);
  Ui::set_text(LightView::_hue_value, text);

  // The saturation gradient ends in the light's hue, at full saturation.
  lv_color_t hue = lv_color_hsv_to_rgb(state.hue, 100, 100);
  lv_color_t end = LightView::_landscape ? lv_obj_get_style_bg_color(LightView::_saturation, LV_PART_MAIN)
                                         : lv_obj_get_style_bg_grad_color(LightView::_saturation, LV_PART_MAIN);
  if (!lv_color_eq(end, hue)) {
    if (LightView::_landscape) {
      lv_obj_set_style_bg_color(LightView::_saturation, hue, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_grad_color(LightView::_saturation, hue, LV_PART_MAIN);
    }
  }
}
