#include "mapdata.h"
#include <string.h>

extern const uint8_t mapStart[] asm("_binary_data_mapdata_bin_start");
extern const uint8_t mapEnd[] asm("_binary_data_mapdata_bin_end");

namespace mapdata {
namespace {

constexpr double UNIT = 512.0;
struct Header { char magic[4]; uint32_t nChunks, nAirports, offChunks, offPoints, offAirports; };
Header H;
bool ok = false;

template <typename T> inline T rd(const uint8_t *p) { T v; memcpy(&v, p, sizeof(T)); return v; }   // flash data may be unaligned

bool init() {
  static bool tried = false;
  if (!tried) {
    tried = true;
    if ((size_t)(mapEnd - mapStart) >= sizeof(Header)) {
      memcpy(&H, mapStart, sizeof(Header));
      ok = !memcmp(H.magic, "MAP2", 4);
    }
    Serial.printf("[map] %s: %u chunks, %u airports, %u KB\n", ok ? "ok" : "no data", ok ? (unsigned)H.nChunks : 0u, ok ? (unsigned)H.nAirports : 0u, (unsigned)((mapEnd - mapStart) / 1024));
  }
  return ok;
}

// the box shifted by one world width: data longitudes lie in [-180, 180]
bool shifted(double lonMin, double lonMax, int off, double &lo, double &hi) {
  lo = lonMin - off; hi = lonMax - off;
  return hi >= -180.0 && lo <= 180.0;
}

}  // namespace

bool ready() { return init(); }

void lines(double latMin, double latMax, double lonMin, double lonMax, LineFn fn, void *ctx) {
  if (!init()) return;
  for (int off = -360; off <= 360; off += 360) {
    double lo, hi;
    if (!shifted(lonMin, lonMax, off, lo, hi)) continue;
    const int bx0 = (int)floor(lo * 100), bx1 = (int)ceil(hi * 100), by0 = (int)floor(latMin * 100), by1 = (int)ceil(latMax * 100);
    for (uint32_t i = 0; i < H.nChunks; i++) {
      const uint8_t *c = mapStart + H.offChunks + (size_t)i * 16;
      const int16_t x0 = rd<int16_t>(c), y0 = rd<int16_t>(c + 2), x1 = rd<int16_t>(c + 4), y1 = rd<int16_t>(c + 6);
      if (x1 < bx0 || x0 > bx1 || y1 < by0 || y0 > by1) continue;
      const int kind = c[8], count = c[9];
      const uint8_t *p = mapStart + H.offPoints + rd<uint32_t>(c + 12);
      double la[100], lon[100];
      int32_t x = rd<int32_t>(p), y = rd<int32_t>(p + 4);
      p += 8;
      la[0] = y / UNIT; lon[0] = x / UNIT + off;
      for (int k = 1; k < count && k < 100; k++, p += 4) {
        x += rd<int16_t>(p); y += rd<int16_t>(p + 2);
        la[k] = y / UNIT; lon[k] = x / UNIT + off;
      }
      fn(la, lon, count < 100 ? count : 100, kind, ctx);
    }
  }
}

void airports(double latMin, double latMax, double lonMin, double lonMax, AirportFn fn, void *ctx) {
  if (!init()) return;
  for (int off = -360; off <= 360; off += 360) {
    double lo, hi;
    if (!shifted(lonMin, lonMax, off, lo, hi)) continue;
    for (uint32_t i = 0; i < H.nAirports; i++) {
      const uint8_t *a = mapStart + H.offAirports + (size_t)i * 16;
      const double lon = rd<int32_t>(a) / UNIT, lat = rd<int32_t>(a + 4) / UNIT;
      if (lon < lo || lon > hi || lat < latMin || lat > latMax) continue;
      char code[4];
      memcpy(code, a + 8, 4);
      code[3] = 0;
      fn(lat, lon + off, code, a[12], ctx);
    }
  }
}

}  // namespace mapdata
