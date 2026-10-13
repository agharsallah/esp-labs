// The shell every Bluetooth LE lab (B1-B8) shares: the screen with its status bar, the push button
// and BOOT, the on-board LED, serial commands, and printing the activity log.
//
// A lab is a BleLabSpec: its views (each one a draw function, plus what a double-click does there),
// its serial commands, and three hooks: setup() once NimBLE is running, tick() on every loop, and
// gesture() to give the button a lab-specific meaning. Each src/ble-lab-bN/main.cpp is that list.
//
// Button or BOOT, by default: click = next view · double-click = this view's action · long press =
// the lab's long action. While a numeric comparison waits: click = yes, long press = no.
// On-board LED, unless the lab claims it: breathing blue = advertising · cyan pulse = scanning ·
// steady blue = connected · violet = encrypted · amber blink = a passkey waits · white blink = data.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "ble_link.h"
#include "ble_views.h"
#include "button.h"
#include "display.h"
#include "pins.h"

class BleLab;

enum class Gesture : uint8_t { Click, DoubleClick, LongPress };

struct BleCommand {
  const char *name;  // the first word you type
  const char *args;  // for help, e.g. "<dBm>"
  const char *what;  // one line for help
  void (*run)(BleLab &lab, const String &arg, const String &lower);
};

struct BleView {
  const char *name;  // "radar"
  void (*draw)(BleLab &lab, FrameBuffer &fb, float t);
  const char *action;  // the command line a double-click runs on this view, or nullptr
};

struct BleLabSpec {
  const char *env;         // "ble-lab-b1"
  const char *id;          // "B1"
  const char *title;       // "Listen"
  const char *deviceName;  // what phones see: "Bench B3"
  const BleView *views;
  int viewCount;
  int startView;
  const BleCommand *commands;
  int commandCount;
  void (*setup)(BleLab &lab) = nullptr;  // after NimBLE is initialised
  void (*tick)(BleLab &lab) = nullptr;   // every loop, before drawing
  // First say on every gesture. Return true when handled, false for the default.
  bool (*gesture)(BleLab &lab, Gesture g, bool external) = nullptr;
  const char *longAction = nullptr;  // the command line a long press runs by default
};

class BleLab {
 public:
  explicit BleLab(const BleLabSpec &spec) : spec_(spec), view_(spec.startView) {}

  void begin() {
    Serial.begin(115200);
    delay(1500);
    if (!display_.begin()) {
      log_e("No memory for the frame buffer. Stopping.");
      return;
    }
    rgbLedWrite(PIN_RGB, 0, 0, 0);
    external_.begin();
    boot_.begin();
    if (!NimBLEDevice::init(spec_.deviceName)) {
      log_e("NimBLE did not start. Stopping.");
      return;
    }
    Serial.printf("\n%s: NimBLE %s up, address %s, TX power %d dBm\n", spec_.env, NimBLEDevice::getVersion(),
                  NimBLEDevice::getAddress().toString().c_str(), NimBLEDevice::getPower());
    if (spec_.setup) spec_.setup(*this);
    printHelp();
  }

  void loop() {
    FrameBuffer &fb = display_.fb();
    if (fb.pixels() == nullptr) {
      delay(1000);
      return;
    }
    pollSerial();
    handleButton(external_, true);
    handleButton(boot_, false);
    measureRssi();
    if (spec_.tick) spec_.tick(*this);
    link_ = ble::link().get();
    printActivity();
    showLed();

    const float t = millis() / 1000.0f;
    const BleView &v = spec_.views[view_];
    fb.fillScreen(pal::night);
    v.draw(*this, fb, t);
    if (passkeyShowing()) bv::passkey(fb, link_, t);
    bv::header(fb, spec_.id, v.name, view_, spec_.viewCount, link_, t);
    char hint[40];
    if (passkeyShowing() && link_.passkeyCompare) snprintf(hint, sizeof(hint), "click = yes, long = no");
    else if (v.action) snprintf(hint, sizeof(hint), "2x: %s", v.action);
    else snprintf(hint, sizeof(hint), "click: next view");
    bv::footer(fb, hint);
    display_.push();
  }

  // ---- For views and commands ------------------------------------------------------------------

  const ble::LinkState &state() const { return link_; }
  const BleLabSpec &spec() const { return spec_; }
  int view() const { return view_; }

  void setView(int v) {
    view_ = (v % spec_.viewCount + spec_.viewCount) % spec_.viewCount;
    log_i("view %d: %s", view_ + 1, spec_.views[view_].name);
  }
  void showView(const char *name) {
    for (int i = 0; i < spec_.viewCount; i++)
      if (strcmp(spec_.views[i].name, name) == 0) return setView(i);
  }

