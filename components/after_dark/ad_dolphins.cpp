/*
 * ad_dolphins.cpp - Dolphins, leaping at sea on a dot-matrix display. Not an
 * After Dark module: an original saver in the spirit of the cyan organic-EL
 * car stereos of the early 2000s, whose dolphin clip everyone remembers.
 * Nothing here is taken from that clip (or from any game); the scenes and the
 * dolphins are drawn in code.
 *
 * Everything is worked out on a grid of dots, as intensities, then dithered
 * to on and off: a 1-bit OLED shows the grid as it is, a colour panel shows
 * it as round-cornered cyan dots with a gap, like the stereo's panel. Only
 * the dots that changed are sent.
 *
 * Two scenes take turns: a pod leaping at the surface (by night under a
 * moon, or at dawn against the sun) and a swim underwater, with light from
 * above, kelp and bubbles.
 */
#if defined(AD_SAVER_DOLPHINS) || defined(AD_ALL_SAVERS)
#include "ad_core.h"
#include "ad_text.h"
#include "ad_sky.h"
#include "ad_live.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace adcore {

/* The time in the sky on a character LCD (the YAML's `clock: true`), and
   the date taking turns with it (`date: true`). The readings (sky.readings)
   take turns at the left. */
bool dolphins_clock = false;
bool dolphins_date = false;

namespace {

const float PI = 3.14159265f;
const float FPS = 20;
/* Intensities run 0..FULL; FULL is always lit, 0 never. */
const int FULL = 64;
const int MAX_DOLPHINS = 6;
const int MAX_DROPS = 48;
const int MAX_BUBBLES = 24;
const int MAX_STARS = 48;
/* Seconds a scene lasts, and its fade in and out. */
const float SURFACE_TIME = 32, DEEP_TIME = 22, FADE = 1.2f;

const Rgb LIT = {70, 255, 245};
const Rgb UNLIT = {0, 26, 30};

const uint8_t BAYER[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};

/* sin over 256 steps, as -64..64, and a wave crest: sin cubed where positive,
   so a swell is a thin bright line with dark between. */
int8_t SIN[256];
uint8_t CREST[256];
bool tables = false;

void make_tables() {
  if (tables)
    return;
  for (int i = 0; i < 256; i++) {
    float v = sinf(i * 2 * PI / 256);
    SIN[i] = (int8_t) lrintf(v * 64);
    CREST[i] = v > 0 ? (uint8_t) lrintf(v * v * v * 64) : 0;
  }
  tables = true;
}

struct V2 {
  float x, y;
};

/* Half the body's depth along it, as a fraction of its length: beak, melon,
   the deep chest, then the long taper to the tail stock. */
const int SPINE = 11;
const float GIRTH[SPINE] = {0.0f, 0.04f, 0.075f, 0.095f, 0.105f, 0.1f, 0.085f, 0.065f, 0.045f, 0.03f, 0.022f};

enum Kind { LEAP, SWIM };

struct Dolphin {
  bool live;
  Kind kind;
  float born;     // when it starts, in scene time (it may wait for its pod)
  float len;      // body length, dots
  int dir;        // +1 swims right
  float x0, lane; // where it breaks the surface, and the surface row there
  float speed;    // dots a second, along x
  float flight;   // seconds out of the water (LEAP)
  float height;   // top of the arc above the lane (LEAP)
  float wave, amp, phase;  // the swim's gentle rise and fall (SWIM)
  bool splashed_up, splashed_down;
};

struct Drop {
  float x, y, vx, vy, floor;
  bool live;
};

struct Bubble {
  float x, y, r, wobble;
  bool live;
};

struct Star {
  uint8_t x, y, phase, rate;
};

class Dolphins : public Saver {
 public:
  /* A character LCD: no dithered sky or sea, which only turns into noise in
     5x8 cells; just the wave line, the dolphins and their splashes. */
  bool simple = false;

  ~Dolphins() override {
    delete[] level_;
    delete[] shown_;
  }

  void start(Surface &s) override {
    make_tables();
    mono_ = s.mono();
    /* About 110 dots across: a colour panel gets cyan dots with a gap. */
    pitch_ = 1;
    if (!mono_) {
      pitch_ = (int) lrintf(fminf(s.width() / 110.0f, s.height() / 55.0f));
      if (pitch_ < 1)
        pitch_ = 1;
    }
    cols_ = s.width() / pitch_;
    rows_ = s.height() / pitch_;
    ox_ = (s.width() - cols_ * pitch_) / 2;
    oy_ = (s.height() - rows_ * pitch_) / 2;
    delete[] level_;
    delete[] shown_;
    level_ = new uint8_t[cols_ * rows_];
    shown_ = new uint8_t[cols_ * rows_];
    memset(shown_, 0, cols_ * rows_);
    s.clear();
    if (pitch_ >= 3)
      for (int y = 0; y < rows_; y++)
        for (int x = 0; x < cols_; x++)
          dot(s, x, y, false);
    clock_ = 0;
    t_ = 0;
    scene_ = -1;
    dawn_ = rng.uniform() < 0.5f;
    next_scene();
  }

  void frame(Surface &s, float dt) override {
    clock_ += dt;
    if (clock_ < 1 / FPS)
      return;
    float step = clock_ > 0.25f ? 0.25f : clock_;
    clock_ = 0;
    t_ += step;
    scene_t_ += step;
    if (scene_t_ >= scene_len_)
      next_scene();

    if (deep_) {
      update_deep(step);
      paint_deep();
    } else {
      update_surface(step);
      paint_surface();
    }
    fade();
    show(s);
  }

 private:
  /* ---- the scenes ---- */

  void next_scene() {
    scene_++;
    deep_ = scene_ % 2 == 1;
    if (!deep_ && scene_ > 0)
      dawn_ = !dawn_;
    scene_t_ = 0;
    scene_len_ = deep_ ? DEEP_TIME : SURFACE_TIME;
    for (auto &d : dolphins_)
      d.live = false;
    for (auto &d : drops_)
      d.live = false;
    for (auto &b : bubbles_)
      b.live = false;
    next_pod_ = 0.8f;
    horizon_ = (int) (rows_ * 0.42f);
    if (deep_) {
      drift_ = rng.range(0, 1000);
      kelp_seed_ = rng.next();
    } else {
      /* The moon high and to one side; the sun low, part set. */
      if (dawn_) {
        orb_r_ = rows_ * 0.15f;
        orb_x_ = cols_ * rng.range(0.3f, 0.7f);
        orb_y_ = horizon_ - orb_r_ * 0.35f;
      } else {
        orb_r_ = fmaxf(rows_ * 0.07f, 1.5f);
        orb_x_ = cols_ * rng.range(0.15f, 0.85f);
        orb_y_ = horizon_ * rng.range(0.3f, 0.5f);
      }
      stars_ = 0;
      int want = simple ? 4 : cols_ * horizon_ / 70;
      if (want > MAX_STARS)
        want = MAX_STARS;
      while (stars_ < want) {
        Star &st = star_[stars_++];
        st.x = (uint8_t) rng.below(cols_);
        st.y = (uint8_t) rng.below(horizon_ > 2 ? horizon_ - 2 : 1);
        st.phase = (uint8_t) rng.below(256);
        st.rate = (uint8_t) (20 + rng.below(60));
      }
    }
  }

