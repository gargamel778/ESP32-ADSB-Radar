#include "adsb.h"
#include "settings.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>
#include <time.h>
#include "phase.h"
#include "ota.h"
#include "flightinfo.h"

// The full Mozilla root bundle is compiled into the Arduino core; these are its linker symbols.
extern const uint8_t caBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t caBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace adsb {
namespace {

// ArduinoJson's many small allocations would all land in internal RAM (the scarce kind); this puts the parsed document in PSRAM.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); return p ? p : malloc(n); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); return q ? q : realloc(p, n); }
};
PsramAllocator gJsonAlloc;

constexpr size_t MAX_BODY = 600 * 1024;  // refuse absurd payloads (a busy 250 NM circle is ~300 KB)

TaskHandle_t gTask = nullptr;
SemaphoreHandle_t gMutex;
Snapshot *gShared = nullptr;   // published to the UI thread (guarded by gMutex)
Snapshot *gWork = nullptr;     // being filled by the fetch task
// Both live in PSRAM (11 KB each): internal RAM is the scarce resource (TLS handshake, Wi-Fi, web server later).
volatile bool gDirty = false;
volatile bool gBusy = false;

// ---- route cache and request slot
constexpr int ROUTE_CACHE = 24;
struct RouteEntry { char cs[10]; RouteState state; RouteInfo info; uint32_t at; };
RouteEntry gRoutes[ROUTE_CACHE];
char gRouteWanted[10] = "";
volatile uint32_t gRouteVer = 0;
constexpr uint32_t NOTIFY_FETCH = 1, NOTIFY_ROUTE = 2;

RouteEntry *findRoute(const char *cs) {   // caller holds gMutex
  for (RouteEntry &e : gRoutes) if (e.state != ROUTE_NONE && !strcmp(e.cs, cs)) return &e;
  return nullptr;
}
RouteEntry *newRoute(const char *cs) {   // oldest entry is recycled
  RouteEntry *best = &gRoutes[0];
  for (RouteEntry &e : gRoutes) { if (e.state == ROUTE_NONE) { best = &e; break; } if (e.at < best->at) best = &e; }
  memset(best, 0, sizeof(*best));
  strlcpy(best->cs, cs, sizeof(best->cs));
  best->at = millis();
  return best;
}


void trimCopy(char *dst, size_t cap, const char *src) {
  strlcpy(dst, src ? src : "", cap);
  for (int i = strlen(dst) - 1; i >= 0 && dst[i] == ' '; i--) dst[i] = 0;
}

struct Candidate { Aircraft a; float d; };  // aircraft plus approximate distance from the centre, for keeping the nearest

// One attempt. Returns true on success (gWork filled). On failure sets code / status text / Retry-After.
bool fetchOnce(int &code, char *status, size_t statusCap, uint32_t &retryAfterS) {
  retryAfterS = 0;
  const settings::Settings &S = settings::get();
  const double lat0 = S.lat, lon0 = S.lon;
  char url[160];
  snprintf(url, sizeof(url), "https://api.airplanes.live/v2/point/%.4f/%.4f/%d", lat0, lon0, settings::apiRadiusNm());

  WiFiClientSecure client;
  client.setCACertBundle(caBundleStart, caBundleEnd - caBundleStart);  // validate the server certificate (clock is valid at this point)
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.setReuse(false);
  const char *keep[] = {"Retry-After"};
  http.collectHeaders(keep, 1);
  if (!http.begin(client, url)) { code = -100; strlcpy(status, "Request error", statusCap); return false; }
  http.addHeader("User-Agent", "ESP32-ADSB-Radar/0.2");
  code = http.GET();
  if (code != 200) {
    if (code == 429) { retryAfterS = http.header("Retry-After").toInt(); snprintf(status, statusCap, "Rate limited (429)"); }
    else if (code > 0) snprintf(status, statusCap, "Server error (HTTP %d)", code);
    else {
      char err[96] = "";
      client.lastError(err, sizeof(err));
      if (strstr(err, "X509") || strstr(err, "ertificate")) strlcpy(status, "Certificate error", statusCap);
      else if (code == HTTPC_ERROR_READ_TIMEOUT) strlcpy(status, "Timeout", statusCap);
      else strlcpy(status, "Connection failed", statusCap);
      Serial.printf("[adsb] http=%d tls='%s'\n", code, err);
    }
    http.end();
    return false;
  }
  int len = http.getSize();
  if (len > (int)MAX_BODY) { http.end(); code = -102; strlcpy(status, "Response too large", statusCap); return false; }
  // The body is read here, not with http.getString(): the library's loops poll the socket with yield(), which never lets the idle task run, and on a slow link
  // (a few kB/s, a big payload) that starves the core-0 idle task for the whole transfer until its 15 s watchdog restarts the radar. This loop sleeps between polls.
  struct Raw { char *p = nullptr; ~Raw() { if (p) heap_caps_free(p); } } owned;
  String chunked;   // only when the server does not announce the length
  const char *raw = nullptr;
  size_t rawLen = 0;
  if (len > 0) {
    owned.p = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!owned.p) { http.end(); code = -103; strlcpy(status, "Out of memory", statusCap); return false; }
    WiFiClient *stream = http.getStreamPtr();
    uint32_t lastData = millis();
    while (rawLen < (size_t)len && (http.connected() || stream->available())) {
      const int avail = stream->available();
      if (avail <= 0) { if (millis() - lastData > 12000) break; vTaskDelay(pdMS_TO_TICKS(3)); continue; }
      rawLen += stream->readBytes(owned.p + rawLen, std::min<size_t>(avail, len - rawLen));
      lastData = millis();
    }
    if (rawLen != (size_t)len) { http.end(); code = HTTPC_ERROR_READ_TIMEOUT; strlcpy(status, "Timeout", statusCap); return false; }
    owned.p[rawLen] = 0;
    raw = owned.p;
  } else {
    chunked = http.getString();
    raw = chunked.c_str();
    rawLen = chunked.length();
  }
  http.end();
  gWork->rawBytes = rawLen;
  if (rawLen > MAX_BODY) { code = -102; strlcpy(status, "Response too large", statusCap); return false; }

  // Keep only the fields we use: this is what makes busy airspace safe.
  JsonDocument filter;
  for (const char *k : {"hex", "lat", "lon", "flight", "alt_baro", "alt_geom", "gs", "track", "t", "r", "desc", "squawk", "category"}) filter["ac"][0][k] = true;
  JsonDocument doc(&gJsonAlloc);
  DeserializationError err = deserializeJson(doc, raw, rawLen, DeserializationOption::Filter(filter));
  if (err) { Serial.printf("[adsb] JSON error: %s\n", err.c_str()); code = -101; strlcpy(status, "Bad data from server", statusCap); return false; }

  static std::vector<Candidate> cand;  // reused between fetches; lives in PSRAM once it grows
  cand.clear();
  const double kmPerDegLat = 110.574, kmPerDegLon = 111.320 * cos(lat0 * M_PI / 180.0);
  for (JsonObject o : doc["ac"].as<JsonArray>()) {
    if (o["lat"].isNull() || o["lon"].isNull()) continue;  // no position (e.g. Mode-S only)
    Candidate c = {};
    Aircraft &a = c.a;
    a.lat = o["lat"].as<double>();
    a.lon = o["lon"].as<double>();
    JsonVariant ab = o["alt_baro"];
    if (ab.is<int>()) a.altFt = ab.as<int>();                    // number
    else if (ab.is<const char *>()) a.altFt = 0;                 // "ground"
    else if (o["alt_geom"].is<int>()) a.altFt = o["alt_geom"].as<int>();
    else a.altFt = -1;
    a.gs = o["gs"] | 0.0f;
    a.track = o["track"] | -1.0f;
    trimCopy(a.hex, sizeof(a.hex), o["hex"] | "");
    trimCopy(a.flight, sizeof(a.flight), o["flight"] | "");
    {   // transponders without a callsign send "@@@@@@@@" (or underscores): that is "none", not a name
      bool junk = true;
      for (const char *c = a.flight; *c; c++) if (*c != '@' && *c != '_' && *c != ' ') junk = false;
      if (junk) a.flight[0] = 0;
    }
    trimCopy(a.type, sizeof(a.type), o["t"] | "");
    trimCopy(a.reg, sizeof(a.reg), o["r"] | "");
    trimCopy(a.squawk, sizeof(a.squawk), o["squawk"] | "");
    trimCopy(a.desc, sizeof(a.desc), o["desc"] | "");
    trimCopy(a.cat, sizeof(a.cat), o["category"] | "");
    double dx = (a.lon - lon0) * kmPerDegLon, dy = (a.lat - lat0) * kmPerDegLat;
    c.d = (float)sqrt(dx * dx + dy * dy);
    cand.push_back(c);
  }
  gWork->totalInRange = cand.size();
  if (cand.size() > (size_t)MAX_AIRCRAFT) {  // far more traffic than we can show: keep the nearest
    std::nth_element(cand.begin(), cand.begin() + MAX_AIRCRAFT, cand.end(), [](const Candidate &a, const Candidate &b) { return a.d < b.d; });
    cand.resize(MAX_AIRCRAFT);
  }
  gWork->n = cand.size();
  for (size_t i = 0; i < cand.size(); i++) gWork->ac[i] = cand[i].a;
  strlcpy(status, "OK", statusCap);
  return true;
}

