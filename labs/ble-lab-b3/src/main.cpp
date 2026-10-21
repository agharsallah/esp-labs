// Bluetooth lab B3 Connect & talk.  uv run pio run -d labs/ble-lab-b3 -e ble-lab-b3 -t upload
// The board becomes a GATT server: a small database your phone connects to. In nRF Connect or
// LightBlue you set the on-board LED's colour, write a message onto the screen, read the chip's
// temperature and subscribe to the button. Every operation lights its row on the gatt view.
//
// Wiring: the screen from lesson 03. The push button on GPIO 5 is optional (it does what BOOT does).
//
// Views: gatt · message · link · log   (click = next view, double-click = the action in the footer)
// Every gesture is also sent to a subscribed phone as a Button notification.
// Serial: help lists the commands. Shared code: kit/lib/ble-kit (bench_service.h is the GATT table).
#include "bench_views.h"
#include "ble_lab.h"
#include "server_link.h"

namespace {

ble::ServerLink links;
bench::BenchService bench_;

void drawGatt(BleLab &, FrameBuffer &fb, float t) { bv::gatt(fb, bench_.state(), t); }
void drawMessage(BleLab &, FrameBuffer &fb, float t) { bv::message(fb, bench_.state(), t); }
void drawLink(BleLab &lab, FrameBuffer &fb, float t) { bv::link(fb, lab.state(), t); }
void drawLog(BleLab &, FrameBuffer &fb, float) { bv::log(fb); }

const BleView kViews[] = {
    {"gatt", drawGatt, "press"},
    {"message", drawMessage, "press"},
    {"link", drawLink, "params fast"},
    {"log", drawLog, nullptr},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdAdv(BleLab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("adv on | adv off");
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (lower == "on") return bench::advertise(lab.spec().deviceName);
  adv->stop();
  ble::link().edit([](ble::LinkState &s) {
    s.advertising = false;
    if (s.radio == ble::Radio::Advertising) s.radio = ble::Radio::Off;
  });
  BLE_NOTE(Info, "advertising off: invisible to phones");
}

void cmdMessage(BleLab &lab, const String &arg, const String &) {
  if (arg.isEmpty()) return lab.warn("message <text>: sets the Message value from the board's side");
  bench_.setMessage(arg.c_str());
  lab.showView("message");
  BLE_NOTE(Info, "Message set here: a phone that reads it now gets it");
}

void cmdPress(BleLab &, const String &, const String &) { bench_.button(1); }

void cmdDisconnect(BleLab &lab, const String &, const String &) {
  const ble::LinkState s = ble::link().get();
  if (s.radio != ble::Radio::Connected) return lab.warn("nobody is connected");
  NimBLEDevice::getServer()->disconnect(s.peer.handle);
}

// The peripheral may only ASK; the central (the phone) decides. iPhones accept 15 ms and up.
void cmdParams(BleLab &lab, const String &, const String &lower) {
  const ble::LinkState s = ble::link().get();
  if (s.radio != ble::Radio::Connected) return lab.warn("connect a phone first");
  // Units: interval 1.25 ms, timeout 10 ms.
  if (lower == "fast") NimBLEDevice::getServer()->updateConnParams(s.peer.handle, 12, 24, 0, 400);         // 15-30 ms
  else if (lower == "slow") NimBLEDevice::getServer()->updateConnParams(s.peer.handle, 80, 160, 4, 600);   // 100-200 ms, skip 4
  else return lab.warn("params fast | params slow   (the phone may say no)");
  BLE_NOTE(Tx, "asked the phone for %s parameters", lower.c_str());
}

void cmdPhy(BleLab &lab, const String &, const String &lower) {
  const ble::LinkState s = ble::link().get();
  if (s.radio != ble::Radio::Connected) return lab.warn("connect a phone first");
  uint8_t mask = 0;
  if (lower == "1m") mask = BLE_GAP_LE_PHY_1M_MASK;
  else if (lower == "2m") mask = BLE_GAP_LE_PHY_2M_MASK;
  else return lab.warn("phy 1m | phy 2m   (2M = twice the bit rate, a bit less range)");
  NimBLEDevice::getServer()->updatePhy(s.peer.handle, mask, mask, 0);
  BLE_NOTE(Tx, "asked for the %s PHY", lower == "2m" ? "2M" : "1M");
}

void cmdLed(BleLab &lab, const String &, const String &lower) {
  if (lower != "auto") return lab.warn("led auto: the LED shows the radio's state again");
  lab.claimLed(false);
  BLE_NOTE(Info, "LED shows the radio again");
}

const BleCommand kCommands[] = {
    {"adv", "on|off", "advertise (visible, connectable) or hide", cmdAdv},
    {"message", "<text>", "set the Message value from the board", cmdMessage},
    {"press", "", "send a Button click notification", cmdPress},
    {"params", "fast|slow", "ask the phone for another connection interval", cmdParams},
    {"phy", "1m|2m", "ask for the 1M or 2M radio mode", cmdPhy},
    {"disconnect", "", "hang up on the phone", cmdDisconnect},
    {"led", "auto", "after the phone set the LED: show radio state again", cmdLed},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(BleLab &lab) {
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&links, false);
  bench_.begin(server, bench::Options{});
  server->start();
  bench::advertise(lab.spec().deviceName);
}

void tick(BleLab &lab) {
  bench_.tick();
  uint8_t c[3];
  if (bench_.takeLed(c)) {
    lab.claimLed(true);
    rgbLedWrite(PIN_RGB, c[0], c[1], c[2]);
  }
}

// Every gesture goes to the phone too; then the shell does its usual thing (return false).
bool gesture(BleLab &, Gesture g, bool) {
  bench_.button(g == Gesture::Click ? 1 : g == Gesture::DoubleClick ? 2 : 3);
  return false;
}

const BleLabSpec kSpec{
    "ble-lab-b3", "B3", "Connect & talk", "Bench B3", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, tick, gesture, nullptr,
};

BleLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
