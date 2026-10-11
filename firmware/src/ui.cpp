#include "ui.h"
#include "app.h"
#include "radar.h"
#include "settings.h"
#include "palette.h"
#include "units.h"
#include "gfx.h"
#include "ui_internal.h"
#include "wifimgr.h"
#include "webui.h"
#include "geo.h"
#include "sdlog.h"
#include "ota.h"
#include "night.h"
#include "flightinfo.h"
#include "config.h"
#include <WiFi.h>

using namespace gfx;

namespace ui {
namespace in {
Cols C;
const lgfx::IFont *const F24 = &lgfx::fonts::DejaVu24, *const F18 = &lgfx::fonts::DejaVu18, *const F12 = &lgfx::fonts::DejaVu12;
}  // namespace in

namespace {
using namespace in;

constexpr int W = 800, H = 480;
lgfx::LovyanGFX *gDisp = nullptr;
bool gActive = false;

// ---------------------------------------------------------------- settings screen layout
constexpr int TAB_X = 8, TAB_W = 168, TAB_Y0 = 72, TAB_PITCH = 48, TAB_H = 44;
constexpr int ROW_X = 204, ROW_Y0 = 72, ROW_H = 54, ROW_R = 792;   // 54: seven rows (the Look page) plus the hint line fit under the header
constexpr int ARR_W = 56, ARR_H = 48, ARR_L = 520, ARR_R = 728;   // stepper arrow buttons; the value sits between them
constexpr int TOG_X = 704, TOG_W = 80, TOG_H = 40;
constexpr int ACT_X = 584, ACT_W = 200;
constexpr int PAGES = 8, PAGE_LOC = 3, PAGE_WIFI = 4, PAGE_FLIGHT = 5, PAGE_LOG = 6, PAGE_SYSTEM = 7;
const char *const kPageNames[PAGES] = {"Radar", "Units", "Look", "Location", "Wi-Fi", "Flights Data", "Log", "System"};
const int kRowCount[PAGES] = {6, 5, 7, 6, 6, 7, 6, 5};

enum Kind : uint8_t { K_STEP, K_TOGGLE, K_ACTION, K_INFO, K_OFF };
const Kind kKinds[PAGES][7] = {
    {K_STEP, K_STEP, K_TOGGLE, K_TOGGLE, K_TOGGLE, K_STEP},
    {K_TOGGLE, K_STEP, K_STEP, K_STEP, K_STEP},
    {K_STEP, K_STEP, K_STEP, K_STEP, K_STEP, K_STEP, K_STEP},
    {K_INFO, K_ACTION, K_ACTION, K_ACTION, K_ACTION, K_ACTION},
    {K_INFO, K_ACTION, K_ACTION, K_INFO, K_INFO, K_INFO},
    {K_TOGGLE, K_ACTION, K_ACTION, K_TOGGLE, K_TOGGLE, K_ACTION, K_INFO},
    {K_INFO, K_TOGGLE, K_STEP, K_INFO, K_ACTION, K_ACTION},
    {K_STEP, K_ACTION, K_ACTION, K_INFO, K_ACTION},
};
const char *const kHints[PAGES] = {
    "Pinch the radar to zoom. Night mode: dim and red; Auto follows the sun.",
    "Units follow the country until you change one by hand.",
    "Colors apply at once. Lines & Buttons: frames, separators and buttons.",
    "Country and time zone follow the location unless you choose them here.",
    "The old network is kept if a new one does not work.",
    "Tap an aircraft: take-off, ETA, track and a photo.",
    "One CSV per day (UTC) + a system log in /adsb. Eject before pulling the card.",
    "* Display mode takes effect after a restart.",
};
const uint8_t kRoleOfRow[5] = {palette::RADAR, palette::TITLES, palette::CALLSIGN, palette::VALUE, palette::LINES};   // Look page rows 1..5

int gPage = 0, gReturnPage = PAGE_WIFI;

// Hit targets
enum HitType : uint8_t { H_NONE, H_TAB, H_DONE, H_ARROW, H_TOGGLE, H_ACTION };
struct Hit { HitType type = H_NONE; int8_t idx = 0; int8_t dir = 0;
  bool operator==(const Hit &o) const { return type == o.type && idx == o.idx && dir == o.dir; } };

Rect tabRect(int i) { return {TAB_X, (int16_t)(TAB_Y0 + i * TAB_PITCH), (int16_t)(TAB_X + TAB_W), (int16_t)(TAB_Y0 + i * TAB_PITCH + TAB_H)}; }
Rect doneRect() { return {660, 8, 792, 54}; }
int16_t rowY(int r) { return (int16_t)(ROW_Y0 + r * ROW_H); }
Rect arrowRect(int r, int dir) { int x = dir < 0 ? ARR_L : ARR_R; const int o = (ROW_H - ARR_H) / 2; return {(int16_t)x, (int16_t)(rowY(r) + o), (int16_t)(x + ARR_W), (int16_t)(rowY(r) + o + ARR_H)}; }
Rect toggleZone(int r) { return {480, rowY(r), ROW_R, (int16_t)(rowY(r) + ROW_H)}; }
Rect actionRect(int r) { const int o = (ROW_H - ARR_H) / 2; return {ACT_X, (int16_t)(rowY(r) + o), (int16_t)(ACT_X + ACT_W), (int16_t)(rowY(r) + o + ARR_H)}; }
Rect grow(Rect r, int m) { return {(int16_t)(r.x0 - m), (int16_t)(r.y0 - m), (int16_t)(r.x1 + m), (int16_t)(r.y1 + m)}; }

Rect hitRectOf(const Hit &h) {
  switch (h.type) {
    case H_TAB: return tabRect(h.idx);
    case H_DONE: return doneRect();
    case H_ARROW: return arrowRect(h.idx, h.dir);
    case H_TOGGLE: return toggleZone(h.idx);
    case H_ACTION: return actionRect(h.idx);
    default: return {0, 0, 0, 0};
  }
}

Hit hitTest(int x, int y) {
  Hit h;
  if (containsR(grow(doneRect(), 6), x, y)) { h.type = H_DONE; return h; }
  for (int i = 0; i < PAGES; i++) if (containsR(grow(tabRect(i), 4), x, y)) { h.type = H_TAB; h.idx = i; return h; }
  for (int r = 0; r < kRowCount[gPage]; r++) {
    switch (kKinds[gPage][r]) {
      case K_STEP:
        for (int d = -1; d <= 1; d += 2) if (containsR(grow(arrowRect(r, d), 4), x, y)) { h.type = H_ARROW; h.idx = r; h.dir = d; return h; }
        break;
      case K_TOGGLE: if (containsR(toggleZone(r), x, y)) { h.type = H_TOGGLE; h.idx = r; return h; } break;
      case K_ACTION: if (containsR(grow(actionRect(r), 4), x, y)) { h.type = H_ACTION; h.idx = r; return h; } break;
      default: break;
    }
  }
  return h;
}

// ---------------------------------------------------------------- what each row shows
struct RowInfo {
  char label[40];
  char value[48];
  char sub[48];              // actions: the current setting, shown left of the button
  Kind kind;
  bool canDec, canInc;       // stepper arrows enabled
  bool on;                   // toggle state
  int swatch;                // >= 0: palette swatch shown beside the value
  const lgfx::IFont *font;   // value font
};

int trailIndex(int deg) { for (int i = 0; i < settings::TRAIL_N; i++) if (settings::kTrails[i] == deg) return i; return 1; }

RowInfo describe(int page, int row) {
  RowInfo r = {};
  r.kind = kKinds[page][row];
  r.canDec = r.canInc = true;
  r.swatch = -1;
  r.font = &lgfx::fonts::DejaVu18;
  const settings::Settings &S = settings::get();
  const auto du = (units::Dist)S.dist;
  if (page == 0) {
    if (row == 0) {
      strcpy(r.label, "Range");
      snprintf(r.value, sizeof(r.value), "%d %s", units::rangeSteps(du)[S.rangeStep], units::name(du));
      r.canDec = S.rangeStep > 0; r.canInc = S.rangeStep < units::RANGE_STEPS - 1;
    } else if (row == 1) {
      strcpy(r.label, "Sweep Trail");
      snprintf(r.value, sizeof(r.value), "%d deg", S.trail);
    } else if (row == 2) {
      strcpy(r.label, "Sweep");
      strcpy(r.value, radar::paused() ? "Paused" : "Running");
      r.on = !radar::paused();
    } else if (row == 3) {
      strcpy(r.label, "Diagnostics");
      strcpy(r.value, S.showDiag ? "Shown" : "Hidden");
      r.on = S.showDiag;
    } else if (row == 4) {
      strcpy(r.label, "Map Overlay");
      strcpy(r.value, S.mapOverlay ? "On" : "Off");
      r.on = S.mapOverlay;
    } else {
      strcpy(r.label, "Night Mode");
      if (S.nightMode == 1) snprintf(r.value, sizeof(r.value), "Auto (%s now)", night::scheduled() ? "on" : "off"); else strcpy(r.value, night::modeName(S.nightMode));
      r.canDec = S.nightMode > 0; r.canInc = S.nightMode < 2;
    }
  } else if (page == 1) {
    if (row == 0) {
      snprintf(r.label, sizeof(r.label), "Units From Country (%s)", S.country);
      strcpy(r.value, S.unitsAuto ? "On" : "Off");
      r.on = S.unitsAuto;
    } else if (row == 1) { strcpy(r.label, "Distance"); strcpy(r.value, units::name(du)); }
    else if (row == 2) { strcpy(r.label, "Altitude"); strcpy(r.value, units::name((units::Alt)S.alt)); }
    else if (row == 3) { strcpy(r.label, "Speed"); strcpy(r.value, units::name((units::Spd)S.spd)); }
    else { strcpy(r.label, "Clock"); strcpy(r.value, S.clock24 ? "24 Hour" : "12 Hour"); }
  } else if (page == 2) {
    static const char *const names[] = {"Font", "Radar Color", "Titles Color", "Callsign Color", "Value Color", "Lines & Buttons", "Brightness"};
    strcpy(r.label, row == 6 && night::scheduled() ? "Night Brightness" : names[row]);
    if (row == 0) { strcpy(r.value, kThemes[radar::themeIndex()].name); r.font = kThemes[radar::themeIndex()].m; }   // shown in its own font
    else if (row <= 5) { r.swatch = S.pal[kRoleOfRow[row - 1]]; strcpy(r.value, palette::kSwatches[r.swatch].name); }
    else {
      const int b = night::settingPct();
      snprintf(r.value, sizeof(r.value), "%d %%", b);
      r.canDec = b > (night::scheduled() ? 2 : 10); r.canInc = b < 100;
    }
  } else if (page == PAGE_LOC) {
    if (row == 0) { strcpy(r.label, "Place"); snprintf(r.value, sizeof(r.value), "%s  %.4f, %.4f", S.place, S.lat, S.lon); r.font = &lgfx::fonts::DejaVu12; }
    else if (row == 1) { strcpy(r.label, "Search City"); strcpy(r.value, "Search"); }
    else if (row == 2) { strcpy(r.label, "Coordinates"); strcpy(r.value, "Enter"); snprintf(r.sub, sizeof(r.sub), "%.4f, %.4f", S.lat, S.lon); }
    else if (row == 3) { strcpy(r.label, "Pick On Map"); strcpy(r.value, "Open"); }
    else if (row == 4) { strcpy(r.label, "Country"); strcpy(r.value, "Change"); snprintf(r.sub, sizeof(r.sub), "%s%s", S.countryAuto ? "Auto: " : "", geo::countryName(geo::countryByCode(S.country))); }
    else { strcpy(r.label, "Time Zone"); strcpy(r.value, "Change"); snprintf(r.sub, sizeof(r.sub), "%s%s", S.tzAuto ? "Auto: " : "", S.tzName[0] ? S.tzName : S.tzPosix); }
  } else if (page == PAGE_LOG) {
    const sdlog::Status st = sdlog::status();
    if (row == 0) {
      strcpy(r.label, "SD Card");
      r.font = &lgfx::fonts::DejaVu12;
      if (st.state == sdlog::S_EJECTED) strcpy(r.value, "Ejected: safe to remove");
      else if (st.totalBytes) snprintf(r.value, sizeof(r.value), "%u GB, %.1f GB free%s", (unsigned)(st.totalBytes >> 30), st.freeBytes / 1073741824.0, st.state == sdlog::S_ERROR ? " (write problem)" : "");
      else strcpy(r.value, "No card");
    } else if (row == 1) { strcpy(r.label, "Logging"); strcpy(r.value, S.sdLog ? "On" : "Off"); r.on = S.sdLog; }
    else if (row == 2) { strcpy(r.label, "Aircraft Every"); snprintf(r.value, sizeof(r.value), "%d s", S.logSecs); }
    else if (row == 3) {
      strcpy(r.label, "Today");
      r.font = &lgfx::fonts::DejaVu12;
      if (st.rowsToday) snprintf(r.value, sizeof(r.value), "%u rows, %.2f MB%s", (unsigned)st.rowsToday, st.bytesToday / 1048576.0, st.dropped ? " (some lost)" : ""); else strcpy(r.value, "-");
    }
    else if (row == 4) { strcpy(r.label, st.state == sdlog::S_EJECTED ? "Mount Card" : "Eject Card"); strcpy(r.value, st.state == sdlog::S_EJECTED ? "Mount" : "Eject"); }
    else { strcpy(r.label, "View Logs"); strcpy(r.value, "Open"); }
  } else if (page == PAGE_FLIGHT) {
    const flightinfo::Status fs = flightinfo::status();
    if (row == 0) { strcpy(r.label, "FlightAware"); strcpy(r.value, S.faEnabled ? "On" : "Off"); r.on = S.faEnabled; }
    else if (row == 1) { strcpy(r.label, "API Key"); strcpy(r.value, "Enter"); strcpy(r.sub, S.faKey[0] ? "Saved" : "Not set"); }
    else if (row == 2) { strcpy(r.label, "Contact Email"); strcpy(r.value, "Enter"); strlcpy(r.sub, S.faEmail[0] ? S.faEmail : "Not set", sizeof(r.sub)); }
    else if (row == 3) { strcpy(r.label, "Plane Photo"); strcpy(r.value, S.faPhoto ? "On" : "Off"); r.on = S.faPhoto; }
    else if (row == 4) { strcpy(r.label, "Flight Track"); strcpy(r.value, S.faTrack ? "On" : "Off"); r.on = S.faTrack; }
    else if (row == 5) { strcpy(r.label, "Test Key"); strcpy(r.value, "Test"); strlcpy(r.sub, fs.test, sizeof(r.sub)); }
    else { strcpy(r.label, "Last Lookup"); strlcpy(r.value, fs.text, sizeof(r.value)); r.font = &lgfx::fonts::DejaVu12; }
  } else if (page == PAGE_WIFI) {
    if (row == 0) {
      strcpy(r.label, "Network");
      if (wifimgr::connected()) snprintf(r.value, sizeof(r.value), "%s  %d dBm", wifimgr::ssid().c_str(), wifimgr::rssi()); else strcpy(r.value, S.wifiSsid[0] ? "Not connected" : "None saved");
      r.font = &lgfx::fonts::DejaVu12;
    } else if (row == 1) { strcpy(r.label, "Change Network"); strcpy(r.value, "Choose"); }
    else if (row == 2) { strcpy(r.label, "Setup From Phone"); strcpy(r.value, "Start"); }
    else if (row == 3) { strcpy(r.label, "Web Address"); strcpy(r.value, wifimgr::connected() ? "radar.local" : "-"); }
    else if (row == 4) { strcpy(r.label, "Web PIN"); strcpy(r.value, S.webPin); }
    else { strcpy(r.label, "IP Address"); strncpy(r.value, wifimgr::ip().c_str(), sizeof(r.value) - 1); }
  } else {
    if (row == 0) {
      strcpy(r.label, "Display Mode");
      snprintf(r.value, sizeof(r.value), "%s%s", display::modeName(S.displayMode), S.displayMode != display::currentMode() ? " *" : "");
    } else if (row == 1) { strcpy(r.label, "Restart Now"); strcpy(r.value, "Restart"); }
    else if (row == 2) { strcpy(r.label, "Tear Test"); strcpy(r.value, "Start"); }
    else if (row == 3) { strcpy(r.label, "Firmware"); snprintf(r.value, sizeof(r.value), "%s  (%s)  GPL-3.0+", FW_VERSION, ota::slot()); r.font = &lgfx::fonts::DejaVu12; }
    else { strcpy(r.label, "Credits"); strcpy(r.value, "Show"); }
  }
  return r;
}

void stepRow(int page, int row, int dir) {
  const settings::Settings &S = settings::get();
  if (page == 0) {
    if (row == 0) app::setRangeStep(S.rangeStep + dir);
    else if (row == 1) app::setTrail(settings::kTrails[(trailIndex(S.trail) + dir + settings::TRAIL_N) % settings::TRAIL_N]);
    else if (row == 5) app::setNightMode(constrain((int)S.nightMode + dir, 0, 2));
  } else if (page == 1) {
    if (row == 1) app::setDist(S.dist + dir);
    else if (row == 2) app::setAlt(S.alt + dir);
    else if (row == 3) app::setSpd(S.spd + dir);
    else if (row == 4) app::setClock24(!S.clock24);
  } else if (page == 2) {
    if (row == 0) app::setTheme(radar::themeIndex() + dir);
    else if (row <= 5) app::setPalette(kRoleOfRow[row - 1], S.pal[kRoleOfRow[row - 1]] + dir);
    else app::stepBrightness(dir);
  } else if (page == PAGE_LOG && row == 2) {
    app::setLogSecs(settings::kLogSecs[(((S.logSecs == 10 ? 0 : S.logSecs == 30 ? 1 : 2) + dir) % settings::LOGSECS_N + settings::LOGSECS_N) % settings::LOGSECS_N]);
  } else if (page == PAGE_SYSTEM && row == 0) {
    app::setDisplayMode(((int)S.displayMode + dir + display::MODE_UI_COUNT) % display::MODE_UI_COUNT);
  }
}

// ---------------------------------------------------------------- painting
void loadCols(lgfx::LGFX_Sprite *s) {
  const settings::Settings &S = settings::get();
  auto col = [&](int role, float f) { palette::Rgb v = palette::shade(night::pal(role), f); return s->color565(v.r, v.g, v.b); };
  C.bright = col(palette::LINES, palette::kBright); C.mid = col(palette::LINES, palette::kMid);   // frames, buttons, separators
  C.dim = col(palette::LINES, palette::kDim);       C.dark = col(palette::LINES, palette::kDark);
  C.title = col(palette::TITLES, palette::kBright); C.title2 = col(palette::TITLES, palette::kMid);
  C.val = col(palette::VALUE, palette::kBright);    C.val2 = col(palette::VALUE, palette::kMid);
}

Hit gPressed;
Screen *gSub = nullptr;          // Wi-Fi sub-screen shown instead of the settings tabs, or null
bool subDown = false, subSwallow = false;   // finger down on a sub-screen / ignore the rest of this touch (the screen changed under it)
int subX = 0, subY = 0;

void drawArrow(Ctx &g, const Rect &r, int dir, bool enabled, bool down) {
  const int cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
  g.frrect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, 8, down ? C.dark : TFT_BLACK);
  g.rrect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, 8, enabled ? (down ? C.bright : C.mid) : C.dark);
  const uint16_t c = enabled ? C.title : C.dark;
  if (dir < 0) g.ftri(cx + 6, cy - 10, cx + 6, cy + 10, cx - 8, cy, c);
  else g.ftri(cx - 6, cy - 10, cx - 6, cy + 10, cx + 8, cy, c);
}

