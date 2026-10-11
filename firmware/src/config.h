// Build-time defaults. Everything here becomes runtime-configurable (NVS) in M2/M4/M5;
// config/secrets.h (git-ignored) only supplies development defaults.
#pragma once
#include <Arduino.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef HOME_LAT
#define HOME_LAT 51.4700   // first-boot default (London Heathrow): change it in Settings > Location, or in config/secrets.h
#endif
#ifndef HOME_LON
#define HOME_LON -0.4543
#endif
#ifndef RADAR_RADIUS_NM
#define RADAR_RADIUS_NM 54
#endif
#ifndef LOCATION_NAME
#define LOCATION_NAME "London"
#endif
// POSIX TZ string for the first boot (Europe/London). Settings > Location derives it from the location afterwards.
#ifndef TIMEZONE_POSIX
#define TIMEZONE_POSIX "GMT0BST,M3.5.0/1,M10.5.0"
#endif
#ifndef TIMEZONE_NAME   // the IANA name of that rule (several zones share a rule, so it cannot be looked up)
#define TIMEZONE_NAME "Europe/London"
#endif
// Country used for the unit profile until M5 derives it from the location (ISO 3166-1 alpha-2).
#ifndef DEFAULT_COUNTRY
#define DEFAULT_COUNTRY "GB"
#endif
#ifndef RADAR_PCLK_HZ
#define RADAR_PCLK_HZ 12000000
#endif

// Firmware version shown in Settings and on the phone page. OTA_TEST builds (see main.cpp) carry a different suffix so an updated image can be told apart.
#ifndef FW_VERSION
#define FW_VERSION "0.9"
#endif

constexpr int MAX_AIRCRAFT = 150;
constexpr uint32_t FETCH_INTERVAL_MS = 5000;  // airplanes.live: stay well under ~1 request/s
