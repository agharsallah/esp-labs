// B8 as the central: the board does what a phone or laptop does with a Bluetooth device.
//
//   scan      listen (B1's table), connectable devices only
//   connect   send CONNECT_IND to one of them; from now on WE set the connection timing
//   explore   discover its services, characteristics and descriptors, read the public basics
//   pair      ask for encryption (a keyboard shows us nothing: we show the passkey, you type it)
//   host      read the Report Map, subscribe to the input reports, and listen
//
// Connecting and discovering block for hundreds of milliseconds (pairing for as long as you take
// to type), so they run in their own task, the "central task", fed through a queue. loop() never
// waits: it keeps drawing, and the passkey overlay can appear while the central task is blocked.
#pragma once

#include <NimBLEDevice.h>
#include <esp_random.h>

#include "ble_link.h"
#include "hid_host.h"
#include "host_state.h"
#include "scan_table.h"
#include "server_link.h"

namespace b8 {

enum class Job : uint8_t { Connect, Explore, Pair, Host, Boot, Disconnect, Forget, Rescan };

struct Request {
  Job job = Job::Explore;
  uint8_t addr[6] = {};
  uint8_t addrType = 0;
  bool autoHost = false;  // Connect: go straight on to host (a bonded keyboard coming back)
  int32_t arg = 0;
  char name[24] = "";
};

// Standard characteristic names, for the explore view.
inline const char *chrName(uint16_t u) {
  switch (u) {
    case 0x2A00: return "Device Name";
    case 0x2A01: return "Appearance";
    case 0x2A04: return "Preferred Params";
    case 0x2A05: return "Service Changed";
    case 0x2A19: return "Battery Level";
    case 0x2A22: return "Boot Kbd Input";
    case 0x2A24: return "Model Number";
    case 0x2A25: return "Serial Number";
    case 0x2A26: return "Firmware Rev";
    case 0x2A27: return "Hardware Rev";
    case 0x2A28: return "Software Rev";
    case 0x2A29: return "Manufacturer";
    case 0x2A32: return "Boot Kbd Output";
    case 0x2A33: return "Boot Mouse Input";
    case 0x2A37: return "Heart Rate";
    case 0x2A4A: return "HID Information";
    case 0x2A4B: return "Report Map";
    case 0x2A4C: return "HID Control";
    case 0x2A4D: return "Report";
    case 0x2A4E: return "Protocol Mode";
    case 0x2A50: return "PnP ID";
    case 0x2AA6: return "Address Resolution";
    case 0x2AC9: return "RPA Only";
    case 0x2B29: return "Client Features";
    case 0x2B2A: return "Database Hash";
    case 0x2B3A: return "Server Features";
    case 0x2900: return "Extended Props";
    case 0x2901: return "User Description";
    case 0x2902: return "CCCD (subscribe)";
    case 0x2904: return "Format";
    case 0x2908: return "Report Reference";
    default: return nullptr;
  }
}

// The only values explore reads: public, standard, harmless. Never anything vendor-specific.
inline bool readable(uint16_t u) {
  switch (u) {
    case 0x2A00: case 0x2A01: case 0x2A19: case 0x2A24: case 0x2A26: case 0x2A27: case 0x2A28: case 0x2A29: case 0x2A50: return true;
    default: return false;
  }
}

inline uint16_t short16(const NimBLEUUID &u) {
  if (u.bitSize() != 16) return 0;
  return BLE_UUID16(u.getBase())->value;
}

// ---- The link from the central's side --------------------------------------------------------

class ClientLink : public NimBLEClientCallbacks {
 public:
  QueueHandle_t jobs = nullptr;  // to ask the central task for a rescan

  void onConnect(NimBLEClient *c) override {
    const NimBLEConnInfo info = c->getConnInfo();
    ble::link().edit([&](ble::LinkState &s) {
      s.peer = ble::Peer{};
      ble::fillPeer(s.peer, info);
      strlcpy(s.peer.name, name_, sizeof(s.peer.name));
      s.radio = ble::Radio::Connected;
      s.scanning = false;
      s.connections++;
      s.connectedAt = millis();
      s.rxOps = s.txOps = s.rxBytes = s.txBytes = 0;
    });
    BLE_NOTE(Good, "connected: we are the central");
    BLE_NOTE(Info, "interval %.2f ms (our choice), MTU next", info.getConnInterval() * 1.25f);
  }

