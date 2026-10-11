#!/usr/bin/env python3
"""Draw national flags (small, simplified, 27x18) and pack them for the radar: firmware/data/flags.bin.

The flags are drawn here from scratch (stripes, crosses, a few simple shapes; emblems are reduced to a dot or a symbol), so there is nothing
to download and nothing to license: they are part of this project (GPL-3.0-or-later). At 27x18 pixels they are for recognising a country at a glance.

    python3 tools/build_flags.py                 # writes firmware/data/flags.bin
    python3 tools/build_flags.py --sheet sheet.png    # also a contact sheet to look at
    python3 tools/build_flags.py --out /tmp/f.bin     # write somewhere else (the tests compare it with the committed file)

Needs rsvg-convert and ImageMagick (brew install librsvg imagemagick). Format of flags.bin:
  "FLG1", u16 count, u8 width, u8 height, count * char[2] ISO codes (sorted), count * width*height RGB565 little-endian pixels.
"""
import math, os, struct, subprocess, sys

W, H = 27, 18
HERE = os.path.dirname(os.path.abspath(__file__))
RANGES = os.path.join(HERE, "icao_ranges.txt")
OUT = os.path.join(HERE, "..", "firmware", "data", "flags.bin")


# ---------------------------------------------------------------- drawing helpers (30 x 20 design grid)
def R(x, y, w, h, c): return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{c}"/>'
def C(cx, cy, r, c): return f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{c}"/>'
def P(pts, c, stroke=None, sw=0.8):
    s = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ""
    return f'<polygon points="{" ".join(f"{x},{y}" for x, y in pts)}" fill="{c}"{s}/>'
def L(d, c, w): return f'<path d="{d}" stroke="{c}" stroke-width="{w}" fill="none"/>'
def A(d, c): return f'<path d="{d}" fill="{c}"/>'

def stripes(cols, weights=None, horizontal=True, x=0, y=0, w=30, h=20):
    weights = weights or [1] * len(cols)
    tot = float(sum(weights)); out = []; pos = 0.0
    for c, k in zip(cols, weights):
        size = (h if horizontal else w) * k / tot
        out.append(R(x, y + pos, w, size + 0.02, c) if horizontal else R(x + pos, y, size + 0.02, h, c))
        pos += size
    return "".join(out)
def Hs(*cols, w=None): return stripes(cols, w, True)
def Vs(*cols, w=None): return stripes(cols, w, False)

def star(cx, cy, r, c, n=5, inner=None, rot=-90):
    inner = inner if inner is not None else (r * 0.382 if n == 5 else r * 0.5)
    pts = []
    for i in range(n * 2):
        a = math.radians(rot + i * 180.0 / n); rr = r if i % 2 == 0 else inner
        pts.append((round(cx + rr * math.cos(a), 2), round(cy + rr * math.sin(a), 2)))
    return P(pts, c)

def crescent(cx, cy, r, c, bg, off=1.2, r2=None):
    return C(cx, cy, r, c) + C(cx + off, cy, r2 or r * 0.82, bg)

def ring(cx, cy, r, n, dr, c):
    return "".join(C(round(cx + r * math.cos(2 * math.pi * i / n), 2), round(cy + r * math.sin(2 * math.pi * i / n), 2), dr, c) for i in range(n))

def nordic(field, cross, edge=None, cx=11, wide=4.4):
    out = R(0, 0, 30, 20, field)
    if edge:
        out += R(0, 10 - wide / 2 - 1.2, 30, wide + 2.4, cross) + R(cx - wide / 2 - 1.2, 0, wide + 2.4, 20, cross)
        out += R(0, 10 - wide / 2, 30, wide, edge) + R(cx - wide / 2, 0, wide, 20, edge)
    else:
        out += R(0, 10 - wide / 2, 30, wide, cross) + R(cx - wide / 2, 0, wide, 20, cross)
    return out

def jack(x, y, w, h):   # Union Jack in a box
    return (f'<svg x="{x}" y="{y}" width="{w}" height="{h}" viewBox="0 0 60 30" preserveAspectRatio="none">'
            '<rect width="60" height="30" fill="#012169"/>'
            '<path d="M0,0 L60,30 M60,0 L0,30" stroke="#fff" stroke-width="6"/>'
            '<path d="M0,0 L60,30 M60,0 L0,30" stroke="#C8102E" stroke-width="2"/>'
            '<path d="M30,0 V30 M0,15 H60" stroke="#fff" stroke-width="10"/>'
            '<path d="M30,0 V30 M0,15 H60" stroke="#C8102E" stroke-width="6"/></svg>')

def scaled(pts, cx, cy, k): return [(round(cx + (x - cx) * k, 2), round(cy + (y - cy) * k, 2)) for x, y in pts]

WHITE, BLACK = "#FFFFFF", "#000000"

