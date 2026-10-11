#include "wifimgr.h"
#include "settings.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <algorithm>

namespace wifimgr {
void startAP();   // internal: the policy below decides when (see applyAp)
void stopAP();
bool connected();

namespace {

Net gNets[MAX_NETS];
int gNetCount = 0;
bool gScanning = false, gScanPending = false;
uint32_t gScanStartAt = 0, gScanT0 = 0, gScanDoneAt = 0;

JoinState gJoin = J_IDLE;
char gJoinMsg[48] = "";
char gPendSsid[33] = "", gPendPass[65] = "", gOldSsid[33] = "", gOldPass[65] = "";
uint32_t gJoinT0 = 0;
volatile uint8_t gLastReason = 0;   // why the station last lost / failed a connection (wifi_err_reason_t), reset when a join starts

bool gAp = false;
bool gScreenWantsAp = false;        // the setup screen is on the display
bool gApSuppressed = false;         // setup just finished: keep the access point off until the screen is left
bool gDebugNoNet = false;           // dev: pretend nothing is saved
bool gStaPaused = false;            // station retries paused so they do not hop channels under a phone that is setting the radar up
uint32_t gOfflineSince = 0;         // millis() when the station was last connected (or boot)
constexpr uint32_t NOT_FOUND_AFTER_MS = 30000;
uint32_t gSetupDoneAt = 0;          // when a join succeeded while the setup access point was up
constexpr uint32_t SETUP_LINGER_MS = 4000;   // the access point switches off this long after the connection is made: just enough for the phone's page to read the result
uint32_t gOutageUntil = 0, gDownSince = 0;

// Wi-Fi driver queries can block while the radio is busy (TLS handshake, scan): the screen asks for these once per paint tile, so cache them
String gSsid, gIp = "-";
int gRssi = 0;
uint32_t gInfoAt = 0;
void refreshInfo() {
  if (WiFi.status() == WL_CONNECTED) { gSsid = WiFi.SSID(); gRssi = WiFi.RSSI(); gIp = WiFi.localIP().toString(); }
  else { gSsid = ""; gRssi = 0; gIp = "-"; }
  gInfoAt = millis();
}

void beginSaved() {
  const settings::Settings &S = settings::get();
  if (S.wifiSsid[0]) WiFi.begin(S.wifiSsid, S.wifiPass);
}

void collectScan() {
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (millis() - gScanT0 > 15000) { WiFi.scanDelete(); gScanning = false; Serial.println("[wifi] scan gave up"); }
    return;
  }
  if (n < 0) { gScanning = false; Serial.println("[wifi] scan failed"); return; }
  gNetCount = 0;
  for (int i = 0; i < n; i++) {   // the driver returns them strongest first; keep the first of each SSID
    const String s = WiFi.SSID(i);
    if (!s.length()) continue;   // hidden network
    bool dup = false;
    for (int k = 0; k < gNetCount; k++) if (s == gNets[k].ssid) { dup = true; break; }
    if (dup || gNetCount >= MAX_NETS) continue;
    Net &d = gNets[gNetCount++];
    strlcpy(d.ssid, s.c_str(), sizeof(d.ssid));
    d.rssi = (int8_t)WiFi.RSSI(i);
    d.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  gScanning = false;
  gScanDoneAt = millis();
  Serial.printf("[wifi] scan: %d networks\n", gNetCount);
}


// ---- when does the setup access point run?
bool noSavedNetwork() { return gDebugNoNet || !settings::get().wifiSsid[0]; }
bool fallbackWanted() {   // nothing to connect to, or the saved network cannot be found / joined
  if (noSavedNetwork()) return true;
  return !connected() && gJoin != J_TRYING && millis() - gOfflineSince > NOT_FOUND_AFTER_MS;
}
void applyAp() {
  if (!gScreenWantsAp) gApSuppressed = false;
  const bool want = !gApSuppressed && (gScreenWantsAp || fallbackWanted());
  if (want && !gAp) startAP();
  else if (!want && gAp) stopAP();
}

}  // namespace

void begin() {
  // access-point diagnostics: who joins, who leaves and why, who gets an address
  WiFi.onEvent([](arduino_event_id_t id, arduino_event_info_t info) {
    const uint8_t *m = info.wifi_ap_staconnected.mac;
    if (id == ARDUINO_EVENT_WIFI_AP_STACONNECTED) Serial.printf("[wifi] AP: %02x:%02x:%02x:%02x:%02x:%02x joined (aid %d)\n", m[0], m[1], m[2], m[3], m[4], m[5], info.wifi_ap_staconnected.aid);
    else if (id == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) Serial.printf("[wifi] AP: %02x:%02x:%02x:%02x:%02x:%02x left (reason %d)\n", m[0], m[1], m[2], m[3], m[4], m[5], (int)info.wifi_ap_stadisconnected.reason);
    else if (id == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) Serial.printf("[wifi] AP: address %s handed out\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
  }, ARDUINO_EVENT_MAX);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) { gLastReason = (uint8_t)std::min<int>(info.wifi_sta_disconnected.reason, 255); }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);  // do not write credentials/config to flash: a flash write stalls the display interrupt
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);    // modem power-save wakes cause latency spikes
  gOfflineSince = millis();
  if (settings::get().wifiSsid[0]) beginSaved();
  else { Serial.println("[wifi] no network saved: the setup access point stays on until one is connected"); applyAp(); }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
