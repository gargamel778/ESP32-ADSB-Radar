#!/usr/bin/env python3
"""Tests against a running radar, over Wi-Fi (HTTP): sign-in, access control, the state document, settings, malformed and hostile requests, firmware-update refusals.

    RADAR_HOST=radar.local RADAR_PIN=123456 python3 tests/device_tests.py -v
    RADAR_LOCKOUT=1 ...      also tests the five-wrong-PINs lock-out (the radar then refuses sign-ins for a minute)

The tests change nothing permanently (settings they touch are put back) but they do abuse the web server on purpose: do not run them while a
soak test or a long-running measurement is going on. The radar must still be up, with the same uptime counter (no restart), when they are done.
"""
import http.client, json, os, socket, sys, threading, time, unittest, urllib.error, urllib.request

HOST = os.environ.get("RADAR_HOST", "radar.local")
PIN = os.environ.get("RADAR_PIN", "")


class Radar:
    def __init__(self):
        self.cookie = None

    def req(self, method, path, body=None, headers=None, timeout=15, raw=False, cookie=True):
        h = dict(headers or {})
        if cookie and self.cookie:
            h["Cookie"] = self.cookie
        data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
        if data is not None and "Content-Type" not in h:
            h["Content-Type"] = "application/json"
        r = urllib.request.Request(f"http://{HOST}{path}", data=data, headers=h, method=method)
        try:
            resp = urllib.request.urlopen(r, timeout=timeout)
            code, payload, hdrs = resp.status, resp.read(), resp.headers
        except urllib.error.HTTPError as e:
            code, payload, hdrs = e.code, e.read(), e.headers
        if raw:
            return code, payload, hdrs
        try:
            return code, json.loads(payload or b"null")
        except ValueError:
            return code, payload

    def login(self):
        r = urllib.request.Request(f"http://{HOST}/api/login", data=json.dumps({"pin": PIN}).encode(), headers={"Content-Type": "application/json"}, method="POST")
        resp = urllib.request.urlopen(r, timeout=15)
        self.cookie = resp.headers["Set-Cookie"].split(";")[0]
        return resp.headers["Set-Cookie"]

    def state(self):
        c, d = self.req("GET", "/api/state")
        assert c == 200, c
        return d


R = Radar()
START = {}


def setUpModule():
    if not PIN:
        raise unittest.SkipTest("set RADAR_PIN (and RADAR_HOST)")
    R.login()
    START.update(R.state())
    print(f"\nradar {START['version']} up for {START['uptime']} s, heap low-water mark {START['health']['heapMinKb']} KB, slot {START['fw']['slot']}")


def tearDownModule():
    if not PIN:
        return
    time.sleep(2)
    R.login()
    end = R.state()
    print(f"\nafter the tests: up {end['uptime']} s (was {START['uptime']}), heap low-water mark {end['health']['heapMinKb']} KB, free heap {end['heapKb']} KB")
    assert end["uptime"] >= START["uptime"], "the radar restarted during the tests"
    assert end["health"]["heapMinKb"] >= 20, "the heap came too close to empty"


