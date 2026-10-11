#!/usr/bin/env python3
"""Project tests that need no hardware: data files, the web page, settings storage, the tools, the privacy tooling and the documentation.

    python3 -m unittest discover -s tests -v          (or tests/run_all.sh, which also runs the C++ host tests)

The firmware's pure logic and its embedded data are tested in C++ (tests/host); the radar itself is tested with tests/device_tests.py.
"""
import csv, gzip, html.parser, json, os, re, shutil, struct, subprocess, sys, tempfile, threading, unittest, urllib.error, urllib.request

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
P = lambda *a: os.path.join(ROOT, *a)
def read(*a):
    with open(P(*a), encoding="utf-8") as f:
        return f.read()


class DataFiles(unittest.TestCase):
    def test_every_embedded_file_exists(self):
        ini = read("firmware", "platformio.ini")
        block = re.search(r"embed_files\s*=(.*?)\nbuild_flags", ini, re.S).group(1)
        files = [l.split(";")[0].strip() for l in block.splitlines() if l.strip() and not l.strip().startswith(";")]
        self.assertGreaterEqual(len(files), 8)
        for f in files:
            self.assertGreater(os.path.getsize(P("firmware", f)), 100, f)

    def test_flags_file_structure(self):
        d = open(P("firmware", "data", "flags.bin"), "rb").read()
        self.assertEqual(d[:4], b"FLG1")
        count, w, h = struct.unpack("<HBB", d[4:8])
        self.assertEqual((w, h), (27, 18))
        self.assertEqual(len(d), 8 + count * 2 + count * w * h * 2)
        codes = [d[8 + 2 * i:10 + 2 * i].decode() for i in range(count)]
        self.assertEqual(codes, sorted(set(codes)), "codes must be sorted and unique (the firmware binary-searches them)")
        table = {l.split()[2] for l in read("tools", "icao_ranges.txt").splitlines() if l.strip() and not l.startswith("#")}
        self.assertEqual(table - set(codes), set(), "a country in the address table has no flag")

    def test_flags_rebuild_is_identical(self):
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, "f.bin")
            r = subprocess.run([sys.executable, P("tools", "build_flags.py"), "--out", out], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-500:])
            self.assertEqual(open(out, "rb").read(), open(P("firmware", "data", "flags.bin"), "rb").read())

    def test_icao_table(self):
        rows = [l.split() for l in read("tools", "icao_ranges.txt").splitlines() if l.strip() and not l.startswith("#")]
        prev = -1
        for a, b, iso in rows:
            a, b = int(a, 16), int(b, 16)
            self.assertTrue(a <= b and a > prev, f"{iso}: not sorted / overlapping")
            self.assertRegex(iso, r"^[A-Z]{2}$")
            prev = b
        self.assertGreater(len(rows), 180)
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, "r.inc")
            subprocess.run([sys.executable, P("tools", "build_icao_ranges.py"), "--inc", out], check=True)
            self.assertEqual(open(out).read(), read("firmware", "src", "icao_ranges.inc"), "regenerate firmware/src/icao_ranges.inc")

    def test_icao_parser_on_a_sample(self):
        sample = "Honduras                               *\n                                                   -\n" \
                 "                                                   000010111010-----------\nHungary                                    *       010001110---------------\n"
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(sample)
        try:
            r = subprocess.run([sys.executable, P("tools", "build_icao_ranges.py"), f.name], capture_output=True, text=True)
        finally:
            os.unlink(f.name)
        self.assertIn("470000 477FFF HU", r.stdout)

    def test_web_page_is_the_compressed_one(self):
        self.assertEqual(gzip.decompress(open(P("firmware", "data", "page.html.gz"), "rb").read()), open(P("firmware", "data", "page.html"), "rb").read(),
                         "page.html changed: build once (pio run) so page.html.gz is regenerated")