void drawRow(Ctx &g, const Rect &t, int page, int r) {
  const Rect rr = {ROW_X - 8, rowY(r), ROW_R, (int16_t)(rowY(r) + ROW_H)};
  if (!hitR(t, rr)) return;
  const RowInfo ri = describe(page, r);
  const int y = rowY(r), cy = y + ROW_H / 2;
  const bool off = ri.kind == K_OFF;
  g.hline(ROW_X, y + ROW_H - 1, ROW_R - ROW_X, C.dark);
  g.textFont(F18, ri.label, ROW_X, cy, textdatum_t::middle_left, off ? C.dim : C.val);
  switch (ri.kind) {
    case K_STEP: {
      const bool dl = gPressed.type == H_ARROW && gPressed.idx == r && gPressed.dir < 0;
      const bool dr = gPressed.type == H_ARROW && gPressed.idx == r && gPressed.dir > 0;
      drawArrow(g, arrowRect(r, -1), -1, ri.canDec, dl);
      drawArrow(g, arrowRect(r, +1), +1, ri.canInc, dr);
      const int vx0 = ARR_L + ARR_W, vx1 = ARR_R, mid = (vx0 + vx1) / 2;
      if (ri.swatch >= 0) {
        const palette::Swatch &sw = palette::kSwatches[ri.swatch];
        const uint16_t sc = g.s->color565(sw.r, sw.g, sw.b);
        g.frrect(vx0 + 8, cy - 11, 22, 22, 4, sc);
        g.rrect(vx0 + 8, cy - 11, 22, 22, 4, C.mid);
        g.textFont(ri.font, ri.value, vx0 + 36 + (vx1 - vx0 - 36) / 2, cy, textdatum_t::middle_center, C.title);
      } else {
        g.textFont(ri.font, ri.value, mid, cy, textdatum_t::middle_center, C.title);
      }
      break;
    }
    case K_TOGGLE: {
      const bool down = gPressed.type == H_TOGGLE && gPressed.idx == r;
      const int px = TOG_X, py = cy - TOG_H / 2;
      g.frrect(px, py, TOG_W, TOG_H, TOG_H / 2, ri.on || down ? C.dark : TFT_BLACK);
      g.rrect(px, py, TOG_W, TOG_H, TOG_H / 2, ri.on ? C.bright : C.mid);
      g.fcircle(ri.on ? px + TOG_W - TOG_H / 2 : px + TOG_H / 2, cy, 13, ri.on ? C.bright : C.dim);
      g.textFont(F18, ri.value, px - 14, cy, textdatum_t::middle_right, C.title);
      break;
    }
    case K_ACTION: {
      const bool down = gPressed.type == H_ACTION && gPressed.idx == r;
      const Rect b = actionRect(r);
      g.frrect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0, 8, down ? C.dark : TFT_BLACK);
      g.rrect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0, 8, down ? C.bright : C.mid);
      g.textFont(F18, ri.value, (b.x0 + b.x1) / 2, cy, textdatum_t::middle_center, C.title);
      if (ri.sub[0]) g.textFont(F12, ri.sub, b.x0 - 12, cy, textdatum_t::middle_right, C.val2);
      break;
    }
    case K_INFO:
      g.textFont(ri.font, ri.value, ROW_R - 8, cy, textdatum_t::middle_right, C.val2);
      break;
    case K_OFF:
      g.textFont(F18, ri.value, ROW_R - 8, cy, textdatum_t::middle_right, C.dim);
      break;
  }
}

