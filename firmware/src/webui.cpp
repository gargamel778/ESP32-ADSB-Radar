#include "webui.h"
extern const uint8_t pageStart[] asm("_binary_data_page_html_gz_start");
extern const uint8_t pageEnd[] asm("_binary_data_page_html_gz_end");
#include "app.h"
#include "radar.h"
#include "settings.h"
#include "wifimgr.h"
#include "palette.h"
#include "units.h"
#include <WebServer.h>
#include <lwip/sockets.h>
#include <strings.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <algorithm>
#include "geo.h"
#include "sdlog.h"
#include "ota.h"
#include "night.h"
#include "flightinfo.h"
#include "config.h"
#include "display.h"

extern const uint8_t leafletJsStart[] asm("_binary_data_leaflet_js_gz_start");
extern const uint8_t leafletJsEnd[] asm("_binary_data_leaflet_js_gz_end");
extern const uint8_t leafletCssStart[] asm("_binary_data_leaflet_css_gz_start");
extern const uint8_t leafletCssEnd[] asm("_binary_data_leaflet_css_gz_end");

namespace webui {
namespace {

// The library parses a request in one blocking go (every read waits up to 5 s for the next byte), so a client that sends half a request and then
// stalls, or trickles bytes, would hold the whole main loop (display included) until the 15 s loop watchdog restarts the radar. This subclass starts
// the parse only when the request has arrived: all headers, and a small body in full (found by peeking at the socket; nothing is consumed). A client
// that does not finish its request within 3 s is dropped. Bodies over 4000 bytes (firmware uploads) are read the library's way.
class GuardedServer : public WebServer {
 public:
  using WebServer::WebServer;
  void handleGuarded() {
    if (_currentStatus == HC_NONE) {   // accept here: the library would start parsing in the very same call
      _currentClient = _server.accept();
      if (!_currentClient) {
        if (_nullDelay) delay(1);
        return;
      }
      _currentStatus = HC_WAIT_READ;
      _statusChange = millis();
    }
    if (_currentStatus == HC_WAIT_READ && _currentClient.connected() && !requestArrived()) {
      if (millis() - _statusChange > 3000) { _currentClient = NetworkClient(); _currentStatus = HC_NONE; }
      return;
    }
    handleClient();
  }

