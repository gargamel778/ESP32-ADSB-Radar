#!/usr/bin/env python3
"""Builds firmware/data/mapdata.bin: coastlines, land borders and airports for the radar's map overlay.

Inputs (Natural Earth 1:10m, public domain, each downloaded into its own folder):
  ne_10m_coastline.geojson, ne_10m_admin_0_boundary_lines_land.geojson, ne_10m_airports.geojson
Usage: python3 -I tools/build_map.py <coastline.geojson> <boundaries.geojson> <airports.geojson> <out mapdata.bin>

Layout (little endian): 32-byte header "MAP2", nChunks, nAirports, offChunks, offPoints, offAirports
  chunk (16 B): int16 minLon, minLat, maxLon, maxLat (0.01 degree, rounded outwards); u8 kind (0 coast, 1 border); u8 count; u16 pad; u32 offset into the point blob
  points: first point int32 lon, int32 lat (1/512 degree), then count-1 pairs of int16 deltas
  airport (16 B): int32 lon, int32 lat (1/512 degree); char iata[4]; u8 type (0 small, 1 mid, 2 major); 3 pad
"""
import json, math, struct, sys

UNIT = 512
MAXPTS = 100          # points per chunk (lines are cut into chunks so the radar can skip the ones outside its circle)
MAXDELTA = 30000      # an int16 delta must fit; longer jumps start a new chunk

def lines_of(geo):
    t, c = geo['type'], geo['coordinates']
    if t == 'LineString': return [c]
    if t == 'MultiLineString': return c
    if t == 'Polygon': return c
    if t == 'MultiPolygon': return [r for p in c for r in p]
    return []

def chunks_of(line):
    q = []
    for lon, lat in line:
        p = (round(lon * UNIT), round(lat * UNIT))
        if not q or p != q[-1]:
            q.append(p)
    out, cur = [], []
    for p in q:
        if cur and (len(cur) >= MAXPTS or abs(p[0] - cur[-1][0]) > MAXDELTA or abs(p[1] - cur[-1][1]) > MAXDELTA):
            if len(cur) >= 2 and abs(p[0] - cur[-1][0]) <= MAXDELTA and abs(p[1] - cur[-1][1]) <= MAXDELTA:
                out.append(cur); cur = [cur[-1]]        # overlap one point so the pieces join
            else:
                if len(cur) >= 2: out.append(cur)
                cur = []                                 # a jump too long to encode: start afresh
        cur.append(p)
    if len(cur) >= 2: out.append(cur)
    return out

def main(coast, bounds, airports, out):
    chunks, blob = [], bytearray()
    total = 0
    for kind, path in ((0, coast), (1, bounds)):
        for feat in json.load(open(path, encoding='utf-8'))['features']:
            for line in lines_of(feat['geometry']):
                for ch in chunks_of(line):
                    xs = [p[0] for p in ch]; ys = [p[1] for p in ch]
                    bb = (math.floor(min(xs) / UNIT * 100), math.floor(min(ys) / UNIT * 100), math.ceil(max(xs) / UNIT * 100), math.ceil(max(ys) / UNIT * 100))
                    off = len(blob)
                    blob += struct.pack('<ii', ch[0][0], ch[0][1])
                    for a, b in zip(ch, ch[1:]):
                        blob += struct.pack('<hh', b[0] - a[0], b[1] - a[1])
                    chunks.append(struct.pack('<hhhhBBHI', *bb, kind, len(ch), 0, off))
                    total += len(ch)
    ap = bytearray()
    n_ap = 0
    for feat in json.load(open(airports, encoding='utf-8'))['features']:
        pr = feat['properties']
        iata = (pr.get('iata_code') or '').strip()[:3]
        if len(iata) != 3: continue
        t = (pr.get('type') or '')
        typ = 2 if 'major' in t else 1 if 'mid' in t else 0
        lon, lat = feat['geometry']['coordinates'][:2]
        ap += struct.pack('<ii4sB3x', round(lon * UNIT), round(lat * UNIT), iata.encode('ascii', 'replace'), typ)
        n_ap += 1
    off_chunks = 32
    off_points = off_chunks + 16 * len(chunks)
    off_air = off_points + len(blob)
    header = struct.pack('<4sIIIII', b'MAP2', len(chunks), n_ap, off_chunks, off_points, off_air).ljust(32, b'\0')
    with open(out, 'wb') as f:
        f.write(header + b''.join(chunks) + blob + ap)
    print('mapdata.bin: %d chunks, %d points, %d airports, %d bytes' % (len(chunks), total, n_ap, 32 + 16 * len(chunks) + len(blob) + len(ap)))

if __name__ == '__main__':
    if len(sys.argv) != 5: sys.exit(__doc__)
    main(*sys.argv[1:])
