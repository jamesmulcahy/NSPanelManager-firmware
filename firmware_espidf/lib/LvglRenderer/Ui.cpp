#include "Ui.hpp"
#include "Theme.hpp"

#include <cstring>

namespace Ui {

void set_text(lv_obj_t *label, const char *text) {
  // lv_label_set_text() redraws the label even when the text is the same.
  if (label != NULL && strcmp(lv_label_get_text(label), text) != 0) {
    lv_label_set_text(label, text);
  }
}

void set_text(lv_obj_t *label, const std::string &text) { set_text(label, text.c_str()); }

void set_visible(lv_obj_t *obj, bool visible) {
  if (obj != NULL && lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == visible) {
    lv_obj_set_flag(obj, LV_OBJ_FLAG_HIDDEN, !visible);
  }
}

void set_text_color(lv_obj_t *obj, lv_color_t color) {
  if (obj != NULL && !lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color)) {
    lv_obj_set_style_text_color(obj, color, 0);
  }
}

void set_bg_color(lv_obj_t *obj, lv_color_t color) {
  if (obj != NULL && !lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color)) {
    lv_obj_set_style_bg_color(obj, color, 0);
  }
}

void set_font(lv_obj_t *obj, const lv_font_t *font) {
  if (obj != NULL && lv_obj_get_style_text_font(obj, LV_PART_MAIN) != font) {
    lv_obj_set_style_text_font(obj, font, 0);
  }
}

void set_checked(lv_obj_t *obj, bool checked) {
  if (obj != NULL && lv_obj_has_state(obj, LV_STATE_CHECKED) != checked) {
    lv_obj_set_state(obj, LV_STATE_CHECKED, checked);
  }
}

void set_slider(lv_obj_t *slider, int32_t value) {
  if (slider != NULL && lv_slider_get_value(slider) != value) {
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
  }
}

void set_bar(lv_obj_t *bar, int32_t value) {
  if (bar != NULL && lv_bar_get_value(bar) != value) {
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
  }
}

lv_obj_t *fill(lv_obj_t *parent, lv_flex_flow_t flow) {
  lv_obj_t *group = Theme::group(parent, flow);
  lv_obj_set_flex_grow(group, 1);
  // Stretch across the parent's other axis.
  lv_flex_flow_t parent_flow = lv_obj_get_style_flex_flow(parent, LV_PART_MAIN);
  if (parent_flow == LV_FLEX_FLOW_ROW || parent_flow == LV_FLEX_FLOW_ROW_REVERSE) {
    lv_obj_set_height(group, lv_pct(100));
  } else {
    lv_obj_set_width(group, lv_pct(100));
  }
  return group;
}

lv_obj_t *icon(lv_obj_t *parent, const char *glyph, int size, lv_color_t color) {
  return Theme::label(parent, glyph, Theme::icon_font(size), color);
}

// Buttons are styled from scratch rather than from LVGL's default theme, whose press transitions
// animate (every frame of an animation is sent as fills) and whose shadows blur.
static void flat(lv_obj_t *obj) {
  lv_obj_remove_style_all(obj);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(obj, 10, 0);
  lv_obj_set_style_text_color(obj, Theme::text(), 0);
  lv_obj_set_style_text_color(obj, Theme::on(), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(obj, Theme::disabled(), LV_STATE_DISABLED);
}

lv_obj_t *icon_button(lv_obj_t *parent, const char *glyph, int size) {
  lv_obj_t *button = lv_button_create(parent);
  flat(button);
  lv_obj_set_size(button, size + 20, size + 12);
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, glyph);
  lv_obj_set_style_text_font(label, Theme::icon_font(size), 0);
  lv_obj_center(label);
  return button;
}

void set_icon(lv_obj_t *button, const char *glyph) {
  if (button != NULL) {
    set_text(lv_obj_get_child(button, 0), glyph);
  }
}

void set_enabled(lv_obj_t *button, bool enabled) {
  if (button != NULL && lv_obj_has_state(button, LV_STATE_DISABLED) == enabled) {
    lv_obj_set_state(button, LV_STATE_DISABLED, !enabled);
  }
}

