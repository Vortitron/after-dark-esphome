#if defined(AD_SAVER_METERS) || defined(AD_ALL_SAVERS)
/*
 * Meters: a pair of VU meters, left and right. Not an After Dark module but
 * an extra, like the dolphins: what a hi-fi's front panel did while the
 * music played. On a screen, two analogue meters with needles; on a
 * character LCD, a bar per channel a row each (or a pair of needles in the
 * top row). The level comes from the YAML (a sensor or a lambda per channel,
 * see ad_meters.h), or, with none, from made-up music: a beat, verses and
 * choruses, a song's end and a pause before the next.
 *
 * The needles move as a VU meter's do, a damped spring that takes about
 * 300 ms to get there and overshoots a little. The bars rise at once and
 * fall back slowly, as a peak meter's do, and hold their peak a moment.
 */
#include "ad_meters.h"
#include "ad_core.h"
#include "ad_draw.h"
#include "ad_font.h"
#include "ad_font_4x6.h"
#include "ad_font_6x10.h"
#include "ad_text.h"

#include <algorithm>
#include <cmath>

namespace adcore {

MeterInput meter_input;

namespace {

const float PI_F = 3.14159265f;

/* 0 VU on a needle's scale, and its scale as a whole, -20 to +3 dB: a VU
   meter reads voltage, so its decibels bunch up at the bottom. */
inline float vu_pos(float db) { return powf(10, (db - 3) / 20); }
const float VU_ZERO = 0.7079f;
/* A bar's scale is in decibels, -30 to +3. */
inline float bar_pos(float db) { return (db + 30) / 33; }

/* Music nobody wrote: a level in dB per channel, about -12 to +2 while a
   song plays. */
class Music {
 public:
  void start() {
    gap_ = 0;
    song();
  }

  void step(float dt, float db[2]) {
    if (gap_ > 0) {
      gap_ -= dt;
      if (gap_ <= 0)
        song();
      db[0] = db[1] = -60;
      return;
    }
    t_ += dt;
    /* The beat: a kick on each, a lighter one between. */
    float before = beat_;
    beat_ += dt * bpm_ / 60;
    if (floorf(beat_) != floorf(before)) {
      int n = (int) floorf(beat_);
      if (n % 16 == 0)
        section();
      kick_ = rng.range(5, 9) * punch_;
      skew_ = rng.range(-1.5f, 1.5f);
    } else if (floorf(beat_ * 2) != floorf(before * 2)) {
      kick_ = fmaxf(kick_, rng.range(2, 4) * punch_);
    }
    kick_ *= expf(-dt / 0.14f);
    /* The level wanders, and the mix leans to one side or the other. */
    drift_ += (rng.range(-1, 1) * 3 - drift_) * fminf(1, dt * 0.8f);
    pan_ += (rng.range(-1, 1) * 2.5f - pan_) * fminf(1, dt * 0.3f);
    float fade = 0;
    if (t_ < 1.5f)
      fade = (1.5f - t_) * -12;  // coming in
    else if (t_ > len_ - 6)
      fade = (t_ - (len_ - 6)) * -6;  // fading out
    for (int ch = 0; ch < 2; ch++) {
      float side = ch ? 1 : -1;
      db[ch] = level_ + drift_ + kick_ + side * (pan_ + skew_ * kick_ / 8) + rng.range(-2.5f, 2.5f) + fade;
    }
    if (t_ >= len_) {
      gap_ = rng.range(2, 4);
    }
  }

 private:
  void song() {
    t_ = 0;
    beat_ = 0;
    len_ = rng.range(70, 200);
    bpm_ = rng.range(84, 140);
    punch_ = rng.range(0.6f, 1.2f);
    kick_ = 0;
    section();
  }
  /* Every 16 beats: a verse, a chorus, now and then a quiet break. */
  void section() {
    float r = rng.uniform();
    level_ = r < 0.12f ? rng.range(-24, -18) : r < 0.55f ? rng.range(-16, -11) : rng.range(-11, -7);
  }

  float t_ = 0, len_ = 120, gap_ = 0;
  float bpm_ = 120, beat_ = 0, punch_ = 1;
  float level_ = -8, kick_ = 0, skew_ = 0, drift_ = 0, pan_ = 0;
};

/* What the meters show, channel by channel, moved on by dt. */
class Engine {
 public:
  struct Channel {
    float needle = 0, speed = 0;  // on the VU scale, 0-1
    float bar = 0, tail = 0, peak = 0, hold = 0;  // on the bar scale
    float over = 0;  // seconds the peak light stays on
  };

