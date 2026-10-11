#!/usr/bin/env python3
"""A screenshot of the radar over Wi-Fi, for the documentation. Needs a firmware built with -DDEV_SCREENSHOT (it is not in the normal firmware).

    RADAR_HOST=radar.local RADAR_PIN=123456 python3 tools/devtools/radar_screen.py out.png [out2.png ...]

Signs in with the PIN, downloads /api/screen (raw 800x480 RGB565, 768 KB, a few seconds) and writes a PNG for each file name given. Much faster and cleaner
than the serial dump of radar_serial.py (which takes minutes and can be garbled by the radar's own log lines). The picture is whatever is on the screen at that moment.
"""
import json, os, struct, sys, urllib.request, zlib

HOST = os.environ.get("RADAR_HOST", "radar.local")
PIN = os.environ.get("RADAR_PIN", "")


def write_png(path, w, h, data):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = data[y * w * 2:(y + 1) * w * 2]
        for x in range(w):
            v = row[2 * x] | (row[2 * x + 1] << 8)
            raw += bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def grab():
    r = urllib.request.Request(f"http://{HOST}/api/login", data=json.dumps({"pin": PIN}).encode(), headers={"Content-Type": "application/json"}, method="POST")
    cookie = urllib.request.urlopen(r, timeout=20).headers["Set-Cookie"].split(";")[0]
    r = urllib.request.Request(f"http://{HOST}/api/screen", headers={"Cookie": cookie})
    data = urllib.request.urlopen(r, timeout=120).read()
    if len(data) != 800 * 480 * 2:
        sys.exit(f"unexpected answer of {len(data)} bytes (is this a -DDEV_SCREENSHOT build?)")
    return data


if __name__ == "__main__":
    if not PIN or len(sys.argv) < 2:
        sys.exit(__doc__)
    for out in sys.argv[1:]:
        write_png(out, 800, 480, grab())
        print("wrote", out)
