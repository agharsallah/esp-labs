// Saved card data ("snapshots"): what a card's lab blocks held at one moment, kept in NVS
// (namespace "snaps", up to 8) so it can be written back onto the same card later, copied onto
// another card, compared, or downloaded by the app as a JSON file.
//
// The lab blocks are the six that Label, Wipe and Inspect use: 4, 5, 6 (sector 1) and 8, 9, 10
// (sector 2), which is where every lab writes (R4's writes, R5's credits, R7's label). Never
// block 0 (the UID) and never a sector trailer (keys and access bits). So a copy carries the
// data, not the identity: the target keeps its own UID, and the door, which decides by UID, sees
// a different card.
#pragma once

#include <Preferences.h>

#include <mutex>
#include <vector>

namespace snap {

constexpr int kBlocks = 6;
constexpr uint8_t kBlock[kBlocks] = {4, 5, 6, 8, 9, 10};

// Position of `block` in kBlock, -1 when it isn't a lab block.
inline int slotOf(int block) {
  for (int i = 0; i < kBlocks; i++)
    if (kBlock[i] == block) return i;
  return -1;
}

struct Snapshot {
  uint32_t id = 0;  // never reused, so the app can't delete the wrong one after a change
  char name[24] = "";
  uint8_t uid[10] = {};  // the card it was read from; length 0 = imported from a file
  uint8_t uidLen = 0;
  uint32_t at = 0;   // unix seconds, 0 without a clock
  uint8_t have = 0;  // bit i: kBlock[i] was readable and data[i] holds it
  uint8_t data[kBlocks][16] = {};

  int count() const { return __builtin_popcount(have); }
};

class Store {
 public:
  static constexpr int kMax = 8;
  static constexpr uint8_t kFormat = 1;  // bump when Snapshot changes

  void begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    Preferences p;
    p.begin("snaps", true);
    if (p.getUChar("format", 0) == kFormat) {
      count_ = std::min<int>(p.getUChar("count", 0), kMax);
      nextId_ = p.getULong("next", 1);
      if (count_ && p.getBytes("list", list_, sizeof(Snapshot) * count_) != sizeof(Snapshot) * count_) count_ = 0;
    }
    p.end();
    log_i("saved card data: %d snapshot%s", count_, count_ == 1 ? "" : "s");
  }

  uint32_t version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
  }

  std::vector<Snapshot> list() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Snapshot>(list_, list_ + count_);
  }

  bool get(uint32_t id, Snapshot &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(id);
    if (i < 0) return false;
    out = list_[i];
    return true;
  }

  // Stores a copy with a new id (written into `s`); false when all 8 places are taken.
  bool add(Snapshot &s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ >= kMax) return false;
    s.id = nextId_++;
    list_[count_++] = s;
    save();
    return true;
  }

  bool remove(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(id);
    if (i < 0) return false;
    for (int k = i; k + 1 < count_; k++) list_[k] = list_[k + 1];
    count_--;
    save();
    return true;
  }

 private:
  int find(uint32_t id) const {
    for (int i = 0; i < count_; i++)
      if (list_[i].id == id) return i;
    return -1;
  }

  void save() {
    Preferences p;
    p.begin("snaps", false);
    p.putUChar("format", kFormat);
    p.putUChar("count", uint8_t(count_));
    p.putULong("next", nextId_);
    if (count_) p.putBytes("list", list_, sizeof(Snapshot) * count_);
    else if (p.isKey("list")) p.remove("list");
    p.end();
    version_++;
  }

  std::mutex mutex_;
  Snapshot list_[kMax];
  int count_ = 0;
  uint32_t nextId_ = 1, version_ = 1;
};

}  // namespace snap
