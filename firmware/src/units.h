// Unit systems for the display. Internally everything is km (distance), ft (altitude) and kt (speed); these convert on output.
#pragma once
#include <Arduino.h>
#include <math.h>

namespace units {

enum Dist : uint8_t { NM = 0, KM = 1, MI = 2 };
enum Alt : uint8_t { FT = 0, M = 1, FL = 2 };           // FL = flight level (hundreds of feet)
enum Spd : uint8_t { KT = 0, KMH = 1, MPH = 2 };
constexpr int DIST_N = 3, ALT_N = 3, SPD_N = 3;

inline const char *name(Dist u) { static const char *n[] = {"nm", "km", "mi"}; return n[u % DIST_N]; }
inline const char *name(Alt u) { static const char *n[] = {"ft", "m", "FL"}; return n[u % ALT_N]; }
inline const char *name(Spd u) { static const char *n[] = {"kt", "km/h", "mph"}; return n[u % SPD_N]; }

inline float fromKm(float km, Dist u) { return u == NM ? km / 1.852f : u == MI ? km / 1.609344f : km; }
inline int fromFt(int ft, Alt u) { return u == M ? (int)lroundf(ft * 0.3048f) : u == FL ? (int)lroundf(ft / 100.0f) : ft; }
inline int fromKt(float kt, Spd u) { return u == KMH ? (int)lroundf(kt * 1.852f) : u == MPH ? (int)lroundf(kt * 1.150779f) : (int)lroundf(kt); }

// Radar range steps, in each unit, chosen so the five rings read as round numbers (range/5). The API limit is 250 NM (463 km).
constexpr int RANGE_STEPS = 7;
inline const int *rangeSteps(Dist u) {
  static const int nm[RANGE_STEPS] = {10, 25, 50, 100, 150, 200, 250};
  static const int km[RANGE_STEPS] = {20, 50, 100, 150, 200, 300, 400};
  static const int mi[RANGE_STEPS] = {10, 25, 50, 100, 150, 200, 250};
  return u == NM ? nm : u == MI ? mi : km;
}
inline float toKm(float v, Dist u) { return u == NM ? v * 1.852f : u == MI ? v * 1.609344f : v; }
inline int nearestRangeStep(Dist u, float km) {  // the step (in unit u) whose range is closest to km
  int best = 0; float bd = 1e9f;
  for (int i = 0; i < RANGE_STEPS; i++) { float d = fabsf(toKm((float)rangeSteps(u)[i], u) - km); if (d < bd) { bd = d; best = i; } }
  return best;
}

}  // namespace units