 private:
  // lwIP's MSG_PEEK shows only the first received segment, so it finds the end of the headers (browsers and curl send them in one segment), while the
  // total that has arrived (available()) tells whether a small body, which many clients send as a second segment, is complete.
  bool requestArrived() {
    static uint8_t pk[1536];
    const int n = recv(_currentClient.fd(), pk, sizeof(pk), MSG_PEEK | MSG_DONTWAIT);
    int hdrEnd = -1;
    for (int i = 3; i < n && hdrEnd < 0; i++)
      if (pk[i] == '\n' && pk[i - 1] == '\r' && pk[i - 2] == '\n' && pk[i - 3] == '\r') hdrEnd = i + 1;
    if (hdrEnd < 0) return n >= (int)sizeof(pk) - 1 && _currentClient.available() > n;   // a huge header block: let the library deal with it
    for (int i = 0; i + 15 < hdrEnd; i++) {
      if ((i == 0 || pk[i - 1] == '\n') && strncasecmp((const char *)pk + i, "Content-Length:", 15) == 0) {
        const long len = strtol((const char *)pk + i + 15, nullptr, 10);
        if (len > 0 && len <= 4000 && (long)_currentClient.available() < hdrEnd + len) return false;   // bigger bodies (firmware) are read the library's way
        break;
      }
    }
    return true;
  }
};

GuardedServer server(80);
DNSServer dns;
bool gDnsOn = false, gMdns = false, gBegun = false;
char gToken[17] = "";              // session token handed out at login (new every boot)
uint32_t gLockUntil = 0;
int gBadPins = 0;

bool viaAp() { return wifimgr::apActive() && server.client().localIP() == WiFi.softAPIP(); }

bool authorized() {
  if (viaAp()) return true;   // joined our WPA2 access point
  if (server.hasHeader("Cookie")) {
    const String c = server.header("Cookie");
    const int i = c.indexOf("sid=");
    if (i >= 0 && c.substring(i + 4, i + 20) == String(gToken)) return true;
  }
  return false;
}

bool need() {
  if (authorized()) return true;
  server.send(401, "application/json", "{\"error\":\"login\"}");
  return false;
}


// WebServer's own writer gives up after ~10 quick retries when the TCP send buffer is full, which cuts big pages short on a busy link
// (browser: ERR_CONTENT_LENGTH_MISMATCH, script never runs). This one keeps going until everything is out or the client is gone.
void sendBig(const char *type, const char *data, size_t len, int code = 200, bool gzip = false, const char *cache = "no-store") {
  server.setContentLength(len);
  server.sendHeader("Cache-Control", cache);
  if (gzip) server.sendHeader("Content-Encoding", "gzip");
  server.send(code, type, "");   // headers only
  NetworkClient &c = server.client();
  uint32_t last = millis();
  size_t off = 0;
  while (off < len && c.connected()) {
    feedLoopWDT();   // a slow client may keep us here for a while
    const size_t w = c.write((const uint8_t *)data + off, std::min<size_t>(1024, len - off));
    if (w) { off += w; last = millis(); }
    else if (millis() - last > 10000) break;
    else delay(3);
  }
  if (off < len) Serial.printf("[web] page cut short: %u/%u bytes\n", (unsigned)off, (unsigned)len);
}

#ifdef DEV_SCREENSHOT
// Developer builds only (-DDEV_SCREENSHOT): the picture on the screen as raw 800x480 RGB565, little-endian. tools/devtools/radar_screen.py turns it into a PNG.
void handleScreen() {
  if (!need()) return;
  lgfx::LGFX_Sprite *fb = display::doubleBuffered() ? display::frontFb() : nullptr;
  if (!fb) { server.send(409, "application/json", "{\"error\":\"no frame buffer in this display mode\"}"); return; }
  sendBig("application/octet-stream", (const char *)fb->getBuffer(), 800 * 480 * 2);
}
#endif

void sendJson(const JsonDocument &d, int code = 200) {
  String out;
  serializeJson(d, out);
  sendBig("application/json", out.c_str(), out.length(), code);
}

void sendOk() { server.send(200, "application/json", "{\"ok\":true}"); }

void captive() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// Phones check for internet with a known URL. While setup is pending we redirect them to the setup page (that is what raises the sign-in sheet).
// Once the radar has joined the home network we give the answer each system expects from an open network, so the sheet closes by itself.
void probe(const char *path) {
  if (!wifimgr::apActive()) { server.send(404, "text/plain", "not found"); return; }
  if (!wifimgr::setupComplete()) { captive(); return; }
  if (!strcmp(path, "/generate_204") || !strcmp(path, "/gen_204")) server.send(204);   // Android
  else if (!strcmp(path, "/connecttest.txt")) server.send(200, "text/plain", "Microsoft Connect Test");   // Windows
  else if (!strcmp(path, "/ncsi.txt")) server.send(200, "text/plain", "Microsoft NCSI");
  else if (!strcmp(path, "/success.txt")) server.send(200, "text/plain", "success\n");   // Firefox
  else server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");   // Apple
}

void handleLogin() {
  JsonDocument d;
  deserializeJson(d, server.arg("plain"));
  if (millis() < gLockUntil) { server.send(429, "application/json", "{\"error\":\"Too many tries, wait a minute\"}"); return; }
  if (strcmp(d["pin"] | "", settings::get().webPin) == 0) {
    gBadPins = 0;
    server.sendHeader("Set-Cookie", String("sid=") + gToken + "; Path=/; SameSite=Strict");
    sendOk();
    return;
  }
  if (++gBadPins >= 5) { gLockUntil = millis() + 60000; gBadPins = 0; }
  server.send(403, "application/json", "{\"error\":\"Wrong PIN\"}");
}

void handleState() {
  if (!need()) return;
  const settings::Settings &S = settings::get();
  JsonDocument d;
  d["version"] = FW_VERSION;
  { JsonObject fw = d["fw"].to<JsonObject>(); fw["slot"] = ota::slot(); fw["state"] = ota::imageState(); fw["canRevert"] = ota::canRevert(); const ota::Status os = ota::status(); fw["active"] = os.active; fw["pct"] = os.pct; fw["failed"] = os.failed; fw["msg"] = String(os.msg); }
  d["uptime"] = millis() / 1000;
  d["heapKb"] = ESP.getFreeHeap() / 1024;
  JsonObject w = d["wifi"].to<JsonObject>();
  w["connected"] = wifimgr::connected();
  w["ssid"] = wifimgr::ssid();
  w["rssi"] = wifimgr::rssi();
  w["ip"] = wifimgr::ip();
  w["ap"] = wifimgr::apActive();
  const radar::Stats rs = radar::stats();
  JsonObject r = d["radar"].to<JsonObject>();
  r["aircraft"] = rs.aircraft;
  r["dataOk"] = rs.dataOk;
  r["paused"] = radar::paused();
  { char eb[24]; const int ne = radar::emergencies(eb, sizeof(eb)); r["emergencies"] = ne; r["emergency"] = String(eb); }
  d["night"] = night::active();
  { const flightinfo::Status fs = flightinfo::status(); JsonObject f = d["fa"].to<JsonObject>();   // the key itself is never sent back
    f["enabled"] = fs.enabled; f["keySet"] = fs.keySet; f["email"] = String(S.faEmail); f["photo"] = S.faPhoto; f["track"] = S.faTrack;
    f["status"] = String(fs.text); f["test"] = String(fs.test); f["calls"] = fs.callsHour; f["callsTotal"] = fs.callsTotal; }
  { JsonObject h = d["health"].to<JsonObject>(); h["heapMinKb"] = ESP.getMinFreeHeap() / 1024; h["psramKb"] = ESP.getFreePsram() / 1024; h["resetReason"] = (int)esp_reset_reason(); h["backlight"] = night::brightnessPct(); h["vsync"] = display::vsyncCount(); h["fps"] = rs.fps; h["dataAgeS"] = radar::dataAgeS(); }
  JsonObject u = d["units"].to<JsonObject>();
  for (int i = 0; i < units::DIST_N; i++) u["dist"].add(units::name((units::Dist)i));
  for (int i = 0; i < units::ALT_N; i++) u["alt"].add(units::name((units::Alt)i));
  for (int i = 0; i < units::SPD_N; i++) u["spd"].add(units::name((units::Spd)i));
  for (int i = 0; i < units::RANGE_STEPS; i++) u["ranges"].add(units::rangeSteps((units::Dist)S.dist)[i]);
  JsonArray sw = d["swatches"].to<JsonArray>();
  for (int i = 0; i < palette::COUNT; i++) {
    JsonObject o = sw.add<JsonObject>();
    o["name"] = palette::kSwatches[i].name;
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", palette::kSwatches[i].r, palette::kSwatches[i].g, palette::kSwatches[i].b);
    o["hex"] = hex;
  }
  JsonArray th = d["themes"].to<JsonArray>();
  th.add("Sans"); th.add("MonoBold"); th.add("DejaVu");
  JsonObject s = d["s"].to<JsonObject>();
  s["rangeStep"] = S.rangeStep;
  s["dist"] = S.dist; s["alt"] = S.alt; s["spd"] = S.spd;
  s["clock24"] = S.clock24;
  s["unitsAuto"] = S.unitsAuto;
  s["theme"] = radar::themeIndex();
  for (int i = 0; i < palette::ROLES; i++) s["pal"].add(S.pal[i]);
  s["brightness"] = S.brightness;
  s["nightMode"] = S.nightMode; s["nightBright"] = S.nightBright;
  s["trail"] = S.trail;
  s["showDiag"] = S.showDiag;
  s["mapOverlay"] = S.mapOverlay;
  s["sdLog"] = S.sdLog; s["logSecs"] = S.logSecs;
  { const sdlog::Status st = sdlog::status(); JsonObject sd = d["sd"].to<JsonObject>(); sd["state"] = (int)st.state; sd["totalMB"] = (uint32_t)(st.totalBytes >> 20); sd["freeMB"] = (uint32_t)(st.freeBytes >> 20); sd["rows"] = st.rowsToday; sd["bytes"] = st.bytesToday; sd["dropped"] = st.dropped; sd["file"] = st.file; }
  s["lat"] = S.lat; s["lon"] = S.lon;
  s["place"] = S.place; s["country"] = S.country; s["tz"] = S.tzPosix;
  s["tzName"] = S.tzName;
  s["countryName"] = geo::countryName(geo::countryByCode(S.country));
  s["countryAuto"] = S.countryAuto; s["tzAuto"] = S.tzAuto;
  sendJson(d);
}

void handleSet() {
  if (!need()) return;
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
  const settings::Settings &S = settings::get();
  if (d["unitsAuto"].is<bool>()) app::setUnitsAuto(d["unitsAuto"]);
  if (d["dist"].is<int>()) app::setDist(d["dist"]);
  if (d["alt"].is<int>()) app::setAlt(d["alt"]);
  if (d["spd"].is<int>()) app::setSpd(d["spd"]);
  if (d["clock24"].is<bool>()) app::setClock24(d["clock24"]);
  if (d["rangeStep"].is<int>()) app::setRangeStep(d["rangeStep"]);
  if (d["trail"].is<int>()) { int t = d["trail"]; for (int i = 0; i < settings::TRAIL_N; i++) if (settings::kTrails[i] == t) app::setTrail(t); }
  if (d["theme"].is<int>()) app::setTheme(d["theme"]);
  if (d["pal"].is<JsonArray>()) { int i = 0; for (JsonVariant v : d["pal"].as<JsonArray>()) { if (i < palette::ROLES && v.is<int>()) app::setPalette(i, v); i++; } }
  if (d["brightness"].is<int>()) app::setDayBrightness(d["brightness"]);
  if (d["nightBright"].is<int>()) app::setNightBrightness(d["nightBright"]);
  if (d["nightMode"].is<int>()) app::setNightMode(d["nightMode"]);
  if (d["showDiag"].is<bool>()) app::setShowDiag(d["showDiag"]);
  if (d["mapOverlay"].is<bool>()) app::setMapOverlay(d["mapOverlay"]);
  if (d["faEnabled"].is<bool>()) app::setFlightEnabled(d["faEnabled"]);
  if (d["faKey"].is<const char *>()) app::setFlightKey(d["faKey"]);
  if (d["faEmail"].is<const char *>()) app::setFlightEmail(d["faEmail"]);
  if (d["faPhoto"].is<bool>()) app::setFlightPhoto(d["faPhoto"]);
  if (d["faTrack"].is<bool>()) app::setFlightTrack(d["faTrack"]);
  if (d["sdLog"].is<bool>()) app::setSdLog(d["sdLog"]);
  if (d["logSecs"].is<int>()) app::setLogSecs(d["logSecs"]);
  if (d["sdEject"].is<bool>()) { if (d["sdEject"]) app::sdEject(); else app::sdMount(); }
  if (d["paused"].is<bool>()) radar::setPaused(d["paused"]);
  // country / zone: "auto" follows the location, anything else is a choice made by hand (set before the location so the new position respects it)
  if (d["country"].is<const char *>()) { const char *c = d["country"]; if (!strcmp(c, "auto")) app::setCountryAuto(true); else if (strlen(c) == 2 && geo::countryByCode(c) >= 0) app::setCountryManual(c); }
  if (d["zone"].is<const char *>()) { const char *z = d["zone"]; if (!strcmp(z, "auto")) app::setZoneAuto(true); else { const int zi = geo::zoneByName(z); if (zi >= 0) app::setZoneManual(zi); } }
  if (d["loc"].is<JsonObject>()) {
    JsonObject l = d["loc"];
    app::setLocationAuto(l["lat"] | S.lat, l["lon"] | S.lon, l["place"] | "");
  }
  sendOk();
}

void handleScan() {
  if (!need()) return;
  if (server.hasArg("start")) {
    // a phone on the setup access point loses its connection for a few seconds during a scan: reuse a recent list unless asked for a fresh one
    const bool reuse = wifimgr::apClients() > 0 && wifimgr::scanCount() > 0 && wifimgr::scanAgeMs() < 90000 && !server.hasArg("fresh");
    if (!reuse) wifimgr::startScan(400);   // deferred a little: the answer to this request leaves before the radio starts hopping channels
  }
  JsonDocument d;
  d["scanning"] = wifimgr::scanning();
  JsonArray a = d["nets"].to<JsonArray>();
  for (int i = 0; i < wifimgr::scanCount(); i++) {
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = wifimgr::scanNet(i).ssid;
    o["rssi"] = wifimgr::scanNet(i).rssi;
    o["secure"] = wifimgr::scanNet(i).secure;
  }
  sendJson(d);
}

void handleJoin() {
  if (!need()) return;
  JsonDocument d;
  deserializeJson(d, server.arg("plain"));
  const char *ssid = d["ssid"] | "";
  if (!*ssid) { server.send(400, "application/json", "{\"error\":\"no network\"}"); return; }
  if (wifimgr::joinState() == wifimgr::J_TRYING) { server.send(409, "application/json", "{\"error\":\"busy\"}"); return; }
  wifimgr::join(ssid, d["pass"] | "");
  sendOk();
}

void handleJoinStatus() {
  if (!need()) return;
  JsonDocument d;
  const wifimgr::JoinState j = wifimgr::joinState();
  d["state"] = j == wifimgr::J_TRYING ? "trying" : j == wifimgr::J_OK ? "ok" : j == wifimgr::J_FAILED ? "failed" : "idle";
  d["msg"] = wifimgr::joinMessage();
  d["connected"] = wifimgr::connected();
  d["ssid"] = wifimgr::ssid();
  d["ip"] = wifimgr::ip();
  d["apSecs"] = wifimgr::setupSecondsLeft();
  sendJson(d);
}

// ---- offline place data
void sendPlace(JsonObject o, const geo::Place &p) {
  o["name"] = p.name; o["cc"] = p.cc; o["country"] = geo::countryName(p.country);
  o["lat"] = p.lat; o["lon"] = p.lon;
  o["zone"] = geo::zoneName(p.zone); o["posix"] = geo::zonePosix(p.zone);
}

void handleGeoLookup() {
  if (!need()) return;
  geo::Place p;
  float km = 0;
  JsonDocument d;
  if (!server.hasArg("lat") || !server.hasArg("lon") || !geo::nearest(server.arg("lat").toDouble(), server.arg("lon").toDouble(), p, &km)) { server.send(404, "application/json", "{\"error\":\"no data\"}"); return; }
  sendPlace(d.to<JsonObject>(), p);
  d["distKm"] = (int)lroundf(km);
  sendJson(d);
}

void handleGeoSearch() {
  if (!need()) return;
  static geo::Place hits[16];
  const int n = geo::search(server.arg("q").c_str(), hits, 16);
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < n; i++) sendPlace(a.add<JsonObject>(), hits[i]);
  sendJson(d);
}

void handleGeoZones() {
  if (!need()) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < geo::zoneCount(); i++) a.add(geo::zoneName(i));
  sendJson(d);
}

