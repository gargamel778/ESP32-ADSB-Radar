// LAN web interface (http://radar.local) and the phone-setup captive portal. Runs in the main loop, so its handlers may call app::* directly.
// Access: clients that joined the setup access point (WPA2) are trusted; everyone else must log in with the 6-digit PIN shown on the radar's screen.
#pragma once
#include <Arduino.h>

namespace webui {

void begin();   // call once Wi-Fi is configured
void tick();    // call every loop pass: serves requests, runs the captive-portal DNS, announces radar.local
String url();   // "http://radar.local" style address for the screen

}  // namespace webui
