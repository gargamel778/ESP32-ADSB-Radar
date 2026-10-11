# Architecture

## The board and what limits it
ESP32-S3 (two 240 MHz cores), 16 MB flash, 8 MB octal PSRAM, an 800×480 RGB565 panel on 16 parallel data lines, a GT911 touch controller, a microSD slot on SPI. The things that decide the design:

- **Internal RAM is scarce** (about 55 KB free once everything runs; the lowest seen in a 10-hour soak is 33 KB). Everything big lives in PSRAM: the two frame buffers (768 KB each), the static
  radar layer, aircraft snapshots, JSON documents (a custom ArduinoJson allocator), TLS buffers (a hook in `tls_psram.cpp` moves mbedTLS' allocations to PSRAM; a secure connection then costs about 3 KB of internal RAM
  instead of 45 KB), the OTA staging buffer, photos.
- **The display and PSRAM share a bus.** The panel is refreshed at 31 Hz by DMA; reading the frame buffer straight from PSRAM starves under load. The default display mode (`E16c1`) uses Espressif's `esp_lcd` RGB driver
  with a **bounce buffer**: an interrupt on core 1 copies ten lines at a time from the PSRAM frame buffer into internal RAM, and the DMA reads from there. The IDF libraries are rebuilt once with
  `CONFIG_LCD_RGB_RESTART_IN_VSYNC=n` so that a late interrupt costs one glitched frame instead of a permanently scrambled picture. Flash erase/write turns the cache off for milliseconds and starves that interrupt:
  hence the "plain fill" screen during a firmware update (`docs/hardware-findings.md` has the measurements).
- **The renderer never touches the live frame buffer.** Two frame buffers are flipped by the driver at the frame wrap. Drawing happens in the hidden buffer, in small tiles (128×72) and bands composed in internal RAM, then pushed; the
  hidden buffer is brought up to date by copying only last frame's changed rectangles (minus the ones being repainted now). Static things (rings, grid, scale, map overlay) live in a layer that is rebuilt only when the range, location or colours change.

## Tasks
| Task | Core | Role |
|---|---|---|
| Arduino `loop()` | 1 | Touch events, UI and radar frames (~30 per second), the web server (`handleClient`), Wi-Fi manager, settings save, night mode, health monitor, serial developer commands. Watched by the task watchdog (15 s) |
| Display interrupt | 1 | Bounce-buffer refill (E16c1). Created by the `lcd_init` task so that the interrupt is allocated on the chosen core |
| `adsb` (20 KB stack) | 0 | The data task: aircraft fetch every 5 s (HTTPS, filtered JSON parse), then, between fetches, route lookups (adsbdb) and one step at a time of flight-detail lookups (FlightAware, Planespotters, JPEG decode) |
| `gt911` | 0 | Polls the touch controller every 8 ms, queues events |
| `sdlog` | 0 | Owns the SD card: batches of log lines, remount on removal, retention, eject |
| `ota_url` | 0 | Temporary: downloads a firmware from a link |
| Wi-Fi / lwIP / mDNS / ArduinoOTA | 0 / loop | ESP-IDF's own tasks; ArduinoOTA and the DNS server for the captive portal run from `loop()` |

The idle tasks are watched (15 s) as well. Breadcrumbs (`phase.h`) in RTC memory record which step each task was in, so that a watchdog reset is explained in the SD log at the next boot.

## Data flow
```
airplanes.live ──HTTPS──► adsb task ──Snapshot (PSRAM, double-buffered)──► loop: radar::setData
                                                                               │  plots[] (distance/bearing, dead-reckoned every 500 ms between fetches)
touch (gt911 task) ──events──► ui::touch ──► selection / settings / zoom      │
                                                                               ▼
settings (NVS) ──► app:: setters ──► radar::apply*  ──► tiles + bands ──► back frame buffer ──flip──► panel
adsbdb / FlightAware / Planespotters ──► route / flightinfo caches ──► card, track, photo
```
- **Aircraft data** is parsed with an ArduinoJson *filter* (only the fields used), the nearest 150 aircraft are kept, and a snapshot is published under a mutex. Between fetches the radar moves each aircraft along its track
  at its ground speed, so marks glide instead of jumping every 5 seconds.
