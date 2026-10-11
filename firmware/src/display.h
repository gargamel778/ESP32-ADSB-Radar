// Display backends for the VIEWE UEDX80480070E-WB-A (800x480, RGB565, 16-bit parallel; pins from
// VIEWE's board header for this model).
//
// Two backends, selectable at boot (saved in NVS, cycled from the UI) while we find what is stable on this board:
//   L*  LovyanGFX's own RGB driver: the PSRAM framebuffer is streamed by DMA straight to the panel.
//   E*  Espressif esp_lcd RGB driver with a bounce buffer (what VIEWE's examples use): the ISR copies
//       framebuffer -> internal SRAM with the CPU, which costs ~PCLK*2 bytes/s of CPU+PSRAM time on the core it runs on.
//
// begin() returns a LovyanGFX target (device or a sprite wrapping the framebuffer); sprites should adopt its
// getColorDepth() so sprite -> screen pushes are plain row copies.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "config.h"

namespace display {

constexpr int WIDTH = 800, HEIGHT = 480;
constexpr int MODE_COUNT = 6;
constexpr int MODE_UI_COUNT = 3;     // modes offered by the on-screen mode tap (the rest are serial-only diagnostics)

const char *modeName(int mode);
int currentMode();                   // mode passed to begin() (what is actually running)
int isrCore();                       // core that services the display interrupts (E* modes; 1 otherwise)
lgfx::LovyanGFX *begin(int mode);    // nullptr on failure
void setBrightness(uint8_t level);   // 0..255
uint32_t vsyncCount();               // frames scanned out (E* modes only; 0 for L*)
int64_t lastVsyncUs();                // esp_timer time of the most recent vsync (E* modes)
// Vsync interval statistics since the last call (E* modes): min/max interval in us, and how many intervals were >1.6 ms longer than nominal.
void vsyncStats(uint32_t &minUs, uint32_t &maxUs, uint32_t &lateCount, uint32_t &count);
void waitVsync(uint32_t timeoutMs);
// Double buffering (E* modes): draw into backFb(), call flip(), and the panel switches to it at the next frame wrap, so the scan can
// never show a half-drawn frame. After flip() the old buffer is still being read until that wrap: waitFlipped() blocks until it is free.
// frontFb() is the buffer on screen (use it to bring the new back buffer up to date). L* modes: doubleBuffered() is false.
bool doubleBuffered();
LGFX_Sprite *backFb();
LGFX_Sprite *frontFb();
void flip();
uint32_t flipUs();                   // smoothed duration of flip() in microseconds (direct mode: includes the cache write-back)
bool waitFlipped(uint32_t timeoutMs);  // E* modes: returns at (or just after) a vsync; L* modes: returns immediately

}  // namespace display
