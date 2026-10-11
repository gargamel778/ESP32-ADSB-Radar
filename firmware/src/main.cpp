// ESP32 ADS-B Radar — VIEWE UEDX80480070E-WB-A  (GPL-3.0-or-later, derived from mircemk's CrowPanel radar)
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <esp_rom_sys.h>
#include <mbedtls/base64.h>
#include "config.h"
#include "settings.h"
#include "display.h"
#include "touch_gt911.h"
#include "adsb.h"
#include "radar.h"
#include "app.h"
#include "ui.h"
#include "wifimgr.h"
#include "geo.h"
#include "sdlog.h"
#include "ota.h"
#include "night.h"
#include "phase.h"
#include <esp_task_wdt.h>
#include "webui.h"

static bool displayOk = false;
static int gMode = 0;
static uint32_t FRAME_MS = 33;      // ~30 fps cap

// Dev stress test (serial 'S', or automatically while the Settings > System > Tear Test bars run): every 1.5 s the display's interrupt
// core masks interrupts for 2 ms, as a flash write or a PSRAM burst would. A bounce-buffer driver that cannot recover shows a
// permanently scrambled picture after the first stall; a healthy one glitches for one frame.
static volatile bool gStallOn = false;
static void stallTask(void *) {
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (!gStallOn && !radar::testPatternOn()) continue;  // the tear-test bars include the stall
    portENTER_CRITICAL(&mux);
    esp_rom_delay_us(2000);
    portEXIT_CRITICAL(&mux);
  }
}

// The display backend is chosen at boot (the two backends cannot be swapped live), so changing it saves and restarts.
static void switchMode(int mode) {
  mode = (mode % display::MODE_COUNT + display::MODE_COUNT) % display::MODE_COUNT;
  app::setDisplayMode(mode);
  Serial.printf("[mode] switching to %d (%s), restarting\n", mode, display::modeName(mode));
  app::restart();
}

// Dev: inject touch events as if a finger did it (serial "X400,240" = tap, "Z120,260" = pinch from 120 px to 260 px apart).
static void injectTap(int x, int y) {
  gt911::Event e = {};
  e.n = 1; e.x[0] = x; e.y[0] = y;
  ui::touch(e);
  delay(80);
  ui::touch(e);
  delay(40);
  e.n = 0;
  ui::touch(e);
  Serial.printf("[dev] tap %d,%d\n", x, y);
}

static void injectPinch(int d0, int d1) {
  gt911::Event e = {};
  e.n = 2;
  const int steps = 12;
  for (int i = 0; i <= steps; i++) {
    const int d = d0 + (d1 - d0) * i / steps;
    e.x[0] = 240 - d / 2; e.x[1] = 240 + d / 2; e.y[0] = e.y[1] = 240;
    ui::touch(e);
    delay(40);
  }
  e.n = 0;
  ui::touch(e);
  Serial.printf("[dev] pinch %d -> %d\n", d0, d1);
}

