// Colour swatches the user can pick for the radar, the titles, callsigns and values (settings: Look).
// A swatch is one bright colour; the dimmer shades the radar needs (rings, grid, labels) are scaled from it.
#pragma once
#include <Arduino.h>

namespace palette {

struct Swatch { const char *name; uint8_t r, g, b; };
inline const Swatch kSwatches[] = {
    {"Green",   0, 255,  35},   // the original radar green
    {"Amber", 255, 176,   0},
    {"Cyan",    0, 220, 255},
    {"White", 255, 255, 255},
    {"Yellow", 255, 255,   0},
    {"Orange", 255, 120,   0},
    {"Magenta", 255, 70, 220},
    {"Red",   255,  50,  50},
};
constexpr int COUNT = sizeof(kSwatches) / sizeof(kSwatches[0]);

enum Role : uint8_t { RADAR = 0, TITLES = 1, CALLSIGN = 2, VALUE = 3, LINES = 4 };   // LINES: frames, separators and buttons (radar screen and settings)
constexpr int ROLES = 5;
constexpr uint8_t kDefaults[ROLES] = {0 /*Green*/, 0 /*Green*/, 4 /*Yellow*/, 3 /*White*/, 0 /*Green*/};

struct Rgb { uint8_t r, g, b; };
inline Rgb shade(int idx, float f) {  // swatch scaled to brightness f (0..1)
  const Swatch &s = kSwatches[((idx % COUNT) + COUNT) % COUNT];
  return {(uint8_t)(s.r * f + 0.5f), (uint8_t)(s.g * f + 0.5f), (uint8_t)(s.b * f + 0.5f)};
}
// Radar shades: bright, mid (rings), dim (grid-cross, labels), dark (grid)
constexpr float kBright = 1.0f, kMid = 0.706f, kDim = 0.353f, kDark = 0.176f;

}  // namespace palette
