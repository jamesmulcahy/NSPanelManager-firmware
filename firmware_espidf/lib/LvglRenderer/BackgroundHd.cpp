#if defined(NSPM_HD_ART)
#include "BackgroundHd.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// Every scene is drawn a row at a time, in float RGB (0-255), then dithered to RGB565. What varies along
// a row only (ridge and crest heights, ray strengths) is worked out once per column; points (stars,
// glints, lights) are small Gaussian splats; the rest is a short sum per pixel. Coordinates are the
// screen's pixels, y down. The scenes were drawn first in a Python prototype (bgart.py) and ported.

namespace {

constexpr int N = BackgroundHd::SIZE;

struct C {
  float r, g, b;
};
inline C operator+(C a, C b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline C operator-(C a, C b) { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
inline C operator*(C a, float k) { return {a.r * k, a.g * k, a.b * k}; }
inline C mix(C a, C b, float t) { return a + (b - a) * t; }

struct Stop {
  float t;
  C c;
};

inline float clamp01(float t) { return t < 0 ? 0 : (t > 1 ? 1 : t); }

// 0 below e0, 1 above e1 (or the other way round when e0 > e1), smooth in between.
inline float smoothstep(float e0, float e1, float x) {
  float t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}

// e^x to ~1e-4, for the many soft falloffs: 2^i from the exponent bits, 2^f from a polynomial.
inline float fexp(float x) {
  if (x < -30) {
    return 0;
  }
  if (x > 30) {
    x = 30;
  }
  float t = x * 1.44269504f;
  float fi = floorf(t);
  float f = t - fi;
  float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
  union {
    float f;
    int32_t i;
  } u;
  u.i = ((int32_t)fi + 127) << 23;
  return p * u.f;
}

C ramp(const Stop *stops, int count, float t) {
  t = clamp01(t);
  for (int i = 0; i + 1 < count; i++) {
    if (t <= stops[i + 1].t) {
      float span = stops[i + 1].t - stops[i].t;
      return mix(stops[i].c, stops[i + 1].c, span > 0 ? (t - stops[i].t) / span : 0);
    }
  }
  return stops[count - 1].c;
}

// One row of the picture.
struct Row {
  float r[N], g[N], b[N];
  void fill(C c) {
    std::fill(r, r + N, c.r);
    std::fill(g, g + N, c.g);
    std::fill(b, b + N, c.b);
  }
  C at(int x) const { return {r[x], g[x], b[x]}; }
  void set(int x, C c) {
    r[x] = c.r;
    g[x] = c.g;
    b[x] = c.b;
  }
  // Paint `c` over pixel x at opacity a.
  void over(int x, C c, float a) {
    if (a <= 0) {
      return;
    }
    if (a >= 1) {
      set(x, c);
      return;
    }
    r[x] += (c.r - r[x]) * a;
    g[x] += (c.g - g[x]) * a;
    b[x] += (c.b - b[x]) * a;
  }
  // Add light.
  void add(int x, C c, float k) {
    r[x] += c.r * k;
    g[x] += c.g * k;
    b[x] += c.b * k;
  }
};

// A small deterministic generator, so every panel draws the same picture.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  float next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return (float)((s * 0x2545F4914F6CDD1Dull) >> 40) / 16777216.0f;
  }
  float normal() {
    float u = std::max(next(), 1e-7f), v = next();
    return sqrtf(-2 * logf(u)) * cosf(6.2831853f * v);
  }
};

// Value noise along a row, 0-1: `octaves` layers from `period` px down, each half the last.
std::vector<float> noise1d(uint64_t seed, int octaves, float period, float persistence = 0.5f) {
  Rng rng(seed);
  std::vector<float> out(N, 0.0f);
  float amp = 1, total = 0;
  for (int o = 0; o < octaves; o++) {
    std::vector<float> knots((size_t)(N / period) + 3);
    for (float &k : knots) {
      k = rng.next();
    }
    for (int x = 0; x < N; x++) {
      float p = x / period;
      int i = (int)p;
      float f = p - i;
      f = f * f * (3 - 2 * f);
      out[x] += amp * (knots[i] + (knots[i + 1] - knots[i]) * f);
    }
    total += amp;
    amp *= persistence;
    period /= 2;
  }
  for (float &v : out) {
    v /= total;
  }
  return out;
}

// Value noise in 2D, 0-1, one octave: a hashed lattice, smoothly interpolated. For texture that must not
// line up with the columns.
inline float lattice(int x, int y, uint32_t seed) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (float)(h & 0xFFFFFF) / 16777216.0f;
}
float noise2d(float x, float y, uint32_t seed) {
  float fx = floorf(x), fy = floorf(y);
  int xi = (int)fx, yi = (int)fy;
  float u = x - fx, v = y - fy;
  u = u * u * (3 - 2 * u);
  v = v * v * (3 - 2 * v);
  float a = lattice(xi, yi, seed), b = lattice(xi + 1, yi, seed), c = lattice(xi, yi + 1, seed), d = lattice(xi + 1, yi + 1, seed);
  return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

// A soft point of light: a Gaussian of `sigma` px, added at `gain`.
struct Splat {
  float x, y, sigma, gain;
  C color;
};

void draw_splats(const std::vector<Splat> &splats, int y, Row &row) {
  for (const Splat &s : splats) {
    int r = (int)ceilf(s.sigma * 3);
    float dy = y - s.y;
    if (dy < -r - 1 || dy > r + 1) {
      continue;
    }
    int x0 = std::max(0, (int)s.x - r), x1 = std::min(N, (int)s.x + r + 2);
    float k = -1 / (2 * s.sigma * s.sigma);
    for (int x = x0; x < x1; x++) {
      float dx = x - s.x;
      row.add(x, s.color, fexp((dx * dx + dy * dy) * k) * s.gain);
    }
  }
}

// A sky's stars: most faint, a few bright with a halo; fading out towards `fade_to` (the horizon).
// `mask` (may be null) dims them where something bright is in front.
template <typename Mask> void add_stars(std::vector<Splat> &out, uint64_t seed, int count, float ymax, float fade_to, Mask mask) {
  static const C tints[] = {{200, 215, 255}, {255, 240, 220}, {230, 235, 255}};
  Rng rng(seed);
  for (int i = 0; i < count; i++) {
    float x = rng.next() * N, y = powf(rng.next(), 1.3f) * ymax;
    float mag = powf(rng.next(), 3.0f);
    C tint = tints[std::min(2, (int)(rng.next() * 3))];
    float fade = (1 - smoothstep(fade_to * 0.6f, fade_to, y)) * mask(x, y);
    if (fade <= 0) {
      continue;
    }
    out.push_back({x, y, 0.55f + 0.5f * mag, (0.25f + 0.95f * mag) * fade, tint});
    if (mag > 0.75f) {
      out.push_back({x, y, 3.0f, 0.05f * fade, tint});
    }
  }
}
float no_mask(float, float) { return 1; }

// Trees along a line: overlapping triangles (spruce), each `base` - height at its tip.
std::vector<float> tree_line(uint64_t seed, float base, float min_h, float var_h) {
  std::vector<float> top(N, base);
  Rng rng(seed);
  for (float x = -10; x < N + 10;) {
    float w = 8 + rng.next() * 12;
    float h = min_h + rng.next() * var_h * (0.5f + 0.5f * powf(sinf(x / 90), 2));
    for (int xi = std::max(0, (int)(x - w)); xi <= std::min(N - 1, (int)(x + w)); xi++) {
      top[xi] = std::min(top[xi], base - h * (1 - fabsf(xi - x) / w));
    }
    x += 4 + rng.next() * 9;
  }
  return top;
}

// A scene draws one row (y) into `row`.
struct Scene {
  virtual ~Scene() = default;
  virtual void row(int y, Row &row) = 0;
};

// --- Ocean: layered swells under a moon path; nearer swells bigger, brighter and sharper ----------------

struct Ocean : Scene {
  static constexpr int LAYERS = 9;
  static constexpr float PATH_X = 470;
  struct Layer {
    float face, soft, lip_w, lip_k;
    C lit, deep;
    std::vector<float> crest, glint;
  };
  Layer layers[LAYERS];
  float path[N];
  std::vector<Splat> glints;

  Ocean() {
    for (int x = 0; x < N; x++) {
      float u = (x - PATH_X) / 120;
      path[x] = fexp(-u * u);
    }
    Rng rng(3);
    for (int k = 0; k < LAYERS; k++) {
      Layer &l = layers[k];
      float z = (float)k / (LAYERS - 1); // 0 far (top) .. 1 near (bottom)
      float base = 40 + 640 * powf(z, 1.35f), amp = 4 + 30 * z;
      float p1 = 40 + 120 * z, p2 = 17 + 40 * z;
      float ph1 = rng.next() * 6.28f, ph2 = rng.next() * 6.28f;
      std::vector<float> n = noise1d(10 + k, 3, 90 + 80 * z);
      l.crest.resize(N);
      for (int x = 0; x < N; x++) {
        l.crest[x] = base + amp * sinf(x / p1 + ph1) + 0.35f * amp * sinf(x / p2 + ph2) + (n[x] - 0.5f) * amp * 0.8f;
      }
      l.glint = noise1d(40 + k, 4, 60);
      l.face = 26 + 70 * z;  // how far down the face the light reaches
      l.soft = 0.8f + 3 * (1 - z); // far edges blur
      l.lip_w = 0.9f + 1.2f * z;
      l.lip_k = 0.4f + 0.6f * z;
      float haze = 1 - z; // far swells take the haze's colour: low contrast
      C lit = mix(C{22, 62, 96}, C{40, 110, 150}, z);
      l.lit = lit * (1 - 0.5f * haze) + C{30, 70, 104} * (0.5f * haze);
      l.deep = mix(C{10, 30, 56}, C{10, 34, 64}, z);
    }
    for (int i = 0; i < 140; i++) {
      float x = PATH_X + rng.normal() * 60, y = 60 + powf(rng.next(), 0.8f) * 640;
      glints.push_back({x, y, 0.5f + rng.next() * 0.6f, 0.20f + 0.5f * powf(rng.next(), 3), {170, 230, 250}});
    }
  }

  void row(int y, Row &row) override {
    static const Stop water[] = {{0, {4, 14, 30}}, {0.18f, {10, 30, 56}}, {0.55f, {8, 24, 46}}, {1, {5, 16, 34}}};
    C base = ramp(water, 4, (float)y / N);
    base = mix(base, C{36, 78, 110}, 0.55f * fexp(-y / 70.0f)); // haze at the far horizon (the top)
    const float path_fall = 0.30f * fexp(-y / 260.0f);
    for (int x = 0; x < N; x++) {
      C c = mix(base, C{40, 96, 132}, path_fall * path[x]);
      // The nearest swell that covers this pixel completely hides the ones behind it: start there.
      int first = 0;
      for (int k = LAYERS - 1; k >= 0; k--) {
        if (y - layers[k].crest[x] >= layers[k].soft) {
          first = k;
          break;
        }
      }
      for (int k = first; k < LAYERS; k++) {
        const Layer &l = layers[k];
        float d = y - l.crest[x]; // > 0 below the crest
        if (d < -std::max(l.soft, 4 * l.lip_w)) {
          continue;
        }
        float dp = std::max(d, 0.0f);
        C col = l.deep + (l.lit - l.deep) * fexp(-dp / l.face);
        col = col + C{30, 70, 80} * (0.5f * path[x] * fexp(-dp / (l.face * 0.5f)));
        c = mix(c, col, smoothstep(-l.soft, l.soft, d));
        // The crest's lip: a thin bright line, brightest in the moon's path.
        float q = d / l.lip_w;
        float lip = fexp(-q * q) * (0.10f + 0.35f * path[x]) * (0.4f + l.glint[x]) * l.lip_k;
        c = mix(c, C{150, 220, 240}, clamp01(lip));
      }
      row.set(x, c);
    }
    draw_splats(this->glints, y, row);
  }
};

// --- Aurora: rayed curtains over a starry sky, a tree line, and a still lake that mirrors it ------------

struct Aurora : Scene {
  static constexpr float LAKE = 560;
  struct Curtain {
    float height;
    C lo, mid, hi;
    std::vector<float> base, reach, gain;
  };
  Curtain curtains[3];
  std::vector<float> hills, trees;
  std::vector<Splat> stars;
  Row scratch;

  Aurora() {
    static const struct {
      float base, amp, period, phase, height;
      C lo, mid, hi;
      float k;
    } spec[] = {
        {300, 46, 150, 0.4f, 230, {60, 235, 150}, {40, 190, 170}, {110, 70, 170}, 1.00f},
        {380, 30, 210, 2.3f, 170, {50, 200, 160}, {30, 150, 160}, {70, 60, 140}, 0.65f},
        {230, 60, 110, 4.0f, 150, {40, 170, 150}, {30, 120, 150}, {60, 40, 120}, 0.45f},
    };
    for (int i = 0; i < 3; i++) {
      Curtain &c = curtains[i];
      const auto &s = spec[i];
      c.height = s.height;
      c.lo = s.lo;
      c.mid = s.mid;
      c.hi = s.hi;
      c.base.resize(N);
      c.reach.resize(N);
      c.gain.resize(N);
      for (int x = 0; x < N; x++) {
        c.base[x] = s.base + s.amp * sinf(x / s.period + s.phase) + 0.3f * s.amp * sinf(x / (s.period * 0.37f) + 2 * s.phase);
      }
      // Rays: fine noise along the curtain. Folds: where it turns, the sheet is seen edge-on, brighter.
      std::vector<float> rays = noise1d(100 + i, 5, 120, 0.65f), env = noise1d(200 + i, 2, 300);
      for (int x = 0; x < N; x++) {
        float slope = fabsf(c.base[std::min(N - 1, x + 1)] - c.base[std::max(0, x - 1)]) / (x == 0 || x == N - 1 ? 1 : 2);
        float r = 0.35f + 0.65f * powf(rays[x], 1.5f);
        float fold = 1 + 1.6f * std::min(slope, 1.2f);
        float envelope = smoothstep(0, 140, x) * smoothstep(N, N - 140, x) * (0.6f + 0.4f * env[x]);
        c.reach[x] = s.height * (0.7f + 0.6f * r);
        c.gain[x] = r * fold * envelope * s.k * 0.85f;
      }
    }
    std::vector<float> n = noise1d(7, 3, 120);
    hills.resize(N);
    for (int x = 0; x < N; x++) {
      hills[x] = 500 + 26 * sinf(x / 130.0f + 1.0f) + 12 * sinf(x / 47.0f + 0.3f) + (n[x] - 0.5f) * 20;
    }
    trees = tree_line(9, LAKE - 6, 18, 46);
    add_stars(stars, 5, 520, LAKE - 40, LAKE - 20, [this](float x, float y) {
      C a = this->light(std::min(N - 1, (int)x), y);
      return 1 - std::min(0.85f, std::max(0.0f, (a.r + a.g + a.b) / 3 / 60));
    });
  }

  // The curtains' light at (x, y).
  C light(int x, float y) const {
    C sum = {0, 0, 0};
    for (const Curtain &c : curtains) {
      float h = y - c.base[x]; // > 0 below its lower edge
      float up;
      if (h < 0) {
        if (h < -c.reach[x] * 12) {
          continue;
        }
        up = fexp(h / c.reach[x]);
      } else {
        if (h > 80) {
          continue;
        }
        up = fexp(-h / 7);
      }
      float t = clamp01(-h / c.height);
      C col = t < 0.35f ? mix(c.lo, c.mid, t / 0.35f) : mix(c.mid, c.hi, (t - 0.35f) / 0.65f);
      sum = sum + col * (up * c.gain[x]);
    }
    return sum;
  }

  // Above the lake: sky, curtains, stars, the far hills and the trees.
  void land(int y, Row &row) const {
    static const Stop sky[] = {{0, {3, 6, 18}}, {0.45f, {8, 16, 38}}, {0.75f, {12, 28, 50}}, {1, {14, 34, 52}}};
    row.fill(ramp(sky, 4, y / LAKE));
    for (int x = 0; x < N; x++) {
      row.add(x, this->light(x, y), 1);
    }
    draw_splats(this->stars, y, row);
    C hill = mix(C{14, 30, 46}, C{10, 22, 34}, clamp01((y - 480) / 80.0f));
    for (int x = 0; x < N; x++) {
      row.over(x, hill, smoothstep(-1.5f, 1.5f, y - hills[x]));
      row.over(x, C{3, 9, 12}, smoothstep(-0.8f, 0.8f, y - trees[x]));
    }
  }

  void row(int y, Row &row) override {
    if (y < LAKE) {
      this->land(y, row);
    } else {
      // The lake: the land above mirrored, darker and a little rippled.
      int src = std::min(N - 1, std::max(0, (int)(2 * LAKE) - y - 2));
      this->land(src, this->scratch);
      float depth = (y - LAKE) / (N - LAKE);
      int shift = (int)lroundf(2.2f * sinf(y / 3.1f) * depth);
      float dark = 0.42f - 0.18f * depth;
      for (int x = 0; x < N; x++) {
        int sx = ((x - shift) % N + N) % N;
        row.set(x, this->scratch.at(sx) * dark + C{4, 10, 16} * (1 - dark));
      }
    }
    float line = 0.25f * fexp(-fabsf(y - LAKE) / 2); // the shore's faint teal haze
    for (int x = 0; x < N && line > 0.002f; x++) {
      row.over(x, C{60, 120, 120}, line);
    }
  }
};

// --- Parthenon: the floodlit octastyle front, shaded and fluted columns, a moon with a halo -------------

struct Parthenon : Scene {
  static constexpr float HORIZON = 560, PW = 600, CX = 360, X0 = CX - PW / 2, X1 = CX + PW / 2;
  static constexpr float STYLOBATE = 500, COL_H = 0.24f * PW, CAPITAL = STYLOBATE - COL_H;
  static constexpr float ARCHI = CAPITAL - 16, FRIEZE = ARCHI - 18, CORNICE = FRIEZE - 8, APEX = CORNICE - 0.12f * PW;
  static constexpr float MX = 584, MY = 118, MR = 40;
  static constexpr int COLUMNS = 8;
  std::vector<Splat> stars;

  Parthenon() { add_stars(stars, 11, 300, 420, 430, no_mask); }

  static float box(float x, float y, float xa, float xb, float ya, float yb, float s = 0.7f) {
    return smoothstep(xa - s, xa + s, x) * smoothstep(xb + s, xb - s, x) * smoothstep(ya - s, ya + s, y) * smoothstep(yb + s, yb - s, y);
  }
  static bool in_rows(float y, float ya, float yb) { return y > ya - 1.5f && y < yb + 1.5f; }

  // Gold lit from below: brighter towards an element's foot, and the whole front brighter lower down.
  static C floodlit(float y, float top, float bottom, float k = 1) {
    static const C gold = {242, 200, 103};
    float t = clamp01((y - top) / std::max(1.0f, bottom - top));
    float whole = clamp01((y - APEX) / (STYLOBATE - APEX));
    return gold * ((0.55f + 0.25f * t + 0.25f * whole) * k);
  }

  void row(int yi, Row &row) override {
    static const Stop sky[] = {{0, {3, 6, 18}}, {0.55f, {12, 20, 46}}, {1, {32, 44, 84}}};
    static const C gold = {242, 200, 103};
    const float y = (float)yi;
    row.fill(ramp(sky, 3, y / HORIZON));
    // Warm light over the city behind the hill, low on the sky.
    const float spill_y = (y - HORIZON) / 120;
    for (int x = 0; x < N; x++) {
      float u = (x - 360) / 380.0f;
      row.over(x, C{110, 80, 70}, 0.22f * fexp(-u * u - spill_y * spill_y));
    }
    draw_splats(this->stars, yi, row);
    // The moon: a wide halo, then the disc, darker at the limb, with two soft maria.
    for (int x = 0; x < N; x++) {
      float dx = x - MX, dy = y - MY, d = sqrtf(dx * dx + dy * dy);
      float out = std::max(d - MR, 0.0f);
      if (out < 600) {
        row.over(x, C{200, 205, 220}, clamp01(0.22f * fexp(-out / 26) + 0.08f * fexp(-out / 90)));
      }
      if (d < MR + 1) {
        float limb = sqrtf(clamp01(1 - (d / MR) * (d / MR)));
        float m1x = x - MX + 12, m1y = y - MY - 6, m2x = x - MX - 10, m2y = y - MY + 12;
        float maria = 1 - 0.16f * fexp(-(m1x * m1x + m1y * m1y) / 180) - 0.12f * fexp(-(m2x * m2x + m2y * m2y) / 120);
        row.over(x, C{244, 236, 214} * ((0.78f + 0.22f * limb) * maria), smoothstep(MR + 0.8f, MR - 0.8f, d));
      }
    }
    // The floodlights' glow around the front, under everything else of it.
    const float glow_k = 0.28f * smoothstep(STYLOBATE + 40, STYLOBATE - 60, y);
    if (glow_k > 0) {
      for (int x = 0; x < N; x++) {
        float dx = std::max(std::max(X0 - x, x - X1), 0.0f);
        float roof = APEX + (CORNICE - APEX) * clamp01(fabsf(x - CX) / (PW / 2));
        float dy = std::max(std::max(roof - y, y - STYLOBATE), 0.0f);
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < 300) {
          row.over(x, C{200, 150, 80}, glow_k * fexp(-dist / 34));
        }
      }
    }
    // The cella behind the columns: dark, warm, a little lit from below.
    if (in_rows(y, CAPITAL, STYLOBATE)) {
      C shadow = C{62, 42, 30} * (0.6f + 0.6f * clamp01((y - CAPITAL) / COL_H));
      for (int x = (int)X0 + 12; x <= (int)X1 - 12; x++) {
        row.over(x, shadow, box(x, y, X0 + 14, X1 - 14, CAPITAL, STYLOBATE));
      }
    }
    // Columns: tapered shafts (entasis), cylinder shading, 20 flutes; an echinus and an abacus on each.
    const float pitch = (PW - 30) / COLUMNS;
    for (int i = 0; i < COLUMNS; i++) {
      float ccx = X0 + 15 + pitch * (i + 0.5f), rb = pitch * 0.33f;
      int xa = (int)(ccx - rb * 1.4f) - 2, xb = (int)(ccx + rb * 1.4f) + 2;
      if (in_rows(y, CAPITAL + 6, STYLOBATE)) {
        float r = rb * (1 - 0.14f * clamp01((STYLOBATE - y) / COL_H));
        C lit = floodlit(y, CAPITAL, STYLOBATE);
        float rows = box(CX, y, 0, N, CAPITAL + 6, STYLOBATE, 0.6f);
        for (int x = xa; x <= xb; x++) {
          float u = (x - ccx) / r;
          float inside = smoothstep(1 + 0.8f / rb, 1 - 0.8f / rb, fabsf(u)) * rows;
          if (inside <= 0) {
            continue;
          }
          float cu = clamp01(1 - u * u);
          float shade = 0.35f + 0.65f * powf(sqrtf(cu), 0.8f) + 0.12f * clamp01(-u); // lit a little from the left
          float flutes = 1 - 0.07f * (0.5f + 0.5f * cosf(asinf(std::max(-1.0f, std::min(1.0f, u))) * 20));
          row.over(x, lit * (shade * flutes), inside);
        }
      }
      if (in_rows(y, CAPITAL + 1, CAPITAL + 6)) {
        C lit = floodlit(y, CAPITAL - 4, CAPITAL + 6, 0.95f);
        for (int x = xa; x <= xb; x++) {
          float w = rb * (0.95f + 0.35f * (CAPITAL + 6 - y) / 5);
          row.over(x, lit, box(x, y, ccx - rb * 1.25f, ccx + rb * 1.25f, CAPITAL + 1, CAPITAL + 6) * smoothstep(1.3f, 0.9f, fabsf((x - ccx) / w)));
        }
      }
      if (in_rows(y, CAPITAL - 4, CAPITAL + 1)) {
        C lit = floodlit(y, CAPITAL - 4, CAPITAL, 0.9f);
        for (int x = xa; x <= xb; x++) {
          row.over(x, lit, box(x, y, ccx - rb * 1.35f, ccx + rb * 1.35f, CAPITAL - 4, CAPITAL + 1));
        }
      }
    }
    // The entablature: architrave, the frieze's triglyphs and metopes, the cornice and its shadowed drip.
    if (in_rows(y, CORNICE, CAPITAL - 4)) {
      C architrave = floodlit(y, ARCHI, CAPITAL - 4, 0.92f), frieze = floodlit(y, FRIEZE, ARCHI, 0.80f), cornice = floodlit(y, CORNICE, FRIEZE, 1.05f);
      for (int x = (int)X0 - 8; x <= (int)X1 + 8; x++) {
        row.over(x, architrave, box(x, y, X0 + 2, X1 - 2, ARCHI, CAPITAL - 4));
        row.over(x, frieze, box(x, y, X0 + 2, X1 - 2, FRIEZE, ARCHI));
        float tri = sinf((x - X0 - 2) / ((PW - 4) / 32) * 3.14159265f) > 0 ? 1.0f : 0.0f; // 16 triglyph-metope pairs
        row.over(x, C{70, 48, 30}, 0.45f * tri * box(x, y, X0 + 2, X1 - 2, FRIEZE + 2, ARCHI - 1));
        row.over(x, cornice, box(x, y, X0 - 6, X1 + 6, CORNICE, FRIEZE));
        row.over(x, C{40, 28, 22}, 0.55f * box(x, y, X0 - 4, X1 + 4, FRIEZE, FRIEZE + 3));
      }
    }
    // The pediment: the raking cornice, and the tympanum recessed in it.
    if (in_rows(y, APEX, CORNICE)) {
      constexpr float half = PW / 2 + 6, inset = 9;
      C outer = floodlit(y, APEX, CORNICE, 1.0f), inner = floodlit(y, APEX, CORNICE, 0.62f);
      float rows_outer = box(CX, y, 0, N, APEX, CORNICE), rows_inner = box(CX, y, 0, N, APEX + inset, CORNICE - 2);
      for (int x = (int)X0 - 8; x <= (int)X1 + 8; x++) {
        row.over(x, outer, smoothstep(-0.8f, 0.8f, half * (y - APEX) / (CORNICE - APEX) - fabsf(x - CX)) * rows_outer);
        row.over(x, inner, smoothstep(-0.8f, 0.8f, (half - inset * 2.6f) * (y - APEX - inset) / (CORNICE - APEX - inset) - fabsf(x - CX)) * rows_inner);
      }
    }
    // The forecourt, three steps on it (each tread lit, each riser darker), and the light's falloff.
    float floor_a = smoothstep(STYLOBATE - 0.5f, STYLOBATE + 0.5f, y);
    if (floor_a > 0) {
      C floor = mix(C{22, 22, 40}, C{8, 12, 26}, clamp01((y - STYLOBATE) / (N - STYLOBATE)));
      for (int x = 0; x < N; x++) {
        row.over(x, floor, floor_a);
      }
      for (int s = 0; s < 3; s++) {
        float ya = STYLOBATE + s * 20, grow = 8 + s * 14;
        if (!in_rows(y, ya, ya + 20)) {
          continue;
        }
        C tread = gold * (0.62f - 0.08f * s), riser = gold * ((0.40f - 0.06f * s) * (1 - 0.35f * clamp01((y - ya - 5) / 15)));
        for (int x = std::max(0, (int)(X0 - grow) - 2); x <= std::min(N - 1, (int)(X1 + grow) + 2); x++) {
          row.over(x, tread, box(x, y, X0 - grow, X1 + grow, ya, ya + 5));
          row.over(x, riser, box(x, y, X0 - grow, X1 + grow, ya + 5, ya + 20));
        }
      }
      float court = 0.18f * fexp(-std::max(y - STYLOBATE - 60, 0.0f) / 40) * smoothstep(STYLOBATE + 58, STYLOBATE + 62, y);
      if (court > 0.001f) {
        for (int x = 0; x < N; x++) {
          float u = (x - CX) / 340;
          row.over(x, C{150, 110, 60}, court * fexp(-u * u));
        }
      }
    }
  }
};