  void start() {
    music_.start();
    for (auto &c : ch_)
      c = Channel{};
  }

  void step(float dt) {
    float vu[2], bar[2];
    const bool live = meter_input.live();
    if (live) {
      for (int c = 0; c < 2; c++)
        vu[c] = bar[c] = meter_input.read(c);
    } else {
      float db[2];
      music_.step(dt, db);
      for (int c = 0; c < 2; c++) {
        vu[c] = fminf(1.05f, vu_pos(db[c]));
        bar[c] = fmaxf(0, fminf(1, bar_pos(db[c])));
      }
    }
    /* The spring is stiff, so it is moved on in small steps. */
    int n = (int) ceilf(dt / 0.004f);
    n = n < 1 ? 1 : n > 60 ? 60 : n;
    float h = dt / n;
    const float W = 13, Z = 0.72f;
    for (int c = 0; c < 2; c++) {
      Channel &k = ch_[c];
      for (int i = 0; i < n; i++) {
        k.speed += (W * W * (vu[c] - k.needle) - 2 * Z * W * k.speed) * h;
        k.needle += k.speed * h;
      }
      k.needle = fmaxf(-0.02f, fminf(1.08f, k.needle));
      k.bar = bar[c] >= k.bar ? bar[c] : fmaxf(bar[c], k.bar - dt * 1.1f);
      k.tail = fmaxf(k.bar, k.tail - dt * 0.22f);
      if (k.bar >= k.peak) {
        k.peak = k.bar;
        k.hold = 1.2f;
      } else if (k.hold > 0) {
        k.hold -= dt;
      } else {
        k.peak = fmaxf(k.bar, k.peak - dt * 0.4f);
      }
      if (k.needle > (live ? 0.9f : VU_ZERO))
        k.over = 0.4f;
      else
        k.over -= dt;
    }
  }

  const Channel &operator[](int c) const { return ch_[c]; }

 private:
  Music music_;
  Channel ch_[2];
};

/* The angle of a needle from upright, radians: 45 degrees each way. */
inline float needle_angle(float pos) { return (-45 + 90 * pos) * PI_F / 180; }

/*
 * On a screen: two meters with cream faces, an arc from -20 to +3 with the
 * top of it red, a needle each, and a peak light.
 */
class Meters : public Saver {
 public:
  void start(Surface &s) override {
    engine_.start();
    mono_ = s.mono();
    face_ = mono_ ? BLACK : rgb(236, 220, 168);
    ink_ = mono_ ? rgb(255, 255, 255) : rgb(34, 28, 22);
    red_ = mono_ ? ink_ : rgb(196, 34, 26);
    s.clear(mono_ ? BLACK : rgb(22, 18, 14));
    const int w = s.width(), h = s.height();
    const bool side = w >= h * 1.2f;
    const int cw = side ? w / 2 : w, ch = side ? h : h / 2;
    for (int i = 0; i < 2; i++) {
      Dial &d = dial_[i];
      int m = cw >= 120 ? 4 : cw >= 50 ? 2 : 1;
      d.fw = std::min(cw - 2 * m, (int) ((ch - 2 * m) * 1.55f));
      d.fh = std::min(ch - 2 * m, (int) (d.fw / 1.2f));
      d.fx = (side ? i * cw : 0) + (cw - d.fw) / 2;
      d.fy = (side ? 0 : i * ch) + (ch - d.fh) / 2;
      big_ = d.fw >= 120;
      const Font &f = big_ ? font_6x10 : font_4x6;
      d.band = d.fh >= 40 ? f.h + 3 : 0;
      d.tick = std::max(2, d.fw / 26);
      int pad = std::max(1, d.fw / 40), numbers = big_ ? font_4x6.h + 2 : 0;
      int r1 = (int) ((d.fw / 2 - pad) / 0.7071f) - d.tick - (big_ ? 6 : 0);
      int r2 = d.fh - d.band - pad - d.tick - numbers - 1;
      d.r = std::max(4, std::min(r1, r2));
      d.cx = d.fx + d.fw / 2;
      d.py = d.fy + d.fh - d.band - (d.band ? 1 : 0);
      d.pr = std::max(2, d.r / 9);
      d.thick = d.r >= 60 ? 2 : 1;
      d.shown = false;
      d.lit = -1;
      draw_face(s, d, i);
    }
  }

