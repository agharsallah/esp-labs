// A push button on any GPIO, wired between the pin and GND.
//
// Wiring: we turn on the chip's internal pull-up resistor, so the pin reads HIGH when the
// button is up and LOW when pressed (the button connects it to GND). No resistor needed.
//
// Bounce: the metal contacts inside a button flicker on/off for a few milliseconds on
// every press and release. Our screen loop only looks about 22 times a second, far too
// slowly to see that. So an interrupt counts every single edge the instant it happens,
// and poll() only trusts a new state once the pin has been quiet for kDebounceMs.
//
// Gestures, reported by poll():
//   Click        press and release, then nothing else for kDoubleGapMs
//   DoubleClick  two clicks within kDoubleGapMs
//   LongPress    held for kLongPressMs (fires while still held, so it feels instant)
#pragma once

#include <Arduino.h>
#include <esp_timer.h>

class Button {
 public:
  enum class Event { None, Click, DoubleClick, LongPress };

  Button(uint8_t pin, const char *name) : pin_(pin), name_(name) {}

  void begin() {
    pinMode(pin_, INPUT_PULLUP);
    // CHANGE = run onEdge on every rising and falling edge, bounces included.
    attachInterruptArg(digitalPinToInterrupt(pin_), onEdge, this, CHANGE);
  }

  const char *name() const { return name_; }
  bool isDown() const { return pressed_; }
  // Raw edges seen during the last press + release, minus the 2 a perfect button makes.
  uint32_t lastBounces() const { return lastBounces_; }

  Event poll() {
    const uint32_t nowUs = uint32_t(esp_timer_get_time());
    const uint32_t now = millis();

    // 1. Debounce: accept the pin's level once no edge has arrived for kDebounceMs.
    const bool level = digitalRead(pin_) == LOW;
    if (level != pressed_ && nowUs - lastEdgeUs_ >= kDebounceMs * 1000) {
      pressed_ = level;
      if (pressed_) {
        pressedAt_ = now;
        longFired_ = false;
      } else {
        // Every edge since the previous release belongs to this press + release.
        const uint32_t edges = edges_ - edgesAtRest_;
        edgesAtRest_ = edges_;
        lastBounces_ = edges > 2 ? edges - 2 : 0;
        if (!longFired_) {
          clicks_++;
          releasedAt_ = now;
        }
      }
    }

    // 2. Turn clean presses into gestures.
    if (pressed_ && !longFired_ && now - pressedAt_ >= kLongPressMs) {
      longFired_ = true;
      clicks_ = 0;
      return Event::LongPress;
    }
    if (!pressed_ && clicks_ >= 2) {
      clicks_ = 0;
      return Event::DoubleClick;
    }
    if (!pressed_ && clicks_ == 1 && now - releasedAt_ >= kDoubleGapMs) {
      clicks_ = 0;
      return Event::Click;
    }
    return Event::None;
  }

 private:
  static constexpr uint32_t kDebounceMs = 25;
  static constexpr uint32_t kLongPressMs = 600;
  static constexpr uint32_t kDoubleGapMs = 280;

  // Runs inside the interrupt: keep it tiny. IRAM_ATTR keeps it in fast internal RAM.
  static void IRAM_ATTR onEdge(void *arg) {
    auto *self = static_cast<Button *>(arg);
    self->edges_ = self->edges_ + 1;
    self->lastEdgeUs_ = uint32_t(esp_timer_get_time());  // 32 bits: read in one go, wraps safely
  }

  uint8_t pin_;
  const char *name_;
  volatile uint32_t edges_ = 0;      // written by the interrupt
  volatile uint32_t lastEdgeUs_ = 0;  // written by the interrupt
  bool pressed_ = false;
  bool longFired_ = false;
  uint8_t clicks_ = 0;
  uint32_t edgesAtRest_ = 0;
  uint32_t lastBounces_ = 0;
  uint32_t pressedAt_ = 0;
  uint32_t releasedAt_ = 0;
};
