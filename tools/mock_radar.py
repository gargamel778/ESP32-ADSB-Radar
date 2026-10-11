#!/usr/bin/env python3
"""A stand-in for the radar's web server, for working on the phone page and for documentation screenshots: no radar, no network name, no real place.

    python3 tools/mock_radar.py [port] [--login]   # default 8200; open http://localhost:8200  (any PIN; with --login the page starts at the PIN prompt)

It serves firmware/data/page.html and answers the same /api/... calls with demo data (a radar near London, a home network called "HomeNetwork").
Changes made on the page (units, colours, night mode, flight-detail switches, ...) are kept in memory while the script runs.
"""
import gzip, json, os, re, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "data")
SWATCHES = [("Green", "#00ff23"), ("Amber", "#ffb000"), ("Cyan", "#00dcff"), ("White", "#ffffff"), ("Yellow", "#ffff00"), ("Orange", "#ff7800"), ("Magenta", "#ff46dc"), ("Red", "#ff3232")]
PLACES = [
    {"name": "London", "cc": "GB", "country": "United Kingdom", "lat": 51.51, "lon": -0.13, "zone": "Europe/London", "posix": "GMT0BST,M3.5.0/1,M10.5.0"},
    {"name": "Londonderry", "cc": "GB", "country": "United Kingdom", "lat": 55.0, "lon": -7.32, "zone": "Europe/London", "posix": "GMT0BST,M3.5.0/1,M10.5.0"},
    {"name": "Lyon", "cc": "FR", "country": "France", "lat": 45.76, "lon": 4.84, "zone": "Europe/Paris", "posix": "CET-1CEST,M3.5.0,M10.5.0/3"},
]
LOGIN_FIRST = "--login" in sys.argv
logged_in = not LOGIN_FIRST
STATE = {
    "version": "0.9", "fw": {"slot": "app0", "state": "valid", "canRevert": True, "active": False, "pct": 0, "failed": False, "msg": ""}, "uptime": 93784, "heapKb": 55,
    "wifi": {"connected": True, "ssid": "HomeNetwork", "rssi": -52, "ip": "192.0.2.50", "ap": False},
    "radar": {"aircraft": 23, "dataOk": True, "paused": False, "emergencies": 0, "emergency": ""}, "night": False,
    "fa": {"enabled": True, "keySet": True, "email": "you@example.com", "photo": True, "track": True, "status": "BAW123: OK", "test": "Key accepted", "calls": 6, "callsTotal": 18},
    "health": {"heapMinKb": 47, "psramKb": 5792, "resetReason": 3, "backlight": 80, "vsync": 2961234, "fps": 29.4, "dataAgeS": 2},
    "units": {"dist": ["nm", "km", "mi"], "alt": ["ft", "m", "FL"], "spd": ["kt", "km/h", "mph"], "ranges": [10, 25, 50, 100, 150, 200, 250]},
    "swatches": [{"name": n, "hex": h} for n, h in SWATCHES], "themes": ["Sans", "MonoBold", "DejaVu"],
    "s": {"rangeStep": 4, "dist": 0, "alt": 0, "spd": 0, "clock24": True, "unitsAuto": True, "theme": 2, "pal": [0, 0, 4, 3, 0], "brightness": 80, "nightMode": 1, "nightBright": 10,
          "trail": 25, "showDiag": True, "mapOverlay": True, "sdLog": True, "logSecs": 30, "lat": 51.47, "lon": -0.4543, "place": "London", "country": "GB",
          "tz": "GMT0BST,M3.5.0/1,M10.5.0", "tzName": "Europe/London", "countryName": "United Kingdom", "countryAuto": True, "tzAuto": True},
    "sd": {"state": 2, "totalMB": 30436, "freeMB": 30210, "rows": 18452, "bytes": 1534001, "dropped": 0, "file": "2026-10-10.csv"},
}
SET_MAP = {"faEnabled": ("fa", "enabled"), "faPhoto": ("fa", "photo"), "faTrack": ("fa", "track"), "faEmail": ("fa", "email")}


def deep_set(o):
    for k, v in o.items():
        if k in SET_MAP:
            STATE[SET_MAP[k][0]][SET_MAP[k][1]] = v
        elif k == "faKey":
            STATE["fa"]["keySet"] = bool(v)
        elif k == "loc":
            STATE["s"].update(lat=v.get("lat", 0), lon=v.get("lon", 0), place=v.get("place") or "Custom place")
        elif k in ("paused",):
            STATE["radar"]["paused"] = v
        elif k in STATE["s"]:
            STATE["s"][k] = v
    STATE["night"] = STATE["s"]["nightMode"] == 2


class H(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            return {}

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/":
            return self.send(200, open(os.path.join(DATA, "page.html"), "rb").read(), "text/html")
        if u.path in ("/leaflet.js", "/leaflet.css"):
            gz = open(os.path.join(DATA, u.path[1:] + ".gz"), "rb").read()
            return self.send(200, gz, "application/javascript" if u.path.endswith("js") else "text/css", {"Content-Encoding": "gzip"})
        if u.path == "/api/state":
            if not logged_in:
                return self.send(401, {"error": "login"})
            return self.send(200, STATE)
        if u.path == "/api/wifi/scan":
            return self.send(200, {"scanning": False, "nets": [{"ssid": "HomeNetwork", "rssi": -52, "secure": True}, {"ssid": "Cafe Guest", "rssi": -71, "secure": False}, {"ssid": "Neighbour-5G", "rssi": -80, "secure": True}]})
        if u.path == "/api/wifi/status":
            return self.send(200, {"state": "idle", "msg": "", "connected": True, "ssid": "HomeNetwork", "ip": "192.0.2.50", "apSecs": 0})
        if u.path == "/api/ota/status":
            return self.send(200, {"active": False, "pct": 0, "failed": False, "msg": ""})
        if u.path == "/api/geo/search":
            t = (q.get("q") or [""])[0].lower()
            return self.send(200, [p for p in PLACES if t and p["name"].lower().startswith(t)])
        if u.path == "/api/geo/lookup":
            return self.send(200, dict(PLACES[0], distKm=3))
        if u.path == "/api/geo/zones":
            return self.send(200, ["Europe/London", "Europe/Paris", "America/New_York", "Asia/Tokyo"])
        if u.path == "/api/geo/countries":
            return self.send(200, [["FR", "France"], ["GB", "United Kingdom"], ["US", "United States"]])
        return self.send(404, {"error": "not found"})

    def do_POST(self):
        u = urlparse(self.path)
        b = self.body()
        if u.path == "/api/login":
            global logged_in
            logged_in = True
            return self.send(200, {"ok": True}, extra={"Set-Cookie": "sid=demo; Path=/"})
        if u.path == "/api/set":
            deep_set(b)
            return self.send(200, {"ok": True})
        if u.path == "/api/fa/test":
            STATE["fa"]["test"] = "Key accepted"
            return self.send(200, {"ok": True})
        return self.send(200, {"ok": True})

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(next((a for a in sys.argv[1:] if a.isdigit()), 8200))
    print(f"mock radar on http://localhost:{port} (any PIN)", flush=True)
    HTTPServer(("127.0.0.1", port), H).serve_forever()
