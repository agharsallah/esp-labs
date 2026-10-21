// Bluetooth lab B1 Listen.  uv run pio run -d labs/ble-lab-b1 -e ble-lab-b1 -t upload
// The board only listens: every Bluetooth LE device around you shouts short "advertising" packets
// on three radio channels, many times a second, whether anyone listens or not. B1 hears them all,
// guesses what each one is, and takes the packets apart byte by byte. Nothing is ever sent to them.
//
// Wiring: the screen from lesson 03. The push button on GPIO 5 is optional (it does what BOOT does).
//
// Views: radar · list · inspect · air   (click = next, double-click = the action in the footer,
//        long press = pause or resume the scan)
// Serial: help lists the commands. Shared code: kit/lib/ble-kit.
#include "b1_views.h"
#include "ble_lab.h"
#include "listener.h"

namespace {

b1::Listener ears;
b1::AirHistory history;
b1::Pick pick;
ble::Heard rows[ble::ScanTable::kMax];
int rowCount = 0;

// The picked device in this frame's rows, or nullptr when it went quiet.
const ble::Heard *picked() {
  for (int i = 0; i < rowCount; i++)
    if (pick.matches(rows[i])) return &rows[i];
  return nullptr;
}

void pickIndex(int i) {
  if (i < 0 || i >= rowCount) return;
  pick.take(rows[i]);
  char addr[18];
  rows[i].addrText(addr, sizeof(addr));
  BLE_NOTE(Info, "picked %d: %s %s", i + 1, rows[i].label(), addr);
}

// ---- Views ------------------------------------------------------------------------------------

void drawRadar(BleLab &, FrameBuffer &fb, float t) { b1v::radar(fb, rows, rowCount, pick, ears.settings, ears.scanning(), history.latest(), t); }
void drawList(BleLab &, FrameBuffer &fb, float) { b1v::list(fb, rows, rowCount, pick); }
void drawInspect(BleLab &, FrameBuffer &fb, float) { b1v::inspect(fb, picked(), ears.settings); }
void drawAir(BleLab &, FrameBuffer &fb, float) { b1v::air(fb, history, rows, rowCount, ears.settings, ears.scanning()); }

const BleView kViews[] = {
    {"radar", drawRadar, "next"},
    {"list", drawList, "next"},
    {"inspect", drawInspect, "watch"},
    {"air", drawAir, "mode"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdScan(BleLab &lab, const String &, const String &lower) {
  const bool on = lower.isEmpty() ? !ears.scanning() : lower == "on";
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("scan | scan on | scan off");
  on ? ears.start() : ears.stop();
}

void cmdMode(BleLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "active" && lower != "passive") return lab.warn("mode | mode active | mode passive");
  ears.settings.active = lower.isEmpty() ? !ears.settings.active : lower == "active";
  ears.start();
}

void cmdTiming(BleLab &lab, const String &arg, const String &) {
  const int sp = arg.indexOf(' ');
  long interval = 0, window = 0;
  if (sp < 0 || !BleLab::number(arg.substring(0, sp), 10, 10240, interval) || !BleLab::number(arg.substring(sp + 1), 10, interval, window))
    return lab.warn("timing <interval ms> <window ms>, 10-10240, window <= interval (default 100 100)");
  ears.settings.intervalMs = uint16_t(interval);
  ears.settings.windowMs = uint16_t(window);
  ears.start();
}

void cmdNear(BleLab &lab, const String &arg, const String &lower) {
  long dbm = 0;
  if (lower == "off") ears.settings.nearDbm = -127;
  else if (BleLab::number(arg, -100, -20, dbm)) ears.settings.nearDbm = int(dbm);
  else return lab.warn("near <dBm> (e.g. near -70) | near off");
  if (ears.settings.nearDbm == -127) BLE_NOTE(Info, "showing every device");
  else BLE_NOTE(Info, "showing devices stronger than %d dBm", ears.settings.nearDbm);
}

void cmdPick(BleLab &lab, const String &arg, const String &) {
  long n = 0;
  if (!BleLab::number(arg, 1, std::max(rowCount, 1), n)) return lab.warn("pick <1-%d>: the number on the list view", rowCount);
  pickIndex(n - 1);
  lab.showView("inspect");
}

void cmdNext(BleLab &, const String &, const String &) {
  if (rowCount == 0) return;
  int at = -1;
  for (int i = 0; i < rowCount; i++)
    if (pick.matches(rows[i])) at = i;
  pickIndex((at + 1) % rowCount);
}

void cmdWatch(BleLab &lab, const String &, const String &lower) {
  if (lower == "off" || ears.watching()) {
    ears.watch(b1::Pick{});
    return BLE_NOTE(Info, "watch off");
  }
  if (!picked()) return lab.warn("pick a device first (list view, double-click; or pick <n>)");
  ears.watch(pick);
  BLE_NOTE(Info, "watching %s: one line per packet, see the gaps", picked()->label());
}

void cmdList(BleLab &, const String &, const String &) {
  Serial.printf("%d devices heard in the last 30 s (strongest first):\n", rowCount);
  Serial.println("  #  dBm  ~m     kind            address            type           packets  name / maker");
  for (int i = 0; i < rowCount; i++) {
    const ble::Heard &h = rows[i];
    char addr[18];
    h.addrText(addr, sizeof(addr));
    Serial.printf("%3d  %4d  %4.1f  %-14s  %s  %-14s %7lu  %s%s\n", i + 1, int(h.rssiAvg), ad::distanceMeters(int(h.rssiAvg), ears.settings.rssiAt1m),
                  ad::kindName(h.kind), addr, ad::addrKindName(h.addrKind()), static_cast<unsigned long>(h.packets), h.label(),
                  h.connectable ? "" : "  (broadcast only)");
  }
}

void printBytes(const char *what, const uint8_t *p, uint8_t n) {
  Serial.printf("  %s, %u of 31 bytes:\n   ", what, n);
  const ad::Parsed ps = ad::split(p, n);
  for (int f = 0; f < ps.count; f++) {
    const ad::Field &fl = ps.fields[f];
    Serial.printf("  [%02X %02X |", fl.len + 1, fl.type);
    for (int i = 0; i < fl.len; i++) Serial.printf(" %02X", p[fl.offset + i]);
    Serial.printf("] %s\n   ", ad::typeName(fl.type));
  }
  if (ps.malformed) Serial.print("  malformed: a length runs past the end");
  Serial.println();
}

void cmdBytes(BleLab &lab, const String &, const String &) {
  const ble::Heard *h = picked();
  if (!h) return lab.warn("pick a device first");
  char addr[18];
  h->addrText(addr, sizeof(addr));
  Serial.printf("%s  %s (%s)\n", h->label(), addr, ad::addrKindName(h->addrKind()));
  printBytes("advertisement", h->adv, h->advLen);
  if (h->rspLen) printBytes("scan response", h->rsp, h->rspLen);
  else Serial.println(ears.settings.active ? "  no scan response (it doesn't send one)" : "  scan response: switch to mode active to ask for it");
}

void cmdCalib(BleLab &lab, const String &, const String &) {
  const ble::Heard *h = picked();
  if (!h) return lab.warn("pick your phone first, hold it 1 m from the board, then calib");
  ears.settings.rssiAt1m = int(h->rssiAvg);
  BLE_NOTE(Good, "1 m = %d dBm now: distances use it", ears.settings.rssiAt1m);
}

void cmdClear(BleLab &, const String &, const String &) {
  ears.table.clear();
  pick = b1::Pick{};
  BLE_NOTE(Info, "table cleared");
}

const BleCommand kCommands[] = {
    {"scan", "[on|off]", "listen, or pause (alone: toggle; long press too)", cmdScan},
    {"mode", "[active|passive]", "active asks every device for its scan response", cmdMode},
    {"timing", "<interval> <window>", "scan interval and window in ms (default 100 100)", cmdTiming},
    {"near", "<dBm>|off", "hide devices weaker than this, e.g. near -70", cmdNear},
    {"list", "", "print every device heard in the last 30 s", cmdList},
    {"pick", "<n>", "inspect device n from the list", cmdPick},
    {"next", "", "pick the next device", cmdNext},
    {"bytes", "", "the picked device's packets, structure by structure", cmdBytes},
    {"watch", "[off]", "one line per packet of the picked device (its interval)", cmdWatch},
    {"calib", "", "the picked device is 1 m away: use its RSSI as 1 m", cmdCalib},
    {"clear", "", "forget every device", cmdClear},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(BleLab &) { ears.begin(); }

void tick(BleLab &) {
  rowCount = ears.table.snapshot(rows, ble::ScanTable::kMax, 30000, ears.settings.nearDbm);
  int recent = 0;  // "in range" = heard in the last 10 s
  for (int i = 0; i < rowCount; i++) recent += millis() - rows[i].lastSeen < 10000;
  history.tick(ears.table, recent);
  if (!pick.set && rowCount) pick.take(rows[0]);
}

const BleLabSpec kSpec{
    "ble-lab-b1", "B1", "Listen", "Bench B1", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, tick, nullptr, "scan",
};

BleLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
