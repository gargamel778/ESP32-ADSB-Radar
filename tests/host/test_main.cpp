// Host-side tests of the firmware's pure logic and of its embedded data, run on the real shipped files. Build and run with tests/run_all.sh.
#include <Arduino.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include "../../firmware/src/solar.h"
#include "../../firmware/src/isotime.h"
#include "../../firmware/src/imagecheck.h"
#include "../../firmware/src/units.h"
#include "../../firmware/src/palette.h"
#include "../../firmware/src/icao.h"
#include "../../firmware/src/airline.h"
#include "../../firmware/src/geo.h"
#include "../../firmware/src/mapdata.h"

SerialShim Serial;
static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, tol) do { gChecks++; const double _a = (a), _b = (b); if (!(fabs(_a - _b) <= (tol))) { gFails++; printf("  FAIL %s:%d: %s = %.4f, expected %.4f +- %.4f\n", __FILE__, __LINE__, #a, _a, _b, (double)(tol)); } } while (0)
#define SECTION(name) printf("%s\n", name)

static std::string readFile(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static uint32_t rnd() { static uint64_t s = 88172645463325252ull; s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }

// ---------------------------------------------------------------- sun: compared with the NOAA solar calculator's formulation (different series, same physics)
static double noaaElevation(double lat, double lon, time_t utc) {
  const double rad = M_PI / 180.0, jd = utc / 86400.0 + 2440587.5, T = (jd - 2451545.0) / 36525.0;
  const double L0 = fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0), M = (357.52911 + T * (35999.05029 - 0.0001537 * T)) * rad;
  const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
  const double C = sin(M) * (1.914602 - T * (0.004817 + 0.000014 * T)) + sin(2 * M) * (0.019993 - 0.000101 * T) + sin(3 * M) * 0.000289;
  const double omega = (125.04 - 1934.136 * T) * rad, lambda = (L0 + C - 0.00569 - 0.00478 * sin(omega)) * rad;
  const double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0, eps = (eps0 + 0.00256 * cos(omega)) * rad;
  const double decl = asin(sin(eps) * sin(lambda)), y = tan(eps / 2) * tan(eps / 2), l0 = L0 * rad;
  const double eqt = 4.0 / rad * (y * sin(2 * l0) - 2 * e * sin(M) + 4 * e * y * sin(M) * cos(2 * l0) - 0.5 * y * y * sin(4 * l0) - 1.25 * e * e * sin(2 * M));
  const double minutes = fmod(utc, 86400.0) / 60.0, tst = fmod(minutes + eqt + 4.0 * lon + 1440.0 * 10, 1440.0), ha = (tst / 4.0 - 180.0) * rad;
  return asin(sin(lat * rad) * sin(decl) + cos(lat * rad) * cos(decl) * cos(ha)) / rad;
}

static void testSolar() {
  SECTION("solar elevation (night mode)");
  double worst = 0;
  for (int i = 0; i < 5000; i++) {
    const double lat = (rnd() % 14000) / 100.0 - 70.0, lon = (rnd() % 36000) / 100.0 - 180.0;
    const time_t t = 1577836800 + (time_t)(rnd() % (86400u * 365 * 10));   // 2020 .. 2030
    worst = std::max(worst, fabs(solar::elevationDeg(lat, lon, t) - noaaElevation(lat, lon, t)));
  }
  printf("  worst difference from the NOAA formulation over 5000 random times and places: %.3f degrees\n", worst);
  CHECK(worst < 0.3);
  // physical sanity: June solstice solar noon on the Tropic of Cancer is overhead; midnight is far below the horizon; polar day / night
  const time_t solstice = isotime::parse("2026-06-21T12:00:00Z");
  CHECK_NEAR(solar::elevationDeg(23.44, 0, solstice - 120), 90.0, 1.0);
  CHECK_NEAR(solar::elevationDeg(23.44, 0, solstice + 43200), -(90.0 - 2 * 23.44), 1.0);   // lower culmination: lat + dec - 90
  CHECK(solar::elevationDeg(80, 0, isotime::parse("2026-06-21T00:00:00Z")) > 5);     // midnight sun
  CHECK(solar::elevationDeg(80, 0, isotime::parse("2026-12-21T12:00:00Z")) < -5);    // polar night
  // London, 2026-03-20 (equinox): sun rises near 06:00 UTC, so it is below the horizon at 04:00 and well above at 09:00
  CHECK(solar::elevationDeg(51.5, -0.1, isotime::parse("2026-03-20T04:00:00Z")) < -5);
  CHECK(solar::elevationDeg(51.5, -0.1, isotime::parse("2026-03-20T09:00:00Z")) > 15);
}

static void testIsoTime() {
  SECTION("ISO 8601 time parsing (FlightAware times)");
  int bad = 0;
  for (int i = 0; i < 20000; i++) {
    const time_t t = (time_t)(rnd() % 4102444800u);   // 1970 .. 2100
    struct tm g; gmtime_r(&t, &g);
    char s[32]; snprintf(s, sizeof(s), "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    bad += isotime::parse(s) != t;
  }
  CHECK(bad == 0);
  CHECK(isotime::parse("2024-02-29T23:59:59Z") == 1709251199);   // leap day
  CHECK(isotime::parse("2000-03-01T00:00:00Z") == 951868800);
  CHECK(isotime::parse("") == 0 && isotime::parse(nullptr) == 0 && isotime::parse("not a time") == 0);
  CHECK(isotime::parse("2026-13-01T00:00:00Z") == 0 && isotime::parse("2026-00-10T00:00:00Z") == 0 && isotime::parse("2026-10-10T25:00:00Z") == 0);
}

// ---------------------------------------------------------------- firmware image check (OTA): the real build, then damaged copies
static void testImageCheck(const std::string &fwPath) {
  SECTION("firmware image verification (OTA)");
  const std::string fw = readFile(fwPath);
  if (fw.size() < 100000) { printf("  (skipped: no firmware image at %s: build it first)\n", fwPath.c_str()); return; }
  std::vector<uint8_t> b(fw.begin(), fw.end());
  char err[96];
  CHECK(imagecheck::verify(b.data(), b.size(), err, sizeof(err)));
  const uint32_t sizes[] = {0, 10, 31, 63, 64, 1000, 100000, (uint32_t)b.size() - 33, (uint32_t)b.size() - 1};
  for (uint32_t n : sizes) CHECK(!imagecheck::verify(b.data(), n, err, sizeof(err)));   // truncated
  int missed = 0;
  for (int i = 0; i < 300; i++) {   // one flipped bit anywhere in the image must be caught
    std::vector<uint8_t> c = b;
    const size_t pos = rnd() % (c.size() - 0), bit = rnd() % 8;
    c[pos] ^= 1 << bit;
    const bool ok = imagecheck::verify(c.data(), c.size(), err, sizeof(err));
    // the only bytes the check cannot cover are trailing bytes after the appended hash (none in a normal build)
    missed += ok;
  }
  CHECK(missed == 0);
  std::vector<uint8_t> w = b; w[0] = 0x7f; CHECK(!imagecheck::verify(w.data(), w.size(), err, sizeof(err)));            // not an image
  w = b; w[12] = 0; CHECK(!imagecheck::verify(w.data(), w.size(), err, sizeof(err)) && strstr(err, "chip"));              // wrong chip
  w = b; w[1] = 0; CHECK(!imagecheck::verify(w.data(), w.size(), err, sizeof(err)));                                      // no segments
  std::vector<uint8_t> junk(300000); for (auto &x : junk) x = (uint8_t)rnd(); CHECK(!imagecheck::verify(junk.data(), junk.size(), err, sizeof(err)));
}

// ---------------------------------------------------------------- ICAO address -> country, flags
struct Rng { uint32_t lo, hi; std::string iso; };
static std::vector<Rng> loadRanges(const std::string &root) {
  std::vector<Rng> v;
  std::istringstream in(readFile(root + "/tools/icao_ranges.txt"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    unsigned a, b; char c[8];
    if (sscanf(line.c_str(), "%x %x %7s", &a, &b, c) == 3) v.push_back({a, b, c});
  }
  return v;
}
static void testIcaoAndFlags(const std::string &root) {
  SECTION("country of registration (ICAO address blocks) and flags");
  const auto R = loadRanges(root);
  CHECK(R.size() > 150);
  bool sorted = true;
  for (size_t i = 1; i < R.size(); i++) sorted &= R[i].lo > R[i - 1].hi;
  CHECK(sorted);
  int wrong = 0;
  char hex[8];
  for (const Rng &r : R) {   // both ends and the middle of every block
    for (uint32_t v : {r.lo, r.hi, r.lo + (r.hi - r.lo) / 2}) { snprintf(hex, sizeof(hex), "%06x", v); const char *c = icao::country(hex); wrong += !c || r.iso != c; }
    if (r.lo > 0) { snprintf(hex, sizeof(hex), "%06X", r.lo - 1); const char *c = icao::country(hex); bool inOther = false; for (const Rng &o : R) inOther |= o.lo <= r.lo - 1 && r.lo - 1 <= o.hi; wrong += (c != nullptr) != inOther; }
  }
  CHECK(wrong == 0);
  int mism = 0;
  for (int i = 0; i < 200000; i++) {
    const uint32_t v = rnd() & 0xFFFFFF; snprintf(hex, sizeof(hex), "%06x", v);
    const char *exp = nullptr; for (const Rng &r : R) if (r.lo <= v && v <= r.hi) { exp = r.iso.c_str(); break; }
    const char *got = icao::country(hex);
    mism += (exp == nullptr) != (got == nullptr) || (exp && strcmp(exp, got));
  }
  CHECK(mism == 0);
  CHECK(!icao::country(nullptr) && !icao::country("") && !icao::country("~1234AB") && !icao::country("12345") && !icao::country("GGGGGG") && !icao::country("1234567"));
  // well-known registrations
  CHECK(!strcmp(icao::country("4CA7B7"), "IE")); CHECK(!strcmp(icao::country("A12345"), "US")); CHECK(!strcmp(icao::country("400000"), "GB"));
  CHECK(!strcmp(icao::country("738123"), "IL")); CHECK(!strcmp(icao::country("0C216C"), "PA")); CHECK(!strcmp(icao::country("0C6062"), "TT"));
  CHECK(!strcmp(icao::country("896123"), "AE")); CHECK(!strcmp(icao::country("3C6444"), "DE")); CHECK(!strcmp(icao::country("780ABC"), "CN")); CHECK(!strcmp(icao::country("78A123"), "HK"));
  // every country the table can return has a flag, and the flag drawings are real pictures
  std::map<std::string, int> codes; for (const Rng &r : R) codes[r.iso]++;
  int noFlag = 0, blank = 0;
  for (auto &kv : codes) {
    const uint16_t *px = icao::flagPixels(kv.first.c_str());
    if (!px) { noFlag++; printf("  no flag for %s\n", kv.first.c_str()); continue; }
    int distinct = 0; uint16_t seen[8] = {0};
    for (int i = 0; i < icao::FLAG_W * icao::FLAG_H && distinct < 8; i++) { bool f = false; for (int k = 0; k < distinct; k++) f |= seen[k] == px[i]; if (!f) seen[distinct++] = px[i]; }
    blank += distinct < 2;
  }
  CHECK(noFlag == 0); CHECK(blank == 0);
  CHECK(!icao::flagPixels("ZZ") && !icao::flagPixels(nullptr));
  // a few flags checked pixel by pixel (RGB565 little-endian, 27x18)
  auto at = [&](const char *c, int x, int y) { const uint16_t v = icao::flagPixels(c)[y * icao::FLAG_W + x]; return std::array<int, 3>{((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31}; };
  auto jp = at("JP", 13, 9); CHECK(jp[0] > 150 && jp[1] < 60 && jp[2] < 90);        // red disc
  auto jp2 = at("JP", 1, 1); CHECK(jp2[0] > 230 && jp2[1] > 230 && jp2[2] > 230);   // white field
  auto de1 = at("DE", 13, 1), de2 = at("DE", 13, 9), de3 = at("DE", 13, 16); CHECK(de1[0] < 40 && de2[0] > 180 && de2[1] < 40 && de3[0] > 220 && de3[1] > 150 && de3[2] < 60);
  auto fr1 = at("FR", 2, 9), fr3 = at("FR", 24, 9); CHECK(fr1[2] > 120 && fr1[0] < 40 && fr3[0] > 200 && fr3[2] < 90);
  auto it1 = at("IT", 2, 9), it3 = at("IT", 24, 9); CHECK(it1[1] > 100 && it1[0] < 40 && it3[0] > 180 && it3[1] < 70);
  int blue = 0; for (int y = 0; y < 8; y++) for (int x = 0; x < 9; x++) { auto p = at("US", x, y); blue += p[2] > 80 && p[0] < 100 && p[1] < 100; }
  CHECK(blue > 30);                                                                  // blue canton (with the stars' white dots in it)
}

// ---------------------------------------------------------------- the other embedded datasets
static void testAirlinesAndGeo() {
  SECTION("airline names, places, time zones");
  CHECK(!strcmp(airline::forCallsign("BAW123"), "British Airways"));
  CHECK(airline::forCallsign("ELY382") && !strcmp(airline::forCallsign("ELY382"), "El Al"));
  CHECK(airline::forCallsign("DLH400") != nullptr);
  CHECK(!airline::forCallsign("N123AB") && !airline::forCallsign("") && !airline::forCallsign(nullptr) && !airline::forCallsign("BAW") && !airline::forCallsign("baw123") && !airline::forCallsign("BAWX23"));
  int known = 0; char cs[8];
  for (int a = 0; a < 26; a++) for (int b = 0; b < 26; b++) for (int c = 0; c < 26; c++) { snprintf(cs, sizeof(cs), "%c%c%c1", 'A' + a, 'A' + b, 'A' + c); known += airline::forCallsign(cs) != nullptr; }
  CHECK(known > 3000 && known < 17576);   // thousands of airlines, but not every three letters

  CHECK(geo::begin());
  CHECK(geo::cityCount() > 20000 && geo::countryCount() > 200 && geo::zoneCount() > 300);
  struct Case { double lat, lon; const char *cc, *zone; } cases[] = {
      {51.47, -0.45, "GB", "Europe/London"}, {40.64, -73.78, "US", "America/New_York"}, {35.55, 139.78, "JP", "Asia/Tokyo"}, {-33.94, 151.18, "AU", "Australia/Sydney"},
      {48.85, 2.35, "FR", "Europe/Paris"}, {52.52, 13.40, "DE", "Europe/Berlin"}, {-23.55, -46.63, "BR", "America/Sao_Paulo"}, {25.25, 55.36, "AE", "Asia/Dubai"},
      {1.35, 103.82, "SG", "Asia/Singapore"}, {37.62, -122.38, "US", "America/Los_Angeles"}, {55.75, 37.62, "RU", "Europe/Moscow"}, {-26.14, 28.24, "ZA", "Africa/Johannesburg"}};
  for (const Case &c : cases) {
    geo::Place p; float d = 0;
    const bool ok = geo::nearest(c.lat, c.lon, p, &d);
    CHECK(ok && !strcmp(p.cc, c.cc) && !strcmp(geo::zoneName(p.zone), c.zone) && d < 60);
    if (!ok || strcmp(p.cc, c.cc) || strcmp(geo::zoneName(p.zone), c.zone)) printf("    at %.2f,%.2f: got %s %s (%s)\n", c.lat, c.lon, ok ? p.cc : "?", ok ? geo::zoneName(p.zone) : "?", ok ? p.name : "?");
  }
  geo::Place res[8];
  int n = geo::search("paris", res, 8); bool paris = false; for (int i = 0; i < n; i++) paris |= !strcmp(res[i].cc, "FR");
  CHECK(n > 0 && paris);
  CHECK(geo::search("zzzzqqqq", res, 8) == 0);
  CHECK(geo::countryByCode("GB") >= 0 && !strcmp(geo::countryName(geo::countryByCode("GB")), "United Kingdom") && geo::countryByCode("XX") < 0);
  const int z = geo::zoneByPosix("GMT0BST,M3.5.0/1,M10.5.0");   // the first-boot default time zone rule (src/config.h): London's, shared by a few other zones
  CHECK(z >= 0 && !strcmp(geo::zonePosix(z), "GMT0BST,M3.5.0/1,M10.5.0") && !strcmp(geo::zonePosix(geo::zoneByName("Europe/London")), "GMT0BST,M3.5.0/1,M10.5.0"));
  // every zone's rule must be understood by the C library, and give plausible offsets in winter and summer
  int implausible = 0;
  const time_t jan = isotime::parse("2026-01-15T12:00:00Z"), jul = isotime::parse("2026-07-15T12:00:00Z");
  for (int i = 0; i < geo::zoneCount(); i++) {
    setenv("TZ", geo::zonePosix(i), 1); tzset();
    struct tm a, b; localtime_r(&jan, &a); localtime_r(&jul, &b);
    implausible += !(a.tm_gmtoff >= -12 * 3600 && a.tm_gmtoff <= 14 * 3600 && b.tm_gmtoff >= -12 * 3600 && b.tm_gmtoff <= 14 * 3600);
  }
  CHECK(implausible == 0);
  auto off = [&](const char *zone, time_t t) { setenv("TZ", geo::zonePosix(geo::zoneByName(zone)), 1); tzset(); struct tm l; localtime_r(&t, &l); return (long)l.tm_gmtoff / 3600; };
  CHECK(off("Europe/London", jan) == 0 && off("Europe/London", jul) == 1 && off("America/New_York", jan) == -5 && off("America/New_York", jul) == -4);
  CHECK(off("Asia/Tokyo", jan) == 9 && off("Asia/Tokyo", jul) == 9 && off("Australia/Sydney", jan) == 11 && off("Australia/Sydney", jul) == 10);
  CHECK(off("Asia/Kolkata", jan) == 5);   // +5:30 -> 5 in whole hours
  // world outline
  CHECK(geo::worldRings() > 200);

  SECTION("map overlay data");
  CHECK(mapdata::ready());
  int segs = 0, lines = 0;
  mapdata::lines(51.0, 52.0, -1.0, 0.5, [](const double *, const double *, int n, int, void *c) { (*(int *)c) += n; }, &lines);   // the English coast does not reach Heathrow; some border or coast must be near
  mapdata::lines(50.0, 53.0, -2.0, 2.0, [](const double *, const double *, int n, int, void *c) { (*(int *)c) += n; }, &segs);
  CHECK(segs > 50);
  struct Ap { bool lhr = false; int n = 0; } ap;
  mapdata::airports(51.0, 52.0, -1.0, 0.5, [](double, double, const char *iata, int, void *c) { Ap *a = (Ap *)c; a->n++; a->lhr |= !strcmp(iata, "LHR"); }, &ap);
  CHECK(ap.lhr && ap.n >= 1);
}

static void testUnitsPalette() {
  SECTION("units and palette");
  CHECK_NEAR(units::fromKm(100, units::NM), 53.996, 0.01); CHECK_NEAR(units::fromKm(100, units::MI), 62.137, 0.01); CHECK_NEAR(units::toKm(54, units::NM), 100.008, 0.01);
  CHECK(units::fromFt(35000, units::FL) == 350 && units::fromFt(1000, units::M) == 305 && units::fromFt(1234, units::FT) == 1234);
  CHECK(units::fromKt(100, units::KMH) == 185 && units::fromKt(100, units::MPH) == 115 && units::fromKt(250.4f, units::KT) == 250);
  for (int u = 0; u < units::DIST_N; u++) {
    const int *s = units::rangeSteps((units::Dist)u); bool inc = true, api = true;
    for (int i = 1; i < units::RANGE_STEPS; i++) inc &= s[i] > s[i - 1];
    for (int i = 0; i < units::RANGE_STEPS; i++) api &= units::toKm((float)s[i], (units::Dist)u) <= 463.1f;   // the data service's limit is 250 nautical miles
    CHECK(inc && api);
    for (int i = 0; i < units::RANGE_STEPS; i++) CHECK(units::nearestRangeStep((units::Dist)u, units::toKm((float)s[i], (units::Dist)u)) == i);
  }
  const palette::Rgb full = palette::shade(0, 1.0f), half = palette::shade(0, 0.5f), none = palette::shade(0, 0);
  CHECK(full.g == 255 && half.g >= 127 && half.g <= 128 && none.g == 0 && palette::shade(-1, 1.0f).g == palette::shade(palette::COUNT - 1, 1.0f).g);
  CHECK(palette::kSwatches[7].r == 255 && !strcmp(palette::kSwatches[7].name, "Red"));   // night mode relies on swatch 7 being red
}

int main(int argc, char **argv) {
  const std::string root = argc > 1 ? argv[1] : "../..";
  const std::string fw = argc > 2 ? argv[2] : root + "/firmware/.pio/build/viewe7/firmware.bin";
  testSolar(); testIsoTime(); testImageCheck(fw); testIcaoAndFlags(root); testAirlinesAndGeo(); testUnitsPalette();
  printf("\n%d checks, %d failed\n", gChecks, gFails);
  return gFails ? 1 : 0;
}
