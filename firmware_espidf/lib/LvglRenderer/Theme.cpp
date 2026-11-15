#include "Theme.hpp"

#include <cstring>
#include <initializer_list>

// Subsets of Montserrat (tools/gen_fonts.sh): only the characters and symbols the screens use.
LV_FONT_DECLARE(nspm_font_clock_48);
LV_FONT_DECLARE(nspm_font_clock_88);
LV_FONT_DECLARE(nspm_font_title);
LV_FONT_DECLARE(nspm_font_body);
LV_FONT_DECLARE(nspm_font_large);
LV_FONT_DECLARE(nspm_font_icon_24);
LV_FONT_DECLARE(nspm_font_icon_40);
LV_FONT_DECLARE(nspm_font_icon_64);

namespace Theme {

// clang-format off
//                       name      light  background card      text      muted     accent    track     pressed   disabled  danger    indicator  highlight lip       on_tint
const Palette DARK   = {"dark",   false, 0x10141C, 0x1C2230, 0xF0F0F0, 0x8890A0, 0x38A8F8, 0x283448, 0x303C50, 0x586070, 0xF85048, 0xF0F0F0, 0x303840, 0x080C10, 0x383020};
const Palette LIGHT  = {"light",  true,  0xE0E4E8, 0xF8F8F8, 0x182028, 0x687080, 0x1078D0, 0xD0D4D8, 0xC0C8D0, 0xA0A8B0, 0xD03028, 0x1078D0, 0xF8FCF8, 0xC8CCD0, 0xF8ECC8};
// Navy, with a cyan accent.
const Palette OCEAN  = {"ocean",  false, 0x081420, 0x102438, 0xF0F8F8, 0x80A0B8, 0x40C8F0, 0x183850, 0x284860, 0x506878, 0xF85048, 0xF0F8F8, 0x283848, 0x001018, 0x303028};
// Violet, with a peach accent.
const Palette DUSK   = {"dusk",   false, 0x181428, 0x282038, 0xF8F0F0, 0xA090A8, 0xF89060, 0x383048, 0x484058, 0x686078, 0xF85048, 0xF8F0F0, 0x383448, 0x100C18, 0x403028};
// Deep teal, with a green accent.
const Palette AURORA = {"aurora", false, 0x081418, 0x102828, 0xF0F8F8, 0x80A8A8, 0x40D0A8, 0x184040, 0x285050, 0x507070, 0xF85048, 0xF0F8F8, 0x283C38, 0x001010, 0x303020};
// Slate blue, with a sky accent.
const Palette PEAKS  = {"peaks",  false, 0x101828, 0x182840, 0xF0F4F8, 0x8898B0, 0x70B0F0, 0x283C58, 0x304868, 0x586880, 0xF85048, 0xF0F4F8, 0x283C50, 0x081018, 0x383028};
// clang-format on
// Highlight and lip are the card mixed 10% towards white and 55% towards black, rounded to RGB565
// (LIGHT's by eye). on_tint is a dark amber-brown, by eye: mixing amber into the blue and teal cards
// turns olive.

const Palette *detail::palette = &DARK;

void set_palette(const Palette &palette) { detail::palette = &palette; }

const Palette *palette_from_name(const char *name) {
  for (const Palette *p : {&DARK, &LIGHT, &OCEAN, &DUSK, &AURORA, &PEAKS}) {
    if (name != nullptr && strcmp(name, p->name) == 0) {
      return p;
    }
  }
  return nullptr;
}

const lv_font_t *clock_font() { return &nspm_font_clock_48; }
const lv_font_t *big_clock_font() { return &nspm_font_clock_88; }
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
  lv_obj_set_style_pad_all(obj, SPACE_L, 0);
  lv_obj_set_style_pad_gap(obj, SPACE_L, 0);
}

void top_highlight(lv_obj_t *obj) {
  lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(obj, HIGHLIGHT_WIDTH, 0);
  lv_obj_set_style_border_color(obj, highlight(), 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

void bottom_lip(lv_obj_t *obj) {
  lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(obj, LIP_WIDTH, 0);
  lv_obj_set_style_border_color(obj, lip(), 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
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
  top_highlight(card);
  lv_obj_set_style_radius(card, RADIUS_CARD, 0);
  lv_obj_set_style_pad_all(card, SPACE_L, 0);
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
  lv_obj_set_style_pad_gap(group, SPACE_M, 0);
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
  lv_obj_set_style_pad_all(root, SPACE_M, 0);
  lv_obj_set_style_pad_gap(root, SPACE_M, 0);
  return root;
}

} // namespace Theme