// A range of hills or mountains, far (haze) to near, as one silhouette with a lit ridge.
struct Ridge {
  std::vector<float> top;   // the ridge's y per column
  std::vector<float> slope; // its slope per column (dy/dx), for the light on each face
};

Ridge make_ridge(uint64_t seed, float base, float amp, float period, int octaves, bool sharp, float waves_amp, float waves_period, float phase) {
  Ridge r;
  std::vector<float> n = noise1d(seed, octaves, period, 0.5f);
  r.top.resize(N);
  for (int x = 0; x < N; x++) {
    float v = n[x];
    if (sharp) {
      v = 1 - fabsf(2 * v - 1); // ridged: sharp peaks where the noise crosses its middle
      v = v * v;
    }
    r.top[x] = base - amp * v + waves_amp * sinf(x / waves_period + phase);
  }
  r.slope.resize(N);
  for (int x = 0; x < N; x++) {
    r.slope[x] = (r.top[std::min(N - 1, x + 1)] - r.top[std::max(0, x - 1)]) / 2;
  }
  return r;
}

// Thin clouds lit from below: streaks along y_center(x), `thick` px, broken up by noise.
struct Streak {
  std::vector<float> center, thick, cover;
  C color;
  float alpha;
};

Streak make_streak(uint64_t seed, float y, float tilt, float thick, C color, float alpha) {
  Streak s;
  s.color = color;
  s.alpha = alpha;
  std::vector<float> wobble = noise1d(seed, 3, 160), t = noise1d(seed + 1, 3, 70), c = noise1d(seed + 2, 4, 140);
  s.center.resize(N);
  s.thick.resize(N);
  s.cover.resize(N);
  for (int x = 0; x < N; x++) {
    s.center[x] = y + tilt * (x - N / 2) + (wobble[x] - 0.5f) * 24;
    s.thick[x] = thick * (0.4f + 1.2f * t[x]);
    s.cover[x] = smoothstep(0.42f, 0.68f, c[x]);
  }
  return s;
}