  /* The pod's size, set by the screen: a third of the height far off, up
     to two thirds in front. */
  float body(float depth) const {
    float l = rows_ * (0.32f + 0.36f * depth);
    if (simple)
      l = rows_ * 0.85f;
    if (l < 9)
      l = 9;
    if (l > cols_ * 0.42f)
      l = cols_ * 0.42f;
    return l;
  }

  void spawn_pod() {
    int n = 1 + rng.below(3);
    int dir = rng.uniform() < 0.5f ? 1 : -1;
    float depth = deep_ ? rng.range(0.35f, 0.75f) : rng.range(0.3f, 0.95f);
    float len = body(deep_ ? 0.6f : depth);
    float lane = deep_ ? rows_ * depth : horizon_ + depth * (rows_ - horizon_ - 2);
    if (simple && !deep_)
      lane = rows_ - 3;
    float speed = len * (deep_ ? rng.range(1.1f, 1.5f) : rng.range(1.7f, 2.2f));
    /* Leap from the far side so the whole arc is on screen. */
    float flight = rng.range(0.75f, 1.05f);
    float x0 = deep_ ? (dir > 0 ? -len * 1.2f : cols_ + len * 1.2f)
                     : (dir > 0 ? rng.range(0.0f, 0.35f) : rng.range(0.65f, 1.0f)) * cols_;
    float height = fminf(len * rng.range(0.7f, 1.1f), lane - 2);
    for (int i = 0; i < n; i++) {
      Dolphin *d = free_dolphin();
      if (!d)
        return;
      d->live = true;
      d->kind = deep_ ? SWIM : LEAP;
      d->born = scene_t_ + i * rng.range(0.3f, 0.55f);
      d->len = len * rng.range(0.9f, 1.05f);
      d->dir = dir;
      d->x0 = x0 - dir * i * len * rng.range(0.15f, 0.4f);
      d->lane = lane + (deep_ ? rng.range(-0.15f, 0.15f) * rows_ : 0);
      d->speed = speed;
      d->flight = flight * rng.range(0.92f, 1.05f);
      d->height = height * rng.range(0.85f, 1.0f);
      d->wave = rng.range(0.4f, 0.8f);
      d->amp = rows_ * rng.range(0.03f, 0.08f);
      d->phase = rng.range(0, 2 * PI);
      d->splashed_up = d->splashed_down = false;
    }
  }

  Dolphin *free_dolphin() {
    for (auto &d : dolphins_)
      if (!d.live)
        return &d;
    return nullptr;
  }

  bool pod_out() const {
    for (auto &d : dolphins_)
      if (d.live)
        return true;
    return false;
  }

  /* Where a dolphin's beak is `tau` seconds into its run. A leap is one
     parabola: below the lane before and after it is out of the water. */
  V2 path(const Dolphin &d, float tau) const {
    if (d.kind == LEAP) {
      float u = tau / d.flight;
      return {d.x0 + d.dir * d.speed * tau, d.lane - d.height * 4 * u * (1 - u)};
    }
    return {d.x0 + d.dir * d.speed * tau, d.lane + d.amp * sinf(d.phase + tau * d.wave * 2 * PI)};
  }

  /* Seconds a leap needs, from leaving the deep to being gone again. */
  float run_time(const Dolphin &d) const {
    if (d.kind == LEAP)
      return d.flight * 2 + 1.0f;
    return (cols_ + d.len * 3) / d.speed;
  }

  void update_surface(float dt) {
    next_pod_ -= dt;
    if (next_pod_ <= 0 && !pod_out() && scene_t_ < scene_len_ - 3) {
      spawn_pod();
      next_pod_ = rng.range(0.3f, 1.5f);
    }
    for (auto &d : dolphins_) {
      if (!d.live)
        continue;
      float tau = scene_t_ - d.born;
      if (tau > run_time(d)) {
        d.live = false;
        continue;
      }
      if (!d.splashed_up && tau >= 0) {
        d.splashed_up = true;
        splash(d.x0, d.lane, d.len, d.dir);
      }
      if (!d.splashed_down && tau >= d.flight) {
        d.splashed_down = true;
        splash(path(d, d.flight).x, d.lane, d.len * 0.8f, d.dir);
      }
    }
    float g = rows_ * 1.6f;
    for (auto &p : drops_) {
      if (!p.live)
        continue;
      p.vy += g * dt;
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      if (p.vy > 0 && p.y >= p.floor)
        p.live = false;
    }
  }

  void splash(float x, float lane, float len, int dir) {
    int n = (int) (len * 0.5f);
    if (n > 14)
      n = 14;
    for (int i = 0; i < n; i++)
      for (auto &p : drops_)
        if (!p.live) {
          p.live = true;
          p.x = x + rng.range(-0.15f, 0.15f) * len;
          p.y = lane - 0.5f;
          p.vx = (rng.range(-0.6f, 0.6f) + dir * 0.3f) * len;
          p.vy = -rng.range(0.6f, 1.25f) * sqrtf(len * rows_);
          p.floor = lane;
          break;
        }
    foam_x_ = x;
    foam_y_ = lane;
    foam_t_ = 0.7f;
    foam_w_ = len * 0.35f;
  }

  void update_deep(float dt) {
    drift_ += dt * rows_ * 0.04f;
    next_pod_ -= dt;
    if (next_pod_ <= 0 && !pod_out() && scene_t_ < scene_len_ - 5) {
      spawn_pod();
      next_pod_ = rng.range(0.5f, 1.8f);
    }
    for (auto &d : dolphins_)
      if (d.live && scene_t_ - d.born > run_time(d))
        d.live = false;
    /* Bubbles: from the floor now and then, and in a string behind each
       dolphin's blowhole. */
    if (rng.uniform() < dt * (simple ? 0.8f : 2.5f))
      bubble(rng.range(0, cols_), rows_ - 1, rng.range(0.6f, 1.6f));
    for (auto &d : dolphins_) {
      float tau = scene_t_ - d.born;
      if (d.live && tau > 0 && rng.uniform() < dt * 1.2f) {
        V2 h = path(d, tau - 0.25f * d.len / d.speed);
        bubble(h.x, h.y - d.len * 0.1f, rng.range(0.6f, 1.2f));
      }
    }
    for (auto &b : bubbles_) {
      if (!b.live)
        continue;
      b.y -= dt * rows_ * (0.12f + 0.08f * b.r);
      b.wobble += dt * 5;
      if (b.y < -3)
        b.live = false;
    }
  }

  void bubble(float x, float y, float r) {
    for (auto &b : bubbles_)
      if (!b.live) {
        b = {x, y, simple ? 0.5f : r, rng.range(0, 6.28f), true};
        return;
      }
  }

  /* ---- painting, as intensities ---- */

  uint8_t &at(int x, int y) { return level_[y * cols_ + x]; }

  /* A phase in turns as 0.32 fixed point: the top byte indexes the tables,
     and sums wrap round as they should. */
  static uint32_t turns(float t) { return (uint32_t) (int64_t) ((t - floorf(t)) * 4294967296.0f); }

