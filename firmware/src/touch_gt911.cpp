#include "touch_gt911.h"
#include <Wire.h>

namespace gt911 {
namespace {
constexpr int PIN_SDA = 19, PIN_SCL = 20, PIN_RST = 38, PIN_INT = 18;
constexpr uint16_t REG_STATUS = 0x814E, REG_POINTS = 0x814F, REG_PID = 0x8140;

uint8_t gAddr = 0;
QueueHandle_t gQueue = nullptr;
volatile uint32_t gReads = 0, gErrs = 0;

void hwReset(bool addr14) {
  pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_INT, OUTPUT);
  digitalWrite(PIN_INT, addr14 ? HIGH : LOW);  // INT level while RST rises selects the I2C address
  digitalWrite(PIN_RST, LOW);
  delay(10);
  digitalWrite(PIN_RST, HIGH);
  delay(8);
  pinMode(PIN_INT, INPUT);
  delay(60);
}

bool readReg(uint16_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(gAddr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)gAddr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

void writeReg8(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(gAddr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(v);
  Wire.endTransmission();
}

uint8_t probe() {
  for (uint8_t a : {0x14, 0x5D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0;
}

// Polls every ~8 ms in its own task so touches are independent of the draw loop's frame rate.
void task(void *) {
  for (;;) {
    uint8_t st = 0;
    if (readReg(REG_STATUS, &st, 1)) {
      gReads = gReads + 1;
      if (st & 0x80) {  // buffer-ready; n==0 means "finger lifted"
        Event e = {};
        int n = st & 0x0F;
        if (n > 5) n = 5;
        uint8_t buf[8 * 5];
        if (n > 0 && readReg(REG_POINTS, buf, n * 8)) {
          e.n = n;
          for (int i = 0; i < n; i++) {  // point = id, xL, xH, yL, yH, sizeL, sizeH, reserved
            e.x[i] = buf[i * 8 + 1] | (buf[i * 8 + 2] << 8);
            e.y[i] = buf[i * 8 + 3] | (buf[i * 8 + 4] << 8);
          }
        }
        writeReg8(REG_STATUS, 0);  // acknowledge
        xQueueSend(gQueue, &e, 0);
      }
    } else {
      gErrs = gErrs + 1;
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}
}  // namespace

bool begin() {
  hwReset(false);
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, INPUT_PULLUP);
  delay(2);
  if (digitalRead(PIN_SDA) == LOW) {  // slave holding SDA after an aborted transfer: clock it free
    pinMode(PIN_SCL, OUTPUT_OPEN_DRAIN);
    for (int i = 0; i < 16 && digitalRead(PIN_SDA) == LOW; i++) {
      digitalWrite(PIN_SCL, LOW);  delayMicroseconds(10);
      digitalWrite(PIN_SCL, HIGH); delayMicroseconds(10);
    }
  }
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  gAddr = probe();
  if (!gAddr) {  // retry with the other address-select level
    hwReset(true);
    gAddr = probe();
  }
  if (!gAddr) return false;
  uint8_t pid[4] = {0};
  if (!readReg(REG_PID, pid, 4)) return false;
  Serial.printf("[touch] GT911 addr=0x%02X pid=%c%c%c\n", gAddr, pid[0], pid[1], pid[2]);
  gQueue = xQueueCreate(32, sizeof(Event));
  xTaskCreatePinnedToCore(task, "gt911", 4096, nullptr, 2, nullptr, 0);
  return true;
}

bool next(Event &e) { return gQueue && xQueueReceive(gQueue, &e, 0) == pdTRUE; }
uint8_t address() { return gAddr; }
uint32_t reads() { return gReads; }
uint32_t errors() { return gErrs; }

}  // namespace gt911
