/*
 * ad_pioneer.cpp - Pioneer, the dolphin clips from Pioneer's organic-EL car
 * stereos of the early 2000s, played as they were recorded. Not an After Dark
 * module. The clips come from Pioneer's OEL Screen Saver Studio as .lkd files;
 * tools/lkd2c.py turns them into ad_art_pioneer_<clip>.h (the format was
 * worked out by carozerra, https://github.com/youxufkhan/carozerra).
 *
 *   dolphins   movie8_f.lkd: dolphins underwater, 60 frames of 256x64
 *   diver      alt_diverdolphins.lkd: a diver swimming with dolphins,
 *              149 frames of 128x33, from the smaller panels
 *
 * `pioneer: {clips: [...]}` in the YAML defines AD_PIONEER_PICKED and one
 * AD_PIONEER_<CLIP> each; with neither, both are in. Each is a couple of
 * hundred KB of flash, which is why they are their own saver and not a mode
 * of Dolphins.
 *
 * A clip is scaled to the width of the screen, keeping its shape, and played
 * at the stereo's 60 ms a frame. Where it would have to shrink but the
 * screen is tall enough for it (the 256x64 clip on a 128x64 OLED), it is
 * shown dot for dot instead and the view pans slowly across it: halving it
 * loses the dither the picture is made of. Shrunk anyway, each pixel takes
 * the average of the dots it covers, dithered back to the four shades. On a
 * 1-bit panel the brightest shade lights, the next is mostly lit and the
 * dimmest is a sparse dither, which keeps the shapes.
 */
#if defined(AD_SAVER_PIONEER) || defined(AD_ALL_SAVERS)
#include "ad_core.h"

#include <cstring>

#ifndef AD_PIONEER_PICKED
#define AD_PIONEER_DOLPHINS
#define AD_PIONEER_DIVER
#endif
#ifdef AD_PIONEER_DOLPHINS
#include "ad_art_pioneer_dolphins.h"
#endif
#ifdef AD_PIONEER_DIVER
#include "ad_art_pioneer_diver.h"
#endif

namespace adcore {

namespace {

struct Clip {
  const uint8_t *data;
  int w, h, frames;
  /* Times round before the next clip, so each plays for ten seconds or so. */
  int loops;
};

const Clip CLIPS[] = {
#ifdef AD_PIONEER_DOLPHINS
    {pioneer_dolphins::DATA, pioneer_dolphins::W, pioneer_dolphins::H, pioneer_dolphins::FRAMES, 3},
#endif
#ifdef AD_PIONEER_DIVER
    {pioneer_diver::DATA, pioneer_diver::W, pioneer_diver::H, pioneer_diver::FRAMES, 1},
#endif
};
const int CLIP_COUNT = sizeof CLIPS / sizeof CLIPS[0];

const float FRAME_TIME = 0.06f;

/* The four shades of the dolphins clip, black to full. */
const Rgb SHADES[4] = {{0, 0, 0}, {7, 158, 175}, {5, 199, 220}, {0, 255, 255}};

const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

class Pioneer : public Saver {
 public:
  ~Pioneer() override { delete[] shown_; }

  void start(Surface &s) override {
    mono_ = s.mono();
    sw_ = s.width();
    sh_ = s.height();
    s.clear();
    clip_ = rng.below(CLIP_COUNT);
    begin_clip();
  }

  void frame(Surface &s, float dt) override {
    clock_ += dt;
    if (clock_ < FRAME_TIME)
      return;
    clock_ -= FRAME_TIME;
    if (clock_ > FRAME_TIME)
      clock_ = 0;
    draw(s);
    played_ += FRAME_TIME;
    if (++frame_ >= CLIPS[clip_].frames) {
      frame_ = 0;
      if (++loop_ >= CLIPS[clip_].loops && CLIP_COUNT > 1) {
        clip_ = (clip_ + 1) % CLIP_COUNT;
        s.clear();
        begin_clip();
      }
    }
  }

