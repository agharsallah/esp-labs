// Lesson 02: your first circuit.
// An external LED on the breadboard, controlled by typing commands in the serial monitor.
//
//   GPIO 4 ──[ 220-330 Ω ]──►|── GND
//                          long  short
//                          leg   leg
//
// New ideas in this lesson:
//   - PWM (LEDC): blinking a pin thousands of times per second to fake "half brightness".
//   - A non-blocking state machine: the LED keeps animating while we listen for commands.
//
// Commands (type in the monitor, press Enter):
//   on | off | blink [ms] | fade | sos | level <0-255> | help
#include <Arduino.h>
#include <driver/gpio.h>

#include "pins.h"

namespace {

constexpr uint32_t kPwmHz = 5000;   // Fast enough that the eye can't see the flicker.
constexpr uint8_t kPwmBits = 8;     // 8 bits -> duty from 0 (off) to 255 (fully on).
constexpr uint8_t kMaxDuty = 255;

enum class Mode { Off, On, Blink, Fade, Sos };

struct State {
  Mode mode = Mode::Fade;
  uint8_t level = kMaxDuty;    // Brightness used by "on" and "blink".
  uint32_t blinkMs = 500;      // Half-period of "blink".
  uint32_t modeStartedAt = 0;  // millis() when the current mode began.
} state;

const char *modeName(Mode m) {
  switch (m) {
    case Mode::Off: return "off";
    case Mode::On: return "on";
    case Mode::Blink: return "blink";
    case Mode::Fade: return "fade";
    case Mode::Sos: return "sos";
  }
  return "?";
}

// Our eyes see brightness logarithmically: duty 128 looks much brighter than "half".
// Squaring the value (gamma ~2) makes fades look smooth and even.
uint8_t perceived(uint8_t x) { return static_cast<uint16_t>(x) * x / kMaxDuty; }

void setMode(Mode m) {
  state.mode = m;
  state.modeStartedAt = millis();
  log_i("mode -> %s", modeName(m));
}

void printHelp() {
  Serial.println();
  Serial.println("Commands: on | off | blink [ms] | fade | sos | level <0-255> | help");
  Serial.printf("Now: mode=%s level=%u blink=%lums\n", modeName(state.mode), state.level,
                static_cast<unsigned long>(state.blinkMs));
}

// ---- Output: what the LED should show right now, for each mode ----------------------------

// Morse "SOS": ... --- ...   (1 = LED on for one 150 ms unit, 0 = off)
constexpr char kSos[] = "101010001110111011100010101000000";
constexpr uint32_t kSosUnitMs = 150;

uint8_t dutyFor(uint32_t elapsed) {
  switch (state.mode) {
    case Mode::Off:
      return 0;
    case Mode::On:
      return state.level;
    case Mode::Blink:
      return (elapsed / state.blinkMs) % 2 == 0 ? state.level : 0;
    case Mode::Fade: {
      // A triangle wave 0 -> 255 -> 0 every 2 seconds.
      uint32_t t = elapsed % 2000;
      uint8_t x = t < 1000 ? t * kMaxDuty / 1000 : (2000 - t) * kMaxDuty / 1000;
      return perceived(x);
    }
    case Mode::Sos: {
      size_t i = (elapsed / kSosUnitMs) % (sizeof(kSos) - 1);
      return kSos[i] == '1' ? state.level : 0;
    }
  }
  return 0;
}

// ---- Input: read one line from the serial monitor without blocking ------------------------

void handleCommand(String line) {
  line.trim();
  line.toLowerCase();
  if (line.isEmpty()) return;

  int space = line.indexOf(' ');
  String cmd = space < 0 ? line : line.substring(0, space);
  String arg = space < 0 ? "" : line.substring(space + 1);

  if (cmd == "on") {
    setMode(Mode::On);
  } else if (cmd == "off") {
    setMode(Mode::Off);
  } else if (cmd == "fade") {
    setMode(Mode::Fade);
  } else if (cmd == "sos") {
    setMode(Mode::Sos);
  } else if (cmd == "blink") {
    if (!arg.isEmpty()) {
      long ms = arg.toInt();
      if (ms < 20 || ms > 5000) {
        log_w("blink speed must be 20-5000 ms, got '%s'", arg.c_str());
        return;
      }
      state.blinkMs = ms;
    }
    setMode(Mode::Blink);
  } else if (cmd == "level") {
    long v = arg.toInt();
    if (arg.isEmpty() || v < 0 || v > kMaxDuty) {
      log_w("level needs a number 0-255, got '%s'", arg.c_str());
      return;
    }
    state.level = v;
    log_i("level -> %ld", v);
  } else if (cmd == "help" || cmd == "?") {
    printHelp();
  } else {
    log_w("unknown command '%s' (type help)", cmd.c_str());
  }
}

void pollSerial() {
  static String buffer;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handleCommand(buffer);
      buffer = "";
    } else if (buffer.length() < 32) {
      buffer += c;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  // Hand the pin to the PWM peripheral. Returns false if the pin can't do PWM.
  if (!ledcAttach(PIN_LED, kPwmHz, kPwmBits)) {
    log_e("Could not attach PWM to GPIO %d. Is it a valid output pin?", PIN_LED);
    return;
  }
  // Safety net: the weakest pin drive (~5 mA) protects the LED and the pin if a resistor is missing.
  // With a proper 220-330 Ω resistor you can delete this line for full brightness.
  gpio_set_drive_capability(static_cast<gpio_num_t>(PIN_LED), GPIO_DRIVE_CAP_0);

  rgbLedWrite(PIN_RGB, 0, 0, 0);  // Switch off the on-board LED from lesson 01.
  log_i("LED on GPIO %d ready.", PIN_LED);
  setMode(Mode::Fade);
  printHelp();
}

void loop() {
  pollSerial();
  ledcWrite(PIN_LED, dutyFor(millis() - state.modeStartedAt));
  delay(5);
}