void handleGeoCountries() {
  if (!need()) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < geo::countryCount(); i++) { JsonArray e = a.add<JsonArray>(); e.add(geo::countryCode(i)); e.add(geo::countryName(i)); }
  sendJson(d);
}

// ---- firmware update (see ota.h). The upload handler runs while the file arrives, in the main loop thread: the radar's screen shows the progress.
bool gUpDeny = false, gUpFail = false, gUpOk = false;
char gUpErr[96] = "";

void otaChunk() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    gUpDeny = !authorized();   // checked before a single byte is written
    gUpFail = gUpOk = false;
    gUpErr[0] = 0;
    if (gUpDeny) return;
    const size_t total = server.hasArg("size") ? (size_t)server.arg("size").toInt() : 0;
    if (!ota::sessionBegin(total)) { gUpFail = true; strlcpy(gUpErr, ota::status().msg, sizeof(gUpErr)); }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (gUpDeny || gUpFail) return;
    if (!ota::sessionWrite(u.buf, u.currentSize)) { gUpFail = true; strlcpy(gUpErr, ota::status().msg, sizeof(gUpErr)); return; }
    ota::tick();   // repaints the progress screen
  } else if (u.status == UPLOAD_FILE_END) {
    if (gUpDeny || gUpFail) return;
    gUpOk = ota::sessionFinish(server.hasArg("sha256") ? server.arg("sha256").c_str() : "", gUpErr, sizeof(gUpErr));
    gUpFail = !gUpOk;
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    ota::sessionAbort();
  }
}

