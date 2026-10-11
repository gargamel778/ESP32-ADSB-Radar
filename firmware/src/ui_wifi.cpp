// Wi-Fi sub-screens of the settings UI: network list, on-screen keyboard, connect progress, phone setup (access point + QR code).
#include "ui_internal.h"
#include "radar.h"
#include "settings.h"
#include "wifimgr.h"

using namespace gfx;

namespace ui {
namespace in {
namespace {

Rect R(int x, int y, int w, int h) { return {(int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)(y + h)}; }
Rect grow(Rect r, int m) { return {(int16_t)(r.x0 - m), (int16_t)(r.y0 - m), (int16_t)(r.x1 + m), (int16_t)(r.y1 + m)}; }
constexpr int W = 800, H = 480;

// ================================================================ keyboard
enum KeyKind : uint8_t { KK_CHAR, KK_SHIFT, KK_SYM, KK_DEL, KK_SPACE, KK_OK, KK_SHOW, KK_CANCEL };
struct Key { Rect r; KeyKind kind; uint8_t col; uint8_t row; };   // CHAR keys: row 0..3 and column index into the layer's row string

class Keyboard : public Screen {
 public:
  void (*onOk)(const char *) = nullptr;
  void (*onCancel)() = nullptr;
  char title[40] = "";
  char text[66] = "";
  bool secret = false;
  size_t maxLen = 63;

  void start(const char *ttl, const char *initial, bool isSecret, size_t maxChars, void (*ok)(const char *), void (*cancel)()) {
    strlcpy(title, ttl, sizeof(title));
    strlcpy(text, initial, sizeof(text));
    secret = isSecret; maxLen = maxChars; onOk = ok; onCancel = cancel;
    show = !isSecret;
    shift = 0; sym = false;
    setSub(this);
  }

  void enter() override { build(); pressed = -1; markAll(); }

  void draw(Ctx &g, const Rect &t) override {
    drawHeader(g, t, title);
    if (hitR(t, backRect())) drawBtn(g, backRect(), "Cancel", pressed == cancelIdx);
    // text field
    const Rect f = fieldRect();
    if (hitR(t, f)) {
      g.rrect(f.x0, f.y0, f.x1 - f.x0, f.y1 - f.y0, 8, C.mid);
      char shown[70];
      const size_t n = strlen(text);
      if (secret && !show) { memset(shown, '*', n); shown[n] = 0; } else strlcpy(shown, text, sizeof(shown));
      const char *p = shown;
      while (strlen(p) > 40) p++;   // long text: show the end, where the typing happens
      g.textFont(F18, p, f.x0 + 14, (f.y0 + f.y1) / 2, textdatum_t::middle_left, C.val);
      const int w = g.s->textWidth(p);
      g.vline(f.x0 + 16 + w, f.y0 + 12, f.y1 - f.y0 - 24, C.title);   // cursor
      char cnt[12];
      snprintf(cnt, sizeof(cnt), "%u/%u", (unsigned)n, (unsigned)maxLen);
      g.textFont(F12, cnt, f.x1 - 10, f.y1 - 8, textdatum_t::bottom_right, C.dim);
    }
    if (secret && hitR(t, showRect())) drawBtn(g, showRect(), show ? "Hide" : "Show", pressed == showIdx);
    for (int i = 0; i < nKeys; i++) {
      const Key &k = keys[i];
      if (!hitR(t, k.r)) continue;
      char lab[8];
      label(k, lab);
      const bool down = pressed == i;
      const bool lit = (k.kind == KK_SHIFT && shift == 1) || (k.kind == KK_SYM && sym);
      g.frrect(k.r.x0, k.r.y0, k.r.x1 - k.r.x0, k.r.y1 - k.r.y0, 8, down || lit ? C.dark : TFT_BLACK);
      g.rrect(k.r.x0, k.r.y0, k.r.x1 - k.r.x0, k.r.y1 - k.r.y0, 8, down ? C.bright : k.kind == KK_OK ? C.bright : C.mid);
      g.textFont(k.kind == KK_CHAR ? F24 : F18, lab, (k.r.x0 + k.r.x1) / 2, (k.r.y0 + k.r.y1) / 2, textdatum_t::middle_center, k.kind == KK_CHAR ? C.val : C.title, down || lit ? C.dark : TFT_BLACK);
    }
  }