void drawScreen(Ctx &g, const Rect &t) {
  if (gSub) { gSub->draw(g, t); return; }
  // header
  if (hitR(t, Rect{0, 0, W, 64})) {
    g.textFont(F24, "Settings", 24, 32, textdatum_t::middle_left, C.title);
    const Rect d = doneRect();
    const bool down = gPressed.type == H_DONE;
    g.frrect(d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0, 8, down ? C.dark : TFT_BLACK);
    g.rrect(d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0, 8, down ? C.bright : C.mid);
    g.textFont(F18, "Done", (d.x0 + d.x1) / 2, (d.y0 + d.y1) / 2, textdatum_t::middle_center, C.title);
    g.hline(0, 62, W, C.mid);
  }
  // tabs
  for (int i = 0; i < PAGES; i++) {
    const Rect tr = tabRect(i);
    if (!hitR(t, grow(tr, 1))) continue;
    const bool sel = i == gPage, down = gPressed.type == H_TAB && gPressed.idx == i;
    g.frrect(tr.x0, tr.y0, tr.x1 - tr.x0, tr.y1 - tr.y0, 8, sel || down ? C.dark : TFT_BLACK);
    if (sel) g.frect(tr.x0, tr.y0 + 6, 6, TAB_H - 12, C.bright);
    g.textFont(F18, kPageNames[i], tr.x0 + 28, (tr.y0 + tr.y1) / 2, textdatum_t::middle_left, sel ? C.title : C.title2, sel || down ? C.dark : TFT_BLACK);
  }
  if (hitR(t, Rect{188, 64, 192, H})) g.vline(190, 70, 400, C.dark);
  for (int r = 0; r < kRowCount[gPage]; r++) drawRow(g, t, gPage, r);
  if (hitR(t, Rect{ROW_X, 452, ROW_R, H})) g.textFont(F12, kHints[gPage], ROW_X, 462, textdatum_t::top_left, C.mid);
}

