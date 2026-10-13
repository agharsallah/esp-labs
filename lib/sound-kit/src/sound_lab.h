// The shell every sound lab (S1-S6) shares: the microphone and its sampler task, the level meter,
// the screen with its header and footer, the push button and BOOT, the on-board RGB LED, and the
// serial commands every lab has (help, view, status, rate, atten, quiet, plot).
//
// A lab is a SoundLabSpec: its views (each a draw function, plus what a double-click does there),
// its serial commands, and hooks: setup() once the mic runs, frame() for every 4 ms frame of sound
// (with the level already updated), tick() once per screen frame, gesture() for the button.
// Each src/sound-lab-sN/main.cpp is that list.
//
// Button or BOOT: click = next view · double-click = this view's action · long press = the lab's
// long action. On-board LED, unless the lab claims it: the level as a colour (green, amber, red),
// dim while quiet.
#pragma once

#include <Arduino.h>

#include "button.h"
#include "display.h"
#include "level.h"
#include "mic.h"
#include "pins.h"
#include "sound_views.h"

class SoundLab;

enum class Gesture : uint8_t { Click, DoubleClick, LongPress };

struct SoundCommand {
  const char *name;  // the first word you type
  const char *args;  // for help, e.g. "<dB>"
  const char *what;  // one line for help
  void (*run)(SoundLab &lab, const String &arg, const String &lower);
};

struct SoundView {
  const char *name;  // "scope"
  void (*draw)(SoundLab &lab, FrameBuffer &fb, float t);
  const char *action;  // the command line a double-click runs on this view, or nullptr
};

struct SoundLabSpec {
  const char *env;    // "sound-lab-s1"
  const char *id;     // "S1"
  const char *title;  // "Hear"
  const SoundView *views;
  int viewCount;
  int startView;
  const SoundCommand *commands;
  int commandCount;
  void (*setup)(SoundLab &lab) = nullptr;
  void (*frame)(SoundLab &lab, const snd::Frame &f, uint32_t ms) = nullptr;  // every 4 ms of sound
  void (*tick)(SoundLab &lab) = nullptr;                                    // every screen frame
  bool (*gesture)(SoundLab &lab, Gesture g, bool external) = nullptr;       // true = handled
  const char *longAction = nullptr;
};

class SoundLab {
 public:
  explicit SoundLab(const SoundLabSpec &spec) : spec_(spec), view_(spec.startView) {}

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
    if (!mic_.begin()) {
      log_e("The microphone sampler did not start. Stopping.");
      return;
    }
    if (spec_.setup) spec_.setup(*this);
    printHelp();
    note(sv::Note::Info, "learning the quiet...");
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
    takeFrames();
    if (spec_.tick) spec_.tick(*this);
    showLed();

