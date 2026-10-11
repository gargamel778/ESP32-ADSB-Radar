#!/bin/bash
# All tests that need no radar: C++ host tests of the firmware's logic and embedded data, then the Python project tests.
#   tests/run_all.sh                       # everything below
#   tests/run_all.sh --device              # plus tests/device_tests.py against a radar (RADAR_HOST, RADAR_PIN)
# Build the firmware first (cd firmware && pio run) if you want the OTA image check to run against a real image.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
echo "== C++ host tests =="
"$HERE/run_host.sh"
echo; echo "== Python project tests =="
python3 -W ignore -m unittest discover -s "$HERE" -v 2>&1 | tail -45
if [ "$1" = "--device" ]; then
  echo; echo "== device tests (${RADAR_HOST:-radar.local}) =="
  python3 "$HERE/device_tests.py" -v
fi