Header header(lv_obj_t *parent, bool back, bool paging) {
  Header h;
  h.bar = Theme::card(parent);
  lv_obj_set_size(h.bar, lv_pct(100), 44);
  lv_obj_set_flex_flow(h.bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(h.bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(h.bar, 2, 0);
  lv_obj_set_style_pad_gap(h.bar, 0, 0);
  if (back) {
    h.back = icon_button(h.bar, NSPM_ICON_CHEVRON_LEFT, 24);
    // A divider between "back" and the page's own navigation, as on the Nextion pages.
    lv_obj_t *divider = lv_obj_create(h.bar);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 2, 24);
    lv_obj_set_style_bg_color(divider, Theme::track(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
  } else {
    lv_obj_set_style_pad_left(h.bar, 12, 0);
  }
  if (paging) {
    h.prev = icon_button(h.bar, NSPM_ICON_CHEVRON_LEFT, 24);
  } else if (back) {
    lv_obj_set_style_pad_column(h.bar, 10, 0);
  }
  h.title = Theme::label(h.bar, "", Theme::title_font(), Theme::text());
  lv_obj_set_flex_grow(h.title, 1);
  // One line, with dots if it doesn't fit: DOTS needs the label's height fixed.
  lv_obj_set_height(h.title, lv_font_get_line_height(Theme::title_font()));
  lv_label_set_long_mode(h.title, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(h.title, paging ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
  h.right = Theme::group(h.bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_gap(h.right, 0, 0);
  lv_obj_set_flex_align(h.right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (paging) {
    h.next = icon_button(h.bar, NSPM_ICON_CHEVRON_RIGHT, 24);
  }
  // Fingers land past the buttons' edges (on Kitchen: x 311-316 and y 50-57 for "next" at
  // 270-310), as the Nextion pages' hotspots reach the screen's edges. Give the buttons a margin
  // around them. LVGL only looks for a touch among a parent's children inside the parent's area, or
  // its extra drawing area when its overflow is visible: extend the bar's by the same margin.
  lv_obj_add_flag(h.bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_add_event_cb(h.bar, [](lv_event_t *event) { lv_event_set_ext_draw_size(event, 10); }, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
  lv_obj_refresh_ext_draw_size(h.bar);
  for (lv_obj_t *button : {h.back, h.prev, h.next}) {
    if (button != NULL) {
      lv_obj_set_ext_click_area(button, 10);
    }
  }
  return h;
}

lv_obj_t *tile(lv_obj_t *parent, const char *glyph, int icon_size, const char *text, bool row) {
  lv_obj_t *card = lv_button_create(parent);
  flat(card);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_bg_opa(card, Theme::card_opa(), 0);
  lv_obj_set_style_bg_color(card, Theme::card(), 0);
  lv_obj_set_style_bg_color(card, Theme::pressed(), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(card, Theme::text(), LV_STATE_PRESSED);
  lv_obj_set_style_pad_all(card, 6, 0);
  lv_obj_set_style_pad_gap(card, row ? 12 : 2, 0);
  lv_obj_set_flex_flow(card, row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, row ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (row) {
    lv_obj_set_style_pad_left(card, 16, 0);
  }
  lv_obj_t *image = Theme::label(card, glyph != NULL ? glyph : "", Theme::icon_font(icon_size), Theme::text());
  lv_obj_t *label = Theme::label(card, text, Theme::body_font(), Theme::text());
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
  if (row) {
    lv_obj_set_flex_grow(label, 1);
  } else {
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  }
  (void)image;
  return card;
}

lv_obj_t *tile_icon(lv_obj_t *tile) { return lv_obj_get_child(tile, 0); }
lv_obj_t *tile_label(lv_obj_t *tile) { return lv_obj_get_child(tile, 1); }

void set_tile_on(lv_obj_t *tile, bool on) {
  set_text_color(tile_icon(tile), on ? Theme::on() : Theme::text());
}

static void style_slider(lv_obj_t *slider, bool vertical) {
  lv_obj_remove_style_all(slider);
  lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_slider_set_orientation(slider, vertical ? LV_SLIDER_ORIENTATION_VERTICAL : LV_SLIDER_ORIENTATION_HORIZONTAL);
  lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, Theme::track(), LV_PART_MAIN);
  lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, Theme::text(), LV_PART_INDICATOR);
  // A wide touch area: the slider responds a little outside its track.
  lv_obj_set_ext_click_area(slider, 8);
}

SliderCard slider_card(lv_obj_t *parent, const char *glyph, bool vertical) {
  SliderCard s;
  s.card = Theme::card(parent);
  lv_obj_set_flex_grow(s.card, 1);
  lv_obj_set_flex_flow(s.card, vertical ? LV_FLEX_FLOW_COLUMN_REVERSE : LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s.card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(s.card, 10, 0);
  lv_flex_flow_t parent_flow = lv_obj_get_style_flex_flow(parent, LV_PART_MAIN);
  if (parent_flow == LV_FLEX_FLOW_ROW || parent_flow == LV_FLEX_FLOW_ROW_REVERSE) {
    lv_obj_set_height(s.card, lv_pct(100));
  } else {
    lv_obj_set_width(s.card, lv_pct(100));
  }
  s.icon = icon(s.card, glyph, 24, Theme::text());

  s.slider = lv_slider_create(s.card);
  style_slider(s.slider, vertical);
  lv_obj_set_flex_grow(s.slider, 1);
  if (vertical) {
    lv_obj_set_width(s.slider, 36);
  } else {
    lv_obj_set_height(s.slider, 36);
  }
  s.track = s.slider;

  s.value = Theme::label(s.card, "", Theme::body_font(), Theme::muted());
  lv_obj_set_style_min_width(s.value, 36, 0);
  lv_obj_set_style_text_align(s.value, LV_TEXT_ALIGN_CENTER, 0);
  return s;
}

static void ring_knob(lv_obj_t *slider) {
  lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_border_width(slider, 4, LV_PART_KNOB);
  lv_obj_set_style_border_color(slider, Theme::text(), LV_PART_KNOB);
  lv_obj_set_style_border_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, -3, LV_PART_KNOB);
}

void gradient_slider(SliderCard &card, lv_color_t from, lv_color_t to, bool vertical) {
  lv_obj_t *slider = card.slider;
  // A vertical slider's minimum is at the bottom, so its gradient runs bottom (from) to top (to).
  lv_obj_set_style_bg_color(slider, vertical ? to : from, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(slider, vertical ? from : to, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(slider, vertical ? LV_GRAD_DIR_VER : LV_GRAD_DIR_HOR, LV_PART_MAIN);
  ring_knob(slider);
}

void hue_slider(SliderCard &card, bool vertical) {
  // The strip takes the slider's place in the card; the slider floats over it, transparent.
  static const uint32_t hues[] = {0xF80000, 0xF8F800, 0x00F800, 0x00F8F8, 0x0000F8, 0xF800F8, 0xF80000};
  lv_obj_t *strip = lv_obj_create(card.card);
  lv_obj_remove_style_all(strip);
  lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(strip, 10, 0);
  lv_obj_set_style_clip_corner(strip, true, 0);
  lv_obj_set_flex_flow(strip, vertical ? LV_FLEX_FLOW_COLUMN_REVERSE : LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_grow(strip, 1);
  if (vertical) {
    lv_obj_set_width(strip, 36);
  } else {
    lv_obj_set_height(strip, 36);
  }
  lv_obj_move_to_index(strip, lv_obj_get_index(card.slider));
  for (int i = 0; i < 6; i++) {
    lv_obj_t *part = lv_obj_create(strip);
    lv_obj_remove_style_all(part);
    lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_grow(part, 1);
    if (vertical) {
      lv_obj_set_width(part, lv_pct(100));
    } else {
      lv_obj_set_height(part, lv_pct(100));
    }
    lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(part, lv_color_hex(vertical ? hues[i + 1] : hues[i]), 0);
    lv_obj_set_style_bg_grad_color(part, lv_color_hex(vertical ? hues[i] : hues[i + 1]), 0);
    lv_obj_set_style_bg_grad_dir(part, vertical ? LV_GRAD_DIR_VER : LV_GRAD_DIR_HOR, 0);
  }
  lv_obj_set_parent(card.slider, strip);
  lv_obj_add_flag(card.slider, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(card.slider, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(card.slider, LV_OPA_TRANSP, LV_PART_MAIN);
  ring_knob(card.slider);
  card.track = strip;
}

} // namespace Ui
