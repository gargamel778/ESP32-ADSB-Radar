# Web interface

The radar serves a single-page UI at `/` and a small JSON API. Everything except the page, the map library and the phones' connectivity probes needs the **PIN cookie**.
There is no HTTPS (see [SECURITY.md](SECURITY.md)).

## Sign in
`POST /api/login` with `{"pin":"123456"}` → `200 {"ok":true}` and `Set-Cookie: sid=<token>; Path=/; SameSite=Strict`, or `403 {"error":"Wrong PIN"}`.
Five wrong PINs lock sign-in for a minute (`429`). The token is new at every boot. All other calls send the cookie; without it they answer `401 {"error":"login"}`.

## Endpoints
| Method and path | Purpose |
|---|---|
| `GET /` | The page (gzip) |
| `GET /api/state` | Everything: version, firmware slot and state, uptime, free heap, Wi-Fi, radar (aircraft count, emergencies), night, flight-detail status, health, units, colours, all settings (`s`), SD card (`sd`) |
| `POST /api/set` | Change settings: a JSON object with any of the keys below; unknown keys are ignored, values are clamped |
| `GET /api/wifi/scan[?start=1]` | Networks seen (`start` begins a scan; the list is `nets: [{ssid, rssi, secure}]`) |
| `POST /api/wifi/join` | `{"ssid","pass"}`: try a network, with rollback; poll `GET /api/wifi/status` |
| `POST /api/restart` | Restart |
| `POST /api/ota/upload` | Firmware upload, `multipart/form-data`: fields `size` and optional `sha256` before the file part `file` |
| `POST /api/ota/url` | `{"url","sha256"}`: the radar downloads and installs; poll `GET /api/ota/status` |
| `POST /api/ota/revert` | Boot the previous firmware (`409` if there is none) |
| `POST /api/fa/test` | Check the FlightAware key (result in `state.fa.test`) |
| `GET /api/geo/search?q=`, `/api/geo/lookup?lat=&lon=`, `/api/geo/zones`, `/api/geo/countries` | Offline place data |
| `GET /leaflet.js`, `/leaflet.css` | Map library (gzip) |

`POST /api/set` keys: `unitsAuto`, `dist` (0 nm, 1 km, 2 mi), `alt` (0 ft, 1 m, 2 FL), `spd` (0 kt, 1 km/h, 2 mph), `clock24`, `rangeStep` (0..6), `trail` (15, 25, 40, 60), `theme` (0..2),
`pal` (five swatch indices: radar, titles, callsign, value, lines), `brightness` (day, 10..100), `nightBright` (2..100), `nightMode` (0 off, 1 auto, 2 on), `showDiag`, `mapOverlay`,
`faEnabled`, `faKey` (string; empty removes it), `faEmail`, `faPhoto`, `faTrack`, `sdLog`, `logSecs` (10, 30, 60), `sdEject` (true eject, false mount), `paused`, `country` (ISO code or `"auto"`),
`zone` (IANA name or `"auto"`), `loc` (`{lat, lon, place}`).

The key, the Wi-Fi password and the PIN are never part of any answer.

## The demo server
`python3 tools/mock_radar.py` serves the same page and API with demo data (no radar needed), for working on the page and for screenshots.
