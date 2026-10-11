// Wi-Fi management: joins the saved network, scans, switches networks with automatic fallback to the old one,
// and runs the phone-setup access point (alongside the normal connection, so the radar keeps working).
#pragma once
#include <Arduino.h>

namespace wifimgr {

struct Net { char ssid[33]; int8_t rssi; bool secure; };
constexpr int MAX_NETS = 24;

void begin();                         // join the saved network (non-blocking); starts the setup access point if none is saved
void tick();                          // call every loop pass

bool connected();
String ssid();                        // current network ("" if none)
int rssi();
String ip();
void simulateOutage(uint32_t ms);     // dev: drop the connection for a while (serial 'w')

// scan for networks (asynchronous: a scan takes 2-4 s and briefly interrupts the connection)
void startScan(uint32_t delayMs = 0);   // delayMs lets an HTTP response leave before the radio starts hopping channels
bool scanning();
uint32_t scanAgeMs();                 // time since the last finished scan (UINT32_MAX if none)
int scanCount();                      // strongest first, duplicates removed
const Net &scanNet(int i);

// switch network: tries the new credentials; on failure goes back to the old ones and says why
enum JoinState : uint8_t { J_IDLE, J_TRYING, J_OK, J_FAILED };
void join(const char *ssid, const char *pass);
JoinState joinState();
const char *joinMessage();
void joinDone();                      // acknowledge a result (back to J_IDLE)

// Phone-setup access point. It runs when (a) the setup screen is on the display, or (b) the radar has no network to use: none was ever
// configured, or the saved one has been unreachable for 30 s ("not found"). It switches off when a connection is made or, for (a), when the screen is left.
void screenWantsAp(bool on);          // the setup screen appeared / went away
void debugNoNetwork(bool on);         // dev (serial 'N'): behave as if no network were saved (not stored)
bool apActive();
String apSsid();
const char *apPass();
String apQr();                        // "WIFI:T:WPA;S:...;P:...;;" for the QR code
uint32_t apClients();
void debugSetupDone();                // dev (serial 'K'): behave as if a join had just succeeded while the setup access point is up
bool setupComplete();                 // a network was joined while the setup access point was up (phones' sign-in checks should now succeed)
uint32_t setupSecondsLeft();          // until the setup access point switches itself off (0 when not counting down)

}  // namespace wifimgr