// One lookup. The adsbdb answer is {"response":{"flightroute":{"origin":{...},"destination":{...}}}} or {"response":"unknown callsign"}.
void doRoute() {
  char cs[10];
  xSemaphoreTake(gMutex, portMAX_DELAY);
  strlcpy(cs, gRouteWanted, sizeof(cs));
  gRouteWanted[0] = 0;
  xSemaphoreGive(gMutex);
  if (!cs[0] || WiFi.status() != WL_CONNECTED || !timeValid()) return;

  char url[64];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);
  WiFiClientSecure client;
  client.setCACertBundle(caBundleStart, caBundleEnd - caBundleStart);
  client.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  http.setReuse(false);
  RouteState result = ROUTE_NONE;
  RouteInfo info = {};
  if (http.begin(client, url)) {
    http.addHeader("User-Agent", "ESP32-ADSB-Radar/0.5");
    const int code = http.GET();
    if (code == 200 || code == 404) {
      JsonDocument filter;
      for (const char *k : {"origin", "destination"}) { filter["response"]["flightroute"][k]["iata_code"] = true; filter["response"]["flightroute"][k]["municipality"] = true; }
      JsonDocument doc(&gJsonAlloc);
      if (!deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter))) {
        JsonObject fr = doc["response"]["flightroute"];
        if (!fr.isNull()) {
          trimCopy(info.oCode, sizeof(info.oCode), fr["origin"]["iata_code"] | "");
          trimCopy(info.oCity, sizeof(info.oCity), fr["origin"]["municipality"] | "");
          trimCopy(info.dCode, sizeof(info.dCode), fr["destination"]["iata_code"] | "");
          trimCopy(info.dCity, sizeof(info.dCity), fr["destination"]["municipality"] | "");
          result = info.oCode[0] && info.dCode[0] ? ROUTE_KNOWN : ROUTE_UNKNOWN;
        } else result = ROUTE_UNKNOWN;   // "unknown callsign"
      }
    }
    http.end();
  }
  Serial.printf("[route] %s -> %s%s%s%s\n", cs, result == ROUTE_KNOWN ? info.oCode : result == ROUTE_UNKNOWN ? "unknown" : "no answer", result == ROUTE_KNOWN ? " > " : "", result == ROUTE_KNOWN ? info.dCode : "", "");
  xSemaphoreTake(gMutex, portMAX_DELAY);
  RouteEntry *e = findRoute(cs);
  if (!e) e = newRoute(cs);
  e->state = result == ROUTE_NONE ? ROUTE_UNKNOWN : result;   // a failed attempt counts as unknown until the entry ages out
  e->info = info;
  e->at = millis();
  gRouteVer = gRouteVer + 1;
  xSemaphoreGive(gMutex);
}

