// R8's card list: which friend lives on which card. Up to 12 cards, kept in NVS (namespace
// "friends", separate from R7's door list) so they survive power-off and re-flashing.
//
// A card the board has never seen is added by itself on its first tap, with no friend yet: the
// app then shows an empty card that a friend can be dragged onto. The card only brings its UID;
// the board remembers who lives there.
//
// Two tasks use the list (the web server edits it, the reader task counts taps), so every method
// takes the mutex. The UID helpers come from R7 (access_rules.h): same hex format in both labs.
#pragma once

#include <Preferences.h>

#include <functional>
#include <mutex>
#include <vector>

#include "access_rules.h"
#include "characters.h"

namespace friends {

using door::uidFromHex;
using door::uidToHex;

struct Card {
  uint8_t uid[10] = {};
  uint8_t uidLen = 0;
  uint8_t number = 0;       // 1, 2, 3 ... in the order the cards were first tapped
  char character[12] = "";  // a cast id ("fox"), "" = nobody lives here yet
  char name[16] = "";       // the friend's name on this card, "" = the character's own name
  uint32_t taps = 0;
};

class List {
 public:
  static constexpr int kMax = 12;
  static constexpr uint8_t kFormat = 1;  // bump when Card changes: an old list is then ignored

  enum class TapResult { Known, Added, Full };

  void begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    Preferences p;
    p.begin("friends", true);
    const uint8_t format = p.getUChar("format", 0);
    if (format == kFormat) {
      count_ = std::min<int>(p.getUChar("count", 0), kMax);
      if (count_ && p.getBytes("cards", cards_, sizeof(Card) * count_) != sizeof(Card) * count_) count_ = 0;
      nextNumber_ = p.getUChar("next", uint8_t(count_ + 1));
    } else if (format != 0) {
      log_w("friend list saved by an older firmware (format %u): starting fresh", format);
    }
    p.end();
    log_i("friend list: %d card%s", count_, count_ == 1 ? "" : "s");
  }

  int count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }
  uint32_t version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
  }

  std::vector<Card> list() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Card>(cards_, cards_ + count_);
  }

  bool get(const uint8_t *uid, uint8_t len, Card &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(uid, len);
    if (i < 0) return false;
    out = cards_[i];
    return true;
  }

  // A tap: count it, or add the card when it is new. `out` is the card after the tap.
  TapResult tap(const uint8_t *uid, uint8_t len, Card &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    int i = find(uid, len);
    TapResult r = TapResult::Known;
    if (i < 0) {
      if (count_ >= kMax) return TapResult::Full;
      i = count_++;
      cards_[i] = Card{};
      memcpy(cards_[i].uid, uid, len);
      cards_[i].uidLen = len;
      cards_[i].number = nextNumber_++;
      r = TapResult::Added;
    }
    cards_[i].taps++;
    out = cards_[i];
    save();
    return r;
  }

  // Change one card in place; false when it isn't in the list.
  bool update(const uint8_t *uid, uint8_t len, const std::function<void(Card &)> &change, Card *after = nullptr) {
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
    if (count_ == 0) nextNumber_ = 1;
    save();
    return true;
  }

 private:
  int find(const uint8_t *uid, uint8_t len) const {
    for (int i = 0; i < count_; i++)
      if (cards_[i].uidLen == len && memcmp(cards_[i].uid, uid, len) == 0) return i;
    return -1;
  }

  void save() {
    Preferences p;
    p.begin("friends", false);
    p.putUChar("format", kFormat);
    p.putUChar("count", uint8_t(count_));
    p.putUChar("next", nextNumber_);
    if (count_) p.putBytes("cards", cards_, sizeof(Card) * count_);
    else if (p.isKey("cards")) p.remove("cards");
    p.end();
    version_++;
  }

  std::mutex mutex_;
  Card cards_[kMax];
  int count_ = 0;
  uint8_t nextNumber_ = 1;
  uint32_t version_ = 1;
};

}  // namespace friends
