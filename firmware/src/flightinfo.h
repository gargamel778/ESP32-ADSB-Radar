// Flight details for the selected aircraft, from two optional services (Settings > Flights, phone page):
//   - FlightAware AeroAPI (needs the user's API key): departure (take-off) time, estimated arrival, route and the flown track.
//   - Planespotters.net (no key, but a contact e-mail in the User-Agent, as they ask): a photo of the airframe, if one exists.
// Nothing is requested until the user taps an aircraft and keeps it selected for a moment; results are cached for a few minutes so selecting
// the same aircraft again costs no API call. FlightAware bills per call, so there is also an hourly ceiling.
// The requests run in the data task (adsb.cpp), one HTTPS exchange per pass, so the aircraft fetch is never held up for long.
#pragma once
#include <Arduino.h>
#include <time.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace flightinfo {

enum FaState : uint8_t { FA_NONE, FA_PENDING, FA_OK, FA_ERROR };
enum PhotoState : uint8_t { PH_NONE, PH_PENDING, PH_OK, PH_ABSENT, PH_ERROR };
enum TimeKind : uint8_t { T_NONE = 0, T_SCHEDULED, T_ESTIMATED, T_ACTUAL };

struct Info {
  FaState fa;
  PhotoState photo;
  char status[32];             // FlightAware's own status ("En Route / On Time")
  char err[44];                // why FlightAware gave nothing (fa == FA_ERROR)
  char oCode[5], oCity[24], dCode[5], dCity[24];   // origin / destination (IATA code, city)
  time_t dep, eta;             // take-off (wheels up) and arrival (wheels down), UTC; 0 = unknown
  TimeKind depKind, etaKind;
  int16_t progress;            // percent of the flight done, -1 unknown
  int16_t delayMin;            // arrival delay in minutes (INT16_MIN unknown)
  int trackPoints;
  char photographer[32];
  int photoW, photoH;
};

struct Status {
  bool enabled, keySet, emailSet;
  char text[48];               // result of the last FlightAware lookup, or why nothing happens
  char test[48];               // result of the last key test
  uint16_t callsHour, callsTotal;
};

void begin();
void request(const char *callsign, const char *hex, const char *reg);   // the user selected this aircraft
void cancel();                                                           // selection cleared
bool get(const char *callsign, const char *hex, Info &out);              // what is known about that aircraft right now
int copyTrack(const char *callsign, const char *hex, float *lat, float *lon, int maxPoints);   // flown track, oldest first
bool drawPhoto(lgfx::LGFX_Sprite *dst, int x, int y, const char *callsign, const char *hex);   // false if there is no photo
uint32_t version();                                                      // changes whenever something new arrived (repaint trigger)
bool wanted();                                                           // the data task has work for serve()
void serve();                                                            // one HTTPS exchange of the pending work (data task)
void flush();                                                            // settings changed: forget everything cached
Status status();
void requestTest();                                                      // check the API key (runs in the data task)

}  // namespace flightinfo
