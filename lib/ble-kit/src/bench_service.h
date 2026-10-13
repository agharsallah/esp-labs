// The Bench service: our own GATT service, met in B3 and reused by B4 and B5.
//
// GATT is a small database on the board. A phone connects, discovers it, and then reads, writes
// or subscribes to its entries (characteristics). Each one has a UUID, properties (read, write,
// notify...) and a value:
//
//   Bench service      ba5e0000-b1e5-4ab1-9e5c-be7cbe7cb0a7
//     LED              ...0001  read, write          3 bytes: red, green, blue
//     Message          ...0002  read, write          up to 40 bytes of UTF-8 text, shown on screen
//     Button           ...0003  read, notify         gesture (1 click, 2 double, 3 long), count (2 B)
//     Temperature      ...0004  read, notify         °C x 100, signed 16-bit (the chip's own sensor)
//     Uptime           ...0005  read                 seconds since power-on, 32-bit
//     Vault (B4)       ...0006  read, write          40 bytes, only over an encrypted, authenticated link
//   Device Information 180A   (standard: who made this)
//   Battery            180F   (standard: 100 %, we're on USB)
//
// Every number is little-endian, like everything else in Bluetooth LE.
#pragma once

#include <Arduino.h>
#include <NimBLE2904.h>
#include <NimBLEDevice.h>

#include "ble_link.h"
#include "server_link.h"

namespace bench {

constexpr const char *kServiceUuid = "ba5e0000-b1e5-4ab1-9e5c-be7cbe7cb0a7";

// "ba5e0001-b1e5-..." for characteristic 1, and so on.
inline NimBLEUUID uuid(uint16_t n) {
  char s[40];
  snprintf(s, sizeof(s), "ba5e%04x-b1e5-4ab1-9e5c-be7cbe7cb0a7", n);
  return NimBLEUUID(s);
}

enum Chr : uint8_t { Led, Message, ButtonChr, Temperature, Uptime, Vault, kChrCount };

inline const char *chrName(int c) {
  static const char *const names[] = {"LED", "Message", "Button", "Temperature", "Uptime", "Vault"};
  return names[c];
}

enum class Op : uint8_t { None, Read, Write, Notify, Subscribe, Denied };

struct Options {
  bool vault = false;         // B4: add the encrypted + authenticated Vault
  bool lockControls = false;  // B4 `lock on`: LED and Message need an encrypted link too
};

// What the screen shows: a copy, taken under the lock.
struct State {
  uint8_t led[3] = {0, 0, 0};
  bool ledWritten = false;
  char message[41] = "";
  uint32_t messageAt = 0;
  float tempC = 0;
  uint16_t buttonCount = 0;
  uint8_t lastGesture = 0;
  char vault[41] = "nothing here yet";
  bool subscribed[kChrCount] = {};
  uint32_t opAt[kChrCount] = {};  // millis() of the last operation, for the pulse on screen
  Op op[kChrCount] = {};
  bool hasVault = false;
};

class BenchService : public NimBLECharacteristicCallbacks {
 public:
  void begin(NimBLEServer *server, const Options &o) {
    opts_ = o;
    state_.hasVault = o.vault;
    NimBLEService *svc = server->createService(kServiceUuid);
    service_ = svc;
    const uint16_t lock = o.lockControls ? (NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_ENC) : 0;

    chr_[Led] = svc->createCharacteristic(uuid(1), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | lock, 3);
    describe(chr_[Led], "LED colour: red, green, blue");
    chr_[Message] = svc->createCharacteristic(uuid(2), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | lock, 40);
    describe(chr_[Message], "Message for the screen");
    chr_[ButtonChr] = svc->createCharacteristic(uuid(3), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 3);
    describe(chr_[ButtonChr], "Button: gesture, count");
    chr_[Temperature] = svc->createCharacteristic(uuid(4), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 2);
    describe(chr_[Temperature], "Chip temperature");
    // 2904 = Presentation Format: tells generic apps "signed 16-bit, x 10^-2, degrees Celsius".
    NimBLE2904 *fmt = chr_[Temperature]->create2904();
    fmt->setFormat(NimBLE2904::FORMAT_SINT16);
    fmt->setExponent(-2);
    fmt->setUnit(0x272F);  // degree Celsius
    chr_[Uptime] = svc->createCharacteristic(uuid(5), NIMBLE_PROPERTY::READ, 4);
    describe(chr_[Uptime], "Uptime in seconds");
    NimBLE2904 *up = chr_[Uptime]->create2904();
    up->setFormat(NimBLE2904::FORMAT_UINT32);
    up->setUnit(0x2703);  // second
    if (o.vault) {
      // READ_AUTHEN / WRITE_AUTHEN: encrypted AND paired with protection against a man in the middle
      // (passkey or numeric comparison). A Just Works link is refused.
      chr_[Vault] = svc->createCharacteristic(uuid(6), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ_ENC |
                                                           NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN,
                                              40);
      describe(chr_[Vault], "Vault (pair first)");
      chr_[Vault]->setValue(state_.vault);
    }
    for (int i = 0; i < kChrCount; i++)
      if (chr_[i]) chr_[i]->setCallbacks(this);
    chr_[Led]->setValue(state_.led, 3);
    chr_[Message]->setValue("");
    const uint8_t button[3] = {0, 0, 0};
    chr_[ButtonChr]->setValue(button, 3);

    // The two standard services, so generic apps show something friendly.
    NimBLEService *dis = server->createService("180A");
    dis->createCharacteristic("2A29", NIMBLE_PROPERTY::READ)->setValue("Hardware Diary bench");
    dis->createCharacteristic("2A24", NIMBLE_PROPERTY::READ)->setValue("ESP32-S3 N16R8");
    dis->createCharacteristic("2A26", NIMBLE_PROPERTY::READ)->setValue("ble-kit 0.1");
    NimBLEService *bat = server->createService("180F");
    NimBLECharacteristic *level = bat->createCharacteristic("2A19", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 1);
    const uint8_t full = 100;
    level->setValue(&full, 1);
  }

