#include "sdlog.h"
#include "settings.h"
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <stdarg.h>
#include <vector>
#include <algorithm>
#include "phase.h"

namespace sdlog {
namespace {

constexpr int PIN_CS = 10, PIN_SCK = 12, PIN_MOSI = 11, PIN_MISO = 13;   // verified on this board (hardware-findings.md)
const char *const DIR = "/adsb";
constexpr uint32_t FLUSH_MS = 5000, MOUNT_RETRY_MS = 10000, PROBE_MS = 15000, RETENTION_MS = 3600000;
constexpr char CSV_HEADER[] = "time_utc,hex,callsign,lat,lon,alt_ft,gs_kt,track,squawk\n";

// ---------------------------------------------------------------- RAM buffers: producers never wait for the card
struct Ring {
  uint8_t *buf = nullptr;
  size_t cap = 0, head = 0, tail = 0, used = 0;
  SemaphoreHandle_t m = nullptr;
  void init(size_t c) {
    buf = (uint8_t *)heap_caps_malloc(c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)malloc(c);
    cap = buf ? c : 0;
    m = xSemaphoreCreateMutex();
  }
  bool push(const char *s, size_t n) {   // a whole line, or nothing
    if (!cap) return false;
    xSemaphoreTake(m, portMAX_DELAY);
    const bool ok = used + n <= cap;
    if (ok) { for (size_t i = 0; i < n; i++) { buf[head] = (uint8_t)s[i]; head = (head + 1) % cap; } used += n; }
    xSemaphoreGive(m);
    return ok;
  }
  size_t peek(char *out, size_t max) {   // look at the oldest bytes without removing them
    xSemaphoreTake(m, portMAX_DELAY);
    const size_t n = std::min(used, max);
    for (size_t i = 0, t = tail; i < n; i++, t = (t + 1) % cap) out[i] = (char)buf[t];
    xSemaphoreGive(m);
    return n;
  }
  void commit(size_t n) {   // they have reached the card
    xSemaphoreTake(m, portMAX_DELAY);
    n = std::min(n, used);
    tail = (tail + n) % cap;
    used -= n;
    xSemaphoreGive(m);
  }
  size_t size() const { return used; }
};
Ring csvRing, sysRing;

SPIClass sdSpi(HSPI);
SemaphoreHandle_t gFs;                   // serialises card access between the task and the log viewer
bool gMounted = false, gEjected = false, gEjectReq = false;
volatile bool gSimRemoved = false;
int gWriteFails = 0;
uint64_t gTotal = 0, gFree = 0;
uint32_t gRowsToday = 0, gBytesToday = 0, gDropped = 0;
char gToday[11] = "", gFileToday[24] = "";
uint32_t gNextMount = 0, gNextFlush = 0, gNextProbe = 0, gNextRetention = 0;

bool timeValid() { return time(nullptr) > 1700000000; }

bool utcDate(char *out) {   // "2026-10-09"
  if (!timeValid()) return false;
  const time_t t = time(nullptr);
  struct tm g;
  gmtime_r(&t, &g);
  snprintf(out, 11, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
  return true;
}

// ---------------------------------------------------------------- card
bool tryMount() {
  if (gSimRemoved) return false;
  sdSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  if (!SD.begin(PIN_CS, sdSpi, 20000000) || SD.cardType() == CARD_NONE) { SD.end(); sdSpi.end(); return false; }
  SD.mkdir(DIR);
  gTotal = SD.totalBytes();
  gFree = gTotal - std::min<uint64_t>(gTotal, SD.usedBytes());
  gMounted = true;
  gWriteFails = 0;
  return true;
}

void unmount() {
  if (gMounted) { SD.end(); sdSpi.end(); }
  gMounted = false;
}

void lost(const char *why) {   // removed, unreadable or full: forget the mount, try again later
  Serial.printf("[sd] card lost (%s)\n", why);
  sys("SD card lost (%s): rows are kept in memory until it is back", why);
  unmount();
  gNextMount = millis() + MOUNT_RETRY_MS;
}

int dateKey(const String &name) {   // 20261009 from "2026-10-09.csv" or "sys-2026-10-09.log"; used to order files by day
  const int i = name.indexOf("20");
  if (i < 0 || name.length() < (unsigned)i + 10) return 0;
  return name.substring(i, i + 4).toInt() * 10000 + name.substring(i + 5, i + 7).toInt() * 100 + name.substring(i + 8, i + 10).toInt();
}

// Delete the oldest day files until at least 10 % of the card is free.
void retention() {
  if (!gMounted) return;
  xSemaphoreTake(gFs, portMAX_DELAY);
  gTotal = SD.totalBytes();
  gFree = gTotal - std::min<uint64_t>(gTotal, SD.usedBytes());
  if (gTotal && gFree < gTotal / 10) {
    struct E { String name; uint32_t size; };
    std::vector<E> files;
    File root = SD.open(DIR);
    for (File f = root.openNextFile(); f; f = root.openNextFile()) if (!f.isDirectory()) files.push_back({String(f.name()), (uint32_t)f.size()});
    root.close();
    std::sort(files.begin(), files.end(), [](const E &a, const E &b) { return dateKey(a.name) < dateKey(b.name); });
    char today[11];
    const bool haveToday = utcDate(today);
    for (const E &e : files) {
      if (gFree >= gTotal / 10) break;
      if (haveToday && e.name.indexOf(today) >= 0) continue;   // never today's files
      if (SD.remove(String(DIR) + "/" + e.name)) { gFree += e.size; Serial.printf("[sd] retention: deleted %s\n", e.name.c_str()); }
    }
  }
  xSemaphoreGive(gFs);
}

// Write what is waiting in one ring to today's file, whole lines only. false = the card did not take it.
bool flushRing(Ring &r, const char *prefix, const char *ext, bool csv, char *chunk, size_t chunkCap) {
  size_t n = r.peek(chunk, chunkCap);
  while (n && chunk[n - 1] != '\n') n--;   // never split a line
  if (!n) return true;
  char date[11];
  if (!utcDate(date)) return true;         // no clock yet: keep the lines
  if (strcmp(date, gToday)) { strcpy(gToday, date); gRowsToday = gBytesToday = 0; }
  char path[56], name[24];
  snprintf(name, sizeof(name), "%s%s%s", prefix, date, ext);
  snprintf(path, sizeof(path), "%s/%s", DIR, name);
  if (gSimRemoved) return false;
  xSemaphoreTake(gFs, portMAX_DELAY);
  const bool existed = SD.exists(path);
  File f = SD.open(path, FILE_APPEND);
  bool ok = (bool)f;
  if (ok && csv && (!existed || f.size() == 0)) ok = f.write((const uint8_t *)CSV_HEADER, sizeof(CSV_HEADER) - 1) == sizeof(CSV_HEADER) - 1;
  if (ok) ok = f.write((const uint8_t *)chunk, n) == n;
  if (f) f.close();
  xSemaphoreGive(gFs);
  if (!ok) return false;
  r.commit(n);
  if (csv) {
    strlcpy(gFileToday, name, sizeof(gFileToday));
    gBytesToday += n;
    for (size_t i = 0; i < n; i++) if (chunk[i] == '\n') gRowsToday++;
  }
  return true;
}

void task(void *) {
  char *chunk = (char *)malloc(4096);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    const uint32_t now = millis();
    if (gEjectReq) {   // user pressed Eject: write everything out, then let go of the card
      gEjectReq = false;
      phase::set(1, "eject");
      if (gMounted && timeValid()) { flushRing(sysRing, "sys-", ".log", false, chunk, 4096); while (csvRing.size() && flushRing(csvRing, "", ".csv", true, chunk, 4096)) {} }
      unmount();
      gEjected = true;
      phase::set(1, "ejected");
      Serial.println("[sd] ejected: safe to remove the card");
      continue;
    }
    if (gEjected) continue;
    if (!gMounted) {
      if (now >= gNextMount) {
        phase::set(1, "mount");
        if (tryMount()) {
          Serial.printf("[sd] mounted: %u MB, %u MB free\n", (unsigned)(gTotal >> 20), (unsigned)(gFree >> 20));
          sys("SD card ready: %u MB, %u MB free", (unsigned)(gTotal >> 20), (unsigned)(gFree >> 20));
          retention();
          gNextRetention = now + RETENTION_MS;
          gNextFlush = now + FLUSH_MS;
          gNextProbe = now + PROBE_MS;
        } else gNextMount = now + MOUNT_RETRY_MS;
      }
      continue;
    }
    if (!settings::get().sdLog) { csvRing.commit(csvRing.size()); sysRing.commit(sysRing.size()); continue; }
    const bool pending = csvRing.size() || sysRing.size();
    if (pending && (now >= gNextFlush || csvRing.size() > 6000)) {
      gNextFlush = now + FLUSH_MS;
      phase::set(1, "flush");
      bool ok = flushRing(sysRing, "sys-", ".log", false, chunk, 4096);
      for (int i = 0; ok && i < 6 && csvRing.size(); i++) ok = flushRing(csvRing, "", ".csv", true, chunk, 4096);
      if (!ok) {
        if (++gWriteFails >= 2) lost("write failed");   // pulled out, or full
      } else gWriteFails = 0;
      phase::set(1, "idle");
    } else if (!pending && now >= gNextProbe) {   // nothing to write: still notice a removed card
      gNextProbe = now + PROBE_MS;
      xSemaphoreTake(gFs, portMAX_DELAY);
      const bool alive = !gSimRemoved && SD.exists(DIR);
      xSemaphoreGive(gFs);
      if (!alive) lost("not answering");
    }
    if (gMounted && now >= gNextRetention) { gNextRetention = now + RETENTION_MS; phase::set(1, "retention"); retention(); phase::set(1, "idle"); }
  }
}

// ---------------------------------------------------------------- sightings
struct Seen { uint32_t key, at; };
Seen seen[256];

uint32_t fnv(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; } return h ? h : 1; }

}  // namespace

