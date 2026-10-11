// Variation 1: the on-board BOOT button only. Nothing to wire.
//   click = next scene · long press (0.6 s) = auto-cycle on/off
// The button is read by polling once per frame (see boot_button.h).
#include "../boot_button.h"
#include "../controls.h"
#include "pins.h"

namespace {
BootButton boot(PIN_BOOT_BTN);
}

namespace controls {

void begin() {
  boot.begin();
  log_i("Controls: BOOT button on GPIO %d (variation 1)", PIN_BOOT_BTN);
}

Action poll() {
  switch (boot.poll()) {
    case BootButton::Event::Click:
      return Action::NextScene;
    case BootButton::Event::LongPress:
      log_i("BOOT long press");
      return Action::ToggleAutoCycle;
    case BootButton::Event::None:
      break;
  }
  return Action::None;
}

bool anyDown() { return false; }
bool showsPressDot() { return false; }
const char *help() { return "BOOT button: click = next scene, long press = auto-cycle on/off"; }

}  // namespace controls
