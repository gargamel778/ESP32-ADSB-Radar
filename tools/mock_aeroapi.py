#!/usr/bin/env python3
"""A stand-in for FlightAware AeroAPI v4 (the few calls the radar makes), for testing without a key or spending money.

    python3 tools/mock_aeroapi.py [port]      # default 8100; key "TESTKEY"
Build a test firmware that talks to it:
    PLATFORMIO_BUILD_FLAGS='-DFA_BASE=\\"http://<this computer's address>:8100/aeroapi\\"' pio run -e viewe7
It also stands in for Planespotters (/pub/photos/hex/<hex> and /img/<hex>.jpg) with an original, drawn picture (a progressive JPEG like the real
ones), so screenshots and tests need no third-party photographs. Needs rsvg-convert and ImageMagick for the picture (without them: "no photo").
Idents: anything gives an in-flight London -> Frankfurt flight; ERR429 answers 429; NONE finds no flight; REG-xxx etc. are treated alike.
The response shapes follow FlightAware's AeroAPI v4 documentation (flights[], track positions[]); real answers may carry more fields.
"""
import json, math, os, subprocess, sys, time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse

KEY = "TESTKEY"
SLOW = float(os.environ.get("MOCK_SLOW_SECONDS", "0"))   # >0: every answer body trickles out over about this many seconds (a slow link: the radar must not hang or restart)
LHR, FRA = (51.4700, -0.4543), (50.0333, 8.5706)   # the stand-in flight goes London Heathrow -> Frankfurt


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def flight(ident, now):
    off = now - timedelta(minutes=70)
    return {
        "ident": ident, "ident_icao": ident, "ident_iata": None,
        "fa_flight_id": f"{ident}-{int(now.timestamp()) - 4200}-schedule-0001",
        "operator": "BAW", "registration": "G-ZZZA", "aircraft_type": "B789",
        "status": "En Route / Delayed", "progress_percent": 55, "cancelled": False, "diverted": False,
        "origin": {"code": "EGLL", "code_icao": "EGLL", "code_iata": "LHR", "name": "London Heathrow", "city": "London", "timezone": "Europe/London"},
        "destination": {"code": "EDDF", "code_icao": "EDDF", "code_iata": "FRA", "name": "Frankfurt Main", "city": "Frankfurt", "timezone": "Europe/Berlin"},
        "scheduled_out": iso(off - timedelta(minutes=25)), "estimated_out": iso(off - timedelta(minutes=12)), "actual_out": iso(off - timedelta(minutes=12)),
        "scheduled_off": iso(off - timedelta(minutes=15)), "estimated_off": iso(off), "actual_off": iso(off),
        "scheduled_on": iso(now + timedelta(minutes=25)), "estimated_on": iso(now + timedelta(minutes=38)), "actual_on": None,
        "scheduled_in": iso(now + timedelta(minutes=32)), "estimated_in": iso(now + timedelta(minutes=45)), "actual_in": None,
        "departure_delay": 780, "arrival_delay": 780,
    }


def old_flight(ident, now):
    f = flight(ident, now - timedelta(days=1))
    f.update({"status": "Arrived / Gate Arrival", "progress_percent": 100, "actual_on": f["estimated_on"], "actual_in": f["estimated_in"]})
    f["fa_flight_id"] += "-old"
    return f


def track(now, n=600):
    pts = []
    for i in range(n):
        t = i / (n - 1) * 0.55   # 55 % of the way
        lat = LHR[0] + (FRA[0] - LHR[0]) * t + 1.2 * math.sin(t * math.pi) * 0.4
        lon = LHR[1] + (FRA[1] - LHR[1]) * t
        pts.append({"fa_flight_id": "x", "altitude": min(380, int(i * 3)), "altitude_change": "C", "groundspeed": 450, "heading": 290,
                    "latitude": round(lat, 4), "longitude": round(lon, 4), "timestamp": iso(now - timedelta(minutes=70 - i * 70 / n)), "update_type": "A"})
    return pts