  void paint_surface() {
    int hz = horizon_;
    int ticks = (int) (t_ * 256);
    /* Sky: a gradient to the horizon, the orb, stars. */
    for (int y = 0; y < hz; y++) {
      float f = (float) y / hz;
      int base = simple ? 0 : dawn_ ? (int) (3 + 30 * f * f * f) : (int) (1 + 9 * f * f * f);
      uint8_t *row = &level_[y * cols_];
      for (int x = 0; x < cols_; x++)
        row[x] = (uint8_t) base;
    }
    if (dawn_ && !simple) {
      /* Two thin dark bands of cloud, drifting. */
      for (int c = 0; c < 2; c++) {
        int cy = (int) (hz * (0.35f + 0.25f * c));
        float cx = fmodf(t_ * (1.5f + c) + c * cols_ * 0.6f, cols_ * 1.6f) - cols_ * 0.3f;
        float half = cols_ * (0.18f + 0.08f * c);
        for (int x = (int) (cx - half); x < cx + half; x++)
          if (x >= 0 && x < cols_ && cy >= 0 && cy < hz) {
            at(x, cy) = 0;
            if (fabsf(x - cx) < half * 0.6f && cy + 1 < hz)
              at(x, cy + 1) = (uint8_t) (at(x, cy + 1) / 3);
          }
      }
    }
    if (!dawn_)
      for (int i = 0; i < stars_; i++) {
        Star &st = star_[i];
        int tw = SIN[(st.phase + ticks * st.rate / 256 / 4) & 255];
        if (tw > -20)
          at(st.x, st.y) = FULL;
      }
    paint_orb();

    /* Sea: two trains of swell seen in perspective, so the crests lie along
       the horizon and crowd together towards it, and the orb's path of
       glitter across them. Phases are 16.16 turns of the tables. */
    for (int y = hz; y < rows_; y++) {
      float d = (float) (y - hz + 1) / (rows_ - hz);
      uint8_t *row = &level_[y * cols_];
      if (simple) {
        for (int x = 0; x < cols_; x++)
          row[x] = 0;
        continue;
      }
      float z = 1 / (d + 0.08f);
      /* Turns of each wave across one dot of x at this distance. */
      float across = 2 * z / cols_;
      const float K = 6.5f;
      uint32_t p1 = turns(z * K - t_ * 0.55f), k1 = turns(across * K * 0.22f);
      uint32_t p2 = turns(z * K * 0.62f - t_ * 0.4f + 0.3f), k2 = turns(-across * K * 0.4f);
      uint32_t p3 = turns(z * K * 2.3f - t_ * 1.7f), k3 = turns(across * K * 1.6f);
      /* Swell far off blurs into haze: less contrast, more glow. */
      float near = fminf(1, d * 2.5f);
      int base = (int) ((dawn_ ? 14 : 7) * (1 - near) + (dawn_ ? 4 : 2) + 6 * d);
      int a1 = (int) ((34 + 20 * d) * near), a2 = (int) ((22 + 10 * d) * near);
      int mid = cols_ / 2;
      float glit_w = orb_r_ * (0.8f + 2.8f * d);
      int gx0 = (int) (orb_x_ - glit_w), gx1 = (int) (orb_x_ + glit_w);
      for (int x = 0; x < cols_; x++) {
        uint32_t dx = (uint32_t) (x - mid);
        int v = base + ((CREST[(p1 + dx * k1) >> 24] * a1 + CREST[(p2 + dx * k2) >> 24] * a2) >> 6);
        if (x >= gx0 && x <= gx1) {
          float g = 1 - fabsf(x - orb_x_) / glit_w;
          v += (int) (g * (dawn_ ? 80 : 64)) * CREST[(p3 + dx * k3) >> 24] >> 6;
        }
        row[x] = (uint8_t) (v > FULL ? FULL : v);
      }
    }
    if (simple)
      for (int x = 0; x < cols_; x++)
        if (CREST[(x * 40 + ticks / 3) & 255] > 8)
          at(x, rows_ - 3) = FULL;

    /* Foam where a dolphin went in or came out. */
    if (foam_t_ > 0) {
      foam_t_ -= 1 / FPS;
      float w = foam_w_ * (1.6f - foam_t_);
      int fy = (int) foam_y_;
      if (fy >= 0 && fy < rows_)
        for (int x = (int) (foam_x_ - w); x <= foam_x_ + w; x++)
          if (x >= 0 && x < cols_ && ((x + fy) & 1))
            at(x, fy) = FULL;
    }

    for (auto &d : dolphins_)
      if (d.live && scene_t_ >= d.born)
        draw_dolphin(d, scene_t_ - d.born, d.lane);
    for (auto &p : drops_)
      if (p.live) {
        int x = (int) p.x, y = (int) p.y;
        if (x >= 0 && x < cols_ && y >= 0 && y < rows_ && y < p.floor)
          at(x, y) = dawn_ && y < horizon_ ? 0 : FULL;
      }
  }

  void paint_orb() {
    int r = (int) ceilf(orb_r_ * 3);
    for (int y = (int) orb_y_ - r; y <= orb_y_ + r; y++) {
      if (y < 0 || y >= horizon_)
        continue;
      for (int x = (int) orb_x_ - r; x <= orb_x_ + r; x++) {
        if (x < 0 || x >= cols_)
          continue;
        float dist = hypotf(x + 0.5f - orb_x_, y + 0.5f - orb_y_) / orb_r_;
        int v = at(x, y);
        if (dist <= 1)
          v = FULL;
        else if (!simple)
          v += (int) ((dawn_ ? 18 : 22) * (1 - (dist - 1) / 2) * (dist < 3));
        at(x, y) = (uint8_t) (v > FULL ? FULL : v);
      }
    }
  }

