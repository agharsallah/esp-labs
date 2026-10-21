// Bluetooth lab B4 Pair & trust.  uv run pio run -d labs/ble-lab-b4 -e ble-lab-b4 -t upload
// B3's Bench service gets a Vault: a characteristic only an encrypted, authenticated link may read
// or write. Touch it from the phone and pairing starts: the passkey appears on the TFT, or both
// screens show 6 digits and you confirm with the button. The board then remembers the phone (a
// bond, saved in flash), recognises it behind its changing private address, and can refuse
// everybody else.
//
// Wiring: the screen from lesson 03. The push button on GPIO 5 is optional (it does what BOOT does).
//
// Views: ceremony · bonds · gatt · link · log   (click = next view, double-click = the view's action;
//        while numbers wait for comparison: click = yes, long press = no)
// Serial: help lists the commands. Shared code: kit/lib/ble-kit (bench_service.h, server_link.h).
#include <Preferences.h>

#include "b4_views.h"
#include "bench_views.h"
#include "ble_lab.h"
#include "ceremony.h"

namespace {

b4::B4Link links;
bench::BenchService bench_;
b4v::Settings settings;
Preferences prefs;
b4v::BondRow bondRows[8];
int bondCount = 0;
uint32_t bondsAt = 0;

void refreshBonds() {
  bondCount = std::min(NimBLEDevice::getNumBonds(), 8);
  for (int i = 0; i < bondCount; i++) {
    const NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
    strlcpy(bondRows[i].addr, a.toString().c_str(), sizeof(bondRows[i].addr));
    bondRows[i].random = a.getType() != BLE_ADDR_PUBLIC;
  }
  bondsAt = millis();
}

// What the board says about itself when pairing starts (its IO capabilities), and what it wants.
void applySecurity() {
  static const uint8_t io[] = {BLE_HS_IO_NO_INPUT_OUTPUT, BLE_HS_IO_DISPLAY_ONLY, BLE_HS_IO_DISPLAY_YESNO};
  NimBLEDevice::setSecurityIOCap(io[settings.method]);
  // bond = remember the keys · mitm = demand a check by a person · sc = LE Secure Connections (ECDH)
  NimBLEDevice::setSecurityAuth(true, settings.method != 0, true);
  // Keys both sides hand over after pairing: the encryption key (LTK) and the identity key (IRK).
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
}

// "allow bonded": only phones on the filter accept list (the bonded identities) may connect. The
// controller resolves each private address with the stored IRKs before it checks the list.
void applyAllow() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  const bool wasOn = adv->isAdvertising();
  adv->stop();
  while (NimBLEDevice::getWhiteListCount()) NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));
  if (settings.allowBondedOnly)
    for (int i = 0; i < NimBLEDevice::getNumBonds(); i++) NimBLEDevice::whiteListAdd(NimBLEDevice::getBondedAddress(i));
  // Scan requests stay open to all (phones still see the board), connections only from the list.
  adv->setScanFilter(false, settings.allowBondedOnly);
  if (settings.allowBondedOnly && NimBLEDevice::getNumBonds() == 0) BLE_NOTE(Warn, "no bonds yet: nobody can connect now");
  if (wasOn || ble::link().get().radio != ble::Radio::Connected) {
    if (adv->start()) ble::link().edit([](ble::LinkState &s) {
        s.advertising = true;
        if (s.radio != ble::Radio::Connected) s.radio = ble::Radio::Advertising;
      });
  }
}

// ---- Views ------------------------------------------------------------------------------------

void drawCeremony(BleLab &lab, FrameBuffer &fb, float t) { b4v::ceremony(fb, b4::recorder().get(), lab.state(), settings, t); }
void drawBonds(BleLab &lab, FrameBuffer &fb, float t) { b4v::bonds(fb, bondRows, bondCount, lab.state(), t); }
void drawGatt(BleLab &, FrameBuffer &fb, float t) { bv::gatt(fb, bench_.state(), t); }
void drawLink(BleLab &lab, FrameBuffer &fb, float t) { bv::link(fb, lab.state(), t); }
void drawLog(BleLab &, FrameBuffer &fb, float) { bv::log(fb); }

