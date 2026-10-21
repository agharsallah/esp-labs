// The board as a Bluetooth keyboard with media keys: "HID over GATT" (HOGP).
//
// A Bluetooth LE keyboard is just a GATT server with a standard service, HID (1812). Its most
// important characteristic is the Report Map (2A4B): a small program in the HID descriptor
// language that tells the computer what our reports mean. After reading it once, the computer
// subscribes to the Input Report characteristics (2A4D) and we send a notification for every
// change: key down, key up. No driver needed: phones, Macs and PCs all ship one.
//
// Two reports, told apart by their Report ID:
//   1 keyboard  8 bytes: modifiers (one bit each), a reserved 0, up to six keys held at once
//               plus an output report the other way: the computer's Caps/Num Lock lights
//   2 consumer  2 bytes: one "consumer control" usage, 16-bit: play/pause, volume, next track
//
// HID hosts insist on an encrypted link (the input reports are READ_ENC), so the first connection
// pairs and bonds (Just Works + Secure Connections: a keyboard without a screen can't show a code).
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

#include "ble_link.h"
#include "keymap.h"
#include "server_link.h"

namespace b6 {

// Consumer page usages (HID Usage Tables, page 0x0C).
namespace media {
constexpr uint16_t kPlayPause = 0x00CD, kNext = 0x00B5, kPrevious = 0x00B6;
constexpr uint16_t kVolumeUp = 0x00E9, kVolumeDown = 0x00EA, kMute = 0x00E2;
}  // namespace media

inline const char *mediaName(uint16_t u) {
  switch (u) {
    case media::kPlayPause: return "play/pause";
    case media::kNext: return "next track";
    case media::kPrevious: return "previous track";
    case media::kVolumeUp: return "volume up";
    case media::kVolumeDown: return "volume down";
    case media::kMute: return "mute";
    default: return "media key";
  }
}

// The Report Map, byte by byte. Each item is a tag byte (what + how many data bytes) and its data.
static const uint8_t kReportMap[] = {
    // ---- Report 1: a keyboard ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID 1
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI): the 8 modifier keys
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1 bit)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute): byte 0 = one bit per modifier
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant): byte 1 = reserved, always 0
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data, Variable, Absolute): the computer tells us its lock lights
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Constant): pad the lights to a whole byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array): bytes 2-7 = the keys held, by number
    0xC0,              // End Collection
    // ---- Report 2: media keys ----
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID 2
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
    0x75, 0x10,        //   Report Size (16 bits)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array): one usage, 0 = released
    0xC0,              // End Collection
};

// What the keys view draws: the last reports we sent.
struct Sent {
  uint8_t report[8] = {};
  bool consumer = false;   // true = report 2 (the first 2 bytes count)
  bool press = false;      // false = the release that follows
  uint32_t at = 0;
  char shown = 0;          // the character this press types with the current layout, 0 = none
};

class Keyboard : public NimBLECharacteristicCallbacks {
 public:
  void begin(NimBLEServer *server) {
    hid_ = new NimBLEHIDDevice(server);
    hid_->setManufacturer("Hardware Diary bench");
    // PnP ID: source 2 = USB-IF vendor id. 0x303A is Espressif's USB vendor id; product is made up.
    hid_->setPnp(0x02, 0x303A, 0xB006, 0x0100);
    hid_->setHidInfo(0x00, 0x01);  // country 0 (not localised), flags: can wake the host
    hid_->setReportMap(const_cast<uint8_t *>(kReportMap), sizeof(kReportMap));
    keys_ = hid_->getInputReport(1);
    media_ = hid_->getInputReport(2);
    leds_ = hid_->getOutputReport(1);
    leds_->setCallbacks(this);
    hid_->setBatteryLevel(100);  // on USB power
  }

