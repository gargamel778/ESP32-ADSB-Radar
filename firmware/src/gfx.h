// Shared drawing helpers: rectangles, font themes and the Ctx drawing context (origin translation into a scratch sprite).
// Used by the radar renderer (radar.cpp) and the settings screen (ui.cpp).
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <algorithm>

namespace gfx {

// ---------------------------------------------------------------- rectangles (x1,y1 exclusive)
struct Rect { int16_t x0, y0, x1, y1; };
inline bool emptyR(const Rect &r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
inline Rect uniR(const Rect &a, const Rect &b) {
  if (emptyR(a)) return b;
  if (emptyR(b)) return a;
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}
inline bool hitR(const Rect &a, const Rect &b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
inline bool insideR(const Rect &inner, const Rect &outer) { return inner.x0 >= outer.x0 && inner.y0 >= outer.y0 && inner.x1 <= outer.x1 && inner.y1 <= outer.y1; }
inline bool containsR(const Rect &r, int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

// ---------------------------------------------------------------- font themes
struct Theme { const char *name; const lgfx::IFont *s, *m, *l; int charW; };  // small / medium / clock; charW = widest small glyph
inline const Theme kThemes[] = {
    // Sans (default): one step smaller than before. 9 pt is the smallest FreeSans, so small and medium share it; headings are told apart by colour.
    {"Sans",     &lgfx::fonts::FreeSans9pt7b,     &lgfx::fonts::FreeSans9pt7b,     &lgfx::fonts::FreeSans18pt7b,     10},
    {"MonoBold", &lgfx::fonts::FreeMonoBold9pt7b, &lgfx::fonts::FreeMonoBold12pt7b, &lgfx::fonts::FreeMonoBold18pt7b, 11},
    {"DejaVu",   &lgfx::fonts::DejaVu12,          &lgfx::fonts::DejaVu18,           &lgfx::fonts::DejaVu24,            9},
};
constexpr int THEME_COUNT = sizeof(kThemes) / sizeof(kThemes[0]);
constexpr int kDejaVu = 2;      // index of the DejaVu theme in kThemes: the radar area and the settings screen always use it
inline int gTheme = 0;          // the user-selected theme (info table)

// ---------------------------------------------------------------- drawing context with origin translation
// Drawing code works in screen (or panel) coordinates; Ctx shifts them into the scratch sprite.
struct Ctx {
  lgfx::LGFX_Sprite *s;
  int ox, oy;
  int themeIdx = -1;  // -1 = the user-selected theme (info table); otherwise a fixed theme
  void line(int x0, int y0, int x1, int y1, uint16_t c) { s->drawLine(x0 - ox, y0 - oy, x1 - ox, y1 - oy, c); }
  void hline(int x, int y, int w, uint16_t c) { s->drawFastHLine(x - ox, y - oy, w, c); }
  void vline(int x, int y, int h, uint16_t c) { s->drawFastVLine(x - ox, y - oy, h, c); }
  void circle(int x, int y, int r, uint16_t c) { s->drawCircle(x - ox, y - oy, r, c); }
  void fcircle(int x, int y, int r, uint16_t c) { s->fillCircle(x - ox, y - oy, r, c); }
  void rect(int x, int y, int w, int h, uint16_t c) { s->drawRect(x - ox, y - oy, w, h, c); }
  void frect(int x, int y, int w, int h, uint16_t c) { s->fillRect(x - ox, y - oy, w, h, c); }
  void rrect(int x, int y, int w, int h, int r, uint16_t c) { s->drawRoundRect(x - ox, y - oy, w, h, r, c); }
  void frrect(int x, int y, int w, int h, int r, uint16_t c) { s->fillRoundRect(x - ox, y - oy, w, h, r, c); }
  void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) { s->drawTriangle(x0 - ox, y0 - oy, x1 - ox, y1 - oy, x2 - ox, y2 - oy, c); }
  void ftri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) { s->fillTriangle(x0 - ox, y0 - oy, x1 - ox, y1 - oy, x2 - ox, y2 - oy, c); }
  const Theme &theme() const { return kThemes[themeIdx >= 0 ? themeIdx : gTheme]; }
  // size 1/2/5: small / medium / clock font of the theme (anti-aliased fonts shimmer far less than 1-px strokes)
  void text(const char *str, int x, int y, int size, textdatum_t d, uint16_t fg, uint16_t bg = TFT_BLACK) {
    const Theme &th = theme();
    s->setFont(size >= 5 ? th.l : size >= 2 ? th.m : th.s);
    s->setTextDatum(d);
    s->setTextSize(1);
    s->setTextColor(fg, bg);
    s->drawString(str, x - ox, y - oy);
  }
  void text(const String &str, int x, int y, int size, textdatum_t d, uint16_t fg, uint16_t bg = TFT_BLACK) { text(str.c_str(), x, y, size, d, fg, bg); }
  // text in an explicit font, ignoring the theme
  void textFont(const lgfx::IFont *f, const char *str, int x, int y, textdatum_t d, uint16_t fg, uint16_t bg = TFT_BLACK) {
    s->setFont(f);
    s->setTextDatum(d);
    s->setTextSize(1);
    s->setTextColor(fg, bg);
    s->drawString(str, x - ox, y - oy);
  }
  int fh(int size) {  // height of the theme's font for this size class
    const Theme &th = theme();
    s->setFont(size >= 5 ? th.l : size >= 2 ? th.m : th.s);
    s->setTextSize(1);
    return s->fontHeight();
  }
};

}  // namespace gfx
