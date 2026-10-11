#include "settings.h"
#include "config.h"
#include "geo.h"
#include <Preferences.h>

namespace settings {
namespace {

Preferences gPrefs;
Settings S;
bool gDirty = false;
uint32_t gChangedAt = 0;

// Country profiles: which units a country uses by default. These are conventions I assumed, not facts to rely on - review them.
// Aviation itself is ft/kt/nm everywhere; "metric" countries default to km and km/h for the radar's distance and speed.
struct Profile { const char *cc; units::Dist d; units::Alt a; units::Spd s; bool clock24; };
const Profile kProfiles[] = {
    {"IL", units::KM, units::FT, units::KT,  true},    // matches the original sketch: km / ft / kt
    {"US", units::MI, units::FT, units::MPH, false},
    {"CA", units::KM, units::FT, units::KMH, false},
    {"GB", units::MI, units::FT, units::MPH, true},
    {"IE", units::KM, units::FT, units::KMH, true},
    {"AU", units::KM, units::FT, units::KMH, true},
    {"NZ", units::KM, units::FT, units::KMH, true},
    {"DE", units::KM, units::FT, units::KMH, true},
    {"FR", units::KM, units::FT, units::KMH, true},
    {"NL", units::KM, units::FT, units::KMH, true},
    {"ES", units::KM, units::FT, units::KMH, true},
    {"IT", units::KM, units::FT, units::KMH, true},
    {"CH", units::KM, units::FT, units::KMH, true},
    {"JP", units::KM, units::FT, units::KMH, true},
    {"MK", units::KM, units::FT, units::KMH, true},
};
bool listHas(const char *list, const char *cc) {   // list = "US GB LR " (two-letter codes, each followed by a space)
  for (const char *p = list; *p; p += 3) if (p[0] == cc[0] && p[1] == cc[1]) return true;
  return false;
}

// Countries above come from the explicit table; every other country gets rules of thumb: metric (km, km/h, 24 h) except for the places that
// use miles / mph, and the countries where a 12-hour clock is the everyday habit. All of it can be overridden in Settings > Units.
Profile profileFor(const char *cc) {
  for (const Profile &p : kProfiles) if (strncmp(p.cc, cc, 2) == 0) return p;
  Profile p = {"--", units::KM, units::FT, units::KMH, true};
  if (listHas("US GB LR MM ", cc)) { p.d = units::MI; p.s = units::MPH; }
  if (listHas("US CA AU NZ IN PK PH BD EG SA JO CO MY LK ", cc)) p.clock24 = false;
  return p;
}

void loadString(const char *key, char *dst, size_t cap, const char *def) {
  String v = gPrefs.getString(key, def);
  strlcpy(dst, v.c_str(), cap);
}

}  // namespace

bool profileKnown(const char *cc) { return strncmp(profileFor(cc).cc, "--", 2) != 0 || geo::countryByCode(cc) >= 0; }

float rangeKm() { return units::toKm((float)units::rangeSteps((units::Dist)S.dist)[S.rangeStep % units::RANGE_STEPS], (units::Dist)S.dist); }
int apiRadiusNm() { return constrain((int)ceilf(rangeKm() / 1.852f), 5, 250); }
void setRangeStep(int step) { S.rangeStep = (uint8_t)constrain(step, 0, units::RANGE_STEPS - 1); }

void applyCountryProfile() {
  if (!S.unitsAuto) return;
  const Profile &p = profileFor(S.country);
  S.dist = p.d; S.alt = p.a; S.spd = p.s; S.clock24 = p.clock24;
}

void begin() {
  gPrefs.begin("radar", false);
  loadString("ssid", S.wifiSsid, sizeof(S.wifiSsid), WIFI_SSID);
  loadString("pass", S.wifiPass, sizeof(S.wifiPass), WIFI_PASS);
  S.lat = gPrefs.getDouble("lat", HOME_LAT);
  S.lon = gPrefs.getDouble("lon", HOME_LON);
  loadString("place", S.place, sizeof(S.place), LOCATION_NAME);
  loadString("cc", S.country, sizeof(S.country), DEFAULT_COUNTRY);
  loadString("tz", S.tzPosix, sizeof(S.tzPosix), TIMEZONE_POSIX);
  loadString("tzn", S.tzName, sizeof(S.tzName), "");
  if (!S.tzName[0]) {
    if (!strcmp(S.tzPosix, TIMEZONE_POSIX)) strlcpy(S.tzName, TIMEZONE_NAME, sizeof(S.tzName));   // the default rule: its own name, not the first zone that shares it
    else { const int z = geo::zoneByPosix(S.tzPosix); if (z >= 0) strlcpy(S.tzName, geo::zoneName(z), sizeof(S.tzName)); }
  }
  S.countryAuto = gPrefs.getBool("cauto", true);
  S.tzAuto = gPrefs.getBool("tauto", true);
  S.unitsAuto = gPrefs.getBool("uauto", true);
  S.dist = gPrefs.getUChar("ud", units::KM);
  S.alt = gPrefs.getUChar("ua", units::FT);
  S.spd = gPrefs.getUChar("us", units::KT);
  S.clock24 = gPrefs.getBool("c24", true);
  S.displayMode = gPrefs.getUChar("disp3", 0);
  S.theme = gPrefs.getUChar("theme2", 0);
  for (int i = 0; i < palette::ROLES; i++) {
    char key[6];
    snprintf(key, sizeof(key), "pal%d", i);
    S.pal[i] = gPrefs.getUChar(key, i == palette::LINES ? S.pal[palette::RADAR] : palette::kDefaults[i]);   // frames used to follow the radar colour: keep that look until chosen
    if (S.pal[i] >= palette::COUNT) S.pal[i] = palette::kDefaults[i];
  }
  S.brightness = constrain((int)gPrefs.getUChar("bri", 80), 10, 100);
  S.nightMode = gPrefs.getUChar("nmode", 0);
  if (S.nightMode > 2) S.nightMode = 0;
  S.nightBright = constrain((int)gPrefs.getUChar("nbri", 10), 2, 100);
  S.nightLast = gPrefs.getBool("nlast", false);
  S.trail = gPrefs.getUChar("trl", 25);
  S.showDiag = gPrefs.getBool("diag", true);
  S.mapOverlay = gPrefs.getBool("map", false);
  S.sdLog = gPrefs.getBool("sdl", true);
  S.faEnabled = gPrefs.getBool("fae", false);
  loadString("fak", S.faKey, sizeof(S.faKey), "");
  loadString("fem", S.faEmail, sizeof(S.faEmail), "");
  S.faPhoto = gPrefs.getBool("fap", true);
  S.faTrack = gPrefs.getBool("fat", true);
  S.logSecs = gPrefs.getUChar("lgs", 30);
  if (S.logSecs != 10 && S.logSecs != 30 && S.logSecs != 60) S.logSecs = 30;
  // access secrets: random, created once (no ambiguous characters in the AP password: it is read off a screen and typed on a phone)
  loadString("pin", S.webPin, sizeof(S.webPin), "");
  if (strlen(S.webPin) != 6) snprintf(S.webPin, sizeof(S.webPin), "%06u", (unsigned)(esp_random() % 1000000u));
  loadString("appw", S.apPass, sizeof(S.apPass), "");
  if (strlen(S.apPass) != 8) {
    static const char alpha[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++) S.apPass[i] = alpha[esp_random() % (sizeof(alpha) - 1)];
    S.apPass[8] = 0;
  }
  if (strlen(gPrefs.getString("pin", "").c_str()) != 6 || strlen(gPrefs.getString("appw", "").c_str()) != 8) { gPrefs.putString("pin", S.webPin); gPrefs.putString("appw", S.apPass); }   // generated just now: keep them
  applyCountryProfile();  // no-op once the user has overridden a unit
  // range: stored as a step; on first boot derive it from the compile-time radius (nearest round value in the current unit)
  S.rangeStep = gPrefs.isKey("rs") ? gPrefs.getUChar("rs", 3) : units::nearestRangeStep((units::Dist)S.dist, RADAR_RADIUS_NM * 1.852f);
  if (S.rangeStep >= units::RANGE_STEPS) S.rangeStep = 3;
  Serial.printf("[settings] home=%.4f,%.4f range=%.0fkm(api %dNM) cc=%s units=%s/%s/%s%s clock24=%d tz=%s\n", S.lat, S.lon, rangeKm(), apiRadiusNm(), S.country,
                units::name((units::Dist)S.dist), units::name((units::Alt)S.alt), units::name((units::Spd)S.spd), S.unitsAuto ? " (auto)" : " (manual)", (int)S.clock24, S.tzPosix);
}

Settings &get() { return S; }

void changed() { gDirty = true; gChangedAt = millis(); }

void tick() {
  if (!gDirty || millis() - gChangedAt < 3000) return;
  saveNow();
}

static bool gHold = false;
void holdSaves() { gHold = true; gDirty = false; }

void saveNow() {
  gDirty = false;
  if (gHold) return;
  // Preferences only writes values that differ, but writing flash stalls the display interrupt: do it once, after the user stops tapping.
  gPrefs.putString("ssid", S.wifiSsid);
  gPrefs.putString("pass", S.wifiPass);
  gPrefs.putDouble("lat", S.lat);
  gPrefs.putDouble("lon", S.lon);
  gPrefs.putUChar("rs", S.rangeStep);
  gPrefs.putString("place", S.place);
  gPrefs.putString("cc", S.country);
  gPrefs.putString("tz", S.tzPosix);
  gPrefs.putString("tzn", S.tzName);
  gPrefs.putBool("cauto", S.countryAuto);
  gPrefs.putBool("tauto", S.tzAuto);
  gPrefs.putBool("uauto", S.unitsAuto);
  gPrefs.putUChar("ud", S.dist);
  gPrefs.putUChar("ua", S.alt);
  gPrefs.putUChar("us", S.spd);
  gPrefs.putBool("c24", S.clock24);
  gPrefs.putUChar("disp3", S.displayMode);
  gPrefs.putUChar("theme2", S.theme);
  for (int i = 0; i < palette::ROLES; i++) {
    char key[6];
    snprintf(key, sizeof(key), "pal%d", i);
    gPrefs.putUChar(key, S.pal[i]);
  }
  gPrefs.putUChar("bri", S.brightness);
  gPrefs.putUChar("nmode", S.nightMode);
  gPrefs.putUChar("nbri", S.nightBright);
  gPrefs.putBool("nlast", S.nightLast);
  gPrefs.putUChar("trl", S.trail);
  gPrefs.putBool("diag", S.showDiag);
  gPrefs.putBool("map", S.mapOverlay);
  gPrefs.putBool("sdl", S.sdLog);
  gPrefs.putBool("fae", S.faEnabled);
  gPrefs.putString("fak", S.faKey);
  gPrefs.putString("fem", S.faEmail);
  gPrefs.putBool("fap", S.faPhoto);
  gPrefs.putBool("fat", S.faTrack);
  gPrefs.putUChar("lgs", S.logSecs);
  gPrefs.putString("pin", S.webPin);
  gPrefs.putString("appw", S.apPass);
  Serial.println("[settings] saved");
}

}  // namespace settings