  NimBLEService *service() { return service_; }
  NimBLECharacteristic *characteristic(Chr c) { return chr_[c]; }

  // From loop(): temperature every 2 s (sent only if someone subscribed).
  void tick() {
    if (millis() - tempAt_ < 2000) return;
    tempAt_ = millis();
    const float c = temperatureRead();
    const int16_t centi = int16_t(c * 100);
    chr_[Temperature]->setValue(reinterpret_cast<const uint8_t *>(&centi), 2);
    bool sub;
    portENTER_CRITICAL(&lock_);
    state_.tempC = c;
    sub = state_.subscribed[Temperature];
    portEXIT_CRITICAL(&lock_);
    if (sub && chr_[Temperature]->notify()) noteOp(Temperature, Op::Notify), ble::countTx(2);
  }

  // From loop(): the button. gesture 1 click, 2 double-click, 3 long press.
  void button(uint8_t gesture) {
    uint16_t count;
    bool sub;
    portENTER_CRITICAL(&lock_);
    count = ++state_.buttonCount;
    state_.lastGesture = gesture;
    sub = state_.subscribed[ButtonChr];
    portEXIT_CRITICAL(&lock_);
    const uint8_t v[3] = {gesture, uint8_t(count & 0xFF), uint8_t(count >> 8)};
    chr_[ButtonChr]->setValue(v, 3);
    static const char *const names[] = {"", "click", "double", "long"};
    if (sub && chr_[ButtonChr]->notify()) {
      noteOp(ButtonChr, Op::Notify);
      ble::countTx(3);
      BLE_NOTE(Tx, "notify Button: %s #%u", names[gesture & 3], count);
    } else {
      BLE_NOTE(Info, "button %s #%u (nobody subscribed)", names[gesture & 3], count);
    }
  }

  // Serial `message`: change the value from the board's side too.
  void setMessage(const char *text) {
    chr_[Message]->setValue(text);
    portENTER_CRITICAL(&lock_);
    strlcpy(state_.message, text, sizeof(state_.message));
    state_.messageAt = millis();
    portEXIT_CRITICAL(&lock_);
  }

  State state() {
    portENTER_CRITICAL(&lock_);
    State s = state_;
    portEXIT_CRITICAL(&lock_);
    return s;
  }

  // From loop(): true once per new LED colour written by the phone.
  bool takeLed(uint8_t rgbOut[3]) {
    portENTER_CRITICAL(&lock_);
    const bool fresh = ledFresh_;
    ledFresh_ = false;
    memcpy(rgbOut, state_.led, 3);
    portEXIT_CRITICAL(&lock_);
    return fresh;
  }

  void setVault(const char *text) {
    if (!chr_[Vault]) return;
    chr_[Vault]->setValue(text);
    portENTER_CRITICAL(&lock_);
    strlcpy(state_.vault, text, sizeof(state_.vault));
    portEXIT_CRITICAL(&lock_);
  }