PHOTO_SVG = """<svg xmlns="http://www.w3.org/2000/svg" width="200" height="133" viewBox="0 0 200 133">
<defs><linearGradient id="s" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2f6fb5"/><stop offset="1" stop-color="#bcdcf5"/></linearGradient></defs>
<rect width="200" height="133" fill="url(#s)"/><ellipse cx="50" cy="30" rx="26" ry="7" fill="#fff" opacity=".85"/><ellipse cx="150" cy="22" rx="34" ry="8" fill="#fff" opacity=".8"/>
<g transform="rotate(-9 100 70)"><path d="M28,70 Q30,58 60,56 L150,56 Q176,58 184,66 L184,74 Q170,80 150,80 L60,80 Q30,82 28,70 Z" fill="#f4f6f8" stroke="#8a97a6" stroke-width="1.2"/>
<path d="M150,56 L172,28 L182,28 L176,58 Z" fill="#1d4f91"/><path d="M80,66 L128,66 L96,100 L84,100 Z" fill="#dfe5ea" stroke="#8a97a6" stroke-width="1"/>
<ellipse cx="108" cy="82" rx="12" ry="5.5" fill="#9aa5b1"/><rect x="62" y="63" width="86" height="3" fill="#1d4f91"/>
<circle cx="168" cy="64" r="1.6" fill="#456"/><circle cx="160" cy="64" r="1.6" fill="#456"/><circle cx="152" cy="64" r="1.6" fill="#456"/><circle cx="44" cy="66" r="2.4" fill="#456"/></g></svg>"""
_photo = None

def photo():
    global _photo
    if _photo is None:
        try:
            png = subprocess.run(["rsvg-convert", "-f", "png"], input=PHOTO_SVG.encode(), capture_output=True, check=True).stdout
            _photo = subprocess.run(["magick", "png:-", "-interlace", "Plane", "-quality", "85", "jpg:-"], input=png, capture_output=True, check=True).stdout
        except Exception:
            _photo = b""
    return _photo


class H(BaseHTTPRequestHandler):
    def out(self, body):
        if SLOW <= 0 or len(body) < 8:
            self.wfile.write(body)
            return
        step = max(1, len(body) // int(SLOW * 4))
        for i in range(0, len(body), step):
            self.wfile.write(body[i:i + step]); self.wfile.flush(); time.sleep(0.25)

    def send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=UTF-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.out(body)

    def do_GET(self):
        u = urlparse(self.path)
        parts = [p for p in u.path.split("/") if p]
        if parts[:3] == ["pub", "photos", "hex"]:   # Planespotters stand-in: no key needed
            host = self.headers.get("Host", "localhost")
            if not photo() or parts[3].upper() == "000000":
                return self.send(200, {"photos": []})
            return self.send(200, {"photos": [{"id": "1", "thumbnail": {"src": f"http://{host}/img/{parts[3].lower()}.jpg", "size": {"width": 200, "height": 133}},
                                               "photographer": "Example Photographer", "link": "http://localhost/"}]})
        if parts[:1] == ["img"]:
            body = photo()
            self.send_response(200); self.send_header("Content-Type", "image/jpeg"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.out(body)
            return
        print(f"{self.client_address[0]} GET {self.path}  key={'ok' if self.headers.get('x-apikey') == KEY else self.headers.get('x-apikey')}", flush=True)
        if self.headers.get("x-apikey") != KEY:
            return self.send(401, {"title": "Unauthorized", "reason": "authentication_failed", "detail": "Invalid API key", "status": 401})
        now = datetime.now(timezone.utc)
        if parts[:2] != ["aeroapi", "flights"] and parts[:3] != ["aeroapi", "account", "usage"]:
            return self.send(404, {"title": "Not found", "status": 404})
        if parts[1] == "account":
            return self.send(200, {"total_calls": 12, "total_cost": 0.06})
        ident = parts[2] if len(parts) > 2 else ""
        if len(parts) == 4 and parts[3] == "track":
            return self.send(200, {"actual_distance": 700, "positions": track(now)})
        if ident == "ERR429":
            return self.send(429, {"title": "Too many requests", "reason": "rate_limited", "detail": "Rate limit exceeded", "status": 429})
        if ident == "NONE":
            return self.send(200, {"flights": [], "links": None, "num_pages": 1})
        return self.send(200, {"flights": [old_flight(ident, now), flight(ident, now)][::-1] if False else [flight(ident, now), old_flight(ident, now)], "links": None, "num_pages": 1})

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8100
    print(f"mock AeroAPI on :{port}, key {KEY}", flush=True)
    HTTPServer(("0.0.0.0", port), H).serve_forever()
