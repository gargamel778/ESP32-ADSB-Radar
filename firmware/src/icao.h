// Country of registration from the aircraft's ICAO 24-bit address (its "hex"): ICAO allocates blocks of addresses to states (Annex 10, Vol. III, Table 9-1).
// Available for every aircraft with a normal address; none for TIS-B / non-ICAO addresses ("~...") and unallocated blocks.
#pragma once
#include <Arduino.h>

namespace icao {

const char *country(const char *hex);   // ISO 3166-1 alpha-2 ("IL"), or nullptr

// Flags: 27x18 pixel drawings of the national flags (tools/build_flags.py), embedded in flash.
constexpr int FLAG_W = 27, FLAG_H = 18;
bool hasFlag(const char *iso);
// Draws the flag with its top-left corner at (x, y) into `target` (any LovyanGFX target). False if there is none for this country.
template <typename Gfx> bool drawFlag(Gfx *target, int x, int y, const char *iso);

const uint16_t *flagPixels(const char *iso);   // FLAG_W*FLAG_H RGB565 little-endian, or nullptr

template <typename Gfx>
bool drawFlag(Gfx *target, int x, int y, const char *iso) {
  const uint16_t *px = flagPixels(iso);
  if (!px) return false;
  for (int j = 0; j < FLAG_H; j++)
    for (int i = 0; i < FLAG_W; i++) {
      const uint16_t v = px[j * FLAG_W + i];
      target->drawPixel(x + i, y + j, target->color565(((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31));
    }
  return true;
}

}  // namespace icao
