#!/usr/bin/env python3
"""Screenshots of the phone/PC page for the documentation, from the demo radar (tools/mock_radar.py): nothing real is on them.

    python3 tools/devtools/web_screens.py [output-folder]        # default docs/screenshots

Needs Google Chrome (or Chromium) for headless screenshots and ImageMagick to cut the long page into pieces.
"""
import glob, os, shutil, subprocess, sys, threading, time
from http.server import HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))


def chrome():
    for c in ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/Applications/Chromium.app/Contents/MacOS/Chromium", shutil.which("google-chrome"), shutil.which("chromium"), shutil.which("chromium-browser")]:
        if c and os.path.exists(c):
            return c
    sys.exit("Chrome or Chromium not found")


def shot(url, out, width=500, height=3300):
    subprocess.run([chrome(), "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1", f"--window-size={width},{height}", "--virtual-time-budget=6000",
                    f"--screenshot={out}", url], capture_output=True, timeout=90)


def main():
    out = os.path.normpath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "screenshots"))
    os.makedirs(out, exist_ok=True)
    import mock_radar
    for name, login in (("web-login", True), ("web", False)):
        mock_radar.logged_in = not login
        srv = HTTPServer(("127.0.0.1", 0), mock_radar.H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        url = f"http://127.0.0.1:{srv.server_port}/"
        full = os.path.join(out, name + "-full.png")
        shot(url, full, 500, 900 if login else 3300)
        srv.shutdown()
        if login:
            subprocess.run(["magick", full, "-crop", "500x330+0+0", "+repage", os.path.join(out, "web-login.png")], check=True)
            os.remove(full)
            continue
        h = int(subprocess.run(["magick", "identify", "-format", "%h", full], capture_output=True, text=True).stdout)
        # cut the long page at section boundaries: status + radar, flight details, SD log + look + units, location
        parts = [(0, 740), (740, 1300), (1300, 2400), (2400, 3050)]
        for i, (a, b) in enumerate(parts, 1):
            subprocess.run(["magick", full, "-crop", f"500x{min(b, h) - a}+0+{a}", "+repage", os.path.join(out, f"web-{i}.png")], check=True)
        os.remove(full)
    print("web screenshots written to", out)


if __name__ == "__main__":
    main()
