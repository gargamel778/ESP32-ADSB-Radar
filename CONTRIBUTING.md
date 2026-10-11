# Contributing

Issues and pull requests are welcome. Before sending a change:

1. `tests/run_all.sh` passes (it needs no hardware). If you touch the web page, build once (`pio run`) so `page.html.gz` is regenerated.
2. If you change something the radar does, say how you checked it on a real board; display changes in particular need a look at the screen at a 400 km range and in the tear test (Settings > System).
3. Keep the polling gentle (airplanes.live every 5 s at most) and keep secrets out of logs and of the repository: `python3 tools/privacy_scan.py` should find nothing.
4. The licence is GPL-3.0-or-later; contributions are accepted under the same licence.

Useful: `docs/ARCHITECTURE.md` (how the pieces fit), `docs/hardware-findings.md` (why the display is set up the way it is), `tools/mock_aeroapi.py` and `tools/mock_radar.py` (work without a key or a radar).