void draw_streak(const Streak &s, float y, Row &row) {
  for (int x = 0; x < N; x++) {
    if (s.cover[x] <= 0) {
      continue;
    }
    float q = (y - s.center[x]) / s.thick[x];
    if (q < -4 || q > 4) {
      continue;
    }
    // Lit from below: the underside brighter than the top.
    float a = s.alpha * s.cover[x] * fexp(-q * q) * (q > 0 ? 1.0f : 0.6f);
    row.over(x, s.color, a);
  }
}

// --- Dusk: a low sun behind layered hills, lit ridges, mist in the valleys, the first stars -------------

struct Dusk : Scene {
  static constexpr float SUN_X = 470, SUN_Y = 486, SUN_R = 46, HORIZON = 540;
  static constexpr int RANGES = 4;
  Ridge ranges[RANGES];
  C fill[RANGES];
  float soft[RANGES], rim[RANGES], mist[RANGES];
  Streak streaks[3];
  std::vector<Splat> stars;

  Dusk() {
    static const struct {
      float base, amp, period;
      C color;
      float soft, rim, mist;
    } spec[] = {
        {478, 70, 240, {150, 84, 112}, 2.0f, 0.55f, 0.00f},
        {528, 80, 200, {98, 54, 98}, 1.4f, 0.65f, 0.30f},
        {588, 70, 260, {58, 34, 72}, 1.0f, 0.75f, 0.30f},
        {650, 60, 300, {30, 20, 46}, 0.8f, 0.85f, 0.25f},
    };
    for (int k = 0; k < RANGES; k++) {
      ranges[k] = make_ridge(20 + k, spec[k].base, spec[k].amp, spec[k].period, 4, false, 10, 90 + 30 * k, k * 1.7f);
      fill[k] = spec[k].color;
      soft[k] = spec[k].soft;
      rim[k] = spec[k].rim;
      mist[k] = spec[k].mist;
    }
    streaks[0] = make_streak(31, 250, -0.04f, 4, {232, 140, 130}, 0.55f);
    streaks[1] = make_streak(32, 330, 0.03f, 6, {250, 160, 120}, 0.55f);
    streaks[2] = make_streak(33, 410, -0.02f, 5, {255, 190, 130}, 0.6f);
    add_stars(stars, 13, 110, 260, 300, [](float, float y) { return 0.6f * (1 - y / 300); });
  }

