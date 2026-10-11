// Coastlines, land borders and airports (Natural Earth 1:10m, public domain) for the radar's map overlay. Built by tools/build_map.py, embedded in flash.
#pragma once
#include <Arduino.h>

namespace mapdata {

bool ready();

// Polylines (at most 100 points each) that may touch the box; the box may cross the antimeridian (longitudes are then delivered shifted by +-360 to continue the box).
// kind: 0 coastline, 1 land border.
typedef void (*LineFn)(const double *lat, const double *lon, int n, int kind, void *ctx);
void lines(double latMin, double latMax, double lonMin, double lonMax, LineFn fn, void *ctx);

// Airports inside the box. type: 0 small, 1 mid-size, 2 major.
typedef void (*AirportFn)(double lat, double lon, const char *iata, int type, void *ctx);
void airports(double latMin, double latMax, double lonMin, double lonMax, AirportFn fn, void *ctx);

}  // namespace mapdata
