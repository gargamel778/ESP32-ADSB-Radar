#include "airline.h"
#include <string.h>

extern const uint8_t airlinesStart[] asm("_binary_data_airlines_bin_start");
extern const uint8_t airlinesEnd[] asm("_binary_data_airlines_bin_end");

namespace airline {

// airlines.bin: "ALN1", count (u32), count records of { char code[4]; u32 nameOffset } sorted by code, then the NUL-terminated names.
const char *forCallsign(const char *cs) {
  if (!cs || strlen(cs) < 4 || (size_t)(airlinesEnd - airlinesStart) < 8 || memcmp(airlinesStart, "ALN1", 4)) return nullptr;
  for (int i = 0; i < 3; i++) if (cs[i] < 'A' || cs[i] > 'Z') return nullptr;
  if (cs[3] < '0' || cs[3] > '9') return nullptr;   // airline flight numbers start with a digit; registrations such as "HBZXY" do not
  uint32_t n;
  memcpy(&n, airlinesStart + 4, 4);
  const uint8_t *recs = airlinesStart + 8;
  const char *pool = (const char *)(recs + (size_t)n * 8);
  int lo = 0, hi = (int)n - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const int c = memcmp(recs + (size_t)mid * 8, cs, 3);
    if (c == 0) {
      uint32_t off;
      memcpy(&off, recs + (size_t)mid * 8 + 4, 4);
      return pool + off;
    }
    if (c < 0) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

}  // namespace airline
