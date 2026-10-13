// The peripheral side of a link (B3-B7): NimBLE's server callbacks, turned into ble::link() state
// and activity lines. Labs make one, hand it to the server, and override what they need.
//
// Pairing hooks are here too, because every lab after B3 meets them: a passkey to show, a number
// to compare. The passkey goes into the link state; the shell draws it over any view, and the
// button (click = yes, long press = no) or serial `yes`/`no` answers through confirm().
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_random.h>

#include "ble_link.h"

namespace ble {

inline void fillPeer(Peer &p, const NimBLEConnInfo &c) {
  strlcpy(p.addr, c.getAddress().toString().c_str(), sizeof(p.addr));
  const NimBLEAddress id = c.getIdAddress();
  if (!(id == c.getAddress())) strlcpy(p.idAddr, id.toString().c_str(), sizeof(p.idAddr));
  else p.idAddr[0] = 0;
  p.addrType = c.getAddress().getType();
  p.handle = c.getConnHandle();
  p.mtu = c.getMTU();
  p.interval = c.getConnInterval();
  p.latency = c.getConnLatency();
  p.timeout = c.getConnTimeout();
  p.encrypted = c.isEncrypted();
  p.authenticated = c.isAuthenticated();
  p.bonded = c.isBonded();
  p.keySize = c.getSecKeySize();
  p.central = c.isSlave();  // we are the slave (peripheral), so the peer is the central
}

class ServerLink : public NimBLEServerCallbacks {
 public:
  // Advertising starts again by itself after a disconnect (NimBLE's advertiseOnDisconnect).
  void onConnect(NimBLEServer *, NimBLEConnInfo &c) override {
    link().edit([&](LinkState &s) {
      s.peer = Peer{};
      fillPeer(s.peer, c);
      s.radio = Radio::Connected;
      s.advertising = false;
      s.connections++;
      s.connectedAt = millis();
      s.rxOps = s.txOps = s.rxBytes = s.txBytes = 0;
    });
    BLE_NOTE(Good, "connected: %s", c.getAddress().toString().c_str());
    BLE_NOTE(Info, "interval %.2f ms, latency %u, timeout %u ms", c.getConnInterval() * 1.25f, c.getConnLatency(), c.getConnTimeout() * 10);
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &c, int reason) override {
    link().edit([&](LinkState &s) {
      s.connections = s.connections ? s.connections - 1 : 0;
      s.radio = advertiseAgain_ ? Radio::Advertising : Radio::Off;
      s.advertising = advertiseAgain_;
      s.lastDisconnect = reason;
      s.passkey = 0;
    });
    BLE_NOTE(Warn, "disconnected 0x%02X: %s", reason & 0xFF, disconnectReason(reason));
    (void)server;
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo &c) override {
    link().edit([&](LinkState &s) { s.peer.mtu = mtu; });
    BLE_NOTE(Info, "MTU %u: %u bytes of data per packet", mtu, mtu - 3);
    (void)c;
  }

  void onConnParamsUpdate(NimBLEConnInfo &c) override {
    link().edit([&](LinkState &s) { fillPeer(s.peer, c); });
    BLE_NOTE(Info, "new params: %.2f ms, latency %u", c.getConnInterval() * 1.25f, c.getConnLatency());
  }

  void onPhyUpdate(NimBLEConnInfo &c, uint8_t tx, uint8_t rx) override {
    link().edit([&](LinkState &s) { s.peer.txPhy = tx, s.peer.rxPhy = rx; });
    BLE_NOTE(Info, "PHY now %s / %s", tx == 2 ? "2M" : tx == 3 ? "Coded" : "1M", rx == 2 ? "2M" : rx == 3 ? "Coded" : "1M");
    (void)c;
  }

  // ---- Pairing -------------------------------------------------------------------------------

  // Passkey entry, we display: six random digits, the other side types them.
  uint32_t onPassKeyDisplay() override {
    const uint32_t key = esp_random() % 1000000;
    link().edit([&](LinkState &s) { s.passkey = key ? key : 1, s.passkeyCompare = false, s.passkeySince = millis(); });
    BLE_NOTE(Warn, "passkey %06lu: type it on the other device", static_cast<unsigned long>(key));
    return key;
  }

  // Numeric comparison: both screens show the same 6 digits; a person says yes on both.
  void onConfirmPassKey(NimBLEConnInfo &c, uint32_t pin) override {
    pendingHandle_ = c.getConnHandle();
    link().edit([&](LinkState &s) { s.passkey = pin ? pin : 1, s.passkeyCompare = true, s.passkeySince = millis(); });
    BLE_NOTE(Warn, "compare %06lu: same on the phone? (click/yes)", static_cast<unsigned long>(pin));
  }

  void onAuthenticationComplete(NimBLEConnInfo &c) override {
    link().edit([&](LinkState &s) {
      fillPeer(s.peer, c);
      s.passkey = 0;
    });
    if (c.isEncrypted())
      BLE_NOTE(Good, "secured: %s%s, key %u B", c.isAuthenticated() ? "authenticated" : "Just Works", c.isBonded() ? ", bonded" : "", c.getSecKeySize());
    else
      BLE_NOTE(Bad, "pairing failed: still plain text");
  }

  void onIdentity(NimBLEConnInfo &c) override {
    link().edit([&](LinkState &s) { fillPeer(s.peer, c); });
    BLE_NOTE(Info, "identity: %s", c.getIdAddress().toString().c_str());
  }

  // Answer a numeric comparison (button or serial). False when nothing is waiting.
  bool confirm(bool yes) {
    if (pendingHandle_ == 0xFFFF) return false;
    NimBLEServer *server = NimBLEDevice::getServer();
    if (server) NimBLEDevice::injectConfirmPasskey(server->getPeerInfoByHandle(pendingHandle_), yes);
    pendingHandle_ = 0xFFFF;
    link().edit([&](LinkState &s) { s.passkey = 0; });
    BLE_NOTE(Info, "you said %s", yes ? "yes, it matches" : "no");
    return true;
  }
  bool comparing() const { return pendingHandle_ != 0xFFFF; }

  void setAdvertiseAgain(bool on) { advertiseAgain_ = on; }

 private:
  uint16_t pendingHandle_ = 0xFFFF;
  bool advertiseAgain_ = true;
};

}  // namespace ble