  void paint_deep() {
    int ticks = (int) (t_ * 256);
    int surf = (int) (rows_ * 0.07f) + 1;
    for (int y = 0; y < rows_; y++) {
      float f = (float) y / rows_;
      int base = simple ? 0 : (int) (4 + 24 * (1 - f) * (1 - f));
      uint8_t *row = &level_[y * cols_];
      if (y < surf && !simple) {
        /* The underside of the surface: a bright, rippling skin. */
        int k = 65536 / 7;
        int p = ticks * 30 + y * 30000;
        for (int x = 0; x < cols_; x++) {
          int v = 26 + (CREST[((x * k + p) >> 8) & 255] * 40 >> 6) +
                  (CREST[((x * k * 3 / 2 - p) >> 8) & 255] * 24 >> 6);
          row[x] = (uint8_t) (v > FULL ? FULL : v);
        }
        continue;
      }
      for (int x = 0; x < cols_; x++)
        row[x] = (uint8_t) base;
    }
    if (!simple) {
      /* Shafts of light, slanting down and swaying. */
      for (int i = 0; i < 4; i++) {
        float cx = cols_ * (0.12f + 0.25f * i) + cols_ * 0.06f * sinf(t_ * 0.3f + i * 1.7f);
        float w = cols_ * (0.03f + 0.015f * (i & 1));
        float slant = 0.35f + 0.1f * i;
        for (int y = surf; y < rows_; y++) {
          float c = cx + (y - surf) * slant;
          float fall = 1 - (float) (y - surf) / (rows_ - surf);
          for (int x = (int) (c - w); x <= c + w; x++) {
            if (x < 0 || x >= cols_)
              continue;
            int v = at(x, y) + (int) (40 * (0.3f + 0.7f * fall) * (1 - fabsf(x - c) / w));
            at(x, y) = (uint8_t) (v > FULL ? FULL : v);
          }
        }
      }
    }
    /* The floor: rock along the bottom, and kelp swaying up from it. */
    for (int x = 0; x < cols_; x++) {
      float u = x + drift_;
      float h = rows_ * (0.1f + 0.04f * sinf(u * 0.11f) + 0.03f * sinf(u * 0.037f + 1) + 0.015f * sinf(u * 0.29f));
      if (simple)
        h = 1;
      int top = rows_ - (int) h;
      for (int y = top; y < rows_; y++)
        if (y >= 0)
          at(x, y) = simple || y > top ? 0 : FULL;
    }
    if (!simple) {
      int every = cols_ / 7 + 1;
      int first = (int) floorf(drift_ / every);
      for (int k = first; k <= first + 8; k++) {
        uint32_t hsh = ((uint32_t) k * 2654435761u ^ kelp_seed_) * 2246822519u;
        hsh ^= hsh >> 15;
        if (hsh % 5 < 2)
          continue;  // a gap in the bed
        float base_x = k * every - drift_ + (hsh >> 4) % (every * 3 / 4 + 1);
        float tall = rows_ * (0.3f + (hsh >> 8) % 40 / 100.0f);
        for (int j = 0; j < (int) tall; j++) {
          int y = rows_ - 1 - j;
          float sway = sinf(t_ * 1.1f + j * 0.12f + k) * j * 0.12f;
          int x = (int) lrintf(base_x + sway);
          plot(x, y, FULL);
          /* A blade every few dots, to alternate sides. */
          if (j > 3 && j % 5 == 0) {
            int side = (j / 5) & 1 ? 1 : -1;
            plot(x + side, y - 1, FULL);
            plot(x + 2 * side, y - 2, FULL);
          }
        }
      }
    }
    for (auto &d : dolphins_)
      if (d.live && scene_t_ >= d.born)
        draw_dolphin(d, scene_t_ - d.born, 1e9f);
    for (auto &b : bubbles_)
      if (b.live) {
        float x = b.x + sinf(b.wobble) * 0.8f;
        if (b.r < 1) {
          plot((int) x, (int) b.y, FULL);
          continue;
        }
        for (int a = 0; a < 8; a++) {
          float ang = a * PI / 4;
          plot((int) lrintf(x + cosf(ang) * b.r), (int) lrintf(b.y + sinf(ang) * b.r), FULL);
        }
      }
  }

  void plot(int x, int y, uint8_t v) {
    if (x >= 0 && y >= 0 && x < cols_ && y < rows_)
      at(x, y) = v;
  }

  /* ---- a dolphin ---- */

  /* The spine, beak first, laid along the dolphin's own path, so the body
     bends through a leap. A swimmer's tail also beats. */
  void spine(const Dolphin &d, float tau, V2 out[SPINE]) const {
    float seg = d.len / (SPINE - 1);
    float dt = 0.02f * d.len / d.speed;
    V2 prev = path(d, tau);
    out[0] = prev;
    float t = tau, have = 0;
    for (int i = 1; i < SPINE; i++) {
      for (int guard = 0; guard < 400; guard++) {
        t -= dt;
        V2 p = path(d, t);
        float step = hypotf(p.x - prev.x, p.y - prev.y);
        if (have + step >= seg) {
          float k = (seg - have) / step;
          prev = {prev.x + (p.x - prev.x) * k, prev.y + (p.y - prev.y) * k};
          t += dt * (1 - k);
          have = 0;
          break;
        }
        have += step;
        prev = p;
      }
      out[i] = prev;
    }
    if (d.kind == SWIM)
      for (int i = 4; i < SPINE; i++) {
        float s = (float) i / (SPINE - 1);
        out[i].y += sinf(tau * 7 - s * 3) * d.len * 0.06f * s * s;
      }
  }

  /* The body as one outline: back from beak to tail along the top, a fin on
     the way, then back along the belly. `grow` thickens it, for an edge. */
  void draw_dolphin(const Dolphin &d, float tau, float water) {
    V2 sp[SPINE];
    spine(d, tau, sp);
    V2 n[SPINE];  // towards the back (dorsal side)
    for (int i = 0; i < SPINE; i++) {
      V2 a = sp[i > 0 ? i - 1 : 0], b = sp[i < SPINE - 1 ? i + 1 : SPINE - 1];
      float tx = a.x - b.x, ty = a.y - b.y, l = hypotf(tx, ty);
      if (l == 0)
        l = 1;
      tx /= l;
      ty /= l;
      n[i] = {d.dir * ty, -d.dir * tx};
    }
    /* A bright body with a dark edge; at dawn the other way about, a
       silhouette with the sun on its rim. */
    bool dark = !deep_ && dawn_;
    clip_ = water;
    if (!simple)
      dolphin_shape(d, sp, n, 1.0f, dark ? FULL : 0);
    dolphin_shape(d, sp, n, 0, dark ? 0 : FULL);
    if (!dark && d.len >= 14) {
      /* An eye, just behind the melon. */
      V2 e = {sp[1].x + n[1].x * d.len * 0.02f, sp[1].y + n[1].y * d.len * 0.02f};
      V2 f = {e.x + (sp[2].x - sp[1].x) * 0.4f, e.y + (sp[2].y - sp[1].y) * 0.4f};
      if (f.y < clip_)
        plot((int) f.x, (int) f.y, 0);
    }
    clip_ = 1e9f;
  }

