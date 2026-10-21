// Host test for labs/ble-lab-b6/src/keymap.h: characters to HID key usages, US and German.
//   c++ -std=c++17 -Wall -I labs/ble-lab-b6/src labs/ble-lab-b6/test/host/ble_keymap_test.cpp -o /tmp/ble_keymap_test && /tmp/ble_keymap_test
#include <cstdio>

#include "keymap.h"

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++;   \
  } while (0)

using namespace keymap;

static bool is(Stroke s, uint8_t mod, uint8_t key) { return s.mod == mod && s.key == key; }

int main() {
  const Layout US = Layout::US, DE = Layout::DE;
  // Letters: 0x04 = a. Capitals add Shift.
  CHECK(is(fromChar('a', US), 0, 0x04));
  CHECK(is(fromChar('A', US), kShift, 0x04));
  CHECK(is(fromChar('z', US), 0, 0x1D));
  // German swaps y and z: the same key 0x1C is y on US and z on German.
  CHECK(is(fromChar('y', US), 0, 0x1C));
  CHECK(is(fromChar('z', DE), 0, 0x1C));
  CHECK(is(fromChar('y', DE), 0, 0x1D));
  CHECK(is(fromChar('Z', DE), kShift, 0x1C));
  // Digits, Enter, space.
  CHECK(is(fromChar('1', US), 0, 0x1E) && is(fromChar('0', DE), 0, 0x27));
  CHECK(is(fromChar('\n', DE), 0, 0x28) && is(fromChar(' ', US), 0, 0x2C));
  // Punctuation that moves.
  CHECK(is(fromChar('@', US), kShift, 0x1F));
  CHECK(fromChar('@', DE).key == 0);  // AltGr/Option: skipped
  CHECK(is(fromChar('-', US), 0, 0x2D) && is(fromChar('-', DE), 0, 0x38));
  CHECK(is(fromChar('?', US), kShift, 0x38) && is(fromChar('?', DE), kShift, 0x2D));
  CHECK(is(fromChar('/', DE), kShift, 0x24));
  CHECK(is(fromChar('"', DE), kShift, 0x1F) && is(fromChar('"', US), kShift, 0x34));
  CHECK(is(fromChar('<', DE), 0, 0x64));
  CHECK(fromChar('\x01', US).key == 0);

  // Names and combos.
  Stroke s;
  CHECK(fromName("enter", s) && is(s, 0, 0x28));
  CHECK(fromName("cmd+space", s) && is(s, kGui, 0x2C));
  CHECK(fromName("ctrl+alt+delete", s) && is(s, kCtrl | kAlt, 0x4C));
  CHECK(fromName("cmd+shift+4", s) && is(s, kGui | kShift, 0x21));
  CHECK(fromName("f12", s) && is(s, 0, 0x45));
  CHECK(fromName("f1", s) && is(s, 0, 0x3A));
  CHECK(fromName("shift", s) && is(s, kShift, 0));
  CHECK(!fromName("f13", s) && !fromName("hyper+a", s) && !fromName("cmd+", s) && !fromName("", s));

  if (failures == 0) std::printf("ble keymap: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