  void row(int yi, Row &row) override {
    static const Stop sky[] = {{0, {18, 18, 48}}, {0.35f, {52, 36, 88}}, {0.62f, {140, 70, 100}}, {0.82f, {222, 118, 86}}, {1, {248, 176, 100}}};
    const float y = (float)yi;
    row.fill(ramp(sky, 5, y / HORIZON));
    draw_splats(this->stars, yi, row);
    // The sun's glow, wide and warm, then the disc: pale at the top, deeper orange towards the hills.
    for (int x = 0; x < N; x++) {
      float dx = x - SUN_X, dy = y - SUN_Y, d = sqrtf(dx * dx + dy * dy), out = std::max(d - SUN_R, 0.0f);
      row.over(x, C{255, 190, 120}, clamp01(0.35f * fexp(-out / 60) + 0.18f * fexp(-out / 220)));
      if (d < SUN_R + 1) {
        C disc = mix(C{255, 238, 190}, C{252, 168, 96}, clamp01((y - (SUN_Y - SUN_R)) / (2 * SUN_R)));
        row.over(x, disc, smoothstep(SUN_R + 1, SUN_R - 1, d));
      }
    }
    for (const Streak &s : this->streaks) {
      draw_streak(s, y, row);
    }
    // The hills, far to near: mist rising behind each nearer range, then its silhouette and lit ridge.
    for (int k = 0; k < RANGES; k++) {
      const Ridge &r = this->ranges[k];
      C haze = mix(ramp(sky, 5, 1.0f), C{170, 100, 120}, 0.5f);
      for (int x = 0; x < N; x++) {
        float d = y - r.top[x];
        if (this->mist[k] > 0 && d < 0 && d > -120) {
          row.over(x, haze, this->mist[k] * fexp(d / 36));
        }
        if (d < -this->soft[k]) {
          continue;
        }
        // Darker lower down, and lit along the ridge, most where it faces the sun.
        C body = this->fill[k] * (1 - 0.25f * clamp01(d / 160));
        float facing = clamp01(1 - fabsf(x - SUN_X) / 520) * (0.6f + 0.4f * clamp01(r.slope[x] * (x < SUN_X ? 1.0f : -1.0f) * 2 + 0.5f));
        body = mix(body, C{255, 170, 120}, this->rim[k] * facing * fexp(-std::max(d, 0.0f) / 2.5f));
        row.over(x, body, smoothstep(-this->soft[k], this->soft[k], d));
      }
    }
  }
};

