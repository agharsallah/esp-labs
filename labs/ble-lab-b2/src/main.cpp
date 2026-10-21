// Bluetooth lab B2 Be seen.  uv run pio run -d labs/ble-lab-b2 -e ble-lab-b2 -t upload
// The board turns from listener into broadcaster: a beacon. It packs live values (button presses,
// the chip's temperature, uptime) into its advertisement, so any phone nearby can read them without
// ever connecting. Then the same 31 bytes as an iBeacon, as an Eddystone-URL, or just a name, and
// what interval and TX power cost in battery.
//
// Wiring: the screen from lesson 03. The push button on GPIO 5 is optional (it does what BOOT does).
// The scanner is your phone: nRF Connect (Android, iPhone) or LightBlue.
//
// Views: packet · air · budget · log   (click = next view, double-click = press: the counter in the
//        packet goes up, long press = advertising on/off)
// Serial: help lists the commands. Shared code: kit/lib/ble-kit; the packets: beacon_payload.h.
#include "b2_views.h"
#include "beacon.h"
#include "ble_lab.h"
#include "server_link.h"

namespace {

b2::Beacon beacon;
b2v::AirSim airSim;
ble::ServerLink links;
uint32_t liveAt = 0;

bool on() { return beacon.settings.on; }

void drawPacket(BleLab &, FrameBuffer &fb, float) { b2v::packet(fb, beacon, on()); }
void drawAir(BleLab &, FrameBuffer &fb, float t) { b2v::air(fb, beacon, airSim, on(), t); }
void drawBudget(BleLab &, FrameBuffer &fb, float) { b2v::budget(fb, beacon); }
void drawLog(BleLab &, FrameBuffer &fb, float) { bv::log(fb); }

const BleView kViews[] = {
    {"packet", drawPacket, "press"},
    {"air", drawAir, "press"},
    {"budget", drawBudget, "press"},
    {"log", drawLog, "press"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdMode(BleLab &lab, const String &, const String &lower) {
  static const b2::Mode modes[] = {b2::Mode::Bench, b2::Mode::IBeacon, b2::Mode::Url, b2::Mode::Name};
  for (b2::Mode m : modes)
    if (lower == b2::modeName(m)) {
      const b2::Mode before = beacon.settings.mode;
      beacon.settings.mode = m;
      if (!beacon.apply(true)) beacon.settings.mode = before, beacon.apply(true);
      return lab.showView("packet");
    }
  lab.warn("mode bench | ibeacon | url | name");
}

void cmdUrl(BleLab &lab, const String &arg, const String &) {
  if (arg.isEmpty() || arg.length() >= sizeof(beacon.settings.url)) return lab.warn("url <https://...> (short: at most 17 bytes after the scheme)");
  char before[sizeof(beacon.settings.url)];
  strlcpy(before, beacon.settings.url, sizeof(before));
  strlcpy(beacon.settings.url, arg.c_str(), sizeof(beacon.settings.url));
  beacon.settings.mode = b2::Mode::Url;
  if (!beacon.apply(true)) {
    strlcpy(beacon.settings.url, before, sizeof(beacon.settings.url));
    beacon.apply(true);
  }
}

void cmdBeacon(BleLab &lab, const String &arg, const String &) {
  const int sp = arg.indexOf(' ');
  long major = 0, minor = 0;
  if (sp < 0 || !BleLab::number(arg.substring(0, sp), 0, 65535, major) || !BleLab::number(arg.substring(sp + 1), 0, 65535, minor))
    return lab.warn("beacon <major> <minor>, each 0-65535");
  beacon.settings.beacon.major = uint16_t(major);
  beacon.settings.beacon.minor = uint16_t(minor);
  beacon.settings.mode = b2::Mode::IBeacon;
  beacon.apply(true);
}

void cmdInterval(BleLab &lab, const String &arg, const String &) {
  long ms = 0;
  const long lo = b2::minIntervalMs(beacon.settings);
  if (!BleLab::number(arg, lo, 10240, ms))
    return lab.warn("interval <ms>, %ld-10240 (%s: %ld ms is the floor)", lo, beacon.settings.connectable ? "connectable" : "not connectable", lo);
  beacon.settings.intervalMs = uint16_t(ms);
  beacon.apply(true);
}

void cmdConnectable(BleLab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("connectable on | connectable off");
  beacon.settings.connectable = lower == "on";
  if (beacon.settings.intervalMs < b2::minIntervalMs(beacon.settings)) {
    beacon.settings.intervalMs = b2::minIntervalMs(beacon.settings);
    BLE_NOTE(Warn, "interval raised to %u ms: the floor for non-connectable", beacon.settings.intervalMs);
  }
  beacon.apply(true);
}

void cmdScanResp(BleLab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("scanresp on | scanresp off");
  beacon.settings.scanResponse = lower == "on";
  beacon.apply(true);
}

void cmdAdv(BleLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("adv | adv on | adv off");
  beacon.settings.on = lower.isEmpty() ? !beacon.settings.on : lower == "on";
  NimBLEDevice::getServer()->advertiseOnDisconnect(beacon.settings.on);
  links.setAdvertiseAgain(beacon.settings.on);
  if (beacon.settings.on) beacon.apply(true);
  else beacon.stop();
}

void cmdPress(BleLab &, const String &, const String &) {
  beacon.live.presses++;
  beacon.apply(false);
  BLE_NOTE(Info, "presses %u: in the next packet", beacon.live.presses);
}

void printPacket(const char *what, const b2::Packet &p) {
  Serial.printf("  %s, %u of 31 bytes:\n", what, p.n);
  const ad::Parsed ps = ad::split(p.b, p.n);
  for (int f = 0; f < ps.count; f++) {
    const ad::Field &fl = ps.fields[f];
    Serial.printf("    [%02X %02X |", fl.len + 1, fl.type);
    for (int i = 0; i < fl.len; i++) Serial.printf(" %02X", p.b[fl.offset + i]);
    Serial.printf("]  %s\n", ad::typeName(fl.type));
  }
}

void cmdBytes(BleLab &, const String &, const String &) {
  Serial.printf("%s · %s · every %u ms · TX %d dBm\n", b2::modeName(beacon.settings.mode), b2::pduName(beacon.settings), beacon.settings.intervalMs,
                NimBLEDevice::getPower());
  printPacket("advertisement", beacon.adv);
  if (beacon.rsp.n) printPacket("scan response", beacon.rsp);
  else Serial.println(beacon.settings.scanResponse ? "  scan response: empty" : "  scan response: off");
}

const BleCommand kCommands[] = {
    {"mode", "bench|ibeacon|url|name", "what the advertisement carries", cmdMode},
    {"url", "<https://...>", "the Eddystone-URL (switches to url mode)", cmdUrl},
    {"beacon", "<major> <minor>", "the iBeacon numbers (switches to ibeacon mode)", cmdBeacon},
    {"interval", "<ms>", "time between advertising events (default 100)", cmdInterval},
    {"connectable", "on|off", "may a phone connect? off = a pure beacon", cmdConnectable},
    {"scanresp", "on|off", "answer active scanners with a second packet", cmdScanResp},
    {"adv", "[on|off]", "advertise or go silent (alone: toggle; long press too)", cmdAdv},
    {"press", "", "count a press into the packet (double-click too)", cmdPress},
    {"bytes", "", "both packets, structure by structure", cmdBytes},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(BleLab &) {
  // A server with no services of its own: in connectable mode a phone may connect, and the link
  // view's state stays right. Advertising starts again by itself when the phone hangs up.
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&links, false);
  beacon.live.tempCenti = int16_t(temperatureRead() * 100);
  beacon.apply(true);
}

// Once a second: fresh live values into the advertisement (the controller keeps sending the old
// bytes until we hand it new ones).
void tick(BleLab &) {
  airSim.tick(beacon.settings.intervalMs, on() && ble::link().get().radio != ble::Radio::Connected);
  if (millis() - liveAt < 1000) return;
  liveAt = millis();
  beacon.live.tempCenti = int16_t(temperatureRead() * 100);
  beacon.live.uptime = uint16_t(millis() / 1000);
  if (on()) beacon.apply(false);
}

const BleLabSpec kSpec{
    "ble-lab-b2", "B2", "Be seen", "Bench B2", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, tick, nullptr, "adv",
};

BleLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
