#include "ota.h"
#include "config.h"
#include "settings.h"
#include "sdlog.h"
#include "ui.h"
#include "wifimgr.h"
#include "phase.h"
#include "app.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include "imagecheck.h"

// Arduino confirms a freshly installed image as soon as it starts, which would defeat the rollback. We confirm it ourselves, later.
extern "C" bool verifyRollbackLater() { return true; }

extern const uint8_t caBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t caBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace ota {
namespace {

// Why the file is received into PSRAM first: every flash erase/write switches the cache off for milliseconds, which starves the display's
// bounce-buffer interrupt (it copies from the PSRAM frame buffer). While flash is being written the picture tears badly, and a long upload
// means a minute of it. So the file is collected in PSRAM (the screen stays clean and shows the progress), checked completely (SHA-256, image
// header and its own hash), and only then written to flash in one short burst, during which the screen shows a plain rising fill: a picture
// without detail has nothing to tear. The inactive slot is not touched until the file has proved good. If PSRAM cannot hold the file
// (or its size is unknown) the data is streamed straight to flash, still with the plain fill.
struct Session {
  bool open = false, staged = false;
  uint8_t *buf = nullptr;            // staged: the whole image
  esp_ota_handle_t handle = 0;       // streaming only
  const esp_partition_t *part = nullptr;
  size_t written = 0, total = 0;
  mbedtls_sha256_context sha;
} S;

volatile bool gActive = false, gFailed = false, gDark = false;   // gDark: flash is being written, the screen shows the plain fill
volatile uint8_t gPct = 0;
char gMsg[72] = "";
uint32_t gRestartAt = 0, gLastPaintMs = 0;
int gLastPaintPct = -1;
bool gArduinoOtaUp = false, gPending = false, gValidated = false, gDataSeen = false;
bool gUiOwned = false;   // the progress screen is up
TaskHandle_t gMainTask = nullptr;   // the screen belongs to this task
bool gUrlBusy = false;

void setMsg(const char *m) { strlcpy(gMsg, m, sizeof(gMsg)); }

void paint(bool force) {   // progress screen (main loop thread only: other tasks leave it to tick())
  if (xTaskGetCurrentTaskHandle() != gMainTask) return;
  const uint32_t now = millis();
  if (!force && gPct == gLastPaintPct && now - gLastPaintMs < 500) return;
  if (!force && now - gLastPaintMs < 200) return;
  gLastPaintPct = gPct;
  gLastPaintMs = now;
  gUiOwned = true;
  if (gDark) ui::otaDark(gPct); else ui::otaProgress(gPct, gMsg, gFailed);
}

void freeBuf() { if (S.buf) { heap_caps_free(S.buf); S.buf = nullptr; } S.staged = false; }

void fail(const char *why) {
  gFailed = true;
  gActive = false;
  gDark = false;
  freeBuf();
  setMsg(why);
  Serial.printf("[ota] FAILED: %s\n", why);
  sdlog::sys("firmware update failed: %s", why);
}

// (the image checks live in imagecheck.h)
bool checkImage(const uint8_t *b, size_t n, char *err, size_t cap) { return imagecheck::verify(b, n, err, cap); }

// Erase and write the staged image, then make it the boot image (which verifies it again). The only phase that touches flash.
bool installStaged(char *err, size_t cap) {
  setMsg("Installing: about 40 seconds");
  gPct = 100;
  gFailed = false;
  paint(true);
  const uint32_t t0 = millis();
  while (millis() - t0 < 2500) { paint(false); feedLoopWDT(); delay(50); }   // let the person read it while the screen still draws cleanly
  gDark = true; gPct = 0; paint(true);
  const size_t padded = (S.total + 4095) & ~(size_t)4095;
  for (size_t off = 0; off < padded; off += 65536) {
    const size_t len = std::min<size_t>(65536, padded - off);
    feedLoopWDT();
    if (esp_partition_erase_range(S.part, off, len) != ESP_OK) { strlcpy(err, "Could not erase the firmware slot", cap); return false; }
    gPct = (uint8_t)((off + len) * 50 / padded);
    paint(false);
  }
  for (size_t off = 0, k = 0; off < S.total; off += 4096, k++) {
    const size_t n = std::min<size_t>(4096, S.total - off);
    if (esp_partition_write(S.part, off, S.buf + off, n) != ESP_OK) { strlcpy(err, "Could not write the firmware", cap); return false; }
    if ((k & 15) == 15) { feedLoopWDT(); gPct = (uint8_t)(50 + (off + n) * 50 / S.total); paint(false); delay(1); }   // delay: lets the idle task run
  }
  gPct = 100;
  const esp_err_t e = esp_ota_set_boot_partition(S.part);   // verifies the image in flash
  if (e != ESP_OK) { strlcpy(err, e == ESP_ERR_OTA_VALIDATE_FAILED ? "The firmware did not verify after writing" : "Could not select the new firmware", cap); return false; }
  return true;
}

}  // namespace

// ---------------------------------------------------------------- session
bool sessionBegin(size_t total) {
  if (S.open) sessionAbort();
  if (gPending && !gValidated) { fail("The new firmware is still proving itself. Try again in a minute."); return false; }
  S.part = esp_ota_get_next_update_partition(nullptr);
  if (!S.part) { fail("No spare firmware slot"); return false; }
  if (total && total > S.part->size) { fail("File is bigger than the firmware slot"); return false; }
  S.buf = nullptr; S.staged = false;
  if (total) {
    S.buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    S.staged = S.buf != nullptr;
    Serial.printf("[ota] staging %u bytes in PSRAM: %s (largest free block %uK)\n", (unsigned)total, S.staged ? "ok" : "no room",
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  }
  if (!S.staged) {   // streaming straight to flash: the screen cannot be kept clean, so it shows only the plain fill
    const esp_err_t e = esp_ota_begin(S.part, OTA_WITH_SEQUENTIAL_WRITES, &S.handle);
    if (e != ESP_OK) { fail(e == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? "The new firmware is still proving itself. Try again in a minute." : "Could not start the update"); return false; }
    gDark = true;
  }
  mbedtls_sha256_init(&S.sha);
  mbedtls_sha256_starts(&S.sha, 0);
  S.open = true; S.written = 0; S.total = total;
  gActive = true; gFailed = false; gPct = 0;
  setMsg(S.staged ? "Receiving firmware" : "Writing firmware");
  Serial.printf("[ota] %s -> %s (%u bytes expected)\n", S.staged ? "receiving" : "streaming", S.part->label, (unsigned)total);
  phase::set(2, "ota");
  return true;
}

bool sessionWrite(const uint8_t *data, size_t n) {
  if (!S.open) return false;
  feedLoopWDT();   // a web upload keeps the main loop inside the request handler for a minute
  if (S.staged) {
    if (S.written + n > S.total) { sessionAbort(); fail("The file is bigger than announced"); return false; }
    memcpy(S.buf + S.written, data, n);
    if (S.written == 0 && n >= 24) {   // wrong kind of file: say so now, not after the whole upload
      char why[72];
      if (S.buf[0] != 0xE9 || S.buf[1] == 0 || S.buf[1] > 16) strlcpy(why, "Not a firmware file for this radar", sizeof(why));
      else if ((S.buf[12] | (S.buf[13] << 8)) != 9) strlcpy(why, "This firmware is for a different chip", sizeof(why));
      else why[0] = 0;
      if (why[0]) { sessionAbort(); fail(why); return false; }
    }
  } else {
    const esp_err_t e = esp_ota_write(S.handle, data, n);
    if (e != ESP_OK) { sessionAbort(); fail(e == ESP_ERR_OTA_VALIDATE_FAILED ? "Not a firmware file for this radar" : "Write to flash failed"); return false; }
  }
  mbedtls_sha256_update(&S.sha, data, n);
  S.written += n;
  const size_t whole = S.total ? S.total : S.part->size;
  gPct = (uint8_t)std::min<size_t>(99, S.written * 100 / whole);
  return true;
}

void sessionAbort() {
  if (S.open) {
    if (!S.staged) esp_ota_abort(S.handle);
    mbedtls_sha256_free(&S.sha);
  }
  S.open = false;
  gActive = false;
  gDark = false;
  freeBuf();
}

bool sessionFinish(const char *expectSha256, char *err, size_t errCap) {
  if (!S.open) { strlcpy(err, "No update in progress", errCap); return false; }
  uint8_t digest[32];
  mbedtls_sha256_finish(&S.sha, digest);
  mbedtls_sha256_free(&S.sha);
  S.open = false;
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
  const bool wantSha = expectSha256 && expectSha256[0];

  if (S.staged) {
    setMsg("Checking firmware");
    if (S.written != S.total) { strlcpy(err, "The file is shorter than announced", errCap); fail(err); return false; }
    if (!checkImage(S.buf, S.total, err, errCap)) { fail(err); return false; }
    if (wantSha && strcasecmp(expectSha256, hex) != 0) { snprintf(err, errCap, "SHA-256 does not match (file is %.12s...)", hex); fail(err); return false; }   // flash untouched
    if (!installStaged(err, errCap)) {
      esp_partition_erase_range(S.part, 0, 4096);   // whatever is there is not a usable "previous firmware"
      fail(err);
      return false;
    }
    freeBuf();
  } else {
    const esp_err_t e = esp_ota_end(S.handle);   // checks the image header, chip, checksum and the SHA-256 the build appended
    if (e != ESP_OK) {
      strlcpy(err, e == ESP_ERR_OTA_VALIDATE_FAILED ? "Not a valid firmware image for this radar (checksum, size or chip)" : "Could not finish the update", errCap);
      fail(err);
      return false;
    }
    if (wantSha && strcasecmp(expectSha256, hex) != 0) {
      snprintf(err, errCap, "SHA-256 does not match (file is %.12s...)", hex);
      fail(err);
      esp_partition_erase_range(S.part, 0, 4096);   // the file is a valid image, but not the one that was asked for: it must not count as "previous firmware"
      return false;   // boot slot unchanged
    }
    if (esp_ota_set_boot_partition(S.part) != ESP_OK) { strlcpy(err, "Could not select the new firmware", errCap); fail(err); return false; }
  }
  gDark = false;
  gPct = 100;
  gActive = true;
  setMsg("Installed. Restarting...");
  gRestartAt = millis() + 2500;
  paint(true);
  Serial.printf("[ota] installed in %s, %u bytes, sha256 %s\n", S.part->label, (unsigned)S.written, hex);
  sdlog::sys("firmware installed in %s (%u bytes, sha256 %.16s...), restarting", S.part->label, (unsigned)S.written, hex);
  phase::set(2, "idle");
  return true;
}

// ---------------------------------------------------------------- download from a link
namespace {
struct UrlJob { char url[256]; char sha[65]; };
UrlJob gJob;

void urlTask(void *) {
  char err[96] = "";
  bool ok = false;
  HTTPClient http;
  WiFiClientSecure sec;
  WiFiClient plain;
  const bool https = strncasecmp(gJob.url, "https://", 8) == 0;
  if (https) { sec.setCACertBundle(caBundleStart, caBundleEnd - caBundleStart); sec.setHandshakeTimeout(10); }
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // release assets redirect to a download host
  http.setUserAgent("ESP32-ADSB-Radar/" FW_VERSION);
  const bool began = https ? http.begin(sec, gJob.url) : http.begin(plain, gJob.url);
  int len = -1;
  if (!began) strlcpy(err, "Bad link", sizeof(err));
  else {
    setMsg("Connecting");
    gActive = true; gFailed = false; gPct = 0;
    const int code = http.GET();
    if (code != 200) snprintf(err, sizeof(err), "The server answered %d", code);
    else {
      len = http.getSize();
      if (sessionBegin(len > 0 ? (size_t)len : 0)) {
        setMsg("Downloading firmware");
        WiFiClient *stream = http.getStreamPtr();
        uint8_t *buf = (uint8_t *)malloc(4096);
        uint32_t lastData = millis();
        bool good = buf != nullptr;
        while (good && (len > 0 || len == -1) && (http.connected() || stream->available())) {
          const size_t avail = stream->available();
          if (!avail) { if (millis() - lastData > 30000) { strlcpy(err, "Download stalled", sizeof(err)); good = false; } delay(2); continue; }
          const size_t n = stream->readBytes(buf, std::min<size_t>(avail, 4096));
          lastData = millis();
          if (!sessionWrite(buf, n)) { good = false; strlcpy(err, "Write to flash failed", sizeof(err)); break; }
          if (len > 0) len -= (int)n;
        }
        free(buf);
        if (good && len > 0) { strlcpy(err, "Download ended early", sizeof(err)); good = false; }
        if (good) ok = sessionFinish(gJob.sha, err, sizeof(err));
        else sessionAbort();
      } else strlcpy(err, gMsg, sizeof(err));
    }
    http.end();
  }
  if (!ok) { if (!err[0]) strlcpy(err, "Update failed", sizeof(err)); fail(err); }
  gUrlBusy = false;
  vTaskDelete(nullptr);
}
}  // namespace

bool startUrl(const char *url, const char *sha256, char *err, size_t errCap) {
  if (gUrlBusy || gActive) { strlcpy(err, "An update is already running", errCap); return false; }
  if (!url || strlen(url) < 8 || strlen(url) >= sizeof(gJob.url) || (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8))) { strlcpy(err, "Give a full http:// or https:// link", errCap); return false; }
  if (sha256 && sha256[0] && strlen(sha256) != 64) { strlcpy(err, "A SHA-256 has 64 hex digits", errCap); return false; }
  if (!wifimgr::connected()) { strlcpy(err, "Not connected to a network", errCap); return false; }
  strlcpy(gJob.url, url, sizeof(gJob.url));
  strlcpy(gJob.sha, sha256 ? sha256 : "", sizeof(gJob.sha));
  gUrlBusy = true;
  xTaskCreatePinnedToCore(urlTask, "ota_url", 8192, nullptr, 1, nullptr, 0);
  return true;
}

// ---------------------------------------------------------------- info, revert
const char *slot() { const esp_partition_t *p = esp_ota_get_running_partition(); return p ? p->label : "?"; }

const char *imageState() {
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) return "valid";   // flashed over USB: no OTA state
  switch (st) {
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_NEW: return "installed";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_UNDEFINED: return "valid";
    default: return "invalid";
  }
}