// --- Peaks: alpine ranges under a clear evening sky; snow on the far summits, mist between, forest -------

struct Peaks : Scene {
  static constexpr int RANGES = 4;
  Ridge ranges[RANGES];
  std::vector<float> mist_top[RANGES]; // the ridge smoothed: the mist behind it follows its outline, not its teeth
  std::vector<float> snow_noise[RANGES];
  std::vector<float> forest;
  C lit[RANGES], shade[RANGES];
  float snow[RANGES], snow_line[RANGES], haze[RANGES], base[RANGES];

  Peaks() {
    static const struct {
      float base, amp, period;
      C lit, shade;
      float snow, snow_line, haze;
    } spec[] = {
        {380, 200, 260, {136, 156, 190}, {84, 102, 140}, 34, 290, 0.42f},
        {460, 170, 220, {100, 122, 158}, {58, 78, 114}, 22, 350, 0.28f},
        {540, 120, 200, {64, 84, 116}, {40, 56, 86}, 0, 0, 0.2f},
        {640, 70, 260, {34, 50, 72}, {22, 34, 52}, 0, 0, 0.0f},
    };
    for (int k = 0; k < RANGES; k++) {
      ranges[k] = make_ridge(50 + k, spec[k].base, spec[k].amp, spec[k].period, 5, k < 3, 6, 70 + 25 * k, k * 2.1f);
      // The faces' slope, averaged over a few columns, so the light changes along a face, not per column.
      std::vector<float> smooth(N);
      for (int x = 0; x < N; x++) {
        float sum = 0;
        int n = 0;
        for (int i = std::max(0, x - 6); i <= std::min(N - 1, x + 6); i++, n++) {
          sum += ranges[k].slope[i];
        }
        smooth[x] = sum / n;
      }
      ranges[k].slope = smooth;
      mist_top[k].resize(N);
      for (int x = 0; x < N; x++) {
        float sum = 0;
        int n = 0;
        for (int i = std::max(0, x - 40); i <= std::min(N - 1, x + 40); i++, n++) {
          sum += ranges[k].top[i];
        }
        mist_top[k][x] = sum / n;
      }
      snow_noise[k] = noise1d(60 + k, 3, 90);
      lit[k] = spec[k].lit;
      shade[k] = spec[k].shade;
      snow[k] = spec[k].snow;
      snow_line[k] = spec[k].snow_line;
      base[k] = spec[k].base;
      haze[k] = spec[k].haze;
    }
    forest = tree_line(71, 0, 10, 30); // heights above the near range's ridge, added per column
  }