  void onConnectFail(NimBLEClient *, int reason) override { BLE_NOTE(Bad, "connect failed (%d)", reason); }

  void onDisconnect(NimBLEClient *, int reason) override {
    ble::link().edit([&](ble::LinkState &s) {
      s.connections = 0;
      s.radio = ble::Radio::Off;
      s.lastDisconnect = reason;
      s.passkey = 0;
    });
    progress().edit([](Progress &p) { p = Progress{}; });
    pendingCompare_ = false;
    BLE_NOTE(Warn, "disconnected 0x%02X: %s", reason & 0xFF, ble::disconnectReason(reason));
    Request r;
    r.job = Job::Rescan;
    if (jobs) xQueueSend(jobs, &r, 0);
  }

  void onMTUChange(NimBLEClient *, uint16_t mtu) override {
    ble::link().edit([&](ble::LinkState &s) { s.peer.mtu = mtu; });
    BLE_NOTE(Info, "MTU %u agreed", mtu);
  }

  // Passkey entry where WE display: a keyboard has no screen, so you type these digits on it.
  uint32_t onPassKeyDisplay(NimBLEConnInfo &) override {
    const uint32_t key = esp_random() % 1000000;
    ble::link().edit([&](ble::LinkState &s) { s.passkey = key ? key : 1, s.passkeyCompare = false, s.passkeySince = millis(); });
    BLE_NOTE(Warn, "type %06lu on the keyboard, then Enter", static_cast<unsigned long>(key));
    return key;
  }

  // The other side has a screen and a keypad, and shows the digits: type them in the monitor.
  void onPassKeyEntry(NimBLEConnInfo &) override {
    awaitingEntry_ = true;
    BLE_NOTE(Warn, "the device shows a passkey: type passkey <6 digits>");
  }

  void onConfirmPasskey(NimBLEConnInfo &, uint32_t pin) override {
    pendingCompare_ = true;
    ble::link().edit([&](ble::LinkState &s) { s.passkey = pin ? pin : 1, s.passkeyCompare = true, s.passkeySince = millis(); });
    BLE_NOTE(Warn, "compare %06lu with the device (click = yes)", static_cast<unsigned long>(pin));
  }

  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    awaitingEntry_ = false;
    pendingCompare_ = false;
    ble::link().edit([&](ble::LinkState &s) {
      ble::fillPeer(s.peer, info);
      s.passkey = 0;
    });
    if (info.isEncrypted())
      BLE_NOTE(Good, "secured: %s%s, key %u B", info.isAuthenticated() ? "passkey" : "Just Works", info.isBonded() ? ", bonded" : "", info.getSecKeySize());
    else
      BLE_NOTE(Bad, "pairing failed: still plain text");
  }

  void onIdentity(NimBLEConnInfo &info) override {
    ble::link().edit([&](ble::LinkState &s) { ble::fillPeer(s.peer, info); });
    BLE_NOTE(Info, "identity address %s", info.getIdAddress().toString().c_str());
  }

  void setName(const char *n) { strlcpy(name_, n, sizeof(name_)); }

  // From loop(): answers to a numeric comparison or a passkey entry.
  bool comparing() const { return pendingCompare_; }
  bool answer(NimBLEClient *c, bool yes) {
    if (!pendingCompare_ || !c) return false;
    pendingCompare_ = false;
    NimBLEDevice::injectConfirmPasskey(c->getConnInfo(), yes);
    ble::link().edit([](ble::LinkState &s) { s.passkey = 0; });
    BLE_NOTE(Info, "you said %s", yes ? "yes, it matches" : "no");
    return true;
  }
  bool enter(NimBLEClient *c, uint32_t pin) {
    if (!awaitingEntry_ || !c) return false;
    awaitingEntry_ = false;
    return NimBLEDevice::injectPassKey(c->getConnInfo(), pin);
  }

 private:
  char name_[24] = "";
  volatile bool pendingCompare_ = false, awaitingEntry_ = false;
};

