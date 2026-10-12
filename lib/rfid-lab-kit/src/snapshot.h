// What the reader task publishes for loop() to draw: one plain struct, copied under a mutex.
#pragma once

#include "iso14443a.h"
#include "mfrc522.h"

// One edge-lab experiment as a bar chart: up to 12 bars on one scale.
struct EdgeResult {
  static constexpr int kMax = 12;
  char title[28] = "";
  char unit[8] = "";       // "%", "us", ...
  char caption[56] = "";   // the one-line conclusion
  char label[kMax][7] = {};
  float value[kMax] = {};
  int n = 0;
  int mark = -1;           // a bar to outline (e.g. the datasheet limit), -1 none
  bool goodHigh = true;    // true: tall = good (success %), false: tall = bad (errors, time)
  void add(const char *l, float v) {
    if (n >= kMax) return;
    strlcpy(label[n], l, sizeof(label[n]));
    value[n++] = v;
  }
};

// The last memory dump: what each of the first 64 blocks (16 sectors) holds.
enum class BlockKind : uint8_t { Unknown, Maker, Empty, Data, Value, Trailer, Closed };
struct MemoryMap {
  bool valid = false;
  iso14443a::Uid uid;
  int sectors = 0;          // read so far (up to 16)
  BlockKind kind[64] = {};
  uint8_t cond[64] = {};    // access condition C1C2C3 of each block
  uint8_t data[64][16] = {};
};

struct Snapshot {
  uint32_t version = 0;
  // chip
  uint8_t chipVersion = 0;
  int8_t selfTest = -1;     // -1 not run · 0 failed · 1 passed · 2 clone without self-test · 3 clone, 2 runs agree
  uint32_t spiHz = 0, timeoutUs = 0;
  uint8_t gain = 4;
  bool irqMode = false, irqPin = false;
  rc522::Stats stats;
  // card
  bool polling = true, present = false;
  iso14443a::Uid uid;
  iso14443a::Collisions collisions;  // from the last selection
  uint32_t seenAt = 0, taps = 0;
  uint32_t reads = 0;                // every successful card read: a new card, or a command reaching it
  uint8_t rate = 0;                  // % of the last 20 polls that found a card
  iso14443a::Uid inventory[4];
  int inventoryN = -1;               // -1 = not run
  // work
  const char *busy = nullptr;
  uint8_t progress = 0;
  char note[64] = "";
  bool noteBad = false;
  // results
  MemoryMap map;
  bool creditsValid = false;
  int32_t credits = 0, backup = 0;
  EdgeResult edge;
};
