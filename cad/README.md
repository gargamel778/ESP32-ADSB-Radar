# CAD model of the board

`UEDX80480070E-WB-A_board.step` is a simplified STEP model of the VIEWE UEDX80480070E-WB-A (7" 800×480 ESP32-S3 touch display) for designing a stand or an
enclosure around it. VIEWE's own STEP file is "under finalization" on their wiki at the time of writing, so this one is built from the numbers on VIEWE's mechanical
drawing (page 12 of *UEDX80480070E-WB-A V2.0 SPEC.pdf*, in [VIEWE's repository](https://github.com/VIEWESMART/UEDX80480070ESP32-7inch-Touch-Display)) and the board photographs in the same sheet.
It is **not** VIEWE's CAD and not a measurement of a board: check it against yours with calipers before you print. When VIEWE publishes theirs, prefer it.

```
python3 -m venv build/cadenv && build/cadenv/bin/pip install build123d
build/cadenv/bin/python cad/make_board_step.py     # writes the STEP file
build/cadenv/bin/python cad/check_board_step.py    # re-reads it, lists the parts, writes front / back / side / top drawings to build/cad-check/
```

**Axes (mm):** the origin is the centre of the four mounting holes; X to the right and Y up *as seen from the front* (the screen side); Z = 0 is the front face of the glass, the board lies behind it at negative Z.
Total size 180.90 × 105.45 × 12.53. Each part is a separate, named, coloured solid: `PCB`, `Display_module_glass`, `Active_area_(visible_picture)` (0.05 mm plate on the glass), `ESP32-S3-WROOM-1`, `microSD_socket`,
`USB-C_USB`, `USB-C_UART_flash_port` (the flashing / console port), `RGB_LED`, `Button_BOOT`, `Button_RESET`, `FPC_connector_J1_display`, `FPC_connector_J6_touch`.

| Exact (dimensions printed on the drawing) | Read off the drawing or estimated (about ±0.5 mm) |
|---|---|
| PCB 180.90 × 105.45 × 1.60; 4 × Ø4.10 holes at 173.91 × 98.47; display module 164.90 × 100.00 and its position; visible area 154.08 × 85.92 and its position; glass face to PCB 6.83; the connector and button positions along the bottom edge (110.71, 92.43, 75.55, 24.03, 13.54 mm); the two 20-pin 2.54 mm pad rows | PCB corner radius (2.5), the antenna notch in the top edge (20.1 × 6.6), the 2 mm steps in the bottom edge, the footprints of the SD socket, USB-C ports, LED, buttons and FPC connectors, and every height on the back (the ESP32 module's 4.1 mm is as drawn on the side view; the rest are typical values) |

Not modelled: resistors, capacitors, small ICs (all lower than 2.2 mm), the flexible tails of the display and the touch panel, the pad-row pins (none are fitted), the stickers.
For an enclosure: keep the antenna end (top edge, over the notch) free of metal and screws, leave openings for the USB-C ports and the two buttons, give the microSD socket a way in, and allow about 13 mm behind the glass.
Licence: GPL-3.0-or-later like the rest of the repository.
