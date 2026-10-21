// The BOOT button on the board, used as a normal input once the program is running.
// It connects GPIO 0 to GND when pressed, so we enable the internal pull-up and read LOW = pressed.
//
// Mechanical buttons "bounce": one press flickers on/off for a few milliseconds.
// We only trust a new state once it has been stable for kDebounceMs.
#pragma once

#include <Arduino.h>

class BootButton {
 public:
  enum class Event { None, Click, LongPress };

  explicit BootButton(uint8_t pin) : pin_(pin) {}

  void begin() { pinMode(pin_, INPUT_PULLUP); }

  Event poll() {
    const bool raw = digitalRead(pin_) == LOW;
    const uint32_t now = millis();
    if (raw != lastRaw_) {
      lastRaw_ = raw;
      changedAt_ = now;
    }
    if (now - changedAt_ < kDebounceMs || raw == pressed_) return Event::None;

    pressed_ = raw;
    if (pressed_) {
      pressedAt_ = now;
      return Event::None;
    }
    return now - pressedAt_ >= kLongPressMs ? Event::LongPress : Event::Click;
  }

 private:
  static constexpr uint32_t kDebounceMs = 30;
  static constexpr uint32_t kLongPressMs = 600;

  uint8_t pin_;
  bool lastRaw_ = false;
  bool pressed_ = false;
  uint32_t changedAt_ = 0;
  uint32_t pressedAt_ = 0;
};