void otaUploadDone() {
  if (gUpDeny) { server.send(401, "application/json", "{\"error\":\"login\"}"); return; }
  JsonDocument d;
  if (gUpOk) d["ok"] = true; else d["error"] = gUpErr[0] ? gUpErr : "Update failed";
  sendJson(d, gUpOk ? 200 : 400);
}

void handleOtaUrl() {
  if (!need()) return;
  JsonDocument d;
  deserializeJson(d, server.arg("plain"));
  char err[96] = "";
  if (!ota::startUrl(d["url"] | "", d["sha256"] | "", err, sizeof(err))) { JsonDocument e; e["error"] = err; sendJson(e, 400); return; }
  sendOk();
}

void handleOtaStatus() {
  if (!need()) return;
  const ota::Status os = ota::status();
  JsonDocument d;
  d["active"] = os.active; d["pct"] = os.pct; d["failed"] = os.failed; d["msg"] = String(os.msg);
  sendJson(d);
}

void handleOtaRevert() {
  if (!need()) return;
  if (!ota::canRevert()) { server.send(409, "application/json", "{\"error\":\"There is no previous firmware\"}"); return; }
  sendOk();
  delay(300);
  ota::revert();
}

void handleRestart() {
  if (!need()) return;
  sendOk();
  delay(200);
  app::restart();
}

}  // namespace

