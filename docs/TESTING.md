# Testing

Four layers, from fast and offline to slow and physical. `tests/run_all.sh` runs the first two; the third needs a radar on your network; the fourth is a day of waiting.

| Layer | What | Where | Needs |
|---|---|---|---|
| 1. Host C++ tests | The firmware's pure logic and its **embedded data files as shipped** | `tests/host`, run by `tests/run_host.sh` | a C++ compiler |
| 2. Project tests | Data files, web page, settings storage, tools, privacy tooling, documentation | `tests/test_project.py` | Python 3 (Node and ImageMagick optional) |
| 3. Device tests | Access control, hostile requests, settings, update refusals, on a real radar | `tests/device_tests.py` | a radar, its PIN |
| 4. Soak | 24 hours of normal running, sampled once a minute | `tools/soak.py` | a radar and a computer |

```
tests/run_all.sh                                                   # layers 1 and 2
RADAR_HOST=radar.local RADAR_PIN=123456 tests/run_all.sh --device  # plus layer 3 (do not run during a soak)
RADAR_PIN=123456 python3 tools/soak.py poll --host radar.local --out soak.csv     # layer 4; then:  python3 tools/soak.py report soak.csv
```

## 1. Host C++ tests (`tests/host/test_main.cpp`)
The firmware's `geo.cpp`, `airline.cpp`, `icao.cpp`, `mapdata.cpp` and the header-only `solar.h`, `isotime.h`, `imagecheck.h`, `units.h`, `palette.h` are compiled for the computer against a tiny Arduino shim
and linked with the real data files (`firmware/data/*.bin`, embedded with `.incbin` exactly as the firmware embeds them). 113 checks:

- **Sun elevation** (night mode): 5,000 random places and times against the NOAA solar-calculator formulation (worst difference 0.011°), plus solstice / polar day / polar night / equinox sanity.
- **ISO 8601 parsing** (FlightAware times): 20,000 random times against the C library, leap days, malformed input.
- **Firmware image check** (OTA): the real build passes; truncations at nine sizes, 300 random single-bit flips anywhere in the image, wrong magic, wrong chip, no segments and random junk are all refused.
- **ICAO address → country:** both ends and the middle of every one of the 190 blocks, the address just before each block, 200,000 random addresses against an independent linear search of the table, malformed input, and
  well-known registrations; every country the table can return has a flag; flags are real pictures; a few flags are checked pixel by pixel.
- **Airline names:** known and unknown callsigns, registrations, case, the whole 3-letter space.
- **Places and time zones:** nearest city → country and zone for twelve places around the world; city search; every one of the 400+ zones' POSIX rule is understood by the C library with plausible winter and summer offsets;
  London / New York / Tokyo / Sydney / Kolkata offsets; the first-boot default rule exists.
