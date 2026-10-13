// Every device the radio has heard, kept by the scan callback and read by the screen.
//
// NimBLE calls onResult() in its own task for every advertisement it hears (with duplicates on,
// that's every packet, many per second per device). Each one updates one row here: the newest
// payload, a smoothed RSSI and a packet counter. The screen takes a sorted copy.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <algorithm>

#include "ad_format.h"
#include "ble_link.h"

namespace ble {

struct Heard {
  uint8_t addr[6] = {};  // little-endian, as on air
  uint8_t addrType = 0;
  char name[24] = "";
  int8_t rssi = -127;      // newest
  float rssiAvg = -127;    // smoothed, what the radar uses
  int8_t txPower = 127;    // 127 = not advertised
  uint16_t company = 0;    // 0 = no manufacturer data
  uint16_t appearance = 0;
  ad::Kind kind = ad::Kind::Unknown;
  bool connectable = false;
  bool scanResponse = false;   // answered an active scan
  uint8_t adv[31] = {}, advLen = 0;
  uint8_t rsp[31] = {}, rspLen = 0;
  uint32_t firstSeen = 0, lastSeen = 0, packets = 0;
  uint32_t lastInterval = 0;   // ms between the last two packets

  void addrText(char *out, size_t n) const {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  }
  ad::AddrKind addrKind() const { return ad::addrKind(addrType, addr[5]); }
  const char *label() const {
    if (name[0]) return name;
    if (const char *c = ad::companyName(company)) return c;
    return ad::kindName(kind);
  }
};

class ScanTable {
 public:
  static constexpr int kMax = 48;

  // From the NimBLE task.
  void record(const NimBLEAdvertisedDevice *d) {
    const uint8_t *a = d->getAddress().getVal();
    const std::vector<uint8_t> &payload = d->getPayload();
    const uint32_t now = millis();
    portENTER_CRITICAL(&lock_);
    Heard *h = findLocked(a, d->getAddress().getType());
    if (!h) h = addLocked(a, d->getAddress().getType(), now);
    if (h) {
      h->lastInterval = h->packets ? now - h->lastSeen : 0;
      h->lastSeen = now;
      h->packets++;
      h->rssi = d->getRSSI();
      h->rssiAvg = h->rssiAvg < -126 ? h->rssi : h->rssiAvg * 0.8f + h->rssi * 0.2f;
      h->connectable = d->isConnectable();
      // NimBLE appends the scan response after the advertisement in getPayload(); getAdvLength()
      // says where the advertisement ends. A lone scan response arrives as its own report.
      const size_t advLen = std::min<size_t>(payload.size(), d->getAdvLength());
      const bool responseOnly = payload.size() <= advLen && d->getAdvType() == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
      if (!responseOnly) {
        h->advLen = uint8_t(std::min<size_t>(advLen, 31));
        memcpy(h->adv, payload.data(), h->advLen);
      }
      const size_t rspFrom = responseOnly ? 0 : advLen;
      if (payload.size() > rspFrom) {
        h->scanResponse = true;
        h->rspLen = uint8_t(std::min<size_t>(payload.size() - rspFrom, 31));
        memcpy(h->rsp, payload.data() + rspFrom, h->rspLen);
      }
      if (d->haveName()) strlcpy(h->name, d->getName().c_str(), sizeof(h->name));
      if (d->haveTXPower()) h->txPower = d->getTXPower();
      if (d->haveAppearance()) h->appearance = d->getAppearance();
      if (d->haveManufacturerData()) {
        const std::string m = d->getManufacturerData();
        if (m.size() >= 2) h->company = uint16_t(uint8_t(m[0]) | (uint8_t(m[1]) << 8));
      }
      const ad::Kind k = ad::guess(h->adv, h->advLen);
      h->kind = k != ad::Kind::Unknown ? k : ad::guess(h->rsp, h->rspLen);
    }
    packets_++;
    portEXIT_CRITICAL(&lock_);
  }

  // Up to max rows, sorted by smoothed RSSI (strongest first), seen within the last maxAgeMs.
  int snapshot(Heard *out, int max, uint32_t maxAgeMs = 30000, int nearDbm = -127) {
    const uint32_t now = millis();
    int n = 0;
    portENTER_CRITICAL(&lock_);
    for (int i = 0; i < count_ && n < max; i++) {
      const Heard &h = rows_[i];
      if (now - h.lastSeen > maxAgeMs || h.rssiAvg < nearDbm) continue;
      out[n++] = h;
    }
    portEXIT_CRITICAL(&lock_);
    std::sort(out, out + n, [](const Heard &a, const Heard &b) { return a.rssiAvg > b.rssiAvg; });
    return n;
  }

  uint32_t packets() {
    portENTER_CRITICAL(&lock_);
    const uint32_t p = packets_;
    portEXIT_CRITICAL(&lock_);
    return p;
  }
  int size() {
    portENTER_CRITICAL(&lock_);
    const int n = count_;
    portEXIT_CRITICAL(&lock_);
    return n;
  }

  void clear() {
    portENTER_CRITICAL(&lock_);
    count_ = 0;
    portEXIT_CRITICAL(&lock_);
  }

 private:
  Heard *findLocked(const uint8_t *a, uint8_t type) {
    for (int i = 0; i < count_; i++)
      if (rows_[i].addrType == type && memcmp(rows_[i].addr, a, 6) == 0) return &rows_[i];
    return nullptr;
  }

  // A full table drops the device heard least recently: phones change private addresses every
  // ~15 minutes, so old rows are often the same phone under a previous name.
  Heard *addLocked(const uint8_t *a, uint8_t type, uint32_t now) {
    int slot = count_;
    if (count_ == kMax) {
      slot = 0;
      for (int i = 1; i < kMax; i++)
        if (rows_[i].lastSeen < rows_[slot].lastSeen) slot = i;
    } else {
      count_++;
    }
    rows_[slot] = Heard{};
    memcpy(rows_[slot].addr, a, 6);
    rows_[slot].addrType = type;
    rows_[slot].firstSeen = now;
    return &rows_[slot];
  }

  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  Heard rows_[kMax];
  int count_ = 0;
  uint32_t packets_ = 0;
};

}  // namespace ble
