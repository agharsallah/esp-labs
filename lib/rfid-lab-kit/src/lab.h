// The shell every RFID lab (R1-R6) shares: the screen, the push button and BOOT, the on-board
// LED, serial input, and the reader task that owns the RC522.
//
// A lab is a LabSpec: which screen views it has (and what a double-click does on each), and which
// serial commands it understands. Each src/rfid-lab-rN/main.cpp is just that list, so every lab's
// `help` shows exactly what that lesson teaches.
//
// Button or BOOT: click = next view · double-click = this view's action · long press = pause polling
// On-board LED: bright green flash = a card was just read · steady green = a card is on the reader ·
//               red = the reader chip isn't answering (wiring) · off = no card
#pragma once

#include <Arduino.h>

#include "button.h"
#include "color.h"
#include "display.h"
#include "pins.h"
#include "reader.h"
#include "rfid_views.h"

enum class View : uint8_t { Chip, Card, Trace, Memory, Credits, Edge };

inline const char *viewName(View v) {
  static const char *const names[] = {"chip", "card", "trace", "memory", "credits", "edge"};
  return names[static_cast<int>(v)];
}

class Lab;

struct LabCommand {
  const char *name;  // the first word you type
  const char *args;  // for help, e.g. "<block> <text>"
  const char *what;  // one line for help
  void (*run)(Lab &lab, const String &arg, const String &lower);
};

struct LabView {
  View view;
  const char *action;  // the command line a double-click runs on this view, or nullptr
};

struct LabSpec {
  const char *env;    // "rfid-lab-r1"
  const char *title;  // "R1 Wire & talk"
  const LabView *views;
  int viewCount;
  int startView;
  const LabCommand *commands;
  int commandCount;
};

