#include "geo.h"
#include <math.h>
#include <string.h>

extern const uint8_t geoStart[] asm("_binary_data_geo_bin_start");
extern const uint8_t geoEnd[] asm("_binary_data_geo_bin_end");
extern const uint8_t worldStart[] asm("_binary_data_world_bin_start");
extern const uint8_t worldEnd[] asm("_binary_data_world_bin_end");

namespace geo {
namespace {

// geo.bin layout (little endian): 32-byte header, zone records (8 B), country records (8 B), city records (12 B), string pool.
struct Header { char magic[4]; uint16_t version, nZones, nCountries, pad; uint32_t nCities, offZones, offCountries, offCities, offPool; };
bool ok = false;
Header H;
const uint8_t *B = geoStart;

template <typename T> inline T rd(const uint8_t *p) { T v; memcpy(&v, p, sizeof(T)); return v; }   // flash data may be unaligned
inline const char *str(uint32_t off) { return (const char *)(B + H.offPool + off); }

struct CityRec { int16_t lat, lon; uint8_t cc, pad; uint16_t zone; uint32_t name; };
inline CityRec city(int i) { return rd<CityRec>(B + H.offCities + (size_t)i * 12); }

void fill(Place &p, const CityRec &c) {
  strlcpy(p.name, str(c.name), sizeof(p.name));
  p.country = c.cc;
  p.zone = c.zone;
  strlcpy(p.cc, countryCode(c.cc), sizeof(p.cc));
  p.lat = c.lat / 100.0f;
  p.lon = c.lon / 100.0f;
}

bool containsNoCase(const char *hay, const char *needle, bool prefixOnly) {
  const size_t n = strlen(needle);
  if (!n) return true;
  for (const char *h = hay; *h; h++) {
    size_t k = 0;
    while (k < n && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)needle[k])) k++;
    if (k == n) return true;
    if (prefixOnly) return false;
  }
  return false;
}

}  // namespace

bool begin() {
  if ((size_t)(geoEnd - geoStart) < sizeof(Header)) return false;
  memcpy(&H, geoStart, sizeof(Header));
  ok = memcmp(H.magic, "GEO1", 4) == 0 && H.nCities > 0;
  Serial.printf("[geo] %s: %u cities, %u zones, %u countries, %u KB; world outline %d rings\n", ok ? "ok" : "BAD DATA", (unsigned)H.nCities, H.nZones, H.nCountries,
                (unsigned)((geoEnd - geoStart) / 1024), ok ? worldRings() : 0);
  return ok;
}

int cityCount() { return ok ? (int)H.nCities : 0; }

bool nearest(double lat, double lon, Place &out, float *distKm) {
  if (!ok) return false;
  const double k = cos(lat * M_PI / 180.0);
  double best = 1e30;
  int bi = -1;
  for (uint32_t i = 0; i < H.nCities; i++) {
    const CityRec c = city(i);
    const double dy = c.lat / 100.0 - lat;
    double dx = c.lon / 100.0 - lon;
    if (dx > 180) dx -= 360; else if (dx < -180) dx += 360;
    dx *= k;
    const double d = dx * dx + dy * dy;
    if (d < best) { best = d; bi = i; }
  }
  if (bi < 0) return false;
  fill(out, city(bi));
  if (distKm) *distKm = (float)(sqrt(best) * 111.2);
  return true;
}

int search(const char *text, Place *out, int max) {
  if (!ok || !text || !*text || max <= 0) return 0;
  int n = 0;
  for (int pass = 0; pass < 2 && n < max; pass++)
    for (uint32_t i = 0; i < H.nCities && n < max; i++) {
      const CityRec c = city(i);
      const char *name = str(c.name);
      const bool prefix = containsNoCase(name, text, true);
      if (pass == 0 ? !prefix : (prefix || !containsNoCase(name, text, false))) continue;
      fill(out[n++], c);
    }
  return n;
}

int countryCount() { return ok ? H.nCountries : 0; }
const char *countryCode(int i) { static char buf[3]; if (!ok || i < 0 || i >= H.nCountries) return "--"; memcpy(buf, B + H.offCountries + i * 8, 2); buf[2] = 0; return buf; }
const char *countryName(int i) { if (!ok || i < 0 || i >= H.nCountries) return ""; return str(rd<uint32_t>(B + H.offCountries + i * 8 + 4)); }
int countryByCode(const char *cc) {
  if (!ok || !cc || strlen(cc) < 2) return -1;
  for (int i = 0; i < H.nCountries; i++) if (!memcmp(B + H.offCountries + i * 8, cc, 2)) return i;
  return -1;
}

int zoneCount() { return ok ? H.nZones : 0; }
const char *zoneName(int i) { if (!ok || i < 0 || i >= H.nZones) return ""; return str(rd<uint32_t>(B + H.offZones + i * 8)); }
const char *zonePosix(int i) { if (!ok || i < 0 || i >= H.nZones) return "UTC0"; return str(rd<uint32_t>(B + H.offZones + i * 8 + 4)); }
int zoneByName(const char *iana) { for (int i = 0; ok && iana && i < H.nZones; i++) if (!strcmp(zoneName(i), iana)) return i; return -1; }
int zoneByPosix(const char *posix) { for (int i = 0; ok && posix && i < H.nZones; i++) if (!strcmp(zonePosix(i), posix)) return i; return -1; }

// ---- world outline
int worldRings() { return (size_t)(worldEnd - worldStart) >= 16 && !memcmp(worldStart, "MAP1", 4) ? (int)rd<uint32_t>(worldStart + 4) : 0; }
Ring worldRing(int i) {
  const uint8_t *r = worldStart + 16 + (size_t)i * 8;
  return {rd<uint32_t>(r), rd<uint16_t>(r + 4)};
}
void worldPoint(uint32_t idx, int16_t &lon100, int16_t &lat100) {
  const uint8_t *p = worldStart + 16 + (size_t)worldRings() * 8 + (size_t)idx * 4;
  lon100 = rd<int16_t>(p);
  lat100 = rd<int16_t>(p + 2);
}

}  // namespace geo
