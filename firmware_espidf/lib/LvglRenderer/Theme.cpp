#include "Theme.hpp"

// Subsets of Montserrat (tools/gen_fonts.sh): only the characters and symbols the screens use.
LV_FONT_DECLARE(nspm_font_clock_48);
LV_FONT_DECLARE(nspm_font_title);
LV_FONT_DECLARE(nspm_font_body);
LV_FONT_DECLARE(nspm_font_large);
LV_FONT_DECLARE(nspm_font_icon_24);
LV_FONT_DECLARE(nspm_font_icon_40);
LV_FONT_DECLARE(nspm_font_icon_64);

namespace Theme {

const lv_font_t *clock_font() { return &nspm_font_clock_48; }
const lv_font_t *title_font() { return &nspm_font_title; }
const lv_font_t *body_font() { return &nspm_font_body; }
const lv_font_t *large_font() { return &nspm_font_large; }
const lv_font_t *icon_font(int size) {
  if (size >= 64) {
    return &nspm_font_icon_64;
  }
  return size >= 40 ? &nspm_font_icon_40 : &nspm_font_icon_24;
}

void screen(lv_obj_t *obj) {
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(obj, background(), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(obj, text(), 0);
  lv_obj_set_style_text_font(obj, body_font(), 0);
  lv_obj_set_style_pad_all(obj, 12, 0);
  lv_obj_set_style_pad_gap(obj, 12, 0);
}

static lv_opa_t card_opacity = LV_OPA_COVER;
lv_opa_t card_opa() { return card_opacity; }
void set_card_opa(lv_opa_t opa) { card_opacity = opa; }

lv_obj_t *card(lv_obj_t *parent) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, Theme::card(), 0);
  lv_obj_set_style_bg_opa(card, card_opacity, 0);
  lv_obj_set_style_text_color(card, text(), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 6, 0);
  return card;
}

lv_obj_t *group(lv_obj_t *parent, lv_flex_flow_t flow) {
  lv_obj_t *group = lv_obj_create(parent);
  lv_obj_remove_style_all(group);
  lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  // A bare object is 100x100 by default; a group is as big as what it holds unless told otherwise.
  lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(group, flow);
  lv_obj_set_style_pad_gap(group, 8, 0);
  return group;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

lv_obj_t *page_root(lv_obj_t *screen, bool landscape) {
  lv_obj_clean(screen);
  Theme::screen(screen);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_t *root = Theme::group(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_size(root, usable_width(landscape), usable_height(landscape));
  lv_obj_set_style_pad_all(root, 8, 0);
  lv_obj_set_style_pad_gap(root, 8, 0);
  return root;
}

} // namespace Theme