  void frame(Surface &s, float dt) override {
    engine_.step(dt);
    for (int i = 0; i < 2; i++) {
      Dial &d = dial_[i];
      const auto &k = engine_[i];
      int x0, y0, x1, y1;
      needle(d, k.needle, x0, y0, x1, y1);
      if (!d.shown || x0 != d.x0 || y0 != d.y0 || x1 != d.x1 || y1 != d.y1) {
        if (d.shown)
          line(s, d.x0, d.y0, d.x1, d.y1, face_, d.thick);
        line(s, x0, y0, x1, y1, mono_ ? ink_ : rgb(20, 16, 12), d.thick);
        d.x0 = x0, d.y0 = y0, d.x1 = x1, d.y1 = y1;
        d.shown = true;
        cap(s, d);
      }
      int lit = k.over > 0;
      if (lit != d.lit && d.band) {
        int lx = d.fx + d.fw - d.band / 2 - 2, ly = d.py + 1 + d.band / 2, lr = std::max(1, d.band / 4);
        if (mono_) {
          fill_circle(s, lx, ly, lr, lit ? ink_ : BLACK);
          circle(s, lx, ly, lr, ink_);
        } else {
          fill_circle(s, lx, ly, lr, lit ? rgb(255, 60, 40) : rgb(80, 22, 16));
        }
        d.lit = lit;
      }
    }
  }

 private:
  struct Dial {
    int fx, fy, fw, fh;  // the face
    int cx, py, r, pr, tick, band, thick;  // pivot, scale radius, pivot cap
    bool shown;
    int x0, y0, x1, y1, lit;
  };

  /* The needle from just outside the pivot's cap to just inside the arc. */
  void needle(const Dial &d, float pos, int &x0, int &y0, int &x1, int &y1) {
    float a = needle_angle(pos), sx = sinf(a), sy = -cosf(a);
    float in = d.pr - 1, out = d.r - 2 - d.thick;
    x0 = d.cx + round_half_up(sx * in);
    y0 = d.py + round_half_up(sy * in);
    x1 = d.cx + round_half_up(sx * out);
    y1 = d.py + round_half_up(sy * out);
  }

  void arc(Surface &s, const Dial &d, float r, float from, float to, Rgb c) {
    int steps = std::max(6, (int) (r * (to - from) * 0.8f));
    int px = 0, py = 0;
    for (int i = 0; i <= steps; i++) {
      float a = from + (to - from) * i / steps;
      int x = d.cx + round_half_up(sinf(a) * r), y = d.py - round_half_up(cosf(a) * r);
      if (i)
        line(s, px, py, x, y, c);
      px = x, py = y;
    }
  }

  void draw_face(Surface &s, const Dial &d, int which) {
    if (mono_) {
      line(s, d.fx, d.fy, d.fx + d.fw - 1, d.fy, ink_);
      line(s, d.fx, d.fy + d.fh - 1, d.fx + d.fw - 1, d.fy + d.fh - 1, ink_);
      line(s, d.fx, d.fy, d.fx, d.fy + d.fh - 1, ink_);
      line(s, d.fx + d.fw - 1, d.fy, d.fx + d.fw - 1, d.fy + d.fh - 1, ink_);
    } else {
      s.fill_rect(d.fx, d.fy, d.fw, d.fh, face_);
    }
    const float zero = needle_angle(VU_ZERO), end = needle_angle(1);
    arc(s, d, d.r, needle_angle(0), zero, ink_);
    /* The red: a band from 0 VU up. */
    {
      const int N = 15;
      const float tw = std::max(2, d.tick - 1);
      int xy[4 * (N + 1)];
      for (int i = 0; i <= N; i++) {
        float a = zero + (end - zero) * i / N;
        xy[2 * i] = d.cx + round_half_up(sinf(a) * (d.r + tw));
        xy[2 * i + 1] = d.py - round_half_up(cosf(a) * (d.r + tw));
        xy[4 * (N + 1) - 2 - 2 * i] = d.cx + round_half_up(sinf(a) * d.r);
        xy[4 * (N + 1) - 1 - 2 * i] = d.py - round_half_up(cosf(a) * d.r);
      }
      fill_polygon(s, xy, 2 * (N + 1), red_);
      polygon(s, xy, 2 * (N + 1), red_);
    }
    static const struct {
      float db;
      const char *label;
      bool major;
    } MARKS[] = {{-20, "20", true}, {-10, "10", true}, {-7, "7", true}, {-5, "5", true}, {-3, "3", true},
                 {-2, nullptr, false}, {-1, nullptr, false}, {0, "0", true}, {1, nullptr, false},
                 {2, nullptr, false}, {3, "3", true}};
    for (const auto &m : MARKS) {
      float a = needle_angle(vu_pos(m.db)), sx = sinf(a), sy = -cosf(a);
      float len = m.major ? d.tick + 1 : d.tick * 0.6f;
      Rgb c = m.db >= 0 ? red_ : ink_;
      line(s, d.cx + round_half_up(sx * d.r), d.py + round_half_up(sy * d.r),
           d.cx + round_half_up(sx * (d.r + len)), d.py + round_half_up(sy * (d.r + len)), c);
      if (big_ && m.label) {
        float rr = d.r + d.tick + 5;
        int lx = d.cx + round_half_up(sx * rr), ly = d.py + round_half_up(sy * rr);
        draw_text(s, font_4x6, lx - text_width(font_4x6, m.label) / 2, ly - 3, m.label, c);
      }
    }
    if (d.band) {
      const Font &f = big_ ? font_6x10 : font_4x6;
      int by = d.py + 1;
      if (mono_)
        line(s, d.fx + 1, d.py, d.fx + d.fw - 2, d.py, ink_);
      else
        s.fill_rect(d.fx, by, d.fw, d.fy + d.fh - by, rgb(48, 40, 32));
      Rgb label = mono_ ? ink_ : face_;
      int ty = by + (d.fy + d.fh - by - f.h) / 2 + 1;
      draw_text(s, f, d.fx + 3, ty, which ? "R" : "L", label);
      draw_text(s, f, d.cx - text_width(f, "VU") / 2, ty, "VU", label);
    }
    cap(s, d);
  }

