// The standard BLE-MIDI service: one characteristic, both directions.
//
//   MIDI service         03B80E5A-EDE8-4B33-A751-6CE34EC4C700
//     MIDI I/O           7772E5DB-3868-4112-A1A9-F2669D106BF3   read, write without response, notify
//
// Notes we play go out as notifications; notes the other side plays (GarageBand's keyboard, the
// ble-app piano) come in as writes. Because it's the standard service, macOS, iOS, Android apps
// and Chrome's Web MIDI all treat the board as a MIDI keyboard: no app of ours needed.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "ble_link.h"
#include "midi_ble.h"
#include "server_link.h"

namespace b7 {

constexpr const char *kMidiService = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700";
constexpr const char *kMidiChr = "7772E5DB-3868-4112-A1A9-F2669D106BF3";

struct Stats {
  uint32_t packetsIn = 0, msgsIn = 0, packetsOut = 0, msgsOut = 0;
  uint8_t mostPerPacket = 0;  // messages in one notification: the radio batches what fits
  bool subscribed = false;
};

class MidiService : public NimBLECharacteristicCallbacks {
 public:
  void begin(NimBLEServer *server) {
    NimBLEService *svc = server->createService(kMidiService);
    chr_ = svc->createCharacteristic(kMidiChr, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY, 64);
    chr_->setValue(static_cast<const uint8_t *>(nullptr), 0);  // a read returns an empty packet (spec)
    chr_->setCallbacks(this);
  }

  // The 128-bit service UUID in the advertisement: that's what GarageBand and Audio MIDI Setup
  // look for. The name goes in the scan response.
  void advertise(const char *name) {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData data, response;
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    data.addServiceUUID(NimBLEUUID(kMidiService));
    response.setName(name);
    adv->setAdvertisementData(data);
    adv->setScanResponseData(response);
    if (adv->start()) {
      ble::link().edit([](ble::LinkState &s) { s.radio = ble::Radio::Advertising, s.advertising = true; });
      BLE_NOTE(Info, "advertising as MIDI instrument \"%s\"", name);
    }
  }

  // From loop(): send messages, as few notifications as possible. Returns false if nobody listens.
  bool send(const midi::Msg *m, size_t n) {
    if (!stats().subscribed) return false;
    const ble::LinkState s = ble::link().get();
    const size_t cap = std::min<size_t>(s.peer.mtu > 3 ? s.peer.mtu - 3 : 20, sizeof(buf_));
    while (n) {
      size_t done = 0;
      const size_t len = midi::encode(m, n, buf_, cap, true, &done);
      if (!len || !done) return false;
      chr_->setValue(buf_, len);
      chr_->notify();
      ble::countTx(len);
      portENTER_CRITICAL(&lock_);
      stats_.packetsOut++;
      stats_.msgsOut += done;
      stats_.mostPerPacket = std::max<uint8_t>(stats_.mostPerPacket, uint8_t(done));
      portEXIT_CRITICAL(&lock_);
      m += done;
      n -= done;
    }
    return true;
  }

  // From loop(): what came in since the last call.
  int drain(midi::Msg *out, int max) {
    portENTER_CRITICAL(&lock_);
    int n = 0;
    while (n < max && head_ != tail_) out[n++] = inbox_[head_++ % kInbox];
    portEXIT_CRITICAL(&lock_);
    return n;
  }

  Stats stats() {
    portENTER_CRITICAL(&lock_);
    const Stats s = stats_;
    portEXIT_CRITICAL(&lock_);
    return s;
  }

  // ---- NimBLE task ----------------------------------------------------------------------------

  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    const NimBLEAttValue v = c->getValue();
    midi::Msg msgs[16];
    const int n = decoder_.decode(v.data(), v.size(), msgs, 16);
    ble::countRx(v.size());
    if (n < 0) return BLE_NOTE(Warn, "not a BLE-MIDI packet (%u bytes)", v.size());
    portENTER_CRITICAL(&lock_);
    stats_.packetsIn++;
    stats_.msgsIn += n;
    for (int i = 0; i < n; i++)
      if (tail_ - head_ < uint32_t(kInbox)) inbox_[tail_++ % kInbox] = msgs[i];
    portEXIT_CRITICAL(&lock_);
  }

  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &, uint16_t subValue) override {
    portENTER_CRITICAL(&lock_);
    stats_.subscribed = subValue != 0;
    portEXIT_CRITICAL(&lock_);
    if (subValue) BLE_NOTE(Good, "a synth is listening (subscribed)");
    else BLE_NOTE(Info, "the synth stopped listening");
  }

 private:
  static constexpr int kInbox = 64;
  NimBLECharacteristic *chr_ = nullptr;
  midi::Decoder decoder_;  // only used in the NimBLE task
  uint8_t buf_[64];
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  midi::Msg inbox_[kInbox];
  uint32_t head_ = 0, tail_ = 0;
  Stats stats_;
};

}  // namespace b7