  // The lab drives the on-board LED itself (B3's LED characteristic); false hands it back.
  void claimLed(bool claimed) {
    ledClaimed_ = claimed;
    ledShown_ = 0xFFFFFFFF;
  }

  // log_w is a macro that can't be returned from a void function; this can.
  [[gnu::format(printf, 2, 3)]] void warn(const char *fmt, ...) {
    char text[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    log_w("%s", text);
  }

  // Parse a number argument; false when it is missing, not a number or out of range.
  static bool number(const String &arg, long lo, long hi, long &out) {
    if (arg.isEmpty()) return false;
    for (size_t i = 0; i < arg.length(); i++)
      if (!isDigit(arg[i]) && !(i == 0 && arg[i] == '-')) return false;
    out = arg.toInt();
    return out >= lo && out <= hi;
  }

  void runLine(const String &raw) {
    String line = raw;
    line.trim();
    if (line.isEmpty()) return;
    const int sp = line.indexOf(' ');
    String cmd = sp < 0 ? line : line.substring(0, sp);
    String arg = sp < 0 ? String() : line.substring(sp + 1);
    cmd.toLowerCase();
    arg.trim();
    String lower = arg;
    lower.toLowerCase();
    if (cmd == "help" || cmd == "?") return printHelp();
    if (cmd == "view") {
      long n = 0;
      if (!number(arg, 1, spec_.viewCount, n)) return warn("view <1-%d>", spec_.viewCount);
      return setView(n - 1);
    }
    if (cmd == "status") return printStatus();
    if (cmd == "power") return power(arg);
    for (int i = 0; i < spec_.commandCount; i++)
      if (cmd == spec_.commands[i].name) return spec_.commands[i].run(*this, arg, lower);
    log_w("unknown command '%s' in %s (type help)", line.c_str(), spec_.env);
  }

 private:
  bool passkeyShowing() const { return link_.passkey != 0 && millis() - link_.passkeySince < 30000; }

  void printHelp() {
    Serial.printf("\nBluetooth lab %s %s  (%s)  ·  phones see \"%s\"\n", spec_.id, spec_.title, spec_.env, spec_.deviceName);
    for (int i = 0; i < spec_.commandCount; i++) {
      const BleCommand &c = spec_.commands[i];
      char usage[40];
      snprintf(usage, sizeof(usage), "%s %s", c.name, c.args);
      Serial.printf("  %-28s %s\n", usage, c.what);
    }
    Serial.printf("  %-28s %s\n", "status", "the radio, the link and its security, in detail");
    Serial.printf("  %-28s %s\n", "power <dBm>", "transmit power: -24 to 20 (default 9)");
    Serial.printf("  %-28s %s\n", "view <n>", "show screen view n");
    Serial.print("Views:");
    for (int i = 0; i < spec_.viewCount; i++)
      Serial.printf(" %d %s%s", i + 1, spec_.views[i].name, i + 1 < spec_.viewCount ? " ·" : "\n");
    Serial.println("Button (GPIO 5, optional) or BOOT: click = next view, double-click = the view's action");
  }

  void printStatus() {
    const ble::LinkState s = ble::link().get();
    Serial.printf("radio %s · this board %s · TX power %d dBm\n", ble::radioName(s.radio), NimBLEDevice::getAddress().toString().c_str(),
                  NimBLEDevice::getPower());
    if (s.radio != ble::Radio::Connected) {
      if (s.lastDisconnect >= 0) Serial.printf("last link ended: 0x%02X %s\n", s.lastDisconnect & 0xFF, ble::disconnectReason(s.lastDisconnect));
      return;
    }
    const ble::Peer &p = s.peer;
    Serial.printf("peer %s%s%s (%s) · we are the %s\n", p.addr, p.idAddr[0] ? " id " : "", p.idAddr, p.name[0] ? p.name : "no name",
                  p.central ? "peripheral" : "central");
    Serial.printf("interval %.2f ms · latency %u · timeout %u ms · MTU %u · PHY %u/%u · RSSI %d dBm\n", p.interval * 1.25f, p.latency,
                  p.timeout * 10, p.mtu, p.txPhy, p.rxPhy, p.rssi);
    Serial.printf("security: %s, %s, %s, key %u bytes\n", p.encrypted ? "encrypted" : "not encrypted",
                  p.authenticated ? "authenticated (MITM)" : "not authenticated", p.bonded ? "bonded" : "not bonded", p.keySize);
    Serial.printf("traffic: rx %lu ops / %lu B · tx %lu ops / %lu B\n", static_cast<unsigned long>(s.rxOps), static_cast<unsigned long>(s.rxBytes),
                  static_cast<unsigned long>(s.txOps), static_cast<unsigned long>(s.txBytes));
  }

  void power(const String &arg) {
    long dbm = 0;
    if (!number(arg, -24, 20, dbm)) return warn("power <dBm>, -24 to 20. Now %d dBm", NimBLEDevice::getPower());
    if (!NimBLEDevice::setPower(int8_t(dbm))) return warn("the radio refused %ld dBm", dbm);
    BLE_NOTE(Info, "TX power %d dBm", NimBLEDevice::getPower());
  }

  void pollSerial() {
    static String buffer;
    while (Serial.available()) {
      const char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (!buffer.isEmpty()) runLine(buffer);
        buffer = "";
      } else if (c == 0x08 || c == 0x7F) {
        if (!buffer.isEmpty()) buffer.remove(buffer.length() - 1);
      } else if (uint8_t(c) < 0x20) {
        // Other control characters (arrow keys send escape sequences): ignore.
      } else if (buffer.length() < 120) {
        buffer += c;
      }
    }
  }

