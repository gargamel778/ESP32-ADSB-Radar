// Persistent settings (NVS). Compile-time values from config/secrets.h are only the first-boot defaults.
// Units follow the country profile until the user overrides one (then unitsAuto becomes false).
#pragma once
#include <Arduino.h>
#include "units.h"
#include "palette.h"

namespace settings {

struct Settings {
  // network
  char wifiSsid[33];
  char wifiPass[65];
  // where the radar is centred
  double lat, lon;
  uint8_t rangeStep;       // index into units::rangeSteps(dist): the radar range, as a round number in the chosen distance unit
  char place[24];          // shown in the footer
  char country[3];         // ISO 3166-1 alpha-2, drives the unit profile (and, in M5, the timezone)
  char tzPosix[48];        // POSIX TZ string for the clock
  char tzName[40];         // IANA name of that zone ("Europe/London"); empty if the rule was typed in by hand
  bool countryAuto;        // the country follows the location (otherwise it was chosen by hand)
  bool tzAuto;             // the time zone follows the location (otherwise it was chosen by hand)
  // units & formats
  bool unitsAuto;          // true: units come from the country profile
  uint8_t dist, alt, spd;  // units::Dist / Alt / Spd
  bool clock24;
  // look
  uint8_t displayMode;     // display backend (display.cpp kModes)
  uint8_t theme;           // font theme of the aircraft table
  uint8_t pal[palette::ROLES];  // swatch index per palette::Role (radar, titles, callsigns, values)
  uint8_t brightness;      // backlight, 10..100 %
  uint8_t nightMode;       // 0 off, 1 automatic (sun below the horizon at the radar's location), 2 always on (see night.h)
  uint8_t nightBright;     // backlight while night mode is in force, 2..100 %
  bool nightLast;          // night mode was in force when last saved: automatic mode uses it until the clock is set after a restart
  uint8_t trail;           // sweep trail length in degrees (15, 25, 40, 60)
  bool showDiag;           // show the diagnostic lines (data age, display mode) in the radar's status text
  bool mapOverlay;         // coastlines, borders and airports behind the radar
  bool sdLog;              // write sightings and a system log to the SD card
  // flight details (flightinfo.h): FlightAware AeroAPI (departure, ETA, track) and Planespotters photos, for the selected aircraft
  bool faEnabled;          // master switch
  char faKey[40];          // AeroAPI key (never shown again once saved)
  char faEmail[64];        // contact address: part of the User-Agent the photo service asks for
  bool faPhoto, faTrack;   // the two optional extras
  uint8_t logSecs;         // an aircraft is logged at most once per this many seconds (10, 30 or 60)
  // access (generated on first boot, kept in NVS)
  char webPin[7];          // 6-digit PIN the LAN web interface asks for
  char apPass[10];         // WPA2 password of the phone-setup access point (shown on the radar's screen / QR code)
};
constexpr uint8_t kLogSecs[] = {10, 30, 60};
constexpr int LOGSECS_N = sizeof(kLogSecs) / sizeof(kLogSecs[0]);
constexpr uint8_t kTrails[] = {15, 25, 40, 60};
constexpr int TRAIL_N = sizeof(kTrails) / sizeof(kTrails[0]);

void begin();                  // load from NVS (first boot: defaults)
Settings &get();               // live settings; call changed() after modifying
void changed();                // schedule a save (written ~3 s after the last change, so UI taps stay smooth)
void tick();                   // call from the main loop: performs the deferred save
void saveNow();                // write immediately (e.g. just before a restart)
void holdSaves();              // from now until the next restart nothing is written to flash (the "Y" demo location must never be stored)
float rangeKm();                // radar range in km for the current step + unit
int apiRadiusNm();             // nautical miles to request from the API (rounded up so the whole range is covered)
void setRangeStep(int step);   // clamp + store; call changed() afterwards
void applyCountryProfile();    // set dist/alt/spd/clock24 from the country (only touches them when unitsAuto)
bool profileKnown(const char *country);

}  // namespace settings
