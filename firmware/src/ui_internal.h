// Shared by ui.cpp (settings tabs, gestures) and ui_wifi.cpp (Wi-Fi sub-screens: network list, keyboard, connecting, phone setup).
#pragma once
#include "gfx.h"

namespace ui {
namespace in {

struct Cols { uint16_t bright, mid, dim, dark, title, title2, val, val2; };
extern Cols C;                                              // palette colours for the tile being painted
extern const lgfx::IFont *const F24, *const F18, *const F12;

void markDirty(gfx::Rect r);   // repaint a region (painted in two consecutive frames so both framebuffers match)
void markAll();
void drawBtn(gfx::Ctx &g, const gfx::Rect &r, const char *text, bool down, bool enabled = true);   // rounded outline button
void drawHeader(gfx::Ctx &g, const gfx::Rect &t, const char *title);                                // title bar with the line under it
gfx::Rect backRect();                                       // top-right button, same place as "Done"

// A full-screen sub-screen that replaces the settings tabs.
struct Screen {
  virtual ~Screen() {}
  virtual void enter() {}
  virtual void leave() {}
  virtual void draw(gfx::Ctx &g, const gfx::Rect &t) = 0;
  virtual void down(int x, int y) {}
  virtual void move(int x, int y) {}
  virtual void up(int x, int y) {}
  virtual void tick() {}       // every frame while shown
};
void setSub(Screen *s);        // nullptr = back to the settings tabs (on the Wi-Fi tab)

// on-screen keyboard (ui_wifi.cpp): ok(text) when OK is pressed, cancel() on Cancel. Both are expected to switch to another screen.
void openKeyboard(const char *title, const char *initial, bool secret, size_t maxLen, void (*ok)(const char *), void (*cancel)());

// entry points implemented in ui_loc.cpp (Settings > Location)
void openCitySearch();
void openCoordinates();
void openCountryPick();
void openZonePick();
void openMap();
void openFlightKey();           // Settings > Flights: on-screen keyboards for the AeroAPI key and the contact e-mail
void openFlightEmail();
void openAbout();                // credits and data licences
void openLogList();              // SD card log files, newest first; opens a viewer for the tail of the chosen file

void otaShow(int pct, const char *msg, bool failed);
void otaShowDark(int pct);   // switch to / update the firmware update screen
void otaHide();

// entry points implemented in ui_wifi.cpp
void openScan();               // choose a network from a scan
void openPhone();              // phone setup screen: shows the setup access point's name, password and QR code

}  // namespace in
}  // namespace ui
