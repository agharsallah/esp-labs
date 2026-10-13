// What B8 knows, shared between three tasks:
//   the NimBLE task   heard devices (scan_table.h), notifications from the keyboard or mouse
//   the central task  connecting, discovering, pairing, reading the Report Map (it waits a lot)
//   loop()            draws it
// Each part has its own spinlock; loop() copies what the current view needs.
#pragma once

#include <Arduino.h>

namespace b8 {

enum class Phase : uint8_t { Idle, Connecting, Discovering, Pairing, Reading, Hosting };

inline const char *phaseName(Phase p) {
  static const char *const names[] = {"idle", "connecting", "discovering", "pairing", "reading the Report Map", "hosting"};
  return names[static_cast<int>(p)];
}

// ---- The GATT tree of the connected device --------------------------------------------------

struct Node {
  uint8_t level = 0;       // 0 service, 1 characteristic, 2 descriptor
  uint16_t uuid16 = 0;     // 0 = a 128-bit (vendor) UUID
  char name[24] = "";
  char value[28] = "";     // a read value, or why it wasn't read
  uint8_t props = 0;       // characteristics: 02 read, 04 write no rsp, 08 write, 10 notify, 20 indicate
};

struct Tree {
  static constexpr int kMax = 72;
  Node nodes[kMax];
  int count = 0;
  bool truncated = false;
  char device[24] = "";
  int services = 0, characteristics = 0;
  bool hasHid = false;
  bool vendorOnly = false;  // nothing but the GAP/GATT basics and the maker's own services
  bool audio = false;       // looks like a speaker or headphones (name, maker, appearance)
  uint32_t version = 0;
};

// ---- What the keyboard and mouse send ---------------------------------------------------------

enum class ReportKind : uint8_t { Keyboard, Mouse, Consumer, Other, BootKeyboard, BootMouse };

struct RawReport {
  uint32_t ms = 0;
  uint8_t id = 0;
  ReportKind kind = ReportKind::Other;
  uint8_t len = 0;
  uint8_t data[16] = {};
  char decoded[40] = "";
};

struct Stroke {
  int16_t x = 0, y = 0;
  uint8_t size = 0, colour = 0;
  bool down = false;
};

struct Input {
  static constexpr int kText = 400, kStrokes = 240, kTrail = 10, kReports = 6;
  // The little editor.
  uint8_t text[kText] = {};  // CP437, so ä ö ü ß show on the screen's built-in font
  int len = 0;
  uint8_t mods = 0;
  uint8_t held[8] = {};
  int heldN = 0;
  uint32_t keystrokes = 0;
  char media[20] = "";
  uint32_t mediaAt = 0;
  // The paint toy.
  int16_t mx = 160, my = 94;
  uint8_t buttons = 0, brush = 3, colour = 0;
  Stroke strokes[kStrokes];
  uint32_t strokeHead = 0;
  int16_t trailX[kTrail] = {}, trailY[kTrail] = {};
  uint32_t trailHead = 0;
  int16_t clickX = 0, clickY = 0;
  uint32_t clickAt = 0;
  uint32_t moves = 0;
  // Raw reports, newest last.
  RawReport reports[kReports];
  uint32_t reportHead = 0;
  uint32_t version = 0;
};

template <typename T>
class Guarded {
 public:
  template <typename F>
  void edit(F &&change) {
    portENTER_CRITICAL(&lock_);
    change(value_);
    value_.version++;
    portEXIT_CRITICAL(&lock_);
  }
  void copyTo(T &out) {
    portENTER_CRITICAL(&lock_);
    out = value_;
    portEXIT_CRITICAL(&lock_);
  }

 private:
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  T value_;
};

inline Guarded<Tree> &tree() {
  static Guarded<Tree> t;
  return t;
}
inline Guarded<Input> &input() {
  static Guarded<Input> i;
  return i;
}

// The central task's progress, for the screen.
struct Progress {
  Phase phase = Phase::Idle;
  char target[24] = "";
  uint32_t since = 0;
  bool keyboard = false, mouse = false, consumer = false, boot = false;
  int fields = 0, mapBytes = 0;
  uint32_t version = 0;
};

inline Guarded<Progress> &progress() {
  static Guarded<Progress> p;
  return p;
}

}  // namespace b8