// A pending repaint: every rectangle is painted twice, in two consecutive frames, so both framebuffers end up identical.
struct Pend { Rect r; uint8_t left; };
constexpr int MAX_PEND = 24;
Pend pend[MAX_PEND];
int nPend = 0;
void doMarkDirty(Rect r) {
  const uint8_t passes = display::doubleBuffered() ? 2 : 1;
  if (nPend < MAX_PEND) pend[nPend++] = {r, passes};
  else { pend[0] = {{0, 0, W, H}, passes}; nPend = 1; }   // overflow: just repaint everything
}
void doMarkAll() { nPend = 0; doMarkDirty({0, 0, W, H}); }


void paintRect(const Rect &r, lgfx::LovyanGFX *target) {
  lgfx::LGFX_Sprite *tile = radar::scratchTile();
  loadCols(tile);
  for (int y = r.y0; y < r.y1; y += radar::TILE_H)
    for (int x = r.x0; x < r.x1; x += radar::TILE_W) {
      const Rect t = {(int16_t)x, (int16_t)y, (int16_t)std::min<int>(x + radar::TILE_W, r.x1), (int16_t)std::min<int>(y + radar::TILE_H, r.y1)};
      const int w = t.x1 - t.x0, h = t.y1 - t.y0;
      tile->setClipRect(0, 0, w, h);
      tile->fillRect(0, 0, w, h, TFT_BLACK);
      Ctx g{tile, t.x0, t.y0, kDejaVu};
      drawScreen(g, t);
      tile->clearClipRect();
      target->setClipRect(t.x0, t.y0, w, h);
      tile->pushSprite(target, t.x0, t.y0);
      target->clearClipRect();
    }
}