- **Map data:** coastlines and the airport (LHR) are found around London.
- **Units and palette:** conversions, range steps (never beyond the data service's 250 nm limit), the red swatch that night mode depends on.

## 2. Project tests (`tests/test_project.py`, 24 tests)
- **Data:** every embedded file exists; `flags.bin` structure, sorted codes, a flag for every country; **rebuilding the flags gives byte-identical output**; the address table is sorted and regenerates `icao_ranges.inc`; the Annex 10 parser on a sample;
  the committed gzipped page equals the page.
- **Web page:** balanced HTML; every `$('id')` the script uses exists; the script passes `node --check`; every `/api/...` the page calls is a route the firmware serves; the page makes no requests to third parties except the map tiles;
  the API key field is a password box and is never pre-filled.
- **Settings storage:** every NVS key fits the 15-character limit; every saved setting is loaded and vice versa; no source line logs the key, a password or a token; the first-boot defaults are the neutral ones.
- **Tools:** the FlightAware / Planespotters stand-in answers with the documented shapes (including a *progressive* JPEG); the soak report passes a good day and fails short runs, restarts, long gaps, low heap, slow frames and leaks (and honours an excused gap);
  the demo radar's state has every key the firmware sends.
- **Privacy tooling:** the scanner finds planted e-mail addresses, MACs, private IPs, home paths, key-like tokens and forbidden files; the public export leaves out private files and passes the scan.
- **Documentation:** every relative link resolves; licence and credits present.

## 3. Device tests (`tests/device_tests.py`)
Run against the real radar over Wi-Fi; they put back what they change and check at the end that the radar did not restart and the heap did not run low.
- **Access control:** wrong PIN refused; cookie `SameSite=Strict`; every protected route (GET and POST) answers 401 without the cookie; a forged or empty cookie is refused; optionally the five-wrong-PINs lock-out.
- **State document:** shape and types; **no secret (PIN, key, passwords) in the answer**; page and assets served complete (Content-Length matches).
- **Settings:** change, read back, restore; absurd values (brightness 100000, range step 999, unit 77, colour 300, ...) are clamped; a FlightAware key is stored without being echoed, and a real round trip to FlightAware with a made-up key
  comes back as a clean *Key rejected*.
- **Hostile input:** invalid JSON and odd bodies, a 4 MB announced body, a 9 KB URL, 300 headers, binary garbage, a negative Content-Length, path-traversal paths, a stalled half-sent request, a request that stops inside its headers, one that stops inside its body and one that trickles a byte a second (for 30 s, while normal requests keep coming and must be answered, and the radar must not restart), 150 sequential requests and 12 parallel connections: the
  radar answers sensibly or drops the connection, and keeps running.
- **Firmware update refusals:** random data, an image header followed by garbage, and bad links (ftp, file, javascript, empty, very long, malformed SHA-256) are refused without touching the slots.

## 4. Soak
`tools/soak.py` polls `/api/state` once a minute for 24 hours. Pass = no restart, no data gap over 3 minutes while online, internal heap low-water mark of at least 25 KB, 20 fps or more in 99 % of the samples, free PSRAM within 600 KB of its start (no leak), never unreachable for more than 5 minutes.

## Done by hand on the hardware
Display behaviour (tearing, jumps) cannot be unit-tested; it was judged by eye and by measurement during development (`docs/hardware-findings.md`): bar test, tear test, a stall test that masks interrupts for 2 ms every 1.5 s, 400 km range with 60+ aircraft,
a slow link (the stand-in server answering over 25 s with `MOCK_SLOW_SECONDS=25`: no restart), captive-portal sign-in on phones, SD card hot-unplug (36 s outage, no rows lost), Wi-Fi outage and recovery, firmware updates by page / link / espota including images that crash at start and 25 s later (automatic rollback), and emergency / night mode rendering on a framebuffer dump.

## Results
Last full run: 2026-10-11, firmware 0.9, radar on Wi-Fi, host and device tests from a Mac.

| Layer | Result |
|---|---|
| 1. Host C++ tests | 113 checks, 0 failed |
| 2. Project tests | 24 tests, all pass |
| 3. Device tests | One run with `RADAR_LOCKOUT=1` on the final firmware: 19 tests, all pass, 1 skipped (the flight-key test skips itself on a radar that already holds a key, because the key cannot be read back and would be lost). The radar did not restart |
| 4. Soak | 41.6 hours, 2,497 samples: 0 restarts, no data gap, internal heap low-water mark 33 KB, free PSRAM flat (5,727 to 5,791 KB), 99.15 % of samples at 20 fps or more. Details in [DEVELOPMENT_LOG.md](DEVELOPMENT_LOG.md), report in `docs/soak/`. **It ran on the firmware before the fixes of the device-test run** (request guard, state request, body reader); see the note below |

Timings from the device tests depend on the Wi-Fi link: 150 sequential state requests plus 12 parallel ones took 13 s on a good link (first run) and 59 s in the last run, when the link was poor (pings of 30 to 170 ms); a stalled client delays others by 3 to 4 s (the guard's limit);
a normal request made while three stalled or trickling connections are open waited up to about 12 s. The assertions check that everything is answered and nothing restarts, with generous limits.

The first device-test run found real faults (a stalled request restarted the radar, a 650 ms state request, a photo that never fitted) and the screenshot session found another (a slow link could restart the radar): see "Publication test run" in the development log. The tests were repeated after each fix until clean.

Known gaps:
- **The final firmware has not been through a 24-hour soak.** The M8 soak ran on the build before the post-soak fixes. They are small and tested (device tests, an hour of London demo traffic with 60 KB answers, a stand-in answering over 25 s), but a new soak is the proper check before tagging a release.
- The FlightAware parser follows the documented v4 field names and has been exercised only against the stand-in server; a real key was not available during development.
- The display and touch driver run on one board revision.
- The device tests need a radar that nobody is using at that moment: they open and close many connections and the lock-out test blocks sign-ins for a minute.