  // Advertise as a keyboard: the HID service and the keyboard appearance (a keyboard icon on the
  // phone's Bluetooth list), the name in the same packet so every host shows it.
  void advertise(const char *name) {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData data;
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    data.setAppearance(HID_KEYBOARD);
    data.addServiceUUID(NimBLEUUID(uint16_t(0x1812)));
    data.setName(name);
    adv->setAdvertisementData(data);
    if (adv->start()) {
      ble::link().edit([](ble::LinkState &s) { s.radio = ble::Radio::Advertising, s.advertising = true; });
      BLE_NOTE(Info, "advertising as keyboard \"%s\"", name);
    }
  }

  // Ready = connected AND encrypted: before that, the host can't read our reports.
  static bool ready() {
    const ble::LinkState s = ble::link().get();
    return s.radio == ble::Radio::Connected && s.peer.encrypted;
  }

  // ---- The typing queue: loop() sends a few events per frame, 8 ms apart ----------------------

  enum class Ev : uint8_t { Down, Up, Media, MediaUp };
  struct Event {
    Ev ev;
    keymap::Stroke stroke;
    uint16_t usage;
    char ch;
  };

  bool tap(keymap::Stroke s, char ch = 0) { return push({Ev::Down, s, 0, ch}) && push({Ev::Up, {}, 0, 0}); }
  bool mediaKey(uint16_t usage) { return push({Ev::Media, {}, usage, 0}) && push({Ev::MediaUp, {}, 0, 0}); }
  int pending() const { return int(tail_ - head_); }
  void clear() { head_ = tail_ = 0; }

  // From loop(): send up to 6 events, 8 ms apart (the host needs to see each key go down and up).
  void pump() {
    for (int n = 0; n < 6 && head_ != tail_; n++) {
      if (n) delay(8);
      send(queue_[head_++ % kQueue]);
    }
    if (head_ == tail_) head_ = tail_ = 0;
  }

  Sent last(int i) const { return sent_[(sentHead_ - 1 - i + kSent * 4) % kSent]; }
  uint32_t sentCount() const { return sentHead_; }
  uint8_t hostLeds() const { return hostLeds_; }

  // The computer writes its lock lights (bit 0 Num, 1 Caps, 2 Scroll) into the output report.
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    const NimBLEAttValue v = c->getValue();
    if (v.size() < 1) return;
    hostLeds_ = v.data()[0];
    ble::countRx(v.size());
    BLE_NOTE(Rx, "host lights: Caps %s, Num %s", hostLeds_ & 2 ? "on" : "off", hostLeds_ & 1 ? "on" : "off");
  }

 private:
  static constexpr int kQueue = 256, kSent = 8;

  bool push(const Event &e) {
    if (tail_ - head_ >= uint32_t(kQueue)) return false;
    queue_[tail_++ % kQueue] = e;
    return true;
  }

  void send(const Event &e) {
    Sent s;
    s.at = millis();
    if (e.ev == Ev::Down || e.ev == Ev::Up) {
      s.press = e.ev == Ev::Down;
      if (s.press) s.report[0] = e.stroke.mod, s.report[2] = e.stroke.key, s.shown = e.ch;
      keys_->setValue(s.report, 8);
      keys_->notify();
      ble::countTx(8);
    } else {
      s.consumer = true;
      s.press = e.ev == Ev::Media;
      if (s.press) s.report[0] = uint8_t(e.usage & 0xFF), s.report[1] = uint8_t(e.usage >> 8);
      media_->setValue(s.report, 2);
      media_->notify();
      ble::countTx(2);
    }
    sent_[sentHead_++ % kSent] = s;
  }

  NimBLEHIDDevice *hid_ = nullptr;
  NimBLECharacteristic *keys_ = nullptr, *media_ = nullptr, *leds_ = nullptr;
  Event queue_[kQueue];
  uint32_t head_ = 0, tail_ = 0;
  Sent sent_[kSent];
  uint32_t sentHead_ = 0;
  volatile uint8_t hostLeds_ = 0;
};

}  // namespace b6
