// Location sub-screens of the settings UI: city search, coordinates, country and time-zone pickers, the offline world-map picker, and the credits.
#include "ui_internal.h"
#include "config.h"
#include "app.h"
#include "geo.h"
#include "settings.h"
#include "sdlog.h"
#include <math.h>

using namespace gfx;

namespace ui {
namespace in {
namespace {

constexpr int W = 800, H = 480;
Rect R(int x, int y, int w, int h) { return {(int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)(y + h)}; }
Rect grow(Rect r, int m) { return {(int16_t)(r.x0 - m), (int16_t)(r.y0 - m), (int16_t)(r.x1 + m), (int16_t)(r.y1 + m)}; }
bool inR(const Rect &r, int x, int y, int m = 0) { return x >= r.x0 - m && x < r.x1 + m && y >= r.y0 - m && y < r.y1 + m; }

bool containsNoCase(const char *hay, const char *needle) {
  const size_t n = strlen(needle);
  if (!n) return true;
  for (const char *h = hay; *h; h++) {
    size_t k = 0;
    while (k < n && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)needle[k])) k++;
    if (k == n) return true;
  }
  return false;
}

void backToSettings() { setSub(nullptr); }

// ================================================================ result list
class ListScreen : public Screen {
 public:
  void begin(const char *ttl, void (*pickFn)(int), void (*backFn)(), const char *emptyText) {
    strlcpy(title, ttl, sizeof(title));
    onPick = pickFn; onBack = backFn;
    strlcpy(empty, emptyText, sizeof(empty));
    n = 0; top = 0; pressed = P_NONE;
  }
  void add(const char *mainText, const char *subText, int id) {
    if (n >= MAXI) return;
    strlcpy(items[n].mainText, mainText, sizeof(items[n].mainText));
    strlcpy(items[n].subText, subText, sizeof(items[n].subText));
    items[n].id = id;
    n++;
  }
  void show() { setSub(this); }

  void enter() override { pressed = P_NONE; markAll(); }
  void draw(Ctx &g, const Rect &t) override {
    drawHeader(g, t, title);
    if (hitR(t, backRect())) drawBtn(g, backRect(), "Back", pressed == P_BACK);
    if (n == 0 && hitR(t, R(0, 70, W, 300))) g.textFont(F18, empty, W / 2, 200, textdatum_t::middle_center, C.val2);
    for (int r = 0; r < ROWS; r++) {
      const int i = top + r;
      const Rect rr = rowRect(r);
      if (i >= n || !hitR(t, rr)) continue;
      const bool down = pressed == i;
      const uint16_t bg = down ? C.dark : TFT_BLACK;
      if (down) g.frrect(rr.x0, rr.y0, rr.x1 - rr.x0, rr.y1 - rr.y0, 8, C.dark);
      g.hline(rr.x0 + 4, rr.y1 - 1, rr.x1 - rr.x0 - 8, C.dark);
      g.textFont(F18, items[i].mainText, rr.x0 + 14, rr.y0 + 20, textdatum_t::middle_left, C.val, bg);
      if (items[i].subText[0]) g.textFont(F12, items[i].subText, rr.x0 + 14, rr.y0 + 44, textdatum_t::middle_left, C.val2, bg);
    }
    if (hitR(t, R(0, 380, W, 100))) {
      drawBtn(g, upRect(), "Up", pressed == P_UP, top > 0);
      drawBtn(g, downRect(), "Down", pressed == P_DOWN, top + ROWS < n);
      char msg[32];
      snprintf(msg, sizeof(msg), "%d result%s", n, n == 1 ? "" : "s");
      g.textFont(F18, msg, 330, 420, textdatum_t::middle_center, C.val2);
    }
  }
  void down(int x, int y) override { setPressed(hit(x, y)); }
  void move(int x, int y) override { if (pressed != P_NONE && hit(x, y) != pressed) setPressed(P_NONE); }
  void up(int x, int y) override {
    const int p = pressed;
    const bool still = hit(x, y) == p;
    setPressed(P_NONE);
    if (!still || p == P_NONE) return;
    if (p == P_BACK) { if (onBack) onBack(); return; }
    if (p == P_UP) { top = std::max(0, top - ROWS); markAll(); return; }
    if (p == P_DOWN) { if (top + ROWS < n) top += ROWS; markAll(); return; }
    if (p < n && onPick) onPick(items[p].id);
  }