String ssid() { return connected() ? gSsid : String(""); }
int rssi() { return connected() ? gRssi : 0; }
String ip() { return connected() ? gIp : String("-"); }

void simulateOutage(uint32_t ms) {
  gOutageUntil = millis() + ms;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  Serial.printf("[dev] simulated Wi-Fi outage for %u s\n", (unsigned)(ms / 1000));
}

void startScan(uint32_t delayMs) {
  if (gScanning) return;
  gScanning = true;   // reported as scanning from now on, even while the start is still deferred
  gScanPending = true;
  gScanStartAt = millis() + delayMs;
}
bool scanning() { return gScanning; }
uint32_t scanAgeMs() { return gScanDoneAt ? millis() - gScanDoneAt : 0xFFFFFFFFu; }
int scanCount() { return gNetCount; }
const Net &scanNet(int i) { return gNets[constrain(i, 0, MAX_NETS - 1)]; }

void join(const char *ssid, const char *pass) {
  if (gJoin == J_TRYING || !ssid || !*ssid) return;
  const settings::Settings &S = settings::get();
  strlcpy(gOldSsid, S.wifiSsid, sizeof(gOldSsid));
  strlcpy(gOldPass, S.wifiPass, sizeof(gOldPass));
  strlcpy(gPendSsid, ssid, sizeof(gPendSsid));
  strlcpy(gPendPass, pass ? pass : "", sizeof(gPendPass));
  gJoin = J_TRYING;
  gJoinMsg[0] = 0;
  gJoinT0 = millis();
  gLastReason = 0;
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);
  WiFi.begin(gPendSsid, gPendPass);
  Serial.printf("[wifi] joining '%s' (old: '%s')\n", gPendSsid, gOldSsid);
}
JoinState joinState() { return gJoin; }
const char *joinMessage() { return gJoinMsg; }
void joinDone() { if (gJoin != J_TRYING) gJoin = J_IDLE; }

void startAP() {
  if (gAp) return;
  gSetupDoneAt = 0;
  char name[24];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(name, sizeof(name), "ADSB-Radar-%02X%02X", mac[4], mac[5]);
  WiFi.mode(WIFI_AP_STA);   // keep the station side running: the radar stays online while someone sets it up
  // Offer this address as the DNS server in DHCP. Without it phones get no DNS from this network, cannot resolve their
  // connectivity-check names and never pop up the sign-in page (the captive portal).
  const bool cfg = WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0), IPAddress(192, 168, 4, 2), IPAddress(192, 168, 4, 1));
  if (!cfg) Serial.println("[wifi] softAPConfig failed");
  gAp = WiFi.softAP(name, settings::get().apPass, 0, 0, 4);
  wifi_config_t cfgNow = {};
  esp_wifi_get_config(WIFI_IF_AP, &cfgNow);
  Serial.printf("[wifi] setup AP '%s' %s ip=%s channel=%d authmode=%d pmf(capable=%d required=%d) dns=%s\n", name, gAp ? "up" : "FAILED", WiFi.softAPIP().toString().c_str(), (int)WiFi.channel(), (int)cfgNow.ap.authmode,
                (int)cfgNow.ap.pmf_cfg.capable, (int)cfgNow.ap.pmf_cfg.required, WiFi.AP.dnsIP().toString().c_str());
  if (gAp) startScan(800);   // fill the network list before any phone joins: scanning later interrupts the phone's connection
}

void stopAP() {
  gSetupDoneAt = 0;
  if (!gAp) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  gAp = false;
  Serial.println("[wifi] setup AP stopped");
}

