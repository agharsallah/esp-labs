// R9's ledger: the master secret, the policy and up to 16 provisioned cards, in NVS namespace
// "vault9" (separate from R7's door list and R8's friends), so they survive power-off and
// re-flashing.
//
// The master secret is made once from the ESP32's hardware random number generator and never
// leaves the board: not over serial, not in the API, not in a log. The screen and the API show a
// fingerprint instead (the first 4 bytes of its SHA-256), enough to tell two boards apart.
//
// Two tasks use the ledger (the web server and serial edit it, the reader task counts taps), so
// every method takes the mutex.
#pragma once

#include <Preferences.h>
#include <esp_random.h>

#include <algorithm>
#include <functional>
#include <mutex>
#include <vector>

#include "vault_crypto.h"

class VaultStore {
 public:
  static constexpr int kMax = 16;
  static constexpr uint8_t kFormat = 1;  // bump when LedgerCard changes: an old ledger is then ignored

  void begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    Preferences p;
    p.begin("vault9", false);
    if (p.getBytes("master", master_, 32) != 32) {
      esp_fill_random(master_, 32);
      p.putBytes("master", master_, 32);
      log_i("vault: made a new 256-bit master secret (hardware RNG); it never leaves the board");
    }
    policy_.minClearance = vault::Clearance(std::min<uint8_t>(p.getUChar("minClr", 1), vault::kClearances - 1));
    policy_.strictCloneGuard = p.getBool("strict", true);
    policy_.autoQuarantine = p.getBool("autoQ", true);
    const uint8_t format = p.getUChar("format", 0);
    if (format == kFormat) {
      count_ = std::min<int>(p.getUChar("count", 0), kMax);
      if (count_ && p.getBytes("cards", cards_, sizeof(vault::LedgerCard) * count_) != sizeof(vault::LedgerCard) * count_) count_ = 0;
      nextNumber_ = p.getUChar("next", uint8_t(count_ + 1));
    } else if (format != 0) {
      log_w("vault ledger saved by an older firmware (format %u): starting fresh", format);
    }
    p.end();
    uint8_t h[32];
    vault::Sha256::compute(master_, 32, h);
    vault::toHex(h, 4, fingerprint_, sizeof(fingerprint_));
    log_i("vault: %d card%s, master fingerprint %s", count_, count_ == 1 ? "" : "s", fingerprint_);
  }

  // Only the reader task asks for it, to derive one card's keys. Wipe your copy after use.
  void master(uint8_t out[32]) {
    std::lock_guard<std::mutex> lock(mutex_);
    memcpy(out, master_, 32);
  }
  const char *fingerprint() const { return fingerprint_; }

  vault::Policy policy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return policy_;
  }

  void setPolicy(const vault::Policy &next) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = next;
    Preferences p;
    p.begin("vault9", false);
    p.putUChar("minClr", uint8_t(policy_.minClearance));
    p.putBool("strict", policy_.strictCloneGuard);
    p.putBool("autoQ", policy_.autoQuarantine);
    p.end();
    version_++;
  }

  uint32_t version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
  }

  int count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }

  std::vector<vault::LedgerCard> list() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<vault::LedgerCard>(cards_, cards_ + count_);
  }

  bool get(const uint8_t *uid, uint8_t len, vault::LedgerCard &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(uid, len);
    if (i < 0) return false;
    out = cards_[i];
    return true;
  }

  bool byNumber(int number, vault::LedgerCard &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < count_; i++)
      if (cards_[i].number == number) return out = cards_[i], true;
    return false;
  }

  // Add a card, or replace the one with the same UID (it keeps its number). false when full.
  bool put(vault::LedgerCard card) {
    std::lock_guard<std::mutex> lock(mutex_);
    int i = find(card.uid, card.uidLen);
    if (i >= 0) {
      card.number = cards_[i].number;
    } else {
      if (count_ >= kMax) return false;
      i = count_++;
      card.number = nextNumber_++;
    }
    cards_[i] = card;
    save();
    return true;
  }

  bool update(const uint8_t *uid, uint8_t len, const std::function<void(vault::LedgerCard &)> &change) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(uid, len);
    if (i < 0) return false;
    change(cards_[i]);
    save();
    return true;
  }

  bool remove(const uint8_t *uid, uint8_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int i = find(uid, len);
    if (i < 0) return false;
    for (int k = i; k + 1 < count_; ++k) cards_[k] = cards_[k + 1];
    count_--;
    if (count_ == 0) nextNumber_ = 1;
    save();
    return true;
  }

  bool full() {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ >= kMax;
  }

 private:
  int find(const uint8_t *uid, uint8_t len) const {
    for (int i = 0; i < count_; ++i)
      if (cards_[i].uidLen == len && memcmp(cards_[i].uid, uid, len) == 0) return i;
    return -1;
  }

  void save() {
    Preferences p;
    p.begin("vault9", false);
    p.putUChar("format", kFormat);
    p.putUChar("count", uint8_t(count_));
    p.putUChar("next", nextNumber_);
    if (count_) p.putBytes("cards", cards_, sizeof(vault::LedgerCard) * count_);
    else p.remove("cards");
    p.end();
    version_++;
  }

  std::mutex mutex_;
  uint8_t master_[32] = {};
  char fingerprint_[9] = "";
  vault::Policy policy_;
  vault::LedgerCard cards_[kMax];
  int count_ = 0;
  uint8_t nextNumber_ = 1;
  uint32_t version_ = 1;
};
