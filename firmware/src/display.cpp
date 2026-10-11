#include "display.h"
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <string.h>
#include <esp_timer.h>

namespace display {
namespace {

struct ModeInfo { const char *name; bool espressif; uint32_t pclk; int isrCore; bool bounce; };
const ModeInfo kModes[MODE_COUNT] = {
    // Espressif bounce-buffer mode: an interrupt refills a 10-line buffer 48x per frame from the PSRAM framebuffer (immune to PSRAM latency).
    // Needs the IDF library built with LCD_RGB_RESTART_IN_VSYNC=n (platformio.ini), which makes a missed interrupt cost one frame instead of a permanent desync.
    {"E16c1", true, 16000000, 1, true},   // DEFAULT. ISR on core 1 (the UI core): clean at 400 km, bar test passes, stall test = single-frame glitches
    {"E16c0", true, 16000000, 0, true},   // ISR on core 0 (with Wi-Fi): clean on the radar, but text jumps a lot under the stall test
    {"L12", false, 12000000, 0, false},   // LovyanGFX 12 MHz: steady, text shimmers (24 Hz refresh) - fallback
    // Hidden from the on-screen mode cycle (serial 3..5 only), kept for diagnostics:
    {"L16", false, 16000000, 0, false},   // LovyanGFX 16 MHz: slight shimmer/jumps
    // Espressif DIRECT mode (no bounce buffers): the display DMA reads the PSRAM framebuffer itself. Starved by CPU PSRAM traffic at 400 km:
    // D16 scrolls the picture sideways, D14 leaves it offset. Not usable with this renderer.
    {"D16", true, 16000000, 1, false},
    {"D14", true, 14000000, 1, false},
    // rejected after testing: L20 (garbled), E20c1 (jumps and tears even with a short trail and 15 fps)
};

int gIsrCoreSel = 1, gCurMode = 0;
constexpr int PIN_BACKLIGHT = 2;
constexpr size_t BOUNCE_LINES = 10;  // as VIEWE: two 16 KB internal buffers

// ---------------------------------------------------------------- backend L: LovyanGFX
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB _bus;
  lgfx::Panel_RGB _panel;
  lgfx::Light_PWM _light;