// ---- The central task --------------------------------------------------------------------------

class Central : public NimBLEScanCallbacks {
 public:
  ble::ScanTable table;
  HidHost hid;
  ClientLink links;

  void begin() {
    // Keyboards ask for MITM protection; we can only display, so they get passkey entry. A mouse
    // (no keys, no screen) falls back to Just Works. Bond, and keep each other's identity keys.
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    queue_ = xQueueCreate(6, sizeof(Request));
    links.jobs = queue_;
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(&links, false);
    client_->setConnectTimeout(8000);
    refreshBonds();
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(this, true);
    scan->setMaxResults(0);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(100);
    startScan();
    xTaskCreatePinnedToCore(task, "central", 8192, this, 1, nullptr, 1);
    if (bondCount_) BLE_NOTE(Info, "%d bonded device(s): reconnecting when one advertises", bondCount_);
  }

  bool submit(const Request &r) {
    if (xQueueSend(queue_, &r, 0) == pdTRUE) return true;
    BLE_NOTE(Warn, "busy: try again in a moment");
    return false;
  }

  NimBLEClient *client() { return client_; }
  bool connected() { return client_ && client_->isConnected(); }
  bool busy() const { return busy_; }

  void setBoot(bool on) { boot_ = on; }
  bool boot() const { return boot_; }

  bool isBonded(const uint8_t *a, uint8_t type) {
    bool hit = false;
    portENTER_CRITICAL(&lock_);
    for (int i = 0; i < bondCount_; i++) hit |= bondTypes_[i] == type && memcmp(bonds_[i], a, 6) == 0;
    portEXIT_CRITICAL(&lock_);
    return hit;
  }