 private:
  /* Where the clip goes: as wide as the screen will take, centred. */
  void begin_clip() {
    const Clip &c = CLIPS[clip_];
    float scale = fminf((float) sw_ / c.w, (float) sh_ / c.h);
    pan_ = scale < 1 && sh_ >= c.h ? c.w - sw_ : 0;
    if (pan_ > 0)
      scale = 1;
    dw_ = pan_ > 0 ? sw_ : (int) (c.w * scale);
    dh_ = (int) (c.h * scale);
    if (dw_ < 1)
      dw_ = 1;
    if (dh_ < 1)
      dh_ = 1;
    dx_ = (sw_ - dw_) / 2;
    dy_ = (sh_ - dh_) / 2;
    frame_ = loop_ = 0;
    clock_ = 0;
    played_ = 0;
    delete[] shown_;
    shown_ = new uint8_t[dw_ * dh_];
    memset(shown_, 0, dw_ * dh_);  // the screen was just cleared to black
  }

  static int level(const uint8_t *f, int w, int x, int y) {
    int i = y * w + x;
    return f[i >> 2] >> (6 - 2 * (i & 3)) & 3;
  }

  /* Shade 0-3 for screen pixel (x, y) of the clip's rectangle. */
  int shade(const uint8_t *f, const Clip &c, int x, int y) const {
    if (pan_ > 0)
      return level(f, c.w, x + offset_, y);
    int x0 = x * c.w / dw_, x1 = (x + 1) * c.w / dw_;
    int y0 = y * c.h / dh_, y1 = (y + 1) * c.h / dh_;
    if (x1 <= x0 + 1 && y1 <= y0 + 1)
      return level(f, c.w, x0, y0);
    if (x1 > c.w)
      x1 = c.w;
    if (y1 > c.h)
      y1 = c.h;
    int sum = 0, n = 0;
    for (int yy = y0; yy < y1; yy++)
      for (int xx = x0; xx < x1; xx++) {
        sum += level(f, c.w, xx, yy);
        n++;
      }
    /* The average in sixteenths of a shade, dithered to a whole one. */
    int v = sum * 16 / n;
    int whole = v >> 4;
    if ((v & 15) > BAYER[y & 3][x & 3])
      whole++;
    return whole > 3 ? 3 : whole;
  }

  void draw(Surface &s) {
    const Clip &c = CLIPS[clip_];
    const uint8_t *f = c.data + (size_t) frame_ * ((c.w * c.h + 3) / 4);
    if (pan_ > 0) {
      /* There and back, about ten seconds each way. */
      float u = fmodf(played_ / 20, 1);
      offset_ = (int) lrintf(pan_ * (0.5f - 0.5f * cosf(u * 2 * 3.14159265f)));
    }
    for (int y = 0; y < dh_; y++) {
      int run_x = -1;
      uint8_t run_v = 0;
      for (int x = 0; x <= dw_; x++) {
        uint8_t v = 0;
        bool changed = false;
        if (x < dw_) {
          int sh = shade(f, c, x, y);
          /* 1-bit: shade 3 on, 0 off, the two between dithered. */
          v = mono_ ? (uint8_t) (sh == 3 || (sh == 2 && BAYER[y & 3][x & 3] < 10) || (sh == 1 && BAYER[y & 3][x & 3] < 2) ? 3 : 0)
                    : (uint8_t) sh;
          uint8_t &old = shown_[y * dw_ + x];
          changed = old != v;
          old = v;
        }
        /* Send runs of one changed colour as one rectangle. */
        if (run_x >= 0 && (!changed || v != run_v)) {
          s.fill_rect(dx_ + run_x, dy_ + y, x - run_x, 1, mono_ ? (run_v ? Rgb{255, 255, 255} : BLACK) : SHADES[run_v]);
          run_x = -1;
        }
        if (changed && run_x < 0) {
          run_x = x;
          run_v = v;
        }
      }
    }
  }

  bool mono_ = false;
  int sw_ = 0, sh_ = 0, dw_ = 1, dh_ = 1, dx_ = 0, dy_ = 0;
  int clip_ = 0, frame_ = 0, loop_ = 0;
  int pan_ = 0, offset_ = 0;
  float clock_ = 0, played_ = 0;
  uint8_t *shown_ = nullptr;
};

}  // namespace

Saver *make_pioneer() { return new Pioneer(); }

AD_REGISTER_SAVER("pioneer", make_pioneer, nullptr)

}  // namespace adcore
#endif