class Access(unittest.TestCase):
    def test_wrong_pin_is_refused_and_cookie_is_scoped(self):
        r = urllib.request.Request(f"http://{HOST}/api/login", data=json.dumps({"pin": "000000" if PIN != "000000" else "111111"}).encode(), headers={"Content-Type": "application/json"}, method="POST")
        with self.assertRaises(urllib.error.HTTPError) as c:
            urllib.request.urlopen(r, timeout=15)
        self.assertEqual(c.exception.code, 403)
        ck = Radar().login()
        self.assertIn("SameSite=Strict", ck)
        self.assertIn("Path=/", ck)

    def test_every_protected_route_needs_the_cookie(self):
        gets = ["/api/state", "/api/wifi/scan", "/api/wifi/status", "/api/ota/status", "/api/geo/zones", "/api/geo/countries", "/api/geo/search?q=lon", "/api/geo/lookup?lat=51&lon=0"]
        posts = ["/api/set", "/api/wifi/join", "/api/restart", "/api/ota/url", "/api/ota/revert", "/api/fa/test"]
        for p in gets:
            c, _ = R.req("GET", p, cookie=False)
            self.assertEqual(c, 401, p)
        for p in posts:
            c, _ = R.req("POST", p, body={}, cookie=False)
            self.assertEqual(c, 401, p)
        # the firmware upload is checked before the first byte is written
        part = b'--zz\r\nContent-Disposition: form-data; name="file"; filename="firmware.bin"\r\nContent-Type: application/octet-stream\r\n\r\n' + b"x" * 2000 + b"\r\n--zz--\r\n"
        c, _ = R.req("POST", "/api/ota/upload", body=part, headers={"Content-Type": "multipart/form-data; boundary=zz"}, cookie=False)
        self.assertEqual(c, 401)
        try:   # a body that is not multipart at all: refused, or the connection is simply closed
            c, _ = R.req("POST", "/api/ota/upload", body=b"x" * 2000, headers={"Content-Type": "multipart/form-data; boundary=zz"}, cookie=False)
            self.assertIn(c, (400, 401))
        except (http.client.HTTPException, ConnectionError, urllib.error.URLError):
            pass
        self.assertEqual(R.req("POST", "/api/set", body={"brightness": 10}, cookie=False)[0], 401)

    def test_a_forged_cookie_is_not_enough(self):
        for ck in ("sid=0000000000000000", "sid=", "sid=' OR 1=1", "x=1"):
            self.assertEqual(R.req("GET", "/api/state", headers={"Cookie": ck}, cookie=False)[0], 401, ck)

    @unittest.skipUnless(os.environ.get("RADAR_LOCKOUT"), "set RADAR_LOCKOUT=1 (locks sign-ins for a minute)")
    def test_lockout_after_five_wrong_pins(self):
        bad = "000000" if PIN != "000000" else "111111"
        codes = []
        for _ in range(7):
            r = urllib.request.Request(f"http://{HOST}/api/login", data=json.dumps({"pin": bad}).encode(), headers={"Content-Type": "application/json"}, method="POST")
            try:
                urllib.request.urlopen(r, timeout=15)
            except urllib.error.HTTPError as e:
                codes.append(e.code)
        self.assertIn(429, codes)
        with self.assertRaises(urllib.error.HTTPError) as c:   # even the right PIN waits
            Radar().login()
        self.assertEqual(c.exception.code, 429)
        time.sleep(62)
        R.login()


class State(unittest.TestCase):
    def test_document_shape(self):
        d = R.state()
        for k in ("version", "fw", "uptime", "heapKb", "wifi", "radar", "night", "fa", "health", "units", "swatches", "themes", "s", "sd"):
            self.assertIn(k, d)
        self.assertRegex(d["version"], r"^\d+\.\d+")
        self.assertIn(d["fw"]["slot"], ("app0", "app1"))
        self.assertTrue(d["wifi"]["connected"])
        self.assertEqual(len(d["s"]["pal"]), 5)
        self.assertEqual(len(d["swatches"]), 8)
        self.assertIsInstance(d["radar"]["aircraft"], int)
        self.assertGreater(d["health"]["fps"], 5)

    def test_no_secret_is_ever_sent_back(self):
        d = json.dumps(R.state())
        self.assertNotIn(PIN, d, "the web PIN must not be in the state")
        for word in ("pass", "apPass", "faKey", "x-apikey", "wifiPass"):
            self.assertNotIn(f'"{word}"', d)

    def test_page_and_assets(self):
        c, body, h = R.req("GET", "/", raw=True, cookie=False)
        self.assertEqual(c, 200)
        self.assertEqual(h["Content-Encoding"], "gzip")
        self.assertEqual(int(h["Content-Length"]), len(body))
        import gzip
        self.assertIn(b"<title>", gzip.decompress(body))
        for p in ("/leaflet.js", "/leaflet.css"):
            c, body, h = R.req("GET", p, raw=True, cookie=False)
            self.assertEqual((c, int(h["Content-Length"]) == len(body)), (200, True), p)


