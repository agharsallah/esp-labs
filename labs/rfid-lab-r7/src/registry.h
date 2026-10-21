// The bench door's card list: up to 20 cards, kept in NVS (namespace "door") so it survives
// power-off and re-flashing. This list, not the card, decides who may open the door.
//
// Two tasks use it: the web server (the app edits cards) and the reader task (a tap is checked
// and counted). Every method takes the mutex, so neither ever sees a half-changed list.
//
// Each change saves the whole list (about 1.2 KB) to flash. Flash wears out after ~100,000 writes
// per spot; NVS spreads the writes around, so a few hundred taps a day are nothing.
#pragma once

#include <Preferences.h>

#include <functional>
#include <mutex>
#include <vector>

#include "access_rules.h"

class Registry {
 public:
  static constexpr int kMax = 20;
  static constexpr uint8_t kFormat = 1;  // bump when door::Card changes: an old list is then ignored

  enum class AddResult { Added, Exists, Full };

  void begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    Preferences p;
    p.begin("door", true);
    const uint8_t format = p.getUChar("format", 0);
    lockdown_ = p.getBool("lockdown", false);
    readerAt_ = p.getUChar("at", 0) == uint8_t(door::Lock::AdminRoom) ? door::Lock::AdminRoom : door::Lock::Front;
    if (format == kFormat) {
      count_ = std::min<int>(p.getUChar("count", 0), kMax);
      if (count_ && p.getBytes("cards", cards_, sizeof(door::Card) * count_) != sizeof(door::Card) * count_) count_ = 0;
    } else if (format != 0) {
      log_w("card list saved by an older firmware (format %u): starting with an empty list", format);
    }
    p.end();
    log_i("card list: %d card%s, lockdown %s, reader at the %s", count_, count_ == 1 ? "" : "s", lockdown_ ? "ON" : "off",
          door::lockTitle(readerAt_));
  }

  int count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }
  uint32_t version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
  }

  bool lockdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lockdown_;
  }
  void setLockdown(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    lockdown_ = on;
    Preferences p;
    p.begin("door", false);
    p.putBool("lockdown", on);
    p.end();
    version_++;
  }

  // Which lock the one reader serves. Saved, so it stays put after a restart.
  door::Lock readerAt() {
    std::lock_guard<std::mutex> lock(mutex_);
    return readerAt_;
  }
  void setReaderAt(door::Lock at) {
    std::lock_guard<std::mutex> lock(mutex_);
    readerAt_ = at;
    Preferences p;
    p.begin("door", false);
    p.putUChar("at", uint8_t(at));
    p.end();
    version_++;
  }

  std::vector<door::Card> list() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<door::Card>(cards_, cards_ + count_);
  }

  bool get(const uint8_t *uid, uint8_t len, door::Card &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(uid, len);
    if (i < 0) return false;
    out = cards_[i];
    return true;
  }

  AddResult add(const door::Card &c) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (find(c.uid, c.uidLen) >= 0) return AddResult::Exists;
    if (count_ >= kMax) return AddResult::Full;
    cards_[count_++] = c;
    save();
    return AddResult::Added;
  }

  // Change one card in place; false when it isn't in the list.
  bool update(const uint8_t *uid, uint8_t len, const std::function<void(door::Card &)> &change, door::Card *after = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(uid, len);
    if (i < 0) return false;
    change(cards_[i]);
    if (after) *after = cards_[i];
    save();
    return true;
  }

  bool remove(const uint8_t *uid, uint8_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(uid, len);
    if (i < 0) return false;
    for (int k = i; k + 1 < count_; k++) cards_[k] = cards_[k + 1];
    count_--;
    save();
    return true;
  }

  // A tap: decide with the list as it is right now, then count it. `out` is the card after the
  // tap (uses already reduced); `known` says whether it was in the list at all.
  door::Decision tap(const uint8_t *uid, uint8_t len, const door::Clock &clk, door::Lock at, door::Card &out, bool &known) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(uid, len);
    known = i >= 0;
    const door::Decision d = door::decide(known ? &cards_[i] : nullptr, clk, lockdown_, at);
    if (!known) return d;
    door::Card &c = cards_[i];
    c.taps++;
    if (clk.synced) c.lastSeen = clk.now;
    if (d == door::Decision::Granted && c.usesLeft > 0) c.usesLeft--;
    out = c;
    save();
    return d;
  }

 private:
  int find(const uint8_t *uid, uint8_t len) const {
    for (int i = 0; i < count_; i++)
      if (cards_[i].uidLen == len && memcmp(cards_[i].uid, uid, len) == 0) return i;
    return -1;
  }

  void save() {
    Preferences p;
    p.begin("door", false);
    p.putUChar("format", kFormat);
    p.putUChar("count", uint8_t(count_));
    if (count_) p.putBytes("cards", cards_, sizeof(door::Card) * count_);
    else if (p.isKey("cards")) p.remove("cards");
    p.end();
    version_++;
  }

  std::mutex mutex_;
  door::Card cards_[kMax];
  int count_ = 0;
  bool lockdown_ = false;
  door::Lock readerAt_ = door::Lock::Front;
  uint32_t version_ = 1;
};