void begin() {
  for (int i = 0; i < 16; i++) gToken[i] = "0123456789abcdef"[esp_random() & 15];
  gToken[16] = 0;
  const char *hdrs[] = {"Cookie"};
  server.collectHeaders(hdrs, 1);
  server.on("/", HTTP_GET, [] { sendBig("text/html", (const char *)pageStart, pageEnd - pageStart, 200, true, "no-store"); });   // compressed: a third of the bytes over a weak link
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/wifi/scan", HTTP_GET, handleScan);
  server.on("/api/wifi/join", HTTP_POST, handleJoin);
  server.on("/api/wifi/status", HTTP_GET, handleJoinStatus);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.on("/api/ota/upload", HTTP_POST, otaUploadDone, otaChunk);
  server.on("/api/ota/url", HTTP_POST, handleOtaUrl);
  server.on("/api/ota/status", HTTP_GET, handleOtaStatus);
  server.on("/api/ota/revert", HTTP_POST, handleOtaRevert);
#ifdef DEV_SCREENSHOT
  server.on("/api/screen", HTTP_GET, handleScreen);
#endif
  server.on("/api/fa/test", HTTP_POST, [] { if (!need()) return; app::testFlightKey(); sendOk(); });
  server.on("/api/geo/lookup", HTTP_GET, handleGeoLookup);
  server.on("/api/geo/search", HTTP_GET, handleGeoSearch);
  server.on("/api/geo/zones", HTTP_GET, handleGeoZones);
  server.on("/api/geo/countries", HTTP_GET, handleGeoCountries);
  server.on("/leaflet.js", HTTP_GET, [] { sendBig("application/javascript", (const char *)leafletJsStart, leafletJsEnd - leafletJsStart, 200, true, "max-age=604800"); });   // map library for the picker, served from the radar itself
  server.on("/leaflet.css", HTTP_GET, [] { sendBig("text/css", (const char *)leafletCssStart, leafletCssEnd - leafletCssStart, 200, true, "max-age=604800"); });
  for (const char *p : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt", "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt", "/fwlink"})
    server.on(p, HTTP_ANY, [p] { probe(p); });   // phones' connectivity probes
  server.onNotFound([] { if (viaAp()) captive(); else server.send(404, "text/plain", "not found"); });
  server.begin();
  gBegun = true;
  Serial.printf("[web] server up, PIN %s\n", settings::get().webPin);
}

void tick() {
  if (!gBegun) return;
  if (wifimgr::apActive() && !gDnsOn) {   // captive portal: every name resolves to us, so phones pop up the setup page
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", WiFi.softAPIP());
    gDnsOn = true;
  } else if (!wifimgr::apActive() && gDnsOn) {
    dns.stop();
    gDnsOn = false;
  }
  if (gDnsOn) dns.processNextRequest();
  if (!gMdns && wifimgr::connected()) {
    if (MDNS.begin("radar")) { MDNS.addService("http", "tcp", 80); gMdns = true; Serial.println("[web] http://radar.local"); }
  } else if (gMdns && !wifimgr::connected()) {
    MDNS.end();
    gMdns = false;   // announced again after the reconnect
  }
  server.handleGuarded();
}

String url() { return wifimgr::connected() ? String("http://radar.local") : String("http://") + WiFi.softAPIP().toString(); }

}  // namespace webui