 private:
  static constexpr int ROWS = 5, MAXI = 48, ROW_Y0 = 72, ROW_H = 60;
  static constexpr int P_NONE = -1, P_BACK = -2, P_UP = -4, P_DOWN = -5;
  struct Item { char mainText[40]; char subText[48]; int id; };
  Item items[MAXI];
  char title[32] = "", empty[48] = "";
  int n = 0, top = 0, pressed = P_NONE;
  void (*onPick)(int) = nullptr;
  void (*onBack)() = nullptr;
  Rect rowRect(int r) const { return R(16, ROW_Y0 + r * ROW_H, 768, ROW_H); }
  Rect upRect() const { return R(520, 394, 120, 52); }
  Rect downRect() const { return R(656, 394, 120, 52); }
  int hit(int x, int y) const {
    if (inR(backRect(), x, y, 6)) return P_BACK;
    if (inR(upRect(), x, y, 4)) return P_UP;
    if (inR(downRect(), x, y, 4)) return P_DOWN;
    for (int r = 0; r < ROWS; r++) if (top + r < n && inR(rowRect(r), x, y)) return top + r;
    return P_NONE;
  }
  void setPressed(int p) { if (p == pressed) return; pressed = p; markAll(); }
};
ListScreen list;

// ================================================================ city search
geo::Place cityHits[24];
int cityCount = 0;

void cityPicked(int id) { if (id >= 0 && id < cityCount) app::setLocationCity(cityHits[id]); setSub(nullptr); }
void citySearchOk(const char *text) {
  cityCount = geo::search(text, cityHits, 24);
  list.begin("Choose City", cityPicked, backToSettings, "No city found. Try fewer letters.");
  for (int i = 0; i < cityCount; i++) {
    char m[48], s[48];
    snprintf(m, sizeof(m), "%s, %s", cityHits[i].name, geo::countryName(cityHits[i].country));
    snprintf(s, sizeof(s), "%s", geo::zoneName(cityHits[i].zone));
    list.add(m, s, i);
  }
  list.show();
}

// ================================================================ coordinates
void coordsOk(const char *text) {
  char buf[40];
  strlcpy(buf, text, sizeof(buf));
  for (char *c = buf; *c; c++) if (*c == ',' || *c == ';') *c = ' ';
  char *e1 = nullptr, *e2 = nullptr;
  const double lat = strtod(buf, &e1);
  const double lon = e1 && e1 != buf ? strtod(e1, &e2) : 0;
  if (!e2 || e2 == e1 || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
    openKeyboard("Not understood: try 51.47, -0.45", text, false, 30, coordsOk, backToSettings);
    return;
  }
  app::setLocationAuto(lat, lon, "");
  setSub(nullptr);
}

// ================================================================ country / time zone
void countryPicked(int id) { if (id < 0) app::setCountryAuto(true); else app::setCountryManual(geo::countryCode(id)); setSub(nullptr); }
void countryOk(const char *text) {
  list.begin("Choose Country", countryPicked, backToSettings, "");
  list.add("Automatic", "Follow the location", -1);
  for (int i = 0; i < geo::countryCount(); i++)
    if (containsNoCase(geo::countryName(i), text) || !strncasecmp(geo::countryCode(i), text, 2)) list.add(geo::countryName(i), geo::countryCode(i), i);
  list.show();
}

void zonePicked(int id) { if (id < 0) app::setZoneAuto(true); else app::setZoneManual(id); setSub(nullptr); }
void zoneOk(const char *text) {
  char key[40];
  strlcpy(key, text, sizeof(key));
  for (char *c = key; *c; c++) if (*c == ' ') *c = '_';   // "new york" finds America/New_York
  list.begin("Choose Time Zone", zonePicked, backToSettings, "");
  list.add("Automatic", "Follow the location", -1);
  for (int i = 0; i < geo::zoneCount(); i++)
    if (containsNoCase(geo::zoneName(i), key)) list.add(geo::zoneName(i), geo::zonePosix(i), i);
  list.show();
}

// ================================================================ world map
class MapScreen : public Screen {
 public:
  void enter() override {
    const settings::Settings &S = settings::get();
    cLat = S.lat; cLon = S.lon; zoom = 5;
    havePick = false; pressed = 0; dragging = false;
    if (!boxed) computeBoxes();
    snprintf(info, sizeof(info), "Drag to move the map, tap to choose a spot.");
    markAll();
  }
  void draw(Ctx &g, const Rect &t) override {
    if (hitR(t, R(0, MAP_Y0, W, MAP_Y1 - MAP_Y0))) drawMap(g, t);
    drawHeader(g, t, "Pick On Map");
    if (hitR(t, backRect())) drawBtn(g, backRect(), "Back", pressed == B_BACK);
    if (hitR(t, R(0, MAP_Y1, W, H - MAP_Y1))) {
      g.frect(0, MAP_Y1, W, H - MAP_Y1, TFT_BLACK);
      g.hline(0, MAP_Y1, W, C.mid);
      g.textFont(F12, info, 14, MAP_Y1 + 18, textdatum_t::middle_left, C.val);
      g.textFont(F12, detail, 14, MAP_Y1 + 40, textdatum_t::middle_left, C.val2);
      drawBtn(g, outRect(), "-", pressed == B_OUT, zoom > 0);
      drawBtn(g, inRect(), "+", pressed == B_IN, zoom < 8);
      drawBtn(g, useRect(), "Use", pressed == B_USE, havePick);
    }
  }
  void down(int x, int y) override {
    const int b = button(x, y);
    if (b) { setPressed(b); return; }
    if (y >= MAP_Y0 && y < MAP_Y1) { dragging = true; moved = false; sx = lx = x; sy = ly = y; }
  }
  void move(int x, int y) override {
    if (pressed) { if (button(x, y) != pressed) setPressed(0); return; }
    if (!dragging) return;
    if (!moved && hypot(x - sx, y - sy) > 8) moved = true;
    if (!moved) return;
    cLon -= (x - lx) / ppd();
    cLat += (y - ly) / ppd();
    lx = x; ly = y;
    clampView();
    if (millis() - lastPaint > 90) { lastPaint = millis(); markAll(); }
  }
  void up(int x, int y) override {
    if (pressed) {
      const int b = pressed;
      const bool still = button(x, y) == b;
      setPressed(0);
      if (!still) return;
      if (b == B_BACK) { setSub(nullptr); return; }
      if (b == B_IN && zoom < 8) { zoom++; markAll(); return; }
      if (b == B_OUT && zoom > 0) { zoom--; clampView(); markAll(); return; }
      if (b == B_USE && havePick) { app::setLocationAuto(pickLat, pickLon, ""); setSub(nullptr); }
      return;
    }
    if (dragging && !moved && y >= MAP_Y0 && y < MAP_Y1) choose(x, y);
    else if (dragging) markAll();
    dragging = false;
  }

