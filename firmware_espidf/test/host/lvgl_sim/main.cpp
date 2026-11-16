// Host simulator for the LVGL screens (lib/LvglRenderer): draws every page on a US (320x480) and an
// EU (480x320) screen, and reports what each frame costs on the terminal link: the bytes of the
// protocol-8 records NextionTerminal would send (PackBits, COBS, headers), and the time at 4 Mbaud.
// Built and run by build.sh.
//
// Writes OUT/<region>-<nn>-<page>.ppm per page (and -after.ppm once its update is shown),
// OUT/us-bg-<preset>-<page>.ppm for the pages over each background preset, and every frame's cost
// to OUT/costs.csv. Page fixtures (mock data) are
// in Pages.cpp.
//
// NSPM_THEME=<palette> (Theme.hpp: dark, light, ocean, ...) draws everything in that palette, as the
// lvgl_theme command does; unset, or "auto", each background takes its own (dark without one).
#include "Background.hpp"
#include "BootScreen.hpp"
#include "Pages.hpp"
#include "TerminalProtocol.hpp"
#include "Theme.hpp"
#include "nspm_icons.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace protocol = nspanel_terminal::protocol;

static constexpr double LINK_BAUD = 4000000;

static int W, H;
static std::vector<uint16_t> frame; // What the display shows
static size_t wire_bytes, records;
static uint32_t ticks;
static std::vector<std::string> csv;
static const Theme::Palette *theme; // NSPM_THEME; nullptr for auto

// A band as NextionTerminal sends it: one rectangle, its pixels PackBits-encoded into DATA records.
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  const uint16_t *px = reinterpret_cast<const uint16_t *>(px_map);
  const int aw = area->x2 - area->x1 + 1, ah = area->y2 - area->y1 + 1;
  for (int y = 0; y < ah; y++) {
    memcpy(&frame[(area->y1 + y) * W + area->x1], px + y * aw, aw * sizeof(uint16_t));
  }
  auto source = [px](unsigned i) { return px[i]; };
  protocol::Encoder<decltype(source)> encoder(aw * ah, source);
  std::vector<uint8_t> body, record;
  while (encoder.next(body)) {
    protocol::data_record(1, (unsigned)records, body, record);
    wire_bytes += record.size();
    records++;
  }
  // RECT before and END after: header-only records.
  wire_bytes += 2 * protocol::record(1, 0, protocol::RECT).size();
  lv_display_flush_ready(display);
}

static void write_ppm(const std::string &path) {
  FILE *f = fopen(path.c_str(), "wb");
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (uint16_t c : frame) {
    unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 0x1F) << 3), (unsigned char)(((c >> 5) & 0x3F) << 2), (unsigned char)((c & 0x1F) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static void report(const char *region, const std::string &what) {
  // 10 bits per byte on the wire.
  double ms = wire_bytes * 10.0 / LINK_BAUD * 1000;
  printf("%-3s %-44s %7zu bytes  %5.1f ms at 4 Mbaud\n", region, what.c_str(), wire_bytes, ms);
  char row[192];
  snprintf(row, sizeof(row), "%s,\"%s\",%zu,%.1f", region, what.c_str(), wire_bytes, ms);
  csv.push_back(row);
  wire_bytes = records = 0;
}

static void refresh() {
  ticks += 1000;
  lv_timer_handler();
  lv_refr_now(NULL);
}

// The button whose icon (first child) is `glyph`, or NULL.
static lv_obj_t *find_button(lv_obj_t *obj, const char *glyph) {
  if (lv_obj_check_type(obj, &lv_button_class) && lv_obj_get_child_count(obj) > 0) {
    lv_obj_t *icon = lv_obj_get_child(obj, 0);
    if (lv_obj_check_type(icon, &lv_label_class) && strcmp(lv_label_get_text(icon), glyph) == 0) {
      return obj;
    }
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
    lv_obj_t *found = find_button(lv_obj_get_child(obj, i), glyph);
    if (found != NULL) {
      return found;
    }
  }
  return NULL;
}

static int hit_failures;

// What a finger at (x, y) presses, as LVGL's own search for the touched object finds it.
static void expect_hit(const char *what, int32_t x, int32_t y, lv_obj_t *expected) {
  lv_point_t point = {x, y};
  lv_obj_t *found = lv_indev_search_obj(lv_screen_active(), &point);
  // A press on a button's label is a press on the button.
  while (found != NULL && expected != NULL && found != expected && !lv_obj_has_flag(found, LV_OBJ_FLAG_CLICKABLE)) {
    found = lv_obj_get_parent(found);
  }
  bool ok = found == expected;
  hit_failures += ok ? 0 : 1;
  printf("hit test  %-44s (%3d,%3d): %s\n", what, (int)x, (int)y, ok ? "ok" : "WRONG OBJECT");
}

// Fingers land past controls near the screen's edges (Ui::reach_screen_edges): check the media
// player's volume row, where on Lounge most taps meant for mute missed it.
static void check_media_hits() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *mute = find_button(screen, NSPM_ICON_VOLUME_HIGH);
  lv_obj_t *down = find_button(screen, NSPM_ICON_MINUS);
  if (mute == NULL || down == NULL) {
    printf("hit test  media player: mute or volume-down not found\n");
    hit_failures++;
    return;
  }
  lv_area_t m, d;
  lv_obj_get_coords(mute, &m);
  lv_obj_get_coords(down, &d);
  int32_t mid = (m.y1 + m.y2) / 2;
  expect_hit("mute itself", (m.x1 + m.x2) / 2, mid, mute);
  expect_hit("left of mute, past its edge", 2, mid, mute);
  expect_hit("below mute, near the bottom edge", (m.x1 + m.x2) / 2, H - 3, mute);
  expect_hit("bottom-left corner", 1, H - 1, mute);
  expect_hit("volume-down's own left edge", d.x1 + 1, (d.y1 + d.y2) / 2, down);
  expect_hit("below volume-down, near the bottom edge", (d.x1 + d.x2) / 2, H - 3, down);
}

