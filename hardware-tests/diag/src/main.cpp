// ESP32-ADSB-Radar - hardware bring-up diagnostic
// Board: VIEWE UEDX80480070E-WB-A (7" 800x480 RGB, ESP32-S3 N16R8, GT911 touch, microSD)
//
// What it proves (all results go to the serial console as "[DIAG] ..." lines and on screen):
//   1. chip / flash / PSRAM identity and a 4 MB PSRAM write-read integrity + speed test
//   2. GT911 touch controller: hardware reset via IO38, I2C bus recovery, address, product ID, resolution
//   3. microSD slot (reports "no card" if empty)
//   4. RGB panel at a sequence of pixel clocks (25/20/16/12 MHz) with a static test pattern plus a
//      radar-like 400x400 PSRAM sprite animation (same memory-bandwidth load as the real radar)
//   5. touch: dots/trails, button taps, multi-touch count
//   6. Wi-Fi scan; and, if config/secrets.h exists, Wi-Fi join + api.airplanes.live fetch/parse
//
// Serial commands (115200): n = next pixel clock   0-3 = pick one   l = lock/unlock auto-cycle
//                           c = clear touch trails i = reprint info
// Pin map: VIEWE's board header for this model (also reproduced below)

#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_SECRETS 1
#else
#define HAVE_SECRETS 0
#endif

namespace pin {
constexpr int TOUCH_SDA = 19, TOUCH_SCL = 20, TOUCH_RST = 38, TOUCH_INT = 18;
constexpr int SD_CS = 10, SD_SCK = 12, SD_MOSI = 11, SD_MISO = 13;
}

static const uint32_t kPclkHz[] = {25000000, 20000000, 16000000, 12000000};
static const int kStages = sizeof(kPclkHz) / sizeof(kPclkHz[0]);
static const uint32_t kStageMs = 12000;