class Settings(unittest.TestCase):
    def test_round_trip_and_restore(self):
        s0 = R.state()["s"]
        try:
            R.req("POST", "/api/set", {"trail": 40, "showDiag": not s0["showDiag"], "brightness": 60})
            time.sleep(1)
            s1 = R.state()["s"]
            self.assertEqual((s1["trail"], s1["showDiag"], s1["brightness"]), (40, not s0["showDiag"], 60))
        finally:
            R.req("POST", "/api/set", {"trail": s0["trail"], "showDiag": s0["showDiag"], "brightness": s0["brightness"]})
        time.sleep(1)
        s2 = R.state()["s"]
        self.assertEqual((s2["trail"], s2["showDiag"], s2["brightness"]), (s0["trail"], s0["showDiag"], s0["brightness"]))

    def test_out_of_range_values_are_clamped_not_trusted(self):
        s0 = R.state()["s"]
        try:
            R.req("POST", "/api/set", {"brightness": 100000, "nightBright": -50, "rangeStep": 999, "dist": 77, "pal": [99, -4, 300, 8, 1000], "theme": 12345})
            time.sleep(1)
            s = R.state()["s"]
            self.assertTrue(10 <= s["brightness"] <= 100 and 2 <= s["nightBright"] <= 100 and 0 <= s["rangeStep"] <= 6)
            self.assertTrue(all(0 <= p < 8 for p in s["pal"]))
            self.assertTrue(0 <= s["dist"] <= 2 and 0 <= s["theme"] <= 2)
        finally:
            R.req("POST", "/api/set", {"brightness": s0["brightness"], "nightBright": s0["nightBright"], "rangeStep": s0["rangeStep"], "dist": s0["dist"], "pal": s0["pal"], "theme": s0["theme"]})

    def test_flight_key_is_stored_without_being_echoed(self):
        fa0 = R.state()["fa"]
        if fa0["keySet"]:
            self.skipTest("this radar holds a flight key (it cannot be read back, so the test would destroy it); use a radar without one")
        secret = "ZXCVBNMASDFGHJKLQWERTYUIOP123456"
        try:
            R.req("POST", "/api/set", {"faKey": secret})
            time.sleep(1)
            text = json.dumps(R.state())
            self.assertNotIn(secret, text)
            self.assertTrue(R.state()["fa"]["keySet"])
            # a real round trip to FlightAware with a made-up key: the answer must be a clean refusal (TLS and the error text work)
            R.req("POST", "/api/fa/test", {})
            for _ in range(12):
                time.sleep(1.5)
                t = R.state()["fa"]["test"]
                if t and t != "Testing...":
                    break
            self.assertRegex(t, r"rejected|401|Not allowed|403", t)
        finally:
            R.req("POST", "/api/set", {"faKey": ""})
        time.sleep(1)
        self.assertFalse(R.state()["fa"]["keySet"] and not fa0["keySet"])


