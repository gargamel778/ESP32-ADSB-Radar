# Security notes

The radar is a hobby appliance for a home network. This is what protects what, and the limits.

## What is protected, and how
| Asset | Protection |
|---|---|
| The web interface and the update function | A random 6-digit **PIN** (made on first boot, shown on the radar's screen, kept in flash) in a cookie; five wrong PINs lock sign-in for a minute; the cookie token changes at every boot; every API route except the page itself checks it |
| Firmware updates | The same PIN (web upload, link, and ArduinoOTA's challenge-response password); the image is verified completely before anything is written; the previous firmware stays in the other slot; a new image must prove itself or the bootloader reverts |
| The setup access point | WPA2 with a random 8-character password shown on the screen (and in the QR code); it exists only while setting up or while no usable network is configured |
| The FlightAware key, the Wi-Fi password | Stored in the radar's flash (NVS). Never sent back by the API, never printed to the serial log or SD log, sent only to FlightAware / your router. The page has a *Remove key* button |
| Data from the internet | HTTPS with certificate validation against the Mozilla root bundle for every external service (airplanes.live, adsbdb, FlightAware, Planespotters); JSON is parsed with a field filter and size limits; downloaded firmware must pass the image check; photos are decoded in PSRAM with a size limit |

## Limits (read these)
- **The web interface is plain HTTP.** On your network anyone who can watch the traffic can read the PIN and the cookie. Do not expose the radar to the internet (no port forwarding), and do not use it on a network you do not trust.
  Browsers' geolocation button is unavailable on plain HTTP pages for the same reason.
- **Flash is not encrypted and secure boot is off.** Anyone with physical access can read the Wi-Fi password and the API key from the board. Use an API key that is only for this device and revoke it if the radar is lost.
- **The PIN is six digits.** The lock-out makes guessing slow (about 5 tries a minute), not impossible; it is meant against casual access, not against a determined attacker on your network.
- **Serial console:** the PIN is printed at boot on the USB serial port (it is also on the screen).
- **Privacy:** airplanes.live receives your radar's coordinates (4 decimals) every 5 seconds; see the table in the [user guide](USER_GUIDE.md#what-the-radar-sends-and-to-whom).
- The radar shows other people's flights; the information is what aircraft broadcast publicly. Do not use it for anything safety-critical.

## Hardening already in the code
- Request bodies and URLs are bounded (the firmware upload is the only large body); a request must arrive complete within 3 seconds or the connection is dropped, so a client that stalls or trickles bytes cannot hold up the display or trigger the watchdog (device test `test_trickling_and_half_sent_requests_do_not_restart_the_radar`); unknown routes answer 404; the page and map library are the only unauthenticated content and are served from fixed memory (no file system, no path handling).
- The firmware upload is checked for the PIN before the first byte is written, and refuses to start while the running image is still on trial.
- The radar serves one request at a time, so a determined client on your network can still keep it busy (a flood slows the web page, not the radar screen); the PIN lock-out and the guard above do not make it a hardened server. An emergency or a flood of requests cannot starve the radar for long: the main loop and the idle tasks are watched, and the radar restarts itself if it hangs.
- `tests/device_tests.py` exercises the access control, malformed and oversized requests, forged cookies and refusals of bad firmware on a real radar (results in [TESTING.md](TESTING.md)).

## Reporting a problem
For a vulnerability, use the repository's **Security** tab, *Report a vulnerability* (GitHub's private reporting), so that nothing is public before it is fixed. For anything else, open an issue, and never post your PIN, API key or Wi-Fi details in it.