  void dolphin_shape(const Dolphin &d, const V2 *sp, const V2 *n, float grow, uint8_t v) {
    float L = d.len;
    V2 pts[2 * SPINE + 8];
    int k = 0;
    for (int i = 0; i < SPINE; i++) {
      float g = GIRTH[i] * L + grow;
      pts[k++] = {sp[i].x + n[i].x * g, sp[i].y + n[i].y * g};
    }
    for (int i = SPINE - 1; i >= 0; i--) {
      float g = GIRTH[i] * L * (i < 3 ? 0.8f : 1.0f) + grow;
      pts[k++] = {sp[i].x - n[i].x * g, sp[i].y - n[i].y * g};
    }
    fill(pts, k, v);

    /* Dorsal fin: rising from the back behind the chest, swept back. */
    int a = 4, b = 6;
    V2 tip = {sp[6].x + n[5].x * (GIRTH[5] + 0.13f) * L, sp[6].y + n[5].y * (GIRTH[5] + 0.13f) * L};
    tip.x += (sp[7].x - sp[6].x) * 0.6f;
    tip.y += (sp[7].y - sp[6].y) * 0.6f;
    V2 fin[3] = {{sp[a].x + n[a].x * (GIRTH[a] * L - 0.5f), sp[a].y + n[a].y * (GIRTH[a] * L - 0.5f)},
                 {tip.x + n[5].x * grow, tip.y + n[5].y * grow},
                 {sp[b].x + n[b].x * (GIRTH[b] * L - 0.5f), sp[b].y + n[b].y * (GIRTH[b] * L - 0.5f)}};
    fill(fin, 3, v);

    /* Flipper: down and back from under the chest. */
    V2 fl[3] = {{sp[2].x - n[2].x * GIRTH[2] * L * 0.6f, sp[2].y - n[2].y * GIRTH[2] * L * 0.6f},
                {sp[4].x - n[3].x * (GIRTH[3] + 0.09f + grow / L) * L, sp[4].y - n[3].y * (GIRTH[3] + 0.09f + grow / L) * L},
                {sp[4].x - n[4].x * GIRTH[4] * L * 0.6f, sp[4].y - n[4].y * GIRTH[4] * L * 0.6f}};
    fill(fl, 3, v);

    /* Flukes: spread across the tail, swept back, with a notch. */
    V2 t = sp[SPINE - 1], u = sp[SPINE - 2];
    float bx = t.x - u.x, by = t.y - u.y, bl = hypotf(bx, by);
    if (bl == 0)
      bl = 1;
    bx /= bl;
    by /= bl;  // pointing back
    V2 nn = n[SPINE - 1];
    float span = 0.16f * L + grow, sweep = 0.1f * L + grow;
    V2 fk[5] = {{u.x + nn.x * 0.02f * L, u.y + nn.y * 0.02f * L},
                {t.x + bx * sweep + nn.x * span, t.y + by * sweep + nn.y * span},
                {t.x + bx * 0.03f * L, t.y + by * 0.03f * L},
                {t.x + bx * sweep - nn.x * span, t.y + by * sweep - nn.y * span},
                {u.x - nn.x * 0.02f * L, u.y - nn.y * 0.02f * L}};
    fill(fk, 5, v);
  }

  /* Even-odd fill of a float polygon, sampled at dot centres, above the
     water line `clip_`. */
  void fill(const V2 *p, int n, uint8_t v) {
    float top = p[0].y, bottom = p[0].y;
    for (int i = 1; i < n; i++) {
      top = fminf(top, p[i].y);
      bottom = fmaxf(bottom, p[i].y);
    }
    int y0 = (int) floorf(top), y1 = (int) ceilf(bottom);
    if (y0 < 0)
      y0 = 0;
    if (y1 > rows_ - 1)
      y1 = rows_ - 1;
    float xs[2 * SPINE + 8];
    for (int y = y0; y <= y1; y++) {
      float fy = y + 0.5f;
      if (fy >= clip_)
        break;
      int k = 0;
      for (int i = 0; i < n; i++) {
        const V2 &a = p[i], &b = p[(i + 1) % n];
        if ((a.y <= fy && b.y > fy) || (b.y <= fy && a.y > fy))
          xs[k++] = a.x + (fy - a.y) / (b.y - a.y) * (b.x - a.x);
      }
      for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
          float t = xs[j];
          xs[j] = xs[j - 1];
          xs[j - 1] = t;
        }
      for (int i = 0; i + 1 < k; i += 2) {
        int xa = (int) ceilf(xs[i] - 0.5f), xb = (int) floorf(xs[i + 1] - 0.5f);
        if (xa < 0)
          xa = 0;
        if (xb > cols_ - 1)
          xb = cols_ - 1;
        for (int x = xa; x <= xb; x++)
          at(x, y) = v;
      }
    }
  }

  /* ---- out to the panel ---- */

  /* In and out of each scene through the dither, as the stereo did. */
  void fade() {
    float in = scene_t_ / FADE, out = (scene_len_ - scene_t_) / FADE;
    float m = fminf(1, fminf(in, out));
    if (m >= 1)
      return;
    int k = (int) (m * 256);
    for (int i = 0; i < cols_ * rows_; i++)
      level_[i] = (uint8_t) (level_[i] * k >> 8);
  }

  void show(Surface &s) {
    for (int y = 0; y < rows_; y++)
      for (int x = 0; x < cols_; x++) {
        int i = y * cols_ + x;
        uint8_t on = level_[i] >= FULL || level_[i] > BAYER[y & 7][x & 7];
        if (on != shown_[i]) {
          shown_[i] = on;
          dot(s, x, y, on);
        }
      }
  }

  void dot(Surface &s, int x, int y, bool on) {
    int px = ox_ + x * pitch_, py = oy_ + y * pitch_;
    if (mono_) {
      s.pixel(px, py, on ? Rgb{255, 255, 255} : BLACK);
      return;
    }
    int size = pitch_ >= 3 ? pitch_ - 1 : pitch_;
    s.fill_rect(px, py, size, size, on ? LIT : (pitch_ >= 3 ? UNLIT : BLACK));
  }

  bool mono_ = false;
  int pitch_ = 1, cols_ = 0, rows_ = 0, ox_ = 0, oy_ = 0;
  uint8_t *level_ = nullptr, *shown_ = nullptr;
  float clock_ = 0, t_ = 0, scene_t_ = 0, scene_len_ = 0, next_pod_ = 0;
  int scene_ = 0;
  bool deep_ = false, dawn_ = false;
  int horizon_ = 0;
  float orb_x_ = 0, orb_y_ = 0, orb_r_ = 1;
  Star star_[MAX_STARS];
  int stars_ = 0;
  Dolphin dolphins_[MAX_DOLPHINS] = {};
  Drop drops_[MAX_DROPS] = {};
  Bubble bubbles_[MAX_BUBBLES] = {};
  float foam_x_ = 0, foam_y_ = 0, foam_t_ = 0, foam_w_ = 0;
  float drift_ = 0;
  uint32_t kelp_seed_ = 0;
  float clip_ = 1e9f;
};

/*
 * On a character LCD the dithered scenes are only noise, so it is drawn by
 * hand: the sea along the bottom row as one glyph, redefined to roll, a moon
 * and stars above, and a dolphin in three poses (rising, over the top,
 * diving) leaping from the bottom row into the top one. Positions are in dots
 * laid out as the panel is, cells 5x8 with a dot's gap between them, so a
 * dolphin crossing a gap goes behind it as on the real thing. Cells the
 * dolphin, its splash or the moon touch get glyphs of their own.
 */
const char *const POSE_UP[] = {
    ".......##",
    ".....####",
    "...####..",
    "..####...",
    ".###.....",
    "#.#......",
    "##.......",
};
const char *const POSE_TOP[] = {
    ".....#.....",
    "....##.....",
    "#.#########",
    ".##########",
    "#.....#....",
};
const char *const POSE_DOWN[] = {
    "##.......",
    "#.#......",
    ".###.....",
    "..####...",
    "...####..",
    ".....####",
    ".......##",
};
/* The sun and moon in the sky row, 5x5, set low or high in the cell. */
const char *const SUN[] = {
    "#.#.#",
    ".###.",
    "#####",
    ".###.",
    "#.#.#",
};

/*
 * The weather at the right of the sky, when it's more than the sun or a clear
 * night: a glyph or two that take turns (the rain falling, the snow drifting).
 */
