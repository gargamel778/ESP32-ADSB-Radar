#!/usr/bin/env python3
"""A STEP model of the VIEWE UEDX80480070E-WB-A (7" ESP32-S3 touch display) for designing an enclosure.

    build/cadenv/bin/python cad/make_board_step.py        # writes cad/UEDX80480070E-WB-A_board.step (needs build123d)

Built from the numbers on VIEWE's mechanical drawing (page 12 of "UEDX80480070E-WB-A V2.0 SPEC.pdf") and the board photographs in the same sheet. It is a *simplified* model
for fitting an enclosure around the board, not VIEWE's own CAD: see cad/README.md for what is exact and what is read from the drawing (about +-0.5 mm) or estimated.

Coordinates (mm): the origin is the centre of the four mounting holes, X to the right and Y up as seen from the FRONT (screen side), Z = 0 is the front face of the glass and the
board lies behind it at negative Z (the viewer is at +Z).
"""
import os
from build123d import (Box, Cylinder, Compound, Color, Pos, Rot, Align, export_step, fillet, Axis)

# ---- numbers from the drawing -----------------------------------------------------------------------------------------------------------------------------
PCB_W, PCB_H, PCB_T = 180.90, 105.45, 1.60          # printed circuit board
Z_FRONT_TO_PCB = 6.83                                # front face of the glass to the front face of the PCB
HOLE_D, HOLE_DX, HOLE_DY = 4.10, 173.91, 98.47       # 4 mounting holes, spacing between centres
HOLE_X_EDGE, HOLE_Y_TOP = 3.495, 3.48                # centre of a hole from the left edge / the top edge
LCM_W, LCM_H = 164.90, 100.00                        # display module outline
LCM_LEFT, LCM_TOP = 8.00, 2.68                       # its position on the PCB
AA_W, AA_H = 154.08, 85.92                           # visible active area
AA_LEFT, AA_TOP = 11.93, 7.68
PIN_PITCH, PIN_N, PIN_EDGE, PIN_TOP, PIN_D = 2.54, 20, 2.49, 28.63, 1.0   # two 20-pin 2.54 mm pad rows (plated holes), no pins fitted

# ---- read from the drawing (about +-0.5 mm) or estimated ---------------------------------------------------------------------------------------------------
CORNER_R = 2.5                                       # PCB corner radius (estimated)
NOTCH_X0, NOTCH_X1, NOTCH_DEPTH = 81.4, 101.5, 6.6   # notch in the top edge under the ESP32 antenna (x from the left edge seen from the front)
RECESS_DEPTH = 2.0                                   # the bottom edge steps back by this much between the connector tabs
RECESSES = [(29.2, 68.4), (117.4, 163.8)]            # x ranges of the recessed bottom-edge sections
ESP_W, ESP_L, ESP_H = 18.0, 25.5, 4.1                # ESP32-S3-WROOM-1: footprint and height above the PCB back (4.1 as drawn on the side view)
ESP_X = 91.5                                         # its centre line from the left edge (front view); its top end sits at the top edge, over the notch

HX = HOLE_X_EDGE + HOLE_DX / 2                       # hole-pattern centre, from the left edge
HY = HOLE_Y_TOP + HOLE_DY / 2                        # and from the top edge


def p(xl, yt):
    """(distance from the left PCB edge, distance from the top PCB edge) as seen from the front -> model X, Y"""
    return xl - HX, HY - yt


def block(xl0, xl1, yt0, yt1, z0, z1):
    """axis-aligned box between PCB-edge coordinates and two Z values (z0 < z1)"""
    (x0, y1), (x1, y0) = p(xl0, yt0), p(xl1, yt1)
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(abs(x1 - x0), abs(y1 - y0), abs(z1 - z0))


Z_PCB_F = -Z_FRONT_TO_PCB                            # PCB front face
Z_PCB_B = Z_PCB_F - PCB_T                            # PCB back face