void begin() {
  gFs = xSemaphoreCreateMutex();
  csvRing.init(96 * 1024);
  sysRing.init(16 * 1024);
  xTaskCreatePinnedToCore(task, "sdlog", 8192, nullptr, 1, nullptr, 0);
}

void sys(const char *fmt, ...) {
  if (!settings::get().sdLog) return;
  char msg[140], line[190];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (timeValid()) {
    const time_t t = time(nullptr);
    struct tm g;
    gmtime_r(&t, &g);
    snprintf(line, sizeof(line), "%04d-%02d-%02dT%02d:%02d:%02dZ %s\n", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, msg);
  } else snprintf(line, sizeof(line), "+%lus %s\n", (unsigned long)(millis() / 1000), msg);   // before the clock is set
  sysRing.push(line, strlen(line));
}

void sightings(const adsb::Snapshot &s) {
  const settings::Settings &S = settings::get();
  if (!S.sdLog || gEjected || !s.ok || !timeValid()) return;
  const uint32_t now = millis(), gap = (uint32_t)S.logSecs * 1000u;
  const time_t t = time(nullptr);
  struct tm g;
  gmtime_r(&t, &g);
  char iso[24];
  snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
  for (int i = 0; i < s.n; i++) {
    const adsb::Aircraft &a = s.ac[i];
    if (!a.hex[0]) continue;
    const uint32_t key = fnv(a.hex);
    Seen *slot = nullptr, *oldest = &seen[key & 255];
    for (int k = 0; k < 8; k++) {
      Seen &c = seen[(key + k) & 255];
      if (c.key == key) { slot = &c; break; }
      if (!c.key || now - c.at > 600000u) { if (!slot) slot = &c; if (!c.key) break; }
      if (now - c.at > now - oldest->at) oldest = &c;
    }
    if (slot && slot->key == key && now - slot->at < gap) continue;   // logged recently
    if (!slot) slot = oldest;
    slot->key = key;
    slot->at = now;
    char row[140], alt[10] = "", trk[6] = "";
    if (a.altFt >= 0) snprintf(alt, sizeof(alt), "%ld", (long)a.altFt);
    if (a.track >= 0) snprintf(trk, sizeof(trk), "%d", (int)lroundf(a.track) % 360);
    const int n = snprintf(row, sizeof(row), "%s,%s,%s,%.5f,%.5f,%s,%d,%s,%s\n", iso, a.hex, a.flight, a.lat, a.lon, alt, (int)lroundf(a.gs), trk, a.squawk);
    if (!csvRing.push(row, n)) gDropped++;
  }
}

