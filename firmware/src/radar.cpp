#include "radar.h"
#include "config.h"
#include "settings.h"
#include "units.h"
#include "wifimgr.h"
#include "airline.h"
#include "mapdata.h"
#include "sdlog.h"
#include "gfx.h"
#include "palette.h"
#include "night.h"
#include "flightinfo.h"
#include "icao.h"
#include "geo.h"
#include <WiFi.h>
#include <math.h>
#include <time.h>
#include <algorithm>

// Rendering strategy (why it looks the way it does):
//  The RGB framebuffer lives in PSRAM and is read continuously by the display DMA/ISR. PSRAM bandwidth is the scarce
//  resource: drawing lines/circles pixel-by-pixel *into PSRAM* makes the CPU read-modify-write whole cache lines and
//  starves the display (picture jumps, tears). So nothing is drawn in PSRAM:
//   * `bg` (PSRAM, written once) is the static radar: grid, rings, scale, labels, compass.
//   * Each frame we work out which rectangles changed (sweep wedge, aircraft that moved/faded, readouts).
//   * Each rectangle is painted in a small INTERNAL-RAM tile: copy the bg rectangle in, draw everything that overlaps
//     it (wedge, rings over the wedge, aircraft, readouts), then push the tile to the screen in one sequential burst.
//   * The info panel is repainted in 47-row bands the same way.

namespace radar {
namespace {
using namespace gfx;

// ---------------------------------------------------------------- layout (same as the original)
constexpr int CX = 240, CY = 240, R = 212;          // radar centre / outer radius inside the 480x480 radar area
constexpr int AREA_X = 5, AREA_Y = 5, AREA_W = 470, AREA_H = 470;
constexpr int INFO_X = 485, INFO_Y = 5, INFO_W = 310, INFO_H = 470;
constexpr int RADAR_W = 480;
constexpr int TRAIL_MAX = 60;                        // longest trail (deg); the live setting is gTrailDeg
constexpr float SWEEP_DEG_PER_S = 80.0f;             // 4.5 s / revolution (original: 2 deg every 25 ms)
float RANGE_KM = RADAR_RADIUS_NM * 1.852f;   // follows settings (applyLocation)
double gHomeLat = HOME_LAT, gHomeLon = HOME_LON;
// Aircraft marks never vanish: bright when the sweep has just passed, then stepping down to a dim floor until it passes again.
// Few coarse steps (not a continuous fade) keep the number of repainted tiles low.
constexpr float kFade[6] = {1.0f, 0.62f, 0.40f, 0.26f, 0.17f, 0.11f};   // floor 0.11: still visible, but only just
constexpr float kFadeAge[5] = {35, 90, 150, 220, 290};   // degrees behind the sweep where each step starts
constexpr float kLabelAge[4] = {50, 100, 150, 200};      // label brightness steps; gone after the last
constexpr float LABEL_DEG = 200.0f;                  // callsign stays up this many degrees behind the sweep (~2.5 s)
constexpr int MAX_ROWS = 14;
constexpr int BAND_H = 47;                           // info band height (470 / 10 bands)

lgfx::LovyanGFX *gDisp = nullptr;
LGFX_Sprite bg;     // PSRAM: static radar layer
LGFX_Sprite tile;   // internal RAM: radar scratch tile
LGFX_Sprite band;   // internal RAM: info panel scratch band

uint16_t cBright, cGreen, cDim, cDark;   // radar shades
uint16_t cLBright, cLMid, cLDim, cLDark;   // frames, separators and buttons (the "Lines & Buttons" colour)
uint16_t cTitle, cTitle2, cCall, cVal, cVal2;   // titles, callsigns, values (palette roles)
uint16_t cMap, cMapB;                            // map overlay: coastlines, borders
uint16_t cEmerg;                                 // emergency squawk (7500/7600/7700): red, or white while the display is all red at night
int16_t tick[360][4];  // x1,y1,x2,y2 of every degree tick on the outer scale

// Sweep trail as a per-pixel lookup ("palette rotation"): angleMap holds every radar pixel's bearing step (0..254, 255 = outside
// the disk). A pixel's trail colour depends only on how many steps it is behind the sweep, so a frame needs one pass over the
// wedge's pixels instead of ~120 radial lines + redrawing the rings on top.
constexpr int ANGLE_STEPS = 255;
constexpr int TRAIL_STEPS_MAX = (int)(TRAIL_MAX * ANGLE_STEPS / 360.0f);
int gTrailDeg = TRAIL_MAX, gTrailSteps = TRAIL_STEPS_MAX;
bool gSwapped = false, gForceFull = false;
uint8_t *angleMap = nullptr;                // PSRAM, 480x480 bytes
uint16_t wedgeLut[TRAIL_STEPS_MAX + 1];         // trail colour by distance behind the sweep, in the screen's native pixel format
uint16_t nBlack = 0, nGrid = 0;             // the only background pixels the trail may replace (black, grid line), native format

inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return bg.color565(r, g, b); }

// Font themes and the Ctx drawing context live in gfx.h.
// Info-table layout, all computed from the active theme's font metrics (columns: call, dist, alt, spd, type).
int infoX[5] = {10, 80, 130, 180, 230}, infoW[5] = {60, 40, 40, 40, 40};
bool infoShowType = true, infoCompact = false;   // compact = short column titles ("Dist" instead of "Dist km") when the long ones do not fit
int infoHdrY = 30, infoLineY = 48, infoRow0 = 54, infoRows = 10;
constexpr int ROW_H = 24, FOOT_Y = 396;     // table rows are 24 px; the footer (location + clock) starts at y=396
bool gBandDirty[10] = {true, true, true, true, true, true, true, true, true, true};

// ---------------------------------------------------------------- dirty rectangles (x1,y1 exclusive)
inline Rect clipR(Rect r) {
  r.x0 = max<int>(r.x0, 0); r.y0 = max<int>(r.y0, 0);
  r.x1 = min<int>(r.x1, RADAR_W); r.y1 = min<int>(r.y1, RADAR_W);
  return r;
}

constexpr int MAX_DIRTY = 300;   // more than this is a full repaint anyway (see frame()); each of the five rectangle lists costs 2.4 KB of internal RAM
inline long areaR(const Rect &r) { return (long)(r.x1 - r.x0) * (r.y1 - r.y0); }
// Merge overlapping dirty rectangles when the union is not much bigger than the parts; painting the same pixels twice is wasted work.
void mergeRects(Rect *r, int &n) {
  for (bool again = true; again;) {
    again = false;
    for (int i = 0; i < n && !again; i++)
      for (int j = i + 1; j < n; j++) {
        if (!hitR(r[i], r[j])) continue;
        Rect u = uniR(r[i], r[j]);
        if (areaR(u) > (areaR(r[i]) + areaR(r[j])) * 3 / 2 + 400) continue;  // would repaint far more than needed
        r[i] = u;
        r[j] = r[--n];
        again = true;
        break;
      }
  }
}
Rect dirty[MAX_DIRTY];
int nDirty = 0;
void addDirty(Rect r) {
  r = clipR(r);
  if (!emptyR(r) && nDirty < MAX_DIRTY) dirty[nDirty++] = r;
}

bool gVerify = false;
bool gTest = false, gWipe = false;   // gWipe: blank both buffers and repaint everything (leaving the tear test / the settings screen)
Rect lastRects[MAX_DIRTY], curRects[MAX_DIRTY];
int nLastR = 0, nCurR = 0;
inline void noteRect(const Rect &r) { if (nCurR < MAX_DIRTY) curRects[nCurR++] = r; }

const Rect HUD_RECT = {8, 8, 300, 148};     // status text, top-left
const Rect BARS_RECT = {388, 8, 478, 90};   // range bars, top-right
// On-screen buttons sit in the corners outside the radar disc (distance from the centre > R+8), 44 px drawn, 54 px touch target.
constexpr int BTN_R = 20, BTN_HIT = 27;
const struct { int16_t x, y; } kBtnPos[5] = {{0, 0}, {450, 385}, {385, 450}, {112, 452}, {440, 440}};   // indexed by Button
inline Rect btnRect(Button b) { return {(int16_t)(kBtnPos[b].x - BTN_R - 2), (int16_t)(kBtnPos[b].y - BTN_R - 2), (int16_t)(kBtnPos[b].x + BTN_R + 3), (int16_t)(kBtnPos[b].y + BTN_R + 3)}; }

// ---------------------------------------------------------------- state
struct Plot {
  char hex[8], flight[10], type[8], cat[4];
  uint8_t emerg;         // 0, or 1/2/3 for squawk 7500 / 7600 / 7700
  double lat0, lon0;     // position at the last fetch; dead reckoning moves distKm/bearing from here
  float distKm, bearing;
  int altFt;
  float gs, track;
};
Plot plots[MAX_AIRCRAFT];
int nPlots = 0;
int infoIdx[MAX_ROWS];  // nearest aircraft for the list

struct AcState { bool vis, sel; int16_t x, y; uint8_t fadeQ, lblQ; };  // what is currently on screen for each aircraft
AcState st[MAX_AIRCRAFT], prevSt[MAX_AIRCRAFT];

// Flight details of the selected aircraft (flightinfo.h): its flown track (drawn on the radar) and photo (drawn under the card)
constexpr int TRK_MAX = 300;
float *gTrkLat = nullptr, *gTrkLon = nullptr, *gTrkX = nullptr, *gTrkY = nullptr;   // PSRAM; X/Y = the track projected on the radar
int gTrkN = 0;
uint32_t gFiVer = 0xFFFFFFFF;       // flightinfo version last looked at
char gFiHex[8] = "";                // aircraft those details belong to
bool gPhotoOn = false;              // the photo is on screen
int gPhotoW = 0, gPhotoH = 0;
char gPhotoWho[32] = "";

bool dataOk = false;
char gStatus[40] = "";        // reason for the last fetch result ("OK", "No Wi-Fi", "Certificate error", ...)
uint32_t gFailStreak = 0;
uint32_t lastOkMs = 0;
int lastHttp = 0;
float sweepAngle = 0;
bool barsDirty = true, infoDataDirty = true, firstFrame = true;
int lastTimeSec = -1;
uint32_t lastHudSig = 0xFFFFFFFF;
const char *gModeLabel = "";
bool gPaused = false;          // sweep frozen
char gSelHex[8] = "";          // selected aircraft (ICAO hex); the index follows it across fetches
int gSelIdx = -1;
Button gPressed = BTN_NONE;    // on-screen button currently held down
Rect wCur = {0, 0, 0, 0};
Stats gStats = {};
const adsb::Snapshot *gSnap = nullptr;   // the last data handed to setData(): the card reads the selected aircraft's model and registration from it (it stays valid until the next setData)
uint32_t usPaintAvg = 0, usFlushAvg = 0;
int tilesThisFrame = 0;
volatile uint32_t gRiskTiles = 0, gPushedTiles = 0;

// ---------------------------------------------------------------- geometry helpers
double distanceKm(double lat1, double lon1, double lat2, double lon2) {
  const double d2r = M_PI / 180.0;
  double dLat = (lat2 - lat1) * d2r, dLon = (lon2 - lon1) * d2r;
  double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dLon / 2) * sin(dLon / 2);
  return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

float bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  const double d2r = M_PI / 180.0;
  double y = sin((lon2 - lon1) * d2r) * cos(lat2 * d2r);
  double x = cos(lat1 * d2r) * sin(lat2 * d2r) - sin(lat1 * d2r) * cos(lat2 * d2r) * cos((lon2 - lon1) * d2r);
  double b = atan2(y, x) / d2r;
  return b < 0 ? b + 360.0 : b;
}

