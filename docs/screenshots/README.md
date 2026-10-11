# Screenshots

None of these show a real place or network. The on-radar pictures are frame-buffer dumps of the real screen taken with the radar set to a **demo location** (central London; live traffic), the web-page pictures come from the **demo server** (`tools/mock_radar.py`).
In the pictures of the aircraft card with flight details, the route, the times and the photo come from a **stand-in** for the FlightAware and Planespotters services (`tools/mock_aeroapi.py`): the flight details are made up and the "photo" is an original drawing,
so that no key, no real look-up and no third-party photograph is involved. Everything else on the radar screen is real live data.

| File | What |
|---|---|
| `radar-overview.png` | The radar screen with the map overlay |
| `radar-card.png` | Aircraft card: flag and country, airline, route, take-off and ETA, progress, photo (stand-in) |
| `radar-card-nophoto.png` | The same aircraft table row without FlightAware: what you get by default |
| `radar-emergency.png` | A 7700 squawk: red mark and rings, banner, red table row |
| `radar-night.png` | Night mode |
| `settings-radar.png`, `settings-units.png`, `settings-look.png`, `settings-location.png`, `settings-flights.png`, `settings-log.png`, `settings-system.png` | The settings tabs (not *Wi-Fi*: it shows the network name and address) |
| `keyboard-key.png` | The on-screen keyboard for the API key (empty) |
| `web-login.png`, `web-1.png` … `web-4.png` | The phone / PC page: sign-in, status and radar settings, flight details, log / look / units, location. `python3 tools/devtools/web_screens.py` (headless Chrome and the demo server) |

## Taking the on-radar pictures again
1. Run the stand-in: `python3 tools/mock_aeroapi.py` (port 8100, key `TESTKEY`; needs `rsvg-convert` and ImageMagick for the drawn photo).
2. Build and install a **test firmware** that talks to it. Use your computer's address on the radar's network:
   ```
   cd firmware
   PLATFORMIO_BUILD_FLAGS='-DDEV_SCREENSHOT -DFA_BASE=\"http://<computer>:8100/aeroapi\" -DPS_BASE=\"http://<computer>:8100\" -DFA_KEY_OVERRIDE=\"TESTKEY\"' pio run -e viewe7 -t upload
   ```
   `FA_KEY_OVERRIDE` makes the test firmware send `TESTKEY` to the stand-in whatever key is stored, so a real key on the radar is never sent to it. Over Wi-Fi the install works too, but a poor link may stall it: USB is the safe way.
3. In Settings > Flights Data (or on the page) switch FlightAware on, enter any key and the e-mail `you@example.com`; note your own settings first, they are restored in step 6.
4. Run the script: `python3 tools/devtools/radar_serial.py run docs/screenshots/shots.script`. It opens the serial port **once** (opening the port restarts the radar on a Mac, so separate commands would lose the demo place), puts the radar over London with `Y` (nothing is saved while it is active), taps through the screens and takes the pictures.
   Each picture takes about 90 seconds because the screen is sent over the serial console and the radar is busy meanwhile; no soak test should be running. Faster: `radar_screen.py` (needs `-DDEV_SCREENSHOT`, which the command above has, and a good Wi-Fi link) downloads the whole screen in a few seconds.
5. Look at **every** picture: a place name, coordinates, network name, IP address, PIN, access-point name or password, e-mail address or key must not be visible (`tools/privacy_scan.py` only reads text). The script ends with a plain restart, which forgets the demo place.
6. Put your settings back (key, e-mail, colours, units), install the normal firmware (`pio run -e viewe7 -t upload` without the flags), and run `python3 tools/privacy_scan.py`.

The tap positions in the script are for the 800×480 screen: the gear is at 440,440; the tabs are at x=90, y=90 + 47 per tab; *Done* / *Cancel* are at 726,30.
