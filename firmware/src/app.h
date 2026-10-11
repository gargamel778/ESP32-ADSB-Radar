// Application-level actions shared by the touch UI, the settings screen and the serial dev commands:
// each one changes a setting, tells the affected modules, and schedules the save.
#pragma once
#include <Arduino.h>
#include "geo.h"

namespace app {

void setRangeStep(int step);       // clamped to the available round ranges; rescales the radar and fetches for newly covered area
void zoom(int dir);                // -1 = zoom in (smaller range), +1 = zoom out
void setDist(int u);               // units::Dist; keeps roughly the same coverage by snapping to the nearest round range
void setAlt(int u);                // units::Alt
void setSpd(int u);                // units::Spd
void setClock24(bool on);
void setUnitsAuto(bool on);        // true: units and clock format follow the country profile again
void setCountry(const char *cc);   // dev (serial "CUS"); units follow the country again
void setTheme(int idx);            // font theme of the aircraft table
void setPalette(int role, int idx);   // palette::Role, swatch index
void setBrightness(int percent);   // 10..100 (the night brightness, 2..100, while night mode is in force)
void setDayBrightness(int percent);   // 10..100, whatever mode is in force
void setNightBrightness(int percent); // 2..100
void stepBrightness(int dir);      // one step up/down for whichever brightness is in force
void setNightMode(int mode);       // 0 off, 1 auto, 2 on
void setLocation(double lat, double lon, const char *place, const char *country, const char *tzPosix, const char *tzName = nullptr);   // centre of the radar; country/tz may be empty to keep them
// Moves the radar's centre and, where the user has not chosen otherwise, derives country (-> units) and time zone from the nearest city. place: shown in
// the footer; empty = name of the nearest city. Returns the detected city (for the caller's message) or nullptr if the place data is missing.
// A move of less than 40 km keeps the country and time zone you already have (nearest-city detection can flip them near borders and in disputed
// areas); force = true (the user asked for "automatic") always re-detects.
const char *setLocationAuto(double lat, double lon, const char *place, bool force = false);
void setLocationCity(const geo::Place &p);   // the user picked this city: its country and time zone apply where they are on automatic
void resetHome();                  // dev: back to the build-time home (config/secrets.h) with its country and time zone
void setCountryAuto(bool on);      // true: follow the location again
void setCountryManual(const char *cc);   // pick a country by hand (units follow it while "units from country" is on)
void setZoneAuto(bool on);
void setZoneManual(int zone);      // index into geo::zoneName()
void setSdLog(bool on);            // SD card logging on/off
void setLogSecs(int secs);         // an aircraft is logged at most once per this many seconds (10, 30, 60)
void sdEject();                    // flush and unmount: safe to remove the card
void sdMount();
void setFlightEnabled(bool on);    // flight details for the selected aircraft: FlightAware (needs a key) and Planespotters photos
void setFlightKey(const char *key);     // AeroAPI key; spaces and quotes are dropped, empty clears it
void setFlightEmail(const char *email); // contact address for the photo service's User-Agent
void setFlightPhoto(bool on);
void setFlightTrack(bool on);
void testFlightKey();              // asks FlightAware whether the key is accepted (result: flightinfo::status().test)
void setMapOverlay(bool on);        // coastlines, borders and airports behind the radar
void setShowDiag(bool on);         // diagnostic lines in the radar's status text
void setTrail(int deg);            // sweep trail length, one of settings::kTrails
void setDisplayMode(int mode);     // saved now, takes effect after a restart
void restart();                    // save and reboot

}  // namespace app