  void startScan() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning()) return;
    if (scan->start(0, false, true)) ble::link().edit([](ble::LinkState &s) { s.radio = ble::Radio::Scanning, s.scanning = true; });
  }
  void stopScan() {
    NimBLEDevice::getScan()->stop();
    ble::link().edit([](ble::LinkState &s) {
      s.scanning = false;
      if (s.radio == ble::Radio::Scanning) s.radio = ble::Radio::Off;
    });
  }

  // NimBLE task: every packet heard. A bonded keyboard that shows up again gets reconnected.
  void onResult(const NimBLEAdvertisedDevice *d) override {
    table.record(d);
    if (!d->isConnectable() || busy_ || connected() || millis() - lastAuto_ < 10000) return;
    if (!isBonded(d->getAddress().getVal(), d->getAddress().getType())) return;
    lastAuto_ = millis();
    Request r;
    r.job = Job::Connect;
    memcpy(r.addr, d->getAddress().getVal(), 6);
    r.addrType = d->getAddress().getType();
    r.autoHost = true;
    strlcpy(r.name, d->haveName() ? d->getName().c_str() : "bonded device", sizeof(r.name));
    BLE_NOTE(Good, "bonded %s is back: reconnecting", r.name);
    xQueueSend(queue_, &r, 0);
  }

  // Asked for by loop() when the view should follow (hosting a keyboard -> keyboard view).
  volatile int wantView = -1;
  enum : int { kViewExplore = 1, kViewKeyboard = 2, kViewMouse = 3 };

 private:
  static void task(void *arg) {
    auto *self = static_cast<Central *>(arg);
    Request r;
    for (;;)
      if (xQueueReceive(self->queue_, &r, portMAX_DELAY) == pdTRUE) {
        self->busy_ = true;
        self->run(r);
        self->busy_ = false;
      }
  }

  void run(const Request &r) {
    switch (r.job) {
      case Job::Connect:
        if (connectTo(r) && explore() && r.autoHost) host();
        break;
      case Job::Explore: explore(); break;
      case Job::Pair: pair(); break;
      case Job::Host: host(); break;
      case Job::Boot:
        if (connected() && progress_().phase == Phase::Hosting) host();
        break;
      case Job::Disconnect:
        if (connected()) client_->disconnect();
        break;
      case Job::Forget:
        if (connected()) client_->disconnect(), vTaskDelay(pdMS_TO_TICKS(300));
        NimBLEDevice::deleteAllBonds();
        refreshBonds();
        BLE_NOTE(Info, "every bond deleted: pair again to use a keyboard");
        break;
      case Job::Rescan:
        hid.reset();
        startScan();
        break;
    }
  }

  Progress progress_() {
    Progress p;
    progress().copyTo(p);
    return p;
  }
  void phase(Phase ph) {
    progress().edit([&](Progress &p) { p.phase = ph, p.since = millis(); });
  }

  bool connectTo(const Request &r) {
    if (connected()) {
      BLE_NOTE(Warn, "already connected: disconnect first");
      return false;
    }
    stopScan();
    links.setName(r.name);
    progress().edit([&](Progress &p) {
      p = Progress{};
      p.phase = Phase::Connecting;
      p.since = millis();
      strlcpy(p.target, r.name, sizeof(p.target));
    });
    ble::link().edit([](ble::LinkState &s) { s.radio = ble::Radio::Connecting; });
    BLE_NOTE(Tx, "CONNECT_IND to %s", r.name);
    const NimBLEAddress addr(r.addr, r.addrType);
    if (!client_->connect(addr, true, false, true)) {
      BLE_NOTE(Bad, "no answer from %s (err %d): still advertising?", r.name, client_->getLastError());
      phase(Phase::Idle);
      startScan();
      return false;
    }
    return true;
  }

  bool explore() {
    if (!connected()) return BLE_NOTE(Warn, "connect first"), false;
    phase(Phase::Discovering);
    BLE_NOTE(Tx, "discovering services...");
    static Tree t;  // 4 KB: built here, then copied under the lock
    t = Tree{};
    strlcpy(t.device, progress_().target, sizeof(t.device));
    const uint32_t started = millis();
    const std::vector<NimBLERemoteService *> &services = client_->getServices(true);
    const bool encrypted = client_->getConnInfo().isEncrypted();
    bool standard = false, vendor = false;  // anything beyond the GAP/GATT basics?
    for (NimBLERemoteService *svc : services) {
      const uint16_t su = short16(svc->getUUID());
      t.services++;
      if (su == 0x1812) t.hasHid = true;
      const bool basic = su == 0x1800 || su == 0x1801, known = su && ad::service16Name(su);
      if (known && !basic && su != 0x180A && su != 0x180F) standard = true;  // info and battery don't count
      if (!known && !basic) vendor = true;
      add(t, 0, su, su ? (ad::service16Name(su) ? ad::service16Name(su) : "service") : "vendor service", "");
      if (!su) snprintf(t.nodes[t.count - 1].value, sizeof(t.nodes[0].value), "%.26s", svc->getUUID().toString().c_str());
      for (NimBLERemoteCharacteristic *chr : svc->getCharacteristics(true)) {
        const uint16_t cu = short16(chr->getUUID());
        t.characteristics++;
        char value[28] = "";
        // Read only the public basics. A HID device locks even those until it is paired, and a
        // locked read would start pairing by itself: leave that to `pair`/`host`.
        if (readable(cu) && chr->canRead()) {
          if (t.hasHid && !encrypted) strlcpy(value, "locked until paired", sizeof(value));
          else formatValue(cu, chr->readValue(), value, sizeof(value));
        }
        add(t, 1, cu, cu && chrName(cu) ? chrName(cu) : "vendor value", value);
        Node &n = t.nodes[t.count - 1];
        n.props = uint8_t((chr->canRead() ? 0x02 : 0) | (chr->canWriteNoResponse() ? 0x04 : 0) | (chr->canWrite() ? 0x08 : 0) |
                          (chr->canNotify() ? 0x10 : 0) | (chr->canIndicate() ? 0x20 : 0));
        if (cu == 0x2A00 && value[0]) strlcpy(t.device, value, sizeof(t.device));
        for (NimBLERemoteDescriptor *d : chr->getDescriptors(true)) {
          const uint16_t du = short16(d->getUUID());
          add(t, 2, du, du && chrName(du) ? chrName(du) : "descriptor", "");
        }
      }
    }
    t.vendorOnly = vendor && !standard;
    const ble::LinkState ls = ble::link().get();
    t.audio = strstr(t.device, "JBL") != nullptr || strstr(progress_().target, "JBL") != nullptr || strstr(ls.peer.name, "JBL") != nullptr;
    tree().edit([&](Tree &shared) {
      const uint32_t v = shared.version;
      shared = t;
      shared.version = v;
    });
    phase(Phase::Idle);
    BLE_NOTE(Good, "%d services, %d characteristics in %lu ms", t.services, t.characteristics, static_cast<unsigned long>(millis() - started));
    if (t.hasHid) BLE_NOTE(Info, "it is a keyboard/mouse (1812): type host");
    else if (t.vendorOnly && t.audio) BLE_NOTE(Info, "JBL app's own service only: audio uses Classic BT");
    else if (t.vendorOnly) BLE_NOTE(Info, "only the maker's own services: its app speaks them");
    wantView = kViewExplore;
    return true;
  }

  bool pair() {
    if (!connected()) return BLE_NOTE(Warn, "connect first"), false;
    if (client_->getConnInfo().isEncrypted()) return BLE_NOTE(Info, "already encrypted"), true;
    phase(Phase::Pairing);
    BLE_NOTE(Tx, "pairing request (bond, MITM, Secure Connections)");
    const bool ok = client_->secureConnection();
    phase(Phase::Idle);
    if (ok) refreshBonds();
    else BLE_NOTE(Bad, "pairing failed (%d): in pairing mode? forget it there too", client_->getLastError());
    return ok;
  }

  bool host() {
    if (!connected()) return BLE_NOTE(Warn, "connect first"), false;
    NimBLERemoteService *svc = client_->getService(NimBLEUUID(uint16_t(0x1812)));
    if (!svc) return BLE_NOTE(Warn, "no HID service (1812): not a keyboard or mouse"), false;
    if (!client_->getConnInfo().isEncrypted() && !pair()) return false;
    phase(Phase::Reading);
    hid.reset();
    // 1. The Report Map, usually 50-500 bytes: a long read over several packets.
    NimBLERemoteCharacteristic *mapChr = svc->getCharacteristic(NimBLEUUID(uint16_t(0x2A4B)));
    if (!mapChr) return BLE_NOTE(Bad, "no Report Map"), phase(Phase::Idle), false;
    const NimBLEAttValue m = mapChr->readValue();
    hid.setMap(m.data(), m.size());
    BLE_NOTE(Rx, "Report Map: %u bytes, %d input fields", m.size(), hid.map.count);
    if (hid.map.malformed) BLE_NOTE(Warn, "the Report Map ends mid-item: decoding what we can");
    // 2. Every input report, matched to its ID through descriptor 2908, then subscribed.
    if (!boot_) {
      setProtocol(svc, 1);
      for (NimBLERemoteCharacteristic *chr : svc->getCharacteristics(true)) {
        if (short16(chr->getUUID()) != 0x2A4D || !chr->canNotify()) continue;
        uint8_t id = 0, type = 1;
        if (NimBLERemoteDescriptor *ref = chr->getDescriptor(NimBLEUUID(uint16_t(0x2908)))) {
          const NimBLEAttValue v = ref->readValue();
          if (v.size() >= 2) id = v.data()[0], type = v.data()[1];
        }
        if (type != 1) continue;  // 1 input, 2 output (keyboard LEDs), 3 feature
        subscribe(chr, id, hid.kindOf(id));
      }
    } else {
      // 3. Boot protocol: fixed formats every BIOS understands; no Report Map needed.
      setProtocol(svc, 0);
      if (NimBLERemoteCharacteristic *k = svc->getCharacteristic(NimBLEUUID(uint16_t(0x2A22)))) subscribe(k, 0, ReportKind::BootKeyboard);
      if (NimBLERemoteCharacteristic *ms = svc->getCharacteristic(NimBLEUUID(uint16_t(0x2A33)))) subscribe(ms, 0, ReportKind::BootMouse);
    }
    bool kb = false, mouse = false, consumer = false;
    for (int i = 0; i < hid.routeCount; i++) {
      kb |= hid.routes[i].kind == ReportKind::Keyboard || hid.routes[i].kind == ReportKind::BootKeyboard;
      mouse |= hid.routes[i].kind == ReportKind::Mouse || hid.routes[i].kind == ReportKind::BootMouse;
      consumer |= hid.routes[i].kind == ReportKind::Consumer;
    }
    progress().edit([&](Progress &p) {
      p.phase = Phase::Hosting, p.since = millis();
      p.keyboard = kb, p.mouse = mouse, p.consumer = consumer, p.boot = boot_;
      p.fields = hid.map.count, p.mapBytes = int(m.size());
    });
    BLE_NOTE(Good, "hosting: %s%s%s%s", kb ? "keyboard " : "", mouse ? "mouse " : "", consumer ? "media keys" : "", boot_ ? " (boot)" : "");
    wantView = kb ? kViewKeyboard : mouse ? kViewMouse : -1;
    return true;
  }

  void setProtocol(NimBLERemoteService *svc, uint8_t mode) {
    // Protocol Mode (2A4E): 0 boot, 1 report. A standard HID setting on your own device.
    NimBLERemoteCharacteristic *pm = svc->getCharacteristic(NimBLEUUID(uint16_t(0x2A4E)));
    if (pm && pm->canWriteNoResponse()) pm->writeValue(&mode, 1, false);
    else if (mode == 0) BLE_NOTE(Warn, "no Protocol Mode: this device has no boot mode");
  }

  void subscribe(NimBLERemoteCharacteristic *chr, uint8_t id, ReportKind kind) {
    if (!hid.addRoute(chr->getHandle(), id, kind)) return;
    // Writes 01 00 into the characteristic's CCCD (2902): "send me notifications".
    const bool ok = chr->subscribe(true, [this](NimBLERemoteCharacteristic *c, uint8_t *data, size_t len, bool) { hid.onReport(c->getHandle(), data, len); });
    static const char *const kinds[] = {"keyboard", "mouse", "media", "other", "boot keyboard", "boot mouse"};
    if (ok) BLE_NOTE(Tx, "CCCD 01 00: report %u (%s)", id, kinds[static_cast<int>(kind)]);
    else BLE_NOTE(Bad, "subscribe to report %u refused", id);
  }

  static void formatValue(uint16_t u, const NimBLEAttValue &v, char *out, size_t n) {
    if (v.size() == 0) return void(strlcpy(out, "(empty)", n));
    if (u == 0x2A19) return void(snprintf(out, n, "%u %%", v.data()[0]));
    if (u == 0x2A01 && v.size() >= 2) {
      const uint16_t a = uint16_t(v.data()[0] | (v.data()[1] << 8));
      return void(snprintf(out, n, "%04X %s", a, ad::kindName(ad::kindFromAppearance(a))));
    }
    if (u == 0x2A50 && v.size() >= 7)
      return void(snprintf(out, n, "VID %04X PID %04X", v.data()[1] | (v.data()[2] << 8), v.data()[3] | (v.data()[4] << 8)));
    size_t i = 0;
    for (; i < v.size() && i + 1 < n; i++) out[i] = (v.data()[i] >= 0x20 && v.data()[i] < 0x7F) ? char(v.data()[i]) : '.';
    out[i] = 0;
  }

  static void add(Tree &t, uint8_t level, uint16_t uuid, const char *name, const char *value) {
    if (t.count == Tree::kMax) return void(t.truncated = true);
    Node &n = t.nodes[t.count++];
    n.level = level;
    n.uuid16 = uuid;
    strlcpy(n.name, name, sizeof(n.name));
    strlcpy(n.value, value, sizeof(n.value));
  }

  void refreshBonds() {
    const int n = std::min(NimBLEDevice::getNumBonds(), 4);
    portENTER_CRITICAL(&lock_);
    bondCount_ = 0;
    portEXIT_CRITICAL(&lock_);
    for (int i = 0; i < n; i++) {
      const NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
      portENTER_CRITICAL(&lock_);
      memcpy(bonds_[bondCount_], a.getVal(), 6);
      bondTypes_[bondCount_++] = a.getType();
      portEXIT_CRITICAL(&lock_);
    }
  }

  NimBLEClient *client_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  volatile bool busy_ = false, boot_ = false;
  volatile uint32_t lastAuto_ = 0;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t bonds_[4][6] = {};
  uint8_t bondTypes_[4] = {};
  int bondCount_ = 0;
};

}  // namespace b8