  // ---- NimBLE callbacks (NimBLE task) --------------------------------------------------------

  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    const int i = indexOf(c);
    if (i == Uptime) {
      const uint32_t s = millis() / 1000;
      c->setValue(reinterpret_cast<const uint8_t *>(&s), 4);
    }
    noteOp(i, Op::Read);
    ble::countRx(0);
    BLE_NOTE(Rx, "read %s", chrName(i));
  }

  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    const int i = indexOf(c);
    const NimBLEAttValue v = c->getValue();
    ble::countRx(v.size());
    noteOp(i, Op::Write);
    if (i == Led) {
      if (v.size() != 3) return BLE_NOTE(Warn, "LED wants 3 bytes, got %u", v.size());
      portENTER_CRITICAL(&lock_);
      memcpy(state_.led, v.data(), 3);
      state_.ledWritten = true;
      portEXIT_CRITICAL(&lock_);
      ledFresh_ = true;  // loop() lights it (takeLed): only one task ever drives the LED
      BLE_NOTE(Rx, "write LED %02X %02X %02X", v.data()[0], v.data()[1], v.data()[2]);
    } else if (i == Message) {
      char text[41];
      const size_t n = std::min<size_t>(v.size(), 40);
      memcpy(text, v.data(), n);
      text[n] = 0;
      portENTER_CRITICAL(&lock_);
      strlcpy(state_.message, text, sizeof(state_.message));
      state_.messageAt = millis();
      portEXIT_CRITICAL(&lock_);
      BLE_NOTE(Rx, "write Message \"%.30s\"", text);
    } else if (i == Vault) {
      char text[41];
      const size_t n = std::min<size_t>(v.size(), 40);
      memcpy(text, v.data(), n);
      text[n] = 0;
      portENTER_CRITICAL(&lock_);
      strlcpy(state_.vault, text, sizeof(state_.vault));
      portEXIT_CRITICAL(&lock_);
      BLE_NOTE(Rx, "write Vault (%u bytes, encrypted)", n);
    }
  }

  // subValue: 0 off, 1 notifications, 2 indications, 3 both. The phone writes it into the
  // characteristic's CCCD (descriptor 2902).
  void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &, uint16_t subValue) override {
    const int i = indexOf(c);
    if (i < 0) return;
    portENTER_CRITICAL(&lock_);
    state_.subscribed[i] = subValue != 0;
    portEXIT_CRITICAL(&lock_);
    noteOp(i, Op::Subscribe);
    BLE_NOTE(Rx, "%s %s (CCCD = %u)", subValue ? "subscribe" : "unsubscribe", chrName(i), subValue);
  }

  // Errors on our own sends. A read or write refused for missing encryption usually never gets
  // here: the NimBLE host checks the permission itself and answers the phone with "Insufficient
  // Authentication" (the phone then starts pairing). Kept for the cases that do arrive.
  void onStatus(NimBLECharacteristic *c, NimBLEConnInfo &, int code) override {
    const int i = indexOf(c);
    if (code == 0 || i < 0) return;
    if (code == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) || code == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) {
      noteOp(i, Op::Denied);
      BLE_NOTE(Bad, "%s refused: pair first", chrName(i));
    }
  }

 private:
  static void describe(NimBLECharacteristic *c, const char *text) {
    // 2901 = User Description: the name nRF Connect and LightBlue show instead of the UUID.
    c->createDescriptor("2901", NIMBLE_PROPERTY::READ, strlen(text))->setValue(text);
  }

  int indexOf(const NimBLECharacteristic *c) const {
    for (int i = 0; i < kChrCount; i++)
      if (chr_[i] == c) return i;
    return -1;
  }

  void noteOp(int i, Op op) {
    if (i < 0) return;
    portENTER_CRITICAL(&lock_);
    state_.op[i] = op;
    state_.opAt[i] = millis();
    portEXIT_CRITICAL(&lock_);
  }

  Options opts_;
  NimBLEService *service_ = nullptr;
  NimBLECharacteristic *chr_[kChrCount] = {};
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  State state_;
  uint32_t tempAt_ = 0;
  bool ledFresh_ = false;
};

// Advertise the Bench service: flags + the 128-bit service UUID in the advertisement (21 of 31
// bytes), the name in the scan response. Phones filter by the UUID, people read the name.
inline void advertise(const char *name, bool start = true) {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData data, response;
  data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  data.addServiceUUID(NimBLEUUID(kServiceUuid));
  response.setName(name);
  response.setAppearance(0x0540);  // generic sensor
  adv->setAdvertisementData(data);
  adv->setScanResponseData(response);
  if (start && adv->start()) {
    ble::link().edit([](ble::LinkState &s) { s.radio = ble::Radio::Advertising, s.advertising = true; });
    BLE_NOTE(Info, "advertising as \"%s\"", name);
  }
}

}  // namespace bench