void task(void *) {
  uint32_t fails = 0;
  for (;;) {
    if (ota::busy()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }   // a firmware update owns PSRAM and the screen
    uint32_t t0 = millis();
    bool ok = false, attempted = false;
    int code = 0;
    uint32_t retryAfterS = 0;
    char status[40] = "";
    if (WiFi.status() != WL_CONNECTED) strlcpy(status, "No Wi-Fi", sizeof(status));
    else if (!timeValid()) strlcpy(status, "Waiting for time sync", sizeof(status));  // certificates cannot be checked without a date
    else {
      attempted = true;
      gBusy = true;
      phase::set(0, "fetch");
      ok = fetchOnce(code, status, sizeof(status), retryAfterS);
      phase::set(0, "idle");
      gBusy = false;
    }
    uint32_t dur = millis() - t0;

    xSemaphoreTake(gMutex, portMAX_DELAY);
    if (ok) {
      *gShared = *gWork;  // whole snapshot (n + aircraft + rawBytes)
      gShared->fetchedAtMs = millis();
      fails = 0;
    } else if (attempted) {
      fails++;
    }
    gShared->ok = ok;
    gShared->httpCode = code;
    strlcpy(gShared->status, status, sizeof(gShared->status));
    gShared->failStreak = fails;
    gShared->fetchMs = dur;
    gDirty = true;
    xSemaphoreGive(gMutex);

    if (attempted)
      Serial.printf("[adsb] %s http=%d n=%d/%u bytes=%u fetch=%ums fails=%u status='%s' heap=%uK\n", ok ? "ok" : "FAIL", code, ok ? gWork->n : -1, (unsigned)(ok ? gWork->totalInRange : 0),
                    (unsigned)gWork->rawBytes, (unsigned)dur, (unsigned)fails, status, (unsigned)(ESP.getFreeHeap() / 1024));

    // Next attempt: normal cadence after success; exponential backoff (5,10,20,40,60 s) after failures; honour Retry-After; poll quickly while waiting for Wi-Fi/time.
    uint32_t waitMs;
    if (ok) waitMs = FETCH_INTERVAL_MS;
    else if (!attempted) waitMs = 1000;
    else {
      waitMs = min<uint32_t>(5000u << min<uint32_t>(fails - 1, 4), 60000u);
      if (retryAfterS) waitMs = max<uint32_t>(waitMs, min<uint32_t>(retryAfterS, 300u) * 1000u);
      waitMs += esp_random() % (waitMs / 8 + 1);  // jitter, so a flock of radars does not retry in lockstep
    }
    // sleep until the next fetch is due; fetchNow() wakes it early, and route lookups are served in between
    const uint32_t due = t0 + waitMs;
    for (;;) {
      if (gRouteWanted[0]) { phase::set(0, "route"); doRoute(); phase::set(0, "idle"); }
      const bool fi = flightinfo::wanted();
      if (fi) { phase::set(0, "flight"); flightinfo::serve(); phase::set(0, "idle"); }   // one HTTPS exchange, then back to the aircraft fetch if it is due
      const int32_t left = (int32_t)(due - millis());
      if (left <= 0) break;
      uint32_t bits = 0;
      xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, pdMS_TO_TICKS(fi ? std::min<int32_t>(left, 200) : std::min<int32_t>(left, 1000)));
      if (bits & NOTIFY_FETCH) break;
    }
  }
}
}  // namespace