 private:
  static constexpr int MAP_Y0 = 64, MAP_Y1 = 420, MAP_CY = 242;
  static constexpr int B_NONE = 0, B_BACK = 1, B_OUT = 2, B_IN = 3, B_USE = 4;
  static constexpr int MAXR = 300;
  double cLat = 0, cLon = 0, pickLat = 0, pickLon = 0;
  int zoom = 5, pressed = 0, sx = 0, sy = 0, lx = 0, ly = 0;
  bool havePick = false, dragging = false, moved = false, boxed = false;
  uint32_t lastPaint = 0;
  char info[80] = "", detail[110] = "";
  int16_t box[MAXR][4];   // per outline: min lon, min lat, max lon, max lat (0.01 degree)

  double ppd() const { return 2.2 * (double)(1 << zoom); }   // pixels per degree
  int px(double lon) const { return (int)lround(W / 2 + (lon - cLon) * ppd()); }
  int py(double lat) const { return (int)lround(MAP_CY - (lat - cLat) * ppd()); }
  Rect outRect() const { return R(540, MAP_Y1 + 12, 64, 46); }
  Rect inRect() const { return R(612, MAP_Y1 + 12, 64, 46); }
  Rect useRect() const { return R(688, MAP_Y1 + 12, 100, 46); }

