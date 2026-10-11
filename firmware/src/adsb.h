// ADS-B data client: polls api.airplanes.live on core 0 and publishes snapshots to the UI thread.
// TLS is validated against the built-in Mozilla CA bundle, which needs a valid clock: nothing is fetched until NTP has set the time.
#pragma once
#include <Arduino.h>
#include "config.h"

namespace adsb {

struct Aircraft {
  char hex[8];       // ICAO 24-bit address (6 hex digits; "~" prefix = non-ICAO address), identifies the aircraft between fetches
  char flight[10];   // callsign, trimmed
  char type[8];      // ICAO type code (e.g. B738), may be empty
  char reg[10];      // registration (e.g. 4X-ABW), may be empty
  char squawk[5];    // transponder code ("7700"), may be empty
  char desc[32];     // readable model from the feed ("Airbus A320"), may be empty
  char cat[4];       // emitter category (A3, ...), may be empty
  double lat, lon;
  int32_t altFt;     // -1 unknown, 0 on ground
  float gs;          // knots
  float track;       // degrees true, <0 unknown
};

struct Snapshot {
  int n;
  Aircraft ac[MAX_AIRCRAFT];
  bool ok;               // last fetch succeeded
  int httpCode;          // last HTTP status (or negative internal error)
  char status[40];       // human-readable result of the last attempt ("OK", "No Wi-Fi", "Certificate error", ...)
  uint32_t failStreak;   // consecutive failed attempts
  uint32_t fetchedAtMs;  // millis() of the last *successful* fetch
  uint32_t fetchMs;      // duration of the last fetch
  uint32_t rawBytes;     // payload size of the last fetch
  uint32_t totalInRange; // aircraft with a position in the payload (before keeping the nearest MAX_AIRCRAFT)
};

Snapshot *newSnapshot();           // zeroed Snapshot in PSRAM (they are too big for internal RAM)
// Flight route (origin and destination) by callsign, looked up on adsbdb.com when the user selects an aircraft. The request runs in the data task,
// between aircraft fetches, so only one secure connection is open at a time. Results are cached.
enum RouteState : uint8_t { ROUTE_NONE, ROUTE_PENDING, ROUTE_KNOWN, ROUTE_UNKNOWN };
struct RouteInfo { char oCode[5], oCity[24], dCode[5], dCity[24]; };
void requestRoute(const char *callsign);                    // starts the lookup unless cached or already running
RouteState route(const char *callsign, RouteInfo &out);     // what is known right now
uint32_t routeVersion();                                    // changes whenever a lookup finishes (repaint trigger)

void begin();                      // start the fetch task
bool take(Snapshot &out);          // true if something new was published since the last call
bool busy();                       // a fetch is running right now
bool timeValid();                  // the system clock has been set (NTP)
void fetchNow();                   // wake the fetch task now (range/location changed)
void wake();                       // wake the task for other work (flight details, see flightinfo.h)

}  // namespace adsb
