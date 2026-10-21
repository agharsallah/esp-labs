// Lesson 01: meet the board.
// 1. Prints the chip's identity card to the serial monitor, so we can confirm it really is an N16R8.
// 2. Cycles the on-board RGB LED through a colour wheel: proof that our code is running.
//
// Every Arduino program has two functions:
//   setup() runs once after power-on or reset.
//   loop()  runs forever, as fast as it can, right after setup().
#include <Arduino.h>

#include "pins.h"

namespace {

constexpr uint8_t kBrightness = 40;      // 0-255. The LED is very bright; 40 is easy on the eyes.
constexpr uint32_t kHeartbeatMs = 5000;  // How often we print a "still alive" line.

// Turns a position on the colour wheel (0-255) into red/green/blue.
void wheel(uint8_t pos, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (pos < 85) {
    r = 255 - pos * 3, g = pos * 3, b = 0;
  } else if (pos < 170) {
    pos -= 85;
    r = 0, g = 255 - pos * 3, b = pos * 3;
  } else {
    pos -= 170;
    r = pos * 3, g = 0, b = 255 - pos * 3;
  }
}

void printIdentityCard() {
  Serial.println();
  Serial.println("+---------------- ESP32 identity card ----------------+");
  Serial.printf("| chip      %s rev %d, %d cores @ %lu MHz\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  Serial.printf("| flash     %lu MB   (the N16 part: expect 16)\n",
                static_cast<unsigned long>(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf("| psram     %lu MB   (the R8 part: expect ~8)\n",
                static_cast<unsigned long>(ESP.getPsramSize() / (1024 * 1024)));
  Serial.printf("| free heap %lu KB\n", static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
  // The eFuse stores the MAC lowest byte first, so print byte 0 first to match what esptool shows.
  const uint64_t mac = ESP.getEfuseMac();
  Serial.print("| mac       ");
  for (int i = 0; i < 6; i++) Serial.printf(i < 5 ? "%02X:" : "%02X\n", static_cast<unsigned>((mac >> (8 * i)) & 0xFF));
  Serial.printf("| sdk       %s\n", ESP.getSdkVersion());
  Serial.println("+-----------------------------------------------------+");

  // Warn loudly when the hardware does not match what we bought.
  if (ESP.getPsramSize() == 0) {
    log_e("No PSRAM found. Check the board setting in platformio.ini (it must be an R8 board).");
  }
  if (ESP.getFlashChipSize() < 16UL * 1024 * 1024) {
    log_w("Flash reports less than 16 MB. Fine for now, but the board may not be an N16.");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);  // Give the serial monitor a moment to connect so we don't miss the first lines.

  printIdentityCard();
  log_i("RGB LED on GPIO %d. You should see a slow rainbow.", PIN_RGB);
}

void loop() {
  static uint8_t hue = 0;
  static uint32_t lastHeartbeat = 0;

  uint8_t r, g, b;
  wheel(hue++, r, g, b);
  // rgbLedWrite talks the WS2812 protocol for us. Scale each channel by our brightness.
  rgbLedWrite(PIN_RGB, r * kBrightness / 255, g * kBrightness / 255, b * kBrightness / 255);

  // millis() = milliseconds since boot. Comparing against it lets us do things "every N ms"
  // without delay() freezing the whole program.
  if (millis() - lastHeartbeat >= kHeartbeatMs) {
    lastHeartbeat = millis();
    log_i("alive for %lus, hue=%u", static_cast<unsigned long>(millis() / 1000), hue);
  }

  delay(20);  // 256 steps x 20 ms = one full rainbow every ~5 seconds.
}