  int button(int x, int y) const {
    if (inR(backRect(), x, y, 6)) return B_BACK;
    if (inR(outRect(), x, y, 4)) return B_OUT;
    if (inR(inRect(), x, y, 4)) return B_IN;
    if (inR(useRect(), x, y, 4)) return B_USE;
    return B_NONE;
  }
  void setPressed(int b) {
    if (b == pressed) return;
    if (pressed) markDirty(grow(pressed == B_BACK ? backRect() : pressed == B_OUT ? outRect() : pressed == B_IN ? inRect() : useRect(), 3));
    pressed = b;
    if (b) markDirty(grow(b == B_BACK ? backRect() : b == B_OUT ? outRect() : b == B_IN ? inRect() : useRect(), 3));
  }
  void clampView() {
    const double halfLat = (MAP_CY - MAP_Y0) / ppd();
    cLat = std::max(-90.0 + halfLat * 0.3, std::min(90.0 - halfLat * 0.3, cLat));
    while (cLon > 180) cLon -= 360;
    while (cLon < -180) cLon += 360;
  }
  void computeBoxes() {
    const int n = std::min(geo::worldRings(), MAXR);
    for (int i = 0; i < n; i++) {
      const geo::Ring r = geo::worldRing(i);
      int16_t lo = 32767, la = 32767, hi = -32768, ha = -32768;
      for (uint32_t k = 0; k < r.count; k++) {
        int16_t x, y;
        geo::worldPoint(r.start + k, x, y);
        lo = std::min(lo, x); hi = std::max(hi, x); la = std::min(la, y); ha = std::max(ha, y);
      }
      box[i][0] = lo; box[i][1] = la; box[i][2] = hi; box[i][3] = ha;
    }
    boxed = true;
  }
  void choose(int x, int y) {
    pickLon = cLon + (x - W / 2) / ppd();
    pickLat = cLat - (y - MAP_CY) / ppd();
    while (pickLon > 180) pickLon -= 360;
    while (pickLon < -180) pickLon += 360;
    pickLat = std::max(-90.0, std::min(90.0, pickLat));
    havePick = true;
    geo::Place c;
    float km = 0;
    snprintf(info, sizeof(info), "Spot: %.4f, %.4f", pickLat, pickLon);
    if (geo::nearest(pickLat, pickLon, c, &km)) snprintf(detail, sizeof(detail), "Nearest: %s, %s (%d km)  %s", c.name, geo::countryName(c.country), (int)lroundf(km), geo::zoneName(c.zone));
    else detail[0] = 0;
    markAll();
  }

