#pragma once
/*
 * ad_meters.h - what drives the meters: a level per channel from the YAML (a
 * sensor's state, or a lambda read every frame), between `min` and `max`. With
 * neither, the meters dance to made-up music.
 */
#include <cmath>
#include <cstdint>
#include <functional>

namespace adcore {

struct MeterInput {
  /* Read every frame, if set; otherwise the last value given. */
  std::function<float()> fn[2];
  float value[2] = {NAN, NAN};
  bool given[2] = {false, false};
  float lo = 0, hi = 100;
  /* On a character LCD: 0 bars, 1 needles, 2 taking turns. */
  uint8_t style = 0;

  void set(int ch, float v) {
    value[ch] = v;
    given[ch] = true;
  }
  bool live() const { return given[0] || given[1] || fn[0] || fn[1]; }
  /* Across the scale, 0-1 (and a little past, to pin a needle). A channel
     with no source of its own follows the other. */
  float read(int ch) const {
    if (!given[ch] && !fn[ch])
      ch = 1 - ch;
    float v = fn[ch] ? fn[ch]() : value[ch];
    if (std::isnan(v) || hi == lo)
      return 0;
    v = (v - lo) / (hi - lo);
    return v < 0 ? 0 : v > 1.05f ? 1.05f : v;
  }
};
extern MeterInput meter_input;

}  // namespace adcore
