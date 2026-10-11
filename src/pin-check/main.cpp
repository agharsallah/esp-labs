// Pin check: is the problem the wires or the screen?
// Safe to run with the screen connected: its inputs only listen, and we never drive two pins against
// each other.
//
// 1. Short test (automatic, printed once at start): finds a data pin touching GND, 3V3 or a
//    neighbouring data pin. Loose wires (open circuits) can't be seen from the chip's side.
// 2. Probe mode (repeats forever): each screen data pin in turn blinks slowly for 8 s.
//    Hold an LED with its resistor between the screen's pin and GND (long leg to the pin):
//    if it blinks at the screen end, the wire is good all the way.
//
// Serial: help | next | short (repeat the short test)
#include <Arduino.h>

#include "pins.h"

namespace {

struct ScreenPin {
  uint8_t gpio;
  const char *label;
};
constexpr ScreenPin kPins[] = {
    {PIN_TFT_SCLK, "SCL"}, {PIN_TFT_MOSI, "SDA"}, {PIN_TFT_CS, "CS"}, {PIN_TFT_DC, "DC"}, {PIN_TFT_RST, "RES"},
};
constexpr int kCount = sizeof(kPins) / sizeof(kPins[0]);
constexpr uint32_t kProbeMs = 8'000, kBlinkMs = 500;

int probing = 0;
uint32_t probeStartedAt = 0;

void releaseAll() {
  for (const auto &p : kPins) pinMode(p.gpio, INPUT);
}

bool readWith(uint8_t gpio, uint8_t mode) {
  pinMode(gpio, mode);
  delayMicroseconds(200);  // let the weak internal pull settle
  return digitalRead(gpio);
}

void shortTest() {
  releaseAll();
  Serial.println("\n== Short test ==");
  int problems = 0;
  bool heldHigh[kCount] = {};
  for (int i = 0; i < kCount; i++) {
    const auto &p = kPins[i];
    const bool up = readWith(p.gpio, INPUT_PULLUP);
    const bool down = readWith(p.gpio, INPUT_PULLDOWN);
    heldHigh[i] = down;
    Serial.printf("  GPIO %2u %-3s  pull-up reads %d, pull-down reads %d  ", p.gpio, p.label, up, down);
    if (!up) {
      Serial.println("<- held LOW: touching GND?");
      problems++;
    } else if (down) {
      // Many screen modules have a pull-up resistor on RES (and sometimes CS): then this is normal,
      // and even a sign the wire reaches the screen.
      Serial.println("<- held HIGH: a pull-up on the screen module, or touching 3V3");
    } else {
      Serial.println("ok (free)");
    }
    pinMode(p.gpio, INPUT);
  }
  // Bridges: drive one pin high, see if a pulled-down neighbour follows it.
  for (int a = 0; a < kCount; a++) {
    for (int i = 0; i < kCount; i++) pinMode(kPins[i].gpio, i == a ? OUTPUT : INPUT_PULLDOWN);
    digitalWrite(kPins[a].gpio, HIGH);
    delayMicroseconds(200);
    for (int b = 0; b < kCount; b++) {
      if (b == a || heldHigh[b]) continue;
      if (digitalRead(kPins[b].gpio)) {
        Serial.printf("  <- GPIO %u %s and GPIO %u %s are connected to each other (short)\n", kPins[a].gpio,
                      kPins[a].label, kPins[b].gpio, kPins[b].label);
        problems++;
      }
    }
    digitalWrite(kPins[a].gpio, LOW);
  }
  releaseAll();
  Serial.printf("Short test: %s\n", problems ? "problems found, see the arrows above" : "no shorts found");
  Serial.println("(A loose wire doesn't show up here: use probe mode with an LED.)\n");
}

void startProbe(int i) {
  releaseAll();
  probing = i % kCount;
  probeStartedAt = millis();
  pinMode(kPins[probing].gpio, OUTPUT);
  Serial.printf("Probe: GPIO %u -> screen pin %s blinks now (8 s)\n", kPins[probing].gpio, kPins[probing].label);
}

void printHelp() {
  Serial.println("Commands: next (probe the next pin) | short (repeat the short test) | help");
}

void pollSerial() {
  static String line;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 16) line += c;
      continue;
    }
    line.trim();
    line.toLowerCase();
    if (line == "next") startProbe(probing + 1);
    else if (line == "short") shortTest(), startProbe(probing);
    else if (line == "help" || line == "?") printHelp();
    else if (!line.isEmpty()) Serial.printf("unknown command '%s'\n", line.c_str());
    line = "";
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);  // keep the backlight on, as a sign the power wires are fine
  Serial.println("\nPin check for the screen wires (SCL 12, SDA 11, CS 10, DC 9, RES 8)");
  shortTest();
  printHelp();
  startProbe(0);
}

void loop() {
  pollSerial();
  const uint32_t now = millis();
  if (now - probeStartedAt >= kProbeMs) startProbe(probing + 1);
  digitalWrite(kPins[probing].gpio, (now / kBlinkMs) % 2);
  rgbLedWrite(PIN_RGB, 0, 0, (now / kBlinkMs) % 2 ? 40 : 0);  // the on-board LED blinks along, in blue
}
