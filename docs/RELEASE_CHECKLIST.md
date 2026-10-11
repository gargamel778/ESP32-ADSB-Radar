# Release checklist

For whoever publishes the repository (and the order that avoids surprises).

1. **Soak finished and signed off, on the firmware you are releasing** (the M8 soak predates the fixes of the device-test run: start a new one on the release build) (`python3 tools/soak.py report ...` says PASS; the result is in `docs/DEVELOPMENT_LOG.md`).
2. **Flash the release build** (`FW_VERSION` in `firmware/src/config.h`): `cd firmware && pio run -t upload` or an over-the-air update. Check *Settings > System*.
3. **Tests**
   - `tests/run_all.sh` (no hardware) must pass.
   - `RADAR_HOST=... RADAR_PIN=... tests/run_all.sh --device`, and once with `RADAR_LOCKOUT=1`. Do not run them during a soak.
   - The flight-key test skips itself on a radar that already holds a key (it could not be put back). Run it on a radar without one, or accept the gap and say so in `docs/TESTING.md`.
   - Copy the results into `docs/TESTING.md`.
4. **Screenshots** of the radar, with the demo location and the stand-in services: follow `docs/screenshots/README.md` (opening the serial port restarts the radar, so use `radar_serial.py run` for a sequence); then `python3 tools/devtools/web_screens.py` for the page. Put the finished pictures into the README and the user guide.
   Look at every picture for a place name, coordinates, network name, IP address, PIN, access point name or password, e-mail address or key.
5. **Remove test settings from the radar** (FlightAware key and e-mail, demo values) and restore your own.
6. **Privacy**
   - Edit `tools/pii_denylist.txt` (private, not exported): your name, e-mail, board MAC, serial-port number, home coordinates, PIN, access-point name.
   - `python3 tools/export_public.py` makes `../ESP32-ADSB-Radar-public` and runs `tools/privacy_scan.py` on it: it must say *NOTHING FOUND*.
   - Read the export's `docs/` once as a stranger would.
7. **Publish from the export**, not from the working folder: `cd ../ESP32-ADSB-Radar-public && git init`, set the author name and e-mail you want to be public *before* the first commit
   (`git config user.name ...`; `git config user.email ...`, e.g. a GitHub no-reply address), commit, tag the version, push.
8. **After publishing:** keep the private folder (secrets, flash backup, denylist) out of any cloud sync or backup you share.