# ---------------------------------------------------------------- the flags
F = {}
F["AD"] = Vs("#10069F", "#FEDF00", "#D50032") + C(15, 10, 1.8, "#C8A02A")
F["AE"] = Hs("#00732F", WHITE, BLACK) + R(0, 0, 7.5, 20, "#FF0000")
F["AF"] = Vs(BLACK, "#D32011", "#007A36") + C(15, 10, 3.4, WHITE) + C(15, 10, 2, "#D32011")
F["AG"] = R(0, 0, 30, 20, "#CE1126") + P([(0, 0), (30, 0), (15, 20)], BLACK) + A("M9,8 A6,6 0 0 1 21,8 Z", "#FCD116") + R(8.5, 8, 13, 2.6, "#0072C6") + R(11, 10.6, 8, 2.2, WHITE)
F["AL"] = R(0, 0, 30, 20, "#E41E20") + C(15, 10, 4, BLACK) + P([(5, 10), (12, 6.5), (12, 13.5)], BLACK) + P([(25, 10), (18, 6.5), (18, 13.5)], BLACK) + C(12.6, 5, 1.4, BLACK) + C(17.4, 5, 1.4, BLACK)
F["AM"] = Hs("#D90012", "#0033A0", "#F2A800")
F["AO"] = Hs("#CC092F", BLACK) + C(15, 10, 3.6, "#FFCB00") + C(15, 10, 2.2, "#CC092F") + star(15, 10, 2, "#FFCB00")
F["AR"] = Hs("#74ACDF", WHITE, "#74ACDF") + C(15, 10, 2.1, "#F6B40E")
F["AT"] = Hs("#ED2939", WHITE, "#ED2939")
F["AU"] = R(0, 0, 30, 20, "#00008B") + jack(0, 0, 15, 10) + star(7.5, 15.5, 3.2, WHITE, 7, 1.4) + "".join(C(x, y, .85, WHITE) for x, y in [(22, 3.5), (26.5, 8), (21, 8.5), (23.5, 13.5), (25, 17)])
F["AZ"] = Hs("#00B5E2", "#EF3340", "#509E2F") + crescent(13.4, 10, 3.2, WHITE, "#EF3340", 1.1, 2.7) + star(17.6, 10, 1.4, WHITE)
F["BA"] = R(0, 0, 30, 20, "#002F6C") + P([(8, 0), (25, 0), (25, 20)], "#FECB00") + "".join(C(7.5 + i * 2.4, i * 2.6 + 1, .8, WHITE) for i in range(7))
F["BB"] = Vs("#00267F", "#FFC726", "#00267F") + R(14.2, 6, 1.6, 9, BLACK) + R(12.4, 6, 1.2, 4, BLACK) + R(16.4, 6, 1.2, 4, BLACK)
F["BD"] = R(0, 0, 30, 20, "#006A4E") + C(13.5, 10, 6, "#F42A41")
F["BE"] = Vs(BLACK, "#FDDA24", "#EF3340")
F["BF"] = Hs("#EF2B2D", "#009E49") + star(15, 10, 4, "#FCD116")
F["BG"] = Hs(WHITE, "#00966E", "#D62612")
F["BH"] = R(0, 0, 30, 20, "#CE1126") + P([(0, 0), (8, 0)] + [pt for i in range(9) for pt in ((11, 2.222 * i + 1.111), (8, 2.222 * (i + 1)))] + [(0, 20)], WHITE)
F["BI"] = R(0, 0, 30, 20, "#1EB53A") + P([(0, 0), (30, 0), (15, 10)], "#CE1126") + P([(0, 20), (30, 20), (15, 10)], "#CE1126") + L("M0,0 L30,20 M30,0 L0,20", WHITE, 2.4) + C(15, 10, 4.6, WHITE) + "".join(C(15 + dx, 10 + dy, .9, "#CE1126") for dx, dy in [(0, -2), (-1.8, 1.2), (1.8, 1.2)])
F["BJ"] = R(0, 0, 12, 20, "#008751") + R(12, 0, 18, 10, "#FCD116") + R(12, 10, 18, 10, "#E8112D")
F["BN"] = R(0, 0, 30, 20, "#F7E017") + P([(0, 3), (0, 8.5), (30, 17), (30, 11.5)], WHITE) + P([(0, 8.5), (0, 11.5), (30, 20), (30, 17)], BLACK) + C(15, 10, 3.4, "#CF1126")
F["BO"] = Hs("#D52B1E", "#F9E300", "#007934")
F["BR"] = R(0, 0, 30, 20, "#009C3B") + P([(15, 1.8), (27.5, 10), (15, 18.2), (2.5, 10)], "#FFDF00") + C(15, 10, 4.4, "#002776") + L("M10.8,9.2 Q15,8 19.2,11.4", WHITE, 0.9)
F["BS"] = Hs("#00778B", "#FFC72C", "#00778B") + P([(0, 0), (0, 20), (11, 10)], BLACK)
F["BT"] = P([(0, 0), (30, 0), (0, 20)], "#FFD520") + P([(30, 0), (30, 20), (0, 20)], "#FF4E12") + L("M5,16 Q15,10 25,4", WHITE, 2)
F["BW"] = R(0, 0, 30, 20, "#75AADB") + R(0, 7, 30, 6, WHITE) + R(0, 8.1, 30, 3.8, BLACK)
F["BY"] = R(0, 0, 30, 13.4, "#C8313E") + R(0, 13.4, 30, 6.6, "#4AA657") + R(0, 0, 3.4, 20, WHITE) + "".join(R(0, y, 3.4, 1.1, "#C8313E") for y in range(1, 20, 2))
F["BZ"] = R(0, 0, 30, 20, "#003F87") + R(0, 0, 30, 2.4, "#CE1126") + R(0, 17.6, 30, 2.4, "#CE1126") + C(15, 10, 6.6, WHITE) + C(15, 10, 5.2, "#6FAF48")
F["CA"] = R(0, 0, 7.5, 20, "#FF0000") + R(7.5, 0, 15, 20, WHITE) + R(22.5, 0, 7.5, 20, "#FF0000") + P([(15, 3.2), (16.4, 6), (18.6, 5.2), (17.8, 9), (20.4, 8.2), (19.2, 11), (21, 12.4), (15.8, 13.3), (15.8, 16.8), (14.2, 16.8), (14.2, 13.3), (9, 12.4), (10.8, 11), (9.6, 8.2), (12.2, 9), (11.4, 5.2), (13.6, 6)], "#FF0000")
F["CD"] = R(0, 0, 30, 20, "#007FFF") + L("M-2,22 L32,-2", "#F7D618", 6.4) + L("M-2,22 L32,-2", "#CE1021", 4) + star(5, 4.6, 3.4, "#F7D618")
F["CF"] = Hs("#003082", WHITE, "#289728", "#FFCE00") + R(12.5, 0, 5, 20, "#D21034") + star(4, 2.8, 1.8, "#FFCE00")
F["CG"] = R(0, 0, 30, 20, "#FBDE4A") + P([(0, 0), (20, 0), (0, 20)], "#009543") + P([(30, 0), (30, 20), (10, 20)], "#DC241F")
F["CH"] = R(0, 0, 30, 20, "#DA291C") + R(12.5, 3.5, 5, 13, WHITE) + R(8.5, 7.5, 13, 5, WHITE)
F["CI"] = Vs("#F77F00", WHITE, "#009E60")
F["CK"] = R(0, 0, 30, 20, "#00247D") + jack(0, 0, 15, 10) + ring(21.5, 13.5, 4, 15, .5, WHITE)
F["CL"] = Hs(WHITE, "#D52B1E") + R(0, 0, 10, 10, "#0039A6") + star(5, 5, 3, WHITE)
F["CM"] = Vs("#007A5E", "#CE1126", "#FCD116") + star(15, 10, 3.6, "#FCD116")
F["CN"] = R(0, 0, 30, 20, "#EE1C25") + star(5.5, 5.5, 3.6, "#FFFF00") + "".join(star(x, y, .95, "#FFFF00") for x, y in [(10.6, 2.4), (12.4, 5), (12.4, 8), (10.6, 10.4)])
F["CO"] = Hs("#FCD116", "#003893", "#CE1126", w=[2, 1, 1])
F["CR"] = Hs("#002B7F", WHITE, "#CE1126", WHITE, "#002B7F", w=[1, 1, 2, 1, 1])
F["CU"] = Hs("#002A8F", WHITE, "#002A8F", WHITE, "#002A8F") + P([(0, 0), (0, 20), (13, 10)], "#CB1515") + star(4.2, 10, 3, WHITE)
F["CV"] = R(0, 0, 30, 20, "#003893") + R(0, 10, 30, 3.4, WHITE) + R(0, 11.1, 30, 1.2, "#CF2027") + ring(10, 11.7, 5, 10, .55, "#F7D116")
F["CY"] = R(0, 0, 30, 20, WHITE) + A("M8,8 Q12,4.5 17,6 Q22,6.5 23,9.5 Q19,13 14,11.5 Q10,12 8,8 Z", "#D57800") + L("M11,15.5 Q15,17.5 19,15.5", "#4E5B31", 1.1)
F["CZ"] = Hs(WHITE, "#D7141A") + P([(0, 0), (15, 10), (0, 20)], "#11457E")
F["DE"] = Hs(BLACK, "#DD0000", "#FFCE00")
F["DJ"] = Hs("#6AB2E7", "#12AD2B") + P([(0, 0), (15, 10), (0, 20)], WHITE) + star(4.5, 10, 2.4, "#D7141A")
F["DK"] = nordic("#C8102E", WHITE)
F["DM"] = R(0, 0, 30, 20, "#006B3F")
F["DO"] = R(0, 0, 15, 10, "#002D62") + R(15, 0, 15, 10, "#CE1126") + R(0, 10, 15, 10, "#CE1126") + R(15, 10, 15, 10, "#002D62") + R(13, 0, 4, 20, WHITE) + R(0, 8, 30, 4, WHITE) + C(15, 10, 1.6, "#3A7D44")
F["DZ"] = R(0, 0, 15, 20, "#006233") + R(15, 0, 15, 20, WHITE) + C(14.6, 10, 4.5, "#D21034") + C(15.9, 10, 3.7, WHITE) + star(17.6, 10, 1.9, "#D21034")
F["EC"] = Hs("#FFDD00", "#034EA2", "#ED1C24", w=[2, 1, 1]) + C(15, 10, 2.4, "#8B6B3A")
F["EE"] = Hs("#0072CE", BLACK, WHITE)
F["EG"] = Hs("#CE1126", WHITE, BLACK) + C(15, 10, 1.9, "#C09300")
F["ER"] = R(0, 0, 30, 10, "#12AD2B") + R(0, 10, 30, 10, "#4189DD") + P([(0, 0), (30, 10), (0, 20)], "#EA0437") + C(6, 10, 2.6, "#FFC726") + C(6, 10, 1.4, "#EA0437")
F["ES"] = Hs("#AA151B", "#F1BF00", "#AA151B", w=[1, 2, 1]) + R(6.2, 7.2, 4, 5.4, "#AD1519") + R(6.2, 7.2, 4, 1.2, "#C8A400")
F["ET"] = Hs("#078930", "#FCDD09", "#DA121A") + C(15, 10, 5, "#0F47AF") + star(15, 10, 3.6, "#FCDD09")
F["FI"] = nordic(WHITE, "#003580", None, 9.5, 4.6)
F["FJ"] = R(0, 0, 30, 20, "#68BFE5") + jack(0, 0, 15, 10) + R(19, 5, 7, 9, WHITE) + R(19, 5, 7, 3, "#CE1126")
F["FM"] = R(0, 0, 30, 20, "#75B2DD") + "".join(star(x, y, 2.2, WHITE) for x, y in [(15, 4.5), (20.5, 10), (15, 15.5), (9.5, 10)])
F["FR"] = Vs("#0055A4", WHITE, "#EF4135")
F["GA"] = Hs("#009E60", "#FCD116", "#3A75C4")
F["GB"] = jack(0, 0, 30, 20)
F["GD"] = R(0, 0, 30, 20, "#CE1126") + P([(3, 3), (27, 3), (15, 10)], "#FCD116") + P([(3, 17), (27, 17), (15, 10)], "#FCD116") + P([(3, 3), (3, 17), (15, 10)], "#007A5E") + P([(27, 3), (27, 17), (15, 10)], "#007A5E") + C(15, 10, 3.3, "#CE1126") + star(15, 10, 2.2, "#FCD116")
F["GE"] = R(0, 0, 30, 20, WHITE) + R(12.5, 0, 5, 20, "#FF0000") + R(0, 7.5, 30, 5, "#FF0000") + "".join(R(x - 1.4, y - .45, 2.8, .9, "#FF0000") + R(x - .45, y - 1.4, .9, 2.8, "#FF0000") for x, y in [(6, 3.5), (24, 3.5), (6, 16.5), (24, 16.5)])
F["GH"] = Hs("#CE1126", "#FCD116", "#006B3F") + star(15, 10, 3.6, BLACK)
F["GM"] = Hs("#CE1126", WHITE, "#0C1C8C", WHITE, "#3A7728", w=[6, 1, 4, 1, 6])
F["GN"] = Vs("#CE1126", "#FCD116", "#009460")
F["GQ"] = Hs("#3E9A00", WHITE, "#E32118") + P([(0, 0), (8, 10), (0, 20)], "#0073CE")
F["GR"] = Hs("#0D5EAF", WHITE, "#0D5EAF", WHITE, "#0D5EAF", WHITE, "#0D5EAF", WHITE, "#0D5EAF") + R(0, 0, 11.2, 11.2, "#0D5EAF") + R(4.5, 0, 2.2, 11.2, WHITE) + R(0, 4.5, 11.2, 2.2, WHITE)
F["GT"] = Vs("#4997D0", WHITE, "#4997D0") + C(15, 10, 2.5, "#8E7F2A")
F["GW"] = R(0, 0, 10, 20, "#CE1126") + R(10, 0, 20, 10, "#FCD116") + R(10, 10, 20, 10, "#009E49") + star(5, 10, 3, BLACK)
F["GY"] = R(0, 0, 30, 20, "#009E49") + P([(0, 0), (30, 10), (0, 20)], WHITE) + P([(0, 1.6), (27, 10), (0, 18.4)], "#FCD116") + P([(0, 0), (15, 10), (0, 20)], BLACK) + P([(0, 2), (12.5, 10), (0, 18)], "#CE1126")
F["HK"] = R(0, 0, 30, 20, "#DE2910") + "".join(C(round(15 + 2.4 * math.cos(math.radians(-90 + 72 * i)), 2), round(10 + 2.4 * math.sin(math.radians(-90 + 72 * i)), 2), 2, WHITE) for i in range(5)) + C(15, 10, .9, "#DE2910")
F["HN"] = Hs("#0073CF", WHITE, "#0073CF") + "".join(star(x, y, 1.3, "#0073CF") for x, y in [(11.5, 10), (18.5, 10), (15, 10), (13, 8.4), (17, 11.6)])
F["HR"] = Hs("#FF0000", WHITE, "#171796") + "".join(R(12.5 + 1.6667 * c, 6.2 + 1.6 * r, 1.6667, 1.6, "#FF0000" if (r + c) % 2 == 0 else WHITE) for r in range(4) for c in range(3)) + R(12.5, 12.6, 5, .6, "#171796")
F["HT"] = Hs("#00209F", "#D21034") + R(10.5, 6, 9, 8, WHITE) + R(12.8, 8, 4.4, 4, "#16A34A")
F["HU"] = Hs("#CE2939", WHITE, "#477050")
F["ID"] = Hs("#FF0000", WHITE)
F["IE"] = Vs("#169B62", WHITE, "#FF883E")
F["IL"] = R(0, 0, 30, 20, WHITE) + R(0, 2.2, 30, 3, "#0038B8") + R(0, 14.8, 30, 3, "#0038B8") + P([(15, 6.2), (18.3, 11.9), (11.7, 11.9)], "none", "#0038B8", .9) + P([(15, 13.8), (11.7, 8.1), (18.3, 8.1)], "none", "#0038B8", .9)
F["IN"] = Hs("#FF9933", WHITE, "#138808") + '<circle cx="15" cy="10" r="2.7" fill="none" stroke="#000080" stroke-width=".6"/>' + C(15, 10, .7, "#000080")
F["IQ"] = Hs("#CE1126", WHITE, BLACK) + R(10, 9.2, 10, 1.6, "#007A3D")
F["IR"] = Hs("#239F40", WHITE, "#DA0000") + C(15, 10, 2, "#DA0000") + R(5, 6.2, 20, .8, WHITE)
F["IS"] = nordic("#02529C", WHITE, "#DC1E35", 9.5, 3.6)
F["IT"] = Vs("#009246", WHITE, "#CE2B37")
F["JM"] = R(0, 0, 30, 20, BLACK) + P([(0, 0), (30, 0), (15, 10)], "#009B3A") + P([(0, 20), (30, 20), (15, 10)], "#009B3A") + L("M0,0 L30,20 M30,0 L0,20", "#FED100", 2.6)
F["JO"] = Hs(BLACK, WHITE, "#007A3D") + P([(0, 0), (15, 10), (0, 20)], "#CE1126") + star(4.6, 10, 2, WHITE, 7, 1)
F["JP"] = R(0, 0, 30, 20, WHITE) + C(15, 10, 6, "#BC002D")
F["KE"] = R(0, 0, 30, 5.6, BLACK) + R(0, 5.6, 30, 1, WHITE) + R(0, 6.6, 30, 6.8, "#BB0000") + R(0, 13.4, 30, 1, WHITE) + R(0, 14.4, 30, 5.6, "#006600") + '<ellipse cx="15" cy="10" rx="2.6" ry="4.6" fill="#BB0000" stroke="#fff" stroke-width=".8"/>'
F["KG"] = R(0, 0, 30, 20, "#E8112D") + C(15, 10, 5.2, "#FFEF00") + C(15, 10, 3, "#E8112D")
F["KH"] = Hs("#032EA1", "#E00025", "#032EA1", w=[1, 2, 1]) + R(9.5, 8, 11, 3.2, WHITE) + R(11, 6.4, 1.6, 1.6, WHITE) + R(14.2, 5.6, 1.6, 2.4, WHITE) + R(17.4, 6.4, 1.6, 1.6, WHITE)
F["KI"] = R(0, 0, 30, 11, "#CE1126") + R(0, 11, 30, 9, "#003F87") + R(0, 12.2, 30, 1, WHITE) + R(0, 15, 30, 1, WHITE) + R(0, 17.8, 30, 1, WHITE) + A("M10,11 A5,5 0 0 1 20,11 Z", "#FCD116")
F["KM"] = Hs("#FFC61E", WHITE, "#CE1126", "#3A75C4") + P([(0, 0), (14, 10), (0, 20)], "#3D8E33") + crescent(4, 10, 3.6, WHITE, "#3D8E33", 1.2)
F["KP"] = R(0, 0, 30, 20, "#024FA2") + R(0, 3.2, 30, 13.6, WHITE) + R(0, 4.5, 30, 11, "#ED1C27") + C(10, 10, 4.3, WHITE) + star(10, 10, 3.4, "#ED1C27")
F["KR"] = R(0, 0, 30, 20, WHITE) + C(15, 10, 4.4, "#CD2E3A") + A("M10.6,10 A4.4,4.4 0 0 0 19.4,10 Z", "#0047A0") + "".join(R(x, y, 3.2, .7, BLACK) + R(x, y + 1.5, 3.2, .7, BLACK) for x, y in [(3.4, 3), (23.4, 3), (3.4, 15), (23.4, 15)])
F["KW"] = Hs("#007A3D", WHITE, "#CE1126") + P([(0, 0), (8, 6.6), (8, 13.4), (0, 20)], BLACK)
F["KZ"] = R(0, 0, 30, 20, "#00AFCA") + C(15, 10, 4, "#FEC50C") + R(2.6, 0, 1.2, 20, "#FEC50C") + L("M9,15 Q15,18.5 21,15", "#FEC50C", 1.1)
F["LA"] = Hs("#CE1126", "#002868", "#CE1126", w=[1, 2, 1]) + C(15, 10, 4, WHITE)
F["LB"] = Hs("#ED1C24", WHITE, "#ED1C24", w=[1, 2, 1]) + P([(15, 4.8), (18.8, 11.4), (16.2, 11.4), (19.6, 14.4), (16, 14.4), (16, 15.6), (14, 15.6), (14, 14.4), (10.4, 14.4), (13.8, 11.4), (11.2, 11.4)], "#00A651")
F["LC"] = R(0, 0, 30, 20, "#66CCFF") + P([(15, 2.5), (25, 17), (5, 17)], WHITE) + P([(15, 4.5), (22.8, 16.4), (7.2, 16.4)], BLACK) + P([(15, 9.4), (22.8, 16.4), (7.2, 16.4)], "#FCD116")
F["LK"] = R(0, 0, 30, 20, "#FFBE29") + R(1.2, 1.2, 3.2, 17.6, "#00534E") + R(4.4, 1.2, 3.2, 17.6, "#EB7400") + R(8.6, 1.2, 20.2, 17.6, "#8D2029") + C(18.5, 10, 3.3, "#FFBE29")
F["LR"] = Hs(*(["#BF0A30", WHITE] * 5 + ["#BF0A30"])) + R(0, 0, 9, 9.1, "#002868") + star(4.5, 4.6, 3, WHITE)
F["LS"] = Hs("#00209F", WHITE, "#009543", w=[3, 4, 3]) + P([(15, 7), (17.8, 13.4), (12.2, 13.4)], BLACK)
F["LT"] = Hs("#FDB913", "#006A44", "#C1272D")
F["LU"] = Hs("#ED2939", WHITE, "#00A1DE")
F["LV"] = R(0, 0, 30, 20, "#9E3039") + R(0, 8, 30, 4, WHITE)
F["LY"] = Hs("#E70013", BLACK, "#239E46", w=[1, 2, 1]) + crescent(14, 10, 3.3, WHITE, BLACK, 1, 2.7) + star(17.8, 10, 1.4, WHITE)
F["MA"] = R(0, 0, 30, 20, "#C1272D") + P([(15, 4.6), (16.7, 9.6), (22, 9.6), (17.7, 12.7), (19.4, 17.5), (15, 14.5), (10.6, 17.5), (12.3, 12.7), (8, 9.6), (13.3, 9.6)], "none", "#006233", 1)
F["MC"] = Hs("#CE1126", WHITE)
F["MD"] = Vs("#0046AE", "#FFD200", "#CC092F") + C(15, 10, 2.2, "#8B5A2B")
F["ME"] = R(0, 0, 30, 20, "#D3AE3B") + R(1, 1, 28, 18, "#C40308") + C(15, 10, 3.4, "#D3AE3B")
F["MG"] = R(0, 0, 10, 20, WHITE) + R(10, 0, 20, 10, "#FC3D32") + R(10, 10, 20, 10, "#007E3A")
F["MH"] = R(0, 0, 30, 20, "#003893") + P([(0, 20), (30, 2), (30, 7.5)], "#DD7500") + P([(0, 20), (30, 7.5), (30, 12)], WHITE) + star(6, 6, 4.2, WHITE, 8, 1.8)
F["MK"] = R(0, 0, 30, 20, "#D20000") + "".join(L(f"M15,10 L{round(15 + 25 * math.cos(math.radians(a)), 1)},{round(10 + 25 * math.sin(math.radians(a)), 1)}", "#FFE600", 1.5) for a in range(0, 360, 45)) + C(15, 10, 3.4, "#D20000") + C(15, 10, 2.6, "#FFE600")
F["ML"] = Vs("#14B53A", "#FCD116", "#CE1126")
F["MM"] = Hs("#FECB00", "#34B233", "#EA2839") + star(15, 10.6, 5.6, WHITE)
F["MN"] = Vs("#DA2032", "#015197", "#DA2032") + R(3.4, 5, 1.2, 9, "#FFD900") + C(4, 6.2, 1, "#FFD900") + C(4, 9, .9, "#FFD900")
F["MR"] = R(0, 0, 30, 20, "#006233") + R(0, 0, 30, 2.6, "#D01C1F") + R(0, 17.4, 30, 2.6, "#D01C1F") + C(15, 9, 5.6, "#FFC400") + C(15, 7.6, 4.8, "#006233") + star(15, 7.4, 1.8, "#FFC400")
F["MT"] = R(0, 0, 15, 20, WHITE) + R(15, 0, 15, 20, "#CF142B") + R(4.2, 1.6, .9, 3.4, "#9A9A9A") + R(3, 2.8, 3.3, .9, "#9A9A9A")
F["MU"] = Hs("#EA2839", "#1A206D", "#FFD500", "#00A551")
F["MV"] = R(0, 0, 30, 20, "#D21034") + R(5, 4.5, 20, 11, "#007E3A") + C(15, 10, 3.6, WHITE) + C(16.2, 10, 3, "#007E3A")
F["MW"] = Hs(BLACK, "#CE1126", "#339E35") + A("M10.5,6.7 A4.5,4.5 0 0 1 19.5,6.7 Z", "#CE1126")
F["MX"] = Vs("#006847", WHITE, "#CE1126") + C(15, 10, 2.5, "#8C6A2F") + L("M12,13.2 Q15,16 18,13.2", "#006847", .9)
F["MY"] = Hs(*(["#CC0001", WHITE] * 7)) + R(0, 0, 15, 10.7, "#010066") + crescent(5.6, 5.3, 3.6, "#FFCC00", "#010066", 1, 2.9) + star(10, 5.3, 2.4, "#FFCC00", 8, 1.2)
F["MZ"] = Hs("#007168", WHITE, BLACK, WHITE, "#FCE100", w=[5, .8, 4.2, .8, 5]) + P([(0, 0), (13, 10), (0, 20)], "#D21034") + star(4.8, 10, 3.2, "#FCE100")
F["NA"] = R(0, 0, 30, 20, "#003580") + P([(30, 0), (30, 20), (0, 20)], "#009543") + L("M-2,22 L32,-2", WHITE, 5.6) + L("M-2,22 L32,-2", "#D21034", 3.6) + C(5.2, 4.6, 2.5, "#FFCE00")
F["NE"] = Hs("#E05206", WHITE, "#0DB02B") + C(15, 10, 2.7, "#E05206")
F["NG"] = Vs("#008751", WHITE, "#008751")
F["NI"] = Hs("#0067C6", WHITE, "#0067C6") + P([(15, 7), (17.8, 12.4), (12.2, 12.4)], "#C8A84A")
F["NL"] = Hs("#AE1C28", WHITE, "#21468B")
F["NO"] = nordic("#BA0C2F", WHITE, "#00205B", 9.5, 3.6)
F["NP"] = R(0, 0, 30, 20, WHITE) + P([(5.5, 0.4), (24.5, 9.3), (15, 9.3), (26.5, 19.6), (5.5, 19.6)], "#003893") + P(scaled([(5.5, 0.4), (24.5, 9.3), (15, 9.3), (26.5, 19.6), (5.5, 19.6)], 5.5, 10, .8), "#DC143C")
F["NR"] = R(0, 0, 30, 20, "#002B7F") + R(0, 9, 30, 2, "#FFC61E") + star(7, 14.6, 3, WHITE, 12, 1.6)
F["NZ"] = R(0, 0, 30, 20, "#00247D") + jack(0, 0, 15, 10) + "".join(star(x, y, 1.5, "#CC142B") for x, y in [(22.5, 4.5), (18.5, 9.5), (26.5, 8.5), (22.5, 16)])
F["OM"] = Hs(WHITE, "#DB161B", "#008000") + R(0, 0, 9, 20, "#DB161B")
F["PA"] = R(0, 0, 15, 10, WHITE) + R(15, 0, 15, 10, "#DA121A") + R(0, 10, 15, 10, "#072357") + R(15, 10, 15, 10, WHITE) + star(7.5, 5, 3.2, "#072357") + star(22.5, 15, 3.2, "#DA121A")
F["PE"] = Vs("#D91023", WHITE, "#D91023")
F["PG"] = P([(0, 0), (30, 0), (30, 20)], "#CE1126") + P([(0, 0), (0, 20), (30, 20)], BLACK) + C(22, 6, 2.4, "#FCD116") + "".join(star(x, y, 1.3, WHITE) for x, y in [(4, 12), (8, 15), (5, 16.5), (10, 17.5), (7, 13)])
F["PH"] = Hs("#0038A8", "#CE1126") + P([(0, 0), (14, 10), (0, 20)], WHITE) + C(4.5, 10, 2, "#FCD116") + star(8.5, 10, 1.2, "#FCD116")
F["PK"] = R(0, 0, 7.5, 20, WHITE) + R(7.5, 0, 22.5, 20, "#01411C") + C(19, 10, 5, WHITE) + C(20.4, 9, 4.2, "#01411C") + star(22.6, 7.6, 1.8, WHITE)
F["PL"] = Hs(WHITE, "#DC143C")
F["PT"] = R(0, 0, 12, 20, "#006600") + R(12, 0, 18, 20, "#FF0000") + C(12, 10, 3.4, "#FFE000") + R(10.3, 8.2, 3.4, 3.8, WHITE) + R(11.1, 9, 1.8, 2.2, "#FF0000")
F["PW"] = R(0, 0, 30, 20, "#4AADD6") + C(13, 10, 5.6, "#FFDE00")
F["PY"] = Hs("#D52B1E", WHITE, "#0038A8") + C(15, 10, 2.6, "#7D9A3D")
F["QA"] = R(0, 0, 30, 20, "#8A1538") + P([(0, 0), (8, 0)] + [pt for i in range(9) for pt in ((11, 2.222 * i + 1.111), (8, 2.222 * (i + 1)))] + [(0, 20)], WHITE)
F["RO"] = Vs("#002B7F", "#FCD116", "#CE1126")
F["RS"] = Hs("#C6363C", "#0C4076", WHITE) + R(6, 4.5, 5, 7, "#C6363C") + R(6, 4.5, 5, 1.2, "#EDB92E") + R(7, 7, 3, 2.5, WHITE)
F["RU"] = Hs(WHITE, "#0039A6", "#D52B1E")
F["RW"] = R(0, 0, 30, 10, "#00A1DE") + R(0, 10, 30, 5, "#FAD201") + R(0, 15, 30, 5, "#20603D") + C(24, 5, 2.6, "#E5BE01")
F["SA"] = R(0, 0, 30, 20, "#006C35") + R(7, 5.2, 16, 2.4, WHITE) + R(8.5, 9.6, 13, 1.2, WHITE) + R(7, 13.6, 16, 1, WHITE) + R(19, 12.8, 4, .8, WHITE)
F["SB"] = P([(0, 0), (30, 0), (0, 20)], "#0051BA") + P([(30, 0), (30, 20), (0, 20)], "#215B33") + L("M-2,22 L32,-2", "#FCD116", 1.8) + "".join(C(x, y, 1, WHITE) for x, y in [(4, 3), (9, 3), (4, 7), (9, 7), (6.5, 5)])
F["SC"] = P([(0, 20), (0, 0), (10, 0)], "#003F87") + P([(0, 20), (10, 0), (20, 0)], "#FCD856") + P([(0, 20), (20, 0), (30, 0), (30, 6.7)], "#D62828") + P([(0, 20), (30, 6.7), (30, 13.3)], WHITE) + P([(0, 20), (30, 13.3), (30, 20)], "#007A3D")
F["SD"] = Hs("#D21034", WHITE, BLACK) + P([(0, 0), (10, 10), (0, 20)], "#007229")
F["SE"] = nordic("#006AA7", "#FECC00", None, 9.5, 4.6)
F["SG"] = Hs("#EF3340", WHITE) + crescent(6.2, 5, 3.6, WHITE, "#EF3340", 1.2, 3) + "".join(C(x, y, .75, WHITE) for x, y in [(10.5, 3), (12.6, 4.6), (11.8, 7), (9.2, 7), (8.4, 4.6)])
F["SI"] = Hs(WHITE, "#0033A0", "#ED1C24") + R(4, 3, 4.6, 6, "#0033A0") + R(4, 3, 4.6, 1, "#ED1C24") + R(5.8, 5, 1, 2, WHITE)
F["SK"] = Hs(WHITE, "#0B4EA2", "#EE1C25") + R(7, 4.5, 5.5, 7.5, "#EE1C25") + R(9, 6, 1.6, 4.4, WHITE) + R(7.9, 7.4, 3.6, 1.2, WHITE)
F["SL"] = Hs("#1EB53A", WHITE, "#0072C6")
F["SM"] = Hs(WHITE, "#5EB6E4") + C(15, 10, 2.4, "#C8A84A")
F["SN"] = Vs("#00853F", "#FDEF42", "#E31B23") + star(15, 10, 3.2, "#00853F")
F["SO"] = R(0, 0, 30, 20, "#4189DD") + star(15, 10.5, 5.2, WHITE)
F["SR"] = Hs("#377E3F", WHITE, "#B40A2D", WHITE, "#377E3F", w=[2, 1, 4, 1, 2]) + star(15, 10, 3.6, "#ECC81D")
F["ST"] = Hs("#12AD2B", "#FFCE00", "#12AD2B", w=[1, 2, 1]) + P([(0, 0), (9, 10), (0, 20)], "#D21034") + star(17.5, 10, 2, BLACK) + star(23, 10, 2, BLACK)
F["SV"] = Hs("#0F47AF", WHITE, "#0F47AF") + C(15, 10, 2.4, "#C9A800")
F["SY"] = Hs("#CE1126", WHITE, BLACK) + star(11, 10, 2.3, "#007A3D") + star(19, 10, 2.3, "#007A3D")
F["SZ"] = Hs("#3E5EB9", "#FFD900", "#B10C0C", "#FFD900", "#3E5EB9", w=[3, 1, 6, 1, 3]) + '<ellipse cx="15" cy="10" rx="5" ry="2.8" fill="#fff"/>' + A("M10,10 A5,2.8 0 0 0 20,10 Z", BLACK)
F["TD"] = Vs("#002664", "#FECB00", "#C60C30")
F["TG"] = Hs("#006A4E", "#FFCE00", "#006A4E", "#FFCE00", "#006A4E") + R(0, 0, 12, 12, "#D21034") + star(6, 6, 3.6, WHITE)
F["TH"] = Hs("#A51931", WHITE, "#2D2A4A", WHITE, "#A51931", w=[1, 1, 2, 1, 1])
F["TJ"] = Hs("#CC0000", WHITE, "#006600", w=[2, 3, 2]) + R(12.4, 8.4, 5.2, 2, "#F8C300") + R(12.4, 7.2, 1.2, 1.2, "#F8C300") + R(14.4, 6.6, 1.2, 1.8, "#F8C300") + R(16.4, 7.2, 1.2, 1.2, "#F8C300")
F["TM"] = R(0, 0, 30, 20, "#00843D") + R(3, 0, 3.8, 20, "#D22630") + crescent(14.5, 5, 3.2, WHITE, "#00843D", .9, 2.6) + "".join(C(x, y, .6, WHITE) for x, y in [(18, 3.2), (19, 5), (18.4, 7), (16.4, 7.6), (14.6, 7.2)])
F["TN"] = R(0, 0, 30, 20, "#E70013") + C(15, 10, 6.2, WHITE) + C(14.3, 10, 3.9, "#E70013") + C(15.5, 10, 3.2, WHITE) + star(16.6, 10, 1.9, "#E70013")
F["TO"] = R(0, 0, 30, 20, "#C10000") + R(0, 0, 13, 10.5, WHITE) + R(5.2, 1.6, 2.6, 7.3, "#C10000") + R(2.9, 3.9, 7.2, 2.6, "#C10000")
F["TR"] = R(0, 0, 30, 20, "#E30A17") + C(11, 10, 5.2, WHITE) + C(12.3, 10, 4.2, "#E30A17") + star(16.8, 10, 2.4, WHITE)
F["TT"] = R(0, 0, 30, 20, "#CE1126") + L("M-2,-1.5 L32,21.5", WHITE, 7.4) + L("M-2,-1.5 L32,21.5", BLACK, 5)
F["TW"] = R(0, 0, 30, 20, "#FE0000") + R(0, 0, 15, 10, "#000095") + C(7.5, 5, 3.2, WHITE) + C(7.5, 5, 2.4, "#000095") + C(7.5, 5, 1.6, WHITE)
F["TZ"] = R(0, 0, 30, 20, "#00A3DD") + P([(0, 0), (30, 0), (0, 20)], "#1EB53A") + L("M-2,22 L32,-2", "#FCD116", 6.4) + L("M-2,22 L32,-2", BLACK, 4.2)
F["UA"] = Hs("#0057B7", "#FFD700")
F["UG"] = Hs(BLACK, "#FCDC04", "#D90000", BLACK, "#FCDC04", "#D90000") + C(15, 10, 3.8, WHITE) + C(15, 10, 1.7, BLACK)
F["US"] = Hs(*(["#B22234", WHITE] * 6 + ["#B22234"])) + R(0, 0, 12, 10.8, "#3C3B6E") + "".join(C(1.6 + 2.2 * c, 1.7 + 2.1 * r, .55, WHITE) for r in range(4) for c in range(5))
F["UY"] = Hs(*([WHITE, "#0038A8"] * 4 + [WHITE])) + R(0, 0, 11, 9.9, WHITE) + C(5.5, 4.9, 2.6, "#FCD116")
F["UZ"] = R(0, 0, 30, 6.4, "#0099B5") + R(0, 6.4, 30, .7, "#CE1126") + R(0, 7.1, 30, 5.8, WHITE) + R(0, 12.9, 30, .7, "#CE1126") + R(0, 13.6, 30, 6.4, "#1EB53A") + crescent(5, 3.2, 2.4, WHITE, "#0099B5", .8, 2)
F["VC"] = Vs("#0072C6", "#FCD116", "#009E60", w=[1, 2, 1]) + "".join(P([(x, y - 2.2), (x + 1.5, y), (x, y + 2.2), (x - 1.5, y)], "#009E60") for x, y in [(11.5, 7.4), (18.5, 7.4), (15, 12.4)])
F["VE"] = Hs("#FFCC00", "#00247D", "#CF142B") + ring(15, 20, 8, 8, .6, WHITE)
F["VN"] = R(0, 0, 30, 20, "#DA251D") + star(15, 10.5, 6.4, "#FFFF00")
F["VU"] = R(0, 0, 30, 10, "#D21034") + R(0, 10, 30, 10, "#009543") + P([(0, 0), (16, 10), (0, 20)], "#FDCE12") + P([(0, 2.6), (10.8, 10), (0, 17.4)], BLACK) + R(9, 9, 21, 2, "#FDCE12")
F["WS"] = R(0, 0, 30, 20, "#CE1126") + R(0, 0, 15, 10, "#002B7F") + "".join(star(x, y, 1.4, WHITE) for x, y in [(7.5, 2.4), (4.4, 5), (10.6, 5), (6, 8), (9, 8)])
F["YE"] = Hs("#CE1126", WHITE, BLACK)
F["ZA"] = R(0, 0, 30, 10, "#E03C31") + R(0, 10, 30, 10, "#001489") + L("M0,1 L11,10 L0,19 M11,10 L30,10", WHITE, 6.4) + L("M0,1 L11,10 L0,19 M11,10 L30,10", "#007749", 3.4) + P([(0, 2.5), (9, 10), (0, 17.5)], "#FFB612") + P([(0, 4), (7.2, 10), (0, 16)], BLACK)
F["ZM"] = R(0, 0, 30, 20, "#198A00") + R(21, 10, 3, 10, "#EF7D00") + R(24, 10, 3, 10, BLACK) + R(27, 10, 3, 10, "#DE2010") + C(24, 6, 2.2, "#EF7D00")
F["ZW"] = Hs("#319208", "#FFD200", "#DE2010", BLACK, "#DE2010", "#FFD200", "#319208") + P([(0, 0), (13, 10), (0, 20)], BLACK) + P([(0, 1.6), (11, 10), (0, 18.4)], WHITE) + star(3.5, 10, 3, "#DE2010")