class WebPage(unittest.TestCase):
    page = read("firmware", "data", "page.html")

    class Ids(html.parser.HTMLParser):
        def __init__(self):
            super().__init__(); self.ids = set(); self.stack = []; self.errors = []
        def handle_starttag(self, tag, attrs):
            for k, v in attrs:
                if k == "id": self.ids.add(v)
            if tag not in ("meta", "link", "br", "hr", "img", "input", "option"): self.stack.append(tag)
        def handle_endtag(self, tag):
            if tag in ("meta", "link", "br", "hr", "img", "input", "option"): return
            if not self.stack or self.stack[-1] != tag: self.errors.append(f"unexpected </{tag}>")
            else: self.stack.pop()

    def test_html_is_balanced(self):
        p = self.Ids(); p.feed(self.page)
        self.assertEqual(p.errors, [])
        self.assertEqual(p.stack, [])

    def test_every_id_the_script_uses_exists(self):
        p = self.Ids(); p.feed(self.page)
        used = set(re.findall(r"\$\('([A-Za-z0-9_]+)'\)", self.page)) | set(re.findall(r"\$\('([a-z]+)'\+i\)", self.page))
        dynamic = {"p"}   # $('p'+i): p0..p4
        missing = {u for u in used if u not in p.ids and u not in dynamic}
        self.assertEqual(missing, set())
        for i in range(5): self.assertIn(f"p{i}", p.ids)

    def test_script_syntax(self):
        scripts = re.findall(r"<script>(.*?)</script>", self.page, re.S)
        self.assertGreaterEqual(len(scripts), 1)
        for js in scripts:
            with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
                f.write(js)
            try:
                r = subprocess.run(["node", "--check", f.name], capture_output=True, text=True)
            except FileNotFoundError:
                self.skipTest("node is not installed")
            finally:
                os.unlink(f.name)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_every_api_call_has_a_route(self):
        routes = set(re.findall(r'server\.on\("(/[^"]*)"', read("firmware", "src", "webui.cpp")))
        used = {m.split("?")[0] for m in re.findall(r"['\"`](/api/[A-Za-z0-9/_]*)", self.page)}
        self.assertEqual({u for u in used if u not in routes}, set(), "the page calls an API route the firmware does not serve")
        self.assertGreater(len(routes), 15)

    def test_no_third_party_requests_except_the_map(self):
        hosts = set(re.findall(r"https?://([A-Za-z0-9.\-{}$]+)", self.page))
        allowed = {"tile.openstreetmap.org", "{s}.tile.openstreetmap.org", "www.openstreetmap.org", "radar.local", "..."}   # "..." is the placeholder of the firmware-link box
        self.assertEqual({h for h in hosts if h not in allowed and not h.endswith("openstreetmap.org")}, set(), "the phone page must not talk to other sites (no analytics, no CDN)")

    def test_key_is_a_password_field_and_never_filled_in(self):
        self.assertRegex(self.page, r'id=fakey type=password')
        self.assertNotIn("fakey').value=", self.page.replace("$('fakey').value=''", ""))


class SettingsStorage(unittest.TestCase):
    src = read("firmware", "src", "settings.cpp")

    def keys(self, verb):
        return set(re.findall(r'gPrefs\.%s\w*\("([a-z0-9]+)"' % verb, self.src)) | set(re.findall(r'loadString\("([a-z0-9]+)"', self.src) if verb == "get" else [])

    def test_nvs_key_names_fit(self):
        for k in self.keys("get") | self.keys("put"):
            self.assertLessEqual(len(k), 15, f"NVS key '{k}' is longer than 15 characters")

    def test_every_saved_setting_is_loaded_and_the_other_way_round(self):
        loaded = self.keys("get")
        saved = self.keys("put")
        # the palette keys are built with snprintf("pal%d")
        self.assertEqual(loaded - saved, set(), "loaded but never saved")
        self.assertEqual(saved - loaded, set(), "saved but never loaded")

    def test_secrets_are_not_printed(self):
        bad = []
        for f in os.listdir(P("firmware", "src")):
            if not f.endswith((".cpp", ".h")) or f == "stb_image.h": continue
            for n, line in enumerate(read("firmware", "src", f).splitlines(), 1):
                if re.search(r"(Serial\.(printf|println|print)|sdlog::sys|snprintf\(.*sys)", line) and re.search(r"faKey|wifiPass|apPass|\.pass\b|gPendPass|x-apikey", line):
                    bad.append(f"{f}:{n}")
        self.assertEqual(bad, [], "a line may be printing a secret")

    def test_neutral_defaults(self):
        cfg = read("firmware", "src", "config.h")
        self.assertIn('#define HOME_LAT 51.4700', cfg)
        self.assertIn('"GMT0BST,M3.5.0/1,M10.5.0"', cfg)
        self.assertIn('#define DEFAULT_COUNTRY "GB"', cfg)
        ex = read("config", "secrets.h.example")
        self.assertNotRegex(ex, r"WIFI_PASS\s+\"(?!your-wifi-password)")