  void row(int yi, Row &row) override {
    static const Stop sky[] = {{0, {28, 56, 100}}, {0.55f, {80, 122, 166}}, {1, {156, 184, 206}}};
    static const C horizon = {156, 184, 206};
    const float y = (float)yi;
    row.fill(ramp(sky, 3, y / 560));
    // The sun, down to the right behind the ranges: a warm glow on the sky.
    for (int x = 0; x < N; x++) {
      float dx = x - 620, dy = y - 330, d = sqrtf(dx * dx + dy * dy);
      row.over(x, C{255, 226, 196}, 0.30f * fexp(-d / 200));
    }
    for (int k = 0; k < RANGES; k++) {
      const Ridge &r = this->ranges[k];
      bool forest = k == RANGES - 1;
      for (int x = 0; x < N; x++) {
        float top = r.top[x];
        if (forest) {
          top += this->forest[x]; // the tree line along the near ridge
        }
        float d = y - top;
        // Valley mist: the air behind each range paler down towards it.
        float m = y - this->mist_top[k][x];
        if (k > 0 && m > -100 && d < 0) {
          row.over(x, horizon, 0.28f * fexp(std::min(m, 0.0f) / 30));
        }
        if (d < -1) {
          continue;
        }
        // The light from the right: faces turned to it (the ridge falling to the right) lit, the rest in
        // shade. The ridge's slope says which way a face turns only near it: lower down, an even half-light.
        float near_ridge = fexp(-std::max(d, 0.0f) / 9);
        float light = 0.42f + (clamp01(0.5f + r.slope[x] * 1.6f) - 0.42f) * near_ridge;
        float rock = 0.92f + 0.12f * noise2d(x / 30.0f, y / 14.0f, 90 + k) + 0.04f * noise2d(x / 8.0f, y / 5.0f, 95 + k);
        C c = mix(this->shade[k], this->lit[k], light) * rock;
        // Snow on the summits, its lower edge ragged.
        if (this->snow[k] > 0) {
          float depth = this->snow[k] * (0.6f + 0.6f * this->snow_noise[k][x]) * (0.75f + 0.5f * noise2d(x / 11.0f, y / 6.0f, 99 + k));
          // Only on the summits above the range's snow line.
          float s = smoothstep(depth, depth * 0.6f, d) * smoothstep(this->snow_line[k] + 30, this->snow_line[k] - 30, r.top[x]);
          C white = mix(C{150, 170, 205}, C{240, 244, 250}, light);
          c = mix(c, white, s);
        }
        // Distance: the far ranges fade into the sky's horizon colour; nearer, darker lower down (by the
        // row, not the depth under the ridge, which jumps from column to column at the peaks).
        float low = clamp01((y - this->base[k] + 120) / 300);
        c = mix(c, horizon, this->haze[k] * (1 - 0.4f * low));
        c = c * (1 - 0.18f * low);
        row.over(x, c, smoothstep(-1, 1, d));
      }
    }
  }
};

