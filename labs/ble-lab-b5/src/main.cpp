// Bluetooth lab B5 Phone app.  uv run pio run -d labs/ble-lab-b5 -e ble-lab-b5 -t upload
// Our own app instead of nRF Connect: esp-labs/apps/ble-app talks to the board with Web Bluetooth,
// straight from Chrome (Mac, Android) or Bluefy (iPhone). No Wi-Fi, no server. B3's Bench service
// stays as it was; B5 adds a sketch pad, live sensors, a speed test and link controls on top.
//
// Wiring: the screen from lesson 03. Optional: a jumper wire on GPIO 1 (touch its free end: the
// live view's touch line jumps). The push button on GPIO 5 is optional (it does what BOOT does).
//
// Views: sketch · live · speed · gatt · link · log   (click = next view, double-click = footer action)
// Every gesture is also sent to a subscribed app as a Button notification (B3's characteristic).
// Serial: help lists the commands. Shared code: kit/lib/ble-kit; B5's own characteristics: extras.h.
#include "b5_views.h"
#include "bench_views.h"
#include "ble_lab.h"
#include "server_link.h"

namespace {

// B3's link callbacks, plus: a new connection starts with no subscriptions.
class Links : public ble::ServerLink {
 public:
  void onConnect(NimBLEServer *s, NimBLEConnInfo &c) override;
};

Links links;
bench::BenchService bench_;
b5::Extras extras;
b5::Sketch sketchPad;
b5::Samples samples;
uint32_t sampleAt = 0;

void Links::onConnect(NimBLEServer *s, NimBLEConnInfo &c) {
  ble::ServerLink::onConnect(s, c);
  extras.resetSubscriptions();
}

// ---- Views ------------------------------------------------------------------------------------

void drawSketch(BleLab &lab, FrameBuffer &fb, float) { b5v::sketch(fb, sketchPad, extras.state(), lab.state().radio == ble::Radio::Connected); }
void drawLive(BleLab &, FrameBuffer &fb, float) { b5v::live(fb, samples, extras.state()); }
void drawSpeed(BleLab &, FrameBuffer &fb, float t) { b5v::speed(fb, extras.state(), t); }
void drawGatt(BleLab &, FrameBuffer &fb, float t) { bv::gatt(fb, bench_.state(), t); }
void drawLink(BleLab &lab, FrameBuffer &fb, float t) { bv::link(fb, lab.state(), t, "this board", "app"); }
void drawLog(BleLab &, FrameBuffer &fb, float) { bv::log(fb); }

const BleView kViews[] = {
    {"sketch", drawSketch, "clear"},
    {"live", drawLive, nullptr},
    {"speed", drawSpeed, "speed 50000"},
    {"gatt", drawGatt, "press"},
    {"link", drawLink, "params fast"},
    {"log", drawLog, nullptr},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdAdv(BleLab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("adv on | adv off");
  if (lower == "on") return bench::advertise(lab.spec().deviceName);
  NimBLEDevice::getAdvertising()->stop();
  ble::link().edit([](ble::LinkState &s) {
    s.advertising = false;
    if (s.radio == ble::Radio::Advertising) s.radio = ble::Radio::Off;
  });
  BLE_NOTE(Info, "advertising off: invisible to phones");
}

void cmdClear(BleLab &lab, const String &, const String &) {
  sketchPad.clear();
  lab.showView("sketch");
  BLE_NOTE(Info, "sketch cleared");
}

void cmdSpeed(BleLab &lab, const String &arg, const String &lower) {
  if (lower == "stop") return extras.stopSpeed();
  long bytes = 0;
  if (!BleLab::number(arg, 1000, 1000000, bytes)) return lab.warn("speed <bytes> (1000-1000000) | speed stop");
  lab.showView("speed");
  extras.startSpeed(uint32_t(bytes));
}

void cmdPress(BleLab &, const String &, const String &) { bench_.button(1); }

void linkOp(BleLab &lab, uint8_t op) {
  const ble::LinkState s = ble::link().get();
  if (s.radio != ble::Radio::Connected) return lab.warn("connect the app first");
  NimBLEServer *server = NimBLEDevice::getServer();
  // Units: interval 1.25 ms, timeout 10 ms. The central (the phone or Mac) has the last word.
  switch (op) {
    case 1: server->updateConnParams(s.peer.handle, 6, 12, 0, 400); BLE_NOTE(Tx, "asked for 7.5-15 ms"); break;
    case 2: server->updateConnParams(s.peer.handle, 80, 160, 4, 600); BLE_NOTE(Tx, "asked for 100-200 ms, latency 4"); break;
    case 3: server->updatePhy(s.peer.handle, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0); BLE_NOTE(Tx, "asked for the 2M PHY"); break;
    case 4: server->updatePhy(s.peer.handle, BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_1M_MASK, 0); BLE_NOTE(Tx, "asked for the 1M PHY"); break;
    case 5:
      // Data Length Extension: link-layer packets of up to 251 bytes instead of 27, so one
      // 247-byte MTU packet no longer has to be cut into 10 radio packets.
      server->setDataLen(s.peer.handle, 251);
      BLE_NOTE(Tx, "asked for 251-byte radio packets");
      break;
  }
}

void cmdParams(BleLab &lab, const String &, const String &lower) {
  if (lower == "fast") return linkOp(lab, 1);
  if (lower == "slow") return linkOp(lab, 2);
  lab.warn("params fast | params slow   (the central may say no)");
}

void cmdPhy(BleLab &lab, const String &, const String &lower) {
  if (lower == "2m") return linkOp(lab, 3);
  if (lower == "1m") return linkOp(lab, 4);
  lab.warn("phy 1m | phy 2m");
}

void cmdDle(BleLab &lab, const String &, const String &) { linkOp(lab, 5); }

void cmdDisconnect(BleLab &lab, const String &, const String &) {
  const ble::LinkState s = ble::link().get();
  if (s.radio != ble::Radio::Connected) return lab.warn("nobody is connected");
  NimBLEDevice::getServer()->disconnect(s.peer.handle);
}

void cmdLed(BleLab &lab, const String &, const String &lower) {
  if (lower != "auto") return lab.warn("led auto: the LED shows the radio's state again");
  lab.claimLed(false);
  BLE_NOTE(Info, "LED shows the radio again");
}

const BleCommand kCommands[] = {
    {"adv", "on|off", "advertise (visible, connectable) or hide", cmdAdv},
    {"clear", "", "wipe the sketch", cmdClear},
    {"speed", "<bytes>|stop", "notify that many bytes to the app, as fast as it goes", cmdSpeed},
    {"params", "fast|slow", "ask the central for another connection interval", cmdParams},
    {"phy", "1m|2m", "ask for the 1M or 2M radio mode", cmdPhy},
    {"dle", "", "ask for 251-byte radio packets (data length extension)", cmdDle},
    {"press", "", "send a Button click notification", cmdPress},
    {"disconnect", "", "hang up on the app", cmdDisconnect},
    {"led", "auto", "after the app set the LED: show radio state again", cmdLed},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(BleLab &lab) {
  if (!sketchPad.begin()) BLE_NOTE(Bad, "no memory for the sketch buffer");
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&links, false);
  bench_.begin(server, bench::Options{});
  extras.begin(bench_.service());  // before start(): the GATT table is fixed once the server runs
  server->start();
  // Ask for the biggest ATT packet; the app's side decides the final MTU.
  NimBLEDevice::setMTU(247);
  bench::advertise(lab.spec().deviceName);
}

void tick(BleLab &lab) {
  bench_.tick();
  uint8_t c[3];
  if (bench_.takeLed(c)) {
    lab.claimLed(true);
    rgbLedWrite(PIN_RGB, c[0], c[1], c[2]);
  }
  // Draw what the app sent: at most 16 queued writes per frame.
  b5::CanvasWrite w;
  for (int i = 0; i < 16 && extras.takeCanvas(w); i++) {
    const canvas::Result r = sketchPad.apply(w.data, w.len);
    if (r.unknown) BLE_NOTE(Warn, "sketch: unknown command, rest of the write skipped");
    else if (r.truncated) BLE_NOTE(Warn, "sketch: a command was cut off");
  }
  if (const uint8_t op = extras.takeLinkOp()) linkOp(lab, op);
  if (const uint32_t bytes = extras.takeSpeedStart()) {
    lab.showView("speed");
    extras.startSpeed(bytes);
  }
  // Sensors every 200 ms: into the sparklines, and to the app if it subscribed.
  if (millis() - sampleAt >= 200) {
    sampleAt = millis();
    const float temp = temperatureRead();
    const uint32_t touch = touchRead(1);
    const uint32_t heap = ESP.getFreeHeap();
    const int8_t rssi = lab.state().radio == ble::Radio::Connected ? lab.state().peer.rssi : 0;
    samples.push(temp, float(touch), float(heap), float(rssi));
    extras.sendSensors(temp, touch, heap, rssi);
  }
}

bool gesture(BleLab &, Gesture g, bool) {
  bench_.button(g == Gesture::Click ? 1 : g == Gesture::DoubleClick ? 2 : 3);
  return false;
}

const BleLabSpec kSpec{
    "ble-lab-b5", "B5", "Phone app", "Bench B5", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, tick, gesture, nullptr,
};

BleLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