def svg(body): return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 30 20" width="{W * 8}" height="{H * 8}">{body}</svg>'

def render(code):
    data = svg(F[code]).encode()
    png = subprocess.run(["rsvg-convert", "-w", str(W), "-h", str(H), "-f", "png"], input=data, capture_output=True, check=True).stdout
    return subprocess.run(["magick", "png:-", "-background", "white", "-flatten", "-depth", "8", "rgb:-"], input=png, capture_output=True, check=True).stdout

def rgb565(rgb):
    out = bytearray()
    for i in range(0, len(rgb), 3):
        v = ((rgb[i] >> 3) << 11) | ((rgb[i + 1] >> 2) << 5) | (rgb[i + 2] >> 3)
        out += struct.pack("<H", v)
    return bytes(out)

def main():
    need = sorted({l.split()[2] for l in open(RANGES) if l.strip() and not l.startswith("#")})
    missing = [c for c in need if c not in F]
    if missing:
        sys.exit(f"no flag drawn for: {missing}")
    codes = sorted(c for c in F if c in need)
    blob = b"FLG1" + struct.pack("<HBB", len(codes), W, H) + b"".join(c.encode() for c in codes)
    px = [render(c) for c in codes]
    for c, p in zip(codes, px):
        assert len(p) == W * H * 3, (c, len(p))
    blob += b"".join(rgb565(p) for p in px)
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else OUT
    open(out, "wb").write(blob)
    print(f"{len(codes)} flags, {len(blob)} bytes -> {os.path.normpath(out)}")
    if "--sheet" in sys.argv:
        sheet = sys.argv[sys.argv.index("--sheet") + 1]
        cols = 14
        os.makedirs("/tmp/flagsheet", exist_ok=True)
        files = []
        for c in codes:
            f = f"/tmp/flagsheet/{c}.png"
            png = subprocess.run(["rsvg-convert", "-w", str(W * 4), "-h", str(H * 4), "-f", "png"], input=svg(F[c]).encode(), capture_output=True, check=True).stdout
            open(f, "wb").write(png); files.append(f)
        labeled = files   # (no captions: ImageMagick may lack a font) - the order is alphabetical by ISO code, 14 per row
        rows = []
        for i in range(0, len(labeled), cols):
            row = f"/tmp/flagsheet/row{i}.png"
            subprocess.run(["magick", *labeled[i:i + cols], "-bordercolor", "#222", "-border", "3", "+append", row], check=True)
            rows.append(row)
        subprocess.run(["magick", *rows, "-background", "#222", "-append", sheet], check=True)
        print("sheet:", sheet)

if __name__ == "__main__":
    main()
