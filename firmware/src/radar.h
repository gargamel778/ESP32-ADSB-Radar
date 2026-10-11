// Radar renderer. Port of mircemk's CrowPanel sketch (GPL-3.0-or-later) to the VIEWE 7" panel.
// Layout (800x480): 480x480 radar on the left, 310x470 info panel on the right.
#pragma once
#include <Arduino.h>
#include "adsb.h"
#include "display.h"   // pulls in LovyanGFX (LGFX_USE_V1)

namespace radar {

constexpr int TILE_W = 128, TILE_H = 72;   // radar scratch tile (18 KB of internal RAM); the settings screen paints through it too
enum Button : uint8_t { BTN_NONE = 0, BTN_ZOOM_IN, BTN_ZOOM_OUT, BTN_PAUSE, BTN_SETTINGS };

struct Stats {
  float fps;
  uint32_t frameMs;    // work time of the last frame (excludes the frame-rate cap delay)
  uint32_t usPaint;    // smoothed time spent painting tiles after vsync, microseconds
  uint32_t usFlush;    // smoothed time spent waiting for vsync (idle slack), microseconds
  int tiles;           // tiles painted last frame
  int aircraft;        // plotted aircraft
  bool dataOk;
};

void begin(lgfx::LovyanGFX *display);
void setData(const adsb::Snapshot &s);          // new aircraft data from the network task
void setTestPattern(bool on);                   // debug: full-screen moving bar through the normal double-buffer path (tear check)
bool testPatternOn();
void setVerify(bool on);                        // debug: check the double-buffer invariant every 10th frame (serial 'v')
void setModeLabel(const char *label);           // shown in the HUD
void setTrailDeg(int deg);                      // sweep trail length, 5..60 degrees (live)
int trailDeg();
void setTheme(int idx);                         // font theme, live
int themeIndex();
const char *themeName();
String timeZoneLabel();                         // e.g. " (UTC+1)" or "" until the clock is set
void applyLocation();                           // settings changed: new centre / range -> redraw rings, forget old aircraft
void applyRange();                              // range step changed: rescale rings, drop aircraft now outside
void refreshUnits();                            // settings changed: units or clock format -> redraw labels and table
int infoHeaderHit(int x, int y);                // tap on the table header: 1 = distance, 2 = altitude, 3 = speed, 0 = none
int infoClockHit(int x, int y);                 // tap in the clock row: 1 = time label (12/24 h), 2 = digits (font theme), 0 = none
Button buttonAt(int x, int y);                  // on-screen button under a touch point (generous 54 px target), or BTN_NONE
void setButtonPressed(Button b);                // draw a button as held down (BTN_NONE = release)
void setPaused(bool p);                         // freeze / resume the sweep
bool paused();
bool selectAt(int x, int y);                    // tap on the radar: select the aircraft there / dismiss the card. True if the tap was used
bool selectRowAt(int x, int y);                 // tap on a row of the aircraft table: select that aircraft. True if the tap was used
void clearSelection();
bool hasSelection();
void debugEmergency(int code);                      // dev (serial 'E'): 0 off, 1/2/3 = the nearest aircraft squawks 7500/7600/7700
uint32_t dataAgeS();                            // seconds since the last good fetch (UINT32_MAX if none yet)
int emergencies(char *buf, size_t cap);         // number of aircraft squawking 7500/7600/7700; buf = "7700 CALLSIGN" of the nearest
void refreshHud();                              // status text settings changed: repaint it
void refreshMap();                              // map overlay switched on/off: rebuild the static layer
void applyPalette();                            // palette choices changed (settings): recolour everything
void invalidate();                              // something else used the screen: wipe it and repaint the radar from scratch
lgfx::LGFX_Sprite *scratchTile();               // TILE_W x TILE_H internal-RAM scratch sprite, shared with the settings screen
void frame(float dtSec);                        // advance sweep, paint what changed
void tearCounters(uint32_t &atRisk, uint32_t &total);   // tiles pushed while the scan was within/just ahead of their rows, since the last call
Stats stats();
void noteFrame();                                 // frame-rate counter: call once per displayed frame (radar or settings)

}  // namespace radar
