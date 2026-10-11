// "2026-10-10T08:15:00Z" -> seconds since 1970 (UTC). Header-only so it can be tested on a computer.
#pragma once
#include <stdio.h>
#include <time.h>

namespace isotime {

inline time_t parse(const char *s) {   // 0 if the text is not a date and time
  int Y, M, D, h, m, sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  if (M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 24 || m < 0 || m > 59 || sec < 0 || sec > 60) return 0;
  Y -= M <= 2;
  const int era = (Y >= 0 ? Y : Y - 399) / 400, yoe = Y - era * 400;
  const int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = (long)era * 146097 + doe - 719468;
  return (time_t)(days * 86400L + h * 3600L + m * 60L + sec);
}

}  // namespace isotime