// Dev: dump a region of the visible framebuffer over serial as base64 RGB565 (serial "D0,0,800,480,2" = whole screen, every 2nd pixel).
// Lets the host rebuild a screenshot, since nobody can see the panel from the terminal. Blocks the loop while it prints.
static void dumpScreen(int x0, int y0, int w, int h, int step) {
  if (step < 1) step = 1;
  lgfx::LGFX_Sprite *fb = display::doubleBuffered() ? display::frontFb() : nullptr;
  if (!fb) { Serial.println("[dump] no framebuffer to dump in this display mode"); return; }
  const uint16_t *px = (const uint16_t *)fb->getBuffer();
  x0 = constrain(x0, 0, 799); y0 = constrain(y0, 0, 479);
  w = constrain(w, 1, 800 - x0); h = constrain(h, 1, 480 - y0);
  const int ow = (w + step - 1) / step, oh = (h + step - 1) / step;
  // The log writer behind log_e() (the HTTPS client's error lines, from the data task on core 0) busy-waits until the UART has gone idle, which a dump that keeps the
  // transmitter busy for minutes never lets happen: that task spun, the idle task starved and the watchdog restarted the radar. So the dump switches that output off.
  Serial.setDebugOutput(false);
  Serial.printf("FBDUMP BEGIN %d %d\n", ow, oh);
  static uint8_t row[800 * 2], enc[800 * 3];
  for (int y = 0; y < oh; y++) {
    feedLoopWDT();
    for (int x = 0; x < ow; x++) {
      const uint16_t v = px[(y0 + y * step) * 800 + x0 + x * step];
      row[x * 2] = v & 0xFF; row[x * 2 + 1] = v >> 8;
    }
    size_t n = 0;
    const int hdr = snprintf((char *)enc, 8, "%d:", y);   // the row number first: a line another task garbles costs one row, not the rows after it
    mbedtls_base64_encode(enc + hdr, sizeof(enc) - hdr, &n, row, ow * 2);
    n += hdr;
    enc[n++] = '\n';
    Serial.write(enc, n);
    feedLoopWDT();
    delay(1);   // lets the idle task run
  }
  Serial.setDebugOutput(true);
  Serial.println("FBDUMP END");
}

// ---- health: problems that only show after days (a wedged Wi-Fi driver, a stuck fetch, a dead display interrupt) end in a restart instead of a frozen radar.
// Everything is written to the system log, so a restart is never a mystery.
static uint32_t gFetchOk = 0, gFetchFail = 0, gLastGoodMs = 0, gOfflineSince = 0, gUpSince = 0;
static bool gWifiWas = false;

static void emergencyLog(const adsb::Snapshot &snap) {   // a new emergency squawk goes into the system log (once per aircraft per half hour)
  struct Seen { char hex[8]; uint8_t code; uint32_t at; };
  static Seen seen[6];
  const uint32_t now = millis();
  for (int i = 0; i < snap.n; i++) {
    const adsb::Aircraft &a = snap.ac[i];
    if (strcmp(a.squawk, "7500") && strcmp(a.squawk, "7600") && strcmp(a.squawk, "7700")) continue;
    const uint8_t code = a.squawk[1] == '5' ? 1 : a.squawk[1] == '6' ? 2 : 3;
    int slot = -1, oldest = 0;
    for (int k = 0; k < 6; k++) {
      if (seen[k].at && !strcmp(seen[k].hex, a.hex) && seen[k].code == code) slot = k;
      if (seen[k].at < seen[oldest].at) oldest = k;
    }
    if (slot >= 0 && now - seen[slot].at < 1800000) continue;
    if (slot < 0) slot = oldest;
    strlcpy(seen[slot].hex, a.hex, sizeof(seen[slot].hex)); seen[slot].code = code; seen[slot].at = now ? now : 1;
    sdlog::sys("emergency squawk %s: %s (%s) %s at %.3f,%.3f alt %d ft", a.squawk, a.flight[0] ? a.flight : "no callsign", a.hex, a.reg, a.lat, a.lon, (int)a.altFt);
    Serial.printf("[emergency] squawk %s from %s (%s)\n", a.squawk, a.flight, a.hex);
  }
}

