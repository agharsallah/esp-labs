// What every BLE lab knows about its radio, shared between two tasks.
//
// NimBLE runs its own task (the "host", on core 0). Every callback it makes (a device was heard,
// a phone connected, a value was written) runs there. The screen and serial run in loop() on
// core 1. So the callbacks never draw or print: they update this state and push a line into the
// activity log, and loop() takes a copy to show it. A spinlock guards each copy (a few hundred
// bytes, microseconds), the same idea as the RFID labs' reader snapshot.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include <stdarg.h>

namespace ble {

// What the radio is doing right now, for the status bar and the on-board LED.
enum class Radio : uint8_t { Off, Scanning, Advertising, Connecting, Connected };

inline const char *radioName(Radio r) {
  static const char *const names[] = {"off", "scanning", "advertising", "connecting", "connected"};
  return names[static_cast<int>(r)];
}

// The other side of a connection: a phone, a laptop, a keyboard.
struct Peer {
  char addr[18] = "";        // as the radio sees it right now (may be a private, changing address)
  char idAddr[18] = "";      // the identity address, once bonding revealed it
  char name[24] = "";        // when we know it (scan response, GAP name, or B8's discovery)
  uint8_t addrType = 0;      // 0 public, 1 random
  uint16_t handle = 0xFFFF;  // connection handle
  uint16_t mtu = 23;         // bytes per ATT packet; 23 until both sides agree on more
  uint16_t interval = 0;     // connection interval, in 1.25 ms units
  uint16_t latency = 0;      // events the peripheral may skip
  uint16_t timeout = 0;      // supervision timeout, in 10 ms units
  uint8_t txPhy = 1, rxPhy = 1;  // 1 = 1M, 2 = 2M, 3 = Coded
  int8_t rssi = 0;           // measured on the live connection (0 = not measured yet)
  bool encrypted = false, authenticated = false, bonded = false;
  uint8_t keySize = 0;
  bool central = false;      // true when the PEER is the central (we are the peripheral)
};

struct LinkState {
  Radio radio = Radio::Off;
  bool advertising = false;
  bool scanning = false;
  uint8_t connections = 0;
  Peer peer;                     // the most recent connection
  uint32_t connectedAt = 0;      // millis()
  int lastDisconnect = -1;       // reason code of the last disconnect, -1 = none yet
  uint32_t rxOps = 0, txOps = 0;      // writes/reads received, notifications/indications sent
  uint32_t rxBytes = 0, txBytes = 0;
  uint32_t passkey = 0;          // non-zero while a passkey is on screen (B4, B8)
  bool passkeyCompare = false;   // true = numeric comparison (press to accept), false = type it in
  uint32_t passkeySince = 0;
  uint32_t version = 0;          // bumps on every change
};

// The shared state, one per program. Callbacks call ble::link().edit([](LinkState &s){ ... }).
class Link {
 public:
  template <typename F>
  void edit(F &&change) {
    portENTER_CRITICAL(&lock_);
    change(state_);
    state_.version++;
    portEXIT_CRITICAL(&lock_);
  }
  LinkState get() {
    portENTER_CRITICAL(&lock_);
    LinkState copy = state_;
    portEXIT_CRITICAL(&lock_);
    return copy;
  }

 private:
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  LinkState state_;
};

inline Link &link() {
  static Link l;
  return l;
}

// Every operation that crosses the air, for the traffic counters on the link view.
inline void countRx(size_t bytes) {
  link().edit([&](LinkState &s) { s.rxOps++, s.rxBytes += bytes; });
}
inline void countTx(size_t bytes) {
  link().edit([&](LinkState &s) { s.txOps++, s.txBytes += bytes; });
}

// ---- The activity log ------------------------------------------------------------------------
// Every lab writes what happens on the air here: "→ notify button click", "← write LED ff8000".
// loop() prints new lines to serial and the log view draws the last few.

enum class Act : uint8_t { Info, Good, Warn, Bad, Rx, Tx };

struct ActEntry {
  uint32_t ms = 0;
  Act kind = Act::Info;
  char text[60] = "";
};

class Activity {
 public:
  static constexpr int kSize = 32;

  [[gnu::format(printf, 3, 4)]] void add(Act kind, const char *fmt, ...) {
    ActEntry e;
    e.ms = millis();
    e.kind = kind;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, ap);  // formatted outside the lock
    va_end(ap);
    portENTER_CRITICAL(&lock_);
    ring_[head_ % kSize] = e;
    head_++;
    portEXIT_CRITICAL(&lock_);
  }

  uint32_t head() {
    portENTER_CRITICAL(&lock_);
    const uint32_t h = head_;
    portEXIT_CRITICAL(&lock_);
    return h;
  }

  // Entry number i (0 = the first ever). False when it has already been overwritten.
  bool get(uint32_t i, ActEntry &out) {
    portENTER_CRITICAL(&lock_);
    const bool ok = i < head_ && head_ - i <= kSize;
    if (ok) out = ring_[i % kSize];
    portEXIT_CRITICAL(&lock_);
    return ok;
  }

 private:
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  ActEntry ring_[kSize];
  uint32_t head_ = 0;
};

inline Activity &activity() {
  static Activity a;
  return a;
}

// Short names for the callbacks.
#define BLE_NOTE(kind, ...) ::ble::activity().add(::ble::Act::kind, __VA_ARGS__)

// Why a link ended, in words. NimBLE reports HCI reasons as 0x200 + the HCI code.
inline const char *disconnectReason(int reason) {
  switch (reason & 0xFF) {
    case 0x08: return "timeout: the peer went out of range or stopped answering";
    case 0x13: return "the peer hung up";
    case 0x16: return "we hung up";
    case 0x3B: return "unacceptable connection parameters";
    case 0x3D: return "MIC failure (encryption keys did not match)";
    case 0x3E: return "the connection never got established";
    case 0x05: return "authentication failure (forgotten bond on one side?)";
    case 0x06: return "PIN or key missing (forgotten bond on one side?)";
    default: return "other";
  }
}

}  // namespace ble