// ---------------------------------------------------------------- settings touch
bool sDown = false;
int sLastX = 0, sLastY = 0;
uint32_t sNextRepeat = 0;

void setPressed(const Hit &h) {
  if (h == gPressed) return;
  if (gPressed.type != H_NONE) markDirty(grow(hitRectOf(gPressed), 2));
  gPressed = h;
  if (h.type != H_NONE) markDirty(grow(hitRectOf(h), 2));
}

void activate(const Hit &h) {
  const int page = gPage;
  switch (h.type) {
    case H_TAB: gPage = h.idx; markAll(); break;
    case H_DONE: close(); return;
    case H_ARROW: stepRow(page, h.idx, h.dir); markAll(); break;
    case H_TOGGLE:
      if (page == 0 && h.idx == 2) radar::setPaused(!radar::paused());
      else if (page == PAGE_LOG && h.idx == 1) app::setSdLog(!settings::get().sdLog);
      else if (page == 0 && h.idx == 3) app::setShowDiag(!settings::get().showDiag);
      else if (page == 0 && h.idx == 4) app::setMapOverlay(!settings::get().mapOverlay);
      else if (page == 1 && h.idx == 0) app::setUnitsAuto(!settings::get().unitsAuto);
      else if (page == PAGE_FLIGHT && h.idx == 0) app::setFlightEnabled(!settings::get().faEnabled);
      else if (page == PAGE_FLIGHT && h.idx == 3) app::setFlightPhoto(!settings::get().faPhoto);
      else if (page == PAGE_FLIGHT && h.idx == 4) app::setFlightTrack(!settings::get().faTrack);
      markAll();
      break;
    case H_ACTION:
      if (page == PAGE_SYSTEM && h.idx == 1) app::restart();
      else if (page == PAGE_SYSTEM && h.idx == 2) { close(); radar::setTestPattern(true); }
      else if (page == PAGE_SYSTEM && h.idx == 4) in::openAbout();
      else if (page == PAGE_WIFI && h.idx == 1) in::openScan();
      else if (page == PAGE_WIFI && h.idx == 2) in::openPhone();
      else if (page == PAGE_LOG && h.idx == 4) { if (sdlog::status().state == sdlog::S_EJECTED) app::sdMount(); else app::sdEject(); markAll(); }
      else if (page == PAGE_LOG && h.idx == 5) in::openLogList();
      else if (page == PAGE_LOC && h.idx == 1) in::openCitySearch();
      else if (page == PAGE_LOC && h.idx == 2) in::openCoordinates();
      else if (page == PAGE_LOC && h.idx == 3) in::openMap();
      else if (page == PAGE_LOC && h.idx == 4) in::openCountryPick();
      else if (page == PAGE_LOC && h.idx == 5) in::openZonePick();
      else if (page == PAGE_FLIGHT && h.idx == 1) in::openFlightKey();
      else if (page == PAGE_FLIGHT && h.idx == 2) in::openFlightEmail();
      else if (page == PAGE_FLIGHT && h.idx == 5) { app::testFlightKey(); markAll(); }
      break;
    default: break;
  }
}