# ---- PCB ---------------------------------------------------------------------------------------------------------------------------------------------------
pcb = block(0, PCB_W, 0, PCB_H, Z_PCB_B, Z_PCB_F)
pcb = fillet(pcb.edges().filter_by(Axis.Z), CORNER_R)
pcb -= block(NOTCH_X0, NOTCH_X1, -1, NOTCH_DEPTH, Z_PCB_B - 1, Z_PCB_F + 1)
for x0, x1 in RECESSES:
    pcb -= block(x0, x1, PCB_H - RECESS_DEPTH, PCB_H + 1, Z_PCB_B - 1, Z_PCB_F + 1)
for sx in (-1, 1):
    for sy in (-1, 1):
        pcb -= Pos(sx * HOLE_DX / 2, sy * HOLE_DY / 2, (Z_PCB_B + Z_PCB_F) / 2) * Cylinder(HOLE_D / 2, PCB_T + 2)
for xl in (PIN_EDGE, PCB_W - PIN_EDGE):
    for i in range(PIN_N):
        x, y = p(xl, PIN_TOP + i * PIN_PITCH)
        pcb -= Pos(x, y, (Z_PCB_B + Z_PCB_F) / 2) * Cylinder(PIN_D / 2, PCB_T + 2)
pcb.label, pcb.color = "PCB", Color(0.10, 0.25, 0.65)

# ---- display module (glass, touch panel and backlight as one block) and the visible area ----------------------------------------------------------------
lcm = block(LCM_LEFT, LCM_LEFT + LCM_W, LCM_TOP, LCM_TOP + LCM_H, Z_PCB_F, 0)
lcm.label, lcm.color = "Display_module_glass", Color(0.05, 0.05, 0.05)
aa = block(AA_LEFT, AA_LEFT + AA_W, AA_TOP, AA_TOP + AA_H, -0.05, 0)
aa.label, aa.color = "Active_area_(visible_picture)", Color(0.35, 0.55, 0.85)

# ---- parts on the back (envelopes) -----------------------------------------------------------------------------------------------------------------------
Z0 = Z_PCB_B                                         # parts hang from the back face of the PCB towards -Z


def back(label, xl0, xl1, yt0, yt1, h, colour):
    b = block(xl0, xl1, yt0, yt1, Z0 - h, Z0)
    b.label, b.color = label, Color(*colour)
    return b


parts = [
    back("ESP32-S3-WROOM-1", ESP_X - ESP_W / 2, ESP_X + ESP_W / 2, 0.0, ESP_L, ESP_H, (0.75, 0.75, 0.78)),
    back("microSD_socket", 136.5, 152.0, 2.9, 18.1, 1.9, (0.7, 0.7, 0.7)),
    back("USB-C_USB", 110.71 - 4.5, 110.71 + 4.5, PCB_H - 7.3, PCB_H, 3.3, (0.8, 0.8, 0.8)),
    back("USB-C_UART_flash_port", 92.43 - 4.5, 92.43 + 4.5, PCB_H - 7.3, PCB_H, 3.3, (0.8, 0.8, 0.8)),
    back("RGB_LED", 75.55 - 2.5, 75.55 + 2.5, PCB_H - 7.0, PCB_H - 2.0, 1.6, (0.9, 0.9, 0.6)),
    back("Button_BOOT", 24.03 - 3.05, 24.03 + 3.05, 101.1, 104.9, 2.0, (0.2, 0.2, 0.2)),
    back("Button_RESET", 13.54 - 3.05, 13.54 + 3.05, 101.1, 104.9, 2.0, (0.2, 0.2, 0.2)),
    back("FPC_connector_J1_display", 29.3, 49.9, 44.9, 50.0, 2.1, (0.6, 0.45, 0.2)),
    back("FPC_connector_J6_touch", 139.0, 146.5, 57.5, 61.0, 2.1, (0.6, 0.45, 0.2)),
]

board = Compound(label="UEDX80480070E-WB-A", children=[pcb, lcm, aa] + parts)

if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "UEDX80480070E-WB-A_board.step")
    export_step(board, out)
    bb = board.bounding_box()
    print(f"wrote {out}")
    print(f"bounding box: X {bb.size.X:.2f}  Y {bb.size.Y:.2f}  Z {bb.size.Z:.2f}  (Z from {bb.min.Z:.2f} to {bb.max.Z:.2f})")
