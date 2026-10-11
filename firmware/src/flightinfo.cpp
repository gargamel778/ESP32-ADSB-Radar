#include "flightinfo.h"
#include "settings.h"
#include "config.h"
#include "adsb.h"
#include "isotime.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>

uint8_t *jpegDecodeRgb(const uint8_t *data, size_t len, int &w, int &h);   // jpegdec.cpp
void jpegFree(uint8_t *p);

extern const uint8_t caBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t caBundleEnd[] asm("_binary_x509_crt_bundle_end");

#ifndef FA_BASE
#define FA_BASE "https://aeroapi.flightaware.com/aeroapi"   // test builds point this at a mock server
#endif
constexpr int PHOTO_MAX_W = 220, PHOTO_MAX_H = 96;   // the card's photo box on the info panel (under the text, above the footer)
#ifdef FA_KEY_OVERRIDE
#define FA_KEY_NOW FA_KEY_OVERRIDE   // test builds only: send this key (the stand-in server's) instead of the stored one, which then never leaves the radar
#else
#define FA_KEY_NOW settings::get().faKey
#endif
#ifndef PS_BASE
#define PS_BASE "https://api.planespotters.net"   // test builds point this at tools/mock_aeroapi.py too
#endif
#ifndef FA_MAX_CALLS_PER_HOUR
#define FA_MAX_CALLS_PER_HOUR 60   // a ceiling on what the radar can spend, whatever is tapped
#endif

namespace flightinfo {
namespace {

constexpr uint32_t DEBOUNCE_MS = 1500;       // the aircraft must stay selected this long before anything is requested
constexpr uint32_t FA_TTL_MS = 10 * 60000;   // flight details are re-requested after this
constexpr uint32_t PHOTO_TTL_MS = 3600000;   // an aircraft without a photo is asked again after this
constexpr int MAX_TRACK = 300;
constexpr int SLOTS = 3;

struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); return p ? p : malloc(n); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); return q ? q : realloc(p, n); }
} gAlloc;

struct Slot {
  bool used;
  char callsign[10], hex[8], reg[10];
  uint32_t usedAt, faAt, photoAt;
  bool faTried, trackTried, photoTried;
  uint8_t photoStage;          // 0 not started, 1 have the picture's address
  char faid[64];
  char photoUrl[110];
  Info info;
  float *lat, *lon;            // flown track (PSRAM)
  lgfx::LGFX_Sprite *spr;      // the photo (PSRAM)
};

SemaphoreHandle_t gMutex;
Slot gSlots[SLOTS];
struct { bool valid; char callsign[10], hex[8], reg[10]; uint32_t at; } gReq;
volatile uint32_t gVersion = 0;
bool gTestPending = false;
char gStatusText[48] = "";
char gTestText[48] = "";
uint32_t gHourStart = 0;
uint16_t gCallsHour = 0, gCallsTotal = 0;

struct Lock { Lock() { xSemaphoreTake(gMutex, portMAX_DELAY); } ~Lock() { xSemaphoreGive(gMutex); } };
void bump() { gVersion = gVersion + 1; }

// ------------------------------------------------------------------ small helpers
void trim(char *dst, size_t cap, const char *src) {
  strlcpy(dst, src ? src : "", cap);
  for (int i = (int)strlen(dst) - 1; i >= 0 && dst[i] == ' '; i--) dst[i] = 0;
}

time_t parseIso(const char *s) { return isotime::parse(s); }   // AeroAPI times are UTC ("2026-10-10T08:15:00Z")

bool budget() {
  if (millis() - gHourStart > 3600000UL || !gHourStart) { gHourStart = millis() ? millis() : 1; gCallsHour = 0; }
  return gCallsHour < FA_MAX_CALLS_PER_HOUR;
}
void countCall() { gCallsHour++; gCallsTotal++; }

