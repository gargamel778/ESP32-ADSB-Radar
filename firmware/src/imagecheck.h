// Checks an ESP32-S3 application image held in memory before any flash is touched: header, chip, segment walk and the SHA-256 the build appends.
// An ESP image: 24-byte header, segments (8-byte header + data), a checksum byte padded to 16 bytes, then (hash_appended) the SHA-256 of all that.
// Header-only so it can be tested on a computer (tests/host).
#pragma once
#include <stdint.h>
#include <string.h>
#include <mbedtls/sha256.h>

namespace imagecheck {

inline bool verify(const uint8_t *b, size_t n, char *err, size_t cap) {
  if (n < 24 + 8 + 32 || b[0] != 0xE9) { strlcpy(err, "Not a firmware file for this radar", cap); return false; }
  if ((b[12] | (b[13] << 8)) != 9) { strlcpy(err, "This firmware is for a different chip", cap); return false; }   // 9 = ESP32-S3
  const unsigned segs = b[1];
  if (!segs || segs > 16) { strlcpy(err, "Not a firmware file for this radar", cap); return false; }
  size_t pos = 24;
  for (unsigned i = 0; i < segs; i++) {
    if (pos + 8 > n) { strlcpy(err, "The firmware file is incomplete", cap); return false; }
    uint32_t len; memcpy(&len, b + pos + 4, 4);
    pos += 8 + (size_t)len;
    if (pos > n) { strlcpy(err, "The firmware file is incomplete", cap); return false; }
  }
  const size_t padded = (pos + 1 + 15) & ~(size_t)15;
  if (b[23]) {   // hash appended
    if (padded + 32 > n) { strlcpy(err, "The firmware file is incomplete", cap); return false; }
    uint8_t d[32];
    mbedtls_sha256(b, padded, d, 0);
    if (memcmp(d, b + padded, 32) != 0) { strlcpy(err, "The firmware file is damaged (its checksum does not match)", cap); return false; }
  } else if (padded > n) { strlcpy(err, "The firmware file is incomplete", cap); return false; }
  return true;
}

}  // namespace imagecheck