void screenWantsAp(bool on) { gScreenWantsAp = on; applyAp(); }
void debugNoNetwork(bool on) { gDebugNoNet = on; applyAp(); Serial.printf("[dev] pretending no network is saved: %s\n", on ? "yes" : "no"); }
bool apActive() { return gAp; }
String apSsid() { return gAp ? WiFi.softAPSSID() : String(""); }
const char *apPass() { return settings::get().apPass; }
String apQr() { return "WIFI:T:WPA;S:" + apSsid() + ";P:" + String(apPass()) + ";;"; }
uint32_t apClients() { return gAp ? WiFi.softAPgetStationNum() : 0; }
void debugSetupDone() { if (gAp) gSetupDoneAt = millis(); }
bool setupComplete() { return gAp && gSetupDoneAt != 0; }
uint32_t setupSecondsLeft() { return setupComplete() && millis() - gSetupDoneAt < SETUP_LINGER_MS ? (SETUP_LINGER_MS - (millis() - gSetupDoneAt) + 999) / 1000 : 0; }

void tick() {
  if (millis() - gInfoAt > 1000) refreshInfo();
  if (connected()) gOfflineSince = millis();
  // While a phone is attached to the access point and the saved network cannot be found, stop the station's retries: their channel
  // hopping would knock the phone off the access point every few seconds. They resume when the phone leaves.
  const bool pauseSta = gAp && !noSavedNetwork() && !connected() && gJoin != J_TRYING && apClients() > 0 && fallbackWanted();
  if (pauseSta && !gStaPaused) { gStaPaused = true; WiFi.setAutoReconnect(false); WiFi.disconnect(false, false); Serial.println("[wifi] phone attached: pausing the station's retries"); }
  else if (!pauseSta && gStaPaused) { gStaPaused = false; WiFi.setAutoReconnect(true); beginSaved(); Serial.println("[wifi] resuming the station's retries"); }
  applyAp();
  if (gAp && gSetupDoneAt && millis() - gSetupDoneAt > SETUP_LINGER_MS) { Serial.println("[wifi] connection made: switching the setup access point off"); gApSuppressed = true; stopAP(); }
  if (gScanPending && millis() >= gScanStartAt) {
    gScanPending = false;
    gScanT0 = millis();
    if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) { gScanning = false; Serial.println("[wifi] scan could not start"); }
    else Serial.println("[wifi] scanning");
  }
  if (gScanning && !gScanPending) collectScan();

  if (gJoin == J_TRYING) {
    const wl_status_t st = WiFi.status();
    const uint32_t el = millis() - gJoinT0;
    if (st == WL_CONNECTED && WiFi.SSID() == String(gPendSsid)) {
      settings::Settings &S = settings::get();
      strlcpy(S.wifiSsid, gPendSsid, sizeof(S.wifiSsid));
      strlcpy(S.wifiPass, gPendPass, sizeof(S.wifiPass));
      settings::saveNow();
      gJoin = J_OK;
      if (gAp) gSetupDoneAt = millis();
      snprintf(gJoinMsg, sizeof(gJoinMsg), "Connected, IP %s", WiFi.localIP().toString().c_str());
      Serial.printf("[wifi] joined '%s' ip=%s (saved)\n", gPendSsid, WiFi.localIP().toString().c_str());
    } else {
      // the driver's disconnect reason tells a wrong password (4-way handshake / auth failure) from a missing network
      const uint8_t why = gLastReason;
      const bool badPass = why == 2 || why == 15 || why == 202 || why == 204 || st == WL_CONNECT_FAILED;
      const bool noNet = why == 201 || st == WL_NO_SSID_AVAIL;
      if (!((badPass && el > 2000) || (noNet && el > 6000) || el > 22000)) { gDownSince = 0; return; }
      gJoin = J_FAILED;
      strlcpy(gJoinMsg, badPass ? "Wrong password?" : noNet ? "Network not found" : "No answer from the network", sizeof(gJoinMsg));
      Serial.printf("[wifi] join '%s' failed (%s, status %d, reason %u): back to '%s'\n", gPendSsid, gJoinMsg, (int)st, (unsigned)why, gOldSsid);
      WiFi.disconnect(false, false);
      if (gOldSsid[0]) WiFi.begin(gOldSsid, gOldPass);   // roll back: a typo must never strand the unit
    }
    gDownSince = 0;
    return;
  }

  // watchdog: the SDK's auto-reconnect normally copes; if we stay offline for a minute, start the join again from scratch
  const uint32_t now = millis();
  if (gOutageUntil && now > gOutageUntil) {
    gOutageUntil = 0;
    WiFi.setAutoReconnect(true);
    beginSaved();
    Serial.println("[dev] outage over, reconnecting");
  }
  if (connected()) gDownSince = 0;
  else if (!gOutageUntil && settings::get().wifiSsid[0]) {
    if (!gDownSince) gDownSince = now;
    else if (now - gDownSince > 60000 && !gStaPaused) {
      Serial.println("[wifi] offline for 60 s: re-joining");
      WiFi.disconnect();
      beginSaved();
      gDownSince = now;
    }
  }
}

}  // namespace wifimgr