struct WeatherArt {
  Weather w;
  float every;  // seconds a frame
  const char *const frames[2][8];
};
const WeatherArt WEATHER_ART[] = {
    {WX_PARTLY, 2.0f, {{".....", ".....", "..##.", ".####", "#####", ".....", ".....", "....."},
                       {".....", ".....", ".##..", "####.", "#####", ".....", ".....", "....."}}},
    {WX_CLOUDY, 2.0f, {{".....", "..##.", ".####", "#####", ".....", ".##..", "####.", "....."},
                       {".....", "..##.", ".####", "#####", ".....", ".##..", "####.", "....."}}},
    {WX_FOG, 1.2f, {{".....", "####.", ".....", ".####", ".....", "####.", ".....", "....."},
                    {".....", ".####", ".....", "####.", ".....", ".####", ".....", "....."}}},
    {WX_RAIN, 0.4f, {{"..##.", ".####", "#####", ".....", "#..#.", ".....", ".#..#", "....."},
                     {"..##.", ".####", "#####", ".....", ".....", "#..#.", ".....", ".#..#"}}},
    {WX_POURING, 0.3f, {{"..##.", ".####", "#####", "#.#.#", ".#.#.", "#.#.#", ".#.#.", "#.#.#"},
                        {"..##.", ".####", "#####", ".#.#.", "#.#.#", ".#.#.", "#.#.#", ".#.#."}}},
    {WX_SNOW, 0.8f, {{"..##.", ".####", "#####", ".....", "#...#", ".....", "..#..", "....."},
                     {"..##.", ".####", "#####", ".....", ".....", "#...#", ".....", "..#.."}}},
    {WX_SLEET, 0.5f, {{"..##.", ".####", "#####", ".....", "#..#.", ".....", "..#.#", "....."},
                      {"..##.", ".####", "#####", ".....", ".....", "#..#.", ".....", "..#.#"}}},
    {WX_HAIL, 0.5f, {{"..##.", ".####", "#####", ".....", "#.#.#", ".....", ".#.#.", "....."},
                     {"..##.", ".####", "#####", ".....", ".....", "#.#.#", ".....", ".#.#."}}},
    {WX_STORM, 0.3f, {{"..##.", ".####", "#####", ".....", ".....", ".....", ".....", "....."},
                      {"..##.", ".####", "#####", "...#.", "..#..", ".###.", "..#..", ".#..."}}},
    {WX_WIND, 0.6f, {{".....", "###..", "...#.", "####.", ".....", "##...", "..#..", "....."},
                     {".....", ".###.", "....#", ".####", ".....", ".##..", "...#.", "....."}}},
    {WX_EXCEPTIONAL, 1.0f, {{"..#..", "..#..", "..#..", "..#..", ".....", "..#..", ".....", "....."},
                            {"..#..", "..#..", "..#..", "..#..", ".....", "..#..", ".....", "....."}}},
};

/* Lines in the sky row that take turns, each fading out through blank and
   the next in. */
struct Turns {
  static const int MAX = 4;
  static constexpr float FADE_STEP = 0.25f;  // each of half gone, blank, half there
  char text[MAX][16] = {};
  float hold[MAX] = {};
  int x0[MAX] = {};
  int placed[MAX] = {-1, -1, -1, -1};  // the length it was placed at
  int n = 0, showing = 0;
  float shown = 0, fade = -1;

  void reset() {
    showing = 0;
    shown = 0;
    fade = -1;
    for (int i = 0; i < MAX; i++) {
      text[i][0] = 0;
      placed[i] = -1;
    }
  }
  bool empty(int i) const { return !text[i][0]; }
  /* The next line with something in it. */
  int next() const {
    for (int k = 1; k <= n; k++) {
      int i = (showing + k) % n;
      if (!empty(i))
        return i;
    }
    return showing;
  }
  void step(float dt) {
    if (n < 1)
      return;
    if (fade >= 0) {
      fade += dt;
      if (fade >= 3 * FADE_STEP) {
        fade = -1;
        placed[showing] = -1;  // placed afresh when it next comes
        showing = next();
        shown = 0;
      }
      return;
    }
    if (empty(showing)) {
      showing = next();
      shown = 0;
      return;
    }
    shown += dt;
    if (shown >= hold[showing] && next() != showing)
      fade = 0;
  }
  /* What the sky row shows in column c, if anything: both codes the same,
     or two to blend (where the panel can). */
  bool cell(int c, uint8_t &code, uint8_t &other) const {
    if (n < 1)
      return false;
    auto at = [&](int i, uint8_t &ch) {
      int j = c - x0[i];
      if (j < 0 || j >= (int) strlen(text[i]))
        return false;
      ch = (uint8_t) text[i][j];
      return true;
    };
    uint8_t ca = ' ', cb = ' ';
    const bool in_a = at(showing, ca);
    if (fade < 0) {
      code = other = ca;
      return in_a;
    }
    const bool in_b = at(next(), cb);
    if (!in_a && !in_b)
      return false;
    /* Through blank, every dot off: the backlight's colour on any panel.
       (The full block, 0xFF, read as black on a light panel.) */
    const uint8_t BLANK = ' ';
    int stage = (int) (fade / FADE_STEP);
    if (stage == 0) {
      code = ca;
      other = BLANK;
    } else if (stage == 1) {
      code = other = BLANK;
    } else {
      code = BLANK;
      other = cb;
    }
    return true;
  }
};

class DolphinsText : public TextSaver {
 public:
  void start(TextScreen &t) override {
    cols_ = t.cols();
    rows_ = t.rows();
    w_ = cols_ * 6 - 1;
    h_ = rows_ * 9 - 1;
    /* The surface sits low in the bottom row; the time (and the date) in
       the top one. Without the real sky, a moon clear of the clock. */
    water_ = (rows_ - 1) * 9 + 4;
    clock_ = dolphins_clock && cols_ >= 12;
    date_ = clock_ && dolphins_date;
    /* The readings at the left, so the clock keeps to the right. */
    reads_.n = cols_ >= 12 ? (int) std::min<size_t>(sky.readings.size(), Turns::MAX) : 0;
    for (int i = 0; i < reads_.n; i++)
      reads_.hold[i] = READING_TIME;
    times_.n = clock_ ? (date_ ? 2 : 1) : 0;
    times_.hold[0] = TIME_TIME;
    times_.hold[1] = DATE_TIME;
    int sky0 = clock_ && !reads_.n ? (date_ ? 11 : 6) : reads_.n ? 4 : 0;
    int room = cols_ - sky0 - 4;
    moon_col_ = sky0 + 2 + rng.below(room > 1 ? room : 1);
    moon_y_ = 1;
    for (int i = 0; i < 3; i++) {
      star_col_[i] = rng.below(cols_);
      star_phase_[i] = rng.range(0, 6.28f);
    }
    t_ = tick_ = 0;
    leap_ = -1;
    wait_ = 0.5f;
    splash_t_ = 0;
    times_.reset();
    reads_.reset();
    sky_at_ = -1;
    real_ = false;
    sun_col_ = -1;
    t.clear();
  }