const BleView kViews[] = {
    {"ceremony", drawCeremony, "secure"},
    {"bonds", drawBonds, "bonds"},
    {"gatt", drawGatt, "press"},
    {"link", drawLink, "secure"},
    {"log", drawLog, nullptr},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdPair(BleLab &lab, const String &, const String &lower) {
  static const char *const names[] = {"justworks", "passkey", "compare"};
  for (uint8_t i = 0; i < 3; i++)
    if (lower == names[i]) {
      settings.method = i;
      prefs.putUChar("method", i);
      applySecurity();
      BLE_NOTE(Good, "next pairing: %s (saved)", b4v::methodName(i));
      if (NimBLEDevice::getNumBonds()) BLE_NOTE(Info, "old bonds keep their old level: forget + pair again");
      return lab.showView("ceremony");
    }
  lab.warn("pair justworks | passkey | compare   (now: %s)", b4v::methodName(settings.method));
}

// The GATT table is built once at start, with its permissions: changing them means a restart.
void cmdLock(BleLab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("lock on | lock off   (now %s)", settings.lock ? "on" : "off");
  prefs.putBool("lock", lower == "on");
  Serial.printf("lock %s saved. The GATT table is fixed at start-up, so the board restarts now...\n", lower.c_str());
  Serial.flush();
  delay(300);
  ESP.restart();
}

void printBonds() {
  refreshBonds();
  Serial.printf("%d bonded device%s (identity addresses, as the bond store keeps them):\n", bondCount, bondCount == 1 ? "" : "s");
  for (int i = 0; i < bondCount; i++) Serial.printf("  %d  %s  %s\n", i + 1, bondRows[i].addr, bondRows[i].random ? "random static" : "public");
}

void cmdBonds(BleLab &lab, const String &, const String &) {
  printBonds();
  lab.showView("bonds");
}

void cmdForget(BleLab &lab, const String &arg, const String &lower) {
  refreshBonds();
  const ble::LinkState s = ble::link().get();
  if (lower == "all") {
    NimBLEDevice::deleteAllBonds();
    BLE_NOTE(Warn, "forgot every bond");
  } else {
    long n = 0;
    if (!BleLab::number(arg, 1, std::max(bondCount, 1), n) || bondCount == 0) return lab.warn("forget <1-%d> | forget all   (see bonds)", bondCount);
    NimBLEDevice::deleteBond(NimBLEDevice::getBondedAddress(n - 1));
    BLE_NOTE(Warn, "forgot bond %ld: %s", n, bondRows[n - 1].addr);
  }
  BLE_NOTE(Info, "now forget \"Bench B4\" on the phone too");
  if (s.radio == ble::Radio::Connected && s.peer.bonded) NimBLEDevice::getServer()->disconnect(s.peer.handle);
  refreshBonds();
  if (settings.allowBondedOnly) applyAllow();
}

void cmdAllow(BleLab &lab, const String &, const String &lower) {
  if (lower != "bonded" && lower != "anyone") return lab.warn("allow bonded | allow anyone   (now %s)", settings.allowBondedOnly ? "bonded" : "anyone");
  settings.allowBondedOnly = lower == "bonded";
  prefs.putBool("allow", settings.allowBondedOnly);
  applyAllow();
  BLE_NOTE(Good, "%s may connect (saved)", settings.allowBondedOnly ? "only bonded phones" : "anyone");
}

void cmdYes(BleLab &lab, const String &, const String &) {
  if (!links.confirm(true)) lab.warn("no numbers are waiting for a comparison");
}
void cmdNo(BleLab &lab, const String &, const String &) {
  if (!links.confirm(false)) lab.warn("no numbers are waiting for a comparison");
}

// The peripheral asks; the phone starts encryption (stored keys) or pairing (no keys yet).
void cmdSecure(BleLab &lab, const String &, const String &) {
  const ble::LinkState s = ble::link().get();
  if (s.radio != ble::Radio::Connected) return lab.warn("connect a phone first");
  if (s.peer.encrypted) return BLE_NOTE(Info, "already encrypted");
  if (NimBLEDevice::startSecurity(s.peer.handle)) BLE_NOTE(Tx, "Security Request: please encrypt");
  else lab.warn("the request was refused (pairing already running?)");
}

void cmdVault(BleLab &lab, const String &arg, const String &) {
  if (arg.isEmpty()) return lab.warn("vault <text>: a secret only a paired phone can read");
  bench_.setVault(arg.c_str());
  BLE_NOTE(Info, "Vault set (%u bytes)", arg.length());
}

void cmdPress(BleLab &, const String &, const String &) { bench_.button(1); }

void cmdDisconnect(BleLab &lab, const String &, const String &) {
  const ble::LinkState s = ble::link().get();
  if (s.radio != ble::Radio::Connected) return lab.warn("nobody is connected");
  NimBLEDevice::getServer()->disconnect(s.peer.handle);
}

const BleCommand kCommands[] = {
    {"pair", "justworks|passkey|compare", "how the next pairing checks for a man in the middle", cmdPair},
    {"secure", "", "ask the phone to encrypt (pairs if it has no keys)", cmdSecure},
    {"yes", "", "the numbers match (or click)", cmdYes},
    {"no", "", "they don't (or long press)", cmdNo},
    {"bonds", "", "list the phones this board remembers", cmdBonds},
    {"forget", "<n>|all", "delete a bond (then forget the board on the phone)", cmdForget},
    {"allow", "bonded|anyone", "only bonded phones may connect, or anyone", cmdAllow},
    {"lock", "on|off", "LED and Message need encryption too (restarts)", cmdLock},
    {"vault", "<text>", "set the secret from the board's side", cmdVault},
    {"press", "", "send a Button notification", cmdPress},
    {"disconnect", "", "hang up on the phone", cmdDisconnect},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(BleLab &lab) {
  prefs.begin("b4", false);
  settings.method = prefs.getUChar("method", 1) % 3;
  settings.lock = prefs.getBool("lock", false);
  settings.allowBondedOnly = prefs.getBool("allow", false);
  applySecurity();
  NimBLEDevice::setCustomGapHandler(b4::gapListener);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&links, false);
  bench::Options o;
  o.vault = true;
  o.lockControls = settings.lock;
  bench_.begin(server, o);
  bench_.setVault("the bench's secret: 42");
  server->start();
  bench::advertise(lab.spec().deviceName, false);
  applyAllow();
  refreshBonds();
  BLE_NOTE(Info, "pairing: %s, lock %s, %s may connect", b4v::methodName(settings.method), settings.lock ? "on" : "off",
           settings.allowBondedOnly ? "bonded" : "anyone");
}

void tick(BleLab &lab) {
  bench_.tick();
  uint8_t c[3];
  if (bench_.takeLed(c)) {
    lab.claimLed(true);
    rgbLedWrite(PIN_RGB, c[0], c[1], c[2]);
  }
  if (millis() - bondsAt > 1000) refreshBonds();
}

bool gesture(BleLab &, Gesture g, bool) {
  if (links.comparing()) {
    if (g == Gesture::Click) links.confirm(true);
    else if (g == Gesture::LongPress) links.confirm(false);
    return true;
  }
  bench_.button(g == Gesture::Click ? 1 : g == Gesture::DoubleClick ? 2 : 3);
  return false;
}

const BleLabSpec kSpec{
    "ble-lab-b4", "B4", "Pair & trust", "Bench B4", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, tick, gesture, nullptr,
};

BleLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
