#include "icao.h"
#include <string.h>
#include <stdlib.h>

extern const uint8_t flagsStart[] asm("_binary_data_flags_bin_start");
extern const uint8_t flagsEnd[] asm("_binary_data_flags_bin_end");

namespace icao {
namespace {
struct Range { uint32_t lo, hi; const char *iso; };
const Range kRanges[] = {
#include "icao_ranges.inc"
};
constexpr int N = sizeof(kRanges) / sizeof(kRanges[0]);
}  // namespace

const char *country(const char *hex) {
  if (!hex || strlen(hex) != 6) return nullptr;
  char *end = nullptr;
  const uint32_t v = strtoul(hex, &end, 16);
  if (!end || *end) return nullptr;
  int lo = 0, hi = N - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (v < kRanges[mid].lo) hi = mid - 1;
    else if (v > kRanges[mid].hi) lo = mid + 1;
    else return kRanges[mid].iso;
  }
  return nullptr;
}

// flags.bin: "FLG1", u16 count, u8 w, u8 h, count * char[2] (sorted), count * w*h RGB565 pixels.
const uint16_t *flagPixels(const char *iso) {
  const size_t len = flagsEnd - flagsStart;
  if (!iso || len < 8 || memcmp(flagsStart, "FLG1", 4)) return nullptr;
  uint16_t n; memcpy(&n, flagsStart + 4, 2);
  if (flagsStart[6] != FLAG_W || flagsStart[7] != FLAG_H) return nullptr;
  const uint8_t *codes = flagsStart + 8;
  const uint8_t *pix = codes + (size_t)n * 2;
  int lo = 0, hi = (int)n - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const int c = memcmp(codes + mid * 2, iso, 2);
    if (c == 0) return (const uint16_t *)(pix + (size_t)mid * FLAG_W * FLAG_H * 2);
    if (c < 0) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

bool hasFlag(const char *iso) { return flagPixels(iso) != nullptr; }

}  // namespace icao