class Lab {
 public:
  explicit Lab(const LabSpec &spec) : spec_(spec), view_(spec.startView) {}

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
    reader_.begin();
    log_i("%s: reader task started on core 0, screen on core %d", spec_.env, xPortGetCoreID());
    printHelp();
  }

  void loop() {
    FrameBuffer &fb = display_.fb();
    if (fb.pixels() == nullptr) {
      delay(1000);
      return;
    }
    pollSerial();
    handleButton(external_);
    handleButton(boot_);
    const uint32_t readsBefore = snap_.reads;
    reader_.update(snap_);
    showLed(snap_.reads != readsBefore);

    const float t = millis() / 1000.0f;
    const LabView &v = spec_.views[view_];
    fb.fillScreen(pal::night);
    switch (v.view) {
      case View::Chip: rv::chip(fb, snap_, t); break;
      case View::Card: rv::card(fb, snap_, t); break;
      case View::Trace: rv::trace(fb, reader_.trace(), traceOn_); break;
      case View::Memory: rv::memory(fb, snap_, t); break;
      case View::Credits: rv::credits(fb, snap_, t); break;
      case View::Edge: rv::edgeChart(fb, snap_); break;
    }
    char hint[48];
    if (v.action) snprintf(hint, sizeof(hint), "double-click: %s", v.action);
    else snprintf(hint, sizeof(hint), "click: next view");
    rv::header(fb, viewName(v.view), view_, spec_.viewCount, snap_);
    rv::footer(fb, hint, snap_);
    display_.push();
  }

  // ---- For the command handlers (lab_commands.h) ----------------------------------------------

  void submit(Reader::Job job, int32_t a = 0, const char *text = "") {
    Reader::Request r{job, a};
    strlcpy(r.text, text, sizeof(r.text));
    if (!reader_.submit(r)) log_w("the reader is busy with 4 jobs already, try again in a moment");
  }

  // log_w is a macro that can't be returned from a void function; this can.
  [[gnu::format(printf, 2, 3)]] void warn(const char *fmt, ...) {
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    log_w("%s", text);
  }

  // Switch to a view if this lab has it (dump shows the memory view, and so on).
  void showView(View v) {
    for (int i = 0; i < spec_.viewCount; i++)
      if (spec_.views[i].view == v) return setView(i);
  }

  bool traceOn() const { return traceOn_; }
  void setTraceOn(bool on) { traceOn_ = on; }
  const Snapshot &snap() const { return snap_; }
  int takeNextEdge() {
    const int e = nextEdge_;
    nextEdge_ = (nextEdge_ + 1) % 7;
    return e;
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
    for (int i = 0; i < spec_.commandCount; i++)
      if (cmd == spec_.commands[i].name) return spec_.commands[i].run(*this, arg, lower);
    log_w("unknown command '%s' in %s (type help)", line.c_str(), spec_.env);
  }

 private:
  void printHelp() {
    Serial.printf("\nRFID lab %s  (%s)\n", spec_.title, spec_.env);
    for (int i = 0; i < spec_.commandCount; i++) {
      const LabCommand &c = spec_.commands[i];
      char usage[40];
      snprintf(usage, sizeof(usage), "%s %s", c.name, c.args);
      Serial.printf("  %-28s %s\n", usage, c.what);
    }
    Serial.printf("  %-28s %s\n", "view <n>", "show screen view n");
    Serial.print("Views:");
    for (int i = 0; i < spec_.viewCount; i++)
      Serial.printf(" %d %s%s", i + 1, viewName(spec_.views[i].view), i + 1 < spec_.viewCount ? " ·" : "\n");
    Serial.println("Button (GPIO 5, optional) or BOOT: click = next view, double-click = the view's action, long = pause polling");
  }

  void setView(int v) {
    view_ = (v % spec_.viewCount + spec_.viewCount) % spec_.viewCount;
    log_i("view %d: %s", view_ + 1, viewName(spec_.views[view_].view));
  }

  void pollSerial() {
    static String buffer;
    while (Serial.available()) {
      const char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (!buffer.isEmpty()) runLine(buffer);
        buffer = "";
      } else if (c == 0x08 || c == 0x7F) {
        // Backspace: the monitor sends it as a character; without this, "chiq<BS>p" would arrive as is.
        if (!buffer.isEmpty()) buffer.remove(buffer.length() - 1);
      } else if (uint8_t(c) < 0x20) {
        // Other control characters (arrow keys send escape sequences): ignore.
      } else if (buffer.length() < 96) {
        buffer += c;
      }
    }
  }

  void handleButton(Button &b) {
    switch (b.poll()) {
      case Button::Event::Click: setView(view_ + 1); break;
      case Button::Event::DoubleClick:
        if (spec_.views[view_].action) {
          Serial.printf("> %s   (double-click)\n", spec_.views[view_].action);
          runLine(spec_.views[view_].action);
        }
        break;
      case Button::Event::LongPress: submit(Reader::Job::Poll, !snap_.polling); break;
      case Button::Event::None: break;
    }
  }

  // The on-board LED says what the reader sees. Sent only when the colour changes: each write is a
  // whole WS2812 frame on GPIO 48.
  void showLed(bool justRead) {
    if (justRead) flashUntil_ = millis() + 300;
    // version 0 = the reader task hasn't reported yet (the first ~50 ms after power-on): not "silent".
    const bool chipSilent = snap_.version != 0 && (snap_.chipVersion == 0x00 || snap_.chipVersion == 0xFF);
    uint8_t r = 0, g = 0, b = 0;
    if (chipSilent) r = 90;                                    // red: the reader chip isn't answering
    else if (millis() < flashUntil_) r = 40, g = 220, b = 70;  // bright green flash: a card was just read
    else if (snap_.present) g = 90, b = 25;                    // steady green: a card is on the reader
    const uint32_t colour = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    if (colour == ledShown_) return;
    ledShown_ = colour;
    rgbLedWrite(PIN_RGB, r, g, b);
  }

  const LabSpec &spec_;
  Display display_;
  Reader reader_;
  Button external_{PIN_BUTTON, "button"};
  Button boot_{PIN_BOOT_BTN, "BOOT"};
  Snapshot snap_;
  int view_;
  bool traceOn_ = true;
  int nextEdge_ = 0;
  uint32_t flashUntil_ = 0, ledShown_ = 0xFFFFFFFF;
};
