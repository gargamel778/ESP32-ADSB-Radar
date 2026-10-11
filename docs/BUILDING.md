# Building, flashing and updating

## What you need
- The board: **VIEWE UEDX80480070E-WB-A** (7" 800×480, ESP32-S3 N16R8; [VIEWE's repository for it](https://github.com/VIEWESMART/UEDX80480070ESP32-7inch-Touch-Display)). A microSD card is optional (logging).
- A USB-C cable to the connector labelled **UART** (CH340 serial chip). The other USB-C connector does not carry data on this board.
- **PlatformIO Core** ([install guide](https://platformio.org/install/cli)) and Python 3. The project pins its platform
  (`pioarduino` 54.03.21: Arduino core 3.2.1 on ESP-IDF 5.4.2), which PlatformIO downloads itself.
- About 2 GB of disk for the toolchains and the first build, and an internet connection for the first build.

> On macOS use PlatformIO's own environment (`~/.platformio/penv/bin/pio`). A `pio` from another Python installation can break the pinned esptool
> (see `docs/hardware-findings.md`, gotcha 6).

## First build
```
git clone <this repository>
cd ESP32-ADSB-Radar
cp config/secrets.h.example config/secrets.h                        # optional: first-boot Wi-Fi name / password and location
cp firmware/platformio.local.ini.example firmware/platformio.local.ini   # set the serial port of the radar
cd firmware
pio run -t upload
```
`config/secrets.h` and `platformio.local.ini` are private to your computer and are never committed (`.gitignore`). Without `secrets.h` the radar starts with
a London location and no Wi-Fi, shows its setup screen and its own access point; you can do all of it from the touch screen or a phone.

**The first build takes about 15 minutes.** The firmware needs ESP-IDF libraries built with one non-default setting (`CONFIG_LCD_RGB_RESTART_IN_VSYNC=n`,
which lets the display recover from a late interrupt instead of staying scrambled; see `docs/hardware-findings.md`, "Display decision v3"). PlatformIO builds
those libraries once (`custom_sdkconfig` in `firmware/platformio.ini`) and caches them; later builds take about 20 seconds. The build also creates the
folders `firmware/managed_components/`, `firmware/.dummy/` and the `sdkconfig*` files: they are generated, not committed.

Two things to know:
- The rebuilt libraries **replace PlatformIO's shared `framework-arduinoespressif32-libs` package**. Building `hardware-tests/diag` afterwards (it uses the stock libraries) makes
  PlatformIO reinstall them, and the next firmware build rebuilds again. Keep to one of the two projects at a time.
- The messages `esp_idf_size: error: unrecognized arguments: --ng` and similar `esptool get_metavar` lines during the library stage are harmless.

## Flashing
`pio run -t upload` writes bootloader, partition table and application at 230400 baud (the CH340 is unreliable above that). The serial console is
`pio device monitor` (115200 baud; DTR and RTS are held low so that opening the monitor does not restart the board).

The flash holds two 6.25 MB application slots (`firmware/default_16MB.csv`) so that updates can be installed over Wi-Fi without risking the running firmware. A build is about
4.9 MB. Embedded data (cities, map, flags, airline names, the web page) is part of the application image.

## Updating over Wi-Fi
After the first USB flash you never need the cable again:
- **Phone / PC page:** `http://radar.local`, section *Firmware*: choose `firmware/.pio/build/viewe7/firmware.bin` and press *Install file*, or paste a link (and optionally its SHA-256).
- **PlatformIO:**
  ```
  RADAR_OTA_PIN=<web PIN> pio run -e viewe7-ota -t upload --upload-port radar.local
  ```
  The PIN is shown on the radar under Settings > Wi-Fi. Python's `espota` makes the radar connect back to the computer's address: on a computer connected to the same
  network twice (Wi-Fi and Ethernet) the transfer can stall at 0-1 %. Disconnect one, or use the phone page.

How an update stays safe: the file is collected completely in memory (so the screen stays clean), checked (ESP32-S3 image, segments, the image's own SHA-256, and yours if you gave one) **before**
the spare slot is touched, written in one burst (the screen shows a plain fill meanwhile: flash writes disturb the display's refresh interrupt), and booted *pending*. It becomes permanent only after
it has run a minute with live aircraft data; a crash or reset before that makes the bootloader return to the previous firmware. *Previous firmware* on the page switches back by hand.

## Build-time options
Set with `PLATFORMIO_BUILD_FLAGS='-DNAME=value ...' pio run`:
| Flag | Effect |
|---|---|
| `FW_VERSION="x.y-test"` | Version string shown in Settings and on the page (to tell test builds apart) |
| `FA_BASE="http://host:8100/aeroapi"` | Talk to a FlightAware stand-in (`tools/mock_aeroapi.py`) instead of the real service |
| `DEV_SCREENSHOT` | Adds `GET /api/screen` (the picture on the screen as raw RGB565, PIN required) for `tools/devtools/radar_screen.py`; documentation pictures only, never in a release |
| `FA_KEY_OVERRIDE="TESTKEY"` | Send this key to the stand-in instead of the stored one (so a real key stored on the radar is never sent to it) |
| `PS_BASE="http://host:8100"` | Same for the Planespotters photo service |
| `OTA_TEST_MODE=1` / `2` | Test images that crash at start-up / 25 s after start (to test the automatic rollback) |
| `FA_MAX_CALLS_PER_HOUR=n` | Ceiling on FlightAware calls (default 60) |
| `HOME_LAT`, `HOME_LON`, `TIMEZONE_POSIX`, `DEFAULT_COUNTRY`, ... | Defaults for the first boot (also settable in `config/secrets.h`) |

## Regenerating the embedded data
The files in `firmware/data/` are generated and committed. The scripts and their sources are listed in `docs/DATA_SOURCES.md`:
`tools/build_geo.py` (cities, countries, time zones, world outline), `build_map.py` (coastlines, borders, airports), `build_airlines.py`, `build_flags.py`
(draws the flags; needs `rsvg-convert` and ImageMagick), `build_icao_ranges.py` (ICAO address blocks from the Annex 10 table). `page.html` is gzipped automatically by `tools/pio_gzip.py` during the build.

## Tests
```
tests/run_all.sh                     # C++ host tests (logic and embedded data) and Python project tests; no hardware
RADAR_HOST=radar.local RADAR_PIN=123456 tests/run_all.sh --device    # plus the tests against a running radar
```
See [TESTING.md](TESTING.md).

## Troubleshooting
| Symptom | Cause / fix |
|---|---|
| Upload fails, wrong port | Set the port in `firmware/platformio.local.ini`; use the USB-C connector labelled UART |
| `Corrupt data, expected 0x1000 bytes` while flashing | The CH340 dislikes high speeds: the project already uses 230400; try a shorter cable or another USB port |
| Black screen after power-up | Check the serial log; the factory firmware has a known warm-reset bug (see `hardware-findings.md`), this firmware resets the touch controller itself |
| Build error about `framework-arduinoespressif32-libs` | A different project replaced the shared libraries: run the build again, it repairs itself (takes the long first-build time again) |
| First build stops during the library stage | Re-run `pio run`; it resumes. Needs internet access to GitHub and the PlatformIO registry |
