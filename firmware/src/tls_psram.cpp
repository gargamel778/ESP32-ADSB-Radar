// TLS memory in PSRAM. One secure connection needs ~45 KB (two 16 KB record buffers plus handshake state); internal RAM is the scarce kind
// (display, Wi-Fi, the SD card). The prebuilt mbedTLS takes all its memory through these two hooks and the stock versions use internal RAM only;
// defining them here makes the linker use these instead. Falls back to internal RAM if PSRAM is full.
#include <esp_heap_caps.h>
#include <stddef.h>

extern "C" {

void *esp_mbedtls_mem_calloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void esp_mbedtls_mem_free(void *ptr) { heap_caps_free(ptr); }

}