class Hostile(unittest.TestCase):
    def raw(self, payload, wait=1.0, read=True):
        s = socket.create_connection((HOST, 80), timeout=10)
        try:
            s.sendall(payload)
            time.sleep(wait)
            return s.recv(4096) if read else b""
        except (socket.timeout, ConnectionResetError, BrokenPipeError):
            return b""
        finally:
            s.close()

    def alive(self):
        for _ in range(20):
            try:
                R.login(); R.state(); return True
            except Exception:
                time.sleep(1)
        return False

    def test_bad_json_and_bodies(self):
        self.assertEqual(R.req("POST", "/api/set", body=b"{not json", headers={"Content-Type": "application/json"})[0], 400)
        self.assertIn(R.req("POST", "/api/set", body=b"")[0], (200, 400))
        self.assertIn(R.req("POST", "/api/set", body=b"[1,2,3]")[0], (200, 400))
        self.assertIn(R.req("POST", "/api/set", body=b'{"loc":"nope","pal":"x","brightness":"x","trail":null}')[0], (200, 400))
        self.assertTrue(self.alive())

    def test_huge_and_odd_requests(self):
        self.raw(b"POST /api/set HTTP/1.1\r\nHost: x\r\nContent-Length: 4000000\r\n\r\n" + b"A" * 20000)
        self.assertTrue(self.alive())
        self.raw(b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertTrue(self.alive())
        self.raw(b"GET / HTTP/1.1\r\n" + b"".join(b"X-%d: y\r\n" % i for i in range(300)) + b"\r\n")
        self.assertTrue(self.alive())
        self.raw(b"\x00\x01\x02garbage\r\n\r\n")
        self.assertTrue(self.alive())
        self.raw(b"POST /api/set HTTP/1.1\r\nContent-Length: -5\r\n\r\n")
        self.assertTrue(self.alive())

    def test_paths_that_must_not_exist(self):
        for p in ("/../../etc/passwd", "/%2e%2e/%2e%2e/etc/passwd", "/api/../api/state", "/api/state/../../x", "/secrets.h", "/config/secrets.h", "/.git/config", "/api/unknown", "//api//state"):
            c = self.raw(f"GET {p} HTTP/1.0\r\nHost: x\r\n\r\n".encode())
            self.assertTrue(c.startswith(b"HTTP/1.") and (b" 404 " in c[:20] or b" 401 " in c[:20] or b" 400 " in c[:20]), (p, c[:40]))
        self.assertTrue(self.alive())

    def test_slow_client_does_not_wedge_the_server(self):
        s = socket.create_connection((HOST, 80), timeout=10)
        s.sendall(b"GET /api/state HTTP/1.1\r\nHost: x\r\nCookie: ")   # never finished
        t0 = time.time()
        ok = self.alive()
        s.close()
        self.assertTrue(ok)
        print(f"\n  a stalled client delayed other requests by {time.time() - t0:.1f} s")
        self.assertLess(time.time() - t0, 25)

    def test_trickling_and_half_sent_requests_do_not_restart_the_radar(self):
        up0 = R.state()["uptime"]
        socks = []
        # a request that stops in the middle of its headers, one that stops in the middle of its body, and one that trickles a byte at a time
        s1 = socket.create_connection((HOST, 80), timeout=10); s1.sendall(b"GET /api/state HTTP/1.1\r\nHost: x\r\nCook"); socks.append(s1)
        s2 = socket.create_connection((HOST, 80), timeout=10); s2.sendall(b"POST /api/set HTTP/1.1\r\nHost: x\r\nContent-Length: 40\r\n\r\n{\"bright"); socks.append(s2)
        s3 = socket.create_connection((HOST, 80), timeout=10); socks.append(s3)
        t0 = time.time()
        worst = 0.0
        for i in range(30):   # 30 s of trickling, while a normal client keeps asking
            try:
                s3.sendall(b"G" if i == 0 else b"E")
            except OSError:
                pass
            t1 = time.time()
            self.assertTrue(self.alive(), f"no answer after {time.time() - t0:.0f} s")
            worst = max(worst, time.time() - t1)
            time.sleep(1)
        for s in socks:
            s.close()
        up1 = R.state()["uptime"]
        print(f"\n  normal requests during 30 s of half-sent and trickled requests: slowest answer {worst:.1f} s")
        self.assertGreaterEqual(up1, up0 + 25, "the radar restarted")
        self.assertLess(worst, 30)   # a normal client may have to wait behind the three stalled ones (3 s each), longer on a poor Wi-Fi link; the point is that it is answered and nothing restarts

    def test_many_requests_and_connections(self):
        t0 = time.time()
        bad = 0
        for _ in range(150):
            try:
                R.state()
            except Exception:
                bad += 1
        self.assertLessEqual(bad, 3)
        results = []
        def one():
            try:
                results.append(Radar().req("GET", "/api/state", cookie=False)[0])
            except Exception as e:
                results.append(str(e))
        threads = [threading.Thread(target=one) for _ in range(12)]
        [t.start() for t in threads]; [t.join() for t in threads]
        self.assertTrue(sum(r == 401 for r in results) >= 6, results)   # a burst may be turned away, but those answered must be right
        self.assertTrue(self.alive())
        print(f"\n  150 sequential state requests + 12 parallel in {time.time() - t0:.1f} s, {bad} failed")


class FirmwareUpdateRefusals(unittest.TestCase):
    def upload(self, blob, fields=None):
        import io, uuid
        b = uuid.uuid4().hex
        parts = [f'--{b}\r\nContent-Disposition: form-data; name="size"\r\n\r\n{len(blob)}\r\n'.encode()]
        for k, v in (fields or {}).items():
            parts.append(f'--{b}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode())
        parts.append(f'--{b}\r\nContent-Disposition: form-data; name="file"; filename="firmware.bin"\r\nContent-Type: application/octet-stream\r\n\r\n'.encode() + blob + f"\r\n--{b}--\r\n".encode())
        return R.req("POST", "/api/ota/upload", body=b"".join(parts), headers={"Content-Type": f"multipart/form-data; boundary={b}"}, timeout=60)

    def test_a_file_that_is_not_firmware_is_refused_before_anything_is_written(self):
        slot = R.state()["fw"]
        code, d = self.upload(os.urandom(120000))
        self.assertEqual(code, 400)
        self.assertIn("firmware", d["error"].lower())
        after = R.state()["fw"]
        self.assertEqual((after["slot"], after["state"], after["canRevert"]), (slot["slot"], slot["state"], slot["canRevert"]), "a refused file must not touch the slots")

    def test_an_image_header_with_garbage_behind_it_is_refused(self):
        blob = bytes([0xE9, 4, 2, 0x2F]) + os.urandom(8) + b"\x09\x00" + os.urandom(10) + os.urandom(100000)
        code, d = self.upload(blob)
        self.assertEqual(code, 400)
        self.assertTrue(R.state()["fw"]["state"] in ("valid", "pending", "installed"))

    def test_bad_links_are_refused(self):
        for url in ("", "ftp://x/y.bin", "file:///etc/passwd", "http://", "x" * 400, "javascript:alert(1)"):
            c, d = R.req("POST", "/api/ota/url", {"url": url})
            self.assertEqual(c, 400, url[:30])
        c, d = R.req("POST", "/api/ota/url", {"url": "http://example.com/firmware.bin", "sha256": "abc"})
        self.assertEqual(c, 400)


if __name__ == "__main__":
    unittest.main(verbosity=2)
