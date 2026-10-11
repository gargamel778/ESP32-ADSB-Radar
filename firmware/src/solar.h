// Sun position (low precision almanac formulas, about 0.1 degree): decides when automatic night mode is on. Header-only so it can be tested on a computer.
#pragma once
#include <math.h>
#include <time.h>

namespace solar {

// Degrees above the horizon (geometric, no refraction) for a place and a UTC time.
inline double elevationDeg(double lat, double lon, time_t utc) {
  const double rad = M_PI / 180.0;
  const double n = (double)utc / 86400.0 + 2440587.5 - 2451545.0;   // days since J2000
  const double L = fmod(280.460 + 0.9856474 * n, 360.0);
  const double g = fmod(357.528 + 0.9856003 * n, 360.0) * rad;
  const double lambda = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * rad;
  const double eps = (23.439 - 0.0000004 * n) * rad;
  const double ra = atan2(cos(eps) * sin(lambda), cos(lambda));
  const double dec = asin(sin(eps) * sin(lambda));
  const double gmst = fmod(18.697374558 + 24.06570982441908 * n, 24.0) * 15.0 * rad;
  const double ha = gmst + lon * rad - ra;
  return asin(sin(lat * rad) * sin(dec) + cos(lat * rad) * cos(dec) * cos(ha)) / rad;
}

}  // namespace solar
