#!/usr/bin/env python3
"""Builds firmware/data/airlines.bin: airline ICAO designator (the first three letters of a callsign such as ELY382) -> airline name.

Input: OpenFlights airlines.dat (Open Database License, openflights.org), downloaded into its own folder.
Usage: python3 -I tools/build_airlines.py <airlines.dat> <out airlines.bin>
Active airlines win when several share a code. EXTRA adds airlines that the OpenFlights file lacks.
"""
import csv, struct, sys

# ICAO designator -> name for airlines missing from (or wrong in) the OpenFlights file; add more as they turn up
EXTRA = {
    'WMT': 'Wizz Air Malta', 'WZZ': 'Wizz Air', 'EJU': 'easyJet Europe', 'EZY': 'easyJet', 'EZS': 'easyJet Switzerland',
    'VLG': 'Vueling', 'TVF': 'Transavia France', 'TRA': 'Transavia', 'NSZ': 'Norwegian Air Sweden', 'NOZ': 'Norwegian',
    'SKY': 'Skyways', 'ISR': 'Israir', 'AIZ': 'Arkia Israeli Airlines', 'ELY': 'El Al', 'RJA': 'Royal Jordanian',
    'LDM': 'Laudamotion', 'RYS': 'Ryanair Sun', 'RUK': 'Ryanair UK', 'MAY': 'Malta Air', 'CYP': 'Cyprus Airways',
    'AEE': 'Aegean Airlines', 'SEH': 'Sky Express', 'PGT': 'Pegasus Airlines', 'THY': 'Turkish Airlines', 'ABY': 'Air Arabia',
    'FDB': 'flydubai', 'UAE': 'Emirates', 'ETD': 'Etihad Airways', 'QTR': 'Qatar Airways', 'SVA': 'Saudia', 'MSR': 'EgyptAir',
    'FPO': 'Europe Airpost', 'CKS': 'Kalitta Air', 'GTI': 'Atlas Air', 'UPS': 'UPS Airlines', 'FDX': 'FedEx Express',
    'DHK': 'DHL Air UK', 'BCS': 'European Air Transport', 'ICL': 'CAL Cargo Airlines', 'ELL': 'Elit Air', 'RCH': 'US Air Force (Reach)',
}

def main(src, out):
    best = {}
    for r in csv.reader(open(src, encoding='utf-8')):
        if len(r) < 8:
            continue
        name, icao, active = r[1].strip(), r[4].strip().upper(), r[7] == 'Y'
        if len(icao) != 3 or not icao.isascii() or not icao.isalnum() or not name or name == 'Unknown':
            continue
        name = name.encode('ascii', 'replace').decode('ascii').replace('?', '')[:34]
        if icao not in best or (active and not best[icao][1]):
            best[icao] = (name, active)
    for k, v in EXTRA.items():
        best[k] = (v, True)
    codes = sorted(best)
    pool = bytearray()
    recs = b''
    for c in codes:
        off = len(pool)
        pool += best[c][0].encode('ascii') + b'\0'
        recs += struct.pack('<4sI', c.encode('ascii'), off)
    with open(out, 'wb') as f:
        f.write(struct.pack('<4sI', b'ALN1', len(codes)) + recs + pool)
    print('airlines.bin: %d airlines, %d bytes' % (len(codes), 8 + len(recs) + len(pool)))
    for c in ('ELY', 'ISR', 'WMT', 'RYR', 'DLH', 'BAW', 'THY', 'AEE', 'UAL', 'DAL', 'AAL'):
        print(' ', c, best.get(c, ('--',))[0])

if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