void settingsTouch(const gt911::Event &e) {
  if (e.n > 0) {
    const int x = e.x[0], y = e.y[0];
    sLastX = x; sLastY = y;
    if (!sDown) {
      sDown = true;
      const Hit h = hitTest(x, y);
      setPressed(h);
      if (h.type == H_TAB || h.type == H_ARROW) {   // act at once; the brightness arrows repeat while held
        activate(h);
        sNextRepeat = (h.type == H_ARROW && gPage == 2 && h.idx == 6) ? millis() + 450 : 0;
      }
    } else if (gPressed.type == H_TOGGLE || gPressed.type == H_ACTION || gPressed.type == H_DONE) {
      if (!(hitTest(x, y) == gPressed)) setPressed(Hit{});   // slid off the button: cancel
    }
  } else if (sDown) {
    sDown = false;
    sNextRepeat = 0;
    const Hit h = gPressed;
    setPressed(Hit{});
    if (h.type == H_TOGGLE || h.type == H_ACTION || h.type == H_DONE) activate(h);
  }
}

// ---------------------------------------------------------------- radar-screen gestures
struct Gesture {
  bool down = false, pinch = false, moved = false;
  int sx = 0, sy = 0, lx = 0, ly = 0;
  float d0 = 1;
  uint32_t lastZoomMs = 0;
  radar::Button btn = radar::BTN_NONE;
} G;

