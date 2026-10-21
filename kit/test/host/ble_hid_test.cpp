// Host test for kit/lib/ble-kit/src/hid_report.h: real report descriptors and reports, on the Mac.
//   c++ -std=c++17 -Wall -I kit/lib/ble-kit/src kit/test/host/ble_hid_test.cpp -o /tmp/ble_hid_test && /tmp/ble_hid_test
#include <cstdio>

#include "hid_report.h"

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++;   \
  } while (0)

// The boot keyboard from the HID spec (appendix B.1): modifiers, a reserved byte, LEDs (output), 6 keys.
static const uint8_t kBootKeyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00, 0xC0};

// A 5-button mouse with report ID 2, 12-bit X and Y (-2047..2047) and an 8-bit wheel.
static const uint8_t kMouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0};

// A keyboard (report 1) with a media row (consumer control, report 3), like most BLE keyboards.
static const uint8_t kCombo[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x08, 0x81, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x19,
    0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00, 0xC0, 0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x03,
    0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0};

int main() {
  // ---- Boot keyboard ----
  hid::Map m = hid::parse(kBootKeyboard, sizeof(kBootKeyboard));
  CHECK(!m.malformed && !m.truncated && !m.hasReportIds);
  CHECK(m.reports == 1 && m.reportBits[0] == 64);  // 8 bytes of input; the LED output adds nothing
  hid::Keyboard k = hid::findKeyboard(m);
  CHECK(k.found && k.modifiers == 0 && m.fields[k.keys].bitOffset == 16 && m.fields[k.keys].count == 6);
  CHECK(m.fields[0].app == hid::kAppKeyboard);
  const uint8_t shiftH[] = {0x02, 0x00, 0x0B, 0, 0, 0, 0, 0};  // L-Shift + h
  hid::KeyState s = hid::keyboardState(m, k, shiftH, sizeof(shiftH));
  CHECK(s.mods == 0x02 && s.n == 1 && s.keys[0] == 0x0B);
  CHECK(hid::toChar(s.keys[0], s.mods, hid::Layout::Us) == 'H');
  hid::KeyState boot = hid::bootKeyboard(shiftH, sizeof(shiftH));
  CHECK(boot.mods == 0x02 && boot.n == 1 && boot.keys[0] == 0x0B);

  // Only keys that weren't already down count as typed.
  const uint8_t hAndI[] = {0x00, 0x00, 0x0B, 0x0C, 0, 0, 0, 0};
  hid::KeyState s2 = hid::keyboardState(m, k, hAndI, sizeof(hAndI));
  uint8_t fresh[8];
  CHECK(hid::newlyPressed(s, s2, fresh) == 1 && fresh[0] == 0x0C);

  // ---- Mouse with report IDs and 12-bit axes ----
  m = hid::parse(kMouse, sizeof(kMouse));
  CHECK(!m.malformed && m.hasReportIds);
  hid::Mouse ms = hid::findMouse(m);
  CHECK(ms.found && ms.reportId == 2 && ms.buttonCount == 5);
  CHECK(m.fields[ms.x].bitOffset == 8 && m.fields[ms.x].size == 12 && m.fields[ms.x].relative);
  CHECK(m.fields[ms.y].bitOffset == 20 && m.fields[ms.wheel].bitOffset == 32);
  CHECK(m.fields[ms.x].logMin == -2047 && m.fields[ms.x].logMax == 2047);
  // Left button, X +5, Y -3 (12 bits: FFD), wheel +1. No report ID byte: BLE keeps it in 2908.
  const uint8_t move[] = {0x01, 0x05, 0xD0, 0xFF, 0x01};
  CHECK(hid::value(m.fields[ms.buttons], move, sizeof(move)) == 1);
  CHECK(hid::value(m.fields[ms.x], move, sizeof(move)) == 5);
  CHECK(hid::value(m.fields[ms.y], move, sizeof(move)) == -3);
  CHECK(hid::value(m.fields[ms.wheel], move, sizeof(move)) == 1);

  // ---- Keyboard + consumer control ----
  m = hid::parse(kCombo, sizeof(kCombo));
  k = hid::findKeyboard(m);
  CHECK(k.found && k.reportId == 1);
  int consumer = -1;
  for (int i = 0; i < m.count; i++)
    if (m.fields[i].page == hid::kPageConsumer) consumer = i;
  CHECK(consumer >= 0 && m.fields[consumer].reportId == 3 && m.fields[consumer].size == 16 && m.fields[consumer].app == hid::kAppConsumer);
  const uint8_t volUp[] = {0xE9, 0x00};
  CHECK(hid::value(m.fields[consumer], volUp, sizeof(volUp)) == 0xE9);
  CHECK(hid::consumerName(0xE9) != nullptr);

  // ---- Layouts: the key says where, the layout says what ----
  CHECK(hid::toChar(0x1C, 0, hid::Layout::Us) == 'y' && hid::toChar(0x1C, 0, hid::Layout::De) == 'z');
  CHECK(hid::toChar(0x2D, 0, hid::Layout::De) == 0xE1);           // ß
  CHECK(hid::toChar(0x14, 0x40, hid::Layout::De) == '@');         // AltGr + q
  CHECK(hid::toChar(0x20, 0x02, hid::Layout::De) == 0x15);        // Shift + 3 = §
  CHECK(hid::toChar(0x20, 0x02, hid::Layout::Us) == '#');
  CHECK(hid::toChar(0x33, 0, hid::Layout::De) == 0x94);           // ö
  CHECK(hid::toChar(hid::key::kEnter, 0, hid::Layout::Us) == '\n');

  // A truncated descriptor is malformed, not a crash.
  m = hid::parse(kMouse, 7);
  CHECK(m.malformed);

  if (failures == 0) std::printf("ble hid: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
