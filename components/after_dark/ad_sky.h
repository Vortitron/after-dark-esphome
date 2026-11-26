#pragma once
/*
 * ad_sky.h - the real sky for the dolphins on a character LCD: where the sun
 * and moon are (low-precision formulae, good to a degree or so, which is a
 * sliver of a 16-column sky), the moon's phase, and the weather now.
 */
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace adcore {

/* Home Assistant's weather conditions, as far as a 5x8 glyph tells them apart. */
enum Weather : uint8_t {
  WX_UNKNOWN,
  WX_SUNNY,
  WX_CLEAR_NIGHT,
  WX_PARTLY,
  WX_CLOUDY,
  WX_FOG,
  WX_RAIN,
  WX_POURING,
  WX_SNOW,
  WX_SLEET,
  WX_HAIL,
  WX_STORM,
  WX_WIND,
  WX_EXCEPTIONAL,
};

/* Set from the YAML's sensors (dolphins: latitude, longitude, weather). */
struct Sky {
  float lat = 1000, lon = 1000;  // unset
  Weather weather = WX_UNKNOWN;
  /* Lines that take turns at the left of the sky (dolphins: readings:), as
     UTF-8; an empty one is skipped. */
  std::vector<std::function<std::string()>> readings;
  bool located() const { return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 360; }
};
extern Sky sky;
Weather weather_from(const std::string &condition);

/* A body seen from the panel. `across` runs from 0 where it rose to 1 where
   it sets; `height` from 0 on the horizon to 1 at its highest today. */
struct Body {
  bool up = false;
  float across = 0, height = 0;
  float alt = 0;  // degrees
};
/* `utc` in seconds since 1970. lit: the moon's lit fraction, 0-1; waxing:
   its lit side is the one away from the sun's setting. */
void sun_and_moon(double utc, float lat, float lon, Body &sun, Body &moon, float &lit, bool &waxing);

}  // namespace adcore
