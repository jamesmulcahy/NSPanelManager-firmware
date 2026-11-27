#include "Background.hpp"
#include "Theme.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr int W = 320;
constexpr int H = 480;

struct Rgb {
  float r, g, b;
};
struct Stop {
  float t;
  Rgb c;
};

Rgb lerp(Rgb a, Rgb b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

float clamp01(float t) {
  return t < 0 ? 0 : (t > 1 ? 1 : t);
}

// The colour at t (0-1) along `stops`.
Rgb ramp(const Stop *stops, int count, float t) {
  t = clamp01(t);
  for (int i = 0; i + 1 < count; i++) {
    if (t <= stops[i + 1].t) {
      float span = stops[i + 1].t - stops[i].t;
      return lerp(stops[i].c, stops[i + 1].c, span > 0 ? (t - stops[i].t) / span : 0);
    }
  }
  return stops[count - 1].c;
}

uint16_t to565(Rgb c) {
  int r = (int)lroundf(c.r), g = (int)lroundf(c.g), b = (int)lroundf(c.b);
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Each design fills one row. Shapes are edges along x; nothing is dithered or anti-aliased.

void ocean(int y, uint16_t *row) {
  static const Stop water[] = {{0, {10, 28, 52}}, {1, {6, 18, 38}}};
  static const struct {
    float base, a, p, ph;
    Rgb c;
  } swells[] = {{120, 16, 70, 0.0f, {18, 48, 78}}, {215, 20, 90, 1.7f, {22, 58, 92}}, {310, 18, 77, 3.1f, {26, 68, 104}}, {400, 22, 101, 4.6f, {30, 78, 116}}};
  std::fill(row, row + W, to565(ramp(water, 2, (float)y / H)));
  for (const auto &s : swells) {
    uint16_t c = to565(s.c);
    for (int x = 0; x < W; x++) {
      float top = s.base + s.a * sinf(x / s.p + s.ph);
      if (top <= y && y <= top + 46 + 10 * sinf(x / (s.p * 0.7f) + s.ph * 2)) {
        row[x] = c;
      }
    }
  }
}

void dusk(int y, uint16_t *row) {
  static const Stop sky[] = {{0, {22, 24, 58}}, {0.45f, {70, 46, 96}}, {0.68f, {196, 96, 84}}, {0.78f, {240, 160, 92}}};
  static const struct {
    float base;
    float w[2][3]; // amplitude, period, phase
    Rgb top, bottom;
  } hills[] = {{330, {{16, 41, 0.3f}, {7, 13, 1.1f}}, {92, 52, 90}, {60, 34, 70}},
               {372, {{20, 57, 2.0f}, {6, 17, 0.4f}}, {54, 34, 66}, {36, 24, 52}},
               {418, {{14, 35, 4.1f}, {5, 11, 2.2f}}, {30, 22, 44}, {20, 16, 34}}};
  std::fill(row, row + W, to565(ramp(sky, 4, y / 380.0f)));
  uint16_t sun = to565({252, 196, 120});
  for (int x = 0; x < W; x++) {
    if ((x - 222) * (x - 222) + (y - 318) * (y - 318) < 34 * 34) {
      row[x] = sun;
    }
  }
  for (const auto &h : hills) {
    uint16_t c = to565(lerp(h.top, h.bottom, clamp01((y - h.base + 30) / (H - h.base + 30))));
    for (int x = 0; x < W; x++) {
      float ridge = h.base;
      for (const auto &w : h.w) {
        ridge += w[0] * sinf(x / w[1] + w[2]);
      }
      if (y >= ridge) {
        row[x] = c;
      }
    }
  }
}

void aurora(int y, uint16_t *row) {
  static const Stop sky[] = {{0, {6, 10, 28}}, {0.6f, {12, 22, 48}}, {1, {8, 14, 32}}};
  static const struct {
    float base, amp, period, ph;
    int tones;
    Rgb tone[3];
  } curtains[] = {{150, 40, 60, 0.4f, 3, {{24, 96, 88}, {36, 140, 112}, {60, 190, 140}}},
                  {250, 26, 85, 2.3f, 3, {{18, 62, 80}, {26, 92, 106}, {40, 128, 136}}},
                  {190, 50, 47, 4.0f, 2, {{18, 30, 62}, {26, 42, 82}}}};
  std::fill(row, row + W, to565(ramp(sky, 3, (float)y / H)));
  // 40 stars from a small LCG, in the top two thirds.
  uint32_t seed = 7;
  for (int i = 0; i < 40; i++) {
    seed = (seed * 1103515245u + 12345u) & 0x7FFFFFFF;
    if ((int)((seed >> 9) % (H * 2 / 3)) == y) {
      row[seed % W] = to565({200, 210, 230});
    }
  }
  for (const auto &c : curtains) {
    for (int x = 0; x < W; x++) {
      float d = fabsf(y - (c.base + c.amp * sinf(x / c.period + c.ph)));
      float band = 70 - 18 * sinf(x / (c.period * 0.5f) + c.ph);
      if (d < band) {
        row[x] = to565(c.tone[std::min(c.tones - 1, (int)((1 - d / band) * c.tones))]);
      }
    }
  }
}

// A polyline's y at x, for points sorted by x across the screen.
float poly(const float (*points)[2], int count, int x) {
  for (int i = 0; i + 1 < count; i++) {
    if (points[i][0] <= x && x <= points[i + 1][0]) {
      return points[i][1] + (points[i + 1][1] - points[i][1]) * (x - points[i][0]) / (points[i + 1][0] - points[i][0]);
    }
  }
  return H;
}

void peaks(int y, uint16_t *row) {
  static const Stop sky[] = {{0, {34, 64, 108}}, {0.7f, {86, 128, 170}}, {1, {120, 158, 190}}};
  static const float far[][2] = {{0, 300}, {60, 230}, {120, 280}, {190, 205}, {260, 270}, {320, 240}};
  static const float mid[][2] = {{0, 340}, {80, 290}, {150, 330}, {230, 270}, {320, 330}};
  static const float near[][2] = {{0, 400}, {100, 350}, {170, 390}, {260, 345}, {320, 380}};
  static const struct {
    const float (*points)[2];
    int count;
    Rgb top, bottom;
  } ranges[] = {{far, 6, {66, 92, 128}, {52, 74, 106}}, {mid, 5, {44, 66, 96}, {34, 52, 78}}, {near, 5, {26, 42, 64}, {18, 30, 48}}};
  std::fill(row, row + W, to565(ramp(sky, 3, y / 340.0f)));
  for (const auto &r : ranges) {
    uint16_t c = to565(lerp(r.top, r.bottom, clamp01((y - 200) / 280.0f)));
    for (int x = 0; x < W; x++) {
      if (y >= poly(r.points, r.count, x)) {
        row[x] = c;
      }
    }
  }
}


Rgb mix(Rgb a, Rgb b, float t) {
  return lerp(a, b, t);
}

// A bay at sunset: the sun low on the left, Vesuvius's twin peaks dark against the glow, the sea
// below with the sun's reflection, and a marble column at each edge.
void vesuvius(int y, uint16_t *row) {
  constexpr int horizon = 355;
  static const Stop sky[] = {{0, {27, 23, 48}}, {0.45f, {92, 46, 82}}, {0.72f, {217, 112, 74}}, {1, {242, 179, 94}}};
  static const Stop sea[] = {{0, {122, 58, 74}}, {1, {27, 23, 48}}};
  static const float volcano[][2] = {{0, 355}, {70, 331}, {128, 288}, {150, 259}, {173, 273}, {192, 245}, {208, 273}, {269, 321}, {320, 341}};
  const Rgb marble = {234, 219, 196};
  Rgb base;
  if (y < horizon) {
    base = ramp(sky, 4, (float)y / horizon);
    for (int x = 0; x < W; x++) {
      Rgb c = base;
      int d2 = (x - 62) * (x - 62) + (y - 332) * (y - 332);
      if (d2 < 30 * 30) {
        c = mix(base, {255, 217, 138}, 0.9f);
      } else if (d2 < 46 * 46) {
        c = mix(base, {255, 217, 138}, 0.15f);
      }
      if (y >= poly(volcano, 9, x)) {
        c = {42, 26, 51};
      }
      row[x] = to565(c);
    }
  } else {
    base = ramp(sea, 2, (float)(y - horizon) / (H - horizon));
    std::fill(row, row + W, to565(base));
    // The sun's reflection: two short glints.
    for (int x = 0; x < W; x++) {
      if (((y == 359 || y == 360) && x >= 40 && x <= 104) || ((y == 367 || y == 368) && x >= 48 && x <= 92)) {
        row[x] = to565(mix(base, {255, 217, 138}, 0.5f));
      }
    }
  }
  // The columns: shaft 16 px wide from y 48, capital 24 px wide on it, both the marble at 16-22%.
  for (int left : {11, 293}) {
    if (y >= 48) {
      for (int x = left; x < left + 16; x++) {
        row[x] = to565(mix(base, marble, 0.16f));
      }
    } else if (y >= 40) {
      for (int x = left - 4; x < left + 20; x++) {
        row[x] = to565(mix(base, marble, 0.22f));
      }
    }
  }
}

// The floodlit Parthenon seen from its steps: its gold front across the screen under a starry navy
// sky and a pale moon, four steps down to a dark forecourt.
void parthenon(int y, uint16_t *row) {
  constexpr float baseY = 384, pw = 294, cx = 160, x0 = cx - pw / 2;
  constexpr int columns = 6;
  constexpr float colW = pw / columns, colH = pw * 0.2f, top = baseY - colH - 10, ped = top - 5, apex = ped - pw * 0.07f;
  static const Stop sky[] = {{0, {5, 10, 24}}, {1, {28, 40, 80}}};
  static const Rgb steps[] = {{58, 58, 82}, {46, 46, 70}, {36, 36, 58}, {26, 26, 46}};
  const Rgb gold = {242, 200, 103};
  Rgb bg = ramp(sky, 2, (float)y / H);
  std::fill(row, row + W, to565(bg));
  // 34 stars, in the top 55%, from a small LCG.
  uint32_t seed = 11;
  for (int i = 0; i < 34; i++) {
    seed = (seed * 1103515245u + 12345u) & 0x7FFFFFFF;
    if ((int)((seed >> 9) % (H * 55 / 100)) == y) {
      row[seed % W] = to565(mix(bg, {255, 255, 255}, 0.6f));
    }
  }
  // The moon, with two faint rings of glow.
  for (int x = 0; x < W; x++) {
    int d2 = (x - 256) * (x - 256) + (y - 125) * (y - 125);
    if (d2 < 34 * 34) {
      row[x] = to565(mix(bg, {244, 235, 210}, 0.9f));
    } else if (d2 < 48 * 48) {
      row[x] = to565(mix(bg, {244, 235, 210}, 0.14f));
    } else if (d2 < 62 * 62) {
      row[x] = to565(mix(bg, {244, 235, 210}, 0.07f));
    }
  }
  const uint16_t temple = to565(mix(bg, gold, 0.55f));
  if (y >= apex && y < ped) { // The pediment: a triangle.
    float half = (pw / 2 + 3) * (y - apex) / (ped - apex);
    for (int x = (int)(cx - half); x <= (int)(cx + half); x++) {
      row[x] = temple;
    }
  } else if (y >= ped && y < top) { // The entablature.
    std::fill(row + (int)(x0 - 3), row + (int)(x0 + pw + 3), temple);
  } else if (y >= top && y < baseY - 10) { // The columns.
    for (int i = 0; i < columns; i++) {
      std::fill(row + (int)(x0 + colW * i + colW * 0.2f), row + (int)(x0 + colW * i + colW * 0.8f), temple);
    }
  } else if (y >= baseY - 10 && y < baseY) { // Three thin courses of the platform.
    int grow = y < baseY - 6.5f ? 5 : (y < baseY - 3 ? 8 : 11);
    std::fill(row + (int)(x0 - grow), row + (int)(x0 + pw + grow), temple);
  } else if (y >= baseY && y < baseY + 77) { // Four steps, each a little wider and darker.
    int i = std::min((y - (int)baseY) / 19, 3); // 77 rows / 19 = 4.05: the last step takes the extra row.
    std::fill(row + std::max(0, 6 - i * 3), row + std::min(W, 314 + i * 3), to565(steps[i]));
    std::fill(row, row + std::max(0, 6 - i * 3), to565(bg));
    std::fill(row + std::min(W, 314 + i * 3), row + W, to565(bg));
  } else if (y >= baseY + 77) {
    std::fill(row, row + W, to565({10, 15, 30}));
  }
}

struct Rect {
  uint16_t x, y, w, h;
  uint16_t color;
};

// Name, design, how opaque cards are over it (see-through, 179, where the art is calm; nearly solid
// where it is busy, as aurora's curtains, or bright behind text, as dusk's horizon glow), and the
// palette "auto" picks for it: its colours, so the cards sit on the art rather than over it.
const struct {
  Background::Preset preset;
  const char *name;
  void (*render)(int y, uint16_t *row);
  uint8_t card_opa;
  const Theme::Palette *palette;
} presets[] = {
    {Background::Preset::OCEAN, "ocean", ocean, 179, &Theme::OCEAN},
    {Background::Preset::DUSK, "dusk", dusk, 217, &Theme::DUSK},
    {Background::Preset::AURORA, "aurora", aurora, 230, &Theme::AURORA},
    {Background::Preset::PEAKS, "peaks", peaks, 179, &Theme::PEAKS},
    {Background::Preset::VESUVIUS, "vesuvius", vesuvius, 217, &Theme::VESUVIUS},
    {Background::Preset::PARTHENON, "parthenon", parthenon, 200, &Theme::PARTHENON},
};

Background::Preset current_preset = Background::Preset::NONE;
// The current preset's rectangles, sorted by y (in LVGL's heap), and the tallest one's height.
Rect *rects = nullptr;
uint32_t rect_count = 0;
uint16_t max_h = 0;

// Cut the art into runs per row and merge runs identical in consecutive rows into rectangles.
void build(void (*render)(int y, uint16_t *row)) {
  struct Run {
    uint16_t x, w, color, y0;
  };
  std::vector<Rect> out;
  std::vector<Run> open, next;
  uint16_t row[W];
  for (int y = 0; y <= H; y++) {
    next.clear();
    if (y < H) {
      render(y, row);
      for (int x = 0; x < W;) {
        int start = x;
        while (x < W && row[x] == row[start]) {
          x++;
        }
        next.push_back({(uint16_t)start, (uint16_t)(x - start), row[start], (uint16_t)y});
      }
    }
    // Both lists are sorted by x: a run continues if the previous row has the same one.
    size_t j = 0;
    for (const Run &run : open) {
      while (j < next.size() && next[j].x < run.x) {
        j++;
      }
      if (j < next.size() && next[j].x == run.x && next[j].w == run.w && next[j].color == run.color) {
        next[j].y0 = run.y0;
      } else {
        out.push_back({run.x, run.y0, run.w, (uint16_t)(y - run.y0), run.color});
      }
    }
    std::swap(open, next);
  }
  std::sort(out.begin(), out.end(), [](const Rect &a, const Rect &b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });

  rects = static_cast<Rect *>(lv_malloc(out.size() * sizeof(Rect)));
  if (rects == nullptr) {
    LV_LOG_WARN("No memory for the background's %u rectangles.", (unsigned)out.size());
    return;
  }
  memcpy(rects, out.data(), out.size() * sizeof(Rect));
  rect_count = out.size();
  max_h = 0;
  for (const Rect &r : out) {
    max_h = std::max(max_h, r.h);
  }
}

void draw(lv_event_t *event) {
  lv_obj_t *screen = (lv_obj_t *)lv_event_get_current_target(event);
  const int32_t screen_w = lv_obj_get_width(screen);
  const int32_t screen_h = lv_obj_get_height(screen);
  if (rects == nullptr || screen_w > screen_h) {
    return;
  }
  // The art is 320x480. A wider screen (the custom PCB's 480x480) gets it scaled to its width, the
  // proportions kept, and its bottom on the screen's: what is cut off is sky. Scale and offset in
  // 1/256 px; on a 320x480 screen they are 1 and 0, and every rectangle lands where it always did.
  const int32_t scale = screen_w * 256 / W;
  const int32_t offset = H * scale / 256 - screen_h;
  auto sx = [scale](int32_t x) { return x * scale / 256; };
  auto sy = [scale, offset](int32_t y) { return y * scale / 256 - offset; };
  lv_layer_t *layer = lv_event_get_layer(event);
  const lv_area_t &clip = layer->_clip_area;
  // The first rectangle that can reach the band: none starting more than max_h above it (in the
  // art's rows).
  int32_t from = (clip.y1 + offset) * 256 / scale - max_h;
  const Rect *first = std::lower_bound(rects, rects + rect_count, from, [](const Rect &r, int32_t y) { return (int32_t)r.y < y; });
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.opa = LV_OPA_COVER;
  for (const Rect *r = first; r < rects + rect_count && sy(r->y) <= clip.y2; r++) {
    // Edges scaled, not sizes, so that neighbours still meet without a gap.
    lv_area_t area = {sx(r->x), sy(r->y), sx(r->x + r->w) - 1, sy(r->y + r->h) - 1};
    if (area.y2 < clip.y1 || area.x2 < area.x1 || area.y2 < area.y1) {
      continue;
    }
    // Exact in RGB565: the low bits LVGL drops are zero.
    fill.color = lv_color_make(((r->color >> 11) & 31) << 3, ((r->color >> 5) & 63) << 2, (r->color & 31) << 3);
    lv_draw_fill(layer, &fill, &area);
  }
}

} // namespace

Background::Preset Background::from_name(const char *name) {
  for (const auto &p : presets) {
    if (name != nullptr && strcmp(name, p.name) == 0) {
      return p.preset;
    }
  }
  return Preset::NONE;
}

const char *Background::name(Preset preset) {
  for (const auto &p : presets) {
    if (p.preset == preset) {
      return p.name;
    }
  }
  return "none";
}

void Background::attach(lv_obj_t *screen) {
  // After the screen's own background, before its children.
  lv_obj_add_event_cb(screen, draw, LV_EVENT_DRAW_MAIN_END, NULL);
}

const Theme::Palette &Background::palette_for(Preset preset, const Theme::Palette *theme) {
  if (theme != nullptr) {
    return *theme;
  }
  for (const auto &p : presets) {
    if (p.preset == preset) {
      return *p.palette;
    }
  }
  return Theme::DARK;
}

void Background::set(Preset preset, const Theme::Palette *theme) {
  const Theme::Palette &palette = palette_for(preset, theme);
  Theme::set_palette(palette);
  if (palette.light) {
    preset = Preset::NONE;
  }
  if (preset == current_preset && (rects != nullptr || preset == Preset::NONE)) {
    return;
  }
  lv_free(rects);
  rects = nullptr;
  rect_count = 0;
  current_preset = preset;
  uint8_t card_opa = LV_OPA_COVER;
  for (const auto &p : presets) {
    if (p.preset == preset) {
      build(p.render);
      card_opa = rects != nullptr ? p.card_opa : LV_OPA_COVER;
    }
  }
  Theme::set_card_opa(card_opa);
}

Background::Preset Background::current() {
  return current_preset;
}

void Background::render_row(Preset preset, int y, uint16_t *row) {
  for (const auto &p : presets) {
    if (p.preset == preset) {
      p.render(y, row);
      return;
    }
  }
  std::fill(row, row + W, 0);
}
