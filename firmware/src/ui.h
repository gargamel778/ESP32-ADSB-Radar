// Touch UI: gestures on the radar screen (buttons, pinch zoom, tap to select) and the full-screen settings screen.
#pragma once
#include <Arduino.h>
#include "touch_gt911.h"
#include "display.h"

namespace ui {

void begin(lgfx::LovyanGFX *disp);
bool active();                       // the settings screen is open (call frame() instead of radar::frame())
void open();
void openSetup();                      // open straight on the phone-setup screen (nothing saved to connect to)
void close();
void touch(const gt911::Event &e);   // feed every touch event here
void frame();
void otaProgress(int pct, const char *msg, bool failed);
void otaDark(int pct);   // plain rising fill, for while flash is written (anything with detail would tear then)   // firmware update progress, full screen; paints at once (call from the main loop thread)
void otaDone();                       // leave the progress screen                        // paints the settings screen where something changed

}  // namespace ui
