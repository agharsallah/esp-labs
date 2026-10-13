// B4's pairing ceremony: every phase of making a link private, with the time it happened.
//
//   1 connect     the phone connects. Everything still travels in plain text.
//   2 features    Pairing Request / Response: both sides say what they can do (display? keyboard?
//                 yes/no buttons?), whether they want to bond and to be protected against a man in
//                 the middle (MITM)
//   3 method      from those two answers the method follows: Just Works, Passkey Entry or Numeric
//                 Comparison (the matrix in the guide)
//   4 keys        LE Secure Connections: each side sends an elliptic-curve public key (ECDH P-256)
//                 and both compute the same secret without ever sending it
//   5 check       the person proves nobody is in between: types the passkey, or confirms that both
//                 screens show the same 6 digits. Just Works skips this, so it stops eavesdroppers
//                 but not a man in the middle.
//   6 encrypted   AES-CCM with the long-term key (LTK) from step 4
//   7 keys out    both sides hand over keys to remember: the LTK and the IRK (Identity Resolving Key,
//                 which recognises the phone behind its changing private addresses)
//   8 bonded      the keys are saved (NVS here). Next time: straight from 1 to 6, no ceremony.
//
// NimBLE reports some of these (connect, passkey, encryption, identity), not all: phases 2-4 happen
// inside the stack. They are drawn as inferred once a later phase proves they happened.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "ble_link.h"
#include "server_link.h"

namespace b4 {

enum Phase : uint8_t { Connect, Features, Method, Keys, Check, Encrypted, KeysOut, Bonded, kPhases };

inline const char *phaseName(int p) {
  static const char *const names[] = {"connect", "features", "method", "ECDH keys", "check", "encrypted", "keys out", "bonded"};
  return names[p];
}

enum class How : uint8_t { Unknown, JustWorks, Passkey, Compare, Stored };

inline const char *howName(How h) {
  static const char *const names[] = {"?", "Just Works", "Passkey Entry", "Numeric Comparison", "stored keys"};
  return names[static_cast<int>(h)];
}

struct Ceremony {
  uint32_t at[kPhases] = {};      // millis() when reached, 0 = not yet
  bool inferred[kPhases] = {};    // reached, but NimBLE didn't report it: a later phase proves it
  How how = How::Unknown;
  bool failed = false;
  char why[52] = "";
  uint32_t failedAt = 0;
  int bondsAtConnect = 0;
  bool knownPeer = false;         // the phone was bonded before this connection
};

// SMP and HCI failure codes, in words. NimBLE: 0x200 + HCI, 0x400 + ours (SMP), 0x500 + the peer's.
inline const char *failureText(int status) {
  const int code = status & 0xFF;
  if (status == BLE_HS_ETIMEOUT) return "timeout: nobody finished in 30 s";
  if ((status & 0xF00) == 0x400 || (status & 0xF00) == 0x500) {
    switch (code) {
      case 0x01: return "passkey entry failed";
      case 0x03: return "security level not met (MITM?)";
      case 0x04: return "confirm failed: wrong passkey";
      case 0x05: return "pairing not supported";
      case 0x08: return "unspecified (cancelled?)";
      case 0x09: return "too many attempts, wait";
      case 0x0B: return "DHKey check failed";
      case 0x0C: return "numbers didn't match: said no";
      default: return "pairing error";
    }
  }
  if ((status & 0xF00) == 0x200) {
    if (code == 0x06) return "key missing: one side forgot the bond";
    if (code == 0x05) return "authentication failure";
    if (code == 0x3D) return "MIC failure: keys don't match";
  }
  return "encryption failed";
}

class Recorder {
 public:
  Ceremony get() {
    portENTER_CRITICAL(&lock_);
    Ceremony c = c_;
    portEXIT_CRITICAL(&lock_);
    return c;
  }

  void connected() {
    const int bonds = NimBLEDevice::getNumBonds();
    portENTER_CRITICAL(&lock_);
    c_ = Ceremony{};
    c_.at[Connect] = millis();
    c_.bondsAtConnect = bonds;
    portEXIT_CRITICAL(&lock_);
  }

  // The person's part starts (passkey shown or numbers to compare): 2-4 must have happened.
  void check(How how) {
    portENTER_CRITICAL(&lock_);
    reach(Check, false);
    c_.how = how;
    portEXIT_CRITICAL(&lock_);
  }

