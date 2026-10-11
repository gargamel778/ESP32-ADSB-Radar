#!/usr/bin/env python3
"""Soak test for the ADS-B radar: poll its web API once a minute, then judge the log.

    RADAR_PIN=123456 python3 tools/soak.py poll   --host radar.local --out soak.csv     # leave running for 24 h (Ctrl-C to stop)
    python3 tools/soak.py report soak.csv                                                # pass/fail summary, any time

The PIN is the one under Settings > Wi-Fi on the radar (read from the environment, never stored).
Pass criteria (24 h): no restarts, no stretch without data longer than 3 minutes while the network was up, internal heap low-water mark >= 25 KB,
frame rate >= 20 fps in at least 99 % of the samples, free PSRAM within 600 KB of its starting value (no leak), and the radar never unreachable for more than 5 minutes.
"""
import argparse, csv, http.cookiejar, json, os, sys, time, urllib.request
from datetime import datetime

COLS = ["time", "ok", "uptime_s", "heap_kb", "heap_min_kb", "psram_kb", "fps", "aircraft", "data_ok", "data_age_s", "rssi", "reset_reason", "vsync", "backlight", "emergencies", "night"]


def poll(args):
    pin = os.environ.get("RADAR_PIN")
    if not pin:
        sys.exit("set RADAR_PIN to the radar's web PIN")
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    base = f"http://{args.host}"

    def login():
        req = urllib.request.Request(base + "/api/login", json.dumps({"pin": pin}).encode(), {"Content-Type": "application/json"})
        op.open(req, timeout=10).read()

    new = not os.path.exists(args.out)
    fails = 0
    with open(args.out, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(COLS)
        while True:
            t0 = time.time()
            row = [datetime.now().isoformat(timespec="seconds"), 0] + [""] * (len(COLS) - 2)
            try:
                try:
                    d = json.load(op.open(base + "/api/state", timeout=10))
                except Exception:
                    login()
                    d = json.load(op.open(base + "/api/state", timeout=10))
                h, r, wf = d["health"], d["radar"], d["wifi"]
                row = [row[0], 1, d["uptime"], d["heapKb"], h["heapMinKb"], h["psramKb"], round(h["fps"], 1), r["aircraft"], int(r["dataOk"]), h["dataAgeS"], wf["rssi"],
                       h["resetReason"], h["vsync"], h.get("backlight", ""), r["emergencies"], int(d["night"])]
                fails = 0
            except Exception as e:
                print("poll failed:", e, file=sys.stderr)
                fails += 1
            w.writerow(row)
            f.flush()
            if fails >= 3:   # a long-running poller once lost its route to the radar (on 2026-10-10) while a fresh process reached it: start afresh
                f.close()
                print("restarting the poller process", file=sys.stderr, flush=True)
                os.execv(sys.executable, [sys.executable] + sys.argv)
            time.sleep(max(1, args.interval - (time.time() - t0)))


def report(args):
    rows = list(csv.DictReader(open(args.file)))
    if not rows:
        sys.exit("empty log")
    ts = [datetime.fromisoformat(r["time"]) for r in rows]
    hours = (ts[-1] - ts[0]).total_seconds() / 3600
    good = [r for r in rows if r["ok"] == "1"]
    problems = []
    print(f"samples: {len(rows)} over {hours:.1f} h ({len(good)} answered)")

    # unreachable stretches (--excuse "HH:MM HH:MM reason": a stretch that was the monitor's fault, shown but not counted)
    excused = []
    for e in args.excuse or []:
        a, b, why = e.split(" ", 2)
        excused.append((a, b, why))
    worst, start = 0, None
    for r, t in zip(rows, ts):
        hhmm = t.strftime("%H:%M")
        ex = next((x for x in excused if x[0] <= hhmm <= x[1]), None)
        if ex:
            start = None
            continue
        if r["ok"] != "1":
            start = start or t
            worst = max(worst, (t - start).total_seconds())
        else:
            start = None
    if start:
        worst = max(worst, (ts[-1] - start).total_seconds())
    for a, b, why in excused:
        print(f"excused monitoring gap {a}-{b}: {why}")
    print(f"longest unreachable stretch: {worst / 60:.1f} min")
    if worst > 300:
        problems.append("radar unreachable for more than 5 minutes")

    # restarts: uptime going backwards
    restarts = []
    prev = None
    for r in good:
        up = int(r["uptime_s"])
        if prev is not None and up < prev - 5:
            restarts.append((r["time"], r["reset_reason"]))
        prev = up
    print(f"restarts: {len(restarts)}" + "".join(f"\n   {t} (reset reason {rr}: 3=software, 4=panic, 5/6/7=watchdog, 9=brownout)" for t, rr in restarts))
    if restarts:
        problems.append(f"{len(restarts)} restart(s)")

    def vals(k, f=float):
        return [f(r[k]) for r in good if r[k] != ""]
    heapmin = min(vals("heap_min_kb"))
    fps = vals("fps")
    psram = vals("psram_kb")
    ages = [(int(r["data_age_s"]), r) for r in good if r["data_age_s"] != ""]
    rssi = vals("rssi", int)
    print(f"internal heap low-water mark: {heapmin:.0f} KB (free now {vals('heap_kb')[-1]:.0f} KB)")
    slow = sum(1 for f in fps if f < 20)
    print(f"frame rate: min {min(fps):.1f}, median {sorted(fps)[len(fps)//2]:.1f}; {slow} of {len(fps)} samples below 20 fps")
    print(f"free PSRAM: start {psram[0]:.0f} KB, min {min(psram):.0f} KB, now {psram[-1]:.0f} KB")
    print(f"Wi-Fi signal: {min(rssi)}..{max(rssi)} dBm")
    stale = [a for a, r in ages if a > 180 and r["rssi"] not in ("", "0")]
    print(f"samples with data older than 3 min: {len(stale)} (max age {max(a for a, _ in ages)} s)")
    em = sum(1 for r in good if r["emergencies"] not in ("", "0"))
    print(f"samples with an emergency squawk on screen: {em}")
    if heapmin < 25:
        problems.append("heap low-water mark below 25 KB")
    if slow > max(1, len(fps) // 100):
        problems.append("frame rate below 20 fps in more than 1 % of the samples")
    if psram[0] - min(psram) > 600:
        problems.append("free PSRAM fell by more than 600 KB")
    if len(stale) > 3:
        problems.append("data was older than 3 minutes at several samples")
    if hours < 23.5:
        problems.append(f"only {hours:.1f} h of data (24 h needed)")
    print("\nRESULT:", "PASS" if not problems else "NOT YET / FAIL: " + "; ".join(problems))


ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
sub = ap.add_subparsers(dest="cmd", required=True)
p = sub.add_parser("poll")
p.add_argument("--host", default="radar.local")
p.add_argument("--out", default="soak.csv")
p.add_argument("--interval", type=int, default=60)
p.set_defaults(fn=poll)
r = sub.add_parser("report")
r.add_argument("file")
r.add_argument("--excuse", action="append", metavar='"HH:MM HH:MM reason"', help="a gap that was the monitor's fault (the radar's own log shows it ran): not counted as unreachable")
r.set_defaults(fn=report)
a = ap.parse_args()
a.fn(a)