// --- Vesuvius: the bay at sunset, the volcano's twin peaks against the glow with a wisp of smoke, the
// lights of the shore, and the sun's path on the sea --------------------------------------------------------

struct Vesuvius : Scene {
  static constexpr float HORIZON = 440, SUN_X = 150, SUN_Y = 418, SUN_R = 42;
  static constexpr float CRATER_X = 432, CRATER_Y = 191;
  std::vector<float> volcano;
  std::vector<Splat> lights;
  Streak streaks[2];

  Vesuvius() {
    // The NSPanel art's outline (Somma on the left, Vesuvius on the right), at 720, smoothed, roughened.
    static const float outline[][2] = {{0, 355}, {70, 331}, {128, 288}, {150, 259}, {173, 273}, {192, 245}, {208, 273}, {269, 321}, {320, 341}};
    std::vector<float> n = noise1d(81, 5, 60);
    volcano.resize(N);
    for (int x = 0; x < N; x++) {
      float ox = x / 2.25f;
      int i = 0;
      while (i < 7 && ox > outline[i + 1][0]) {
        i++;
      }
      float t = clamp01((ox - outline[i][0]) / (outline[i + 1][0] - outline[i][0]));
      t = t * t * (3 - 2 * t);
      float oy = outline[i][1] + (outline[i + 1][1] - outline[i][1]) * t;
      volcano[x] = oy * 2.25f - 360 + (n[x] - 0.5f) * 8;
    }
    // The towns along the shore at the volcano's foot: warm points, more towards the bay's ends.
    Rng rng(83);
    for (int i = 0; i < 90; i++) {
      float x = rng.next() * N;
      float y = HORIZON - 2 - rng.next() * rng.next() * 26;
      if (y < volcano[std::min(N - 1, (int)x)] + 3) {
        continue;
      }
      lights.push_back({x, y, 0.6f + rng.next() * 0.5f, 0.35f + 0.5f * rng.next(), {255, 200, 140}});
    }
    streaks[0] = make_streak(84, 120, 0.05f, 5, {230, 130, 140}, 0.45f);
    streaks[1] = make_streak(85, 300, -0.03f, 6, {255, 180, 120}, 0.55f);
  }

