// Jumper wires as touch keys: the ESP32-S3's capacitive touch sensor.
//
// GPIO 1-14 can measure how much charge their pin holds. A finger on a bare wire end adds
// capacitance, so the reading rises (on the S3 it goes UP when touched; on the original ESP32
// it went down). We measure each pad with nobody touching it (the baseline) when you switch
// touch on, then call it touched above +25 % and released below +15 %: the gap (hysteresis)
// stops a finger hovering at the edge from flickering on and off.
//
// Shared by B5-B7 (kit/lib/ble-kit). Nothing to wire beyond a jumper in the pin, the other end loose. Keep your hands off the wires
// while `touch on` measures the baselines.
#pragma once

#include <Arduino.h>

template <int N>
class TouchPads {
 public:
  explicit TouchPads(const uint8_t (&pins)[N]) {
    for (int i = 0; i < N; i++) pins_[i] = pins[i];
  }

  bool on() const { return on_; }
  uint8_t pin(int i) const { return pins_[i]; }
  uint32_t value(int i) const { return value_[i]; }
  uint32_t baseline(int i) const { return base_[i]; }
  bool down(int i) const { return (down_ >> i) & 1; }
  // How far above the baseline, in percent (for a bar on screen and a velocity).
  int rise(int i) const { return base_[i] ? int((int64_t(value_[i]) - base_[i]) * 100 / base_[i]) : 0; }

  // Switch on: measure every pad 8 times with nobody touching. Takes ~0.1 s.
  void enable(bool on) {
    on_ = on;
    down_ = 0;
    if (!on) return;
    for (int i = 0; i < N; i++) {
      uint64_t sum = 0;
      for (int k = 0; k < 8; k++) sum += touchRead(pins_[i]);
      base_[i] = uint32_t(sum / 8);
      value_[i] = base_[i];
    }
  }

  // Every ~40 ms from loop(). Returns the pads that went down (bit i) since the last call;
  // `released` gets the ones that came up.
  uint32_t poll(uint32_t &released) {
    released = 0;
    if (!on_ || millis() - at_ < 40) return 0;
    at_ = millis();
    uint32_t pressed = 0;
    for (int i = 0; i < N; i++) {
      value_[i] = touchRead(pins_[i]);
      const bool was = down(i);
      const uint64_t v = uint64_t(value_[i]) * 100;
      if (!was && v > uint64_t(base_[i]) * 125) down_ |= 1u << i, pressed |= 1u << i;
      else if (was && v < uint64_t(base_[i]) * 115) down_ &= ~(1u << i), released |= 1u << i;
    }
    return pressed;
  }

 private:
  uint8_t pins_[N];
  uint32_t base_[N] = {}, value_[N] = {};
  uint32_t down_ = 0, at_ = 0;
  bool on_ = false;
};