    const float t = millis() / 1000.0f;
    const SoundView &v = spec_.views[view_];
    fb.fillScreen(pal::night);
    v.draw(*this, fb, t);
    sv::header(fb, spec_.id, v.name, view_, spec_.viewCount, level_.reading(), mic_.doHigh(), recording_, t);
    char hint[40];
    if (v.action) snprintf(hint, sizeof(hint), "2x: %s", v.action);
    else snprintf(hint, sizeof(hint), "click: next view");
    sv::footer(fb, hint, note_, noteKind_, millis() - noteAt_);
    display_.push();
  }

  // ---- For views and commands ------------------------------------------------------------------

  snd::Mic &mic() { return mic_; }
  const snd::LevelReading &level() const { return level_.reading(); }
  snd::Level &meter() { return level_; }
  Display &display() { return display_; }
  const SoundLabSpec &spec() const { return spec_; }
  int view() const { return view_; }
  // The sound clock: milliseconds of sound heard so far, advanced 64 samples' worth per frame. Every
  // detector runs on it (not millis()), so a slow screen frame never bends their timing.
  uint32_t soundMs() const { return uint32_t(soundMs_); }

  void setView(int v) {
    view_ = (v % spec_.viewCount + spec_.viewCount) % spec_.viewCount;
    log_i("view %d: %s", view_ + 1, spec_.views[view_].name);
  }
  void showView(const char *name) {
    for (int i = 0; i < spec_.viewCount; i++)
      if (strcmp(spec_.views[i].name, name) == 0) return setView(i);
  }

  // The lab drives the on-board LED itself; false hands it back to the level colour.
  void claimLed(bool claimed) {
    ledClaimed_ = claimed;
    ledShown_ = 0xFFFFFFFF;
  }
  // The REC dot in the header. The recorder sets it; nothing can record without it showing.
  void setRecording(bool on) { recording_ = on; }

  // A short line for the footer, also printed to serial.
  [[gnu::format(printf, 3, 4)]] void note(sv::Note kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(note_, sizeof(note_), fmt, ap);
    va_end(ap);
    noteKind_ = kind;
    noteAt_ = millis();
    Serial.printf("  %s %s\n", sv::noteMark(kind), note_);
  }

  // log_w is a macro that can't be returned from a void function; this can.
  [[gnu::format(printf, 2, 3)]] void warn(const char *fmt, ...) {
    char text[140];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    log_w("%s", text);
  }

  static bool number(const String &arg, long lo, long hi, long &out) {
    if (arg.isEmpty()) return false;
    for (size_t i = 0; i < arg.length(); i++)
      if (!isDigit(arg[i]) && !(i == 0 && arg[i] == '-')) return false;
    out = arg.toInt();
    return out >= lo && out <= hi;
  }
  static bool decimal(const String &arg, float lo, float hi, float &out) {
    if (arg.isEmpty()) return false;
    char *end = nullptr;
    out = strtof(arg.c_str(), &end);
    return end && *end == 0 && out >= lo && out <= hi;
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
    // The lab's own commands first, so a lab can give a shared word its own meaning.
    for (int i = 0; i < spec_.commandCount; i++)
      if (cmd == spec_.commands[i].name) return spec_.commands[i].run(*this, arg, lower);
    if (cmd == "help" || cmd == "?") return printHelp();
    if (cmd == "view") {
      long n = 0;
      if (!number(arg, 1, spec_.viewCount, n)) return warn("view <1-%d>", spec_.viewCount);
      return setView(n - 1);
    }
    if (cmd == "status") return printStatus();
    if (cmd == "rate") return cmdRate(arg);
    if (cmd == "atten") return cmdAtten(lower);
    if (cmd == "quiet") {
      level_.relearn();
      return note(sv::Note::Info, "shh... learning the quiet");
    }
    if (cmd == "plot") {
      plot_ = lower != "off";
      return note(sv::Note::Info, plot_ ? "plot on: one bar per 50 ms" : "plot off");
    }
    log_w("unknown command '%s' in %s (type help)", line.c_str(), spec_.env);
  }

 private:
  void printHelp() {
    Serial.printf("\nSound lab %s %s  (%s)  ·  KY-037: AO on GPIO %d, DO on GPIO %d\n", spec_.id, spec_.title, spec_.env, PIN_MIC_AO,
                  PIN_MIC_DO);
    for (int i = 0; i < spec_.commandCount; i++) {
      const SoundCommand &c = spec_.commands[i];
      char usage[40];
      snprintf(usage, sizeof(usage), "%s %s", c.name, c.args);
      Serial.printf("  %-28s %s\n", usage, c.what);
    }
    Serial.printf("  %-28s %s\n", "quiet", "re-learn the quiet of your room (half a second: shh)");
    Serial.printf("  %-28s %s\n", "plot [off]", "print the level as a bar every 50 ms");
    Serial.printf("  %-28s %s\n", "status", "the sampler, the ADC and the level, in numbers");
    Serial.printf("  %-28s %s\n", "rate <Hz>", "sample rate, 2000-48000 (default 16000)");
    Serial.printf("  %-28s %s\n", "atten <0|2.5|6|12>", "the ADC's input range (default 12 dB = 0-3.1 V)");
    Serial.printf("  %-28s %s\n", "view <n>", "show screen view n");
    Serial.print("Views:");
    for (int i = 0; i < spec_.viewCount; i++)
      Serial.printf(" %d %s%s", i + 1, spec_.views[i].name, i + 1 < spec_.viewCount ? " ·" : "\n");
    Serial.println("Button (GPIO 5, optional) or BOOT: click = next view, double-click = the view's action");
  }

  void printStatus() {
    const snd::LevelReading &r = level_.reading();
    Serial.printf("sampler: %lu Hz asked, %lu Hz measured · frames dropped %lu · DMA overruns %lu\n",
                  static_cast<unsigned long>(mic_.rate()), static_cast<unsigned long>(mic_.measuredRate()),
                  static_cast<unsigned long>(mic_.framesDropped()), static_cast<unsigned long>(mic_.overruns()));
    Serial.printf("ADC: attenuation %s · bias %d mV (raw %d) · last 100 ms raw %d..%d%s · %s\n", snd::attenName(mic_.atten()), mic_.biasMv(),
                  int(mic_.biasRaw()), mic_.rawLow(), mic_.rawHigh(), mic_.rawLow() <= 0 || mic_.rawHigh() >= 4095 ? " CLIPPING" : "",
                  mic_.calibrated() ? "eFuse calibration" : "no calibration (straight-line mV)");
    Serial.printf("level: %.1f dBFS fast, %.1f slow, floor %.1f dBFS · %.1f dB above the quiet · peak %.1f dBFS\n", r.fastDb, r.slowDb,
                  r.floorDb, r.above(), r.peakDb);
    Serial.printf("comparator DO: %s now, %lu rising edges\n", mic_.doHigh() ? "HIGH" : "low", static_cast<unsigned long>(mic_.doRises()));
  }

  void cmdRate(const String &arg) {
    long hz = 0;
    if (!number(arg, 2000, 48000, hz)) return warn("rate <Hz>, 2000-48000. Now %lu", static_cast<unsigned long>(mic_.rate()));
    mic_.reconfigure(uint32_t(hz), mic_.atten());
    level_ = snd::Level(snd::kChunk * 1000.0f / float(hz));  // a frame is 64 samples: its length changes
    note(sv::Note::Info, "rate %ld Hz", hz);
  }

  void cmdAtten(const String &lower) {
    adc_atten_t a;
    if (lower == "0") a = ADC_ATTEN_DB_0;
    else if (lower == "2.5") a = ADC_ATTEN_DB_2_5;
    else if (lower == "6") a = ADC_ATTEN_DB_6;
    else if (lower == "12" || lower == "11") a = ADC_ATTEN_DB_12;
    else return warn("atten 0 | 2.5 | 6 | 12   (now %s)", snd::attenName(mic_.atten()));
    mic_.reconfigure(mic_.rate(), a);
    level_.relearn();
    note(sv::Note::Info, "attenuation %s", snd::attenName(a));
  }

  // All frames the sampler has queued since the last screen frame: level first, then the lab.
  void takeFrames() {
    snd::Frame f;
    while (mic_.frame(f)) {
      soundMs_ += snd::kChunk * 1000.0 / mic_.rate();
      const uint32_t ms = uint32_t(soundMs_);
      level_.push(f.rms, ms);
      if (spec_.frame) spec_.frame(*this, f, ms);
      if (plot_ && ms - plotAt_ >= 50) plotAt_ = ms, printPlot();
    }
  }

  void printPlot() {
    const snd::LevelReading &r = level_.reading();
    char bar[41];
    const int n = int(snd::meterFraction(r.instAbove(), 60) * 40);
    for (int i = 0; i < 40; i++) bar[i] = i < n ? '#' : (i == int(snd::meterFraction(r.peakDb - r.floorDb, 60) * 40) ? '|' : ' ');
    bar[40] = 0;
    Serial.printf("%5.1f dB |%s| DO %c\n", r.instAbove(), bar, mic_.doHigh() ? '1' : '0');
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
      } else if (uint8_t(c) >= 0x20 && buffer.length() < 120) {
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

  void showLed() {
    if (ledClaimed_) return;
    uint8_t r = 0, g = 0, b = 0;
    if (recording_) r = 90;  // the recorder always shows itself, here too
    else {
      // 8 brightness steps, so the LED (a whole WS2812 frame per write) only changes when it should.
      const float above = level_.reading().above();
      const uint8_t v = uint8_t(4 + int(snd::meterFraction(above, 50) * 7) * 12);
      if (above < 25) g = v;
      else if (above < 40) r = v, g = v / 2;
      else r = v;
    }
    const uint32_t colour = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    if (colour == ledShown_) return;
    ledShown_ = colour;
    rgbLedWrite(PIN_RGB, r, g, b);
  }

  const SoundLabSpec &spec_;
  Display display_;
  Button external_{PIN_BUTTON, "button"};
  Button boot_{PIN_BOOT_BTN, "BOOT"};
  snd::Mic mic_;
  snd::Level level_;
  int view_;
  bool ledClaimed_ = false, recording_ = false, plot_ = false;
  uint32_t ledShown_ = 0xFFFFFFFF, plotAt_ = 0;
  double soundMs_ = 0;
  char note_[64] = "";
  sv::Note noteKind_ = sv::Note::Info;
  uint32_t noteAt_ = 0;
};
