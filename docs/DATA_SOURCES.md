# Data sources and licences

The radar uses these open datasets. The first four are compiled into the firmware by the scripts in `tools/`; the rest are used at run time.

| Data | Source | Licence | Used for |
|---|---|---|---|
| Cities (population > 15 000), countries | GeoNames `cities15000.zip`, `countryInfo.txt` (geonames.org) | CC BY 4.0 — attribution required (Settings > System > Credits) | Offline nearest-city lookup (country + time zone from a position), city search |
| Time-zone rules | tz database (`/usr/share/zoneinfo` on the build machine) | Public domain | POSIX TZ rule of every IANA zone |
| World outline | Natural Earth 1:110m country polygons (naturalearthdata.com) | Public domain | Map picker on the radar |
| Coastlines, land borders, airports | Natural Earth 1:10m `ne_10m_coastline`, `ne_10m_admin_0_boundary_lines_land`, `ne_10m_airports` | Public domain | Map overlay in the radar view |
| Flight routes (origin, destination) | adsbdb.com `/v0/callsign/<callsign>`, looked up when an aircraft is selected | Community service, free; no key | Route line on the aircraft card |
| Take-off time, estimated arrival, delay, progress, flown track, route | **FlightAware AeroAPI v4** (`aeroapi.flightaware.com`), **optional**: the user's own API key, entered under Settings > Flights Data or on the phone page. Calls: `/flights/{ident}`, `/flights/{fa_flight_id}/track`, `/account/usage` (key test) | FlightAware's AeroAPI terms; billed per call to the user's account (a few calls per aircraft selected, 10-minute cache, ceiling of 60 calls an hour in the firmware) | Dep / ETA lines, progress bar and the flown track on the radar |
| Aircraft photo | Planespotters.net public API `/pub/photos/hex/<ICAO hex>` and the thumbnail it names (`t.plnspttrs.net`) | Free; they ask for a descriptive User-Agent with a contact address (the user's e-mail, entered in settings) and credit to the photographer (shown under every photo) | Photo on the aircraft card (progressive JPEG, decoded with stb_image) |
| Country of registration | ICAO Annex 10, Vol. III, Part I, Chapter 9, Table 9-1 (24-bit aircraft address blocks by state), parsed by `tools/build_icao_ranges.py` into `tools/icao_ranges.txt` and `firmware/src/icao_ranges.inc`. Hong Kong (inside China's block), Taiwan and Montenegro are real-world additions. Checked against 100+ live aircraft and adsbdb.com owner countries: no disagreement in the state of registry | Facts (public ICAO allocation table) | Country name + flag on the aircraft card, for every aircraft with a normal address |
| National flags | Drawn for this project by `tools/build_flags.py` (27x18 px, simplified: stripes, crosses, a few shapes) into `firmware/data/flags.bin` | Part of this project (GPL-3.0-or-later) | Flag on the aircraft card |
| JPEG decoding | stb_image v2.30 (Sean Barrett), JPEG only, `firmware/src/third_party/stb_image.h` | Public domain / MIT (dual) | Planespotters photos are progressive JPEGs, which LovyanGFX's decoder cannot read |
| Airline names | OpenFlights `airlines.dat` (openflights.org) | Open Database License — attribution, share-alike for the database | Airline name from a callsign such as ELY382 |
| Aircraft positions, readable model, registration | airplanes.live API (`/v2/point`) | Free for non-commercial use, please keep the polling gentle | The radar itself |
| Map library of the phone page | Leaflet 1.9.4 (leafletjs.com), embedded unmodified | BSD-2-Clause | Phone/PC map picker |
| Map tiles of the phone page | OpenStreetMap (tile.openstreetmap.org), loaded by the phone's browser | ODbL + tile usage policy; "© OpenStreetMap contributors" shown | Phone/PC map picker (needs internet on that device) |

## Rebuilding the embedded data
Download each file into its own folder, then:

    python3 -I tools/build_geo.py <cities15000.txt> <countryInfo.txt> <ne_110m_admin_0_countries.geojson> firmware/data/geo.bin firmware/data/world.bin
    python3 -I tools/build_airlines.py <airlines.dat> firmware/data/airlines.bin
    python3 -I tools/build_map.py <ne_10m_coastline.geojson> <ne_10m_admin_0_boundary_lines_land.geojson> <ne_10m_airports.geojson> firmware/data/mapdata.bin

`firmware/data/page.html` (the web interface) is gzipped automatically at build time by `tools/pio_gzip.py`.
`firmware/data/leaflet.js.gz` / `leaflet.css.gz` are Leaflet 1.9.4 `dist/` files compressed with `gzip -9`.

## Not provided by airplanes.live (checked 2026-10-09 and 2026-10-10)
Aircraft photos, flight routes (origin / destination), take-off time and estimated arrival are not in the position feed (fields seen: position, altitude, speed, heading, squawk, type, registration, accuracy and navigation data). Routes come from adsbdb.com (verified: BAW117 → London LHR to New York JFK; it has airports, no times). Times, track and a better route come from FlightAware AeroAPI when the user enables it; photos from Planespotters.net.

## Flight details: how it behaves
Nothing is requested until an aircraft is selected and stays selected for 1.5 s. Results are kept for 10 minutes (photo "none" for an hour), so selecting the same aircraft again costs no call. Costs on the FlightAware side: one `/flights` call and one `/track` call per aircraft (the track can be switched off), at most 60 calls an hour whatever is tapped. The key is stored in the radar's flash like the Wi-Fi password and is never shown again or sent anywhere but aeroapi.flightaware.com. Take-off = wheels up (`actual_off`, else `estimated_off`, `scheduled_off`); ETA = wheels down (`estimated_on`, else `scheduled_on`); "Landed" once `actual_on` exists. The firmware was written against FlightAware's documented AeroAPI v4 response shapes and tested with a stand-in server (`tools/mock_aeroapi.py`); a real answer may differ in small ways, and the Flights Data page shows the last result or error text.
