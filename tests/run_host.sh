#!/bin/bash
# Build and run the host-side C++ tests (no hardware needed). Needs clang++ or g++.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/.."; SRC="$ROOT/firmware/src"
mkdir -p "$HERE/host/build"
CXX="${CXX:-c++}"
$CXX -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I"$HERE/host/shim" -I"$SRC" -DDATA_DIR="\"$ROOT/firmware/data\"" \
  "$HERE/host/test_main.cpp" "$HERE/host/embedded.cpp" "$SRC/geo.cpp" "$SRC/airline.cpp" "$SRC/icao.cpp" "$SRC/mapdata.cpp" \
  -o "$HERE/host/build/host_tests"
"$HERE/host/build/host_tests" "$ROOT" "${1:-$ROOT/firmware/.pio/build/viewe7/firmware.bin}"