  void down(int x, int y) override {
    const int i = hit(x, y);
    setPressed(i);
    if (i < 0) return;
    const Key *k = i < nKeys ? &keys[i] : nullptr;
    if (!k) return;
    switch (k->kind) {
      case KK_CHAR: { char c[2] = {charOf(*k), 0}; add(c[0]); if (shift == 1) { shift = 0; markDirty(keysArea()); } break; }
      case KK_SPACE: add(' '); break;
      case KK_DEL: del(); repeatAt = millis() + 450; break;
      case KK_SHIFT: if (!sym) { shift = shift ? 0 : 1; markDirty(keysArea()); } break;
      case KK_SYM: sym = !sym; shift = 0; markDirty(keysArea()); break;
      default: break;   // OK, Show, Cancel act on release
    }
  }

  void move(int x, int y) override {
    if (pressed >= 0 && hit(x, y) != pressed) setPressed(-1);
  }

  void up(int x, int y) override {
    const int i = pressed;
    const bool still = hit(x, y) == i;
    repeatAt = 0;
    setPressed(-1);
    if (i < 0 || !still) return;
    if (i == cancelIdx) { if (onCancel) onCancel(); return; }
    if (i == showIdx) { show = !show; markDirty(fieldRect()); markDirty(showRect()); return; }
    if (i < nKeys && keys[i].kind == KK_OK) { if (onOk) { char copy[66]; strlcpy(copy, text, sizeof(copy)); onOk(copy); } }
  }

  void tick() override {
    if (repeatAt && pressed >= 0 && pressed < nKeys && keys[pressed].kind == KK_DEL && millis() >= repeatAt) { del(); repeatAt = millis() + 90; }
  }

 private:
  static constexpr int MAXK = 48, cancelIdx = 100, showIdx = 101;
  Key keys[MAXK];
  int nKeys = 0, pressed = -1;
  uint8_t shift = 0;   // 0 lower, 1 upper (one letter)
  bool sym = false, show = false;
  uint32_t repeatAt = 0;

  Rect fieldRect() const { return R(20, 76, 640, 54); }
  Rect showRect() const { return R(676, 76, 104, 54); }
  Rect keysArea() const { return R(0, 140, W, 320); }

  void add(char c) { const size_t n = strlen(text); if (n < maxLen) { text[n] = c; text[n + 1] = 0; markDirty(fieldRect()); } }
  void del() { const size_t n = strlen(text); if (n) { text[n - 1] = 0; markDirty(fieldRect()); } }

  void setPressed(int i) {
    if (i == pressed) return;
    if (pressed >= 0) markDirty(grow(rectOf(pressed), 2));
    pressed = i;
    if (i >= 0) markDirty(grow(rectOf(i), 2));
  }
  Rect rectOf(int i) const { return i == cancelIdx ? backRect() : i == showIdx ? showRect() : keys[i].r; }

  int hit(int x, int y) const {
    if (x >= backRect().x0 && x < backRect().x1 && y >= backRect().y0 && y < backRect().y1) return cancelIdx;
    if (secret && x >= showRect().x0 && x < showRect().x1 && y >= showRect().y0 && y < showRect().y1) return showIdx;
    for (int i = 0; i < nKeys; i++) {
      const Rect r = grow(keys[i].r, 3);
      if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return i;
    }
    return -1;
  }

