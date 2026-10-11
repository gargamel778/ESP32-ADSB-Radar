# Hardware findings — VIEWE UEDX80480070E-WB-A

Tested 2026-10-09 on macOS. **Verdict: all required hardware works.**

## Identity (esptool, read-only)
- ESP32-S3 (QFN56) rev v0.2, dual-core 240 MHz, Wi-Fi + BLE 5
- 16 MB quad flash @ 80 MHz, 8 MB embedded octal PSRAM
- Secure Boot **disabled**, Flash Encryption **disabled** → freely flashable

## Connectors — important
- **USB-C labelled `UART`** → CH340C (`1A86:7523`) → UART0. This is the **programming + console port**
  (on macOS a `/dev/cu.wchusbserial…` device). Use this one.
- USB-C labelled `USB`: no data device ever enumerated on the Mac; treat as power-only. (GPIO19/20 are
  the touch I2C pins, so native USB is not available on this board.)

## Results (diag firmware `hardware-tests/diag`; serial `[DIAG]` lines + user's video of the screen)
| Test | Result |
|---|---|
| PSRAM 4 MB write/verify (+ inverted pass) | PASS, 0 bad words, W 33 MB/s / R 53 MB/s |
| GT911 touch, direct I2C driver | **PASS** — 800x480, 5 points, 0 I2C errors in ~5,900 reads; corner taps map 1:1 to screen coords (no swap/mirror) |
| RGB panel @ **16 MHz** pixel clock | **Clean** in video (1 fps stills and 30 fps consecutive-frame bursts: no tearing/shift), including frames with Wi-Fi joined and `API 200` on screen; ~24 fps. User reported occasional jumps/vibration — not visible in the recorded frames (those videos were of the auto-cycling build) |
| RGB panel @ **12 MHz** (original author's value) | **Clean** in video (stills + 30 fps burst) with Wi-Fi + API active; ~25–27 fps. Locked/flicker-fixed build awaiting the user's confirmation |
| RGB panel @ 20 MHz | **FAIL** — garbled, horizontally torn (PSRAM bandwidth underrun; video-verified) |
| RGB panel @ 25 MHz (VIEWE's own value) | **FAIL** — full-screen horizontal noise for the whole stage (video-verified) |
| 400x400 16-bit PSRAM sprite + push | ~21–24 fps (CPU/draw bound, independent of pixel clock) |
| Wi-Fi scan / join | PASS — joined in 1.2 s, RSSI -50…-54 dBm. 2.4 GHz only |
| api.airplanes.live fetch + filtered parse | **PASS** — HTTP 200, 13–15 aircraft within 54 NM, ~7 KB, fetch 0.7–1.0 s, parse 8–14 ms, ~196 KB internal heap free |
| microSD (CS10 CLK12 MOSI11 MISO13) | **PASS** — 64 GB SDHC, FAT mounts; 512 KB write + read-back verified, 0 bad bytes; ~580 KB/s write, ~1.4 MB/s read at 20 MHz SPI |
| Multi-touch (2+ fingers) | **PASS** (user-confirmed) |

## Operating point (provisional — 12 MHz; 16 MHz also looked clean)
`pclk = 12 MHz` for margin under load (refresh ≈ 23.5 Hz; 16 MHz = ≈ 31 Hz), HPW 48 / HBP 40 / HFP 88, VPW 6 / VBP 26 / VFP 10, `pclk_active_neg = 1`, RGB565,
framebuffer in PSRAM. Pins: see VIEWE's board header for this model (in [VIEWE's own repository](https://github.com/VIEWESMART/UEDX80480070ESP32-7inch-Touch-Display); not copied here)
and `hardware-tests/diag/src/main.cpp` (LovyanGFX order B0-4, G0-5, R0-4).

## Gotchas found
1. **Stock firmware is blank after a warm reset.** The factory demo (ESP32_Display_Panel) configures touch
   RST as `-1`, so the GT911 is never reset; its I2C init then fails, `board->begin()` asserts and the
   screen stays black in a boot loop. Our firmware resets the GT911 via IO38 itself.
2. **LovyanGFX's `Touch_GT911` returned no touches here** (init "passed", getTouch never reported).
   Use the direct driver in `diag/src/main.cpp` (`touchTask`): poll reg `0x814E` every ~8 ms, read points
   from `0x814F` (8 bytes each), write `0` to `0x814E`.
3. **GT911 I2C address varies between boots (0x5D one boot, 0x14 the next).** Always probe both.
4. **Pixel clock must be low with LovyanGFX's PSRAM framebuffer** (its Bus_RGB streams GDMA straight from PSRAM, no bounce buffer): 25 MHz is pure noise, 20 MHz tears; 16 and 12 MHz look clean (12 chosen for margin under load).
   Fallback only if jumps/vibration persist at 12 MHz: Espressif `esp_lcd` RGB driver with a bounce buffer (what VIEWE's stock demo uses at 25 MHz) + LovyanGFX sprite for drawing + GT911 driver unchanged. Don't auto-cycle clocks
   (each restart blanks the screen ~3 s — this was the "disappearing screen").
5. **CH340 bulk reads fail above 230400 baud** (`Serial data stream stopped` / `Corrupt data`).
   Flash writes at 230400 verify OK (~35 s for 1.2 MB). The backup script reads 256 KB chunks with retries.
6. **Use `~/.platformio/penv/bin/pio`**, not the `pio` on PATH. The PATH one runs in pyenv 3.11.9 with
   click 8.3.2, which breaks the pinned esptool; the private penv has click 8.1.8.
7. Original author's sketch assumes CrowPanel pins, a CrowPanel backlight I2C controller (addr 0x18)
   and a 12 MHz clock — none apply here (backlight is PWM on GPIO2).

8. **Text flicker is self-inflicted if you clear-then-redraw on the live framebuffer.** The panel scans the PSRAM framebuffer while you write it; draw text with a padded background and only when it changes.

10. **Opening the serial port restarts the board** (macOS, CH340): the DTR/RTS lines glitch when a program opens `/dev/cu.wchusbserial*`, even when it sets them low straight away. Keep one serial session open for as long as you need the console
    (the repository's helper scripts that open the port per command restart the radar each time; a demo location set with serial `Y` is lost that way).

9. Diag firmware's on-screen **LOCK** button *toggles* auto-cycling of pixel clocks (state is saved in NVS); serial `3` (or `0`-`3`) selects a clock and locks. This caused the "still cycles" report.

## Display backends compared (firmware M1, live radar, ~21 aircraft, Wi-Fi + HTTPS running)
Measured over serial on the real board; "jumps/shimmer" judgements are the user's eyes.
| Mode | Backend | Pixel clock | Render fps | Panel refresh | API fetch | Notes |
|---|---|---|---|---|---|---|
| L12 | LovyanGFX (DMA straight from PSRAM FB) | 12 MHz | 20.5 | ~23.5 Hz | 1.2 s | steady; text shimmers (low refresh) |
| L16 | LovyanGFX | 16 MHz | 19.5 | ~31 Hz | 1.2 s | pending user's eyes |
| L20 | LovyanGFX | 20 MHz | 18 | ~39 Hz | 1.3 s | pending user's eyes |
| E16c1 | Espressif esp_lcd + 10-line bounce buffer, ISR on core 1 | 16 MHz | 15.8 | 31.4 Hz | 1.3 s | pending |
| E20c1 | Espressif, ISR on core 1 | 20 MHz | 14 | 39.4 Hz | 1.1 s | pending |
| E16c0 | Espressif, ISR on core 0 (with Wi-Fi) | 16 MHz | 17.4 | 31.4 Hz | 1.9 s | user reported "jumps like crazy" with an Espressif build (which one not stated) |
| E25 | Espressif, ISR core 0 | 25 MHz | 27 (no data) | 48.9 Hz | — | **task watchdog reset ~20 s after boot** (see below); removed from the mode list |

Espressif backend facts (IDF 5.4.2 source, `esp_lcd_panel_rgb.c`):
- Bounce mode refills two internal-SRAM buffers with a **CPU `memcpy` from the PSRAM framebuffer inside the ISR**, so it costs
  ≈ pclk×2 bytes/s of CPU+PSRAM bandwidth on the core that created the panel (measured PSRAM CPU read ≈ 54 MB/s ⇒ 25 MHz ≈ 90 % of a core).
  At 25 MHz the Wi-Fi task + that ISR starved IDLE0 → task WDT abort. 20 MHz made TLS fetches 3-4x slower (core 0).
- Because bounce mode reads the FB through the CPU cache, drawing straight into the FB needs no cache flush.
- Prebuilt Arduino libs here have `CONFIG_LCD_RGB_ISR_IRAM_SAFE` **unset** (ISR can be delayed by flash-cache stalls/Wi-Fi critical sections →
  possible cause of picture slips) and data cache line 32 B. Changing that needs a custom-sdkconfig rebuild (pioarduino `custom_sdkconfig`, hybrid Arduino+IDF).
- LovyanGFX sprites must use `rgb565_nonswapped` to match the esp_lcd framebuffer; the firmware adopts `display->getColorDepth()`.

### User verdict on the first renderer (full-wedge redraw into PSRAM canvas, dirty-rect restore/push)
- L12: fine, text shimmers (low refresh). L16: jumps sideways a bit. L20: completely garbled.
- E16c1: better text, but artifacts + some tearing. E20c1: fine until data arrived, then jumped up/down about once a second.
- E16c0: jumps up/down.  ⇒ every higher-refresh mode failed under load, i.e. the display was starved by CPU PSRAM traffic.

### Renderer v2 (tile renderer in internal RAM + per-pixel sweep LUT) — measured, ~19 aircraft, Wi-Fi + HTTPS running
| Mode | Render fps (cap 30) | paint/frame | Panel refresh | API fetch |
|---|---|---|---|---|
| L12 | 30 | ~23 ms | ~23.5 Hz | 1.0 s |
| L16 | 30 | ~24-27 ms | ~31 Hz | 1.1 s |
| L20 | 30 | ~25 ms | ~39 Hz | 1.0 s |
| E16c1 | 29.6 | ~27 ms | 31.4 Hz | 1.0 s |
| E20c1 | 29.7 | ~30 ms | 39.3 Hz | 1.0 s |
What changed: nothing is drawn in PSRAM any more (pixel-by-pixel drawing there = cache-line read-modify-write that starves the display DMA/ISR).
Each changed rectangle is painted in a 96x96 internal-RAM tile (bg copied in, overlays drawn, pushed in one burst); the sweep trail is a
per-pixel lookup (`angleMap` bearing steps + colour LUT) instead of ~120 radial lines + redrawing rings; info panel repaints in 47-row bands, 2 per frame.
Also: `WiFi.setSleep(false)`, 30 fps cap. (First tile version without the LUT was *slower*: 13 fps, because every wedge tile redrew all lines+rings.)

### User verdict on renderer v2
L12 most stable but text poor/shimmers; L16 jumps a little, slight shimmer; L20 garbled; **E16c1 best looking so far, text more readable than L12**; E20c1 jumps.
Next steps taken: smooth anti-aliased DejaVu fonts (12/18/40) instead of the scaled 1-px 6x8 font, and live tap controls for sweep-trail length (60/40/25/15 deg)
and frame cap (30/20/15 fps). Paint cost on E20c1 with smooth fonts: ~38 ms/frame at trail 60 deg, ~29 ms at 40, ~20 ms at 15.

### Task / core layout (firmware M1)
| Work | Core | Priority | Notes |
|---|---|---|---|
| Arduino `loopTask`: render, touch-event handling, serial commands | 1 | 1 | frame cap via `delay`, 30 fps default |
| Wi-Fi + lwIP system tasks | 0 | (system) | fixed by the SDK |
| `adsb` fetch/parse task | 0 | 1 | every 5 s, ~1 s of work incl. TLS |
| `gt911` touch poll task | 0 | 2 | 8 ms I2C poll |
| Espressif bounce-buffer ISR (E modes) | chosen by mode name: E16**c1** = core 1, E16**c0** = core 0 | ISR | created by a task pinned to that core |
| LovyanGFX RGB (L modes) | GDMA hardware, LovyanGFX ISR on core 1 | ISR | no CPU copy |

## DISPLAY DECISION (2026-10-09, after user comparison)
**Default: Espressif esp_lcd RGB + 10-line bounce buffer, 16 MHz, display ISR on core 1 ("E16c1")**, LovyanGFX used only for sprites.
- E16c1: steady, best text (31 Hz refresh). L12: steadiest but text shimmers (23.5 Hz). L16: slight jumps/shimmer.
- Rejected: L20 (garbled), E20c1 (jumps and tears even with 15° trail and 15 fps), E16c0 (jumps). E25 impossible (CPU copy needs ~90 % of a core; watchdog).
- Tearing in E16c1 appeared intermittently: updates were landing while the scan read the same rows, and our 30 fps loop beat against the 31.3 Hz refresh.
  Fix: wait for vsync (`display::waitVsync`, driven by the RGB panel's on_vsync callback), then paint changed tiles top-to-bottom, each pushed straight to the
  screen (the scan moves ~16 rows/ms; we paint about as fast, so we stay ahead of it). A PSRAM staging buffer + burst flush was tried and rejected: PSRAM→PSRAM copy
  took 19-24 ms because it competes with the display's own PSRAM reads.
- Aircraft marks never vanish: 6 brightness steps (bright just after the sweep passes → dim floor), labels 4 steps. Coarse steps matter: a continuous fade repainted ~30 tiles/frame.
- Measured at default: 6-13 tiles/frame, paint 13-21 ms, vsync slack 3-9 ms, ~29 fps, refresh 31.4 Hz, fetch ~1.0 s, trail 25°.
- Dev controls still on screen (to be moved into a settings screen in M3): tap dial = next display mode, tap status text = trail length, tap range bars = frame cap.
  Modes: 0 E16c1 (default), 1 L12 (fallback), 2 L16, 3 E16c0. Serial: 0-3 mode, t trail, f cap.

### Tearing/jumping root cause, measured (2026-10-09, firmware instrumentation)
- Vsync interval over ~158 frames per 5 s window, through boot, Wi-Fi join and every fetch: 31,834-31,850 us (nominal 31,840), **0 late frames**.
  ⇒ the display ISR is never delayed (not by Wi-Fi, not by flash writes). A custom sdkconfig build (`LCD_RGB_ISR_IRAM_SAFE`, 64 KB data cache) was considered and is **not needed**.
- 13-22 % of tile pushes landed while the scan was inside (or ~20 lines ahead of) the tile's rows ⇒ a seam in nearly every frame. Cause: painting (16-34 ms with ~22 aircraft) is slower than the scan, so "wait for vsync, paint top-first" cannot stay ahead of it.
- Fix: **true double buffering** (esp_lcd `num_fbs = 2`). Paint into the hidden buffer, `esp_lcd_panel_draw_bitmap(fb_ptr)` makes it current at the next frame wrap (bounce mode: `bb_fb_index = cur_fb_index`).
  The old front buffer is only free after the driver's `on_frame_buf_complete` (the wrap), so `waitFlipped()` waits on that event, not on vsync.
  The new back buffer is one frame behind: it is synced by copying last frame's painted rectangles from the front buffer, **minus the areas repainted this frame** (rectangle subtraction),
  which cuts the copy to a few KB (a naive full sync cost ~10 ms and dropped fps to 15).
- Result: 24-30 fps locked to 31.3 Hz refresh, vsync jitter unchanged, no crashes.

### M2 measurements (firmware, real board)
- Validated TLS fetch: ~1.7 s; busy-airspace payload (558 aircraft, 360 KB): 7-8 s, parsed with the field filter, nearest 150 kept.
- Vsync interval stayed 31.8 ms ± 0.1 ms with **0 late frames** through settings saves (NVS flash writes), Wi-Fi outage/rejoin, TLS handshakes and the 150-aircraft case: flash writes do not visibly stall the display interrupt.
- Dense traffic cost: every changed aircraft = one tile with AA text; 150 aircraft produced 93-314 tiles/frame (0.7-2.7 s). Fixed by merging dirty rects, a full-repaint fallback and thinner labels.
- Internal heap: 83-87 KB idle, 22 KB low-water mark (boot + TLS handshake).

### Tearing investigation tools (2026-10-09)
- `v` (serial): every 10th frame, after the back-buffer sync, compares the hidden and visible framebuffers outside the areas about to be repainted. 212 checks across a full range sweep (200→150→200→300→400→20→50→100→200 km) and a tear-test on/off: **0 mismatches**. The double-buffer bookkeeping is sound.
- IDF here is built with `LCD_RGB_RESTART_IN_VSYNC=y`: the display DMA is restarted every vsync, so the pipeline cannot stay desynchronised between frames.
- Tear test (`T` over serial, or tap the exact centre of the radar): two full-screen bars sweep across at constant speed through the same double-buffer path. A bar that breaks horizontally = display-path tearing; straight bars = the artifact is in the radar's own drawing.
- Grid pitch is now R/5 = one ring step (one cell = range/5, a round number), clipped to the circle.

## DISPLAY DECISION v2 (2026-10-09, after the 400 km tearing report) — supersedes "E16c1 default" above
**Root cause of the persistent tearing: the bounce-buffer driver desynchronises and never recovers.**
- Bounce mode refills one of two 10-line buffers from a DMA end-of-chunk interrupt, 48 times per frame, choosing the buffer by `bb_eof_count % 2`.
  If an interrupt is late and two merge into one, the parity is wrong from then on. `bb_eof_count = 0` is only executed in the `#else` branch of
  `lcd_rgb_panel_try_restart_transmission`; this build has `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`, so it is **never reset**: every later frame is scrambled until a power cycle.
- Trigger: a burst of PSRAM traffic. Changing the range to 400 km rebuilds the whole static layer in PSRAM and repaints the full radar, delaying the ISR's 16 KB PSRAM→SRAM memcpy past its 0.6 ms deadline.
- Evidence: user's video of the tear test shows every bar cut into slices from two consecutive frames (offset = one frame of bar travel, ~15 px) and "L" steps in the last 10 rows; user: switching to 400 km started it, it stayed at 20 km, a power cycle cleared it.
- Not the cause (measured): flash writes, Wi-Fi, vsync timing (0 late frames), framebuffer contents (212 verifier checks, 0 mismatches).

**Fix: Espressif DIRECT mode** (`bounce_buffer_size_px = 0`, `num_fbs = 2`): the display DMA reads the PSRAM framebuffer itself, the driver flips between the two buffers, and the DMA is restarted
every vsync, so there is no per-chunk interrupt to get out of step. Frees ~32 KB internal RAM and ~half a core (no memcpy ISR). The flip (`draw_bitmap` with a pointer inside a framebuffer) also writes the CPU cache back (measured 0.48 ms).
Modes now: D16 (default), D14, E16c1 (bounce, kept for comparison only), L12, L16, E16c0. Verified on the board: 215 buffer checks / 0 mismatches / no crashes through the tear test and the full 20-400 km range sweep; fps 22-27 at 63 aircraft (400 km).
Open: whether D16 survives heavy PSRAM load without line glitches (direct mode's failure is transient, never persistent); judged by the user with the tear test + 400 km.

## DISPLAY DECISION v3 (2026-10-09, after a phone video of the tearing) — custom IDF library build
User report: at 400 km the picture jumped sideways and tore in D16 (direct mode); in E16c0 the radar was clean, but the full-screen bar test tore heavily.
Two different mechanisms, both load-related:
1. **Direct mode (D*)** — the display DMA reads the framebuffer from PSRAM, sharing the PSRAM bus with the CPU. A dense 400 km frame (tile repaints, static-layer rebuild, sync copies) starves the DMA FIFO and the line drifts sideways until the next vsync restart. Needs far less CPU PSRAM traffic or a lower clock; not fixable by configuration.
2. **Bounce mode (E*)** — immune to PSRAM latency (the ISR copies into internal SRAM), but one late interrupt flips the even/odd bounce-buffer parity for good (see v2). The bar test rewrites the whole screen, so it provokes the late interrupt.
**Fix applied: rebuild the Arduino IDF libraries once with `CONFIG_LCD_RGB_RESTART_IN_VSYNC=n`** (`custom_sdkconfig` in `firmware/platformio.ini`). In IDF 5.4.2 `lcd_rgb_panel_try_restart_transmission()` then resets `bb_eof_count` at every vsync and restarts the DMA only when a frame came up short of its 48 EOFs, so a missed interrupt costs one glitched frame instead of a permanent desync. This is the upstream default; Arduino's lib builder turns the opposite on.
Rebuild notes: the custom build starts from the generic `esp32s3/sdkconfig` (quad PSRAM), so the OPI settings of the stock `qio_opi` variant had to be restored explicitly (octal PSRAM, boot init, ignore-not-found, dynamic Wi-Fi TX buffers x32, RX BA window 6, TCP OOSEQ 4 — see platformio.ini). First attempt without them booted with `PSRAM ID read error`, no framebuffer. After restoring: psram=8192K, heap idle 86 KB (same as the stock libs), TLS fetch OK. Needs `firmware/default_16MB.csv` in the project (the IDF stage looks for it there). The `esp_idf_size --ng` / esptool `get_metavar` messages during the lib stage are harmless (no bootloader image is needed from that stage).
`IRAM_SAFE` was deliberately not enabled: the ISR copies from PSRAM, which is unreadable while flash cache is off, so it could only crash instead of glitch.
New dev stress test: tap the centre of the radar (bars) — it now also stalls the display-interrupt core for 2 ms every 1.5 s (serial `S` does the same without bars). With the stock library this scrambles the picture for good after the first stall; with the rebuilt one each stall must cost at most a single glitched frame.

### User verdict on the rebuilt library (2026-10-09) — DISPLAY DECISION v4: **E16c1 is the default**
- E16c1: clean at 400 km; bar test passes (no tearing); under the 2 ms/1.5 s interrupt-stall test the text occasionally jumps for a frame — the designed recovery. **Default.**
- E16c0 (ISR on core 0, shared with Wi-Fi): clean at 400 km, but text jumps a lot in the stall test. Kept as a selectable alternative.
- D16: the screen scrolls to the right. D14: the screen is offset. Direct mode is starved by CPU PSRAM traffic and does not recover cleanly; **dropped** from the on-screen cycle (serial `4`/`5` only).
- Mode order is now E16c1, E16c0, L12 (on-screen tap cycles these three), then L16, D16, D14 (serial only). NVS key renamed `disp3` so old saved indices cannot map to the wrong mode.
- Real-world stalls are far shorter and rarer than the test's 2 ms every 1.5 s (Wi-Fi/lwIP critical sections are tens of microseconds; flash writes only on a settings change), so occasional single-frame glitches are not expected in normal use.
- Heap watch: internal heap idles at ~84 KB but dips to a minimum of 33 KB during the TLS handshake. Settings UI (M3) is cheap; the phone web server (M4) and OTA (M7) need a heap budget first.

## Factory firmware backup
`backup/factory-flash-<board MAC>.bin` (kept locally, not published) = flash `0x0`–`0x240000` (bootloader, partition table, NVS,
otadata, app0 ≈ 790 KB; SHA-256 starts `0792cdb350ebf07b`; git-ignored). Restore:
`esptool --port <serial port> --baud 230400 write-flash 0x0 backup/factory-flash-<board MAC>.bin`

### Flash writes vs the display (M7, 2026-10-09)
The note above that "flash writes do not visibly stall the display interrupt" holds for the occasional NVS write (a few ms, one frame at worst). It does **not** hold for a firmware update: erasing/writing ~5 MB keeps the cache off for milliseconds, over and over, so the bounce-buffer ISR (which reads the PSRAM frame buffer) misses its 0.6 ms deadline continually. A phone video of a streamed update: doubled text, repeated lines, "98 %" shown twice. Fix in `ota.cpp`: collect the file in PSRAM first (screen unaffected), flash in one burst, and during the burst show a plain fill with no detail (a late interrupt then only shifts the fill's edge by a few lines). `IRAM_SAFE` is still not an option (the ISR needs PSRAM, unreadable while the cache is off).
