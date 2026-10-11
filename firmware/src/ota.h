// Over-the-air firmware updates, three ways, all into the inactive app slot:
//   - the phone page (multipart upload of firmware.bin), PIN or setup-Wi-Fi protected like the rest of the web interface
//   - a link (http/https) the radar downloads from itself, optionally checked against a SHA-256
//   - PlatformIO's espota (ArduinoOTA), password = the web PIN
// A new image is "pending" until it has proven itself (display up, loop running, data arriving, 60 s): if the radar resets before that,
// the bootloader goes back to the previous image by itself. A bad file (wrong chip, truncated, corrupt) is rejected before the boot slot changes.
#pragma once
#include <Arduino.h>

namespace ota {

struct Status { bool active; uint8_t pct; bool failed; char msg[72]; };

void begin();                          // mDNS-independent ArduinoOTA setup (started once Wi-Fi is up)
void tick();                           // main loop: ArduinoOTA, progress screen, deferred restart, validation of a fresh image
Status status();
bool busy();                           // an update is running (other work, such as the ADS-B fetch, keeps out of the way)
void noteData(bool ok);                // main loop reports whether the last fetch succeeded (a validation criterion)

// One write session (the web upload and the link download use it)
bool sessionBegin(size_t total);       // total = size of the file if known, else 0
bool sessionWrite(const uint8_t *data, size_t n);
bool sessionFinish(const char *expectSha256, char *err, size_t errCap);   // verifies the image, then makes it the boot image
void sessionAbort();
bool startUrl(const char *url, const char *sha256, char *err, size_t errCap);   // background download

const char *slot();                    // "app0" / "app1"
const char *imageState();              // "valid", "pending", "installed", "invalid"
bool canRevert();                      // a previous image exists
void revert();                         // boot the previous image now (restarts)

}  // namespace ota