  void encrypted(const NimBLEConnInfo &info) {
    const int bonds = NimBLEDevice::getNumBonds();
    portENTER_CRITICAL(&lock_);
    // No passkey, no comparison: either Just Works, or the bond from last time (no pairing at all).
    if (c_.how == How::Unknown) c_.how = info.isBonded() && bonds == c_.bondsAtConnect && c_.at[Features] == 0 ? How::Stored : How::JustWorks;
    if (c_.how == How::Stored) {
      c_.knownPeer = true;
      c_.at[Encrypted] = millis();
      c_.at[Bonded] = millis();
    } else {
      reach(Encrypted, false);
      if (info.isBonded()) reach(KeysOut, true), reach(Bonded, false);
    }
    portEXIT_CRITICAL(&lock_);
  }

  void identity() {
    portENTER_CRITICAL(&lock_);
    if (c_.at[KeysOut] == 0 || c_.inferred[KeysOut]) c_.at[KeysOut] = millis(), c_.inferred[KeysOut] = false;
    portEXIT_CRITICAL(&lock_);
  }

  void failed(const char *why) {
    portENTER_CRITICAL(&lock_);
    c_.failed = true;
    c_.failedAt = millis();
    strlcpy(c_.why, why, sizeof(c_.why));
    portEXIT_CRITICAL(&lock_);
  }

  // A disconnect while the person still had to act = the ceremony broke off.
  void disconnected(int reason) {
    portENTER_CRITICAL(&lock_);
    const bool midway = c_.at[Connect] && c_.at[Features] && !c_.at[Encrypted] && !c_.failed;
    portEXIT_CRITICAL(&lock_);
    if (midway) failed(disconnectText(reason));
  }

 private:
  static const char *disconnectText(int reason) {
    return (reason & 0xFF) == 0x05 || (reason & 0xFF) == 0x06 ? "dropped: authentication failed" : "dropped before encryption";
  }

  // Mark phase p and every earlier phase not seen yet (those as inferred). Lock held.
  void reach(int p, bool inferred) {
    const uint32_t now = millis();
    for (int i = 0; i < p; i++)
      if (!c_.at[i]) c_.at[i] = now, c_.inferred[i] = true;
    if (!c_.at[p]) c_.at[p] = now, c_.inferred[p] = inferred;
  }

  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  Ceremony c_;
};

inline Recorder &recorder() {
  static Recorder r;
  return r;
}

// The shared server callbacks, plus the ceremony.
class B4Link : public ble::ServerLink {
 public:
  void onConnect(NimBLEServer *s, NimBLEConnInfo &c) override {
    recorder().connected();
    ServerLink::onConnect(s, c);
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &c, int reason) override {
    recorder().disconnected(reason);
    ServerLink::onDisconnect(s, c, reason);
  }
  uint32_t onPassKeyDisplay() override {
    recorder().check(How::Passkey);
    return ServerLink::onPassKeyDisplay();
  }
  void onConfirmPassKey(NimBLEConnInfo &c, uint32_t pin) override {
    recorder().check(How::Compare);
    ServerLink::onConfirmPassKey(c, pin);
  }
  void onAuthenticationComplete(NimBLEConnInfo &c) override {
    if (c.isEncrypted()) recorder().encrypted(c);
    ServerLink::onAuthenticationComplete(c);
  }
  void onIdentity(NimBLEConnInfo &c) override {
    recorder().identity();
    ServerLink::onIdentity(c);
  }
};

// A second listener on GAP events, next to NimBLE's own. In this NimBLE only connect, disconnect and
// encryption changes reach listeners (passkey, identity and repeat pairing go to the server's
// callbacks above). The encryption change is the one we need: it carries the failure code.
inline int gapListener(ble_gap_event *e, void *) {
  if (e->type == BLE_GAP_EVENT_ENC_CHANGE) {
    if (e->enc_change.status == 0) {
      BLE_NOTE(Info, "ENC_CHANGE: link encrypted (AES-CCM)");
    } else {
      const char *why = failureText(e->enc_change.status);
      recorder().failed(why);
      BLE_NOTE(Bad, "ENC_CHANGE failed 0x%03X: %s", e->enc_change.status, why);
      if ((e->enc_change.status & 0xFF) == 0x06 && (e->enc_change.status & 0xF00) == 0x200)
        BLE_NOTE(Warn, "forget the board on the phone, then pair again");
    }
  }
  return 0;
}

}  // namespace b4