void radarTap(int x, int y) {
  if (radar::testPatternOn()) { radar::setTestPattern(false); Serial.println("[dev] tear test off"); return; }   // any tap leaves the test
  if (int col = radar::infoHeaderHit(x, y)) {   // table header: next distance / altitude / speed unit
    const settings::Settings &S = settings::get();
    if (col == 1) app::setDist(S.dist + 1); else if (col == 2) app::setAlt(S.alt + 1); else app::setSpd(S.spd + 1);
    return;
  }
  if (radar::infoClockHit(x, y) == 1) { app::setClock24(!settings::get().clock24); return; }   // "Time" label: 12 / 24 h
  if (radar::selectRowAt(x, y)) return;
  radar::selectAt(x, y);
}

void radarTouch(const gt911::Event &e) {
  if (e.n >= 2) {   // pinch: fingers apart = zoom in (smaller range), together = zoom out; one range step per ~35 % change
    const float dx = (float)(e.x[0] - e.x[1]), dy = (float)(e.y[0] - e.y[1]);
    const float d = sqrtf(dx * dx + dy * dy);
    if (!G.pinch) {
      G.pinch = true; G.d0 = std::max(d, 1.0f);
      radar::setButtonPressed(radar::BTN_NONE);
      G.btn = radar::BTN_NONE;
    } else if (millis() - G.lastZoomMs > 250) {
      const float ratio = d / G.d0;
      if (ratio > 1.35f) { app::zoom(-1); G.d0 = d; G.lastZoomMs = millis(); }
      else if (ratio < 0.74f) { app::zoom(+1); G.d0 = d; G.lastZoomMs = millis(); }
    }
    G.down = true; G.moved = true;
    return;
  }
  if (e.n == 1) {
    if (G.pinch) return;   // one finger left after a pinch: ignore until everything is lifted
    const int x = e.x[0], y = e.y[0];
    G.lx = x; G.ly = y;
    if (!G.down) {
      G.down = true; G.moved = false; G.sx = x; G.sy = y;
      G.btn = radar::buttonAt(x, y);
      radar::setButtonPressed(G.btn);
    } else {
      if (abs(x - G.sx) > 18 || abs(y - G.sy) > 18) G.moved = true;
      if (G.btn != radar::BTN_NONE && radar::buttonAt(x, y) != G.btn) { G.btn = radar::BTN_NONE; radar::setButtonPressed(radar::BTN_NONE); }
    }
    return;
  }
  if (!G.down) return;   // lift
  const radar::Button b = G.btn;
  const bool wasPinch = G.pinch, moved = G.moved;
  const int sx = G.sx, sy = G.sy;
  G = Gesture();
  radar::setButtonPressed(radar::BTN_NONE);
  if (wasPinch) return;
  if (b != radar::BTN_NONE) {
    switch (b) {
      case radar::BTN_ZOOM_IN: app::zoom(-1); break;
      case radar::BTN_ZOOM_OUT: app::zoom(+1); break;
      case radar::BTN_PAUSE: radar::setPaused(!radar::paused()); break;
      case radar::BTN_SETTINGS: open(); break;
      default: break;
    }
    return;
  }
  if (!moved) radarTap(sx, sy);
}

}  // namespace

