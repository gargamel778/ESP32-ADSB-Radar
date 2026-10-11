// Night mode: a dim, all-red display that keeps your night vision. Off, always on, or automatic (the sun is below the horizon at the radar's location).
// While it is on, the backlight follows the "night brightness" and every palette role is the red swatch.
// An emergency squawk (7500/7600/7700) overrides it: while one is on the radar, and for two minutes after, the normal colours and full brightness are in force.
#pragma once
#include <Arduino.h>
#include <time.h>

namespace night {

constexpr int RED_SWATCH = 7;                 // palette::kSwatches index of "Red"
const char *modeName(int mode);               // "Off", "Auto", "On"
bool active();                                // night colours are in force right now (false during an emergency)
bool scheduled();                             // the mode (and, for Auto, the sun) say it is night; this is what the night settings edit
bool emergency();                             // an emergency squawk currently overrides night mode
void noteEmergency(bool present);             // radar: aircraft squawking an emergency code are (not) on the screen
int pal(int role);                            // palette swatch in force for a role (red at night, the user's choice by day)
int brightnessPct();                          // backlight in force: 100 during an emergency, else night or day brightness
int settingPct();                             // the brightness setting the stepper edits (night brightness while it is night, else day)
void applyBrightness();                       // push brightnessPct() to the backlight
void evaluate();                              // decide now (mode or location changed); on a change applies brightness and colours
void tick();                                  // main loop: applies emergency hold expiry at once, re-reads the mode / sun every 20 s
double sunElevationDeg(double lat, double lon, time_t utc);   // degrees above the horizon (geometric, about 0.1 degree accurate)
int stepBrightness(int dirSign);              // next backlight level for the setting in force (day: steps of 10 %, night: finer steps near the bottom)

}  // namespace night