bool busy() { return gActive || gUrlBusy; }

// esp_ota_check_rollback_is_possible() verifies the other slot's whole image (a SHA-256 over 5 MB: about half a second with the main loop blocked), so it is
// asked once per boot (the answer only changes with an install, which restarts) and not on every state request or settings repaint.
static int8_t gCanRevert = -1;
bool canRevert() {
  if (gCanRevert < 0) gCanRevert = esp_ota_check_rollback_is_possible() ? 1 : 0;
  return gCanRevert == 1;
}

void revert() {
  sdlog::sys("firmware: going back to the previous image by request");
  Serial.println("[ota] reverting to the previous firmware");
  delay(200);
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

// ---------------------------------------------------------------- main loop
void begin() {
  gMainTask = xTaskGetCurrentTaskHandle();
  esp_ota_img_states_t st;
  gPending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  Serial.printf("[ota] running %s, version %s, image %s%s\n", slot(), FW_VERSION, imageState(), gPending ? " (must prove itself within 60 s)" : "");
  canRevert();   // fill the cache now, while nobody waits for a web page
  sdlog::sys("firmware %s in %s, image %s", FW_VERSION, slot(), imageState());
}

void noteData(bool ok) { if (ok) gDataSeen = true; }

Status status() {
  Status s;
  s.active = gActive; s.pct = gPct; s.failed = gFailed;
  strlcpy(s.msg, gMsg, sizeof(s.msg));
  return s;
}

void tick() {
  feedLoopWDT();
  if (!gArduinoOtaUp && wifimgr::connected()) {   // PlatformIO's espota: password is the web PIN
    ArduinoOTA.setHostname("radar");
    ArduinoOTA.setPassword(settings::get().webPin);
    ArduinoOTA.setTimeout(15000);   // the radar may spend seconds in a flash erase between two chunks
    ArduinoOTA.setMdnsEnabled(false);   // the radar already announces radar.local
    ArduinoOTA.onStart([] { gActive = true; gFailed = false; gDark = true; gPct = 0; setMsg("Receiving firmware (espota)"); sdlog::sys("firmware update started over espota"); paint(true); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
      gPct = t ? (uint8_t)(p * 100ull / t) : 0;
      feedLoopWDT();   // espota runs its whole transfer inside ArduinoOTA.handle()
      static uint32_t lastLog = 0;
      if (millis() - lastLog > 5000) { lastLog = millis(); Serial.printf("[ota] espota %u/%u bytes, heap %uK\n", p, t, (unsigned)(ESP.getFreeHeap() / 1024)); }
      paint(false);
    });
    ArduinoOTA.onEnd([] { gDark = false; gPct = 100; setMsg("Installed. Restarting..."); sdlog::sys("firmware installed over espota, restarting"); sdlog::eject(); paint(true); });
    ArduinoOTA.onError([](ota_error_t e) { fail(e == OTA_BEGIN_ERROR ? "Not ready: a new firmware must run a minute first" : e == OTA_AUTH_ERROR ? "Wrong PIN" : "espota transfer failed"); paint(true); });
    ArduinoOTA.begin();
    gArduinoOtaUp = true;
    Serial.println("[ota] espota ready (hostname radar, password = web PIN)");
  }
  if (gArduinoOtaUp) ArduinoOTA.handle();

  if (gActive || gFailed) {
    if (gActive || gUiOwned) paint(false);
  }
  if (gFailed && gUiOwned && millis() - gLastPaintMs > 8000) { gFailed = false; gUiOwned = false; ui::otaDone(); }   // leave the error screen after a while
  if (gRestartAt && (int32_t)(millis() - gRestartAt) > 0) { Serial.println("[ota] restarting"); app::restart(); }   // the same orderly restart as everywhere else: settings saved, log flushed

  // A freshly installed image proves itself, then the bootloader's rollback is cancelled. The checks: it has been running for a minute,
  // and data is arriving (or, with no network to test, after ten minutes). A reset before that returns to the previous image.
  if (gPending && !gValidated) {
    const uint32_t up = millis();
    const bool healthy = (up > 60000 && gDataSeen) || up > 600000;
    if (healthy) {
      gValidated = true;
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.printf("[ota] image validated after %u s\n", (unsigned)(up / 1000));
      sdlog::sys("new firmware %s validated after %u s", FW_VERSION, (unsigned)(up / 1000));
    }
  }
}

}  // namespace ota
