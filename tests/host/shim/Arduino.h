// Just enough of Arduino.h for the firmware's pure modules (units, palette, geo, airline, icao, mapdata) to compile on a computer.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <algorithm>

#define constrain(x, a, b) ((x) < (a) ? (a) : (x) > (b) ? (b) : (x))
struct SerialShim {
  int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (!getenv("TEST_VERBOSE")) return 0;
    va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); va_end(ap); return n;
  }
};
extern SerialShim Serial;