Status status() {
  Status st = {};
  st.state = gEjected ? S_EJECTED : !settings::get().sdLog ? S_OFF : !gMounted ? S_NO_CARD : gWriteFails > 0 ? S_ERROR : S_OK;
  st.totalBytes = gMounted ? gTotal : 0;
  st.freeBytes = gMounted ? gFree : 0;
  st.rowsToday = gRowsToday;
  st.bytesToday = gBytesToday;
  st.dropped = gDropped;
  strlcpy(st.file, gFileToday, sizeof(st.file));
  return st;
}

void eject() { gEjectReq = true; }
void ejectAndWait(uint32_t ms) { gEjectReq = true; const uint32_t t0 = millis(); while (!gEjected && millis() - t0 < ms) delay(20); }
void mount() { gEjected = false; gNextMount = 0; }
void simulateRemoval(bool on) { gSimRemoved = on; Serial.printf("[dev] pretending the card is %s\n", on ? "removed" : "back"); }

int listFiles(char names[][24], uint32_t *sizes, int max) {
  if (!gMounted) return 0;
  struct E { String name; uint32_t size; };
  std::vector<E> files;
  xSemaphoreTake(gFs, portMAX_DELAY);
  File root = SD.open(DIR);
  for (File f = root ? root.openNextFile() : File(); f; f = root.openNextFile()) if (!f.isDirectory()) files.push_back({String(f.name()), (uint32_t)f.size()});
  if (root) root.close();
  xSemaphoreGive(gFs);
  std::sort(files.begin(), files.end(), [](const E &a, const E &b) { const int ka = dateKey(a.name), kb = dateKey(b.name); return ka != kb ? ka > kb : a.name > b.name; });
  int n = 0;
  for (const E &e : files) {
    if (n >= max) break;
    strlcpy(names[n], e.name.c_str(), 24);
    sizes[n++] = e.size;
  }
  return n;
}

bool readTail(const char *name, char *buf, size_t cap, size_t &len) {
  len = 0;
  if (!gMounted || cap < 2) return false;
  char path[56];
  snprintf(path, sizeof(path), "%s/%s", DIR, name);
  xSemaphoreTake(gFs, portMAX_DELAY);
  File f = SD.open(path, FILE_READ);
  if (f) {
    const size_t size = f.size();
    const size_t start = size > cap - 1 ? size - (cap - 1) : 0;
    f.seek(start);
    len = f.read((uint8_t *)buf, cap - 1);
    f.close();
    if (start > 0) {   // begin at a line start
      size_t k = 0;
      while (k < len && buf[k] != '\n') k++;
      if (k < len) { memmove(buf, buf + k + 1, len - k - 1); len -= k + 1; }
    }
    buf[len] = 0;
  }
  xSemaphoreGive(gFs);
  return len > 0;
}

}  // namespace sdlog