void setStatus(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void setStatus(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  Lock l;
  vsnprintf(gStatusText, sizeof(gStatusText), fmt, ap);
  va_end(ap);
}

// A response body collected in PSRAM (works for chunked bodies too, unlike reading the stream by hand).
class Sink : public Stream {
 public:
  uint8_t *buf = nullptr;
  size_t len = 0, cap = 0, maxBytes;
  bool over = false;
  explicit Sink(size_t m) : maxBytes(m) {}
  ~Sink() { if (buf) heap_caps_free(buf); }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *p, size_t n) override {
    if (len + n > maxBytes) { over = true; return 0; }
    if (len + n + 1 > cap) {
      size_t nc = std::max<size_t>(cap * 2, len + n + 1 + 4096);
      if (nc > maxBytes + 1) nc = maxBytes + 1;
      uint8_t *nb = (uint8_t *)heap_caps_realloc(buf, nc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!nb) { over = true; return 0; }
      buf = nb; cap = nc;
    }
    memcpy(buf + len, p, n);
    len += n;
    buf[len] = 0;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

// One GET. Returns the HTTP status, or a negative number when there was none. The body (also of an error answer) is in `sink`.
int httpGet(const char *url, const char *hdr, const char *hdrVal, const char *ua, Sink &sink, bool json) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  const bool https = strncasecmp(url, "https://", 8) == 0;
  WiFiClientSecure sec;
  WiFiClient plain;
  if (https) { sec.setCACertBundle(caBundleStart, caBundleEnd - caBundleStart); sec.setHandshakeTimeout(8); }
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(10000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!(https ? http.begin(sec, url) : http.begin(plain, url))) return -2;
  http.setUserAgent(ua);
  http.addHeader("Accept", json ? "application/json" : "image/jpeg,*/*");
  if (hdr) http.addHeader(hdr, hdrVal);
  int code = http.GET();
  if (code > 0) {
    int w = 0;
    if (http.getSize() > 0) {   // read it here and sleep between polls: the library's loops spin on yield() and starve the core-0 idle task on a slow link (see adsb.cpp)
      WiFiClient *stream = http.getStreamPtr();
      int left = http.getSize();
      uint32_t lastData = millis();
      uint8_t chunk[512];
      while (left > 0 && (http.connected() || stream->available())) {
        const int avail = stream->available();
        if (avail <= 0) { if (millis() - lastData > 10000) { w = -1; break; } vTaskDelay(pdMS_TO_TICKS(3)); continue; }
        const int n = stream->readBytes(chunk, std::min<int>(avail, std::min<int>(left, sizeof(chunk))));
        if ((int)sink.write(chunk, n) != n) { w = -1; break; }
        left -= n; w += n; lastData = millis();
      }
      if (left > 0) w = -1;
    } else w = http.writeToStream(&sink);   // chunked answer: the library parses the chunks
    if (code == 200 && (w < 0 || sink.over)) code = -3;
  }
  http.end();
  return code;
}

const char *errText(int code, Sink &sink, char *buf, size_t cap) {
  if (code == 401) snprintf(buf, cap, "Key rejected (401)");
  else if (code == 403) snprintf(buf, cap, "Not allowed (403): check the key's plan");
  else if (code == 429) snprintf(buf, cap, "Limit reached (429)");
  else if (code == 404) snprintf(buf, cap, "No flight found");
  else if (code > 0) {
    // FlightAware explains its errors in the body: {"title":..,"reason":..,"detail":..}
    JsonDocument filter; filter["detail"] = true; filter["reason"] = true; filter["title"] = true;
    JsonDocument doc(&gAlloc);
    const char *why = nullptr;
    if (sink.buf && !deserializeJson(doc, (const char *)sink.buf, sink.len, DeserializationOption::Filter(filter))) {
      why = doc["detail"] | (const char *)nullptr;
      if (!why) why = doc["reason"] | (const char *)nullptr;
      if (!why) why = doc["title"] | (const char *)nullptr;
    }
    if (why) snprintf(buf, cap, "HTTP %d: %.30s", code, why); else snprintf(buf, cap, "Server error (HTTP %d)", code);
  } else if (code == -1) snprintf(buf, cap, "No Wi-Fi");
  else snprintf(buf, cap, "No answer from the server");
  return buf;
}

Slot *findSlot(const char *cs, const char *hex) {
  for (Slot &s : gSlots) if (s.used && !strcmp(s.callsign, cs) && !strcmp(s.hex, hex)) return &s;
  return nullptr;
}

void freeSlotData(Slot &s) {   // caller holds the lock
  if (s.lat) { heap_caps_free(s.lat); s.lat = nullptr; }
  if (s.lon) { heap_caps_free(s.lon); s.lon = nullptr; }
  if (s.spr) { s.spr->deleteSprite(); delete s.spr; s.spr = nullptr; }
}

Slot *newSlot(const char *cs, const char *hex, const char *reg) {   // caller holds the lock; the least recently used slot is recycled
  Slot *best = &gSlots[0];
  for (Slot &s : gSlots) { if (!s.used) { best = &s; break; } if (s.usedAt < best->usedAt) best = &s; }
  freeSlotData(*best);
  memset(best, 0, sizeof(*best));
  best->used = true;
  strlcpy(best->callsign, cs, sizeof(best->callsign));
  strlcpy(best->hex, hex, sizeof(best->hex));
  strlcpy(best->reg, reg, sizeof(best->reg));
  best->usedAt = millis();
  best->info.progress = -1;
  best->info.delayMin = INT16_MIN;
  return best;
}

bool validHex(const char *h) { return h && strlen(h) == 6 && strspn(h, "0123456789abcdefABCDEF") == 6; }
bool keySet() { return settings::get().faKey[0] != 0; }
bool emailSet() { const char *e = settings::get().faEmail; return strchr(e, '@') && strchr(e, '.'); }

// ------------------------------------------------------------------ FlightAware
void pickTime(JsonObject f, const char *const *keys, const TimeKind *kinds, int n, time_t &out, TimeKind &kind) {
  out = 0; kind = T_NONE;
  for (int i = 0; i < n; i++) {
    const char *v = f[keys[i]] | "";
    if (v[0]) { const time_t t = parseIso(v); if (t > 1700000000) { out = t; kind = kinds[i]; return; } }
  }
}

bool parseFlights(Sink &sink, Slot *s, char *err, size_t errCap) {
  JsonDocument filter;
  for (const char *k : {"fa_flight_id", "ident", "status", "progress_percent", "arrival_delay", "cancelled", "scheduled_off", "estimated_off", "actual_off", "scheduled_out", "estimated_out",
                        "actual_out", "scheduled_on", "estimated_on", "actual_on", "scheduled_in", "estimated_in", "actual_in"})
    filter["flights"][0][k] = true;
  for (const char *e : {"origin", "destination"})
    for (const char *k : {"code_iata", "code_icao", "code", "city"}) filter["flights"][0][e][k] = true;
  JsonDocument doc(&gAlloc);
  if (deserializeJson(doc, (const char *)sink.buf, sink.len, DeserializationOption::Filter(filter))) { strlcpy(err, "Unreadable answer", errCap); return false; }
  JsonArray fl = doc["flights"];
  if (fl.isNull() || fl.size() == 0) { strlcpy(err, "No flight found", errCap); return false; }
  int pick = -1, i = 0;
  for (JsonObject f : fl) {   // the one that is in the air now: wheels up, not yet down
    if (strlen(f["actual_off"] | "") && !strlen(f["actual_on"] | "") && !strlen(f["actual_in"] | "") && !(f["cancelled"] | false)) { pick = i; break; }
    i++;
  }
  if (pick < 0) { i = 0; for (JsonObject f : fl) { const int p = f["progress_percent"] | -1; if (p > 0 && p < 100) { pick = i; break; } i++; } }
  if (pick < 0) pick = 0;
  JsonObject f = fl[pick];

  static const char *const depKeys[] = {"actual_off", "actual_out", "estimated_off", "estimated_out", "scheduled_off", "scheduled_out"};
  static const TimeKind depKinds[] = {T_ACTUAL, T_ACTUAL, T_ESTIMATED, T_ESTIMATED, T_SCHEDULED, T_SCHEDULED};
  static const char *const etaKeys[] = {"actual_on", "actual_in", "estimated_on", "estimated_in", "scheduled_on", "scheduled_in"};
  static const TimeKind etaKinds[] = {T_ACTUAL, T_ACTUAL, T_ESTIMATED, T_ESTIMATED, T_SCHEDULED, T_SCHEDULED};
  Info in = s->info;
  pickTime(f, depKeys, depKinds, 6, in.dep, in.depKind);
  pickTime(f, etaKeys, etaKinds, 6, in.eta, in.etaKind);
  in.progress = f["progress_percent"] | -1;
  in.delayMin = f["arrival_delay"].is<int>() ? (int16_t)((int)f["arrival_delay"] / 60) : INT16_MIN;
  trim(in.status, sizeof(in.status), f["status"] | "");
  for (int e = 0; e < 2; e++) {
    JsonObject o = f[e ? "destination" : "origin"];
    char *code = e ? in.dCode : in.oCode, *city = e ? in.dCity : in.oCity;
    const char *c = o["code_iata"] | "";
    if (!c[0]) c = o["code_icao"] | (o["code"] | "");
    trim(code, 5, c);
    trim(city, 24, o["city"] | "");
  }
  in.fa = FA_OK;
  in.err[0] = 0;
  const char *id = f["fa_flight_id"] | "";
  Lock l;
  s->info = in;
  strlcpy(s->faid, id, sizeof(s->faid));
  return true;
}

void doFlight(Slot *s) {
  const settings::Settings &S = settings::get();
  char url[200], key[40], cs[10], reg[10];
  strlcpy(key, FA_KEY_NOW, sizeof(key));
  { Lock l; strlcpy(cs, s->callsign, sizeof(cs)); strlcpy(reg, s->reg, sizeof(reg)); }
  const char *ident = cs[0] ? cs : reg;
  snprintf(url, sizeof(url), FA_BASE "/flights/%s?max_pages=1%s", ident, cs[0] ? "" : "&ident_type=registration");
  { Lock l; s->faTried = true; s->faAt = millis(); s->info.fa = FA_PENDING; s->info.err[0] = 0; bump(); }
  setStatus("Looking up %s...", ident);
  countCall();
  Sink sink(256 * 1024);
  char ua[48]; snprintf(ua, sizeof(ua), "ESP32-ADSB-Radar/%s", FW_VERSION);
  const int code = httpGet(url, "x-apikey", key, ua, sink, true);
  char err[44] = "";
  bool ok = false;
  if (code == 200) ok = parseFlights(sink, s, err, sizeof(err));
  else errText(code, sink, err, sizeof(err));
  if (!ok) { Lock l; s->info.fa = FA_ERROR; strlcpy(s->info.err, err, sizeof(s->info.err)); }
  setStatus("%s: %s", ident, ok ? "OK" : err);
  Serial.printf("[flight] %s -> %s%s (http %d)\n", ident, ok ? "ok" : "failed: ", ok ? "" : err, code);
  Lock l;
  bump();
}

void doTrack(Slot *s) {
  char url[200], key[40], faid[64];
  strlcpy(key, FA_KEY_NOW, sizeof(key));
  { Lock l; strlcpy(faid, s->faid, sizeof(faid)); s->trackTried = true; }
  snprintf(url, sizeof(url), FA_BASE "/flights/%s/track", faid);
  countCall();
  Sink sink(400 * 1024);
  char ua[48]; snprintf(ua, sizeof(ua), "ESP32-ADSB-Radar/%s", FW_VERSION);
  const int code = httpGet(url, "x-apikey", key, ua, sink, true);
  int kept = 0;
  float *la = nullptr, *lo = nullptr;
  if (code == 200) {
    JsonDocument filter;
    filter["positions"][0]["latitude"] = true;
    filter["positions"][0]["longitude"] = true;
    JsonDocument doc(&gAlloc);
    if (!deserializeJson(doc, (const char *)sink.buf, sink.len, DeserializationOption::Filter(filter))) {
      JsonArray pos = doc["positions"];
      const int n = pos.size();
      if (n >= 2) {
        const int stride = (n + MAX_TRACK - 1) / MAX_TRACK;
        la = (float *)heap_caps_malloc(MAX_TRACK * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        lo = (float *)heap_caps_malloc(MAX_TRACK * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (la && lo) {
          for (int i = 0; i < n && kept < MAX_TRACK; i += stride) {
            la[kept] = pos[i]["latitude"] | 0.0f;
            lo[kept] = pos[i]["longitude"] | 0.0f;
            if (la[kept] != 0.0f || lo[kept] != 0.0f) kept++;
          }
          if ((n - 1) % stride && kept < MAX_TRACK) { la[kept] = pos[n - 1]["latitude"] | 0.0f; lo[kept] = pos[n - 1]["longitude"] | 0.0f; kept++; }   // always end on the latest position
        }
      }
    }
  }
  Serial.printf("[flight] track %d points (http %d)\n", kept, code);
  Lock l;
  if (kept >= 2) {
    if (s->lat) heap_caps_free(s->lat);
    if (s->lon) heap_caps_free(s->lon);
    s->lat = la; s->lon = lo; s->info.trackPoints = kept;
  } else { if (la) heap_caps_free(la); if (lo) heap_caps_free(lo); }
  bump();
}

// ------------------------------------------------------------------ Planespotters
void photoUa(char *ua, size_t cap) { snprintf(ua, cap, "ESP32-ADSB-Radar/%s (contact: %s)", FW_VERSION, settings::get().faEmail); }

void doPhotoMeta(Slot *s) {
  char url[96], hex[8], ua[110];
  { Lock l; strlcpy(hex, s->hex, sizeof(hex)); s->photoTried = true; s->photoAt = millis(); s->info.photo = PH_PENDING; }
  for (char *p = hex; *p; p++) *p = toupper((unsigned char)*p);
  snprintf(url, sizeof(url), PS_BASE "/pub/photos/hex/%s", hex);
  photoUa(ua, sizeof(ua));
  Sink sink(32 * 1024);
  const int code = httpGet(url, nullptr, nullptr, ua, sink, true);
  PhotoState st = PH_ERROR;
  char src[110] = "", who[32] = "";
  if (code == 200) {
    JsonDocument filter;
    filter["photos"][0]["thumbnail"]["src"] = true;
    filter["photos"][0]["photographer"] = true;
    JsonDocument doc(&gAlloc);
    if (!deserializeJson(doc, (const char *)sink.buf, sink.len, DeserializationOption::Filter(filter))) {
      JsonArray ph = doc["photos"];
      if (ph.isNull() || ph.size() == 0) st = PH_ABSENT;
      else if (strlen(ph[0]["thumbnail"]["src"] | "") > 0 && strlen(ph[0]["thumbnail"]["src"] | "") < sizeof(src)) {
        strlcpy(src, ph[0]["thumbnail"]["src"] | "", sizeof(src));
        trim(who, sizeof(who), ph[0]["photographer"] | "");
        st = PH_PENDING;
      }
    }
  }
  Serial.printf("[photo] %s -> %s (http %d)\n", hex, st == PH_PENDING ? "found" : st == PH_ABSENT ? "none" : "error", code);
  Lock l;
  s->info.photo = st;
  if (st == PH_PENDING) { strlcpy(s->photoUrl, src, sizeof(s->photoUrl)); strlcpy(s->info.photographer, who, sizeof(s->info.photographer)); s->photoStage = 1; }
  bump();
}

void doPhotoImage(Slot *s) {
  char url[110], ua[110];
  { Lock l; strlcpy(url, s->photoUrl, sizeof(url)); }
  photoUa(ua, sizeof(ua));
  Sink sink(128 * 1024);
  const int code = httpGet(url, nullptr, nullptr, ua, sink, false);
  lgfx::LGFX_Sprite *spr = nullptr;
  int w = 0, h = 0;
  if (code == 200 && sink.len > 100) {
    uint8_t *rgb = jpegDecodeRgb(sink.buf, sink.len, w, h);
    if (rgb && w > 0 && h > 0 && w <= 440 && h <= 300) {
      // fit the picture into the card's photo box (PHOTO_MAX_W x PHOTO_MAX_H, never enlarged): a box filter over the source pixels each target pixel covers
      int ow = w, oh = h;
      if (w > PHOTO_MAX_W || h > PHOTO_MAX_H) {
        const float sc = std::min((float)PHOTO_MAX_W / w, (float)PHOTO_MAX_H / h);
        ow = std::max(1, (int)(w * sc)); oh = std::max(1, (int)(h * sc));
      }
      spr = new lgfx::LGFX_Sprite();
      spr->setPsram(true);
      spr->setColorDepth(16);
      if (spr->createSprite(ow, oh)) {
        for (int y = 0; y < oh; y++) {
          const int sy0 = y * h / oh, sy1 = std::max(sy0 + 1, (y + 1) * h / oh);
          for (int x = 0; x < ow; x++) {
            const int sx0 = x * w / ow, sx1 = std::max(sx0 + 1, (x + 1) * w / ow);
            int r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++)
              for (int sx = sx0; sx < sx1; sx++) { const uint8_t *p = rgb + (sy * w + sx) * 3; r += p[0]; g += p[1]; b += p[2]; n++; }
            spr->drawPixel(x, y, spr->color565(r / n, g / n, b / n));
          }
        }
        w = ow; h = oh;
      } else { delete spr; spr = nullptr; }
    }
    if (rgb) jpegFree(rgb);
  }
  Serial.printf("[photo] image %s (http %d, %u bytes, %dx%d)\n", spr ? "decoded" : "failed", code, (unsigned)sink.len, w, h);
  Lock l;
  if (spr) {
    if (s->spr) { s->spr->deleteSprite(); delete s->spr; }
    s->spr = spr; s->info.photo = PH_OK; s->info.photoW = w; s->info.photoH = h;
  } else s->info.photo = PH_ERROR;
  s->photoStage = 0;
  bump();
}

void doTest() {
  gTestPending = false;
  char key[40], url[160], ua[48];
  strlcpy(key, FA_KEY_NOW, sizeof(key));
  if (!key[0]) { Lock l; strlcpy(gTestText, "Enter the API key first", sizeof(gTestText)); bump(); return; }
  { Lock l; strlcpy(gTestText, "Testing...", sizeof(gTestText)); bump(); }
  snprintf(url, sizeof(url), FA_BASE "/account/usage");
  snprintf(ua, sizeof(ua), "ESP32-ADSB-Radar/%s", FW_VERSION);
  Sink sink(32 * 1024);
  const int code = httpGet(url, "x-apikey", key, ua, sink, true);
  char msg[48];
  if (code == 200) strlcpy(msg, "Key accepted", sizeof(msg)); else errText(code, sink, msg, sizeof(msg));
  Serial.printf("[flight] key test: %s (http %d)\n", msg, code);
  Lock l;
  strlcpy(gTestText, msg, sizeof(gTestText));
  bump();
}

}  // namespace

// ------------------------------------------------------------------ public API
void begin() { gMutex = xSemaphoreCreateMutex(); }

void request(const char *callsign, const char *hex, const char *reg) {
  if (!gMutex || !hex || !hex[0]) return;
  const settings::Settings &S = settings::get();
  if (!S.faEnabled) return;
  {
    Lock l;
    strlcpy(gReq.callsign, callsign ? callsign : "", sizeof(gReq.callsign));
    strlcpy(gReq.hex, hex, sizeof(gReq.hex));
    strlcpy(gReq.reg, reg ? reg : "", sizeof(gReq.reg));
    gReq.at = millis();
    gReq.valid = true;
    Slot *s = findSlot(gReq.callsign, gReq.hex);
    if (!s) s = newSlot(gReq.callsign, gReq.hex, gReq.reg);
    s->usedAt = millis();
    if (gReq.reg[0] && !s->reg[0]) strlcpy(s->reg, gReq.reg, sizeof(s->reg));
    if (s->faTried && millis() - s->faAt > FA_TTL_MS) { s->faTried = false; s->trackTried = false; }
    if (s->photoTried && s->info.photo != PH_OK && millis() - s->photoAt > (s->info.photo == PH_ABSENT ? PHOTO_TTL_MS : 300000)) s->photoTried = false;
    bump();
  }
  adsb::wake();
}

void cancel() { if (gMutex) { Lock l; gReq.valid = false; } }

bool get(const char *callsign, const char *hex, Info &out) {
  if (!gMutex) return false;
  Lock l;
  const Slot *s = findSlot(callsign ? callsign : "", hex ? hex : "");
  if (!s) return false;
  out = s->info;
  return true;
}

int copyTrack(const char *callsign, const char *hex, float *lat, float *lon, int maxPoints) {
  if (!gMutex) return 0;
  Lock l;
  const Slot *s = findSlot(callsign ? callsign : "", hex ? hex : "");
  if (!s || !s->lat || !s->lon) return 0;
  const int n = std::min(s->info.trackPoints, maxPoints);
  memcpy(lat, s->lat, n * sizeof(float));
  memcpy(lon, s->lon, n * sizeof(float));
  return n;
}

bool drawPhoto(lgfx::LGFX_Sprite *dst, int x, int y, const char *callsign, const char *hex) {
  if (!gMutex) return false;
  Lock l;
  const Slot *s = findSlot(callsign ? callsign : "", hex ? hex : "");
  if (!s || !s->spr) return false;
  s->spr->pushSprite(dst, x, y);
  return true;
}

uint32_t version() { return gVersion; }

bool wanted() {
  if (!gMutex) return false;
  if (gTestPending) return true;
  Lock l;
  return gReq.valid;
}

void serve() {
  if (!gMutex) return;
  if (gTestPending) { doTest(); return; }
  const settings::Settings &S = settings::get();
  Slot *s = nullptr;
  bool wantFa = false, wantTrack = false, wantMeta = false, wantImage = false;
  {
    Lock l;
    if (!gReq.valid) return;
    if (!S.faEnabled) { gReq.valid = false; return; }
    if (millis() - gReq.at < DEBOUNCE_MS) return;   // still deciding which aircraft to look at
    s = findSlot(gReq.callsign, gReq.hex);
    if (!s) { gReq.valid = false; return; }
    wantFa = keySet() && (s->callsign[0] || s->reg[0]) && !s->faTried;
    wantTrack = S.faTrack && keySet() && s->info.fa == FA_OK && s->faid[0] && !s->trackTried;
    wantMeta = S.faPhoto && emailSet() && validHex(s->hex) && !s->photoTried;
    wantImage = s->photoStage == 1;
    if (!wantFa && !wantTrack && !wantMeta && !wantImage) { gReq.valid = false; return; }   // nothing (more) to do for this aircraft
  }
  if (WiFi.status() != WL_CONNECTED || !adsb::timeValid()) return;
  if ((wantFa || wantTrack) && !budget()) {
    setStatus("Hourly limit reached (%d calls)", FA_MAX_CALLS_PER_HOUR);
    Lock l; s->faTried = s->trackTried = true; if (wantFa) { s->info.fa = FA_ERROR; strlcpy(s->info.err, "Hourly call limit reached", sizeof(s->info.err)); } bump();
    return;
  }
  // one exchange per pass: the data task goes back to its aircraft fetch in between
  if (wantFa) doFlight(s);
  else if (wantTrack) doTrack(s);
  else if (wantMeta) doPhotoMeta(s);
  else if (wantImage) doPhotoImage(s);
}

void flush() {
  if (!gMutex) return;
  Lock l;
  for (Slot &s : gSlots) { freeSlotData(s); memset(&s, 0, sizeof(s)); }
  gReq.valid = false;
  gStatusText[0] = 0;
  bump();
}

Status status() {
  Status st = {};
  const settings::Settings &S = settings::get();
  st.enabled = S.faEnabled; st.keySet = keySet(); st.emailSet = emailSet();
  if (gMutex) {
    Lock l;
    strlcpy(st.text, gStatusText, sizeof(st.text));
    strlcpy(st.test, gTestText, sizeof(st.test));
  }
  if (!st.text[0]) strlcpy(st.text, !S.faEnabled ? "Off" : !st.keySet ? "Enter the API key" : "Ready: tap an aircraft", sizeof(st.text));
  st.callsHour = gCallsHour; st.callsTotal = gCallsTotal;
  return st;
}

void requestTest() { gTestPending = true; adsb::wake(); }

}  // namespace flightinfo
