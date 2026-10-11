// Direct GT911 driver (I2C via Wire). Hardware facts (docs/hardware-findings.md):
//  - SDA=19 SCL=20 RST=38 INT=18; the I2C address varies between boots (0x5D or 0x14) -> probe both.
//  - Reset via IO38 is mandatory (the stock firmware skips it and dies on a warm reset).
//  - Coordinates are already in screen space (800x480), no swap/mirror.
#pragma once
#include <Arduino.h>

namespace gt911 {

struct Event {
  uint8_t n;       // 0 = finger lifted, else number of contacts (<=5)
  int16_t x[5];
  int16_t y[5];
};

bool begin();          // reset, bus recovery, probe, start the polling task; false if the chip isn't found
bool next(Event &e);   // pop one event, non-blocking
uint8_t address();
uint32_t reads();
uint32_t errors();

}  // namespace gt911
