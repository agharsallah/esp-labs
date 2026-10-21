// B1's ears: a continuous scan that feeds the device table, the per-second air statistics, and
// the "watch" mode that prints every packet of one device (that's how you see its interval).
#pragma once

#include <NimBLEDevice.h>

#include "ble_link.h"
#include "scan_table.h"

namespace b1 {

struct Settings {
  bool active = true;      // ask every device for its scan response too
  uint16_t intervalMs = 100;  // how often the radio starts listening
  uint16_t windowMs = 100;    // and for how long: window/interval = duty cycle
  int nearDbm = -127;      // hide devices weaker than this (-127 = show all)
  int rssiAt1m = -59;      // calibration for the distance guess
};

// The device on the inspect view, by address (rows move around as RSSI changes).
struct Pick {
  uint8_t addr[6] = {};
  uint8_t type = 0;
  bool set = false;
  bool matches(const ble::Heard &h) const { return set && h.addrType == type && memcmp(h.addr, addr, 6) == 0; }
  void take(const ble::Heard &h) {
    memcpy(addr, h.addr, 6);
    type = h.addrType;
    set = true;
  }
};

class Listener : public NimBLEScanCallbacks {
 public:
  ble::ScanTable table;
  Settings settings;

  void begin() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    // wantDuplicates = true: report every packet, not just the first per device, so RSSI stays live.
    scan->setScanCallbacks(this, true);
    // Keep nothing inside NimBLE: our table is the record.
    scan->setMaxResults(0);
    start();
  }

  void start() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->stop();
    scan->setActiveScan(settings.active);
    scan->setInterval(settings.intervalMs);
    scan->setWindow(settings.windowMs);
    // duration 0 = scan until told to stop.
    if (scan->start(0, false, true)) {
      ble::link().edit([](ble::LinkState &s) { s.radio = ble::Radio::Scanning, s.scanning = true; });
      BLE_NOTE(Good, "%s scan, %u/%u ms (%u%% listening)", settings.active ? "active" : "passive", settings.windowMs, settings.intervalMs,
               settings.windowMs * 100 / settings.intervalMs);
    } else {
      BLE_NOTE(Bad, "the scan did not start");
    }
  }

  void stop() {
    NimBLEDevice::getScan()->stop();
    ble::link().edit([](ble::LinkState &s) { s.radio = ble::Radio::Off, s.scanning = false; });
    BLE_NOTE(Info, "scan stopped");
  }

  bool scanning() { return NimBLEDevice::getScan()->isScanning(); }

  void watch(const Pick &p) {
    portENTER_CRITICAL(&lock_);
    watch_ = p;
    watchLast_ = 0;
    portEXIT_CRITICAL(&lock_);
  }
  bool watching() {
    portENTER_CRITICAL(&lock_);
    const bool w = watch_.set;
    portEXIT_CRITICAL(&lock_);
    return w;
  }

  // NimBLE task: every packet heard.
  void onResult(const NimBLEAdvertisedDevice *d) override {
    table.record(d);
    portENTER_CRITICAL(&lock_);
    const bool watched = watch_.set && watch_.type == d->getAddress().getType() && memcmp(watch_.addr, d->getAddress().getVal(), 6) == 0;
    const uint32_t now = millis(), gap = watchLast_ ? now - watchLast_ : 0;
    if (watched) watchLast_ = now;
    portEXIT_CRITICAL(&lock_);
    if (watched) BLE_NOTE(Rx, "+%4lu ms  %d dBm  %u B%s", static_cast<unsigned long>(gap), d->getRSSI(), d->getPayload().size(),
                          d->getPayload().size() > d->getAdvLength() ? " +scan rsp" : "");
  }

  void onScanEnd(const NimBLEScanResults &, int reason) override {
    ble::link().edit([](ble::LinkState &s) { s.radio = ble::Radio::Off, s.scanning = false; });
    BLE_NOTE(Warn, "scan ended (reason %d)", reason);
  }

 private:
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  Pick watch_;
  uint32_t watchLast_ = 0;
};

// Packets per second and devices in range, once a second for the last minute.
struct AirHistory {
  static constexpr int kN = 60;
  uint16_t pps[kN] = {};
  uint8_t devices[kN] = {};
  int head = 0;
  uint32_t lastPackets = 0, at = 0;

  void tick(ble::ScanTable &t, int inRange) {
    const uint32_t now = millis();
    if (now - at < 1000) return;
    at = now;
    const uint32_t p = t.packets();
    pps[head % kN] = uint16_t(std::min<uint32_t>(p - lastPackets, 65535));
    devices[head % kN] = uint8_t(std::min(inRange, 255));
    lastPackets = p;
    head++;
  }
  // i = 0 is the oldest of the last kN seconds.
  uint16_t ppsAt(int i) const { return head < kN ? (i < head ? pps[i] : 0) : pps[(head + i) % kN]; }
  uint8_t devicesAt(int i) const { return head < kN ? (i < head ? devices[i] : 0) : devices[(head + i) % kN]; }
  uint16_t latest() const { return head ? pps[(head - 1) % kN] : 0; }
};

}  // namespace b1
