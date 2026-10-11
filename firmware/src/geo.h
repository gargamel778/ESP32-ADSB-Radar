// Offline place data (embedded in flash, built by tools/build_geo.py from GeoNames cities15000, GeoNames countryInfo, Natural Earth 110m and the tz database):
// nearest city -> country + time zone, city search by name, country and zone lists, and the world outline for the map picker.
// GeoNames data is CC-BY 4.0 (geonames.org); Natural Earth and the tz database are public domain.
#pragma once
#include <Arduino.h>

namespace geo {

struct Place {
  char name[32];     // ASCII city name
  char cc[3];        // ISO 3166-1 alpha-2
  int country;       // index into the country list
  int zone;          // index into the zone list
  float lat, lon;    // degrees (0.01 resolution)
};

bool begin();                                                       // check the embedded data
int cityCount();
bool nearest(double lat, double lon, Place &out, float *distKm = nullptr);   // closest city, whatever the distance
// Case-insensitive search by city name; names that start with the text come first, then names that contain it, each group most populous first.
int search(const char *text, Place *out, int max);

int countryCount();
const char *countryCode(int i);                                     // sorted by ISO code
const char *countryName(int i);
int countryByCode(const char *cc);                                  // -1 if unknown

int zoneCount();
const char *zoneName(int i);                                        // IANA name, sorted alphabetically ("Europe/Paris")
const char *zonePosix(int i);                                       // POSIX TZ rule for setenv("TZ")
int zoneByName(const char *iana);                                   // -1 if unknown
int zoneByPosix(const char *posix);                                 // first zone with exactly this rule, -1 if none

// World outline (country polygons, 0.01 degree units) for the map picker.
struct Ring { uint32_t start; uint16_t count; };
int worldRings();
Ring worldRing(int i);
void worldPoint(uint32_t idx, int16_t &lon100, int16_t &lat100);

}  // namespace geo