  static const char *rowChars(int row, bool sy) {
    static const char *const lo[5] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm", "."};
    static const char *const sm[5] = {"1234567890", "!@#$%^&*()", "-_=+[];:'", "\",./?\\|", "."};
    return sy ? sm[row] : lo[row];
  }
  char charOf(const Key &k) const {
    char c = rowChars(k.row, sym)[k.col];
    if (!sym && shift == 1 && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    return c;
  }
  void label(const Key &k, char *out) const {
    switch (k.kind) {
      case KK_CHAR: out[0] = charOf(k); out[1] = 0; break;
      case KK_SHIFT: strcpy(out, "Shift"); break;
      case KK_SYM: strcpy(out, sym ? "abc" : "123"); break;
      case KK_DEL: strcpy(out, "Del"); break;
      case KK_SPACE: strcpy(out, "Space"); break;
      default: strcpy(out, "OK"); break;
    }
  }

  void build() {
    nKeys = 0;
    const int KW = 72, KH = 54, G = 6;
    const int y0[4] = {146, 206, 266, 326};
    const int x0[4] = {13, 13, 52, 130};
    for (int row = 0; row < 4; row++) {
      const int n = strlen(rowChars(row, false));
      for (int c = 0; c < n; c++) keys[nKeys++] = {R(x0[row] + c * (KW + G), y0[row], KW, KH), KK_CHAR, (uint8_t)c, (uint8_t)row};
    }
    keys[nKeys++] = {R(24, 326, 100, KH), KK_SHIFT, 0, 3};
    keys[nKeys++] = {R(676, 326, 100, KH), KK_DEL, 0, 3};
    keys[nKeys++] = {R(61, 386, 104, KH), KK_SYM, 0, 4};
    keys[nKeys++] = {R(171, 386, 380, KH), KK_SPACE, 0, 4};
    keys[nKeys++] = {R(557, 386, 72, KH), KK_CHAR, 0, 4};   // "."
    keys[nKeys++] = {R(635, 386, 104, KH), KK_OK, 0, 4};
  }
};

Keyboard kb;

// ================================================================ connect progress
class JoinScreen : public Screen {
 public:
  void start(const char *name, const char *pass) {
    strlcpy(ssid, name, sizeof(ssid));
    wifimgr::join(name, pass);
    doneAt = 0;
    setSub(this);
  }
  void enter() override { sig = -1; pressed = -1; markAll(); }
  void draw(Ctx &g, const Rect &t) override {
    drawHeader(g, t, "Connecting");
    const wifimgr::JoinState st = wifimgr::joinState();
    if (!hitR(t, R(0, 70, W, 400))) return;
    char line[64];
    if (st == wifimgr::J_TRYING) {
      static const char *const dots[4] = {"", ".", "..", "..."};
      snprintf(line, sizeof(line), "Joining %s%s", ssid, dots[(millis() / 500) % 4]);
      g.textFont(F24, line, W / 2, 190, textdatum_t::middle_center, C.title);
      g.textFont(F18, "This takes up to 20 seconds.", W / 2, 240, textdatum_t::middle_center, C.val2);
      g.textFont(F12, "The radar keeps the old network until the new one works.", W / 2, 285, textdatum_t::middle_center, C.dim);
    } else if (st == wifimgr::J_OK) {
      g.textFont(F24, "Connected", W / 2, 170, textdatum_t::middle_center, C.title);
      g.textFont(F18, wifimgr::joinMessage(), W / 2, 225, textdatum_t::middle_center, C.val);
      g.textFont(F18, "Saved. The radar will use this network from now on.", W / 2, 270, textdatum_t::middle_center, C.val2);
      drawBtn(g, R(300, 330, 200, 52), "Done", pressed == 1);
    } else {
      g.textFont(F24, "Could not connect", W / 2, 150, textdatum_t::middle_center, rgbWarn(g));
      g.textFont(F18, wifimgr::joinMessage(), W / 2, 205, textdatum_t::middle_center, C.val);
      snprintf(line, sizeof(line), "Still on %s", settings::get().wifiSsid[0] ? settings::get().wifiSsid : "the old network");
      g.textFont(F18, line, W / 2, 250, textdatum_t::middle_center, C.val2);
      drawBtn(g, R(190, 330, 200, 52), "Try Again", pressed == 2);
      drawBtn(g, R(410, 330, 200, 52), "Back", pressed == 3);
    }
  }
  void down(int x, int y) override { setPressed(btnAt(x, y)); }
  void move(int x, int y) override { if (pressed > 0 && btnAt(x, y) != pressed) setPressed(0); }
  void up(int x, int y) override {
    const int b = pressed;
    const bool still = btnAt(x, y) == b;
    setPressed(0);
    if (!still || b <= 0) return;
    if (b == 1) { wifimgr::joinDone(); setSub(nullptr); }
    else if (b == 2) { openScan(); }
    else if (b == 3) { wifimgr::joinDone(); setSub(nullptr); }
  }
  void tick() override {
    const int s = (int)wifimgr::joinState() * 10 + (wifimgr::joinState() == wifimgr::J_TRYING ? (millis() / 500) % 4 : 0);
    if (s != sig) { sig = s; markDirty(R(0, 70, W, 400)); }
    if (wifimgr::joinState() == wifimgr::J_OK) {
      if (!doneAt) doneAt = millis();
      else if (millis() - doneAt > 4000) { wifimgr::joinDone(); setSub(nullptr); }
    }
  }
 private:
  char ssid[33] = "";
  int sig = -1, pressed = 0;
  uint32_t doneAt = 0;
  static uint16_t rgbWarn(Ctx &g) { return g.s->color565(255, 120, 90); }
  int btnAt(int x, int y) const {
    const wifimgr::JoinState st = wifimgr::joinState();
    auto in = [&](Rect r) { return x >= r.x0 - 4 && x < r.x1 + 4 && y >= r.y0 - 4 && y < r.y1 + 4; };
    if (st == wifimgr::J_OK) return in(R(300, 330, 200, 52)) ? 1 : 0;
    if (st == wifimgr::J_FAILED) return in(R(190, 330, 200, 52)) ? 2 : in(R(410, 330, 200, 52)) ? 3 : 0;
    return 0;
  }
  void setPressed(int b) { if (b == pressed) return; pressed = b; markDirty(R(150, 320, 500, 72)); }
};

JoinScreen joinScr;

char gChosenSsid[33] = "";

void startJoin(const char *pass) { joinScr.start(gChosenSsid, pass); }
void backToScan() { setSub(nullptr); openScan(); }
void passEntered(const char *pass) { startJoin(pass); }
void ssidEntered(const char *name) {
  if (!*name) { backToScan(); return; }
  strlcpy(gChosenSsid, name, sizeof(gChosenSsid));
  char title[40];
  snprintf(title, sizeof(title), "Password for %.22s", gChosenSsid);
  kb.start(title, "", true, 63, passEntered, backToScan);
}

// ================================================================ network list
class ScanScreen : public Screen {
 public:
  bool fresh = true;
  void enter() override {
    top = 0; pressed = -1; sig = -1;
    if (fresh) wifimgr::startScan();
    fresh = false;
    markAll();
  }
  void draw(Ctx &g, const Rect &t) override {
    drawHeader(g, t, "Choose Network");
    if (hitR(t, backRect())) drawBtn(g, backRect(), "Back", pressed == P_BACK);
    const int n = total();
    for (int r = 0; r < ROWS; r++) {
      const int i = top + r;
      const Rect rr = rowRect(r);
      if (!hitR(t, rr)) continue;
      if (i >= n) continue;
      const bool down = pressed == i;
      if (down) g.frrect(rr.x0, rr.y0, rr.x1 - rr.x0, rr.y1 - rr.y0, 8, C.dark);
      g.hline(rr.x0 + 4, rr.y1 - 1, rr.x1 - rr.x0 - 8, C.dark);
      const uint16_t bg = down ? C.dark : TFT_BLACK;
      if (i < wifimgr::scanCount()) {
        const wifimgr::Net &nt = wifimgr::scanNet(i);
        g.textFont(F18, nt.ssid, rr.x0 + 14, (rr.y0 + rr.y1) / 2, textdatum_t::middle_left, C.val, bg);
        if (wifimgr::connected() && wifimgr::ssid() == nt.ssid) g.textFont(F12, "Connected", rr.x0 + 440, (rr.y0 + rr.y1) / 2, textdatum_t::middle_left, C.title2, bg);
        bars(g, rr.x1 - 140, rr.y1 - 16, nt.rssi);
        g.textFont(F12, nt.secure ? "Locked" : "Open", rr.x1 - 12, (rr.y0 + rr.y1) / 2, textdatum_t::middle_right, C.val2, bg);
      } else {
        g.textFont(F18, "Other network...", rr.x0 + 14, (rr.y0 + rr.y1) / 2, textdatum_t::middle_left, C.title, bg);
        g.textFont(F12, "Hidden or not listed", rr.x1 - 12, (rr.y0 + rr.y1) / 2, textdatum_t::middle_right, C.dim, bg);
      }
    }
    if (hitR(t, R(0, 380, W, 100))) {
      drawBtn(g, rescanRect(), "Rescan", pressed == P_RESCAN, !wifimgr::scanning());
      drawBtn(g, upRect(), "Up", pressed == P_UP, top > 0);
      drawBtn(g, downRect(), "Down", pressed == P_DOWN, top + ROWS < n);
      char msg[40];
      if (wifimgr::scanning()) snprintf(msg, sizeof(msg), "Scanning...");
      else snprintf(msg, sizeof(msg), "%d network%s", wifimgr::scanCount(), wifimgr::scanCount() == 1 ? "" : "s");
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
    if (p == P_BACK) { fresh = true; setSub(nullptr); return; }
    if (p == P_RESCAN) { if (!wifimgr::scanning()) { wifimgr::startScan(); top = 0; markAll(); } return; }
    if (p == P_UP) { top = max(0, top - ROWS); markAll(); return; }
    if (p == P_DOWN) { if (top + ROWS < total()) top += ROWS; markAll(); return; }
    // a network
    if (p < wifimgr::scanCount()) {
      const wifimgr::Net &nt = wifimgr::scanNet(p);
      strlcpy(gChosenSsid, nt.ssid, sizeof(gChosenSsid));
      if (!nt.secure) { startJoin(""); return; }
      char title[40];
      snprintf(title, sizeof(title), "Password for %.22s", nt.ssid);
      kb.start(title, "", true, 63, passEntered, backToScan);
    } else {
      kb.start("Network Name", "", false, 32, ssidEntered, backToScan);
    }
  }
  void tick() override {
    const int s = (wifimgr::scanning() ? 1000 : 0) + wifimgr::scanCount();
    if (s != sig) { sig = s; markAll(); }
  }
 private:
  static constexpr int ROWS = 5, ROW_Y0 = 72, ROW_H = 60;
  static constexpr int P_NONE = -1, P_BACK = -2, P_RESCAN = -3, P_UP = -4, P_DOWN = -5;
  int top = 0, pressed = P_NONE, sig = -1;
  int total() const { return wifimgr::scanCount() + 1; }   // + "Other network..."
  Rect rowRect(int r) const { return R(16, ROW_Y0 + r * ROW_H, 768, ROW_H); }
  Rect rescanRect() const { return R(24, 394, 160, 52); }
  Rect upRect() const { return R(520, 394, 120, 52); }
  Rect downRect() const { return R(656, 394, 120, 52); }
  static bool in(const Rect &r, int x, int y, int m = 0) { return x >= r.x0 - m && x < r.x1 + m && y >= r.y0 - m && y < r.y1 + m; }
  int hit(int x, int y) const {
    if (in(backRect(), x, y, 6)) return P_BACK;
    if (in(rescanRect(), x, y, 4)) return P_RESCAN;
    if (in(upRect(), x, y, 4)) return P_UP;
    if (in(downRect(), x, y, 4)) return P_DOWN;
    for (int r = 0; r < ROWS; r++) if (top + r < total() && in(rowRect(r), x, y)) return top + r;
    return P_NONE;
  }
  void setPressed(int p) { if (p == pressed) return; pressed = p; markAll(); }
  static void bars(Ctx &g, int x, int baseY, int rssi) {
    const int level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
    for (int b = 0; b < 4; b++) g.frect(x + b * 9, baseY - (6 + b * 5), 6, 6 + b * 5, b < level ? C.title : C.dark);
  }
};

ScanScreen scan;

// ================================================================ phone setup
class PhoneScreen : public Screen {
 public:
  void enter() override {
    wifimgr::screenWantsAp(true);   // the access point runs while this screen is up (and, with no usable network, also when it is not)
    pressed = false;
    doneAt = 0;
    // QR code of the access point's credentials (white background, black modules: that is what scanners need)
    qr.setPsram(true);
    qr.setColorDepth(radar::scratchTile()->getColorDepth());
    if (qr.createSprite(QR, QR)) {
      qr.fillSprite(TFT_WHITE);
      qr.qrcode(wifimgr::apQr().c_str(), 0, 0, QR, 1, true);
    }
    sig = -1;
    markAll();
  }
  void leave() override {
    qr.deleteSprite();
    wifimgr::screenWantsAp(false);   // off again unless the radar has no network to use
  }
  void draw(Ctx &g, const Rect &t) override {
    drawHeader(g, t, "Setup From Phone");
    if (hitR(t, backRect())) drawBtn(g, backRect(), "Back", pressed);
    const bool on = wifimgr::apActive();
    const Rect qrR = R(30, 90, QR, QR);
    if (on && qr.width() > 0 && hitR(t, qrR)) qr.pushSprite(g.s, qrR.x0 - g.ox, qrR.y0 - g.oy);
    if (!hitR(t, R(300, 70, 500, 400))) return;
    const int x = 320;
    if (!on) {   // switched off: the connection was made
      g.textFont(F24, wifimgr::joinState() == wifimgr::J_OK ? "Connected" : "Setup Wi-Fi is off", x, 150, textdatum_t::top_left, C.title);
      if (wifimgr::joinState() == wifimgr::J_OK) g.textFont(F18, wifimgr::joinMessage(), x, 195, textdatum_t::top_left, C.val);
      return;
    }
    g.textFont(F18, "1  Scan the code, or join this Wi-Fi:", x, 100, textdatum_t::top_left, C.val);
    g.textFont(F24, wifimgr::apSsid().c_str(), x, 138, textdatum_t::top_left, C.title);
    g.textFont(F18, "Password", x, 182, textdatum_t::top_left, C.val2);
    g.textFont(F24, wifimgr::apPass(), x + 120, 178, textdatum_t::top_left, C.title);
    g.textFont(F18, "2  Your phone opens the setup page.", x, 240, textdatum_t::top_left, C.val);
    g.textFont(F12, "If it does not, open http://192.168.4.1 in a browser.", x, 272, textdatum_t::top_left, C.val2);
    char line[64];
    snprintf(line, sizeof(line), "Phones connected: %u", (unsigned)wifimgr::apClients());
    g.textFont(F18, line, x, 330, textdatum_t::top_left, C.val2);
    if (wifimgr::joinState() == wifimgr::J_OK) g.textFont(F18, wifimgr::joinMessage(), x, 370, textdatum_t::top_left, C.title);
    else if (wifimgr::joinState() == wifimgr::J_TRYING) g.textFont(F18, "Phone is connecting the radar...", x, 370, textdatum_t::top_left, C.title2);
    if (wifimgr::joinState() == wifimgr::J_OK) g.textFont(F12, "Connected - the setup Wi-Fi is switching off.", x, 410, textdatum_t::top_left, C.val2);

    else if (wifimgr::joinState() == wifimgr::J_FAILED) g.textFont(F18, wifimgr::joinMessage(), x, 370, textdatum_t::top_left, C.val2);
  }
  void down(int x, int y) override { const bool h = inBack(x, y); if (h != pressed) { pressed = h; markDirty(backRect()); } }
  void move(int x, int y) override { down(x, y); }
  void up(int x, int y) override {
    const bool go = pressed && inBack(x, y);
    pressed = false;
    markDirty(backRect());
    if (go) setSub(nullptr);
  }
  void tick() override {
    if (wifimgr::joinState() == wifimgr::J_OK && !wifimgr::apActive()) {   // connected and the setup Wi-Fi is off: back to the settings
      if (!doneAt) doneAt = millis();
      else if (millis() - doneAt > 3000) { wifimgr::joinDone(); setSub(nullptr); return; }
    }
    const int s = ((int)wifimgr::apClients() * 10 + (int)wifimgr::joinState()) * 1000 + (int)wifimgr::setupSecondsLeft() * 2 + (wifimgr::apActive() ? 1 : 0);
    if (s != sig) { sig = s; markDirty(R(300, 70, 500, 400)); }
  }
 private:
  static constexpr int QR = 250;
  lgfx::LGFX_Sprite qr;
  bool pressed = false;
  int sig = -1;
  uint32_t doneAt = 0;
  static bool inBack(int x, int y) { const Rect r = backRect(); return x >= r.x0 - 6 && x < r.x1 + 6 && y >= r.y0 - 6 && y < r.y1 + 6; }
};

PhoneScreen phone;

// ================================================================ firmware update progress
class OtaScreen : public Screen {
 public:
  int pct = 0;
  bool failed = false, dark = false;
  char msg[72] = "";
  void enter() override { markAll(); }
  void draw(Ctx &g, const Rect &t) override {
    if (dark) {   // while flash is written the display interrupt is starved: a fill with no detail has nothing to tear
      g.frect(t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0, TFT_BLACK);
      const int top = H - (H * pct) / 100;
      if (t.y1 > top) { const int y0 = std::max<int>(t.y0, top); g.frect(t.x0, y0, t.x1 - t.x0, t.y1 - y0, C.mid); }
      return;
    }
    drawHeader(g, t, "Firmware Update");
    if (!hitR(t, R(0, 70, W, 400))) return;
    char big[16];
    snprintf(big, sizeof(big), "%d %%", pct);
    g.textFont(F24, failed ? "Update failed" : big, W / 2, 150, textdatum_t::middle_center, failed ? g.s->color565(255, 120, 90) : C.title);
    g.rrect(100, 200, 600, 36, 8, C.mid);
    if (!failed && pct > 0) g.frrect(104, 204, std::max(8, (592 * pct) / 100), 28, 5, C.title);
    g.textFont(F18, msg, W / 2, 275, textdatum_t::middle_center, C.val);
    g.textFont(F12, failed ? "The previous firmware keeps running." : "Do not switch the radar off.", W / 2, 320, textdatum_t::middle_center, C.val2);
  }
};
OtaScreen otaScr;
bool otaShown = false;

}  // namespace

void otaShow(int pct, const char *msg, bool failed) {
  const bool wasDark = otaScr.dark;
  otaScr.dark = false; otaScr.pct = pct; otaScr.failed = failed; strlcpy(otaScr.msg, msg, sizeof(otaScr.msg));
  if (!otaShown) { otaShown = true; setSub(&otaScr); }
  else if (wasDark) markAll();
  else markDirty(R(0, 70, W, 400));
}

void otaShowDark(int pct) {
  const bool wasDark = otaScr.dark;
  const int old = otaScr.pct;
  otaScr.dark = true; otaScr.pct = pct; otaScr.failed = false;
  if (!otaShown) { otaShown = true; setSub(&otaScr); }
  else if (!wasDark) markAll();
  else if (pct != old) {
    const int a = H - (H * std::max(pct, old)) / 100, b = H - (H * std::min(pct, old)) / 100;
    markDirty(R(0, a, W, b + 1));
  }
}

void otaHide() { otaShown = false; }

void openKeyboard(const char *title, const char *initial, bool secret, size_t maxLen, void (*ok)(const char *), void (*cancel)()) { kb.start(title, initial, secret, maxLen, ok, cancel); }
void openScan() { scan.fresh = true; setSub(&scan); }
void openPhone() { setSub(&phone); }

}  // namespace in
}  // namespace ui
