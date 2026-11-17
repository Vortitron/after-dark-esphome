#if defined(AD_SAVER_DOLPHINS) || defined(AD_ALL_SAVERS)
#include "ad_sky.h"

#include <cmath>

namespace adcore {

Sky sky;

Weather weather_from(const std::string &s) {
  static const struct {
    const char *name;
    Weather w;
  } NAMES[] = {
      {"sunny", WX_SUNNY},
      {"clear", WX_SUNNY},
      {"clear-night", WX_CLEAR_NIGHT},
      {"partlycloudy", WX_PARTLY},
      {"cloudy", WX_CLOUDY},
      {"fog", WX_FOG},
      {"rainy", WX_RAIN},
      {"pouring", WX_POURING},
      {"snowy", WX_SNOW},
      {"snowy-rainy", WX_SLEET},
      {"hail", WX_HAIL},
      {"lightning", WX_STORM},
      {"lightning-rainy", WX_STORM},
      {"windy", WX_WIND},
      {"windy-variant", WX_WIND},
      {"exceptional", WX_EXCEPTIONAL},
  };
  for (const auto &n : NAMES)
    if (s == n.name)
      return n.w;
  return WX_UNKNOWN;
}

namespace {

const double RAD = M_PI / 180;

double rev(double deg) {
  deg = fmod(deg, 360);
  return deg < 0 ? deg + 360 : deg;
}

/* Right ascension and declination (degrees) from ecliptic longitude and
   latitude. */
void equatorial(double lambda, double beta, double eps, double &ra, double &dec) {
  double l = lambda * RAD, b = beta * RAD, e = eps * RAD;
  ra = rev(atan2(sin(l) * cos(e) - tan(b) * sin(e), cos(l)) / RAD);
  dec = asin(sin(b) * cos(e) + cos(b) * sin(e) * sin(l)) / RAD;
}

/* Where a body is between its rising and setting, from the local sidereal
   time. h0: its altitude as it rises (refraction, its size, parallax). */
Body place(double lst, double ra, double dec, double lat, double h0) {
  Body b;
  double phi = lat * RAD, d = dec * RAD;
  double ha = rev(lst - ra);
  if (ha > 180)
    ha -= 360;  // west of south positive, east negative
  b.alt = (float) (asin(sin(phi) * sin(d) + cos(phi) * cos(d) * cos(ha * RAD)) / RAD);
  double c = (sin(h0 * RAD) - sin(phi) * sin(d)) / (cos(phi) * cos(d));
  if (c >= 1)
    return b;  // below the horizon all day
  double h_set = c <= -1 ? 180 : acos(c) / RAD;
  if (fabs(ha) >= h_set)
    return b;
  b.up = true;
  b.across = (float) ((ha + h_set) / (2 * h_set));
  double top = 90 - fabs(lat - dec);
  b.height = top > h0 ? (float) ((b.alt - h0) / (top - h0)) : 1;
  b.height = b.height < 0 ? 0 : b.height > 1 ? 1 : b.height;
  return b;
}

}  // namespace

void sun_and_moon(double utc, float lat, float lon, Body &sun, Body &moon, float &lit, bool &waxing) {
  double n = utc / 86400.0 + 2440587.5 - 2451545.0;  // days from J2000
  double t = n / 36525;
  double eps = 23.439 - 0.0000004 * n;
  double lst = rev(280.46061837 + 360.98564736629 * n + lon);

  /* The sun. */
  double g = rev(357.528 + 0.9856003 * n) * RAD;
  double ls = rev(280.460 + 0.9856474 * n + 1.915 * sin(g) + 0.020 * sin(2 * g));
  double ra, dec;
  equatorial(ls, 0, eps, ra, dec);
  sun = place(lst, ra, dec, lat, -0.833);

  /* The moon (the Astronomical Almanac's low-precision series). */
  auto s = [](double a, double b, double t) { return sin(rev(a + b * t) * RAD); };
  double lm = rev(218.32 + 481267.881 * t + 6.29 * s(135.0, 477198.87, t) - 1.27 * s(259.3, -413335.36, t) +
                  0.66 * s(235.7, 890534.22, t) + 0.21 * s(269.9, 954397.74, t) - 0.19 * s(357.5, 35999.05, t) -
                  0.11 * s(186.5, 966404.03, t));
  double bm = 5.13 * s(93.3, 483202.02, t) + 0.28 * s(228.2, 960400.89, t) - 0.28 * s(318.3, 6003.15, t) -
              0.17 * s(217.6, -407332.21, t);
  equatorial(lm, bm, eps, ra, dec);
  /* Seen from the ground, not the earth's centre: a degree lower. */
  moon = place(lst, ra, dec, lat, 0.125);

  double elong = acos(cos(bm * RAD) * cos((lm - ls) * RAD));
  lit = (float) ((1 - cos(elong)) / 2);
  waxing = sin((lm - ls) * RAD) > 0;
}

}  // namespace adcore
#endif