- **Offline data** in flash (`firmware/data`, embedded with `board_build.embed_files`): `geo.bin` (34 k cities → country and time zone), `world.bin` (country outlines for the map picker), `mapdata.bin` (coastlines, borders,
  airports for the overlay, drawn with an azimuthal-equidistant projection around the radar), `airlines.bin` (callsign prefix → airline), `flags.bin` (flag bitmaps), `page.html.gz`.
- **Country of registration** is a binary search of the ICAO address blocks (`icao_ranges.inc`, generated from the Annex 10 table) and a flag lookup: no network involved.
- **Settings** are in one NVS namespace, loaded at boot, saved 3 seconds after the last change. Modules never change settings themselves; the `app::` functions do (change, tell the affected modules, schedule the save).

## Network behaviour
- `wifimgr`: join the saved network without blocking; scan asynchronously; join a new network with rollback to the old one; a setup access point plus DNS server and captive-portal probe answers (so phones raise the sign-in sheet and close it
  when done) under rules written down in the source comments; a watchdog that re-joins after 60 s offline.
- `webui`: Arduino `WebServer` with a PIN cookie, a JSON API ([WEB_API.md](WEB_API.md)), a gzipped single-page UI served by a writer that keeps going until the whole page is out (the stock writer gives up on a busy link).
  The library parses a request in one blocking go (each read waits up to 5 s), so `GuardedServer` in `webui.cpp` accepts the connection itself and starts the parse only once the headers (and a small body) have arrived, found by peeking at the socket; a client that has not finished within 3 s is dropped. Without it, one stalled client held the main loop until the 15 s watchdog restarted the radar (found by the device tests).
- HTTPS everywhere it matters: certificates are checked against the Mozilla bundle built into the Arduino core, which is why the clock must be set (NTP) before the first fetch.

## Firmware update and recovery
Two OTA slots. `ota.cpp` receives into PSRAM, verifies (`imagecheck.h`), erases and writes the spare slot in one burst, sets it as the boot slot and restarts. The bootloader's app-rollback is on: the new image runs *pending* and
`ota::tick()` confirms it after 60 s with live data (or 10 minutes without a network); a reset before that boots the old slot. ArduinoOTA (espota) shares the same rules. `esp_ota_*` is used directly, not Arduino's `Update`,
so that the SHA-256 is checked before the boot slot is changed.

## Health and watchdogs
Task watchdog on the loop task and both idle tasks (15 s); a monitor in `main.cpp` restarts the radar if no aircraft data arrives for 15 minutes with Wi-Fi up, if Wi-Fi is down for 30 minutes with a saved network,
or if the display interrupt stops counting frames; intentional restarts flush the SD log and leave a mark so the next boot can tell them from crashes (this chip reports many software restarts as a watchdog reset).

## Source map (`firmware/src`)
| File | What |
|---|---|
| `main.cpp` | setup, main loop, health monitor, serial developer commands |
| `display.*` | display backends and double buffering |
| `radar.*` | the renderer: layout, tiles, dead reckoning, selection, card, track, emergency highlight |
| `ui*.cpp`, `gfx.h` | settings screens, keyboard, Wi-Fi / location screens, drawing helpers |
| `adsb.*` | aircraft fetch, routes (adsbdb), the data task |
| `flightinfo.*`, `jpegdec.cpp` | FlightAware and Planespotters lookups, photo decoding (stb_image) |
| `icao.*`, `geo.*`, `airline.*`, `mapdata.*` | embedded-data lookups |
| `settings.*`, `app.*`, `night.*`, `units.h`, `palette.h` | settings, actions, night mode, units, colours |
| `wifimgr.*`, `webui.*`, `ota.*`, `sdlog.*` | network, web interface, updates, SD logging |
| `solar.h`, `isotime.h`, `imagecheck.h` | pure logic kept header-only so the host tests can compile it |
| `touch_gt911.*`, `tls_psram.cpp`, `phase.h` | touch driver, TLS memory hook, crash breadcrumbs |
