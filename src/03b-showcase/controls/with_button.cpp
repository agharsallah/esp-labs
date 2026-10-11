// Variation 2: an external push button on GPIO 5, plus the BOOT button. Both do the same:
//   click = next scene · double-click = previous scene · long press (0.6 s) = auto-cycle on/off
//
// Wiring: GPIO 5 -> one leg of the button, the diagonally opposite leg -> GND.
// Both buttons use interrupts to catch every contact bounce (see button.h).
#include "button.h"
#include "../controls.h"
#include "pins.h"

namespace {

Button external(PIN_BUTTON, "button");
Button boot(PIN_BOOT_BTN, "BOOT");

Action toAction(Button &b) {
  switch (b.poll()) {
    case Button::Event::Click:
      log_i("%s: click (%lu bounces filtered)", b.name(), static_cast<unsigned long>(b.lastBounces()));
      return Action::NextScene;
    case Button::Event::DoubleClick:
      log_i("%s: double-click", b.name());
      return Action::PreviousScene;
    case Button::Event::LongPress:
      log_i("%s: long press", b.name());
      return Action::ToggleAutoCycle;
    case Button::Event::None:
      break;
  }
  return Action::None;
}

}  // namespace

namespace controls {

void begin() {
  external.begin();
  boot.begin();
  log_i("Controls: button on GPIO %d and BOOT on GPIO %d (variation 2)", PIN_BUTTON, PIN_BOOT_BTN);
}

Action poll() {
  // Poll both every frame so neither misses a gesture; report the external button first.
  const Action a = toAction(external);
  const Action b = toAction(boot);
  return a != Action::None ? a : b;
}

bool anyDown() { return external.isDown() || boot.isDown(); }
bool showsPressDot() { return true; }
const char *help() { return "Button (GPIO 5) or BOOT: click = next, double-click = previous, long press = auto-cycle"; }

}  // namespace controls