 public:
  explicit LGFX(uint32_t pclkHz) {
    {
      auto cfg = _panel.config();
      cfg.memory_width = WIDTH;
      cfg.memory_height = HEIGHT;
      cfg.panel_width = WIDTH;
      cfg.panel_height = HEIGHT;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel.config(cfg);
    }
    {
      auto cfg = _bus.config();
      cfg.panel = &_panel;
      cfg.pin_d0 = 8;   cfg.pin_d1 = 3;   cfg.pin_d2 = 46;  cfg.pin_d3 = 9;   cfg.pin_d4 = 1;    // B0-B4
      cfg.pin_d5 = 5;   cfg.pin_d6 = 6;   cfg.pin_d7 = 7;   cfg.pin_d8 = 15;  cfg.pin_d9 = 16;   // G0-G4
      cfg.pin_d10 = 4;                                                                           // G5
      cfg.pin_d11 = 45; cfg.pin_d12 = 48; cfg.pin_d13 = 47; cfg.pin_d14 = 21; cfg.pin_d15 = 14;  // R0-R4
      cfg.pin_henable = 40;
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
      cfg.pin_bl = PIN_BACKLIGHT;
      cfg.freq = 20000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX *gLgfx = nullptr;

// ---------------------------------------------------------------- backend E: Espressif esp_lcd
esp_lcd_panel_handle_t gPanel = nullptr;
SemaphoreHandle_t gDone = nullptr;
esp_err_t gErr = ESP_OK;
uint32_t gPclk = 0;
bool gBounce = true;
volatile uint32_t gWrapCount = 0;   // frame-complete events (a frame wrap in bounce mode; DMA end-of-frame in direct mode)
uint32_t gFlipWrapCount = 0;
volatile uint32_t gVsync = 0;
volatile int64_t gLastVsyncUs = 0;
volatile uint32_t gIvMin = 0xFFFFFFFF, gIvMax = 0, gIvLate = 0, gIvCount = 0;
SemaphoreHandle_t gVsyncSem = nullptr;
LGFX_Sprite gFbSpr[2];  // wrap the two panel framebuffers
void *gFbPtr[2] = {nullptr, nullptr};
int gBackIdx = 1;
bool gDouble = false;
volatile int64_t gFlipUs = 0, gWrapUs = 0;
uint32_t gFlipDurAvg = 0;
SemaphoreHandle_t gWrapSem = nullptr;
bool gEspressif = false;

bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  gVsync = gVsync + 1;
  int64_t now = esp_timer_get_time();
  if (gLastVsyncUs) {
    uint32_t iv = (uint32_t)(now - gLastVsyncUs);
    if (iv < gIvMin) gIvMin = iv;
    if (iv > gIvMax) gIvMax = iv;
    if (iv > (uint32_t)(976ull * 522ull * 1000000ull / gPclk + 1600)) gIvLate = gIvLate + 1;  // nominal frame time + 1.6 ms
    gIvCount = gIvCount + 1;
  }
  gLastVsyncUs = now;
  BaseType_t woken = pdFALSE;
  if (gVsyncSem) xSemaphoreGiveFromISR(gVsyncSem, &woken);
  return woken == pdTRUE;
}

// Called by the driver at the frame wrap, right after it has switched to the framebuffer chosen by flip().
bool IRAM_ATTR onFrameBufComplete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  gWrapUs = esp_timer_get_time();
  gWrapCount = gWrapCount + 1;
  BaseType_t woken = pdFALSE;
  if (gWrapSem) xSemaphoreGiveFromISR(gWrapSem, &woken);
  return woken == pdTRUE;
}

// The bounce-buffer interrupt is allocated on the core that creates the panel, so this runs pinned to the chosen core.
void createPanelTask(void *) {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = gPclk;
  cfg.timings.h_res = WIDTH;
  cfg.timings.v_res = HEIGHT;
  cfg.timings.hsync_pulse_width = 48;
  cfg.timings.hsync_back_porch = 40;
  cfg.timings.hsync_front_porch = 88;
  cfg.timings.vsync_pulse_width = 6;
  cfg.timings.vsync_back_porch = 26;
  cfg.timings.vsync_front_porch = 10;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 2;
  cfg.bounce_buffer_size_px = gBounce ? WIDTH * BOUNCE_LINES : 0;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = 39;
  cfg.vsync_gpio_num = 41;
  cfg.de_gpio_num = 40;
  cfg.pclk_gpio_num = 42;
  cfg.disp_gpio_num = -1;
  const int pins[16] = {8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14};
  memcpy(cfg.data_gpio_nums, pins, sizeof(pins));
  cfg.flags.fb_in_psram = 1;

  gErr = esp_lcd_new_rgb_panel(&cfg, &gPanel);
  if (gErr == ESP_OK) {
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_vsync = onVsync;
    cbs.on_frame_buf_complete = onFrameBufComplete;
    esp_lcd_rgb_panel_register_event_callbacks(gPanel, &cbs, nullptr);
    esp_lcd_panel_reset(gPanel);
    gErr = esp_lcd_panel_init(gPanel);
  }
  xSemaphoreGive(gDone);
  vTaskDelete(nullptr);
}

lgfx::LovyanGFX *beginEspressif(const ModeInfo &m) {
  gPclk = m.pclk;
  gBounce = m.bounce;
  gVsyncSem = xSemaphoreCreateBinary();
  gWrapSem = xSemaphoreCreateBinary();
  gDone = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(createPanelTask, "lcd_init", 6144, nullptr, 5, nullptr, m.isrCore);
  xSemaphoreTake(gDone, portMAX_DELAY);
  if (gErr != ESP_OK || !gPanel) {
    Serial.printf("[display] esp_lcd RGB panel FAILED: %s\n", esp_err_to_name(gErr));
    return nullptr;
  }
  esp_lcd_rgb_panel_get_frame_buffer(gPanel, 2, &gFbPtr[0], &gFbPtr[1]);
  if (!gFbPtr[0] || !gFbPtr[1]) { Serial.println("[display] no framebuffers"); return nullptr; }
  for (int i = 0; i < 2; i++) {
    gFbSpr[i].setColorDepth(lgfx::rgb565_nonswapped);  // the RGB peripheral reads native little-endian RGB565
    gFbSpr[i].setBuffer(gFbPtr[i], WIDTH, HEIGHT);
    gFbSpr[i].fillSprite(TFT_BLACK);
  }
  gBackIdx = 1;  // the panel starts on buffer 0
  gDouble = true;
  ledcAttach(PIN_BACKLIGHT, 20000, 8);
  gEspressif = true;
  return &gFbSpr[gBackIdx];
}

}  // namespace

const char *modeName(int mode) { return kModes[(mode % MODE_COUNT + MODE_COUNT) % MODE_COUNT].name; }

int isrCore() { return gIsrCoreSel; }
int currentMode() { return gCurMode; }

lgfx::LovyanGFX *begin(int mode) {
  const ModeInfo &m = kModes[(mode % MODE_COUNT + MODE_COUNT) % MODE_COUNT];
  gIsrCoreSel = m.isrCore;
  gCurMode = (mode % MODE_COUNT + MODE_COUNT) % MODE_COUNT;
  lgfx::LovyanGFX *d = nullptr;
  if (m.espressif) {
    d = beginEspressif(m);
  } else {
    gLgfx = new LGFX(m.pclk);
    gLgfx->init();
    d = gLgfx;
  }
  if (d) {
    setBrightness(200);
    Serial.printf("[display] mode=%s backend=%s pclk=%uMHz isr_core=%d heap_int=%uK\n", m.name, !m.espressif ? "lovyan" : m.bounce ? "esp_lcd+bounce" : "esp_lcd direct",
                  (unsigned)(m.pclk / 1000000), m.espressif && m.bounce ? m.isrCore : -1, (unsigned)(ESP.getFreeHeap() / 1024));
  }
  return d;
}

void setBrightness(uint8_t level) {
  if (gEspressif) ledcWrite(PIN_BACKLIGHT, level);
  else if (gLgfx) gLgfx->setBrightness(level);
}

uint32_t vsyncCount() { return gVsync; }
int64_t lastVsyncUs() { return gLastVsyncUs; }
void vsyncStats(uint32_t &minUs, uint32_t &maxUs, uint32_t &lateCount, uint32_t &count) {
  minUs = gIvMin; maxUs = gIvMax; lateCount = gIvLate; count = gIvCount;
  gIvMin = 0xFFFFFFFF; gIvMax = 0; gIvLate = 0; gIvCount = 0;
}

bool doubleBuffered() { return gDouble; }
LGFX_Sprite *backFb() { return &gFbSpr[gBackIdx]; }
LGFX_Sprite *frontFb() { return &gFbSpr[gBackIdx ^ 1]; }

uint32_t flipUs() { return gFlipDurAvg; }

void flip() {
  if (!gDouble) return;
  const int64_t tf0 = esp_timer_get_time();
  // the buffer pointer lies inside the driver's framebuffers, so the driver does not copy: it just makes it current at the next wrap
  esp_lcd_panel_draw_bitmap(gPanel, 0, 0, WIDTH, HEIGHT, gFbPtr[gBackIdx]);  // direct mode: also writes the CPU cache back to PSRAM for this buffer
  gFlipWrapCount = gWrapCount;  // read AFTER the call: a wrap racing with it just makes us wait one frame longer
  gFlipUs = esp_timer_get_time();
  gBackIdx ^= 1;
  gFlipDurAvg = (gFlipDurAvg * 7 + (uint32_t)(gFlipUs - tf0)) / 8;
}

bool waitFlipped(uint32_t timeoutMs) {
  if (!gDouble) return true;
  uint32_t t0 = millis();
  while (gWrapCount == gFlipWrapCount) {
    if (millis() - t0 > timeoutMs) return false;
    xSemaphoreTake(gWrapSem, pdMS_TO_TICKS(5));
  }
  return true;
}

// Update the framebuffer right after vsync and the scan (which begins ~2 ms later) stays behind us: no tearing.
// If we are already within a few ms of the last vsync (the scan is still in the top lines) go now rather than lose a frame.
void waitVsync(uint32_t timeoutMs) {
  if (!gEspressif || !gVsyncSem) return;
  if (esp_timer_get_time() - gLastVsyncUs < 4000) return;
  xSemaphoreTake(gVsyncSem, 0);  // drop a stale signal
  xSemaphoreTake(gVsyncSem, pdMS_TO_TICKS(timeoutMs));
}

}  // namespace display
