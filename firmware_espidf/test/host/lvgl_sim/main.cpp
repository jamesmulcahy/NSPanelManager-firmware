// Host simulator for the LVGL screens (lib/LvglRenderer): draws every page on a US (320x480) and an
// EU (480x320) screen, and reports what each frame costs on the terminal link: the bytes of the
// protocol-8 records NextionTerminal would send (PackBits, COBS, headers), and the time at 4 Mbaud.
// Built and run by build.sh.
//
// Writes OUT/<region>-<nn>-<page>.ppm per page, and every frame's cost to OUT/costs.csv. Page
// fixtures (mock data) are in Pages.cpp.
#include "BootScreen.hpp"
#include "Pages.hpp"
#include "TerminalProtocol.hpp"
#include "Theme.hpp"

#include <cstdio>
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

  // Every page.
  int n = 0;
  for (const PageCase &page : page_cases()) {
    page.build(Theme::page_root(screen, landscape), landscape);
    lv_obj_invalidate(screen);
    refresh();
    report(region, page.name);
    char number[8];
    snprintf(number, sizeof(number), "%02d", n++);
    write_ppm(std::string(out) + "/" + region + "-" + number + "-" + page.name + ".ppm");
    if (page.update != nullptr) {
      page.update();
      refresh();
      report(region, std::string(page.name) + ": " + page.update_what);
    }
  }
  lv_display_delete(display);
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : ".";
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
  return 0;
}