float behindSweep(float sweep, float bearing) {  // how far (deg) the target is behind the sweep line
  float d = sweep - bearing;
  while (d < 0) d += 360;
  while (d >= 360) d -= 360;
  return d;
}

uint16_t altColor(int altFt, float fade) {
  uint8_t r, g, b;
  if (night::active()) { const palette::Rgb h = palette::shade(night::pal(0), fade); return rgb(h.r, h.g, h.b); }   // all red at night
  if (altFt >= 30000)      { r = 0;           g = 180 * fade; b = 255 * fade; }
  else if (altFt >= 10000) { r = 0;           g = 255 * fade; b = 35 * fade; }
  else if (altFt >= 0)     { r = 255 * fade;  g = 220 * fade; b = 0; }
  else                     { r = 255 * fade;  g = 40 * fade;  b = 40 * fade; }
  return rgb(r, g, b);
}

// bounding box of the sweep wedge (centre + arc), padded
Rect wedgeRect(float sweep) {
  float a1 = sweep + 1.0f, a0 = sweep - gTrailDeg - 1.0f;
  float minx = CX, maxx = CX, miny = CY, maxy = CY;
  auto add = [&](float bearing) {
    float rad = (bearing - 90) * DEG_TO_RAD;
    float x = CX + cosf(rad) * (R + 1), y = CY + sinf(rad) * (R + 1);
    minx = min(minx, x); maxx = max(maxx, x); miny = min(miny, y); maxy = max(maxy, y);
  };
  add(a0);
  add(a1);
  for (int c = (int)ceilf(a0 / 90.0f) * 90; c <= a1; c += 90) add((float)c);  // axis extremes inside the arc
  return clipR({(int16_t)(floorf(minx) - 3), (int16_t)(floorf(miny) - 3), (int16_t)(ceilf(maxx) + 4), (int16_t)(ceilf(maxy) + 4)});
}

Rect acRect(const AcState &a, size_t len);
Rect markerBox(int x, int y) { return {(int16_t)(x - 20), (int16_t)(y - 20), (int16_t)(x + 21), (int16_t)(y + 21)}; }
// Callsigns on the radar always use DejaVu (other fonts look bad there). 9 = one step below the 12 used before; set kLabelFont back to DejaVu12 / charW 9 to undo.
const lgfx::IFont *const kLabelFont = &lgfx::fonts::DejaVu9;
constexpr int kLabelCharW = 7;
Rect labelBox(int x, int y, size_t len) { return {(int16_t)(x + 8), (int16_t)(y - 14), (int16_t)(x + 14 + len * kLabelCharW), (int16_t)(y + 3)}; }
const char *labelOf(const Plot &p) { return p.flight[0] ? p.flight : "UNKNOWN"; }
size_t lblLen(const Plot &p) { return strlen(labelOf(p)) + (p.emerg ? 5 : 0); }   // an emergency's label carries its squawk code
uint8_t emergencyOf(const char *sq) { return !strcmp(sq, "7500") ? 1 : !strcmp(sq, "7600") ? 2 : !strcmp(sq, "7700") ? 3 : 0; }
const char *emergencyCode(uint8_t e) { return e == 1 ? "7500" : e == 2 ? "7600" : "7700"; }
const char *emergencyName(uint8_t e) { return e == 1 ? "HIJACK" : e == 2 ? "RADIO FAIL" : "EMERGENCY"; }
int gEmergCount = 0, gEmergIdx = -1;      // aircraft squawking an emergency code, and the nearest of them
uint8_t gDebugEmerg = 0;                  // dev: pretend the nearest aircraft squawks this code (serial 'E')
void scanEmergencies() {
  gEmergCount = 0; gEmergIdx = -1;
  for (int i = 0; i < nPlots; i++)
    if (plots[i].emerg) { gEmergCount++; if (gEmergIdx < 0 || plots[i].distKm < plots[gEmergIdx].distKm) gEmergIdx = i; }
}
Rect acRect(const AcState &a, size_t len) { return a.lblQ ? uniR(markerBox(a.x, a.y), labelBox(a.x, a.y, len)) : markerBox(a.x, a.y); }

// ---------------------------------------------------------------- static layer (drawn once into bg)
void drawGrid(Ctx &g) {
  // Grid cells are exactly one ring step wide (range/5, always a round number), so lines meet the rings on the axes and the cell size
  // follows the selected range. Lines exist only inside the radar circle: each one is cut to its chord. (The square frame stays.)
  for (int k = -4; k <= 4; k++) {
    const int d = (int)lroundf(k * R / 5.0f);
    const int half = (int)sqrtf((float)(R * R - d * d));
    g.vline(CX + d, CY - half, half * 2 + 1, cDark);
    g.hline(CX - half, CY + d, half * 2 + 1, cDark);
  }
  g.rect(AREA_X, AREA_Y, AREA_W, AREA_H, cLMid);
}

void drawRings(Ctx &g) {
  // Rings sit at exact fractions of the range, so a "60 km" ring really is 60 km (the original's labels were off).
  for (int i = 1; i <= 5; i++) g.circle(CX, CY, R * i / 5, i == 5 ? cBright : cGreen);
  for (int i = 0; i < 3; i++) g.circle(CX, CY, R + 4 + i, cBright);
}

void drawCross(Ctx &g) {
  g.hline(CX - R, CY, R * 2, cBright);
  g.vline(CX, CY - R, R * 2, cBright);
  for (int d = 20; d < R; d += 40) {
    g.vline(CX + d, CY - 7, 14, cBright);
    g.vline(CX - d, CY - 7, 14, cBright);
    g.hline(CX - 7, CY + d, 14, cBright);
    g.hline(CX - 7, CY - d, 14, cBright);
  }
  for (int a = 30; a < 360; a += 30) {
    if (a % 90 == 0) continue;
    float rad = (a - 90) * DEG_TO_RAD;
    g.line(CX, CY, CX + cosf(rad) * R, CY + sinf(rad) * R, cDim);
  }
}

void drawRangeLabels(Ctx &g) {
  for (int i = 1; i <= 5; i++) {
    int r = R * i / 5;
    const auto du = (units::Dist)settings::get().dist;
    String s = String((int)lroundf(units::fromKm(RANGE_KM * i / 5, du))) + units::name(du);
    g.text(s, CX + 18, CY - r - 9, 1, textdatum_t::middle_center, cBright);
    g.text(s, CX + 18, CY + r + 9, 1, textdatum_t::middle_center, cBright);
  }
  g.text("0", CX + 15, CY + 12, 1, textdatum_t::middle_center, cBright);
}

void drawAngleScale(Ctx &g) {
  for (int a = 0; a < 360; a++) g.line(tick[a][0], tick[a][1], tick[a][2], tick[a][3], cBright);
  for (int a = 0; a < 360; a += 30) {
    float rad = (a - 90) * DEG_TO_RAD;
    char buf[6];
    snprintf(buf, sizeof(buf), "%d", a);
    g.text(buf, CX + cosf(rad) * (R - 28), CY + sinf(rad) * (R - 28), 2, textdatum_t::middle_center, cBright);
  }
}

void drawCompass(Ctx &g) {
  const int ccx = 50, ccy = 427;
  g.circle(ccx, ccy, 17, cDim);
  g.circle(ccx, ccy, 18, cDark);
  g.ftri(ccx, ccy - 16, ccx - 5, ccy - 4, ccx + 5, ccy - 4, cBright);
  g.tri(ccx, ccy - 16, ccx - 5, ccy - 4, ccx + 5, ccy - 4, TFT_WHITE);
  g.ftri(ccx, ccy + 16, ccx - 5, ccy + 4, ccx + 5, ccy + 4, cDim);
  g.ftri(ccx - 16, ccy, ccx - 4, ccy - 5, ccx - 4, ccy + 5, cDim);
  g.ftri(ccx + 16, ccy, ccx + 4, ccy - 5, ccx + 4, ccy + 5, cDim);
  g.fcircle(ccx, ccy, 2, cBright);
  g.circle(ccx, ccy, 4, cDim);
  g.text("N", ccx, ccy - 29, 1, textdatum_t::middle_center, cBright);
  g.text("S", ccx, ccy + 29, 1, textdatum_t::middle_center, cDim);
  g.text("W", ccx - 29, ccy - 3, 1, textdatum_t::middle_center, cDim);
  g.text("E", ccx + 29, ccy - 3, 1, textdatum_t::middle_center, cDim);
}

// ---------------------------------------------------------------- map overlay (static layer)
// Coastlines, borders and airports from the embedded 1:10m data, drawn with the same azimuthal-equidistant projection the aircraft use
// (distance and bearing from the home position), so a coast line and an aircraft that crosses it agree. It lives in `bg`: no cost per frame.
struct MapCtx { Ctx *g; float sLat0, cLat0; double lon0; float kmToPx; int segs, pts, airports; };

inline void azProject(const MapCtx &m, double lat, double lon, float &x, float &y) {
  const float la = (float)(lat * DEG_TO_RAD), dl = (float)((lon - m.lon0) * DEG_TO_RAD);
  const float sLa = sinf(la), cLa = cosf(la), sD = sinf(dl), cD = cosf(dl);
  const float a = cLa * sD;                                   // east component
  const float b = m.cLat0 * sLa - m.sLat0 * cLa * cD;         // north component
  const float cc = m.sLat0 * sLa + m.cLat0 * cLa * cD;
  const float s = hypotf(a, b);                               // sin of the angular distance
  const float k = s < 1e-6f ? 1.0f : atan2f(s, cc) / s;       // distance / sin(distance): scales the direction to the true distance
  x = CX + 6371.0f * k * a * m.kmToPx;
  y = CY - 6371.0f * k * b * m.kmToPx;
}

// Cut a segment to the radar disc; false if no part of it is inside.
bool clipToDisc(float &x0, float &y0, float &x1, float &y1) {
  const float rr = (float)R + 0.5f, dx = x1 - x0, dy = y1 - y0, fx = x0 - CX, fy = y0 - CY;
  const float a = dx * dx + dy * dy, c = fx * fx + fy * fy - rr * rr;
  if (a < 1e-6f) return c <= 0;
  const float b = 2 * (fx * dx + fy * dy), disc = b * b - 4 * a * c;
  if (disc < 0) return false;
  const float sq = sqrtf(disc), t0 = (-b - sq) / (2 * a), t1 = (-b + sq) / (2 * a);
  if (t1 < 0 || t0 > 1) return false;
  const float ta = t0 > 0 ? t0 : 0, tb = t1 < 1 ? t1 : 1;
  const float nx0 = x0 + dx * ta, ny0 = y0 + dy * ta, nx1 = x0 + dx * tb, ny1 = y0 + dy * tb;
  x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
  return true;
}

void projectTrack() {
  if (!gTrkX) return;
  MapCtx m = {nullptr, sinf((float)(gHomeLat * DEG_TO_RAD)), cosf((float)(gHomeLat * DEG_TO_RAD)), gHomeLon, (float)R / RANGE_KM, 0, 0, 0};
  for (int i = 0; i < gTrkN; i++) azProject(m, gTrkLat[i], gTrkLon[i], gTrkX[i], gTrkY[i]);
}