// ---------------------------------------------------------------- display driver
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB _bus;
  lgfx::Panel_RGB _panel;
  lgfx::Light_PWM _light;

 public:
  explicit LGFX(uint32_t pclkHz) {
    {
      auto cfg = _panel.config();
      cfg.memory_width = 800;
      cfg.memory_height = 480;
      cfg.panel_width = 800;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel.config(cfg);
    }
    {
      auto cfg = _bus.config();
      cfg.panel = &_panel;
      // LovyanGFX data order is B0-B4, G0-G5, R0-R4 (matches ESP_PANEL_BOARD_LCD_RGB_IO_DATA0..15).
      cfg.pin_d0 = 8;   cfg.pin_d1 = 3;   cfg.pin_d2 = 46;  cfg.pin_d3 = 9;   cfg.pin_d4 = 1;    // B0-B4
      cfg.pin_d5 = 5;   cfg.pin_d6 = 6;   cfg.pin_d7 = 7;   cfg.pin_d8 = 15;  cfg.pin_d9 = 16;   // G0-G4
      cfg.pin_d10 = 4;                                                                           // G5
      cfg.pin_d11 = 45; cfg.pin_d12 = 48; cfg.pin_d13 = 47; cfg.pin_d14 = 21; cfg.pin_d15 = 14;  // R0-R4
      cfg.pin_henable = 40;  // DE
      cfg.pin_vsync = 41;
      cfg.pin_hsync = 39;
      cfg.pin_pclk = 42;
      cfg.freq_write = pclkHz;
      cfg.hsync_polarity = 0;
      cfg.hsync_front_porch = 88;
      cfg.hsync_pulse_width = 48;
      cfg.hsync_back_porch = 40;
      cfg.vsync_polarity = 0;
      cfg.vsync_front_porch = 10;
      cfg.vsync_pulse_width = 6;
      cfg.vsync_back_porch = 26;
      cfg.pclk_active_neg = 1;
      cfg.de_idle_high = 0;
      cfg.pclk_idle_high = 0;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 2;
      cfg.freq = 20000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

static LGFX *gfx = nullptr;
static LGFX_Sprite *spr = nullptr;
static Preferences prefs;

// ---------------------------------------------------------------- shared state
struct Diag {
  String chip, psram, touch, sd, wifi, api;
  bool psramOk = false, touchOk = false, lcdOk = false, sprOk = false;
  float fps = 0;
  int maxTouches = 0, lastN = 0, lastX = -1, lastY = -1;
} D;

static int gStage = 0;
static bool gLocked = false;
static uint32_t gStageStart = 0;
static bool gInfoForce = true;  // set when the info area was wiped (drawPattern) so every line redraws

#define LOG(...) do { Serial.printf("[DIAG] " __VA_ARGS__); Serial.println(); } while (0)

// ---------------------------------------------------------------- 1. memory
static void testMemory() {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  D.chip = String("ESP32-S3 r") + (int)(ci.revision / 100) + "." + (int)(ci.revision % 100) + "  " + ci.cores +
           "c " + getCpuFrequencyMhz() + "MHz  flash " + (ESP.getFlashChipSize() >> 20) + "MB";
  LOG("chip=%s", D.chip.c_str());
  LOG("flash_size=%uMB flash_speed=%uMHz", (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)(ESP.getFlashChipSpeed() / 1000000));
  LOG("psram_total=%uKB psram_free=%uKB heap_free=%uKB", (unsigned)(ESP.getPsramSize() / 1024),
      (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getFreeHeap() / 1024));

  const size_t n = 4 * 1024 * 1024, words = n / 4;
  uint32_t *p = (uint32_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    D.psram = "PSRAM: alloc FAILED";
    LOG("psram_test=FAIL reason=alloc");
    return;
  }
  uint32_t t0 = micros();
  for (size_t i = 0; i < words; i++) p[i] = (uint32_t)i * 2654435761u;
  uint32_t tw = micros() - t0;
  t0 = micros();
  size_t bad = 0;
  for (size_t i = 0; i < words; i++) bad += (p[i] != (uint32_t)i * 2654435761u);
  uint32_t tr = micros() - t0;
  for (size_t i = 0; i < words; i++) p[i] = ~((uint32_t)i * 2654435761u);  // inverted pass
  for (size_t i = 0; i < words; i++) bad += (p[i] != ~((uint32_t)i * 2654435761u));
  heap_caps_free(p);
  float wMBs = 4.0f / (tw / 1e6f), rMBs = 4.0f / (tr / 1e6f);
  D.psramOk = (bad == 0);
  D.psram = String("PSRAM ") + (ESP.getPsramSize() >> 20) + "MB: " + (D.psramOk ? "OK" : "FAIL") + "  W " + String(wMBs, 0) + " / R " + String(rMBs, 0) + " MB/s";
  LOG("psram_test=%s bytes=%u bad_words=%u write=%.1fMB/s read=%.1fMB/s", D.psramOk ? "PASS" : "FAIL", (unsigned)n, (unsigned)bad, wMBs, rMBs);
}

// ---------------------------------------------------------------- 2. GT911 touch
static void gt911Reset(bool addr14) {
  pinMode(pin::TOUCH_RST, OUTPUT);
  pinMode(pin::TOUCH_INT, OUTPUT);
  digitalWrite(pin::TOUCH_INT, addr14 ? HIGH : LOW);
  digitalWrite(pin::TOUCH_RST, LOW);
  delay(10);
  digitalWrite(pin::TOUCH_RST, HIGH);
  delay(8);  // INT level is latched as the I2C address while RST rises
  pinMode(pin::TOUCH_INT, INPUT);
  delay(60);
}

static uint8_t gGtAddr = 0;
static bool i2cRead(uint8_t addr, uint16_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static void testTouchHardware() {
  gt911Reset(false);
  pinMode(pin::TOUCH_SDA, INPUT_PULLUP);
  pinMode(pin::TOUCH_SCL, INPUT_PULLUP);
  delay(2);
  bool sdaLow = digitalRead(pin::TOUCH_SDA) == LOW, sclLow = digitalRead(pin::TOUCH_SCL) == LOW;
  LOG("i2c_idle sda=%s scl=%s", sdaLow ? "LOW(stuck)" : "high", sclLow ? "LOW(stuck)" : "high");
  if (sdaLow) {  // clock the slave out of a half-finished transfer
    pinMode(pin::TOUCH_SCL, OUTPUT_OPEN_DRAIN);
    for (int i = 0; i < 16 && digitalRead(pin::TOUCH_SDA) == LOW; i++) {
      digitalWrite(pin::TOUCH_SCL, LOW);  delayMicroseconds(10);
      digitalWrite(pin::TOUCH_SCL, HIGH); delayMicroseconds(10);
    }
    LOG("i2c_recovery sda_after=%s", digitalRead(pin::TOUCH_SDA) == LOW ? "STILL LOW" : "released");
  }

  Wire.begin(pin::TOUCH_SDA, pin::TOUCH_SCL, 400000);
  uint8_t found = 0, pid[4] = {0}, cfg[6] = {0};
  for (uint8_t a : {0x14, 0x5D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { found = a; break; }
  }
  if (!found) {  // retry with the other address-select level
    gt911Reset(true);
    for (uint8_t a : {0x14, 0x5D}) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { found = a; break; }
    }
  }
  gGtAddr = found;
  if (found && i2cRead(found, 0x8140, pid, 4) && i2cRead(found, 0x8047, cfg, 6)) {
    int xr = cfg[1] | (cfg[2] << 8), yr = cfg[3] | (cfg[4] << 8);
    D.touchOk = true;
    D.touch = String("GT911 @0x") + String(found, HEX) + "  PID " + (char)pid[0] + (char)pid[1] + (char)pid[2] + "  res " + xr + "x" + yr + "  max " + (cfg[5] & 0x0F) + "pt";
    LOG("touch=PASS addr=0x%02X pid=%c%c%c cfg_ver=0x%02X res=%dx%d max_points=%d", found, pid[0], pid[1], pid[2], cfg[0], xr, yr, cfg[5] & 0x0F);
  } else {
    D.touch = found ? "GT911: found but read failed" : "GT911: NOT FOUND on I2C";
    LOG("touch=FAIL found_addr=0x%02X", found);
  }
}

// ---------------------------------------------------------------- 3. microSD
static void testSD() {
  SPIClass sdSpi(HSPI);
  sdSpi.begin(pin::SD_SCK, pin::SD_MISO, pin::SD_MOSI, pin::SD_CS);
  if (SD.begin(pin::SD_CS, sdSpi, 20000000)) {
    uint64_t mb = SD.cardSize() / (1024 * 1024);
    D.sd = String("SD: card ") + (unsigned)mb + " MB  type " + (int)SD.cardType();
    LOG("sd=PASS size_mb=%u type=%d", (unsigned)mb, (int)SD.cardType());
    // write + read-back + verify 512 KB, with speeds
    static uint8_t buf[4096];
    const int blocks = 128;
    bool ok = true;
    uint32_t t0 = millis();
    File f = SD.open("/diag_test.bin", FILE_WRITE);
    if (!f) ok = false;
    for (int b = 0; ok && b < blocks; b++) {
      for (int i = 0; i < 4096; i++) buf[i] = (uint8_t)(i * 7 + b);
      ok = f.write(buf, 4096) == 4096;
    }
    if (f) f.close();
    uint32_t tw = millis() - t0;
    t0 = millis();
    size_t bad = 0;
    f = SD.open("/diag_test.bin", FILE_READ);
    if (!f) ok = false;
    for (int b = 0; ok && b < blocks; b++) {
      if (f.read(buf, 4096) != 4096) { ok = false; break; }
      for (int i = 0; i < 4096; i++) bad += (buf[i] != (uint8_t)(i * 7 + b));
    }
    if (f) f.close();
    uint32_t tr = millis() - t0;
    SD.remove("/diag_test.bin");
    LOG("sd_rw=%s bytes=%d bad=%u write=%.0fKB/s read=%.0fKB/s used_mb=%u", (ok && !bad) ? "PASS" : "FAIL", blocks * 4096, (unsigned)bad,
        512000.0f / max(1u, (unsigned)tw), 512000.0f / max(1u, (unsigned)tr), (unsigned)(SD.usedBytes() / (1024 * 1024)));
    if (!(ok && !bad)) D.sd += "  R/W FAIL";
    else D.sd += "  R/W OK";
    SD.end();
  } else {
    D.sd = "SD: no card (slot not testable)";
    LOG("sd=NO_CARD");
  }
  sdSpi.end();
}

// ---------------------------------------------------------------- 4. screen
static const int LEFT_W = 400, ANIM_Y = 0, BTN_Y = 412, BTN_H = 60, BTN_W = 120;
static const int BTN_X[3] = {405, 540, 675};
static const char *BTN_LABEL[3] = {"LOCK", "NEXT", "CLEAR"};

static void drawButtons() {
  gfx->setTextFont(2);
  gfx->setTextSize(1);
  for (int i = 0; i < 3; i++) {
    uint16_t fill = (i == 0 && gLocked) ? gfx->color565(180, 90, 0) : gfx->color565(40, 60, 110);
    gfx->fillRoundRect(BTN_X[i], BTN_Y, BTN_W, BTN_H, 8, fill);
    gfx->drawRoundRect(BTN_X[i], BTN_Y, BTN_W, BTN_H, 8, TFT_WHITE);
    gfx->setTextColor(TFT_WHITE, fill);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->drawString((i == 0 && gLocked) ? "LOCKED" : BTN_LABEL[i], BTN_X[i] + BTN_W / 2, BTN_Y + BTN_H / 2);
  }
  gfx->setTextDatum(textdatum_t::top_left);
}

static void drawPattern() {
  gInfoForce = true;
  gfx->fillRect(0, 0, LEFT_W, 480, TFT_BLACK);
  const uint16_t bars[8] = {TFT_WHITE, TFT_YELLOW, TFT_CYAN, TFT_GREEN, TFT_MAGENTA, TFT_RED, TFT_BLUE, TFT_BLACK};
  for (int i = 0; i < 8; i++) gfx->fillRect(i * 50, 0, 50, 120, bars[i]);
  for (int x = 0; x < LEFT_W; x++) {  // grey ramp + R/G/B ramps
    uint8_t v = x * 255 / (LEFT_W - 1);
    gfx->drawFastVLine(x, 120, 40, gfx->color565(v, v, v));
    gfx->drawFastVLine(x, 160, 30, gfx->color565(v, 0, 0));
    gfx->drawFastVLine(x, 190, 30, gfx->color565(0, v, 0));
    gfx->drawFastVLine(x, 220, 30, gfx->color565(0, 0, v));
  }
  gfx->drawRect(0, 0, 800, 480, TFT_WHITE);  // edge-to-edge border: all four edges must be visible
  gfx->fillRect(1, 1, 24, 24, TFT_RED);               // TL red
  gfx->fillRect(776, 1, 24, 24, TFT_GREEN);           // TR green
  gfx->fillRect(1, 455, 24, 24, TFT_BLUE);            // BL blue
  gfx->fillRect(775, 455, 24, 24, TFT_WHITE);         // BR white
  drawButtons();
}

static void drawInfo() {
  // Flicker-free: no clear-then-redraw. Each line is drawn once with a padded background and only
  // when its text changed (the panel scans the framebuffer while we write it).
  gfx->setTextFont(2);
  gfx->setTextSize(1);
  gfx->setTextDatum(textdatum_t::top_left);
  gfx->setTextPadding(LEFT_W - 12);
  static String cache[12];
  if (gInfoForce) { for (auto &c : cache) c = ""; gInfoForce = false; }
  int y = 258, idx = 0;
  auto line = [&](const String &s, uint16_t c = TFT_WHITE) {
    if (cache[idx] != s) {
      cache[idx] = s;
      gfx->setTextColor(c, TFT_BLACK);
      gfx->drawString(s, 6, y);
    }
    idx++;
    y += 18;
  };
  line(String("PCLK ") + (kPclkHz[gStage] / 1000000) + " MHz  stage " + (gStage + 1) + "/" + kStages + (gLocked ? "  LOCKED" : "  auto"), TFT_YELLOW);
  line(D.chip);
  line(D.psram, D.psramOk ? TFT_GREEN : TFT_RED);
  line(D.touch, D.touchOk ? TFT_GREEN : TFT_RED);
  line(String("Touch n=") + D.lastN + " x=" + D.lastX + " y=" + D.lastY + "  max " + D.maxTouches, TFT_CYAN);
  line(D.sd);
  line(D.wifi);
  line(D.api);
  line(String("Anim ") + String(D.fps, 1) + " fps   heap " + (ESP.getFreeHeap() / 1024) + "K  psram " + (ESP.getFreePsram() / 1024) + "K");
}

static void bootScreen() {
  uint32_t pclk = kPclkHz[gStage];
  LOG("display_init pclk=%uHz stage=%d", (unsigned)pclk, gStage);
  gfx = new LGFX(pclk);
  D.lcdOk = gfx->init();
  LOG("lcd_init=%s", D.lcdOk ? "PASS" : "FAIL");
  if (!D.lcdOk) return;
  gfx->setBrightness(200);
  gfx->fillScreen(TFT_BLACK);
  spr = new LGFX_Sprite(gfx);
  spr->setPsram(true);
  spr->setColorDepth(16);
  D.sprOk = spr->createSprite(400, 400) != nullptr;
  LOG("sprite_400x400_psram=%s", D.sprOk ? "PASS" : "FAIL");
  drawPattern();
  drawInfo();
}

static void animate() {
  if (!D.sprOk) return;
  static float ang = 0;
  const uint16_t dark = gfx->color565(0, 12, 0), ring = gfx->color565(0, 90, 0);
  spr->fillSprite(dark);
  for (int r = 50; r <= 190; r += 46) spr->drawCircle(200, 200, r, ring);
  spr->drawFastHLine(0, 200, 400, ring);
  spr->drawFastVLine(200, 0, 400, ring);
  for (int k = 0; k < 40; k++) {  // fading sweep trail
    float a = (ang - k * 1.5f) * DEG_TO_RAD;
    uint8_t g = 255 - k * 6;
    spr->drawLine(200, 200, 200 + 190 * cosf(a), 200 + 190 * sinf(a), spr->color565(0, g, 0));
  }
  for (int i = 0; i < 24; i++) {  // moving "aircraft"
    float a = (ang * 0.2f + i * 15.0f) * DEG_TO_RAD, r = 30 + (i * 37) % 160;
    spr->fillCircle(200 + r * cosf(a), 200 + r * sinf(a), 3, (i & 1) ? TFT_YELLOW : TFT_CYAN);
  }
  ang += 4.0f; if (ang >= 360) ang -= 360;
  spr->pushSprite(LEFT_W, ANIM_Y);
}

// ---------------------------------------------------------------- 5. touch handling
static void gotoStage(int s) {
  prefs.putUChar("stage", (uint8_t)((s % kStages + kStages) % kStages));
  prefs.putBool("lock", gLocked);
  LOG("restart_for_stage=%d", (s % kStages + kStages) % kStages);
  Serial.flush();
  delay(50);
  ESP.restart();
}

struct TouchEvt { uint8_t n; int16_t x[5], y[5]; };
static QueueHandle_t touchQ = nullptr;
static volatile uint32_t gGtReads = 0, gGtErrs = 0, gGtEvents = 0;
static TouchEvt gLastTouch = {};
static bool gTouchDown = false;

// Reads the GT911 straight over Wire every ~8 ms in its own task, so touches are not limited by the
// ~20 fps draw loop and do not depend on the graphics library's touch layer.
static void touchTask(void *) {
  for (;;) {
    uint8_t st = 0;
    if (gGtAddr && i2cRead(gGtAddr, 0x814E, &st, 1)) {
      gGtReads++;
      if (st & 0x80) {  // buffer-ready flag; n == 0 means "finger lifted"
        TouchEvt e = {};
        int n = st & 0x0F;
        uint8_t buf[8 * 5];
        if (n > 5) n = 5;
        if (n > 0 && i2cRead(gGtAddr, 0x814F, buf, n * 8)) {
          e.n = n;
          for (int i = 0; i < n; i++) {  // point = id, xL, xH, yL, yH, sizeL, sizeH, rsv
            e.x[i] = buf[i * 8 + 1] | (buf[i * 8 + 2] << 8);
            e.y[i] = buf[i * 8 + 3] | (buf[i * 8 + 4] << 8);
          }
        }
        Wire.beginTransmission(gGtAddr);  // acknowledge: clear the buffer-ready flag
        Wire.write(0x81); Wire.write(0x4E); Wire.write(0x00);
        Wire.endTransmission();
        gGtEvents++;
        xQueueSend(touchQ, &e, 0);
      }
    } else {
      gGtErrs++;
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

static void handleTouch() {
  if (!D.lcdOk || !touchQ) return;
  static uint32_t lastLog = 0, lastBtn = 0;
  TouchEvt e;
  while (xQueueReceive(touchQ, &e, 0) == pdTRUE) {
    D.lastN = e.n;
    if (e.n > D.maxTouches) D.maxTouches = e.n;
    if (e.n == 0) { gTouchDown = false; gLastTouch = e; continue; }
    D.lastX = e.x[0]; D.lastY = e.y[0];
    if (!gTouchDown) {
      LOG("touch_down n=%d x=%d y=%d", e.n, e.x[0], e.y[0]);
      if (e.y[0] >= BTN_Y && e.y[0] < BTN_Y + BTN_H && millis() - lastBtn > 400) {
        for (int i = 0; i < 3; i++) {
          if (e.x[0] >= BTN_X[i] && e.x[0] < BTN_X[i] + BTN_W) {
            lastBtn = millis();
            LOG("button=%s", BTN_LABEL[i]);
            if (i == 0) { gLocked = !gLocked; drawButtons(); }
            if (i == 1) gotoStage(gStage + 1);
            if (i == 2) { D.maxTouches = 0; drawPattern(); drawInfo(); }
          }
        }
      }
    } else if (millis() - lastLog > 150) {
      LOG("touch_move n=%d x=%d y=%d", e.n, e.x[0], e.y[0]);
      lastLog = millis();
    }
    for (int i = 0; i < e.n; i++)
      if (e.x[i] < LEFT_W - 2) gfx->fillCircle(e.x[i], e.y[i], 6, i == 0 ? TFT_YELLOW : (i == 1 ? TFT_MAGENTA : TFT_ORANGE));
    gLastTouch = e;
    gTouchDown = true;
  }
}

// Right half is repainted by the animation every frame, so show touch markers after each push.
static void drawRightTouchMarkers() {
  if (!D.lcdOk || !gTouchDown) return;
  for (int i = 0; i < gLastTouch.n; i++)
    if (gLastTouch.x[i] >= LEFT_W && gLastTouch.y[i] < 400) gfx->drawCircle(gLastTouch.x[i], gLastTouch.y[i], 14, TFT_YELLOW);
}

// ---------------------------------------------------------------- 6. network
static void netTask(void *) {
#if HAVE_SECRETS
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    D.wifi = "WiFi join FAILED";
    LOG("wifi_join=FAIL status=%d", (int)WiFi.status());
    vTaskDelete(nullptr);
  }
  D.wifi = String("WiFi ") + WiFi.localIP().toString() + " " + WiFi.RSSI() + "dBm";
  LOG("wifi_join=PASS ip=%s rssi=%d join_ms=%u", WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned)(millis() - t0));
  for (;;) {
    char url[128];
    snprintf(url, sizeof(url), "https://api.airplanes.live/v2/point/%.4f/%.4f/%d", (double)HOME_LAT, (double)HOME_LON, (int)RADAR_RADIUS_NM);
    WiFiClientSecure client;
    client.setInsecure();  // diag only; the firmware will pin a CA bundle
    HTTPClient http;
    http.setTimeout(12000);
    uint32_t t1 = millis();
    if (http.begin(client, url)) {
      http.addHeader("User-Agent", "ESP32-ADSB-Radar-diag/0.1");
      int code = http.GET();
      if (code == 200) {
        String body = http.getString();
        uint32_t fetchMs = millis() - t1;
        JsonDocument filter;
        filter["ac"][0]["lat"] = true; filter["ac"][0]["lon"] = true; filter["ac"][0]["flight"] = true;
        filter["ac"][0]["alt_baro"] = true; filter["ac"][0]["gs"] = true; filter["ac"][0]["track"] = true;
        JsonDocument doc;
        uint32_t t2 = millis();
        DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
        uint32_t parseMs = millis() - t2;
        int count = err ? -1 : (int)doc["ac"].size();
        D.api = String("API 200 ") + fetchMs + "ms " + (body.length() / 1024) + "KB  ac=" + count + (err ? String(" ") + err.c_str() : String(" parse ok ") + parseMs + "ms");
        LOG("api=%s http=200 fetch_ms=%u parse_ms=%u bytes=%u aircraft=%d err=%s", err ? "PARTIAL" : "PASS", (unsigned)fetchMs, (unsigned)parseMs, (unsigned)body.length(), count, err.c_str());
      } else {
        D.api = String("API HTTP ") + code;
        LOG("api=FAIL http=%d", code);
      }
      http.end();
    } else {
      D.api = "API begin failed";
      LOG("api=FAIL begin");
    }
    vTaskDelay(pdMS_TO_TICKS(15000));
  }
#else
  D.wifi = "WiFi join: no config/secrets.h";
  D.api = "API: skipped (no secrets.h)";
  LOG("network_test=SKIPPED reason=no_secrets_h");
  vTaskDelete(nullptr);
#endif
}

static void testWifiScan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  uint32_t t0 = millis();
  int n = WiFi.scanNetworks();
  int best = -127;
  for (int i = 0; i < n; i++) best = max(best, (int)WiFi.RSSI(i));
  D.wifi = String("WiFi scan: ") + n + " APs, best " + best + "dBm";
  LOG("wifi_scan=%s aps=%d best_rssi=%d scan_ms=%u", n > 0 ? "PASS" : "FAIL", n, best, (unsigned)(millis() - t0));
  for (int i = 0; i < min(n, 5); i++) LOG("  ap%d ssid=%s rssi=%d ch=%d", i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  WiFi.scanDelete();
}

// ---------------------------------------------------------------- arduino
void setup() {
  Serial.begin(115200);
  delay(400);
  prefs.begin("diag2", false);
  gStage = prefs.getUChar("stage", 2) % kStages;   // default 16 MHz (stable in video test)
  gLocked = prefs.getBool("lock", true);      // no auto-cycle unless asked
  LOG("boot reset_reason=%d stage=%d locked=%d sketch=viewe7-diag/0.1", (int)esp_reset_reason(), gStage, (int)gLocked);

  testMemory();
  testTouchHardware();
  testSD();
  touchQ = xQueueCreate(32, sizeof(TouchEvt));
  if (D.touchOk) xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 2, nullptr, 0);
  bootScreen();
  testWifiScan();
  if (D.lcdOk) drawInfo();
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  gStageStart = millis();
  LOG("SUMMARY psram=%s touch=%s lcd=%s sprite=%s", D.psramOk ? "PASS" : "FAIL", D.touchOk ? "PASS" : "FAIL", D.lcdOk ? "PASS" : "FAIL", D.sprOk ? "PASS" : "FAIL");
}

void loop() {
  static uint32_t frames = 0, fpsT = millis(), infoT = 0, statT = 0;
  if (D.lcdOk) {
    animate();
    handleTouch();
    drawRightTouchMarkers();
    frames++;
  }
  uint32_t now = millis();
  if (now - fpsT >= 1000) {
    D.fps = frames * 1000.0f / (now - fpsT);
    frames = 0;
    fpsT = now;
  }
  if (D.lcdOk && now - infoT > 500) { drawInfo(); infoT = now; }
  if (now - statT > 2000) {
    statT = now;
    LOG("stat fps=%.1f heap=%uK psram_free=%uK touches_max=%d gt_reads=%u gt_errs=%u gt_events=%u stage=%d pclk=%uMHz", D.fps, (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024), D.maxTouches, (unsigned)gGtReads, (unsigned)gGtErrs, (unsigned)gGtEvents, gStage, (unsigned)(kPclkHz[gStage] / 1000000));
  }
  if (!gLocked && now - gStageStart > kStageMs) gotoStage(gStage + 1);

  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'n') gotoStage(gStage + 1);
    else if (c >= '0' && c < '0' + kStages) { gLocked = true; gotoStage(c - '0'); }
    else if (c == 'l') { gLocked = !gLocked; LOG("locked=%d", (int)gLocked); if (D.lcdOk) drawButtons(); }
    else if (c == 'c' && D.lcdOk) { drawPattern(); drawInfo(); }
    else if (c == 'i') LOG("info %s | %s | %s | %s", D.chip.c_str(), D.psram.c_str(), D.touch.c_str(), D.sd.c_str());
  }
}