static void restartFor(const char *why) {
  Serial.printf("[health] restarting: %s\n", why);
  sdlog::sys("health: restarting (%s); up %u s, heap %u KB (min %u KB), psram %u KB", why, (unsigned)((millis() - gUpSince) / 1000), (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  app::restart();
}

static void healthTick(bool displayOk) {
  static uint32_t lastCheck = 0, lastLog = 0, vsyncSeen = 0, vsyncMovedAt = 0;
  const uint32_t now = millis();
  const bool wc = wifimgr::connected();
  if (wc != gWifiWas) {   // connection events, for the log
    if (wc) {
      sdlog::sys("wifi connected: %s, %d dBm", WiFi.SSID().c_str(), WiFi.RSSI());
      if (gOfflineSince && now - gOfflineSince > 5000) sdlog::sys("wifi was offline %u s", (unsigned)((now - gOfflineSince) / 1000));
    } else if (now > 20000) sdlog::sys("wifi lost");
    gOfflineSince = wc ? 0 : now;
    gWifiWas = wc;
  }
  if (now - lastCheck < 10000) return;
  lastCheck = now;
  if (ota::busy()) { gLastGoodMs = now; vsyncMovedAt = now; return; }   // an update owns the radar for a couple of minutes

  // a configured network, but no data for a long time
  if (settings::get().wifiSsid[0]) {
    if (wc && now - gLastGoodMs > 15 * 60000UL) restartFor("no aircraft data for 15 minutes with Wi-Fi up");
    else if (!wc && gOfflineSince && now - gOfflineSince > 30 * 60000UL) restartFor("Wi-Fi offline for 30 minutes");
  }
  // the display interrupt must keep counting frames (only the Espressif backends count them)
  if (displayOk && !radar::testPatternOn()) {
    const uint32_t v = display::vsyncCount();
    if (v != vsyncSeen) { vsyncSeen = v; vsyncMovedAt = now; }
    else if (v > 0 && now - vsyncMovedAt > 15000) restartFor("the display stopped refreshing");
  }
  if (now - lastLog >= 3600000UL) {   // hourly: the numbers a soak test needs
    lastLog = now;
    sdlog::sys("health: up %u h, heap %u KB (min %u KB), psram %u KB, wifi %s %d dBm, fetches ok/failed %u/%u", (unsigned)((now - gUpSince) / 3600000UL), (unsigned)(ESP.getFreeHeap() / 1024),
               (unsigned)(ESP.getMinFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024), wc ? "up" : "DOWN", wc ? wifimgr::rssi() : 0, (unsigned)gFetchOk, (unsigned)gFetchFail);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  geo::begin();
  settings::begin();
  settings::Settings &S = settings::get();
  gMode = S.displayMode < display::MODE_COUNT ? S.displayMode : 0;
  Serial.printf("\n[boot] ESP32-ADSB-Radar  display=%s home=%.4f,%.4f range=%.0fkm psram=%uK\n", display::modeName(gMode), S.lat, S.lon, settings::rangeKm(),
                (unsigned)(ESP.getPsramSize() / 1024));

  bool touchOk = gt911::begin();
  Serial.printf("[boot] touch %s, clock %s\n", touchOk ? "ok" : "NOT FOUND", time(nullptr) > 1700000000 ? "kept across the restart" : "not set yet");

  lgfx::LovyanGFX *disp = display::begin(gMode);
  displayOk = disp != nullptr;
  if (displayOk) {
    night::evaluate();   // an always-on night mode must be in force before the first frame is painted
    night::applyBrightness();
    radar::begin(disp);
    radar::setTrailDeg(S.trail);
    radar::setTheme(S.theme);
    radar::setModeLabel(display::modeName(gMode));
    radar::applyLocation();
    ui::begin(disp);
    if (!S.wifiSsid[0]) ui::openSetup();   // nothing saved to connect to: offer phone setup right away
    xTaskCreatePinnedToCore(stallTask, "stall", 2048, nullptr, 3, nullptr, display::isrCore());
  }
  Serial.printf("[boot] display %s\n", displayOk ? "ok" : "FAILED");

  sdlog::begin();
#ifdef OTA_TEST_MODE
  if (OTA_TEST_MODE == 1) { Serial.println("[test] crashing at start-up on purpose"); delay(300); abort(); }   // 1 = crash right at boot
#endif
  {
    const esp_reset_reason_t rr = esp_reset_reason();
    if (phase::takePlanned()) sdlog::sys("boot after a planned restart (reset reason %d), free heap %u KB", (int)rr, (unsigned)(ESP.getFreeHeap() / 1024));
    else if (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT || rr == ESP_RST_PANIC)   // abnormal: say which step each task was in
      sdlog::sys("boot after a crash: reset reason %d (%s), data task in '%s', SD task in '%s', main loop in '%s', free heap %u KB", (int)rr, rr == ESP_RST_PANIC ? "panic" : "watchdog", phase::get(0), phase::get(1), phase::get(2), (unsigned)(ESP.getFreeHeap() / 1024));
    else sdlog::sys("boot: reset reason %d, free heap %u KB", (int)rr, (unsigned)(ESP.getFreeHeap() / 1024));
    phase::clear();
    // A secure fetch is CPU-bound for 2-4 s on core 0 (longer while the display hammers PSRAM): give the idle-task watchdog 15 s instead of 5 s.
    esp_task_wdt_config_t wc = {.timeout_ms = 15000, .idle_core_mask = 1u << 0, .trigger_panic = true};
    esp_task_wdt_reconfigure(&wc);
    enableLoopWDT();   // the main loop is watched too: a hang of 15 s restarts the radar (long jobs call feedLoopWDT())
  }
  gUpSince = gLastGoodMs = millis();
  ota::begin();
  wifimgr::begin();   // non-blocking: the radar runs (No Wi-Fi) until it joins
  webui::begin();
  adsb::begin();
}

void loop() {
  static uint32_t last = millis(), statT = 0;
  static bool ntpStarted = false;
  static adsb::Snapshot *snapP = adsb::newSnapshot();  // large: PSRAM, not stack or internal RAM
  adsb::Snapshot &snap = *snapP;

  uint32_t now = millis();
  float dt = (now - last) / 1000.0f;
  last = now;
  if (dt > 0.2f) dt = 0.2f;
  settings::Settings &S = settings::get();

  gt911::Event e;
  while (gt911::next(e)) {
    if (e.n) Serial.printf("[touch] n=%d x=%d y=%d\n", e.n, e.x[0], e.y[0]);
    if (displayOk) ui::touch(e);
  }
  while (Serial.available()) {  // dev: '0'..'5' display mode, t trail, g font theme, r range, S stall, T tear test, "X400,240" tap, "Z120,260" pinch, "CUS" country, "L<lat>,<lon>,<step>" test location
    int c = Serial.read();
    if (c >= '0' && c < '0' + display::MODE_COUNT) switchMode(c - '0');
    else if (c == 't') app::setTrail(settings::kTrails[(settings::kTrails[0] == S.trail ? 1 : S.trail == 25 ? 2 : S.trail == 40 ? 3 : 0)]);
    else if (c == 'g') app::setTheme(radar::themeIndex() + 1);
    else if (c == 'T') { radar::setTestPattern(!radar::testPatternOn()); Serial.printf("[dev] tear test %s\n", radar::testPatternOn() ? "ON" : "off"); }
    else if (c == 'r') app::setRangeStep((S.rangeStep + 1) % units::RANGE_STEPS);
    else if (c == 'S') {
      gStallOn = !gStallOn;
      Serial.printf("[dev] interrupt stall test %s (2 ms every 1.5 s on core %d)\n", gStallOn ? "ON" : "off", display::isrCore());
    }
    else if (c == 'v') { static bool v = false; v = !v; radar::setVerify(v); Serial.printf("[dev] buffer verification %s\n", v ? "ON" : "off"); }
    else if (c == 'H') Serial.printf("[heap] internal free=%u largest=%u min=%u | psram free=%u | adsb busy=%d\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (int)adsb::busy());
    else if (c == 'N') { static bool n = false; n = !n; wifimgr::debugNoNetwork(n); }   // dev: act as if no network were saved (access point comes up and stays)
    else if (c == 'O') { app::resetHome(); }   // dev: build-time home, country and time zone
    else if (c == 'V') {   // dev: card status, file list and the last lines of the newest files
      const sdlog::Status st = sdlog::status();
      Serial.printf("[sd] state=%d total=%uMB free=%uMB rows_today=%u bytes_today=%u dropped=%u file=%s\n", (int)st.state, (unsigned)(st.totalBytes >> 20), (unsigned)(st.freeBytes >> 20), (unsigned)st.rowsToday, (unsigned)st.bytesToday, (unsigned)st.dropped, st.file);
      static char names[12][24]; static uint32_t sizes[12];
      const int n = sdlog::listFiles(names, sizes, 12);
      for (int i = 0; i < n; i++) Serial.printf("[sd] file %s %u bytes\n", names[i], (unsigned)sizes[i]);
      static char tail[2600];
      for (int i = 0; i < n && i < 2; i++) { size_t len; if (sdlog::readTail(names[i], tail, sizeof(tail), len)) Serial.printf("[sd] --- tail of %s ---\n%s\n", names[i], tail); }
    }
    else if (c == 'U') { static bool u = false; u = !u; sdlog::simulateRemoval(u); }   // dev: pretend the SD card was pulled / put back
    else if (c == 'K') { wifimgr::debugSetupDone(); Serial.println("[dev] pretending the setup join succeeded"); }
    else if (c == 'P') {  // dev: "P1,2" = palette role 1 (radar 0, titles 1, callsign 2, value 3) to swatch 2
      String line = Serial.readStringUntil('\n');
      int role = 0, idx = 0;
      if (sscanf(line.c_str(), "%d,%d", &role, &idx) == 2) app::setPalette(role, idx);
    }
    else if (c == 'D') {
      String line = Serial.readStringUntil('\n');
      int x0 = 0, y0 = 0, w = 800, h = 480, st = 2;
      sscanf(line.c_str(), "%d,%d,%d,%d,%d", &x0, &y0, &w, &h, &st);
      dumpScreen(x0, y0, w, h, st);
    }
    else if (c == 'X') {
      String line = Serial.readStringUntil('\n');
      int x = 0, y = 0;
      if (sscanf(line.c_str(), "%d,%d", &x, &y) == 2 && displayOk) injectTap(x, y);
    }
    else if (c == 'Z') {
      String line = Serial.readStringUntil('\n');
      int d0 = 0, d1 = 0;
      if (sscanf(line.c_str(), "%d,%d", &d0, &d1) == 2 && displayOk) injectPinch(d0, d1);
    }
    else if (c == 'L') {  // dev: "L51.47,-0.45,6" = lat,lon,range step (not saved) - to test busy airspace
      String line = Serial.readStringUntil('\n');
      double la = 0, lo = 0; int st = 3;
      if (sscanf(line.c_str(), "%lf,%lf,%d", &la, &lo, &st) == 3) {
        S.lat = la; S.lon = lo;
        settings::setRangeStep(st);
        radar::applyLocation();
        adsb::fetchNow();
        Serial.printf("[dev] test location %.4f,%.4f range %.0f km (not saved)\n", la, lo, settings::rangeKm());
      }
    }
    else if (c == 'E') {  // dev: "E3" = the nearest aircraft squawks 7700 (1 = 7500, 2 = 7600, 0 = off)
      String line = Serial.readStringUntil('\n');
      radar::debugEmergency(line.toInt());
      Serial.printf("[dev] emergency squawk test %d\n", (int)line.toInt());
    }
    else if (c == 'Y') {  // dev (screenshots): "Y51.5080,-0.1281,London,GB,Europe/London,GMT0BST,M3.5.0/1,M10.5.0" = a demo place, country and time zone. Saving is switched off until the next restart: end with 'R'.
      String line = Serial.readStringUntil('\n');
      double la = 0, lo = 0; char name[32] = "", cc[4] = "", zone[40] = "", tz[48] = "";
      const int k = sscanf(line.c_str(), "%lf,%lf,%31[^,\n],%3[^,\n],%39[^,\n],%47[^\n]", &la, &lo, name, cc, zone, tz);
      if (k >= 2) {
        settings::holdSaves();   // whatever happens next (a settings tap, a restart through the menu), the demo place must not reach the flash
        app::setLocation(la, lo, name, k >= 4 ? cc : nullptr, k >= 6 ? tz : nullptr, k >= 6 ? zone : nullptr);
        Serial.printf("[dev] demo location %.4f,%.4f '%s' (not saved: finish with R, a plain restart)\n", S.lat, S.lon, S.place);
      }
    }
    else if (c == 'R') { Serial.println("[dev] plain ESP.restart() now"); Serial.flush(); ESP.restart(); }   // dev: restart without any preparation (reset-reason check)
    else if (c == 'n') { app::setNightMode((S.nightMode + 1) % 3); }   // dev: night mode off -> auto -> on
    else if (c == 'Q') { Serial.println("[dev] blocking the main loop for 20 s (the loop watchdog must restart the radar after 15 s)"); delay(20000); }
    else if (c == 'w') {  // dev: drop Wi-Fi for 45 s to watch the stale banner and the automatic recovery
      wifimgr::simulateOutage(45000);
    }
    else if (c == 'C') {
      delay(15);
      char cc[3] = {0, 0, 0};
      if (Serial.available() >= 2) { cc[0] = toupper(Serial.read()); cc[1] = toupper(Serial.read()); app::setCountry(cc); }
    }
  }

  if (adsb::take(snap)) {
    radar::setData(snap);
    ota::noteData(snap.ok);
    if (snap.ok) { gFetchOk++; gLastGoodMs = millis(); emergencyLog(snap); } else gFetchFail++;
    sdlog::sightings(snap);
    static bool lastOk = true;
    if (snap.ok != lastOk) { lastOk = snap.ok; sdlog::sys("data %s: %s", snap.ok ? "restored" : "fetch failing", snap.status); }
  }

  phase::set(2, "wifi");
  wifimgr::tick();
  phase::set(2, "web");
  webui::tick();
  phase::set(2, "settings");
  night::tick();

  if (!ntpStarted && wifimgr::connected()) {
    configTzTime(S.tzPosix, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
    Serial.printf("[net] wifi up ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }

  settings::tick();  // deferred NVS save, a few seconds after the last change
  ota::tick();
#ifdef OTA_TEST_MODE
  if (OTA_TEST_MODE == 2 && millis() > 25000) { Serial.println("[test] crashing 25 s after boot on purpose"); delay(300); abort(); }   // 2 = crash before the image is confirmed
#endif

  healthTick(displayOk);
  phase::set(2, "frame");
  if (displayOk) {
    if (ui::active()) ui::frame(); else radar::frame(dt);
    uint32_t spent = millis() - now;
    if (spent < FRAME_MS) delay(FRAME_MS - spent);  // frame-rate cap
    radar::noteFrame();
  }

  if (now - statT > 5000) {
    statT = now;
    radar::Stats s = radar::stats();
    uint32_t ivMin, ivMax, ivLate, ivN;
    display::vsyncStats(ivMin, ivMax, ivLate, ivN);
    static uint32_t lastV = 0, lastT = 0;
    uint32_t v = display::vsyncCount();
    float hz = lastT ? (v - lastV) * 1000.0f / (now - lastT) : 0;
    lastV = v; lastT = now;
    Serial.printf("[stat] fps=%.1f refresh=%.1fHz work=%ums paint=%uus waitsync=%uus flip=%uus tiles=%d vsync_iv=%u..%uus late=%u/%u ac=%d data=%d heap=%uK(min %uK) psram=%uK touch_reads=%u errs=%u tz='%s'\n", s.fps, hz,
                  (unsigned)s.frameMs, (unsigned)s.usPaint, (unsigned)s.usFlush, (unsigned)display::flipUs(), s.tiles, (unsigned)(ivN ? ivMin : 0), (unsigned)ivMax, (unsigned)ivLate, (unsigned)ivN,
                  s.aircraft, (int)s.dataOk, (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024), (unsigned)gt911::reads(),
                  (unsigned)gt911::errors(), radar::timeZoneLabel().c_str());
  }
}
