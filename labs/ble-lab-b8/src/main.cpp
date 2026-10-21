// Bluetooth lab B8 Host.  uv run pio run -d labs/ble-lab-b8 -e ble-lab-b8 -t upload
// Roles swap: until now a phone or laptop was the central and the board the peripheral. Here the
// board is the central, like a laptop. It connects to devices, explores what they offer (read-only:
// the public basics, never anything vendor-specific), and hosts your own BLE keyboard and mouse:
// it pairs (you type the passkey on the keyboard), reads the Report Map, subscribes to the input
// reports, and turns them into text and a paint cursor on the screen.
//
// Wiring: the screen from lesson 03. The push button on GPIO 5 is optional (it does what BOOT does).
//
// Views: devices · explore · keyboard · mouse · reports · link · log
// Button: click = next view, double-click = the view's action, long press = the next step
// (connect -> host -> disconnect). While a numeric comparison waits: click = yes, long press = no.
// Serial: help lists the commands. Shared code: kit/lib/ble-kit (hid_report.h decodes the Report Map).
#include "b8_views.h"
#include "ble_lab.h"
#include "central.h"

namespace {

b8::Central central;
b8v::Pick pick;
ble::Heard rows[ble::ScanTable::kMax];
int rowCount = 0;
int scroll = 0;
b8::Progress prog;
b8::Input in;
b8::Tree tr;

const ble::Heard *picked() {
  for (int i = 0; i < rowCount; i++)
    if (pick.matches(rows[i])) return &rows[i];
  return nullptr;
}

// ---- Views ------------------------------------------------------------------------------------

bool connectedNow() { return central.connected(); }

void drawDevices(BleLab &, FrameBuffer &fb, float t) { b8v::devices(fb, rows, rowCount, pick, central, prog, t); }
void drawExplore(BleLab &, FrameBuffer &fb, float t) {
  b8::tree().copyTo(tr);
  b8v::explore(fb, tr, scroll, prog, t);
}
void drawKeyboard(BleLab &, FrameBuffer &fb, float t) {
  b8::input().copyTo(in);
  b8v::keyboard(fb, in, prog, connectedNow(), central.hid.layout(), t);
}
void drawMouse(BleLab &, FrameBuffer &fb, float t) {
  b8::input().copyTo(in);
  b8v::mouse(fb, in, prog, connectedNow(), t);
}
void drawReports(BleLab &, FrameBuffer &fb, float t) {
  b8::input().copyTo(in);
  b8v::reports(fb, in, central.hid, prog, connectedNow(), t);
}
void drawLink(BleLab &lab, FrameBuffer &fb, float t) { bv::link(fb, lab.state(), t, "this board"); }
void drawLog(BleLab &, FrameBuffer &fb, float) { bv::log(fb); }

const BleView kViews[] = {
    {"devices", drawDevices, "next"},
    {"explore", drawExplore, "more"},
    {"keyboard", drawKeyboard, "clear"},
    {"mouse", drawMouse, "clear"},
    {"reports", drawReports, nullptr},
    {"link", drawLink, nullptr},
    {"log", drawLog, nullptr},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdScan(BleLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("scan | scan on | scan off");
  const bool on = lower.isEmpty() ? !NimBLEDevice::getScan()->isScanning() : lower == "on";
  if (on && central.connected()) return lab.warn("connected: disconnect first (one link at a time in this lab)");
  if (on) central.startScan(), BLE_NOTE(Info, "scanning");
  else central.stopScan(), BLE_NOTE(Info, "scan paused");
}

void cmdList(BleLab &, const String &, const String &) {
  Serial.printf("%d connectable devices (strongest first):\n", rowCount);
  for (int i = 0; i < rowCount; i++) {
    const ble::Heard &h = rows[i];
    char addr[18];
    h.addrText(addr, sizeof(addr));
    Serial.printf("%3d  %4d dBm  %-14s  %s  %s%s%s\n", i + 1, int(h.rssiAvg), ad::kindName(h.kind), addr, h.label(),
                  h.kind == ad::Kind::Input ? "  [HID]" : "", central.isBonded(h.addr, h.addrType) ? "  [bonded]" : "");
  }
}

void pickIndex(int i) {
  if (i < 0 || i >= rowCount) return;
  pick.take(rows[i]);
  BLE_NOTE(Info, "picked %d: %s", i + 1, rows[i].label());
}

void cmdNext(BleLab &, const String &, const String &) {
  if (!rowCount) return;
  int at = -1;
  for (int i = 0; i < rowCount; i++)
    if (pick.matches(rows[i])) at = i;
  pickIndex((at + 1) % rowCount);
}

void cmdConnect(BleLab &lab, const String &arg, const String &) {
  long n = 0;
  if (!arg.isEmpty()) {
    if (!BleLab::number(arg, 1, std::max(rowCount, 1), n)) return lab.warn("connect <1-%d> (the number from list)", rowCount);
    pickIndex(n - 1);
  }
  const ble::Heard *h = picked();
  if (!h) return lab.warn("nothing picked: devices view double-click, or connect <n>");
  if (central.connected()) return lab.warn("already connected: disconnect first");
  b8::Request r;
  r.job = b8::Job::Connect;
  memcpy(r.addr, h->addr, 6);
  r.addrType = h->addrType;
  strlcpy(r.name, h->label(), sizeof(r.name));
  scroll = 0;
  central.submit(r);
}

void simple(b8::Job job) {
  b8::Request r;
  r.job = job;
  central.submit(r);
}

void cmdDisconnect(BleLab &lab, const String &, const String &) {
  if (!central.connected()) return lab.warn("nothing connected");
  simple(b8::Job::Disconnect);
}
void cmdExplore(BleLab &lab, const String &, const String &) {
  if (!central.connected()) return lab.warn("connect first");
  scroll = 0;
  simple(b8::Job::Explore);
}
void cmdPair(BleLab &lab, const String &, const String &) {
  if (!central.connected()) return lab.warn("connect first");
  simple(b8::Job::Pair);
}
void cmdHost(BleLab &lab, const String &, const String &) {
  if (!central.connected()) return lab.warn("connect a keyboard or mouse first");
  simple(b8::Job::Host);
}
void cmdForget(BleLab &, const String &, const String &) { simple(b8::Job::Forget); }

void cmdBoot(BleLab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("boot on | boot off   (now %s)", central.boot() ? "on" : "off");
  central.setBoot(lower == "on");
  BLE_NOTE(Info, "%s protocol from the next host", central.boot() ? "boot" : "report");
  if (prog.phase == b8::Phase::Hosting) simple(b8::Job::Boot);
}

void cmdLayout(BleLab &lab, const String &, const String &lower) {
  if (lower != "us" && lower != "de") return lab.warn("layout us | layout de   (the layout your keyboard is printed with)");
  central.hid.setLayout(lower == "de" ? hid::Layout::De : hid::Layout::Us);
  BLE_NOTE(Info, "layout %s: the same keys, other letters", lower == "de" ? "German (QWERTZ)" : "US (QWERTY)");
}

void cmdClear(BleLab &lab, const String &, const String &) {
  b8::input().edit([](b8::Input &i) {
    i.len = 0;
    i.strokeHead = 0;
  });
  (void)lab;
}

void cmdMore(BleLab &, const String &, const String &) {
  b8::tree().copyTo(tr);
  scroll = scroll + 8 >= tr.count ? 0 : scroll + 8;
}

void cmdYes(BleLab &lab, const String &, const String &) {
  if (!central.links.answer(central.client(), true)) lab.warn("no numeric comparison is waiting");
}
void cmdNo(BleLab &lab, const String &, const String &) {
  if (!central.links.answer(central.client(), false)) lab.warn("no numeric comparison is waiting");
}
void cmdPasskey(BleLab &lab, const String &arg, const String &) {
  long pin = 0;
  if (!BleLab::number(arg, 0, 999999, pin)) return lab.warn("passkey <6 digits shown on the device>");
  if (!central.links.enter(central.client(), uint32_t(pin))) lab.warn("no passkey entry is waiting");
}

const BleCommand kCommands[] = {
    {"scan", "[on|off]", "listen for connectable devices (alone: toggle)", cmdScan},
    {"list", "", "print the connectable devices", cmdList},
    {"next", "", "pick the next device", cmdNext},
    {"connect", "[n]", "connect to device n (or the picked one), then explore", cmdConnect},
    {"explore", "", "discover its services again and read the basics", cmdExplore},
    {"more", "", "scroll the explore view", cmdMore},
    {"pair", "", "encrypt + bond (keyboards: type the passkey on them)", cmdPair},
    {"host", "", "use it: read the Report Map, subscribe to its reports", cmdHost},
    {"boot", "on|off", "boot protocol (fixed formats) instead of the Report Map", cmdBoot},
    {"layout", "us|de", "how key codes become letters", cmdLayout},
    {"clear", "", "empty the editor and the drawing", cmdClear},
    {"disconnect", "", "hang up", cmdDisconnect},
    {"forget", "", "delete every bond on the board", cmdForget},
    {"yes", "", "numeric comparison: the numbers match", cmdYes},
    {"no", "", "numeric comparison: they don't", cmdNo},
    {"passkey", "<digits>", "the device shows a passkey: type it here", cmdPasskey},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(BleLab &) { central.begin(); }

void tick(BleLab &lab) {
  // Only connectable devices: a broadcast-only beacon can't be connected to.
  const int n = central.table.snapshot(rows, ble::ScanTable::kMax, 20000);
  rowCount = 0;
  for (int i = 0; i < n; i++)
    if (rows[i].connectable) rows[rowCount++] = rows[i];
  if (!pick.set && rowCount) pick.take(rows[0]);
  b8::progress().copyTo(prog);
  const int want = central.wantView;
  if (want >= 0) {
    central.wantView = -1;
    lab.setView(want);
  }
}

// The long press walks the path: connect -> host -> disconnect.
bool gesture(BleLab &lab, Gesture g, bool) {
  if (central.links.comparing() && g != Gesture::DoubleClick) {
    central.links.answer(central.client(), g == Gesture::Click);
    return true;
  }
  if (g != Gesture::LongPress) return false;
  b8::tree().copyTo(tr);
  const char *next = !central.connected() ? "connect" : (tr.hasHid && prog.phase != b8::Phase::Hosting) ? "host" : "disconnect";
  Serial.printf("> %s   (long press)\n", next);
  lab.runLine(next);
  return true;
}

const BleLabSpec kSpec{
    "ble-lab-b8", "B8", "Host", "Bench B8", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, tick, gesture, nullptr,
};

BleLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