static void run(bool landscape, const char *out) {
  const char *region = landscape ? "eu" : "us";
  W = landscape ? 480 : 320;
  H = landscape ? 320 : 480;
  frame.assign(W * H, 0);

  lv_display_t *display = lv_display_create(W, H);
  // 20 lines, as LvglRenderer draws.
  static std::vector<uint8_t> buffer;
  buffer.assign(480 * 20 * 2, 0);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display, buffer.data(), NULL, buffer.size(), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush);
  lv_display_set_default(display);
  lv_obj_t *screen = lv_screen_active();
  Background::attach(screen);
  Theme::set_palette(Background::palette_for(Background::Preset::NONE, theme));

  // The boot screen, as LvglRenderer builds it.
  Theme::screen(screen);
  lv_obj_t *root = Theme::group(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_size(root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_gap(root, 12, 0);
  BootScreen::build(root, landscape);
  BootStatus status;
  status.time = "12:34";
  status.date = "Thursday 12 November";
  status.wifi = "Home WiFi";
  status.ip = "192.168.1.188";
  status.mqtt = "MQTT connected";
  status.loading = "Loading config and status";
  status.version = "Firmware 0.3.1111";
  BootScreen::show(status);
  refresh();
  report(region, "boot screen");
  write_ppm(std::string(out) + "/" + region + "-boot.ppm");
  status.time = "12:35";
  BootScreen::show(status);
  refresh();
  report(region, "boot screen: clock 12:34 -> 12:35");

  // Every page, plain; and on a portrait screen over each background preset.
  std::vector<Background::Preset> presets = {Background::Preset::NONE};
  if (!landscape) {
    presets.insert(presets.end(), {Background::Preset::OCEAN, Background::Preset::DUSK, Background::Preset::AURORA, Background::Preset::PEAKS});
  }
  for (Background::Preset preset : presets) {
    Background::set(preset, theme);
    const bool plain = preset == Background::Preset::NONE;
    int n = 0;
    for (const PageCase &page : page_cases()) {
      page.build(Theme::page_root(screen, landscape), landscape);
      lv_obj_invalidate(screen);
      refresh();
      std::string what = plain ? std::string(page.name) : std::string(Background::name(preset)) + " " + page.name;
      report(region, what);
      char number[8];
      snprintf(number, sizeof(number), "%02d", n++);
      write_ppm(std::string(out) + "/" + region + (plain ? "-" + std::string(number) : std::string("-bg-") + Background::name(preset)) + "-" + page.name + ".ppm");
      if (plain && !landscape && strcmp(page.name, "media-photo-art") == 0) {
        check_media_hits();
      }
      if (page.update != nullptr) {
        page.update();
        refresh();
        report(region, what + ": " + page.update_what);
        write_ppm(std::string(out) + "/" + region + (plain ? "-" + std::string(number) : std::string("-bg-") + Background::name(preset)) + "-" + page.name + "-after.ppm");
      }
    }
  }
  Background::set(Background::Preset::NONE, theme);
  lv_display_delete(display);
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : ".";
  theme = Theme::palette_from_name(getenv("NSPM_THEME"));
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return ticks; });
  run(false, out);
  run(true, out);
  FILE *f = fopen((std::string(out) + "/costs.csv").c_str(), "w");
  fprintf(f, "region,frame,wire_bytes,ms_4mbaud\n");
  for (const std::string &row : csv) {
    fprintf(f, "%s\n", row.c_str());
  }
  fclose(f);
  return hit_failures == 0 ? 0 : 1;
}