  void row(int yi, Row &row) override {
    static const Stop sky[] = {{0, {24, 20, 46}}, {0.42f, {88, 44, 82}}, {0.72f, {214, 108, 74}}, {1, {246, 182, 98}}};
    static const Stop sea[] = {{0, {132, 64, 80}}, {0.4f, {80, 40, 74}}, {1, {26, 22, 46}}};
    const float y = (float)yi;
    if (y < HORIZON) {
      row.fill(ramp(sky, 4, y / HORIZON));
      for (int x = 0; x < N; x++) {
        float dx = x - SUN_X, dy = y - SUN_Y, d = sqrtf(dx * dx + dy * dy), out = std::max(d - SUN_R, 0.0f);
        row.over(x, C{255, 200, 130}, clamp01(0.40f * fexp(-out / 50) + 0.20f * fexp(-out / 240)));
        if (d < SUN_R + 1) {
          row.over(x, mix(C{255, 240, 196}, C{255, 186, 110}, clamp01((y - SUN_Y + SUN_R) / (2 * SUN_R))), smoothstep(SUN_R + 1, SUN_R - 1, d));
        }
      }
      for (const Streak &s : this->streaks) {
        draw_streak(s, y, row);
      }
      // A wisp of smoke from the crater, drifting up and to the right.
      if (y < CRATER_Y + 4) {
        float rise = CRATER_Y - y;
        float cx = CRATER_X + rise * 0.6f + 16 * sinf(y / 40), w = 8 + rise * 0.22f;
        float a = 0.20f * smoothstep(-4, 12, rise) * fexp(-rise / 160);
        for (int x = std::max(0, (int)(cx - 4 * w)); x < std::min(N, (int)(cx + 4 * w)); x++) {
          float u = (x - cx) / w;
          row.over(x, C{196, 150, 160}, a * fexp(-u * u));
        }
      }
      // The volcano: dark against the glow, hazier near its foot, its slopes towards the sun rim-lit.
      for (int x = 0; x < N; x++) {
        float d = y - this->volcano[x];
        if (d < -1) {
          continue;
        }
        C body = mix(C{44, 26, 52}, ramp(sky, 4, 0.9f), 0.28f * clamp01((y - 300) / (HORIZON - 300)));
        float slope = (this->volcano[std::min(N - 1, x + 1)] - this->volcano[std::max(0, x - 1)]) / 2;
        float facing = clamp01(slope * 1.5f) * clamp01(1 - fabsf(x - SUN_X) / 600); // rising to the right: facing the sun on the left
        body = mix(body, C{250, 160, 110}, 0.6f * facing * fexp(-std::max(d, 0.0f) / 3));
        row.over(x, body, smoothstep(-1, 1, d));
      }
      draw_splats(this->lights, yi, row);
    } else {
      // The sea: the sky's glow on it near the horizon, and the sun's path broken into ripples.
      float depth = (y - HORIZON) / (N - HORIZON);
      C water = ramp(sea, 3, depth);
      water = mix(water, C{220, 120, 90}, 0.4f * fexp(-(y - HORIZON) / 40));
      row.fill(water);
      // Glitter: short horizontal dashes, longer and further apart nearer, thickest in the sun's path.
      float width = 30 + 260 * depth, k = 0.9f * (1 - 0.45f * depth);
      float dash = 6 + 40 * depth, rows = 1.2f + 3.5f * depth;
      for (int x = 0; x < N; x++) {
        float u = (x - SUN_X) / width;
        float path = fexp(-u * u);
        row.over(x, C{200, 110, 90}, 0.25f * path * (1 - depth));
        if (path < 0.01f) {
          continue;
        }
        float n = noise2d(x / dash, y / rows, 120);
        row.over(x, C{255, 214, 150}, smoothstep(0.62f - 0.25f * path, 0.85f, n) * path * k);
      }
      // Further out, a few dashes of the sky's glow on the swell.
      for (int x = 0; x < N; x++) {
        float n = noise2d(x / (dash * 1.5f), y / rows, 121);
        row.over(x, C{190, 110, 120}, 0.25f * smoothstep(0.78f, 0.92f, n) * (1 - depth));
      }
      float line = 0.5f * fexp(-(y - HORIZON) / 1.5f); // the horizon's bright edge
      for (int x = 0; x < N; x++) {
        row.over(x, C{255, 190, 140}, line);
      }
    }
  }
};

// 8x8 ordered dither: each pixel's threshold, so a gradient steps between RGB565's levels evenly.
const uint8_t BAYER[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
                             {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25}, {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};

void to565(const Row &row, int y, uint16_t *out) {
  for (int x = 0; x < N; x++) {
    float d = (BAYER[y & 7][x & 7] + 0.5f) / 64;
    int r = (int)(std::max(0.0f, row.r[x]) / 8 + d), g = (int)(std::max(0.0f, row.g[x]) / 4 + d), b = (int)(std::max(0.0f, row.b[x]) / 8 + d);
    out[x] = (uint16_t)((std::min(r, 31) << 11) | (std::min(g, 63) << 5) | std::min(b, 31));
  }
}

std::unique_ptr<Scene> make(Background::Preset preset) {
  switch (preset) {
  case Background::Preset::OCEAN:
    return std::unique_ptr<Scene>(new Ocean());
  case Background::Preset::DUSK:
    return std::unique_ptr<Scene>(new Dusk());
  case Background::Preset::AURORA:
    return std::unique_ptr<Scene>(new Aurora());
  case Background::Preset::PEAKS:
    return std::unique_ptr<Scene>(new Peaks());
  case Background::Preset::VESUVIUS:
    return std::unique_ptr<Scene>(new Vesuvius());
  case Background::Preset::PARTHENON:
    return std::unique_ptr<Scene>(new Parthenon());
  default:
    return nullptr;
  }
}

} // namespace

bool BackgroundHd::has(Background::Preset preset) {
  return preset != Background::Preset::NONE;
}

bool BackgroundHd::render(Background::Preset preset, uint16_t *out) {
  std::unique_ptr<Scene> scene = make(preset);
  if (!scene) {
    return false;
  }
  std::unique_ptr<Row> row(new Row());
  for (int y = 0; y < N; y++) {
    scene->row(y, *row);
    to565(*row, y, out + (size_t)y * N);
  }
  return true;
}
#endif
