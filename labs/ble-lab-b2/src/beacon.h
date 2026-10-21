// B2's mouth: turns the settings into the two packets (beacon_payload.h) and hands them to the radio.
//
// The controller (the radio's own little processor) does the shouting by itself: once advertising
// starts, it sends the advertisement on channels 37, 38 and 39, waits the interval plus a random
// 0-10 ms ("advDelay", so two beacons never stay in lockstep), and repeats, with no CPU help.
// We only replace the data once a second so the live values stay fresh.
#pragma once

#include <NimBLEDevice.h>

#include "beacon_payload.h"
#include "ble_link.h"

namespace b2 {

struct Settings {
  Mode mode = Mode::Bench;
  char url[40] = "https://espressif.com";
  uint16_t intervalMs = 100;
  bool connectable = true;
  bool scanResponse = true;
  bool on = true;
  IBeacon beacon;
};

// What kind of advertising PDU the controller sends, from our settings.
inline const char *pduName(const Settings &s) {
  if (s.connectable) return "ADV_IND";             // connectable + scannable
  if (s.scanResponse) return "ADV_SCAN_IND";       // scannable only
  return "ADV_NONCONN_IND";                        // a pure broadcast: nobody may ask anything
}

// Bluetooth 4.x set the floor at 100 ms for non-connectable adverts, 20 ms for connectable ones.
// Bluetooth 5 allows 20 ms for every type, but beacons still keep 100 ms: older phones and the old
// rule. We keep the old floors so the lab shows the difference.
inline uint16_t minIntervalMs(const Settings &s) { return s.connectable ? 20 : 100; }

class Beacon {
 public:
  Settings settings;
  Live live;
  Packet adv, rsp;
  // millis() when each byte of the advertisement last changed: the packet view flashes them.
  uint32_t changedAt[31] = {};
  uint32_t updates = 0;

  // Rebuild the packets from settings + live values. Restart = also new parameters (interval,
  // connectable): the controller only takes those while stopped.
  bool apply(bool restart) {
    Config c;
    c.mode = settings.mode;
    c.live = live;
    c.beacon = settings.beacon;
    c.url = settings.url;
    c.txPower = int8_t(NimBLEDevice::getPower());
    c.scanResponse = settings.scanResponse;
    Packet a, r;
    const char *why = nullptr;
    if (!build(c, a, r, &why)) {
      BLE_NOTE(Bad, "%s", why);
      return false;
    }
    const uint32_t now = millis();
    for (int i = 0; i < 31; i++)
      if (i >= a.n || i >= adv.n || a.b[i] != adv.b[i]) changedAt[i] = (i < a.n) ? now : 0;
    adv = a;
    rsp = r;
    updates++;

    NimBLEAdvertising *ad = NimBLEDevice::getAdvertising();
    if (restart) {
      ad->stop();
      ad->setConnectableMode(settings.connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON);
      // Discoverable = scannable when not connectable; NON + NON gives ADV_NONCONN_IND.
      ad->setDiscoverableMode(settings.connectable || settings.scanResponse ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON);
      const uint16_t units = uint16_t(settings.intervalMs * 1000 / 625);  // 0.625 ms units
      ad->setMinInterval(units);
      ad->setMaxInterval(units);
      ad->enableScanResponse(settings.scanResponse && rsp.n > 0);
    }
    NimBLEAdvertisementData data, response;
    data.addData(adv.b, adv.n);
    ad->setAdvertisementData(data);
    if (settings.scanResponse && rsp.n > 0) {
      response.addData(rsp.b, rsp.n);
      ad->setScanResponseData(response);
    } else {
      ble_gap_adv_rsp_set_data(nullptr, 0);  // an empty answer
    }
    if (restart && settings.on) {
      if (!ad->start()) {
        BLE_NOTE(Bad, "advertising did not start");
        return false;
      }
    }
    ble::link().edit([&](ble::LinkState &s) {
      if (s.radio != ble::Radio::Connected) s.radio = settings.on ? ble::Radio::Advertising : ble::Radio::Off;
      s.advertising = settings.on;
    });
    if (restart) BLE_NOTE(Good, "%s %s every %u ms, %u+%u B", modeName(settings.mode), pduName(settings), settings.intervalMs, adv.n, rsp.n);
    return true;
  }

  void stop() {
    NimBLEDevice::getAdvertising()->stop();
    ble::link().edit([](ble::LinkState &s) {
      s.advertising = false;
      if (s.radio == ble::Radio::Advertising) s.radio = ble::Radio::Off;
    });
    BLE_NOTE(Info, "advertising off: the board is invisible");
  }

  bool advertising() { return NimBLEDevice::getAdvertising()->isAdvertising(); }
};

}  // namespace b2
