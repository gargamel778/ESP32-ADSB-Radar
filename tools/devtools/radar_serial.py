#!/usr/bin/env python3
"""Talk to the radar over its serial console (the USB-C connector labelled UART): watch its log, send the developer commands, take screenshots.

    python3 tools/devtools/radar_serial.py monitor [seconds]            # print the log (all lines; Ctrl-C to stop)
    python3 tools/devtools/radar_serial.py send "X440,440" "X90,200"    # send commands, a short pause between them
    python3 tools/devtools/radar_serial.py shot out.png [x y w h step]   # the screen as a PNG (default: whole screen, every 2nd pixel, ~25 s)
    python3 tools/devtools/radar_serial.py run script.txt               # several steps in ONE session, see below

The port is RADAR_PORT or the only /dev/cu.wchusbserial* / /dev/ttyUSB* found. It is opened with DTR/RTS low, but on macOS with the CH340 chip **opening the port
still restarts the radar** (it is back after about 5 seconds). Every command above therefore starts from a freshly booted radar, and anything set earlier by a
separate command (a demo location with Y, for instance) is gone. Use `run` for a sequence: it opens the port once, waits for the radar to boot and then executes the lines
of the script, which are  `send <command>`,  `sleep <seconds>`,  `shot <file.png> [x y w h step]`  and comments starting with #.
A screenshot works by making the radar print its frame buffer as base64 over the console (serial 'D'): it blocks the radar's main loop for the
duration (about 25 s at step 2, 90 s at full resolution), so do not take them while a soak test is running.
Developer commands (see main.cpp): X<x>,<y> tap, Z<d0>,<d1> pinch, Y<lat>,<lon>,<name>,<cc>,<zone>,<posix tz> demo location (nothing is saved until the next restart; finish with R), E1|E2|E3 emergency squawk
on the nearest aircraft (E0 off), n night mode off/auto/on (saved!), L<lat>,<lon>,<range step> test location, T tear test, H heap, V SD card, R plain restart.
"""
import base64, glob, os, struct, sys, time, zlib

try:
    import serial
except ImportError:
    sys.exit("needs pyserial: pip install pyserial")


def open_port():
    port = os.environ.get("RADAR_PORT") or next(iter(glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/ttyUSB*")), None)
    if not port:
        sys.exit("no serial port found: set RADAR_PORT")
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.3
    s.dtr = False
    s.rts = False
    s.open()
    return s


def write_png(path, w, h, rows):
    raw = bytearray()
    for r in rows:
        raw.append(0)
        for x in range(w):
            v = r[2 * x] | (r[2 * x + 1] << 8)
            raw += bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def shot(s, path, x=0, y=0, w=800, h=480, step=2):
    s.reset_input_buffer()
    s.write(f"D{x},{y},{w},{h},{step}\n".encode())
    rows, ow, oh, state, t0 = [], 0, 0, 0, time.time()
    while time.time() - t0 < 400:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("FBDUMP BEGIN"):
            _, _, a, b = line.split(); ow, oh, state, rows = int(a), int(b), 1, []
        elif line.startswith("FBDUMP END"):
            break
        elif state == 1 and ":" in line:   # "<row>:<base64>"; a line that another task garbled is skipped and its row copies the one above
            num, _, data = line.partition(":")
            if num.isdigit() and len(data) == ((ow * 2 + 2) // 3) * 4:
                n = int(num)
                if n < oh:
                    while len(rows) < n:
                        rows.append(rows[-1] if rows else bytes(ow * 2))
                    if len(rows) == n:
                        rows.append(base64.b64decode(data))
    if not rows:
        sys.exit("no frame buffer received (is the radar in a mode that has a frame buffer to dump, and is the console free?)")
    rows += [bytes(ow * 2)] * (oh - len(rows))
    write_png(path, ow, oh, rows)
    print("wrote", path, ow, oh)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    s = open_port()
    cmd = sys.argv[1]
    if cmd == "monitor":
        end = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 1e9)
        while time.time() < end:
            line = s.readline().decode(errors="replace").rstrip()
            if line:
                print(line, flush=True)
    elif cmd == "send":
        for c in sys.argv[2:]:
            s.write((c if c.endswith("\n") else c + "\n").encode())
            print("sent", repr(c))
            time.sleep(1.5)
    elif cmd == "run":
        time.sleep(8)   # opening the port restarted the radar: let it boot (and its first aircraft arrive)
        s.reset_input_buffer()
        for line in (l.strip() for l in open(sys.argv[2])):
            if not line or line.startswith("#"):
                continue
            op, _, arg = line.partition(" ")
            if op == "send":
                s.write((arg if arg.endswith("\n") else arg + "\n").encode())
                print("sent", repr(arg), flush=True)
                time.sleep(1.5)
            elif op == "sleep":
                time.sleep(float(arg))
            elif op == "shot":
                a = arg.split()
                shot(s, a[0], *[int(v) for v in a[1:6]])
            else:
                sys.exit(f"unknown step: {line}")
    elif cmd == "shot":
        a = [int(v) for v in sys.argv[3:8]] if len(sys.argv) > 3 else []
        shot(s, sys.argv[2], *a)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