Snapshot *newSnapshot() {
  void *p = heap_caps_calloc(1, sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = calloc(1, sizeof(Snapshot));
  return (Snapshot *)p;
}

void fetchNow() { if (gTask) xTaskNotify(gTask, NOTIFY_FETCH, eSetBits); }
void wake() { if (gTask) xTaskNotify(gTask, NOTIFY_ROUTE, eSetBits); }

void requestRoute(const char *callsign) {
  if (!callsign || !callsign[0] || !gMutex) return;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  RouteEntry *e = findRoute(callsign);
  const bool fresh = e && (e->state == ROUTE_PENDING || millis() - e->at < (e->state == ROUTE_KNOWN ? 3600000u : 600000u));   // known routes for an hour, misses for 10 minutes
  if (!fresh) {
    if (!e) e = newRoute(callsign);
    e->state = ROUTE_PENDING;
    e->at = millis();
    strlcpy(gRouteWanted, callsign, sizeof(gRouteWanted));
  }
  xSemaphoreGive(gMutex);
  if (!fresh && gTask) xTaskNotify(gTask, NOTIFY_ROUTE, eSetBits);
}

RouteState route(const char *callsign, RouteInfo &out) {
  RouteState st = ROUTE_NONE;
  if (!callsign || !gMutex) return st;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  const RouteEntry *e = findRoute(callsign);
  if (e) { st = e->state; out = e->info; }
  xSemaphoreGive(gMutex);
  return st;
}

uint32_t routeVersion() { return gRouteVer; }

bool timeValid() { return time(nullptr) > 1700000000; }

void begin() {
  flightinfo::begin();
  gMutex = xSemaphoreCreateMutex();
  gShared = newSnapshot();
  gWork = newSnapshot();
  strlcpy(gShared->status, "Starting", sizeof(gShared->status));
  xTaskCreatePinnedToCore(task, "adsb", 20480, nullptr, 1, &gTask, 0);
}

bool take(Snapshot &out) {
  if (!gDirty) return false;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  out = *gShared;
  gDirty = false;
  xSemaphoreGive(gMutex);
  return true;
}

bool busy() { return gBusy; }

}  // namespace adsb
