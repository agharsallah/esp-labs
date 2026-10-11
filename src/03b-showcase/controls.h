// How the showcase is controlled, without main.cpp knowing which buttons exist.
//
// Two variations implement this, and platformio.ini compiles exactly one of them:
//   controls/boot_only.cpp     variation 1, env 03b-showcase: the BOOT button, nothing to wire
//   controls/with_button.cpp   variation 2, env 03b-showcase-button: a push button on GPIO 5 + BOOT
#pragma once

// What a button gesture asks the showcase to do.
enum class Action { None, NextScene, PreviousScene, ToggleAutoCycle };

namespace controls {

void begin();
// Call every loop. Returns at most one action per call.
Action poll();
// True while a button is held down (variation 2 lights a dot on screen).
bool anyDown();
// Whether this variation draws the press dot.
bool showsPressDot();
// One line for the serial help text.
const char *help();

}  // namespace controls
