#include "app.h"
#include "settings.h"
#include "radar.h"
#include "adsb.h"
#include "display.h"
#include "geo.h"
#include "sdlog.h"
#include "config.h"
#include "night.h"
#include "flightinfo.h"
#include "phase.h"
#include <math.h>

namespace app {
namespace {

void rangeChanged() {
  radar::applyRange();
  adsb::fetchNow();
  settings::changed();
  Serial.printf("[range] %.0f km (api %d NM)\n", settings::rangeKm(), settings::apiRadiusNm());
}

// A manual unit choice switches the units from "follow the country profile" to "manual".
void manualUnits() {
  settings::get().unitsAuto = false;
  settings::changed();
  radar::refreshUnits();
  const settings::Settings &S = settings::get();
  Serial.printf("[units] %s / %s / %s clock24=%d\n", units::name((units::Dist)S.dist), units::name((units::Alt)S.alt), units::name((units::Spd)S.spd), (int)S.clock24);
}

}  // namespace

void setRangeStep(int step) {
  step = constrain(step, 0, units::RANGE_STEPS - 1);
  if (step == settings::get().rangeStep) return;
  settings::setRangeStep(step);
  rangeChanged();
}

void zoom(int dir) { setRangeStep(settings::get().rangeStep + dir); }

void setDist(int u) {
  settings::Settings &S = settings::get();
  u = ((u % units::DIST_N) + units::DIST_N) % units::DIST_N;
  if (u == S.dist) return;
  const float km = settings::rangeKm();
  S.dist = (uint8_t)u;
  settings::setRangeStep(units::nearestRangeStep((units::Dist)u, km));
  adsb::fetchNow();
  radar::applyRange();   // the nearest round range in the new unit may differ a little from the old one
  manualUnits();
}

void setAlt(int u) { settings::get().alt = (uint8_t)(((u % units::ALT_N) + units::ALT_N) % units::ALT_N); manualUnits(); }
void setSpd(int u) { settings::get().spd = (uint8_t)(((u % units::SPD_N) + units::SPD_N) % units::SPD_N); manualUnits(); }
void setClock24(bool on) { settings::get().clock24 = on; manualUnits(); }

void setUnitsAuto(bool on) {
  settings::Settings &S = settings::get();
  if (on == S.unitsAuto) return;
  if (!on) { S.unitsAuto = false; settings::changed(); return; }
  const float km = settings::rangeKm();
  S.unitsAuto = true;
  settings::applyCountryProfile();
  settings::setRangeStep(units::nearestRangeStep((units::Dist)S.dist, km));
  adsb::fetchNow();
  radar::applyRange();
  settings::changed();
  radar::refreshUnits();
  Serial.printf("[units] follow country %s -> %s / %s / %s clock24=%d\n", S.country, units::name((units::Dist)S.dist), units::name((units::Alt)S.alt), units::name((units::Spd)S.spd), (int)S.clock24);
}

void setCountry(const char *cc) {
  settings::Settings &S = settings::get();
  S.country[0] = cc[0]; S.country[1] = cc[1]; S.country[2] = 0;
  const bool was = S.unitsAuto;
  S.unitsAuto = false;   // force the transition below to re-apply the profile
  (void)was;
  setUnitsAuto(true);
  Serial.printf("[country] %s%s\n", S.country, settings::profileKnown(S.country) ? "" : " (no profile: aviation defaults)");
}

void setTheme(int idx) {
  radar::setTheme(idx);
  settings::get().theme = (uint8_t)radar::themeIndex();
  settings::changed();
  Serial.printf("[theme] %s\n", radar::themeName());
}

void setPalette(int role, int idx) {
  settings::Settings &S = settings::get();
  role = constrain(role, 0, palette::ROLES - 1);
  idx = ((idx % palette::COUNT) + palette::COUNT) % palette::COUNT;
  if (S.pal[role] == idx) return;
  S.pal[role] = (uint8_t)idx;
  radar::applyPalette();
  settings::changed();
  Serial.printf("[palette] role %d = %s\n", role, palette::kSwatches[idx].name);
}

void setDayBrightness(int percent) {
  settings::get().brightness = (uint8_t)constrain(percent, 10, 100);
  night::applyBrightness();
  settings::changed();
}

void setNightBrightness(int percent) {
  settings::get().nightBright = (uint8_t)constrain(percent, 2, 100);
  night::applyBrightness();
  settings::changed();
}

void setBrightness(int percent) { if (night::scheduled()) setNightBrightness(percent); else setDayBrightness(percent); }

void stepBrightness(int dir) { setBrightness(night::stepBrightness(dir)); }

void setNightMode(int mode) {
  settings::get().nightMode = (uint8_t)constrain(mode, 0, 2);
  night::evaluate();
  night::applyBrightness();
  settings::changed();
  Serial.printf("[night] mode %s\n", night::modeName(settings::get().nightMode));
}

void setTrail(int deg) {
  settings::get().trail = (uint8_t)deg;
  radar::setTrailDeg(deg);
  settings::changed();
}

void setLocation(double lat, double lon, const char *place, const char *country, const char *tzPosix, const char *tzName) {
  settings::Settings &S = settings::get();
  const float km = settings::rangeKm();
  S.lat = constrain(lat, -90.0, 90.0);
  S.lon = constrain(lon, -180.0, 180.0);
  if (place && *place) strlcpy(S.place, place, sizeof(S.place));
  if (country && strlen(country) == 2) { S.country[0] = toupper((unsigned char)country[0]); S.country[1] = toupper((unsigned char)country[1]); S.country[2] = 0; }
  if (tzPosix && *tzPosix) {
    strlcpy(S.tzPosix, tzPosix, sizeof(S.tzPosix));
    if (tzName) strlcpy(S.tzName, tzName, sizeof(S.tzName));
    else { const int z = geo::zoneByPosix(S.tzPosix); strlcpy(S.tzName, z >= 0 ? geo::zoneName(z) : "", sizeof(S.tzName)); }
    setenv("TZ", S.tzPosix, 1);
    tzset();
  }
  if (S.unitsAuto) { settings::applyCountryProfile(); settings::setRangeStep(units::nearestRangeStep((units::Dist)S.dist, km)); }
  radar::applyLocation();
  adsb::fetchNow();
  settings::changed();
  Serial.printf("[location] %.4f,%.4f '%s' cc=%s tz=%s\n", S.lat, S.lon, S.place, S.country, S.tzPosix);
}

const char *setLocationAuto(double lat, double lon, const char *place, bool force) {
  settings::Settings &S = settings::get();
  static geo::Place c;
  float dist = 0;
  const bool have = geo::nearest(lat, lon, c, &dist);
  const char *name = (place && *place) ? place : have ? c.name : "Home";
  // a small move keeps what the radar already knows about this area
  const double dLat = lat - S.lat, dLon = (lon - S.lon) * cos(lat * M_PI / 180.0);
  const bool sameArea = !force && S.country[0] && S.tzPosix[0] && sqrt(dLat * dLat + dLon * dLon) * 111.2 < 40.0;
  setLocation(lat, lon, name, have && S.countryAuto && !sameArea ? c.cc : nullptr, have && S.tzAuto && !sameArea ? geo::zonePosix(c.zone) : nullptr,
              have && S.tzAuto && !sameArea ? geo::zoneName(c.zone) : nullptr);
  return have ? c.name : nullptr;
}

void setLocationCity(const geo::Place &p) {
  const settings::Settings &S = settings::get();
  setLocation(p.lat, p.lon, p.name, S.countryAuto ? p.cc : nullptr, S.tzAuto ? geo::zonePosix(p.zone) : nullptr, S.tzAuto ? geo::zoneName(p.zone) : nullptr);
}

void resetHome() {
  const int z = geo::zoneByPosix(TIMEZONE_POSIX);
  settings::Settings &S = settings::get();
  S.countryAuto = S.tzAuto = true;   // as shipped
  setLocation(HOME_LAT, HOME_LON, LOCATION_NAME, DEFAULT_COUNTRY, TIMEZONE_POSIX, z >= 0 ? geo::zoneName(z) : nullptr);
}

void setCountryAuto(bool on) {
  settings::Settings &S = settings::get();
  S.countryAuto = on;
  settings::changed();
  if (on) setLocationAuto(S.lat, S.lon, S.place, true);   // recompute from the location (keeps the name)
}

void setCountryManual(const char *cc) {
  settings::Settings &S = settings::get();
  S.countryAuto = false;
  const float km = settings::rangeKm();
  S.country[0] = toupper((unsigned char)cc[0]); S.country[1] = toupper((unsigned char)cc[1]); S.country[2] = 0;
  if (S.unitsAuto) { settings::applyCountryProfile(); settings::setRangeStep(units::nearestRangeStep((units::Dist)S.dist, km)); adsb::fetchNow(); radar::applyRange(); }
  settings::changed();
  radar::refreshUnits();
  Serial.printf("[country] %s chosen by hand\n", S.country);
}

void setZoneAuto(bool on) {
  settings::Settings &S = settings::get();
  S.tzAuto = on;
  settings::changed();
  if (on) setLocationAuto(S.lat, S.lon, S.place, true);
}

void setZoneManual(int zone) {
  settings::Settings &S = settings::get();
  const int n = geo::zoneCount();
  if (n <= 0) return;
  zone = ((zone % n) + n) % n;
  S.tzAuto = false;
  strlcpy(S.tzPosix, geo::zonePosix(zone), sizeof(S.tzPosix));
  strlcpy(S.tzName, geo::zoneName(zone), sizeof(S.tzName));
  setenv("TZ", S.tzPosix, 1);
  tzset();
  settings::changed();
  Serial.printf("[tz] %s (%s) chosen by hand\n", S.tzName, S.tzPosix);
}

void setSdLog(bool on) {
  settings::get().sdLog = on;
  settings::changed();
  sdlog::sys("logging %s by the user", on ? "switched on" : "switched off");
  Serial.printf("[sd] logging %s\n", on ? "on" : "off");
}

void setLogSecs(int secs) {
  settings::get().logSecs = (uint8_t)(secs <= 10 ? 10 : secs <= 30 ? 30 : 60);
  settings::changed();
}

void sdEject() { sdlog::eject(); }
void sdMount() { sdlog::mount(); }

static void flightChanged() {
  flightinfo::flush();   // what was cached was fetched under the old settings
  radar::refreshHud();
  settings::changed();
}
void setFlightEnabled(bool on) { settings::get().faEnabled = on; flightChanged(); Serial.printf("[flight] details %s\n", on ? "on" : "off"); }
void setFlightKey(const char *key) {
  char k[40]; int n = 0;
  for (const char *p = key ? key : ""; *p && n < (int)sizeof(k) - 1; p++) if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') k[n++] = *p;   // a pasted key often brings spaces or quotes
  k[n] = 0;
  strlcpy(settings::get().faKey, k, sizeof(settings::get().faKey));
  flightChanged();
  Serial.printf("[flight] key %s\n", k[0] ? "saved" : "cleared");
}
void setFlightEmail(const char *email) {
  char e[64]; int n = 0;
  for (const char *p = email ? email : ""; *p && n < (int)sizeof(e) - 1; p++) if (!isspace((unsigned char)*p) && *p != '"' && *p != '<' && *p != '>' && *p != '(' && *p != ')') e[n++] = *p;
  e[n] = 0;
  strlcpy(settings::get().faEmail, e, sizeof(settings::get().faEmail));
  flightChanged();
}
void setFlightPhoto(bool on) { settings::get().faPhoto = on; flightChanged(); }
void setFlightTrack(bool on) { settings::get().faTrack = on; flightChanged(); }
void testFlightKey() { flightinfo::requestTest(); }

void setMapOverlay(bool on) {
  settings::get().mapOverlay = on;
  radar::refreshMap();
  settings::changed();
}

void setShowDiag(bool on) {
  settings::get().showDiag = on;
  radar::refreshHud();
  settings::changed();
}

void setDisplayMode(int mode) {
  settings::get().displayMode = (uint8_t)constrain(mode, 0, display::MODE_COUNT - 1);
  settings::changed();
}

void restart() {
  Serial.println("[app] restarting");
  settings::saveNow();
  sdlog::ejectAndWait(3000);   // flushes the log buffers, so the reason for this restart reaches the card
  Serial.flush();
  delay(100);
  phase::gPlanned = phase::kPlanned;
  ESP.restart();
}

}  // namespace app