class Tools(unittest.TestCase):
    def test_mock_aeroapi_answers_like_the_firmware_expects(self):
        port = 18100
        proc = subprocess.Popen([sys.executable, P("tools", "mock_aeroapi.py"), str(port)], stdout=subprocess.DEVNULL)
        try:
            base = f"http://127.0.0.1:{port}/aeroapi"
            for _ in range(50):
                try:
                    urllib.request.urlopen(urllib.request.Request(base + "/account/usage", headers={"x-apikey": "TESTKEY"}), timeout=1); break
                except Exception:
                    import time; time.sleep(0.1)
            get = lambda path, key="TESTKEY": json.load(urllib.request.urlopen(urllib.request.Request(base + path, headers={"x-apikey": key}), timeout=5))
            fl = get("/flights/BAW123")["flights"]
            self.assertTrue(any(f["actual_off"] and not f["actual_on"] for f in fl), "one flight must be in the air")
            f = fl[0]
            for k in ("fa_flight_id", "status", "progress_percent", "origin", "destination", "estimated_on", "actual_off", "arrival_delay"): self.assertIn(k, f)
            for e in ("origin", "destination"):
                for k in ("code_iata", "code_icao", "city"): self.assertIn(k, f[e])
            pos = get("/flights/" + f["fa_flight_id"] + "/track")["positions"]
            self.assertGreater(len(pos), 100)
            self.assertTrue(all("latitude" in p and "longitude" in p for p in pos))
            with self.assertRaises(urllib.error.HTTPError) as c: get("/flights/X", key="wrong")
            self.assertEqual(c.exception.code, 401)
            with self.assertRaises(urllib.error.HTTPError) as c: get("/flights/ERR429")
            self.assertEqual(c.exception.code, 429)
            self.assertEqual(get("/flights/NONE")["flights"], [])
            if shutil.which("rsvg-convert") and shutil.which("magick"):   # the Planespotters stand-in: a progressive JPEG (the radar's decoder must handle those)
                ph = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/pub/photos/hex/4CA7B7", timeout=10))["photos"][0]
                self.assertTrue(ph["thumbnail"]["src"].endswith("/img/4ca7b7.jpg") and ph["photographer"])
                jpg = urllib.request.urlopen(ph["thumbnail"]["src"], timeout=10).read()
                self.assertEqual(jpg[:2], b"\xff\xd8")
                self.assertIn(b"\xff\xc2", jpg[:600], "must be a progressive JPEG (SOF2)")
                self.assertEqual(json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/pub/photos/hex/000000", timeout=10)), {"photos": []})
        finally:
            proc.terminate()

    def make_csv(self, rows):
        f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, newline="")
        w = csv.writer(f)
        w.writerow(["time", "ok", "uptime_s", "heap_kb", "heap_min_kb", "psram_kb", "fps", "aircraft", "data_ok", "data_age_s", "rssi", "reset_reason", "vsync", "backlight", "emergencies", "night"])
        for r in rows: w.writerow(r)
        f.close()
        return f.name

    def soak(self, rows, *extra):
        p = self.make_csv(rows)
        try:
            return subprocess.run([sys.executable, P("tools", "soak.py"), "report", p, *extra], capture_output=True, text=True).stdout
        finally:
            os.unlink(p)

    @staticmethod
    def good_rows(hours=24, step_min=10):
        import datetime
        t0 = datetime.datetime(2026, 1, 1, 0, 0)
        return [[(t0 + datetime.timedelta(minutes=i * step_min)).isoformat(), 1, 600 * i, 55, 40, 5700, 28, 10, 1, 3, -50, 7, 1000 * i, 50, 0, 0] for i in range(int(hours * 60 / step_min) + 1)]

    def test_soak_report_pass_and_failures(self):
        self.assertIn("RESULT: PASS", self.soak(self.good_rows()))
        self.assertIn("only 5.0 h", self.soak(self.good_rows(5)))
        rows = self.good_rows(); rows[50][2] = 5            # uptime went backwards: a restart
        self.assertIn("1 restart", self.soak(rows))
        rows = self.good_rows()
        for r in rows[30:40]: r[1] = 0; r[2:] = [""] * 14   # 100 minutes unreachable
        out = self.soak(rows); self.assertIn("unreachable for more than 5 minutes", out)
        self.assertIn("RESULT: PASS", self.soak(rows, "--excuse", "05:00 06:50 monitor fault"))
        rows = self.good_rows(); rows[10][4] = 12            # heap low-water mark of 12 KB
        self.assertIn("heap low-water mark below 25 KB", self.soak(rows))
        rows = self.good_rows()
        for r in rows[:20]: r[6] = 10                       # slow frames
        self.assertIn("frame rate below 20 fps", self.soak(rows))
        rows = self.good_rows()
        for i, r in enumerate(rows): r[5] = 5700 - i * 10   # PSRAM leak
        self.assertIn("PSRAM fell", self.soak(rows))

    def test_privacy_scan_finds_planted_data_and_passes_clean_code(self):
        with tempfile.TemporaryDirectory() as t:
            os.makedirs(os.path.join(t, "docs"))
            clean = os.path.join(t, "docs", "a.md")
            open(clean, "w").write("Contact: you@example.com. The setup access point is http://192.168.4.1. Board MAC xx:xx:xx:xx:xx:xx.\n")
            r = subprocess.run([sys.executable, P("tools", "privacy_scan.py"), t], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            # (the planted values are assembled here so that this file itself stays clean for the scanner)
            open(clean, "a").write("mail me at " + "jane.doe" + "@" + "mailhost.org\nMAC " + "fc:01:2c:" + "11:22:33\nrouter " + "192.168." + "1.10\n/Users/" + "jane/projects/x\nkey " + "0123456789abcdef" + "ABCDEF0123456789abcd\n")
            open(os.path.join(t, "secrets.h"), "w").write("x")
            r = subprocess.run([sys.executable, P("tools", "privacy_scan.py"), t], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            for what in ("e-mail address", "MAC address", "private IPv4", "home-directory path", "key-like token", "must not be published"):
                self.assertIn(what, r.stdout)

    def test_export_leaves_out_private_files_and_is_clean(self):
        with tempfile.TemporaryDirectory() as t:
            target = os.path.join(t, "public")
            r = subprocess.run([sys.executable, P("tools", "export_public.py"), target], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout[-1500:])
            names = {os.path.relpath(os.path.join(d, f), target) for d, _, fs in os.walk(target) for f in fs}
            for private in ("config/secrets.h", "firmware/platformio.local.ini", "tools/pii_denylist.txt"):
                self.assertNotIn(private, names)
            self.assertFalse(any(n.startswith(("backup/", "firmware/.pio", "firmware/managed_components", "reference/viewe-board")) for n in names))
            for needed in ("LICENSE", "README.md", "firmware/platformio.ini", "config/secrets.h.example", "firmware/data/flags.bin", "tools/soak.py"):
                self.assertIn(needed, names)


class MockRadar(unittest.TestCase):
    def test_demo_state_has_every_key_the_firmware_sends(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("mock_radar", P("tools", "mock_radar.py")); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        src = read("firmware", "src", "webui.cpp")
        body = src[src.index("void handleState()"):src.index("void handleSet()")]
        top = set(re.findall(r'\bd\["([a-zA-Z]+)"\]', body))
        self.assertEqual(top - set(m.STATE), set(), "mock_radar.py lacks a top-level key of /api/state")
        for group, var in (("s", "s"), ("fa", "f"), ("health", "h"), ("radar", "r"), ("wifi", "w"), ("fw", "fw"), ("sd", "sd")):
            keys = set(re.findall(r'\b%s\["([a-zA-Z]+)"\]' % var, body))
            self.assertEqual(keys - set(m.STATE[group]), set(), f"mock_radar.py lacks /api/state {group} keys")


class Documentation(unittest.TestCase):
    def test_relative_links_resolve(self):
        broken = []
        for d, _, fs in os.walk(ROOT):
            if any(x in d for x in (".pio", "managed_components", "/.dummy", "third_party", "reference")): continue
            for f in fs:
                if not f.endswith(".md"): continue
                path = os.path.join(d, f)
                for m in re.finditer(r"\]\(([^)#]+)(#[^)]*)?\)", open(path, encoding="utf-8").read()):
                    t = m.group(1)
                    if re.match(r"[a-z]+:", t) or t.startswith("mailto"): continue
                    if not os.path.exists(os.path.normpath(os.path.join(d, t))): broken.append(f"{os.path.relpath(path, ROOT)} -> {t}")
        self.assertEqual(broken, [])

    def test_licence_and_credits(self):
        self.assertIn("GNU GENERAL PUBLIC LICENSE", read("LICENSE")[:200])
        self.assertIn("GPL-3.0", read("README.md"))
        self.assertIn("stb_image", read("docs", "DATA_SOURCES.md"))
        self.assertIn("public domain", read("firmware", "src", "third_party", "stb_image.h").lower())
        self.assertIn("mircemk", read("reference", "original-mircemk", "ATTRIBUTION.md"))

    def test_version_is_consistent(self):
        v = re.search(r'#define FW_VERSION "([^"]+)"', read("firmware", "src", "config.h")).group(1)
        self.assertRegex(v, r"^\d+\.\d+$")


if __name__ == "__main__":
    unittest.main(verbosity=2)