void mapLine(const double *lat, const double *lon, int n, int kind, void *ctx) {
  MapCtx &m = *(MapCtx *)ctx;
  float px[100], py[100];
  for (int i = 0; i < n; i++) azProject(m, lat[i], lon[i], px[i], py[i]);
  const uint16_t col = kind ? cMapB : cMap;
  for (int i = 1; i < n; i++) {
    float x0 = px[i - 1], y0 = py[i - 1], x1 = px[i], y1 = py[i];
    if (!clipToDisc(x0, y0, x1, y1)) continue;
    m.g->line((int)lroundf(x0), (int)lroundf(y0), (int)lroundf(x1), (int)lroundf(y1), col);
    m.segs++;
  }
  m.pts += n;
}

void mapAirport(double lat, double lon, const char *iata, int type, void *ctx) {
  MapCtx &m = *(MapCtx *)ctx;
  float x, y;
  azProject(m, lat, lon, x, y);
  const float dx = x - CX, dy = y - CY, lim = R - 14.0f;
  if (dx * dx + dy * dy > lim * lim) return;
  const int xi = (int)lroundf(x), yi = (int)lroundf(y);
  m.g->line(xi, yi - 4, xi + 4, yi, cMap); m.g->line(xi + 4, yi, xi, yi + 4, cMap);    // small diamond
  m.g->line(xi, yi + 4, xi - 4, yi, cMap); m.g->line(xi - 4, yi, xi, yi - 4, cMap);
  if (type >= 1 || RANGE_KM <= 80) m.g->textFont(kLabelFont, iata, xi + 7, yi - 5, textdatum_t::top_left, cMap);
  m.airports++;
}

void drawMapOverlay(Ctx &g) {
  if (!settings::get().mapOverlay || !mapdata::ready()) return;
  const uint32_t t0 = millis();
  const double lat0 = gHomeLat;
  MapCtx m = {&g, sinf((float)(lat0 * DEG_TO_RAD)), cosf((float)(lat0 * DEG_TO_RAD)), gHomeLon, (float)R / RANGE_KM, 0, 0, 0};
  const double dLat = RANGE_KM * 1.12 / 111.0 + 0.05;
  const double cl = std::max(0.02, cos(lat0 * DEG_TO_RAD));
  const double dLon = std::min(180.0, RANGE_KM * 1.12 / (111.3 * cl) + 0.05);
  const double latMin = std::max(-90.0, lat0 - dLat), latMax = std::min(90.0, lat0 + dLat);
  mapdata::lines(latMin, latMax, gHomeLon - dLon, gHomeLon + dLon, mapLine, &m);
  mapdata::airports(latMin, latMax, gHomeLon - dLon, gHomeLon + dLon, mapAirport, &m);
  Serial.printf("[map] %d segments from %d points, %d airports in %u ms\n", m.segs, m.pts, m.airports, (unsigned)(millis() - t0));
}

void buildStaticLayer() {
  bg.fillSprite(TFT_BLACK);
  Ctx g{&bg, 0, 0, kDejaVu};
  drawGrid(g);
  drawMapOverlay(g);   // behind rings, scale and labels
  drawRings(g);
  drawCross(g);
  drawRangeLabels(g);
  drawAngleScale(g);
  drawCompass(g);
}

// ---------------------------------------------------------------- dynamic painters (all take a Ctx)
void paintWedge(Ctx &g, const Rect &t) {
  uint16_t *buf = (uint16_t *)g.s->getBuffer();
  const int stride = TILE_W;  // tile sprite width
  const int x0 = max<int>(t.x0, wCur.x0), x1 = min<int>(t.x1, wCur.x1);
  const int y0 = max<int>(t.y0, wCur.y0), y1 = min<int>(t.y1, wCur.y1);
  const int s = ((int)(sweepAngle * (ANGLE_STEPS / 360.0f))) % ANGLE_STEPS;
  for (int y = y0; y < y1; y++) {
    const uint8_t *am = angleMap + y * RADAR_W;
    uint16_t *row = buf + (y - t.y0) * stride - t.x0;
    for (int x = x0; x < x1; x++) {
      int a = am[x];
      if (a == 255) continue;
      int d = s - a;
      if (d < 0) d += ANGLE_STEPS;
      if (d > gTrailSteps) continue;
      uint16_t p = row[x];
      if (p == nBlack || p == nGrid) row[x] = wedgeLut[d];  // rings, scale, labels, axes stay on top of the trail
    }
  }
  float rad = (sweepAngle - 90) * DEG_TO_RAD;
  int sx = CX + cosf(rad) * R, sy = CY + sinf(rad) * R;
  g.line(CX, CY, sx, sy, cBright);
  g.line(CX + 1, CY, sx + 1, sy, cBright);
}

void paintAircraft(Ctx &g, const Rect &t) {
  for (int i = 0; i < nPlots; i++) {
    const AcState &a = st[i];
    if (!a.vis) continue;
    const Plot &p = plots[i];
    bool hitMarker = hitR(markerBox(a.x, a.y), t);
    bool hitLabel = a.lblQ && hitR(labelBox(a.x, a.y, lblLen(p)), t);
    if (!hitMarker && !hitLabel) continue;
    const bool em = p.emerg != 0;
    float fade = em ? 1.0f : kFade[a.fadeQ];
    uint16_t col = em ? cEmerg : altColor(p.altFt, fade);
    int size = 4;
    if (p.altFt > 30000) size = 5;
    if (p.distKm < RANGE_KM * 0.2f) size = 6;
    g.fcircle(a.x, a.y, size, col);
    if (em) { g.circle(a.x, a.y, size + 4, cEmerg); g.circle(a.x, a.y, size + 8, cEmerg); g.circle(a.x, a.y, size + 9, cEmerg); }   // two heavy rings: impossible to miss
    else { palette::Rgb h = palette::shade(night::pal(palette::RADAR), palette::kDim * fade); g.circle(a.x, a.y, size + 2, rgb(h.r, h.g, h.b)); }  // the halo dims with the mark
    if (a.sel) { g.circle(a.x, a.y, size + 8, cTitle); g.circle(a.x, a.y, size + 9, cTitle); }
    if (p.track >= 0) {
      float tr = (p.track - 90) * DEG_TO_RAD;
      g.line(a.x, a.y, a.x + cosf(tr) * 12, a.y + sinf(tr) * 12, col);
    }
    if (a.lblQ) {  // callsign: bright just after the sweep passes, then fades with the marker
      uint8_t v = 255 - (a.lblQ - 1) * 45;
      if (em) { char lb[24]; snprintf(lb, sizeof(lb), "%s %s", labelOf(p), emergencyCode(p.emerg)); g.textFont(kLabelFont, lb, a.x + 9, a.y - 11, textdatum_t::top_left, cEmerg); }
      else g.textFont(kLabelFont, labelOf(p), a.x + 9, a.y - 11, textdatum_t::top_left, night::active() ? rgb(v, v / 5, v / 5) : rgb(v, v, v));
    }
  }
}

// The flown track of the selected aircraft (FlightAware), drawn over the radar under the marks.
void paintTrack(Ctx &g, const Rect &t) {
  if (gTrkN < 2 || gSelIdx < 0 || !gTrkX) return;
  for (int i = 1; i < gTrkN; i++) {
    float x0 = gTrkX[i - 1], y0 = gTrkY[i - 1], x1 = gTrkX[i], y1 = gTrkY[i];
    if (fmaxf(x0, x1) < t.x0 - 1 || fminf(x0, x1) > t.x1 + 1 || fmaxf(y0, y1) < t.y0 - 1 || fminf(y0, y1) > t.y1 + 1) continue;
    if (!clipToDisc(x0, y0, x1, y1)) continue;
    g.line((int)lroundf(x0), (int)lroundf(y0), (int)lroundf(x1), (int)lroundf(y1), cTitle);
  }
}

String clockStr(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  char b[12];
  if (settings::get().clock24) snprintf(b, sizeof(b), "%02d:%02d", lt.tm_hour, lt.tm_min);
  else snprintf(b, sizeof(b), "%d:%02d%c", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min, lt.tm_hour >= 12 ? 'p' : 'a');
  return b;
}