  void drawMap(Ctx &g, const Rect &t) {
    const double s = ppd();
    // graticule
    static const double steps[] = {30, 10, 5, 2, 1, 0.5, 0.2, 0.1};
    double step = 30;
    for (double st : steps) if (st * s >= 36) step = st;
    const double lonMin = cLon - W / 2 / s, lonMax = cLon + W / 2 / s, latMax = cLat + (MAP_CY - MAP_Y0) / s, latMin = cLat - (MAP_Y1 - MAP_CY) / s;
    int lines = 0;
    for (double lo = ceil(lonMin / step) * step; lo <= lonMax && lines < 60; lo += step, lines++) g.vline(px(lo), MAP_Y0, MAP_Y1 - MAP_Y0, C.dark);
    for (double la = ceil(std::max(latMin, -90.0) / step) * step; la <= std::min(latMax, 90.0) && lines < 120; la += step, lines++) g.hline(0, py(la), W, C.dark);
    // outlines (country borders and coasts); the world repeats every 360 degrees
    const int nr = std::min(geo::worldRings(), MAXR);
    for (int i = 0; i < nr; i++) {
      for (int off = -360; off <= 360; off += 360) {
        const int x0 = px(box[i][0] / 100.0 + off), x1 = px(box[i][2] / 100.0 + off);
        const int y0 = py(box[i][3] / 100.0), y1 = py(box[i][1] / 100.0);
        if (x1 < t.x0 || x0 >= t.x1 || y1 < t.y0 || y0 >= t.y1) continue;
        const geo::Ring r = geo::worldRing(i);
        int16_t ax, ay;
        geo::worldPoint(r.start, ax, ay);
        int pxA = px(ax / 100.0 + off), pyA = py(ay / 100.0);
        for (uint32_t k = 1; k < r.count; k++) {
          int16_t bx, by;
          geo::worldPoint(r.start + k, bx, by);
          const int pxB = px(bx / 100.0 + off), pyB = py(by / 100.0);
          g.line(pxA, pyA, pxB, pyB, C.mid);
          pxA = pxB; pyA = pyB;
        }
      }
    }
    // where the radar is now (small cross) and the chosen spot (ring)
    const settings::Settings &S = settings::get();
    const int hx = px(S.lon), hy = py(S.lat);
    g.hline(hx - 6, hy, 13, C.title); g.vline(hx, hy - 6, 13, C.title);
    if (havePick) {
      const int kx = px(pickLon), ky = py(pickLat);
      g.circle(kx, ky, 9, C.bright); g.circle(kx, ky, 10, C.bright);
      g.hline(kx - 14, ky, 28, C.bright); g.vline(kx, ky - 14, 28, C.bright);
    }
  }
};
MapScreen mapScr;

// ================================================================ SD card log viewer
// The tail of one log file (the last ~12 KB), scrolled by lines.
class TextView : public Screen {
 public:
  void open(const char *name) {
    strlcpy(title, name, sizeof(title));
    if (!buf) buf = (char *)heap_caps_malloc(BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    n = 0; len = 0;
    if (buf && sdlog::readTail(name, buf, BUF, len)) {
      lineAt[n++] = 0;
      for (size_t i = 0; i < len && n < MAXL; i++) if (buf[i] == '\n' && i + 1 < len) lineAt[n++] = (uint16_t)(i + 1);
    }
    top = std::max(0, n - LINES);
    pressed = 0;
    setSub(this);
  }
  void enter() override { markAll(); }
  void draw(Ctx &g, const Rect &t) override {
    drawHeader(g, t, title);
    if (hitR(t, backRect())) drawBtn(g, backRect(), "Back", pressed == 1);
    if (hitR(t, R(0, 66, W, 330))) {
      if (!n) g.textFont(F18, "Nothing to show yet.", W / 2, 200, textdatum_t::middle_center, C.val2);
      for (int i = 0; i < LINES && top + i < n; i++) {
        char line[110];
        const int k = top + i;
        const size_t a = lineAt[k], b = k + 1 < n ? lineAt[k + 1] : len;
        size_t m = std::min<size_t>(b - a, sizeof(line) - 1);
        memcpy(line, buf + a, m);
        while (m && (line[m - 1] == '\n' || line[m - 1] == '\r')) m--;
        line[m] = 0;
        g.textFont(F12, line, 12, 70 + i * 17, textdatum_t::top_left, C.val);
      }
    }
    if (hitR(t, R(0, 400, W, 80))) {
      drawBtn(g, upRect(), "Older", pressed == 2, top > 0);
      drawBtn(g, downRect(), "Newer", pressed == 3, top + LINES < n);
      char msg[48];
      snprintf(msg, sizeof(msg), "Lines %d-%d of the last %d", n ? top + 1 : 0, std::min(n, top + LINES), n);
      g.textFont(F12, msg, 24, 428, textdatum_t::middle_left, C.val2);
    }
  }
  void down(int x, int y) override { setPressed(hit(x, y)); }
  void move(int x, int y) override { if (pressed && hit(x, y) != pressed) setPressed(0); }
  void up(int x, int y) override {
    const int p = pressed;
    const bool still = hit(x, y) == p;
    setPressed(0);
    if (!still) return;
    if (p == 1) setSub(nullptr);
    else if (p == 2) { top = std::max(0, top - LINES + 2); markAll(); }
    else if (p == 3) { top = std::min(std::max(0, n - LINES), top + LINES - 2); markAll(); }
  }
 private:
  static constexpr int BUF = 12 * 1024, MAXL = 400, LINES = 19;
  char title[28] = "";
  char *buf = nullptr;
  uint16_t lineAt[MAXL];
  size_t len = 0;
  int n = 0, top = 0, pressed = 0;
  Rect upRect() const { return R(520, 408, 120, 52); }
  Rect downRect() const { return R(656, 408, 120, 52); }
  int hit(int x, int y) const { return inR(backRect(), x, y, 6) ? 1 : inR(upRect(), x, y, 4) ? 2 : inR(downRect(), x, y, 4) ? 3 : 0; }
  void setPressed(int p) { if (p == pressed) return; pressed = p; markAll(); }
};
TextView textView;

char gLogNames[40][24];
uint32_t gLogSizes[40];
int gLogCount = 0;

void logPicked(int id) { if (id >= 0 && id < gLogCount) textView.open(gLogNames[id]); }
void logListBack() { setSub(nullptr); }

// ================================================================ credits
class AboutScreen : public Screen {
 public:
  void enter() override { pressed = false; markAll(); }
  void draw(Ctx &g, const Rect &t) override {
    drawHeader(g, t, "Credits");
    if (hitR(t, backRect())) drawBtn(g, backRect(), "Back", pressed);
    if (!hitR(t, R(0, 70, W, 400))) return;
    char head[40];
    snprintf(head, sizeof(head), "ADS-B Radar %s", FW_VERSION);
    const char *const lines[][2] = {
        {head, "GPL-3.0-or-later. Based on mircemk's CrowPanel ADS-B radar (GPL-3.0)."},
        {"Live data", "airplanes.live (aircraft), adsbdb.com (flight routes)."},
        {"Flight details (optional)", "FlightAware AeroAPI with your key; photos from Planespotters.net, credited on screen."},
        {"Cities, countries, time zones", "GeoNames, geonames.org (CC BY 4.0). Zone rules: tz database (public domain)."},
        {"Map overlay and world map", "Natural Earth 1:10m / 1:110m, naturalearthdata.com (public domain)."},
        {"Airline names", "OpenFlights, openflights.org (Open Database License)."},
        {"Country and flag", "From the ICAO address blocks (Annex 10); the flags are drawn for this project."},
        {"Phone page map", "Leaflet (BSD-2) with OpenStreetMap contributors (ODbL), shown only on the phone."},
        {"Fonts and decoders", "DejaVu, GNU FreeFont (via LovyanGFX); stb_image JPEG decoder (public domain)."},
    };
    int y = 76;
    for (const auto &l : lines) {
      g.textFont(F18, l[0], 24, y, textdatum_t::top_left, C.title);
      g.textFont(F12, l[1], 24, y + 22, textdatum_t::top_left, C.val2);
      y += 40;
    }
  }
  void down(int x, int y) override { const bool h = inR(backRect(), x, y, 6); if (h != pressed) { pressed = h; markDirty(backRect()); } }
  void move(int x, int y) override { down(x, y); }
  void up(int x, int y) override { const bool go = pressed && inR(backRect(), x, y, 6); pressed = false; markDirty(backRect()); if (go) setSub(nullptr); }
 private:
  bool pressed = false;
};
AboutScreen about;

}  // namespace

void openCitySearch() { openKeyboard("Search City", "", false, 24, citySearchOk, backToSettings); }
void openCoordinates() {
  char init[40];
  snprintf(init, sizeof(init), "%.5f, %.5f", settings::get().lat, settings::get().lon);
  openKeyboard("Latitude, Longitude", init, false, 30, coordsOk, backToSettings);
}
void openCountryPick() { openKeyboard("Country: type part of the name or the code", "", false, 20, countryOk, backToSettings); }
void openZonePick() { openKeyboard("Time zone: type part of its name", "", false, 24, zoneOk, backToSettings); }
void keyOk(const char *t) { app::setFlightKey(t); backToSettings(); }
void emailOk(const char *t) { app::setFlightEmail(t); backToSettings(); }
void openFlightKey() { openKeyboard("FlightAware AeroAPI key", "", true, 39, keyOk, backToSettings); }   // never pre-filled: the saved key is not shown again
void openFlightEmail() { openKeyboard("Contact e-mail (sent to Planespotters)", settings::get().faEmail, false, 60, emailOk, backToSettings); }
void openMap() { setSub(&mapScr); }
void openLogList() {
  gLogCount = sdlog::listFiles(gLogNames, gLogSizes, 40);
  list.begin("SD Card Logs", logPicked, logListBack, "No log files yet (or no card).");
  for (int i = 0; i < gLogCount; i++) {
    char sub[32];
    if (gLogSizes[i] >= 1048576) snprintf(sub, sizeof(sub), "%.1f MB", gLogSizes[i] / 1048576.0); else snprintf(sub, sizeof(sub), "%u KB", (unsigned)((gLogSizes[i] + 1023) / 1024));
    list.add(gLogNames[i], sub, i);
  }
  list.show();
}
void openAbout() { setSub(&about); }

}  // namespace in
}  // namespace ui
