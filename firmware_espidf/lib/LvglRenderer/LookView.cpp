#include "LookView.hpp"
#include "Background.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

#include <cstring>

// clang-format off
const Look Looks::ALL[Looks::COUNT] = {
    {"Ocean",  "ocean",  "auto"},
    {"Dusk",   "dusk",   "auto"},
    {"Aurora", "aurora", "auto"},
    {"Peaks",  "peaks",  "auto"},
    {"Dark",   "none",   "dark"},
    {"Grey",   "none",   "grey"},
};
// clang-format on

namespace {
// What a look draws: the preset (none under a light palette, as Background::set() does) and the palette.
struct Drawn {
  Background::Preset preset;
  const Theme::Palette *palette;
};

Drawn drawn(const char *background, const char *theme) {
  Background::Preset preset = Background::from_name(background);
  const Theme::Palette &palette = Background::palette_for(preset, Theme::palette_from_name(theme));
  return {palette.light ? Background::Preset::NONE : preset, &palette};
}
} // namespace

int Looks::find(const char *background, const char *theme) {
  Drawn current = drawn(background, theme);
  for (int i = 0; i < COUNT; i++) {
    Drawn look = drawn(ALL[i].background, ALL[i].theme);
    if (look.preset == current.preset && look.palette == current.palette) {
      return i;
    }
  }
  return -1;
}

static void look_clicked(lv_event_t *event) {
  if (LookView::on_action != nullptr) {
    intptr_t look = (intptr_t)lv_event_get_user_data(event);
    LookView::on_action(look < 0 ? LookView::Action::BACK : LookView::Action::PICK, (int32_t)look);
  }
}

// The preview's image owns its pixels: one block, freed with the image.
static void free_preview(lv_event_t *event) { lv_free(lv_event_get_user_data(event)); }

// The whole screen's art, scaled down to PREVIEW_W x PREVIEW_H (nearest pixel, squeezed more down the
// screen than across, which keeps the bands and the horizon that tell the presets apart). Generated
// when the page is built, into LVGL's heap (PSRAM on the panel, 25 KB each); nothing in flash.
static lv_obj_t *art_preview(lv_obj_t *parent, Background::Preset preset) {
  const int32_t w = LookView::PREVIEW_W, h = LookView::PREVIEW_H;
  const size_t bytes = sizeof(lv_image_dsc_t) + (size_t)w * h * 2;
  uint8_t *block = static_cast<uint8_t *>(lv_malloc(bytes));
  if (block == nullptr) {
    return nullptr;
  }
  lv_image_dsc_t *image = reinterpret_cast<lv_image_dsc_t *>(block);
  uint16_t *pixels = reinterpret_cast<uint16_t *>(block + sizeof(lv_image_dsc_t));
  uint16_t row[320];
  for (int32_t y = 0; y < h; y++) {
    Background::render_row(preset, (int)(y * 480 / h + 480 / h / 2), row);
    for (int32_t x = 0; x < w; x++) {
      pixels[y * w + x] = row[x * 320 / w + 320 / w / 2];
    }
  }
  memset(image, 0, sizeof(*image));
  image->header.magic = LV_IMAGE_HEADER_MAGIC;
  image->header.cf = LV_COLOR_FORMAT_RGB565;
  image->header.w = w;
  image->header.h = h;
  image->header.stride = w * 2;
  image->data_size = (uint32_t)w * h * 2;
  image->data = reinterpret_cast<const uint8_t *>(pixels);

  lv_obj_t *obj = lv_image_create(parent);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_pos(obj, 0, 0);
  lv_image_set_src(obj, image);
  lv_obj_add_event_cb(obj, free_preview, LV_EVENT_DELETE, block);
  return obj;
}

// A small screen in the look's colours: its art (portrait only, as nothing else draws it) or its
// background colour, with a card holding an accent dot and two lines of text.
static lv_obj_t *preview(lv_obj_t *tile, const Look &look, bool landscape) {
  Drawn d = drawn(look.background, look.theme);
  const Theme::Palette &p = *d.palette;
  lv_obj_t *screen = lv_obj_create(tile);
  lv_obj_remove_style_all(screen);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_style_radius(screen, Theme::RADIUS_INSET, 0);
  lv_obj_set_style_clip_corner(screen, true, 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(screen, lv_color_hex(p.background), 0);
  if (landscape) {
    lv_obj_set_width(screen, lv_pct(100));
    lv_obj_set_flex_grow(screen, 1);
  } else {
    lv_obj_set_size(screen, LookView::PREVIEW_W, LookView::PREVIEW_H);
  }
  bool art = !landscape && d.preset != Background::Preset::NONE && art_preview(screen, d.preset) != nullptr;

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(card, Theme::RADIUS_INSET, 0);
  lv_obj_set_style_bg_opa(card, art ? LV_OPA_80 : LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(p.card), 0);
  lv_obj_set_style_border_side(card, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(card, Theme::HIGHLIGHT_WIDTH, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(p.highlight), 0);
  lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_size(card, lv_pct(76), 34);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *dot = lv_obj_create(card);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 16, 16);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(p.accent), 0);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 9, 0);
  static const struct {
    int32_t y, w;
    bool muted;
  } lines[] = {{-5, 50, false}, {5, 34, true}};
  for (const auto &l : lines) {
    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(l.w), 4);
    lv_obj_set_style_radius(line, 2, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(l.muted ? p.muted : p.text), 0);
    lv_obj_align(line, LV_ALIGN_LEFT_MID, 33, l.y);
  }
  return screen;
}

void LookView::build(lv_obj_t *parent, bool landscape) {
  Ui::Header header = Ui::header(parent, true, false);
  Ui::set_text(header.title, "Look");
  lv_obj_add_event_cb(header.back, look_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

  // 2 x 3 on a portrait screen, 3 x 2 on a landscape one, as the menu.
  lv_obj_t *grid = Ui::fill(parent, LV_FLEX_FLOW_ROW);
  static const int32_t two[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  static const int32_t three[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  lv_obj_set_grid_dsc_array(grid, landscape ? three : two, landscape ? two : three);
  int columns = landscape ? 3 : 2;
  for (int i = 0; i < Looks::COUNT; i++) {
    // A tile (card, lip, pressed colour) whose icon is replaced by the preview.
    lv_obj_t *tile = Ui::tile(grid, NULL, 24, Looks::ALL[i].name, false);
    lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, i % columns, 1, LV_GRID_ALIGN_STRETCH, i / columns, 1);
    lv_obj_set_style_pad_gap(tile, Theme::SPACE_S, 0);
    lv_obj_delete(Ui::tile_icon(tile));
    lv_obj_move_to_index(preview(tile, Looks::ALL[i], landscape), 0);
    lv_obj_add_event_cb(tile, look_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    LookView::_tiles[i] = tile;
  }
}

void LookView::show(const LookState &state) {
  for (int i = 0; i < Looks::COUNT; i++) {
    // The look in use: the tile in the accent tint with the amber outline the control being set has.
    lv_obj_t *tile = LookView::_tiles[i];
    Ui::set_bg_color(tile, i == state.selected ? Theme::on_tint() : Theme::card());
    Ui::set_tile_outlined(tile, i == state.selected);
  }
}