// The aircraft's flight details changed (or the selection did): pick up the track, note whether the photo is on screen, repaint what is affected.
void refreshFlightInfo() {
  gFiVer = flightinfo::version();
  lastHudSig = 0xFFFFFFFF;
  if (gSelIdx < 0) {
    if (gPhotoOn) { for (bool &b : gBandDirty) b = true; gPhotoOn = false; }
    if (gTrkN) { gTrkN = 0; gForceFull = true; }
    return;
  }
  const Plot &p = plots[gSelIdx];
  flightinfo::Info fi;
  const bool have = flightinfo::get(p.flight, p.hex, fi);
  const bool ph = have && fi.photo == flightinfo::PH_OK;
  if (ph != gPhotoOn || (ph && (fi.photoW != gPhotoW || fi.photoH != gPhotoH))) for (bool &b : gBandDirty) b = true;   // the table makes room for the photo / takes it back
  gPhotoOn = ph;
  if (ph) { gPhotoW = fi.photoW; gPhotoH = fi.photoH; strlcpy(gPhotoWho, fi.photographer, sizeof(gPhotoWho)); }
  if (!gTrkLat) {
    gTrkLat = (float *)heap_caps_malloc(TRK_MAX * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    gTrkLon = (float *)heap_caps_malloc(TRK_MAX * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    gTrkX = (float *)heap_caps_malloc(TRK_MAX * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    gTrkY = (float *)heap_caps_malloc(TRK_MAX * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!gTrkLat || !gTrkLon || !gTrkX || !gTrkY) return;
  const int n = have && fi.trackPoints >= 2 ? flightinfo::copyTrack(p.flight, p.hex, gTrkLat, gTrkLon, TRK_MAX) : 0;
  if (n != gTrkN || n) { gTrkN = n; projectTrack(); gForceFull = true; }
}

// Info card for the selected aircraft, drawn in place of the status text.
const adsb::Aircraft *sourceOf(const Plot &p) {
  if (!gSnap || !p.hex[0]) return nullptr;
  for (int i = 0; i < gSnap->n; i++) if (!strcmp(gSnap->ac[i].hex, p.hex)) return &gSnap->ac[i];
  return nullptr;
}

void paintCardPanel(Ctx &g, int y0) {   // the aircraft card: takes the place of the aircraft table in the info panel (panel coordinates)
  const Plot &p = plots[gSelIdx];
  const adsb::Aircraft *src = sourceOf(p);
  const char *airlineName = airline::forCallsign(p.flight);
  const char *model = src && src->desc[0] ? src->desc : p.type[0] ? p.type : nullptr;
  const settings::Settings &S = settings::get();
  const auto du = (units::Dist)S.dist; const auto au = (units::Alt)S.alt; const auto su = (units::Spd)S.spd;
  const int X = 12;
  if (p.emerg) g.rect(3, 3, INFO_W - 6, INFO_H - 6, cEmerg);
  g.text(labelOf(p), X, 9, 2, textdatum_t::top_left, p.emerg ? cEmerg : cCall);
  if (p.emerg) { char eb[24]; snprintf(eb, sizeof(eb), "%s %s", emergencyCode(p.emerg), emergencyName(p.emerg)); g.text(eb, INFO_W - 10, 14, 1, textdatum_t::top_right, cEmerg); }
  else g.text(p.type[0] ? p.type : p.cat, INFO_W - 10, 14, 1, textdatum_t::top_right, cVal);
  int y = 9 + g.fh(2) + 5;
  const char *iso = icao::country(p.hex);   // state of registry, from the ICAO address block
  if (iso) {
    const int ci = geo::countryByCode(iso);
    char cn[40];
    snprintf(cn, sizeof(cn), "%.32s", ci >= 0 ? geo::countryName(ci) : iso);
    if (icao::drawFlag(g.s, X + 1 - g.ox, y + 1 - g.oy, iso)) g.rect(X, y, icao::FLAG_W + 2, icao::FLAG_H + 2, cLDim);
    g.text(cn, X + icao::FLAG_W + 10, y + 3, 1, textdatum_t::top_left, cVal);
    y += icao::FLAG_H + 6;
  }
  if (airlineName) { g.text(airlineName, X, y, 1, textdatum_t::top_left, cTitle); y += g.fh(1) + 2; }   // "El Al" for ELY382
  if (model) { g.text(model, X, y, 1, textdatum_t::top_left, cVal); y += g.fh(1) + 2; }                 // "Airbus A320" for A320
  flightinfo::Info fi;
  const bool haveFi = flightinfo::get(p.flight, p.hex, fi);
  const bool faOk = haveFi && fi.fa == flightinfo::FA_OK;
  {   // origin > destination: FlightAware's when it answered, else adsbdb.com's (both looked up when the aircraft was selected)
    adsb::RouteInfo ri;
    const adsb::RouteState rs = adsb::route(p.flight, ri);
    char rb[64];
    if (faOk && fi.oCode[0] && fi.dCode[0]) snprintf(rb, sizeof(rb), "%s %.10s  >  %s %.10s", fi.oCode, fi.oCity, fi.dCode, fi.dCity);
    else if (rs == adsb::ROUTE_KNOWN) snprintf(rb, sizeof(rb), "%s %.10s  >  %s %.10s", ri.oCode, ri.oCity, ri.dCode, ri.dCity);
    else rb[0] = 0;
    if (rb[0]) { g.text(rb, X, y, 1, textdatum_t::top_left, cTitle2); y += g.fh(1) + 2; }
    else if (rs == adsb::ROUTE_PENDING) { g.text("Route: looking up...", X, y, 1, textdatum_t::top_left, cDim); y += g.fh(1) + 2; }
  }
  if (faOk && (fi.dep || fi.eta)) {   // take-off and arrival times in local time; "~" = estimated, "sch" = only scheduled
    char tb[64];
    if (fi.dep) {
      snprintf(tb, sizeof(tb), "%s %s%s%s", fi.depKind == flightinfo::T_ACTUAL ? "Took off" : "Take-off", fi.depKind == flightinfo::T_ESTIMATED ? "~" : "", clockStr(fi.dep).c_str(), fi.depKind == flightinfo::T_SCHEDULED ? "  (sch)" : "");
      g.text(tb, X, y, 1, textdatum_t::top_left, cCall); y += g.fh(1) + 2;
    }
    if (fi.eta) {
      int n = snprintf(tb, sizeof(tb), "%s %s%s", fi.etaKind == flightinfo::T_ACTUAL ? "Landed" : "ETA", clockStr(fi.eta).c_str(), fi.etaKind == flightinfo::T_SCHEDULED ? "  (sch)" : "");
      if (fi.etaKind != flightinfo::T_ACTUAL && fi.delayMin != INT16_MIN && abs(fi.delayMin) >= 5) snprintf(tb + n, sizeof(tb) - n, "  (%+d min)", (int)fi.delayMin);
      g.text(tb, X, y, 1, textdatum_t::top_left, cCall); y += g.fh(1) + 2;
    }
    if (fi.status[0]) { g.text(fi.status, X, y, 1, textdatum_t::top_left, cDim); y += g.fh(1) + 2; }
    if (fi.progress >= 0 && fi.progress <= 100) {   // how far along the flight is
      g.rect(X, y, 150, 7, cLMid);
      if (fi.progress > 0) g.frect(X + 1, y + 1, std::max(1, 148 * fi.progress / 100), 5, cTitle);
      char pb[8]; snprintf(pb, sizeof(pb), "%d%%", (int)fi.progress);
      g.text(pb, X + 158, y - 3, 1, textdatum_t::top_left, cDim);
      y += 11;
    }
  } else if (haveFi && fi.fa == flightinfo::FA_PENDING) { g.text("FlightAware: looking up...", X, y, 1, textdatum_t::top_left, cDim); y += g.fh(1) + 2; }
  else if (haveFi && fi.fa == flightinfo::FA_ERROR) { char eb[60]; snprintf(eb, sizeof(eb), "FlightAware: %.40s", fi.err); g.text(eb, X, y, 1, textdatum_t::top_left, rgb(255, 130, 70)); y += g.fh(1) + 2; }
  y += 3;
  char b[64], alt[12], trk[8], hx[8];
  if (p.altFt >= 0) snprintf(alt, sizeof(alt), "%d", units::fromFt(p.altFt, au)); else strcpy(alt, "---");
  if (p.track >= 0) snprintf(trk, sizeof(trk), "%03d", (int)lroundf(p.track) % 360); else strcpy(trk, "---");
  for (int i = 0; i < 7; i++) hx[i] = toupper((unsigned char)p.hex[i]);
  hx[7] = 0;
  snprintf(b, sizeof(b), "Alt %s %s   Spd %d %s", alt, units::name(au), units::fromKt(p.gs, su), units::name(su));
  g.text(b, X, y, 1, textdatum_t::top_left, cVal);
  y += g.fh(1) + 2;
  snprintf(b, sizeof(b), "Dist %d %s   Brg %03d   Trk %s", (int)lroundf(units::fromKm(p.distKm, du)), units::name(du), (int)lroundf(p.bearing) % 360, trk);
  g.text(b, X, y, 1, textdatum_t::top_left, cVal);
  y += g.fh(1) + 2;
  snprintf(b, sizeof(b), "%s%s   ICAO %s", src && src->reg[0] ? "Reg " : "", src && src->reg[0] ? src->reg : "", hx);
  g.text(b, X, y, 1, textdatum_t::top_left, cVal2);
  y += g.fh(1) + 6;
  if (gPhotoOn && y + gPhotoH + 4 + 2 * g.fh(1) <= FOOT_Y) {   // the picture, a gap and the two credit lines must end above the footer   // Planespotters photo with the photographer's credit that the service asks for
    const int px = (INFO_W - gPhotoW) / 2;
    if (flightinfo::drawPhoto(g.s, px - g.ox, y - g.oy, p.flight, p.hex)) {
      g.rect(px - 1, y - 1, gPhotoW + 2, gPhotoH + 2, cLMid);
      char who[40];
      snprintf(who, sizeof(who), "Photo: %.22s", gPhotoWho[0] ? gPhotoWho : "unknown");
      g.text(who, INFO_W / 2, y + gPhotoH + 4, 1, textdatum_t::top_center, cVal2);
      g.text("planespotters.net", INFO_W / 2, y + gPhotoH + 4 + g.fh(1), 1, textdatum_t::top_center, cDim);
    }
  }
}

// SD card status: a small card outline. Palette colour = logging fine, grey = off, orange = no card or a write problem, amber = ejected.
void paintSdIcon(Ctx &g, int x, int y) {
  const sdlog::State st = sdlog::status().state;
  const uint16_t c = st == sdlog::S_OK ? cTitle : st == sdlog::S_OFF ? cDim : st == sdlog::S_EJECTED ? rgb(255, 200, 0) : rgb(255, 130, 70);
  g.line(x - 8, y - 11, x + 2, y - 11, c); g.line(x + 2, y - 11, x + 8, y - 5, c); g.line(x + 8, y - 5, x + 8, y + 11, c);
  g.line(x + 8, y + 11, x - 8, y + 11, c); g.line(x - 8, y + 11, x - 8, y - 11, c);
  for (int k = 0; k < 3; k++) g.vline(x - 4 + k * 4, y - 9, 6, c);   // contacts
  if (st == sdlog::S_NO_CARD || st == sdlog::S_ERROR) { g.line(x - 5, y + 1, x + 5, y + 9, c); g.line(x + 5, y + 1, x - 5, y + 9, c); }   // cross
  else if (st == sdlog::S_OFF) g.line(x - 9, y + 12, x + 9, y - 12, c);                                                                  // slash
  else if (st == sdlog::S_EJECTED) { g.ftri(x, y + 1, x - 4, y + 7, x + 4, y + 7, c); }                                                   // eject arrow
}

void paintHud(Ctx &g) {
  paintSdIcon(g, 282, 22);
  const bool diag = settings::get().showDiag;
  uint32_t age = (millis() - lastOkMs) / 1000;
  int y = 8;
  if (gEmergIdx >= 0) {   // an aircraft squawks 7500 / 7600 / 7700: a blinking banner with who and where
    const Plot &e = plots[gEmergIdx];
    const bool on = (millis() / 500) & 1;
    char t1[32], t2[48];
    snprintf(t1, sizeof(t1), "%s %s", emergencyCode(e.emerg), emergencyName(e.emerg));
    const int bh = g.fh(2) + 8;
    if (on) g.frect(12, y, 238, bh, cEmerg); else g.rect(12, y, 238, bh, cEmerg);
    g.text(t1, 18, y + 4, 2, textdatum_t::top_left, on ? (uint16_t)TFT_BLACK : cEmerg, on ? cEmerg : (uint16_t)TFT_BLACK);
    y += bh + 3;
    const auto du = (units::Dist)settings::get().dist;
    snprintf(t2, sizeof(t2), "%s  %d %s  brg %03d%s", labelOf(e), (int)lroundf(units::fromKm(e.distKm, du)), units::name(du), (int)lroundf(e.bearing) % 360, gEmergCount > 1 ? "  (+more)" : "");
    g.text(t2, 12, y, 1, textdatum_t::top_left, cEmerg);
    y += g.fh(1) + 4;
  }
  if (dataOk) {
    g.text("Live ADS-B", 12, y, 2, textdatum_t::top_left, cTitle2);
    y += g.fh(2) + 2;
    g.text("Aircraft: " + String(nPlots), 12, y, 2, textdatum_t::top_left, cBright);
    y += g.fh(2) + 6;
    if (gFailStreak > 0 || age >= 12) {  // data is getting old: say why (a warning, shown even with the diagnostics hidden)
      g.text(String("Stale: ") + (gStatus[0] && strcmp(gStatus, "OK") ? gStatus : "no response") + " (" + String(age) + " s)", 12, y, 1, textdatum_t::top_left, rgb(255, 130, 70));
      y += g.fh(1) + 3;
    } else if (diag) {
      g.text("Data age " + String(age) + " s" + (adsb::busy() ? "  -  updating" : ""), 12, y, 1, textdatum_t::top_left, cDim);
      y += g.fh(1) + 3;
    }
  } else {
    bool connecting = !gStatus[0] || !strcmp(gStatus, "Starting");
    g.text(connecting ? "Connecting" : "No Data", 12, y, 2, textdatum_t::top_left, rgb(255, 80, 80));
    y += g.fh(2) + 6;
    g.text(gStatus[0] ? gStatus : "Starting", 12, y, 1, textdatum_t::top_left, rgb(255, 80, 80));
    y += g.fh(1) + 3;
  }
  if (wifimgr::apActive()) { g.text("Setup Wi-Fi is on: Settings > Wi-Fi", 12, y, 1, textdatum_t::top_left, cTitle); y += g.fh(1) + 3; }   // no usable network: say where to connect
  if (diag) g.text(String(gModeLabel) + "  " + kThemes[gTheme].name + (gPaused ? "  PAUSED" : ""), 12, y, 1, textdatum_t::top_left, gPaused ? cTitle : cDim);
  else if (gPaused) g.text("PAUSED", 12, y, 1, textdatum_t::top_left, cTitle);   // the user's own action stays visible
}

void paintBars(Ctx &g) {  // aircraft count per fifth of the range
  const int bins = 5;
  int counts[bins] = {0};
  for (int i = 0; i < nPlots; i++) counts[constrain((int)(plots[i].distKm / (RANGE_KM / bins)), 0, bins - 1)]++;
  const int startX = 395, baseY = 62, barW = 10, barGap = 14, tickH = 3, tickGap = 2, maxTicks = 8;
  g.text("By Range", startX, 12, 1, textdatum_t::top_left, cTitle2);
  for (int b = 0; b < bins; b++) {
    int x = startX + b * barGap, c = min(counts[b], maxTicks);
    for (int t = 0; t < c; t++) g.frect(x, baseY - t * (tickH + tickGap), barW, tickH, cBright);
    g.hline(x, baseY + 7, barW, cLDark);
  }
  const auto du = (units::Dist)settings::get().dist;
  g.text(String((int)lroundf(units::fromKm(RANGE_KM / 5, du))), startX - 1, baseY + 14, 1, textdatum_t::top_left, cGreen);
  g.text(String((int)lroundf(units::fromKm(RANGE_KM * 3 / 5, du))), startX + 2 * barGap - 1, baseY + 14, 1, textdatum_t::top_left, cGreen);
  g.text(String((int)lroundf(units::fromKm(RANGE_KM, du))), startX + 4 * barGap - 4, baseY + 14, 1, textdatum_t::top_left, cGreen);
}

// On-screen buttons: zoom in / zoom out / pause / settings. Drawn on top of everything in their corner tiles.
void paintButton(Ctx &g, Button b) {
  const int cx = kBtnPos[b].x, cy = kBtnPos[b].y;
  const bool down = gPressed == b;
  const settings::Settings &S = settings::get();
  const bool canIn = S.rangeStep > 0, canOut = S.rangeStep < units::RANGE_STEPS - 1;
  g.fcircle(cx, cy, BTN_R, down ? cLDark : TFT_BLACK);
  g.circle(cx, cy, BTN_R, down ? cLBright : cLMid);
  g.circle(cx, cy, BTN_R - 1, down ? cLBright : cLMid);
  uint16_t c = cLBright;
  switch (b) {
    case BTN_ZOOM_IN:
      if (!canIn) c = cLDim;
      g.frect(cx - 9, cy - 1, 19, 3, c);
      g.frect(cx - 1, cy - 9, 3, 19, c);
      break;
    case BTN_ZOOM_OUT:
      if (!canOut) c = cLDim;
      g.frect(cx - 9, cy - 1, 19, 3, c);
      break;
    case BTN_PAUSE:
      if (gPaused) g.ftri(cx - 5, cy - 9, cx - 5, cy + 9, cx + 9, cy, c);
      else { g.frect(cx - 7, cy - 8, 5, 17, c); g.frect(cx + 3, cy - 8, 5, 17, c); }
      break;
    case BTN_SETTINGS:
      for (int k = 0; k < 8; k++) {   // gear: eight teeth around a ring with a hub
        float a = k * (float)M_PI / 4;
        g.line(cx + cosf(a) * 8, cy + sinf(a) * 8, cx + cosf(a) * 13, cy + sinf(a) * 13, c);
        g.line(cx + cosf(a) * 8 + 1, cy + sinf(a) * 8, cx + cosf(a) * 13 + 1, cy + sinf(a) * 13, c);
      }
      g.circle(cx, cy, 8, c);
      g.circle(cx, cy, 3, c);
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- tile painting
void paintTileRect(const Rect &t) {  // t is at most TILE_W x TILE_H
  int w = t.x1 - t.x0, h = t.y1 - t.y0;
  tile.setClipRect(0, 0, w, h);
  bg.pushSprite(&tile, -t.x0, -t.y0);
  Ctx g{&tile, t.x0, t.y0, kDejaVu};  // radar area: fixed DejaVu, independent of the table's font theme
  if (hitR(t, wCur)) paintWedge(g, t);
  paintTrack(g, t);
  paintAircraft(g, t);
  if (hitR(t, HUD_RECT)) paintHud(g);
  if (hitR(t, BARS_RECT)) paintBars(g);
  for (int b = BTN_ZOOM_IN; b <= BTN_SETTINGS; b++) if (hitR(t, btnRect((Button)b))) paintButton(g, (Button)b);
  tile.clearClipRect();
  gDisp->setClipRect(t.x0, t.y0, w, h);
  int64_t tPush = esp_timer_get_time();
  tile.pushSprite(gDisp, t.x0, t.y0);  // one sequential burst of rows
  gDisp->clearClipRect();
  tilesThisFrame++;
  noteRect(t);
  if (!display::doubleBuffered() && display::lastVsyncUs()) {  // is the scan inside this tile's rows (or the ~20-line bounce lookahead before it)?
    float beamRow = ((tPush - display::lastVsyncUs()) - 2000) / 61.2f;  // active video starts ~2 ms after vsync, 61 us per line
    gPushedTiles = gPushedTiles + 1;
    if (beamRow >= t.y0 - 20 && beamRow <= t.y1 + 5) gRiskTiles = gRiskTiles + 1;
  }
}

void paintRect(const Rect &r) {
  for (int y = r.y0; y < r.y1; y += TILE_H)
    for (int x = r.x0; x < r.x1; x += TILE_W) paintTileRect({(int16_t)x, (int16_t)y, (int16_t)min<int>(x + TILE_W, r.x1), (int16_t)min<int>(y + TILE_H, r.y1)});
}

// ---------------------------------------------------------------- info panel (repainted in bands)
// "Time (UTC+1)": the offset currently in force (DST included), from the configured timezone.
String utcLabel() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";  // clock not set yet
  struct tm lt, gt;
  localtime_r(&now, &lt);
  gmtime_r(&now, &gt);
  int off = (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
  int dd = lt.tm_yday - gt.tm_yday;
  if (dd > 1) dd = -1; else if (dd < -1) dd = 1;  // year wrap
  off += dd * 1440;
  if (off == 0) return " (UTC)";
  char buf[20];
  int a = abs(off);
  if (a % 60) snprintf(buf, sizeof(buf), " (UTC%c%d:%02d)", off < 0 ? '-' : '+', a / 60, a % 60);
  else snprintf(buf, sizeof(buf), " (UTC%c%d)", off < 0 ? '-' : '+', a / 60);
  return buf;
}

void paintInfoBand(int y0) {
  Ctx g{&band, 0, y0};
  band.fillSprite(TFT_BLACK);
  g.rect(0, 0, INFO_W, INFO_H, cLMid);
  const settings::Settings &S = settings::get();
  if (gSelIdx >= 0 && gSelIdx < nPlots) paintCardPanel(g, y0);
  else {
  g.text("Detected Aircraft", INFO_W / 2, 8, 2, textdatum_t::top_center, cTitle);

  // column titles (numbers are right-aligned under their titles)
  g.text("Call", infoX[0], infoHdrY, 1, textdatum_t::top_left, cTitle2);
  const auto du = (units::Dist)S.dist; const auto au = (units::Alt)S.alt; const auto su = (units::Spd)S.spd;
  g.text(infoCompact ? String("Dist") : String("Dist ") + units::name(du), infoX[1] + infoW[1], infoHdrY, 1, textdatum_t::top_right, cTitle2);
  g.text(infoCompact ? String("Alt") : String("Alt ") + units::name(au), infoX[2] + infoW[2], infoHdrY, 1, textdatum_t::top_right, cTitle2);
  g.text(infoCompact ? String("Spd") : String("Spd ") + units::name(su), infoX[3] + infoW[3], infoHdrY, 1, textdatum_t::top_right, cTitle2);
  if (infoShowType) g.text("Type", infoX[4], infoHdrY, 1, textdatum_t::top_left, cTitle2);
  g.hline(8, infoLineY, INFO_W - 16, cLMid);

  int y = infoRow0;
  for (int row = 0; row < infoRows && infoIdx[row] != -1; row++, y += ROW_H) {
    if (y + ROW_H < y0 || y > y0 + BAND_H) continue;
    const Plot &p = plots[infoIdx[row]];
    uint16_t dot = night::active() ? rgb(200, 30, 30) : p.altFt >= 30000 ? rgb(0, 180, 255) : p.altFt >= 10000 ? rgb(0, 255, 35) : p.altFt >= 0 ? rgb(255, 220, 0) : rgb(255, 40, 40);
    if (infoIdx[row] == gSelIdx) g.rect(4, y - 2, INFO_W - 8, ROW_H - 2, cTitle);   // the selected aircraft
    if (p.emerg) g.rect(4, y - 2, INFO_W - 8, ROW_H - 2, cEmerg);                     // an emergency squawk (also around the selection: it wins)
    g.text(String(labelOf(p)).substring(0, 7), infoX[0], y, 1, textdatum_t::top_left, p.emerg ? cEmerg : cCall);
    g.text(String((int)lroundf(units::fromKm(p.distKm, du))), infoX[1] + infoW[1], y, 1, textdatum_t::top_right, cVal);
    g.text(p.altFt >= 0 ? String(units::fromFt(p.altFt, au)) : String("---"), infoX[2] + infoW[2], y, 1, textdatum_t::top_right, cVal);
    g.text(String(units::fromKt(p.gs, su)), infoX[3] + infoW[3], y, 1, textdatum_t::top_right, cVal);
    if (infoShowType) g.text(p.emerg ? String(emergencyCode(p.emerg)) : String(p.type[0] ? p.type : p.cat).substring(0, 4), infoX[4], y, 1, textdatum_t::top_left, p.emerg ? cEmerg : cVal2);
    g.fcircle(INFO_W - 12, y + 7, 4, dot);
    g.hline(10, y + ROW_H - 4, INFO_W - 20, cLDark);
  }
  if (nPlots == 0) g.text(dataOk ? "No aircraft in range" : "Waiting for data", INFO_W / 2, infoRow0 + 30, 1, textdatum_t::top_center, cDim);
  }   // table / card

  // footer: location on one line, local time (with its UTC offset) on the next
  g.hline(10, FOOT_Y, INFO_W - 20, cLDark);
  g.text(String(S.place) + "   " + String(S.lat, 4) + ", " + String(S.lon, 4), 10, FOOT_Y + 6, 1, textdatum_t::top_left, cCall);
  struct tm t;
  g.text("Time" + utcLabel(), 10, 446, 1, textdatum_t::middle_left, cTitle);
  if (getLocalTime(&t, 0)) {
    char s[16];
    if (S.clock24) {
      snprintf(s, sizeof(s), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
      g.text(s, INFO_W - 10, 446, 5, textdatum_t::middle_right, cCall);
    } else {  // 12 h: digits plus a small AM/PM at the right edge
      int h = t.tm_hour % 12;
      snprintf(s, sizeof(s), "%d:%02d:%02d", h == 0 ? 12 : h, t.tm_min, t.tm_sec);
      g.fh(1);
      int wAp = g.s->textWidth("PM");
      g.text(t.tm_hour >= 12 ? "PM" : "AM", INFO_W - 10, 452, 1, textdatum_t::middle_right, cTitle);
      g.text(s, INFO_W - 10 - wAp - 6, 446, 5, textdatum_t::middle_right, cCall);
    }
    lastTimeSec = t.tm_sec;
  } else {
    g.text("--:--:--", INFO_W - 10, 446, 5, textdatum_t::middle_right, cCall);
    lastTimeSec = -2;
  }
  tilesThisFrame++;
  band.pushSprite(gDisp, INFO_X, INFO_Y + y0);
  noteRect({(int16_t)INFO_X, (int16_t)(INFO_Y + y0), (int16_t)(INFO_X + INFO_W), (int16_t)(INFO_Y + y0 + BAND_H)});
}

void rebuildInfoList() {
  for (int &v : infoIdx) v = -1;
  for (int i = 0; i < nPlots; i++)
    for (int j = 0; j < MAX_ROWS; j++)
      if (infoIdx[j] == -1 || plots[i].distKm < plots[infoIdx[j]].distKm) {
        for (int k = MAX_ROWS - 1; k > j; k--) infoIdx[k] = infoIdx[k - 1];
        infoIdx[j] = i;
        break;
      }
}

}  // namespace

// ---------------------------------------------------------------- public API
// Measure the theme's fonts and lay the info-list columns out from them (wide fonts drop the type column).
void computeThemeMetrics() {
  const Theme &th = kThemes[gTheme];
  band.setTextSize(1);
  band.setFont(th.s);
  const int fhS = band.fontHeight();
  auto w = [&](const char *a, const char *b) { return max(band.textWidth(a), band.textWidth(b)); };
  const int gap = 7, right = INFO_W - 26;  // leave room for the altitude dot
  // Preference order: full titles + Type, short titles + Type, full titles without Type, short titles without Type.
  const bool tryCompact[4] = {false, true, false, true}, tryType[4] = {true, true, false, false};
  for (int k = 0; k < 4; k++) {
    const bool c = tryCompact[k];
    const int wCall = w("Call", "AAA000"), wDist = w(c ? "Dist" : "Dist km", "000"), wAlt = w(c ? "Alt" : "Alt ft", "00000"), wSpd = w(c ? "Spd" : "Spd km/h", "000"), wType = w("Type", "A320");
    infoX[0] = 10;                          infoW[0] = wCall;
    infoX[1] = infoX[0] + infoW[0] + gap;   infoW[1] = wDist;
    infoX[2] = infoX[1] + infoW[1] + gap;   infoW[2] = wAlt;
    infoX[3] = infoX[2] + infoW[2] + gap;   infoW[3] = wSpd;
    infoX[4] = infoX[3] + infoW[3] + gap;   infoW[4] = wType;
    infoCompact = c;
    infoShowType = tryType[k];
    int endX = infoShowType ? infoX[4] + wType : infoX[3] + wSpd;
    if (endX <= right) break;
  }
  band.setFont(th.m);
  const int fhM = band.fontHeight();
  // fontHeight() is the line advance, a bit more than the visible glyph: pack the header a little tighter
  infoHdrY = 8 + fhM * 85 / 100 + 4;
  infoLineY = infoHdrY + fhS * 85 / 100 + 2;
  infoRow0 = infoLineY + 5;
  infoRows = min(MAX_ROWS, (FOOT_Y - 4 - infoRow0) / ROW_H);
  Serial.printf("[radar] theme=%s fontH=%d/%d rows=%d type=%d compact=%d cols=%d,%d,%d,%d,%d\n", th.name, fhS, fhM, infoRows, (int)infoShowType, (int)infoCompact, infoX[0], infoX[1], infoX[2], infoX[3], infoX[4]);
}

uint16_t nativePx(uint16_t c) { return gSwapped ? (uint16_t)__builtin_bswap16(c) : c; }

void reselect();

// Colours from the user's palette choices (settings: Look).
void loadPalette() {
  const settings::Settings &S = settings::get();
  auto col = [&](int role, float f) { palette::Rgb v = palette::shade(night::pal(role), f); return rgb(v.r, v.g, v.b); };
  cBright = col(palette::RADAR, palette::kBright);
  cGreen = col(palette::RADAR, palette::kMid);
  cDim = col(palette::RADAR, palette::kDim);
  cDark = col(palette::RADAR, palette::kDark);
  cLBright = col(palette::LINES, palette::kBright);
  cLMid = col(palette::LINES, palette::kMid);
  cLDim = col(palette::LINES, palette::kDim);
  cLDark = col(palette::LINES, palette::kDark);
  cTitle = col(palette::TITLES, palette::kBright);
  cTitle2 = col(palette::TITLES, palette::kMid);
  cCall = col(palette::CALLSIGN, palette::kBright);
  cVal = col(palette::VALUE, palette::kBright);
  cVal2 = col(palette::VALUE, palette::kMid);
  cMap = night::active() ? rgb(120, 25, 25) : rgb(150, 150, 150);    // map overlay: light grey, so it stands apart from the radar colour. Coastlines and airports ...
  cMapB = night::active() ? rgb(80, 15, 15) : rgb(100, 100, 100);    // ... and land borders, a notch darker
  cEmerg = rgb(255, 40, 40);  // emergency squawk: always red, whatever the palette or night mode
  nGrid = nativePx(cDark);
}

void setTrailDeg(int deg) {
  gTrailDeg = constrain(deg, 5, TRAIL_MAX);
  gTrailSteps = max(1, (int)(gTrailDeg * ANGLE_STEPS / 360.0f));
  const palette::Rgb base = palette::shade(night::pal(palette::RADAR), 1.0f);
  for (int d = 0; d <= gTrailSteps; d++) {
    const float f = 0.024f + (1.0f - d / (float)gTrailSteps) * 0.47f;   // 2 % at the tail, 49 % next to the sweep line
    wedgeLut[d] = nativePx(rgb(base.r * f, base.g * f, base.b * f));
  }
  gForceFull = true;  // repaint the radar so the old, longer trail disappears
}
int trailDeg() { return gTrailDeg; }

void setTheme(int idx) {
  gTheme = (idx % THEME_COUNT + THEME_COUNT) % THEME_COUNT;
  computeThemeMetrics();  // only the info table follows the theme; the radar is fixed DejaVu
  for (bool &b : gBandDirty) b = true;
  lastHudSig = 0xFFFFFFFF;
}
int themeIndex() { return gTheme; }
const char *themeName() { return kThemes[gTheme].name; }
String timeZoneLabel() { return utcLabel(); }

void begin(lgfx::LovyanGFX *display) {
  gDisp = display;
  for (int a = 0; a < 360; a++) {
    int len = (a % 10 == 0) ? 13 : (a % 5 == 0) ? 8 : 4;
    float rad = (a - 90) * DEG_TO_RAD, c = cosf(rad), s = sinf(rad);
    tick[a][0] = CX + c * (R - len); tick[a][1] = CY + s * (R - len);
    tick[a][2] = CX + c * R;         tick[a][3] = CY + s * R;
  }
  auto depth = display->getColorDepth();  // same format as the screen, so pushes are plain row copies
  bg.setPsram(true);
  bg.setColorDepth(depth);
  tile.setPsram(false);
  tile.setColorDepth(depth);
  band.setPsram(false);
  band.setColorDepth(depth);
  bool ok[3] = {bg.createSprite(RADAR_W, RADAR_W) != nullptr, tile.createSprite(TILE_W, TILE_H) != nullptr, band.createSprite(INFO_W, BAND_H) != nullptr};
  Serial.printf("[radar] sprites bg=%d tile=%d band=%d psram_free=%uK heap_free=%uK\n", ok[0], ok[1], ok[2], (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned)(ESP.getFreeHeap() / 1024));
  gSwapped = (depth == lgfx::rgb565_2Byte);  // sprite memory is byte-swapped on LovyanGFX's own panel
  loadPalette();
  computeThemeMetrics();
  buildStaticLayer();

  // per-pixel bearing map and trail colours
  angleMap = (uint8_t *)heap_caps_malloc(RADAR_W * RADAR_W, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (angleMap) {
    for (int y = 0; y < RADAR_W; y++)
      for (int x = 0; x < RADAR_W; x++) {
        int dx = x - CX, dy = y - CY;
        if (dx * dx + dy * dy > (R + 1) * (R + 1)) { angleMap[y * RADAR_W + x] = 255; continue; }
        float b = atan2f((float)dx, (float)-dy) * (180.0f / M_PI);
        if (b < 0) b += 360.0f;
        angleMap[y * RADAR_W + x] = (uint8_t)min(254, (int)(b * (ANGLE_STEPS / 360.0f)));
      }
  } else {
    Serial.println("[radar] angleMap alloc FAILED");
  }
  gSwapped = (depth == lgfx::rgb565_2Byte);  // sprite memory is byte-swapped on LovyanGFX's own panel
  nBlack = nativePx(0);
  nGrid = nativePx(cDark);
  setTrailDeg(TRAIL_MAX);

  gDisp->fillScreen(TFT_BLACK);
  for (int &v : infoIdx) v = -1;
}

void setData(const adsb::Snapshot &s) {
  gSnap = &s;
  lastHttp = s.httpCode;
  strlcpy(gStatus, s.status, sizeof(gStatus));
  gFailStreak = s.failStreak;
  lastHudSig = 0xFFFFFFFF;  // status text may have changed
  if (s.ok) {
    // old on-screen aircraft must be erased: remember where they were, then forget their state
    for (int i = 0; i < nPlots; i++)
      if (prevSt[i].vis) addDirty(acRect(prevSt[i], lblLen(plots[i])));
    for (auto &p : prevSt) p.vis = false;
    dataOk = true;
    lastOkMs = s.fetchedAtMs;
    nPlots = 0;
    for (int i = 0; i < s.n && nPlots < MAX_AIRCRAFT; i++) {
      const adsb::Aircraft &a = s.ac[i];
      float d = distanceKm(gHomeLat, gHomeLon, a.lat, a.lon);
      if (d > RANGE_KM) continue;
      Plot &p = plots[nPlots++];
      strlcpy(p.hex, a.hex, sizeof(p.hex));
      strlcpy(p.flight, a.flight, sizeof(p.flight));
      strlcpy(p.type, a.type, sizeof(p.type));
      strlcpy(p.cat, a.cat, sizeof(p.cat));
      p.emerg = emergencyOf(a.squawk);
      p.lat0 = a.lat;
      p.lon0 = a.lon;
      p.distKm = d;
      p.bearing = bearingDeg(gHomeLat, gHomeLon, a.lat, a.lon);
      p.altFt = a.altFt;
      p.gs = a.gs;
      p.track = a.track;
    }
    if (gDebugEmerg && nPlots) {   // dev: the nearest aircraft squawks the chosen code
      int nearest = 0;
      for (int i = 1; i < nPlots; i++) if (plots[i].distKm < plots[nearest].distKm) nearest = i;
      plots[nearest].emerg = gDebugEmerg;
    }
    scanEmergencies();
    night::noteEmergency(gEmergCount > 0);   // an emergency overrides night mode: normal colours, full brightness
    rebuildInfoList();
    reselect();
  }
  infoDataDirty = true;
  barsDirty = true;
}

void applyLocation() {
  const settings::Settings &S = settings::get();
  RANGE_KM = settings::rangeKm();
  gHomeLat = S.lat;
  gHomeLon = S.lon;
  for (auto &p : prevSt) p.vis = false;
  nPlots = 0;
  gSelHex[0] = 0;
  reselect();
  rebuildInfoList();
  buildStaticLayer();  // ring labels
  gForceFull = true;
  for (bool &b : gBandDirty) b = true;
  barsDirty = true;
  lastHudSig = 0xFFFFFFFF;
}

// Range step changed: rescale the rings and drop aircraft that are now outside; the caller triggers a fetch for any newly covered area.
void applyRange() {
  RANGE_KM = settings::rangeKm();
  int k = 0;
  for (int i = 0; i < nPlots; i++) if (plots[i].distKm <= RANGE_KM) plots[k++] = plots[i];
  nPlots = k;
  for (auto &p : prevSt) p.vis = false;
  reselect();
  rebuildInfoList();
  projectTrack();   // the flown track follows the new scale
  buildStaticLayer();  // ring labels
  gForceFull = true;
  for (bool &b : gBandDirty) b = true;
  barsDirty = true;
  lastHudSig = 0xFFFFFFFF;
}

void refreshUnits() {
  buildStaticLayer();  // ring labels carry the distance unit
  gForceFull = true;
  for (bool &b : gBandDirty) b = true;
  barsDirty = true;
}

int infoHeaderHit(int x, int y) {
  if (gSelIdx >= 0) return 0;   // the card has no unit headings
  if (y < INFO_Y + infoHdrY - 6 || y > INFO_Y + infoLineY + 2) return 0;
  for (int k = 1; k <= 3; k++)
    if (x >= INFO_X + infoX[k] - 8 && x <= INFO_X + infoX[k] + infoW[k] + 8) return k;
  return 0;
}

int infoClockHit(int x, int y) {
  if (x < INFO_X || y < INFO_Y + 416) return 0;
  return x < INFO_X + 150 ? 1 : 2;
}

void applyPalette() {
  loadPalette();
  setTrailDeg(gTrailDeg);   // trail colour follows the radar colour
  buildStaticLayer();
  gForceFull = true;
  for (bool &b : gBandDirty) b = true;
  barsDirty = true;
  lastHudSig = 0xFFFFFFFF;
}

void debugEmergency(int code) {
  gDebugEmerg = (uint8_t)constrain(code, 0, 3);
  for (int i = 0; i < nPlots; i++) {
    const adsb::Aircraft *src = sourceOf(plots[i]);
    plots[i].emerg = src ? emergencyOf(src->squawk) : 0;
  }
  if (gDebugEmerg && nPlots) {
    int nearest = 0;
    for (int i = 1; i < nPlots; i++) if (plots[i].distKm < plots[nearest].distKm) nearest = i;
    plots[nearest].emerg = gDebugEmerg;
  }
  scanEmergencies();
  night::noteEmergency(gEmergCount > 0);
  gForceFull = true; infoDataDirty = true; lastHudSig = 0xFFFFFFFF;
}

uint32_t dataAgeS() { return lastOkMs ? (millis() - lastOkMs) / 1000 : UINT32_MAX; }

int emergencies(char *buf, size_t cap) {
  if (buf && cap) buf[0] = 0;
  if (gEmergIdx < 0) return 0;
  if (buf && cap) { const Plot &e = plots[gEmergIdx]; snprintf(buf, cap, "%s %s", emergencyCode(e.emerg), labelOf(e)); }
  return gEmergCount;
}

void invalidate() { gWipe = true; }
void refreshMap() { buildStaticLayer(); gForceFull = true; }
void refreshHud() { lastHudSig = 0xFFFFFFFF; }
LGFX_Sprite *scratchTile() { return &tile; }

// ---- on-screen buttons
Button buttonAt(int x, int y) {
  Button best = BTN_NONE;
  long bd = (long)BTN_HIT * BTN_HIT + 1;
  for (int b = BTN_ZOOM_IN; b <= BTN_SETTINGS; b++) {
    long dx = x - kBtnPos[b].x, dy = y - kBtnPos[b].y, d = dx * dx + dy * dy;
    if (d < bd) { bd = d; best = (Button)b; }
  }
  return best;
}

void setButtonPressed(Button b) {
  if (b == gPressed) return;
  if (gPressed != BTN_NONE) addDirty(btnRect(gPressed));
  if (b != BTN_NONE) addDirty(btnRect(b));
  gPressed = b;
}

void setPaused(bool p) {
  if (p == gPaused) return;
  gPaused = p;
  addDirty(btnRect(BTN_PAUSE));
  lastHudSig = 0xFFFFFFFF;
}
bool paused() { return gPaused; }

// ---- selection (tap an aircraft on the radar or a row of the table)
void reselect() {
  const int old = gSelIdx;
  gSelIdx = -1;
  if (gSelHex[0]) {
    for (int i = 0; i < nPlots; i++) if (!strcmp(plots[i].hex, gSelHex)) { gSelIdx = i; break; }
    if (gSelIdx < 0) gSelHex[0] = 0;   // it left the area
  }
  if (old != gSelIdx) { lastHudSig = 0xFFFFFFFF; for (bool &b : gBandDirty) b = true; }
  if (strcmp(gSelHex, gFiHex)) {   // another aircraft (or none): its details are not these
    strlcpy(gFiHex, gSelHex, sizeof(gFiHex));
    if (!gSelHex[0]) flightinfo::cancel();
    if (gTrkN) { gTrkN = 0; gForceFull = true; }
    gFiVer = 0xFFFFFFFF;
  }
}

static void select(int idx) {
  if (idx >= 0 && idx < nPlots && plots[idx].hex[0]) strlcpy(gSelHex, plots[idx].hex, sizeof(gSelHex)); else gSelHex[0] = 0;
  reselect();
  if (gSelIdx >= 0) {   // airline-style callsigns (three letters, then a digit) have scheduled routes
    const char *cs = plots[gSelIdx].flight;
    if (strlen(cs) >= 4 && isupper((unsigned char)cs[0]) && isupper((unsigned char)cs[1]) && isupper((unsigned char)cs[2]) && isdigit((unsigned char)cs[3])) adsb::requestRoute(cs);
    const adsb::Aircraft *src = sourceOf(plots[gSelIdx]);
    flightinfo::request(cs, plots[gSelIdx].hex, src ? src->reg : "");   // departure, ETA, track, photo (only if the user enabled it)
  }
}

void clearSelection() { gSelHex[0] = 0; reselect(); }
bool hasSelection() { return gSelIdx >= 0; }

bool selectAt(int x, int y) {
  if (x < 0 || x >= RADAR_W) return false;
  int best = -1;
  long bd = 34L * 34L;
  for (int i = 0; i < nPlots; i++) {
    if (!st[i].vis) continue;
    long dx = x - st[i].x, dy = y - st[i].y, d = dx * dx + dy * dy;
    if (d < bd) { bd = d; best = i; }
  }
  if (best >= 0) { if (best == gSelIdx) clearSelection(); else select(best); return true; }
  if (gSelIdx >= 0) { clearSelection(); return true; }
  return false;
}

bool selectRowAt(int x, int y) {
  if (x < INFO_X || x >= INFO_X + INFO_W) return false;
  const int row = (y - INFO_Y - infoRow0 + 2) / ROW_H;
  if (gSelIdx >= 0 && y < INFO_Y + FOOT_Y) { clearSelection(); return true; }   // the card is showing: tapping it brings the table back
  if (y < INFO_Y + infoRow0 - 2 || row < 0 || row >= infoRows || infoIdx[row] == -1) return false;
  if (infoIdx[row] == gSelIdx) clearSelection(); else select(infoIdx[row]);
  return true;
}

void setVerify(bool on) { gVerify = on; }
void setTestPattern(bool on) { if (gTest && !on) gWipe = true; gTest = on; }
bool testPatternOn() { return gTest; }

void setModeLabel(const char *label) { gModeLabel = label; lastHudSig = 0xFFFFFFFF; }

// Tear test: two bars sweeping across the whole screen at constant speed, drawn full-frame every frame. Any horizontal break in a bar
// is tearing in the display path itself (not in the radar drawing).
static void testFrame(float dt) {
  static float x = 0;
  x += dt * 480.0f;
  if (x >= 800) x -= 800;
  const bool dbl = display::doubleBuffered();
  if (dbl) { display::waitFlipped(100); gDisp = display::backFb(); }
  else display::waitVsync(40);
  gDisp->fillScreen(TFT_BLACK);
  const int w = 48;
  for (int k = 0; k < 2; k++) {
    int bx = ((int)x + k * 400) % 800;
    int w1 = min(w, 800 - bx);
    gDisp->fillRect(bx, 0, w1, 480, TFT_WHITE);
    if (w1 < w) gDisp->fillRect(0, 0, w - w1, 480, TFT_WHITE);  // wrap-around
  }
  gDisp->setTextColor(TFT_RED, TFT_WHITE);
  gDisp->setFont(&lgfx::fonts::DejaVu18);
  gDisp->setTextDatum(textdatum_t::middle_center);
  gDisp->drawString("Tear test - tap anywhere to exit", 400, 240);
  if (dbl) display::flip();
}

void frame(float dt) {
  if (gTest) { testFrame(dt); return; }
  if (gWipe) {  // leave the test / settings: wipe both buffers (they hold something else, and the margins are never repainted by the radar), then repaint everything
    for (int i = 0; i < 2; i++) {
      if (display::doubleBuffered()) { display::waitFlipped(100); gDisp = display::backFb(); }
      gDisp->fillScreen(TFT_BLACK);
      if (display::doubleBuffered()) display::flip();
    }
    if (display::doubleBuffered()) display::waitFlipped(100);
    gWipe = false;
    firstFrame = true;
    nDirty = 0;
    nLastR = nCurR = 0;
    for (bool &b : gBandDirty) b = true;
    barsDirty = true;
    lastHudSig = 0xFFFFFFFF;
  }
  uint32_t t0 = micros();
  tilesThisFrame = 0;

  if (dataOk && millis() - lastOkMs > 30000) {  // stale: drop aircraft, say so in the HUD
    dataOk = false;
    for (int i = 0; i < nPlots; i++) if (prevSt[i].vis) addDirty(markerBox(prevSt[i].x, prevSt[i].y));
    for (auto &p : prevSt) p.vis = false;
    nPlots = 0;
    scanEmergencies();
    gSelHex[0] = 0;
    reselect();
    rebuildInfoList();
    infoDataDirty = true;
    barsDirty = true;
  }

  // Dead reckoning: between fetches (5 s apart) move each aircraft along its track at its ground speed.
  static uint32_t lastDrMs = 0;
  if (dataOk && millis() - lastDrMs >= 500) {
    lastDrMs = millis();
    const float dtS = min(25.0f, (millis() - lastOkMs) / 1000.0f);
    for (int i = 0; i < nPlots; i++) {
      Plot &p = plots[i];
      if (p.track < 0 || p.gs < 5) continue;
      const double dNm = p.gs * dtS / 3600.0, tr = p.track * M_PI / 180.0;
      const double lat = p.lat0 + dNm * cos(tr) / 60.0;
      const double lon = p.lon0 + dNm * sin(tr) / (60.0 * cos(p.lat0 * M_PI / 180.0));
      p.distKm = distanceKm(gHomeLat, gHomeLon, lat, lon);
      p.bearing = bearingDeg(gHomeLat, gHomeLon, lat, lon);
    }
  }

  float prevSweep = sweepAngle;
  if (!gPaused) sweepAngle += dt * SWEEP_DEG_PER_S;
  if (sweepAngle >= 360) sweepAngle -= 360;

  // ---- 1. what changed?
  if (flightinfo::version() != gFiVer) refreshFlightInfo();   // before the wedge: its erase rectangles are queued first
  wCur = wedgeRect(sweepAngle);
  Rect wU = gPaused ? Rect{0, 0, 0, 0} : uniR(wedgeRect(prevSweep), wCur);   // paused: the wedge stays where it is
  if (firstFrame || gForceFull) { wU = {0, 0, RADAR_W, RADAR_W}; gForceFull = false; }
  addDirty(wU);
  const int wedgeEnd = nDirty;  // dirty[0..wedgeEnd) = erase rects queued by setData() + the wedge itself

  for (int i = 0; i < nPlots; i++) {
    const Plot &p = plots[i];
    AcState n = {};
    float age = behindSweep(sweepAngle, p.bearing);
    {
      float rr = (p.distKm / RANGE_KM) * R, rad = (p.bearing - 90) * DEG_TO_RAD;
      n.vis = true;
      n.sel = (i == gSelIdx);
      n.x = CX + cosf(rad) * rr;
      n.y = CY + sinf(rad) * rr;
      n.fadeQ = 0;
      while (n.fadeQ < 5 && age >= kFadeAge[n.fadeQ]) n.fadeQ++;
      n.lblQ = 0;
      const int maxLbl = nPlots > 100 ? 1 : nPlots > 60 ? 2 : 4;  // in dense traffic only the freshest callsigns are shown: text is the expensive part
      for (int k = 0; k < maxLbl; k++) if (age < kLabelAge[k]) { n.lblQ = k + 1; break; }
      if (n.sel || p.emerg) { n.fadeQ = 0; n.lblQ = 1; }   // the selected aircraft and any emergency stay bright and labelled
    }
    st[i] = n;
    const AcState &o = prevSt[i];
    if (n.vis == o.vis && n.sel == o.sel && n.x == o.x && n.y == o.y && n.fadeQ == o.fadeQ && n.lblQ == o.lblQ) continue;
    size_t len = lblLen(p);
    Rect ro = o.vis ? acRect(o, len) : Rect{0, 0, 0, 0}, rn = n.vis ? acRect(n, len) : Rect{0, 0, 0, 0};
    addDirty(uniR(ro, rn));  // old and new areas overlap almost completely: one repaint
  }

  uint32_t age = (millis() - lastOkMs) / 1000;
  uint32_t selSig = 0;
  if (gSelIdx >= 0 && gSelIdx < nPlots) { const Plot &sp = plots[gSelIdx]; selSig = 1u + (uint32_t)gSelIdx * 977u + (uint32_t)lroundf(sp.distKm) * 31u + (uint32_t)lroundf(sp.bearing) * 7u + (uint32_t)(sp.altFt + 1) / 100u + (uint32_t)sp.gs + (uint32_t)(sp.track + 1); }
  uint32_t hudSig = (gEmergIdx >= 0 ? 59u + ((millis() / 500) & 1) * 61u + (uint32_t)gEmergIdx * 67u + (uint32_t)gEmergCount * 71u : 0u) + (uint32_t)sdlog::status().state * 43u + (wifimgr::apActive() ? 23u : 0u) + (gPaused ? 3u : 0u) + (settings::get().showDiag ? 19u : 0u) + (uint32_t)nPlots * 131u + (dataOk ? 7u : 0u) + age * 3u + (adsb::busy() ? 5u : 0u) + (WiFi.status() == WL_CONNECTED ? 11u : 0u) + (uint32_t)(lastHttp + 1000) * 17u + gFailStreak * 29u + (uint8_t)gStatus[0] * 7u + (uint8_t)gStatus[1];
  const uint32_t cardSig = selSig * 101u + flightinfo::version() * 13u + adsb::routeVersion() * 37u;   // the aircraft card (info panel) shows these
  static uint32_t lastCardSig = 0;
  if (cardSig != lastCardSig) { lastCardSig = cardSig; for (bool &b : gBandDirty) b = true; }
  if (firstFrame || hudSig != lastHudSig) addDirty(HUD_RECT);
  lastHudSig = hudSig;
  if (firstFrame || barsDirty) addDirty(BARS_RECT);
  barsDirty = false;


  // ---- 2. decide this frame's painting up front: radar rectangles (top first) and at most two info-panel bands.
  //         With two framebuffers the buffer we paint is a frame behind; it only needs syncing where we are NOT repainting.
  static Rect order[MAX_DIRTY];
  int nOrder = 0;
  for (int i = 0; i < nDirty; i++)
    if (i < wedgeEnd || !insideR(dirty[i], wU)) order[nOrder++] = dirty[i];
  if (nOrder > 150) { order[0] = {0, 0, RADAR_W, RADAR_W}; nOrder = 1; }  // absurdly many changes: one bounded full repaint beats hundreds of tiny tiles
  else {
    mergeRects(order, nOrder);
    long total = 0;
    for (int i = 0; i < nOrder; i++) total += areaR(order[i]);
    if (nOrder > 40 || total > (long)RADAR_W * RADAR_W * 6 / 10) { order[0] = {0, 0, RADAR_W, RADAR_W}; nOrder = 1; }
  }
  std::sort(order, order + nOrder, [](const Rect &a, const Rect &b) { return a.y0 < b.y0; });
  nDirty = 0;

  struct tm tmv;
  bool clockTick = getLocalTime(&tmv, 0) ? (tmv.tm_sec != lastTimeSec) : (lastTimeSec != -2);
  if (infoDataDirty) { for (int b = 0; b < 9; b++) gBandDirty[b] = true; infoDataDirty = false; }
  if (clockTick) gBandDirty[9] = true;
  int bandSel[10], nSel = 0;  // all dirty bands in one frame: with double buffering the table changes atomically (a 2-per-frame trickle showed as a seam sweeping down it)
  for (int b = 0; b < 10; b++)
    if (gBandDirty[b]) { bandSel[nSel++] = b; gBandDirty[b] = false; }

  uint32_t tWait0 = micros();
  const bool dbl = display::doubleBuffered();
  if (dbl) {
    display::waitFlipped(100);  // the old on-screen buffer is only free once the panel has really switched
    gDisp = display::backFb();
    LGFX_Sprite *front = display::frontFb();
    static Rect cover[MAX_DIRTY + 2];
    int nCover = 0;
    for (int i = 0; i < nOrder; i++) cover[nCover++] = order[i];
    for (int k = 0; k < nSel; k++) cover[nCover++] = {(int16_t)INFO_X, (int16_t)(INFO_Y + bandSel[k] * BAND_H), (int16_t)(INFO_X + INFO_W), (int16_t)(INFO_Y + bandSel[k] * BAND_H + BAND_H)};
    for (int i = 0; i < nLastR; i++) {  // last frame's rectangles minus everything we repaint now
      Rect frag[24], next[24];
      int nf = 1;
      frag[0] = lastRects[i];
      for (int c = 0; c < nCover && nf > 0; c++) {
        const Rect &cv = cover[c];
        int nn = 0;
        for (int f = 0; f < nf; f++) {
          const Rect &a = frag[f];
          if (!hitR(a, cv)) { if (nn < 24) next[nn++] = a; continue; }
          Rect parts[4] = {
              {a.x0, a.y0, a.x1, (int16_t)min<int>(a.y1, max<int>(a.y0, cv.y0))},                      // above
              {a.x0, (int16_t)max<int>(a.y0, min<int>(a.y1, cv.y1)), a.x1, a.y1},                      // below
              {a.x0, (int16_t)max<int>(a.y0, cv.y0), (int16_t)min<int>(a.x1, max<int>(a.x0, cv.x0)), (int16_t)min<int>(a.y1, cv.y1)},   // left
              {(int16_t)max<int>(a.x0, min<int>(a.x1, cv.x1)), (int16_t)max<int>(a.y0, cv.y0), a.x1, (int16_t)min<int>(a.y1, cv.y1)}};  // right
          for (const Rect &p : parts) if (!emptyR(p) && nn < 24) next[nn++] = p;
        }
        memcpy(frag, next, sizeof(Rect) * nn);
        nf = nn;
      }
      for (int f = 0; f < nf; f++) {
        gDisp->setClipRect(frag[f].x0, frag[f].y0, frag[f].x1 - frag[f].x0, frag[f].y1 - frag[f].y0);
        front->pushSprite(gDisp, 0, 0);
        gDisp->clearClipRect();
      }
    }
    nLastR = 0;
    if (gVerify) {  // after the sync, the hidden buffer must equal the visible one everywhere we are not about to repaint
      static uint32_t n = 0;
      if (++n % 10 == 0) {
        const uint16_t *fa = (const uint16_t *)front->getBuffer(), *fb = (const uint16_t *)display::backFb()->getBuffer();
        static bool covered[50 * 30];
        memset(covered, 0, sizeof(covered));
        for (int c = 0; c < nCover; c++)
          for (int by = cover[c].y0 / 16; by <= (cover[c].y1 - 1) / 16 && by < 30; by++)
            for (int bx = cover[c].x0 / 16; bx <= (cover[c].x1 - 1) / 16 && bx < 50; bx++) covered[by * 50 + bx] = true;
        int badBlocks = 0, firstX = -1, firstY = -1, badPx = 0;
        for (int by = 0; by < 30; by++)
          for (int bx = 0; bx < 50; bx++) {
            if (covered[by * 50 + bx]) continue;
            int bad = 0;
            for (int y = by * 16; y < by * 16 + 16 && y < 480; y++)
              for (int x = bx * 16; x < bx * 16 + 16 && x < 800; x++) bad += fa[y * 800 + x] != fb[y * 800 + x];
            if (bad) { if (!badBlocks) { firstX = bx * 16; firstY = by * 16; } badBlocks++; badPx += bad; }
          }
        Serial.printf("[verify] hidden vs visible buffer outside repaint areas: %d blocks / %d px differ%s (first at %d,%d)\n", badBlocks, badPx, badBlocks ? "  <-- SYNC BUG" : "  ok", firstX, firstY);
      }
    }
  } else {
    display::waitVsync(40);
  }
  uint32_t tWait1 = micros();

  // ---- 3. paint
  for (int i = 0; i < nOrder; i++) paintRect(order[i]);
  memcpy(prevSt, st, sizeof(AcState) * nPlots);
  firstFrame = false;
  uint32_t tPainted = micros();
  for (int k = 0; k < nSel; k++) paintInfoBand(bandSel[k] * BAND_H);
  if (dbl) {  // show the finished frame; next frame starts by syncing the other buffer from this one
    display::flip();
    memcpy(lastRects, curRects, sizeof(Rect) * nCurR);
    nLastR = nCurR;
  }
  nCurR = 0;
  uint32_t t3 = micros();

  usPaintAvg = (usPaintAvg * 7 + (tPainted - tWait1)) / 8;   // painting after vsync
  usFlushAvg = (usFlushAvg * 7 + (tWait1 - tWait0)) / 8;     // time spent waiting for vsync (slack)
  gStats.frameMs = (t3 - t0) / 1000;
  gStats.usPaint = usPaintAvg;
  gStats.usFlush = usFlushAvg;
  gStats.tiles = tilesThisFrame;
  gStats.aircraft = nPlots;
  gStats.dataOk = dataOk;
}

void tearCounters(uint32_t &atRisk, uint32_t &total) { atRisk = gRiskTiles; total = gPushedTiles; gRiskTiles = 0; gPushedTiles = 0; }
Stats stats() { return gStats; }

void noteFrame() {   // once per displayed frame, whether it was the radar or the settings screen
  static uint32_t fpsT = millis(), frames = 0;
  frames++;
  const uint32_t now = millis();
  if (now - fpsT >= 1000) {
    gStats.fps = frames * 1000.0f / (now - fpsT);
    frames = 0;
    fpsT = now;
  }
}

}  // namespace radar
