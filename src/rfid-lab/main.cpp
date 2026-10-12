// RFID lab (R1-R6).  uv run pio run -e rfid-lab -t upload
// An RC522 reader (NXP MFRC522) on its own SPI bus, driven by our own driver in lib/rfid-kit so
// every byte is visible: chip registers, the ISO 14443-A card protocol, MIFARE Classic memory,
// and an edge lab that pushes each setting until it breaks.
//
// Wiring: the screen from lesson 03, plus the RC522 (3.3V only):
//   SDA->15  SCK->16  MOSI->17  MISO->18  IRQ->13 (optional)  GND->GND  RST->14  3.3V->3V3
// The push button on GPIO 5 is optional: it does exactly what BOOT does.
//
// On-board RGB LED: bright green flash = a card was just read · steady green = a card is on the
// reader · red = the reader chip isn't answering (wiring) · off = no card.
//
// Views: chip · card · trace · memory · credits · edge
// Button or BOOT: click = next view · double-click = this view's action · long press = pause polling
// Serial: help (lists everything)
#include <Arduino.h>

#include "button.h"
#include "color.h"
#include "display.h"
#include "pins.h"
#include "reader.h"
#include "rfid_views.h"

namespace {

const char *const kViewNames[] = {"chip", "card", "trace", "memory", "credits", "edge"};
constexpr int kViews = sizeof(kViewNames) / sizeof(kViewNames[0]);
// What a double-click does on each view.
const char *const kHints[] = {"click: view  double: self-test", "click: view  double: inventory",
                              "click: view  double: sniffer on/off", "click: view  double: dump",
                              "click: view  double: +1 credit", "click: view  double: next test"};

Display display;
Reader reader;
Button external(PIN_BUTTON, "button");
Button boot(PIN_BOOT_BTN, "BOOT");

struct {
  int view = 1;
  Snapshot snap;
  bool traceOn = true;
  int nextEdge = 0;
} state;

using J = Reader::Job;

// log_w is a macro that can't be returned from a void function; this can.
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char *fmt, ...) {
  char text[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  log_w("%s", text);
}

void submit(J job, int32_t a = 0, const char *text = "") {
  Reader::Request r{job, a};
  strlcpy(r.text, text, sizeof(r.text));
  if (!reader.submit(r)) log_w("the reader is busy with 4 jobs already, try again in a moment");
}

void setView(int v) {
  state.view = (v % kViews + kViews) % kViews;
  log_i("view %d: %s", state.view + 1, kViewNames[state.view]);
}

void viewAction() {
  static const J edges[] = {J::EdgeSpi, J::EdgeGain, J::EdgeTimeout, J::EdgeTiming, J::EdgeIrq, J::EdgeCrc, J::EdgeStress};
  switch (state.view) {
    case 0: return submit(J::SelfTest);
    case 1: return submit(J::Inventory);
    case 2:
      state.traceOn = !state.traceOn;
      return submit(J::Trace, state.traceOn);
    case 3: return submit(J::Dump);
    case 4: return submit(J::CreditsAdd, 1);
    default:
      submit(edges[state.nextEdge]);
      state.nextEdge = (state.nextEdge + 1) % 7;
  }
}

void printHelp() {
  Serial.println();
  Serial.println("R1 chip    chip | reset | selftest | loopback | regs | spi <MHz> | gain <0-7> | irq on|off | timeout <us>");
  Serial.println("R2 wake    poll on|off | trace on|off   (the card view shows what's in the field)");
  Serial.println("R3 who     inventory   (lay the card and the tag on the reader together)");
  Serial.println("R4 memory  dump | read <block>");
  Serial.println("R5 write   write <block> <text, 16 chars> | credits | credits init <n> | credits add <n> | credits use <n>");
  Serial.println("R6 edge    edge spi | edge gain | edge timeout | edge timing | edge irq | edge crc | edge stress [n]");
  Serial.println("           view <1-6> | help");
  Serial.println("Button (GPIO 5, optional) or BOOT: click = next view, double-click = this view's action, long = pause polling");
}

// Parse "<cmd> <number>", false when the number is missing or out of range.
bool number(const String &arg, long lo, long hi, long &out) {
  if (arg.isEmpty()) return false;
  for (size_t i = 0; i < arg.length(); i++)
    if (!isDigit(arg[i]) && !(i == 0 && arg[i] == '-')) return false;
  out = arg.toInt();
  return out >= lo && out <= hi;
}

void handleCommand(const String &raw) {
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
  long n = 0;

  if (cmd == "help" || cmd == "?") return printHelp();
  if (cmd == "chip") return submit(J::Chip);
  if (cmd == "reset") return submit(J::Reset);
  if (cmd == "selftest") return submit(J::SelfTest);
  if (cmd == "loopback") return submit(J::Loopback);
  if (cmd == "regs") return submit(J::Regs);
  if (cmd == "spi") {
    if (!number(arg, 1, 40, n)) return warn("spi <MHz>, 1-40 (the chip is rated for 10)");
    return submit(J::Spi, n);
  }
  if (cmd == "gain") {
    if (!number(arg, 0, 7, n)) return warn("gain <0-7>: 0=18 1=23 4=33 5=38 6=43 7=48 dB");
    return submit(J::Gain, n);
  }
  if (cmd == "timeout") {
    if (!number(arg, 25, 1'000'000, n)) return warn("timeout <us>, 25-1000000 (default 25000)");
    return submit(J::Timeout, n);
  }
  if (cmd == "irq" || cmd == "poll" || cmd == "trace") {
    if (lower != "on" && lower != "off") return warn("%s on | %s off", cmd.c_str(), cmd.c_str());
    const bool on = lower == "on";
    if (cmd == "trace") state.traceOn = on;
    return submit(cmd == "irq" ? J::Irq : cmd == "poll" ? J::Poll : J::Trace, on);
  }
  if (cmd == "inventory") return submit(J::Inventory);
  if (cmd == "dump") {
    setView(3);
    return submit(J::Dump);
  }
  if (cmd == "read") {
    if (!number(arg, 0, 63, n)) return warn("read <block>, 0-63");
    return submit(J::Read, n);
  }
  if (cmd == "write") {
    const int s2 = arg.indexOf(' ');
    if (s2 < 0 || !number(arg.substring(0, s2), 1, 63, n)) return warn("write <block> <text>, e.g. write 4 hello bench");
    return submit(J::Write, n, arg.substring(s2 + 1).c_str());
  }
  if (cmd == "credits") {
    setView(4);
    if (arg.isEmpty()) return submit(J::Credits);
    const int s2 = lower.indexOf(' ');
    const String op = s2 < 0 ? lower : lower.substring(0, s2);
    if (s2 < 0 || !number(lower.substring(s2 + 1), 0, 100000, n)) return warn("credits init|add|use <n>, 0-100000");
    if (op == "init") return submit(J::CreditsInit, n);
    if (op == "add") return submit(J::CreditsAdd, n);
    if (op == "use") return submit(J::CreditsUse, n);
    return warn("credits init|add|use <n>");
  }
  if (cmd == "edge") {
    setView(5);
    const int s2 = lower.indexOf(' ');
    const String what = s2 < 0 ? lower : lower.substring(0, s2);
    if (what == "spi") return submit(J::EdgeSpi);
    if (what == "gain") return submit(J::EdgeGain);
    if (what == "timeout") return submit(J::EdgeTimeout);
    if (what == "timing") return submit(J::EdgeTiming);
    if (what == "irq") return submit(J::EdgeIrq);
    if (what == "crc") return submit(J::EdgeCrc);
    if (what == "stress") {
      if (s2 >= 0 && !number(lower.substring(s2 + 1), 10, 5000, n)) return warn("edge stress [10-5000]");
      return submit(J::EdgeStress, s2 < 0 ? 200 : n);
    }
    return warn("edge spi | gain | timeout | timing | irq | crc | stress [n]");
  }
  if (cmd == "view") {
    if (!number(arg, 1, kViews, n)) return warn("view <1-%d>", kViews);
    return setView(n - 1);
  }
  log_w("unknown command '%s' (type help)", line.c_str());
}

void pollSerial() {
  static String buffer;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!buffer.isEmpty()) handleCommand(buffer);
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

// The on-board LED says what the reader sees. Sent only when the colour changes: each write is a
// whole WS2812 frame on GPIO 48.
void showLed(bool justRead) {
  static uint32_t flashUntil = 0, shown = 0xFFFFFFFF;
  if (justRead) flashUntil = millis() + 300;
  // version 0 = the reader task hasn't reported yet (the first ~50 ms after power-on): not "silent".
  const bool chipSilent = state.snap.version != 0 && (state.snap.chipVersion == 0x00 || state.snap.chipVersion == 0xFF);
  uint8_t r = 0, g = 0, b = 0;
  if (chipSilent) r = 90;                            // red: the reader chip isn't answering
  else if (millis() < flashUntil) r = 40, g = 220, b = 70;  // bright green flash: a card was just read
  else if (state.snap.present) g = 90, b = 25;       // steady green: a card is on the reader
  const uint32_t colour = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
  if (colour == shown) return;
  shown = colour;
  rgbLedWrite(PIN_RGB, r, g, b);
}

void handleButton(Button &b) {
  switch (b.poll()) {
    case Button::Event::Click: setView(state.view + 1); break;
    case Button::Event::DoubleClick: viewAction(); break;
    case Button::Event::LongPress:
      submit(J::Poll, !state.snap.polling);
      break;
    case Button::Event::None: break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  if (!display.begin()) {
    log_e("No memory for the frame buffer. Stopping.");
    return;
  }
  rgbLedWrite(PIN_RGB, 0, 0, 0);
  external.begin();
  boot.begin();
  reader.begin();
  log_i("rfid-lab: reader task started on core 0, screen on core %d", xPortGetCoreID());
  printHelp();
}

void loop() {
  FrameBuffer &fb = display.fb();
  if (fb.pixels() == nullptr) {
    delay(1000);
    return;
  }
  pollSerial();
  handleButton(external);
  handleButton(boot);
  const uint32_t readsBefore = state.snap.reads;
  reader.update(state.snap);
  showLed(state.snap.reads != readsBefore);

  const float t = millis() / 1000.0f;
  fb.fillScreen(pal::night);
  switch (state.view) {
    case 0: rv::chip(fb, state.snap, t); break;
    case 1: rv::card(fb, state.snap, t); break;
    case 2: rv::trace(fb, reader.trace(), state.traceOn); break;
    case 3: rv::memory(fb, state.snap, t); break;
    case 4: rv::credits(fb, state.snap, t); break;
    default: rv::edgeChart(fb, state.snap); break;
  }
  rv::header(fb, kViewNames[state.view], state.view, kViews, state.snap);
  rv::footer(fb, kHints[state.view], state.snap);
  display.push();
}