  void frame(TextScreen &t, float dt) override {
    tick_ += dt;
    if (tick_ < 0.1f)
      return;
    float step = tick_ > 0.5f ? 0.5f : tick_;
    tick_ = 0;
    t_ += step;

    /* A dolphin at a time, with a breath between. */
    if (leap_ < 0) {
      wait_ -= step;
      if (wait_ <= 0) {
        leap_ = 0;
        dir_ = rng.uniform() < 0.5f ? 1 : -1;
        flight_ = rng.range(1.0f, 1.3f);
        speed_ = rng.range(26, 34);
        float span = speed_ * flight_;
        x0_dolphin_ = dir_ > 0 ? rng.range(4, w_ - span - 4) : rng.range(span + 4, w_ - 4);
        if (span + 8 > w_)
          x0_dolphin_ = dir_ > 0 ? 2 : w_ - 2;
        splash(x0_dolphin_);
      }
    } else {
      float before = leap_;
      leap_ += step;
      if (before < flight_ && leap_ >= flight_)
        splash(x0_dolphin_ + dir_ * speed_ * flight_);
      if (leap_ > flight_ + 0.6f) {
        leap_ = -1;
        wait_ = rng.range(0.8f, 2.5f);
      }
    }
    if (splash_t_ > 0)
      splash_t_ -= step;

    Clock k;
    const bool have_time = local_time(k);
    update_sky(have_time, k);
    update_text(have_time, k, step);

    memset(px_, 0, sizeof px_);
    int roll = (int) (t_ * 5) % 5;
    for (int c = 0; c < cols_; c++)
      wave(c * 6, roll);
    cell_bits(0, rows_ - 1, sea_);
    /* The sun and moon behind the time, the dolphin in front of them all. */
    uint8_t code, other;
    if (real_) {
      if (moon_col_ >= 0 && !text_cell(moon_col_, code, other))
        draw_moon(moon_col_ * 6, moon_y_);
      if (sun_col_ >= 0 && !text_cell(sun_col_, code, other))
        draw(SUN, 5, 5, sun_col_ * 6, sun_y_, false);
    } else if (!text_cell(moon_col_, code, other)) {
      draw_moon(moon_col_ * 6, moon_y_);
    }
    if (leap_ >= 0 && leap_ < flight_ + 0.4f) {
      float u = leap_ / flight_;
      float x = x0_dolphin_ + dir_ * speed_ * leap_;
      float y = water_ - (water_ - 2) * 4 * u * (1 - u);
      const char *const *pose = u < 0.35f ? POSE_UP : u < 0.65f ? POSE_TOP : POSE_DOWN;
      int pw = (int) strlen(pose[0]), ph = pose == POSE_TOP ? 5 : 7;
      draw(pose, ph, pw, (int) lrintf(x) - pw / 2, (int) lrintf(y) - ph / 2, dir_ < 0);
    }
    if (splash_t_ > 0) {
      int s = splash_t_ > 0.3f ? 0 : 1;
      int sx = (int) splash_x_, sy = water_ - 1;
      const int DROPS[2][6][2] = {{{-2, -1}, {2, -1}, {-1, -3}, {1, -3}, {0, -2}, {0, 0}},
                                  {{-3, 0}, {3, 0}, {-2, -2}, {2, -2}, {0, -4}, {0, 0}}};
      for (auto &d : DROPS[s])
        set(sx + d[0], sy + d[1]);
    }
    compose(t);
  }

 private:
  /* The weather glyph, or null on a clear day or night. */
  const WeatherArt *weather_art() const {
    for (const auto &a : WEATHER_ART)
      if (a.w == sky.weather)
        return &a;
    return nullptr;
  }
  /* Columns the sun and moon cross: all but the weather's. */
  int sky_cols() const { return cols_ - (weather_art() ? 1 : 0); }

  /* Where the sun and moon are, every 20 seconds: right where they rose,
     left where they set, higher in the cell the higher they are. */
  void update_sky(bool have_time, const Clock &k) {
    if (!have_time || !sky.located()) {
      real_ = false;
      return;
    }
    if (real_ && k.utc - sky_at_ < 20)
      return;
    sky_at_ = k.utc;
    real_ = true;
    Body sun, moon;
    sun_and_moon((double) k.utc, sky.lat, sky.lon, sun, moon, lit_, waxing_);
    night_ = sun.alt < -6;
    const int n = sky_cols();
    auto col = [n](const Body &b) { return (int) lrintf((1 - b.across) * (n - 1)); };
    auto y = [](const Body &b) { return (int) lrintf(3 * (1 - b.height)); };
    sun_col_ = sun.up ? col(sun) : -1;
    sun_y_ = y(sun);
    /* A new moon is the dark side: nothing to see. */
    moon_col_ = moon.up && lit_ > 0.04f ? col(moon) : -1;
    moon_y_ = y(moon);
    if (moon_col_ >= 0 && moon_col_ == sun_col_)
      moon_col_ = -1;  // the sun outshines it
  }

  /* The time, then the date, taking turns through blank; the readings the
     same way at the left. */
  void update_text(bool have_time, const Clock &k, float step) {
    for (int i = 0; i < reads_.n; i++) {
      std::string line;
      set_live(line, sky.readings[i]());
      int n = 0;
      for (unsigned char ch : line) {
        if (n >= cols_ / 2 || n >= 15)
          break;
        /* The HD44780's own degree sign; '?' for what its ROM lacks. */
        reads_.text[i][n++] = ch == 0xB0 ? (char) 0xDF : ch < 0x80 ? (char) ch : '?';
      }
      reads_.text[i][n] = 0;
      reads_.x0[i] = 0;
    }
    reads_.step(step);

    times_.text[0][0] = times_.text[1][0] = 0;
    if (!clock_ || !have_time)
      return;
    snprintf(times_.text[0], sizeof times_.text[0], "%02d:%02d", k.hour, k.minute);
    static const char *const DAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char *mon = MON[(k.month + 11) % 12];
    if (cols_ >= 14)
      snprintf(times_.text[1], sizeof times_.text[1], "%s %d %s", DAY[k.weekday % 7], k.day, mon);
    else
      snprintf(times_.text[1], sizeof times_.text[1], "%d %s", k.day, mon);
    /* Each placed as it comes in (so the sun moving doesn't shift it), and
       again if its length changes. */
    for (int i : {times_.showing, times_.next()}) {
      int len = (int) strlen(times_.text[i]);
      if (times_.placed[i] != len) {
        times_.x0[i] = place_text(len);
        times_.placed[i] = len;
      }
    }
    times_.step(step);
  }

  /* At the left, unless the right keeps the sun and moon in sight; always
     at the right with readings at the left. */
  int place_text(int len) const {
    int right = sky_cols() - len;
    if (reads_.n)
      return right < 0 ? 0 : right;
    auto hides = [&](int x0) {
      for (int b : {sun_col_, moon_col_})
        if (b >= x0 && b < x0 + len)
          return true;
      return false;
    };
    if (!real_ || !hides(0) || right < 0 || hides(right))
      return 0;
    return right;
  }

  /* What the sky row shows in column c for the time, the date or a
     reading, if anything. The clock wins where they meet. */
  bool text_cell(int c, uint8_t &code, uint8_t &other) const {
    return times_.cell(c, code, other) || reads_.cell(c, code, other);
  }