  /* The cap over the pivot: the top half of a disc, the band below. */
  void cap(Surface &s, const Dial &d) {
    for (int dy = 0; dy <= d.pr; dy++) {
      int half = (int) sqrtf((float) (d.pr * d.pr - dy * dy));
      s.fill_rect(d.cx - half, d.py - dy, 2 * half + 1, 1, mono_ ? ink_ : rgb(60, 50, 40));
    }
  }

  Engine engine_;
  Dial dial_[2];
  bool mono_ = false, big_ = false;
  Rgb face_, ink_, red_;
};

/*
 * On a character LCD. Bars: a channel a row, L and R at the left, the rest
 * a bar to the dot (glyphs filled from the left, a column at a time), the
 * peak held as a line a moment, and where the panel shows dim cells, the bar
 * leaving a dim trail as it falls. Needles: a meter in each half of the top
 * row, its scale along the top with the red at the right, and the needle
 * the upper end of one seen through a window; the peak light below.
 */
enum { G_SEG, G_PART_L, G_PART_R, G_PEAK_L, G_PEAK_R };
enum { G_SCALE, G_RED, G_NEEDLE };  // G_NEEDLE..+5: three cells a needle

class MetersText : public TextSaver {
 public:
  void start(TextScreen &t) override {
    engine_.start();
    style_ = meter_input.style;
    needles_ = style_ == 1;
    turn_ = 0;
    t.clear();
  }

  void frame(TextScreen &t, float dt) override {
    engine_.step(dt);
    if (style_ == 2) {
      turn_ += dt;
      if (turn_ >= TURN) {
        turn_ = 0;
        needles_ = !needles_;
      }
    }
    t.clear();
    if (needles_)
      draw_needles(t);
    else
      draw_bars(t);
  }

 private:
  static constexpr float TURN = 30;
  /* The bar a cell high but a dot short of each edge, so L and R stand apart. */
  static constexpr uint8_t TOP = 1, BOTTOM = 6;

  static uint8_t cols_mask(int from, int to) {  // columns [from, to) of 5
    uint8_t m = 0;
    for (int x = from; x < to; x++)
      m |= 0x10 >> x;
    return m;
  }

