#include "night.h"
#include "settings.h"
#include "display.h"
#include "radar.h"
#include <math.h>
#include "solar.h"

namespace night {
namespace {
bool gSched = false;        // night by mode / sun
bool gEmerg = false;        // an emergency squawk overrides it
bool gOn = false;           // night colours in force = gSched && !gEmerg
bool gEmergSeen = false;
uint32_t gEmergUntil = 0, gLastEval = 0;
constexpr uint8_t kNightLevels[] = {2, 4, 7, 10, 15, 20, 30, 40, 50, 60, 80, 100};
constexpr int kNightN = sizeof(kNightLevels) / sizeof(kNightLevels[0]);

bool want() {
  const settings::Settings &S = settings::get();
  if (S.nightMode == 2) return true;
  if (S.nightMode == 0) return false;
  const time_t t = time(nullptr);
  if (t < 1700000000) return gSched || S.nightLast;   // clock not set yet (just after a restart): the state we had
  return sunElevationDeg(S.lat, S.lon, t) < -1.0;   // a little after sunset / before sunrise
}

void update() {   // effective state from schedule + emergency; apply what changed
  const bool em = gEmergSeen && (int32_t)(gEmergUntil - millis()) > 0;
  const bool on = gSched && !em;
  if (on == gOn && em == gEmerg) return;
  const bool colours = on != gOn;
  if (em != gEmerg) Serial.printf("[night] emergency override %s\n", em ? "on: normal colours, full brightness" : "over");
  gEmerg = em; gOn = on;
  if (colours) Serial.printf("[night] %s\n", gOn ? "on" : "off");
  applyBrightness();
  if (colours) radar::applyPalette();
}
}  // namespace

const char *modeName(int mode) { return mode == 1 ? "Auto" : mode == 2 ? "On" : "Off"; }
bool active() { return gOn; }
bool scheduled() { return gSched; }
bool emergency() { return gEmerg; }
int pal(int role) { return gOn ? RED_SWATCH : settings::get().pal[role]; }
int brightnessPct() { return gEmerg ? 100 : gSched ? settings::get().nightBright : settings::get().brightness; }
int settingPct() { return gSched ? settings::get().nightBright : settings::get().brightness; }
void applyBrightness() { display::setBrightness((uint8_t)lroundf(brightnessPct() * 2.55f)); }

void noteEmergency(bool present) {
  if (present) { gEmergSeen = true; gEmergUntil = millis() + 120000; }
  update();
}

void evaluate() {
  const bool w = want();
  if (w != gSched) {
    gSched = w;
    settings::get().nightLast = w;
    settings::changed();
  }
  update();
}

void tick() {
  update();
  if (millis() - gLastEval < 20000) return;
  gLastEval = millis();
  evaluate();
}

double sunElevationDeg(double lat, double lon, time_t utc) { return solar::elevationDeg(lat, lon, utc); }

int stepBrightness(int dirSign) {
  const settings::Settings &S = settings::get();
  if (!gSched) return constrain(S.brightness + dirSign * 10, 10, 100);
  int i = 0;
  while (i < kNightN - 1 && kNightLevels[i] < S.nightBright) i++;   // first level not below the current one
  i = constrain(i + dirSign, 0, kNightN - 1);
  return kNightLevels[i];
}

}  // namespace night
