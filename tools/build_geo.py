#!/usr/bin/env python3
"""Builds firmware/data/geo.bin (cities -> country + time zone) and firmware/data/world.bin (world outline for the on-device map).

Inputs (download them yourself, each into its own folder; see docs/DATA_SOURCES.md):
  GeoNames cities15000.txt   (CC-BY 4.0, geonames.org)      -> cities, their country and IANA time zone
  GeoNames countryInfo.txt   (CC-BY 4.0)                    -> country names
  Natural Earth ne_110m_admin_0_countries.geojson (public domain) -> world outline
  /usr/share/zoneinfo        (tz database, public domain)   -> POSIX TZ rule of every IANA zone (the footer of each TZif file)

Usage: python3 -I tools/build_geo.py <cities15000.txt> <countryInfo.txt> <ne_110m_admin_0_countries.geojson> <out geo.bin> <out world.bin>
"""
import json, struct, sys, os

def posix_tz(zone):
    """POSIX TZ string of an IANA zone: the last line of its TZif file."""
    path = os.path.join('/usr/share/zoneinfo', zone)
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return None
    if not data.startswith(b'TZif'):
        return None
    tail = data.rstrip(b'\n').split(b'\n')[-1].decode('ascii', 'replace')
    return tail or None

def ascii_name(s):
    return s.encode('ascii', 'replace').decode('ascii').replace('?', '')[:30]

def main(cities_txt, country_txt, geojson, out_geo, out_world):
    countries = {}   # cc -> name
    for line in open(country_txt, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        f = line.rstrip('\n').split('\t')
        countries[f[0]] = ascii_name(f[4])

    rows = []   # population desc order: the biggest places come first in search results
    zones = {}
    for line in open(cities_txt, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        name, lat, lon, cc, pop, tz = ascii_name(f[2] or f[1]), float(f[4]), float(f[5]), f[8], int(f[14] or 0), f[17]
        if not name or cc not in countries:
            continue
        rows.append((pop, name, lat, lon, cc, tz))
    rows.sort(key=lambda r: -r[0])

    missing = set()
    for r in rows:
        tz = r[5]
        if tz in zones:
            continue
        p = posix_tz(tz)
        if p is None:
            missing.add(tz)
            continue
        zones[tz] = p
    rows = [r for r in rows if r[5] in zones]
    if missing:
        print('zones without a local tz file (cities dropped):', sorted(missing))

    zone_names = sorted(zones)
    zone_idx = {z: i for i, z in enumerate(zone_names)}
    cc_list = sorted({r[4] for r in rows} | set(countries))
    cc_idx = {c: i for i, c in enumerate(cc_list)}

    pool = bytearray()
    def intern(s):
        off = len(pool)
        pool.extend(s.encode('ascii') + b'\0')
        return off

    zone_recs = b''.join(struct.pack('<II', intern(z), intern(zones[z])) for z in zone_names)
    country_recs = b''.join(struct.pack('<2sHI', c.encode('ascii'), 0, intern(countries.get(c, c))) for c in cc_list)
    city_recs = bytearray()
    names = {}
    for pop, name, lat, lon, cc, tz in rows:
        key = name
        off = names.get(key)
        if off is None:
            off = names[key] = intern(name)
        city_recs += struct.pack('<hhBBHI', round(lat * 100), round(lon * 100), cc_idx[cc], 0, zone_idx[tz], off)

    header_size = 32
    off_zones = header_size
    off_countries = off_zones + len(zone_recs)
    off_cities = off_countries + len(country_recs)
    off_pool = off_cities + len(city_recs)
    header = struct.pack('<4sHHHHIIIII', b'GEO1', 1, len(zone_names), len(cc_list), 0, len(rows), off_zones, off_countries, off_cities, off_pool)
    header = header.ljust(header_size, b'\0')
    with open(out_geo, 'wb') as f:
        f.write(header + zone_recs + country_recs + city_recs + pool)
    print('geo.bin: %d cities, %d zones, %d countries, %d bytes' % (len(rows), len(zone_names), len(cc_list), os.path.getsize(out_geo)))

    # ---- world outline: outer rings of every country polygon, 0.01 degree units
    gj = json.load(open(geojson, encoding='utf-8'))
    rings = []
    for feat in gj['features']:
        g = feat['geometry']
        polys = g['coordinates'] if g['type'] == 'MultiPolygon' else [g['coordinates']]
        for poly in polys:
            ring = poly[0]
            pts = [(round(x * 100), round(y * 100)) for x, y in ring]
            clean = [pts[0]]
            for p in pts[1:]:
                if p != clean[-1]:
                    clean.append(p)
            if len(clean) >= 3:
                rings.append(clean)
    table = b''
    points = b''
    n = 0
    for r in rings:
        table += struct.pack('<IHH', n, len(r), 0)
        points += b''.join(struct.pack('<hh', x, y) for x, y in r)
        n += len(r)
    with open(out_world, 'wb') as f:
        f.write(struct.pack('<4sIII', b'MAP1', len(rings), n, 0) + table + points)
    print('world.bin: %d rings, %d points, %d bytes' % (len(rings), n, os.path.getsize(out_world)))

if __name__ == '__main__':
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    main(*sys.argv[1:])