  void draw_bars(TextScreen &t) {
    const int cols = t.cols(), rows = t.rows();
    const int cells = cols - 1, dots = cells * 5;
    uint8_t seg[8];
    for (int y = 0; y < 8; y++)
      seg[y] = y >= TOP && y <= BOTTOM ? 0x1F : 0;
    t.glyph(G_SEG, seg);
    for (int c = 0; c < 2; c++) {
      const auto &k = engine_[c];
      int lit = round_half_up(fminf(1, k.bar) * dots);
      int pk = round_half_up(fminf(1, k.peak) * dots) - 1;  // the dot it marks
      int tail = round_half_up(fminf(1, k.tail) * dots);
      if (pk < lit)
        pk = -1;  // under the bar: nothing to mark
      int part_cell = lit / 5, part = lit % 5;
      uint8_t pg[8], kg[8];
      for (int y = 0; y < 8; y++) {
        pg[y] = y >= TOP && y <= BOTTOM ? cols_mask(0, part) : 0;
        kg[y] = 0;
      }
      if (pk >= 0) {
        uint8_t line = 0x10 >> (pk % 5);
        uint8_t *g = pk / 5 == part_cell && part ? pg : kg;
        for (int y = 0; y < 8; y++)
          g[y] |= line;
      }
      t.glyph(c ? G_PART_R : G_PART_L, pg);
      t.glyph(c ? G_PEAK_R : G_PEAK_L, kg);
      /* Which rows are this channel's: halves of the panel. */
      int r0 = rows >= 2 ? c * rows / 2 : 0, r1 = rows >= 2 ? (c + 1) * rows / 2 : 1;
      if (rows < 2 && c == 1)
        break;
      const uint8_t part_code = c ? G_PART_R : G_PART_L, peak_code = c ? G_PEAK_R : G_PEAK_L;
      for (int r = r0; r < r1; r++) {
        t.put(0, r, r == r0 ? (c ? 'R' : 'L') : ' ');
        for (int i = 0; i < cells; i++) {
          int x = 1 + i;
          uint8_t code = ' ', other;
          if (i < part_cell)
            code = G_SEG;
          else if (i == part_cell && (part || (pk >= 0 && pk / 5 == i)))
            code = part ? part_code : peak_code;
          else if (pk >= 0 && pk / 5 == i)
            code = peak_code;
          /* The trail, dim: whole segments the bar has just left. */
          other = code;
          if (i >= part_cell && i * 5 + 5 <= tail && code == ' ')
            other = G_SEG;
          else if (i == part_cell && part && i * 5 + 5 <= tail)
            other = G_SEG;
          t.blend(x, r, code, t.dimmable() ? other : code);
        }
      }
    }
  }

  void draw_needles(TextScreen &t) {
    const int cols = t.cols();
    const int mw = cols / 2;  // cells a meter
    const int W = mw * 6 - 1;  // its dots, with the gaps between cells
    const float cx = (W - 1) / 2.0f;
    /* The pivot, below the panel, far enough that the needle reaches the
       ends of the scale. */
    const float P = 1 + cx;
    uint8_t scale[8] = {0x1F, 0x10, 0, 0, 0, 0, 0, 0}, red[8] = {0x1F, 0x1F, 0, 0, 0, 0, 0, 0};
    t.glyph(G_SCALE, scale);
    t.glyph(G_RED, red);
    /* Where the red begins, as a cell. */
    const float zx = cx + P * tanf(needle_angle(VU_ZERO));
    const int red_from = (int) (zx / 6);
    for (int m = 0; m < 2; m++) {
      const auto &k = engine_[m];
      const float tn = tanf(needle_angle(k.needle));
      uint8_t bits[3][8] = {};
      int cell[3] = {-1, -1, -1}, used = 0;
      for (int y = 1; y < 8; y++) {
        int x = round_half_up(cx + (P - y) * tn);
        if (x < 0 || x >= W || x % 6 == 5)
          continue;
        int c = x / 6, j = 0;
        while (j < used && cell[j] != c)
          j++;
        if (j == used) {
          if (used == 3)
            continue;
          cell[used++] = c;
          const uint8_t *base = c >= red_from ? red : scale;
          for (int yy = 0; yy < 8; yy++)
            bits[j][yy] = base[yy];
        }
        bits[j][y] |= 0x10 >> (x % 6);
      }
      for (int j = 0; j < 3; j++)
        if (j < used)
          t.glyph(G_NEEDLE + m * 3 + j, bits[j]);
      int x0 = m * mw;
      for (int c = 0; c < mw; c++) {
        uint8_t code = c >= red_from ? G_RED : G_SCALE;
        for (int j = 0; j < used; j++)
          if (cell[j] == c)
            code = G_NEEDLE + m * 3 + j;
        t.put(x0 + c, 0, code);
      }
      if (t.rows() > 1) {
        t.put(x0, 1, m ? 'R' : 'L');
        if (k.over > 0)
          t.put(x0 + mw - 1, 1, '*');
      }
    }
  }

  Engine engine_;
  uint8_t style_ = 0;
  bool needles_ = false;
  float turn_ = 0;
};

}  // namespace

Saver *make_meters() { return new Meters(); }
TextSaver *make_meters_text() { return new MetersText(); }

AD_REGISTER_SAVER("meters", make_meters, make_meters_text)

}  // namespace adcore
#endif