void begin(lgfx::LovyanGFX *disp) { gDisp = disp; }
bool active() { return gActive; }

void open() {
  gActive = true;
  gPage = 0;
  gPressed = Hit{};
  sDown = false;
  G = Gesture();
  radar::setButtonPressed(radar::BTN_NONE);
  radar::clearSelection();
  markAll();
  Serial.println("[ui] settings open");
}

void openSetup() {   // no network saved: straight to the phone-setup screen
  open();
  gPage = PAGE_WIFI;
  in::openPhone();
}

void close() {
  if (gSub) { gSub->leave(); gSub = nullptr; }
  gActive = false;
  nPend = 0;
  gPressed = Hit{};
  sDown = false;
  settings::changed();
  radar::invalidate();
  Serial.println("[ui] settings closed");
}

static void otaTakeOver() {
  if (gActive) return;   // take over the screen from the radar (touch is ignored while updating)
  gActive = true; gPressed = Hit{}; sDown = false; subDown = false; subSwallow = false; G = Gesture();
  radar::setButtonPressed(radar::BTN_NONE);
  radar::clearSelection();
}

void otaProgress(int pct, const char *msg, bool failed) {
  if (!gDisp) return;
  otaTakeOver();
  in::otaShow(pct, msg, failed);
  frame();
}

void otaDark(int pct) {
  if (!gDisp) return;
  otaTakeOver();
  in::otaShowDark(pct);
  frame();
}

void otaDone() { in::otaHide(); close(); }

void touch(const gt911::Event &e) {
  if (!gActive) { radarTouch(e); return; }
  if (!gSub) { settingsTouch(e); return; }
  if (subSwallow) {   // the screen changed during this touch: ignore it until the finger lifts
    if (e.n == 0) { subSwallow = false; subDown = false; }
    return;
  }
  if (e.n > 0) {
    subX = e.x[0]; subY = e.y[0];
    if (!subDown) { subDown = true; gSub->down(subX, subY); }
    else gSub->move(subX, subY);
  } else if (subDown) {
    subDown = false;
    gSub->up(subX, subY);
  }
}

void frame() {
  if (!gActive) return;
  if (gSub) gSub->tick();
  static uint32_t seenFlightVer = 0;
  if (!gSub && gPage == PAGE_FLIGHT && flightinfo::version() != seenFlightVer) { seenFlightVer = flightinfo::version(); markAll(); }   // lookup / key test results arrive in the background
  if (sDown && sNextRepeat && millis() >= sNextRepeat && gPressed.type == H_ARROW) {   // held brightness arrow
    activate(gPressed);
    sNextRepeat = millis() + 130;
  }
  if (nPend == 0) return;
  const bool dbl = display::doubleBuffered();
  lgfx::LovyanGFX *target = gDisp;
  if (dbl) { display::waitFlipped(100); target = display::backFb(); }
  else display::waitVsync(40);
  for (int i = 0; i < nPend; i++) paintRect(pend[i].r, target);
  if (dbl) display::flip();
  int k = 0;
  for (int i = 0; i < nPend; i++) if (--pend[i].left > 0) pend[k++] = pend[i];
  nPend = k;
}

namespace in {

void markDirty(Rect r) { doMarkDirty(r); }
void markAll() { doMarkAll(); }

void setSub(Screen *s) {
  if (gSub) gSub->leave();
  if (!gSub && s) gReturnPage = gPage;   // the settings tab to come back to
  gSub = s;
  if (subDown) subSwallow = true;
  if (gSub) gSub->enter(); else gPage = gReturnPage;
  doMarkAll();
}

Rect backRect() { return doneRect(); }

void drawBtn(Ctx &g, const Rect &r, const char *text, bool down, bool enabled) {
  g.frrect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, 8, down ? C.dark : TFT_BLACK);
  g.rrect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, 8, enabled ? (down ? C.bright : C.mid) : C.dark);
  g.textFont(F18, text, (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2, textdatum_t::middle_center, enabled ? C.title : C.dim);
}

void drawHeader(Ctx &g, const Rect &t, const char *title) {
  if (!hitR(t, Rect{0, 0, W, 64})) return;
  g.textFont(F24, title, 24, 32, textdatum_t::middle_left, C.title);
  g.hline(0, 62, W, C.mid);
}

}  // namespace in

}  // namespace ui
