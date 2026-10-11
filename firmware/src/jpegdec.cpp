// Baseline AND progressive JPEG decoding (Planespotters serves progressive files, which LovyanGFX's own decoder cannot read).
// stb_image (public domain, v2.30), JPEG only, allocating from PSRAM.
#include <esp_heap_caps.h>
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_REALLOC(p, sz) heap_caps_realloc((p), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(p) heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

// RGB888, w*h*3 bytes (free with jpegFree), or nullptr if the data is not a decodable JPEG.
uint8_t *jpegDecodeRgb(const uint8_t *data, size_t len, int &w, int &h) {
  int comp = 0;
  return stbi_load_from_memory(data, (int)len, &w, &h, &comp, 3);
}
void jpegFree(uint8_t *p) { stbi_image_free(p); }