  void handleButton(Button &b, bool external) {
    Gesture g;
    switch (b.poll()) {
      case Button::Event::Click: g = Gesture::Click; break;
      case Button::Event::DoubleClick: g = Gesture::DoubleClick; break;
      case Button::Event::LongPress: g = Gesture::LongPress; break;
      default: return;
    }
    if (spec_.gesture && spec_.gesture(*this, g, external)) return;
    switch (g) {
      case Gesture::Click: setView(view_ + 1); break;
      case Gesture::DoubleClick:
        if (spec_.views[view_].action) {
          Serial.printf("> %s   (double-click)\n", spec_.views[view_].action);
          runLine(spec_.views[view_].action);
        }
        break;
      case Gesture::LongPress:
        if (spec_.longAction) {
          Serial.printf("> %s   (long press)\n", spec_.longAction);
          runLine(spec_.longAction);
        }
        break;
    }
  }

  // RSSI of a live link: the controller measures it on every connection event; we ask once a second.
  void measureRssi() {
    const ble::LinkState &s = link_;
    if (s.radio != ble::Radio::Connected || millis() - rssiAt_ < 1000) return;
    rssiAt_ = millis();
    int8_t rssi = 0;
    if (ble_gap_conn_rssi(s.peer.handle, &rssi) == 0) ble::link().edit([&](ble::LinkState &st) { st.peer.rssi = rssi; });
  }

  // New activity lines go to serial from here, in order, never from the NimBLE task.
  void printActivity() {
    ble::Activity &a = ble::activity();
    const uint32_t head = a.head();
    if (printed_ + ble::Activity::kSize < head) {
      Serial.printf("  ... %lu lines skipped\n", static_cast<unsigned long>(head - ble::Activity::kSize - printed_));
      printed_ = head - ble::Activity::kSize;
    }
    ble::ActEntry e;
    for (; printed_ < head; printed_++)
      if (a.get(printed_, e)) Serial.printf("  %s %s\n", bv::actMark(e.kind), e.text);
  }

  void showLed() {
    if (ledClaimed_) return;
    const uint32_t now = millis();
    const uint32_t traffic = link_.rxOps + link_.txOps;
    if (traffic != traffic_) traffic_ = traffic, flashUntil_ = now + 60;
    // Brightness in 12 steps, so the LED (a whole WS2812 frame per write) is only rewritten on a change.
    const float wave = 0.5f + 0.5f * sinf(now / 1000.0f * 3.1416f);
    const uint8_t breathe = uint8_t(int(wave * 11) * 8 + 6);
    uint8_t r = 0, g = 0, b = 0;
    if (passkeyShowing()) r = (now / 250) & 1 ? 120 : 0, g = r / 2;
    else if (now < flashUntil_) r = 120, g = 120, b = 120;
    else switch (link_.radio) {
      case ble::Radio::Off: break;
      case ble::Radio::Scanning: g = breathe / 2, b = breathe / 2; break;
      case ble::Radio::Advertising: b = breathe; break;
      case ble::Radio::Connecting: r = 60, g = 40; break;
      case ble::Radio::Connected:
        if (link_.peer.encrypted) r = 50, b = 90;
        else b = 90;
        break;
    }
    const uint32_t colour = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    if (colour == ledShown_) return;
    ledShown_ = colour;
    rgbLedWrite(PIN_RGB, r, g, b);
  }

  const BleLabSpec &spec_;
  Display display_;
  Button external_{PIN_BUTTON, "button"};
  Button boot_{PIN_BOOT_BTN, "BOOT"};
  ble::LinkState link_;
  int view_;
  bool ledClaimed_ = false;
  uint32_t ledShown_ = 0xFFFFFFFF, flashUntil_ = 0, traffic_ = 0, rssiAt_ = 0, printed_ = 0;
};