  void splash(float x) {
    splash_x_ = x;
    splash_t_ = 0.6f;
  }

  /* One cell's worth of sea: a crest that rolls along every cell at once. */
  void wave(int x0, int roll) {
    int top = water_;
    for (int i = 0; i < 5; i++) {
      int dx = ((i - roll) % 5 + 5) % 5;
      int y = dx == 0 ? top : dx == 1 || dx == 4 ? top + 1 : top + 2;
      set(x0 + i, y);
      if (y + 1 <= (rows_ - 1) * 9 + 7)
        for (int yy = y + 2; yy <= (rows_ - 1) * 9 + 7; yy += 2)
          if ((i + yy) & 1)
            set(x0 + i, yy);
    }
  }

  void set(int x, int y) {
    if (x >= 0 && y >= 0 && x < w_ && y < h_ && x % 6 != 5 && y % 9 != 8)
      px_[y][x] = 1;
  }

  void clear(int x, int y) {
    if (x >= 0 && y >= 0 && x < w_ && y < h_)
      px_[y][x] = 0;
  }

  /* A pose, clearing the sea (and sky) behind it; below the surface it stays
     hidden. The sun is drawn the same way, high in the sky row. */
  void draw(const char *const *art, int h, int w, int x0, int y0, bool flip) {
    const bool body = art == SUN;
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) {
        char c = art[j][flip ? w - 1 - i : i];
        int x = x0 + i, y = y0 + j;
        if (y > water_ && !body)
          continue;
        if (c == '#')
          set(x, y);
        else if (!body && y < water_)
          clear(x, y);
      }
  }

  /* The moon as it is tonight: lit on the right while it waxes (seen from
     the north; the other way round south of the equator). */
  void draw_moon(int x0, int y0) {
    float lit = real_ ? lit_ : 1;
    bool right = !real_ || (waxing_ == (sky.lat >= 0));
    float edge = 1 - 2 * lit;  // the terminator, -1 (full) to 1 (new)
    for (int y = 0; y < 5; y++)
      for (int x = 0; x < 5; x++) {
        float u = (x - 2) / 2.0f, v = (y - 2) / 2.0f;
        if (u * u + v * v > 1.25f)
          continue;
        float half = sqrtf(fmaxf(0, 1 - v * v));
        if ((right ? u : -u) >= edge * half - 0.3f)
          set(x0 + x, y0 + y);
      }
  }

  void cell_bits(int c, int r, uint8_t out[8]) const {
    for (int y = 0; y < 8; y++) {
      uint8_t b = 0;
      for (int x = 0; x < 5; x++)
        if (px_[r * 9 + y][c * 6 + x])
          b |= 1 << (4 - x);
      out[y] = b;
    }
  }

  void compose(TextScreen &t) {
    /* Glyph 0 is the plain sea; the rest go to cells that differ from it,
       the weather's last, so a dolphin never goes short. */
    const uint8_t *sea = sea_;
    t.glyph(0, sea);
    uint8_t used[TextScreen::GLYPHS][8];
    int slots = 1;
    const WeatherArt *wx = weather_art();
    bool wx_cell = false;
    auto glyph_for = [&](const uint8_t bits[8]) {
      for (int k = 1; k < slots; k++)
        if (!memcmp(used[k], bits, 8))
          return k;
      if (slots >= TextScreen::GLYPHS)
        return -1;
      memcpy(used[slots], bits, 8);
      t.glyph(slots, bits);
      return slots++;
    };
    for (int r = 0; r < rows_; r++)
      for (int c = 0; c < cols_; c++) {
        uint8_t bits[8];
        cell_bits(c, r, bits);
        bool blank = true;
        for (int y = 0; y < 8; y++)
          blank &= bits[y] == 0;
        /* The sea is dim where the panel can do it, so a dolphin and its
           splash stand out bright against it. */
        if (r == rows_ - 1 && !memcmp(bits, sea, 8)) {
          t.put(c, r, 0, true);
          continue;
        }
        if (blank && r == 0) {
          uint8_t code, other;
          if (text_cell(c, code, other)) {
            t.blend(c, r, code, other);
            continue;
          }
          if (wx && c == cols_ - 1) {
            wx_cell = true;
            continue;
          }
          bool faint;
          uint8_t s = star(c, faint);
          t.put(c, r, s, faint);
          continue;
        }
        if (blank) {
          t.put(c, r, ' ');
          continue;
        }
        int slot = glyph_for(bits);
        t.put(c, r, slot < 0 ? (r == rows_ - 1 ? 0 : ' ') : slot);
      }
    if (wx_cell) {
      int frame = (int) (t_ / wx->every) & 1;
      /* A storm's lightning comes now and then, not every other beat. */
      if (wx->w == WX_STORM)
        frame = fmodf(t_, 2.7f) < 0.3f;
      uint8_t bits[8];
      glyph_from_art(wx->frames[frame], 0, bits);
      int slot = glyph_for(bits);
      t.put(cols_ - 1, 0, slot < 0 ? ' ' : slot);
    }
  }

  /* A few stars in the top row on a clear night, coming and going: faint
     (dim, where the panel can do it) on the way in and out. */
  uint8_t star(int c, bool &faint) const {
    faint = false;
    if (real_ && !night_)
      return ' ';
    switch (sky.weather) {
      case WX_UNKNOWN:
      case WX_SUNNY:
      case WX_CLEAR_NIGHT:
      case WX_PARTLY:
      case WX_WIND:
        break;
      default:
        return ' ';  // clouded over
    }
    for (int i = 0; i < 3; i++)
      if (star_col_[i] == c) {
        float s = sinf(t_ * 1.3f + star_phase_[i]);
        faint = s < 0.35f;
        return s > -0.3f ? (i == 1 ? '\'' : '.') : ' ';
      }
    return ' ';
  }

  /* Seconds the time, the date and each reading stay. */
  static constexpr float TIME_TIME = 9, DATE_TIME = 4, READING_TIME = 5;

  int cols_ = 16, rows_ = 2, w_ = 0, h_ = 0, water_ = 0;
  bool clock_ = false, date_ = false;
  Turns times_, reads_;
  bool real_ = false, night_ = true, waxing_ = true;
  long long sky_at_ = -1;
  float lit_ = 1;
  int sun_col_ = -1, sun_y_ = 0, moon_col_ = 0, moon_y_ = 1;
  uint8_t px_[TextScreen::MAX_ROWS * 9][TextScreen::MAX_COLS * 6];
  uint8_t sea_[8];
  int star_col_[3];
  float star_phase_[3];
  float t_ = 0, tick_ = 0, leap_ = -1, wait_ = 0, flight_ = 1, speed_ = 16, x0_dolphin_ = 0;
  int dir_ = 1;
  float splash_x_ = 0, splash_t_ = 0;
};

}  // namespace

Saver *make_dolphins() { return new Dolphins(); }
TextSaver *make_dolphins_text() { return new DolphinsText(); }

AD_REGISTER_SAVER("dolphins", make_dolphins, make_dolphins_text)

}  // namespace adcore
#endif
