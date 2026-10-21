// The shell every sound lab (S1-S8) shares: the microphone and its sampler task, the speaker, the
// level meter, the screen with its header and footer, the push button and BOOT, the on-board RGB
// LED, and the serial commands every lab has (help, view, status, mic, vol, beep, tune, rate,
// atten, quiet, plot).
//
// Which microphone? `mic auto` (the default) listens for an INMP441 on the I2S pins at start-up and
// uses it when it answers, the KY-037 otherwise. `mic ky037` / `mic inmp441` force one (saved in
// NVS, then a restart). The speaker (MAX98357A) is always started: with nothing wired it is silent.
//
// Echo guard: while the speaker plays (and 200 ms after), the lab's frame() hook gets no frames, so
// a beep is never taken for a clap. A lab that wants to hear through it sets hearSpeaker.
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
#include <Preferences.h>

#include "level.h"
#include "mic.h"
#include "pins.h"
#include "sound_views.h"
#include "speaker.h"

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
  bool hearSpeaker = false;  // true: frame() keeps running while the speaker plays (S8's effects)
};

// Little sounds every lab can make (only when a speaker is wired, of course).
enum class Chime : uint8_t { Click, Ok, No, Start, Stop, Score, Hello };

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
    Preferences prefs;
    prefs.begin("sound", true);
    const String choice = prefs.isKey("mic") ? prefs.getString("mic") : String("auto");
    const int volume = prefs.getInt("vol", 40);
    prefs.end();
    snd::Source source = snd::Source::Ky037;
    if (choice == "inmp441") source = snd::Source::Inmp441;
    else if (choice == "auto") {
      Serial.println("Listening for an INMP441 on the I2S pins...");
      if (snd::Mic::probeInmp441()) source = snd::Source::Inmp441;
    }
    Serial.printf("Microphone: %s (mic setting: %s)\n", snd::sourceName(source), choice.c_str());
    speakerOk_ = speaker_.begin();
    speaker_.setVolume(volume);
    if (!mic_.begin(source)) {
      log_e("The microphone sampler did not start. Stopping.");
      return;
    }
    if (spec_.setup) spec_.setup(*this);
    printHelp();
    chime(Chime::Hello);
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
    sv::header(fb, spec_.id, v.name, view_, spec_.viewCount, level_.reading(), mic_.doHigh(), recording_, t, mic_.digital(),
               speaker_.busy());
    char hint[40];
    if (v.action) snprintf(hint, sizeof(hint), "2x: %s", v.action);
    else snprintf(hint, sizeof(hint), "click: next view");
    sv::footer(fb, hint, note_, noteKind_, millis() - noteAt_);
    display_.push();
  }

  // ---- For views and commands ------------------------------------------------------------------

  snd::Mic &mic() { return mic_; }
  snd::Speaker &speaker() { return speaker_; }
  // The level in dB SPL (INMP441 only; NAN with the KY-037, which has no known sensitivity).
  float spl() const { return mic_.splOf(level_.reading().fastDb); }
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

  void chime(Chime c) {
    if (!speakerOk_) return;
    switch (c) {
      case Chime::Click: speaker_.tone(2400, 12, syn::Wave::Square, 0.4f); break;
      case Chime::Ok: speaker_.tune("E6/16 B6/8", 240, syn::Wave::Sine); break;
      case Chime::No: speaker_.tune("C4/16 r/32 C4/8", 200, syn::Wave::Square); break;
      case Chime::Start: speaker_.tune("G5/16 C6/16", 260, syn::Wave::Triangle); break;
      case Chime::Stop: speaker_.tune("C6/16 G5/16", 260, syn::Wave::Triangle); break;
      case Chime::Score: speaker_.tune("C6/32 E6/32 G6/32 C7/16", 240, syn::Wave::Square); break;
      case Chime::Hello: speaker_.tune("C5/16 E5/16 G5/8", 220, syn::Wave::Triangle); break;
    }
  }

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
    if (cmd == "mic") return cmdMic(lower);
    if (cmd == "vol") return cmdVol(arg);
    if (cmd == "beep") return cmdBeep(arg, lower);
    if (cmd == "tune") return cmdTune(lower);
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
    Serial.printf("\nSound lab %s %s  (%s)  ·  microphone: %s\n", spec_.id, spec_.title, spec_.env, snd::sourceName(mic_.source()));
    for (int i = 0; i < spec_.commandCount; i++) {
      const SoundCommand &c = spec_.commands[i];
      char usage[40];
      snprintf(usage, sizeof(usage), "%s %s", c.name, c.args);
      Serial.printf("  %-28s %s\n", usage, c.what);
    }
    Serial.printf("  %-28s %s\n", "quiet", "re-learn the quiet of your room (half a second: shh)");
    Serial.printf("  %-28s %s\n", "plot [off]", "print the level as a bar every 50 ms");
    Serial.printf("  %-28s %s\n", "status", "the sampler, the microphone and the level, in numbers");
    Serial.printf("  %-28s %s\n", "mic [auto|ky037|inmp441]", "which microphone (saved; restarts)");
    Serial.printf("  %-28s %s\n", "vol <0-100>", "speaker volume (saved, default 40)");
    Serial.printf("  %-28s %s\n", "beep [Hz] [wave]", "a test tone on the speaker (sine square triangle saw noise)");
    Serial.printf("  %-28s %s\n", "tune <name>", "play a tune: hello scale twinkle ode alarm");
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
    if (mic_.digital())
      Serial.printf("INMP441: 24-bit, offset %d · last 100 ms %d..%d of +-%d%s · dB SPL = dBFS + %.0f\n", int(mic_.biasRaw()), mic_.rawLow(),
                    mic_.rawHigh(), mic_.rawMax(), mic_.clipping() ? " CLIPPING" : "", snd::Mic::kInmp441Spl);
    else
      Serial.printf("KY-037 on the ADC: attenuation %s · bias %d mV (raw %d) · last 100 ms raw %d..%d%s · %s\n", snd::attenName(mic_.atten()),
                    mic_.biasMv(), int(mic_.biasRaw()), mic_.rawLow(), mic_.rawHigh(), mic_.clipping() ? " CLIPPING" : "",
                    mic_.calibrated() ? "eFuse calibration" : "no calibration (straight-line mV)");
    Serial.printf("level: %.1f dBFS fast, %.1f slow, floor %.1f dBFS · %.1f dB above the quiet · peak %.1f dBFS\n", r.fastDb, r.slowDb,
                  r.floorDb, r.above(), r.peakDb);
    if (mic_.digital()) Serial.printf("sound level: about %.0f dB SPL (unweighted)\n", spl());
    else Serial.printf("comparator DO: %s now, %lu rising edges\n", mic_.doHigh() ? "HIGH" : "low", static_cast<unsigned long>(mic_.doRises()));
    Serial.printf("speaker: volume %d, %s\n", speaker_.volume(), speaker_.busy() ? "playing" : "quiet");
  }

  void cmdRate(const String &arg) {
    long hz = 0;
    if (!number(arg, 2000, 48000, hz)) return warn("rate <Hz>, 2000-48000. Now %lu", static_cast<unsigned long>(mic_.rate()));
    mic_.reconfigure(uint32_t(hz), mic_.atten());
    level_ = snd::Level(snd::kChunk * 1000.0f / float(hz));  // a frame is 64 samples: its length changes
    note(sv::Note::Info, "rate %ld Hz", hz);
  }

  void cmdMic(const String &lower) {
    if (lower != "auto" && lower != "ky037" && lower != "inmp441")
      return warn("mic auto | mic ky037 | mic inmp441   (now: %s)", snd::sourceName(mic_.source()));
    Preferences prefs;
    prefs.begin("sound", false);
    prefs.putString("mic", lower);
    prefs.end();
    Serial.printf("mic %s saved. Restarting to switch...\n", lower.c_str());
    Serial.flush();
    delay(200);
    ESP.restart();
  }

  void cmdVol(const String &arg) {
    long v = 0;
    if (!number(arg, 0, 100, v)) return warn("vol <0-100>   (now %d)", speaker_.volume());
    speaker_.setVolume(int(v));
    Preferences prefs;
    prefs.begin("sound", false);
    prefs.putInt("vol", int(v));
    prefs.end();
    chime(Chime::Click);
    note(sv::Note::Info, "volume %ld", v);
  }

  void cmdBeep(const String &arg, const String &lower) {
    float hz = 880;
    syn::Wave wave = syn::Wave::Sine;
    const int sp = lower.indexOf(' ');
    if (!arg.isEmpty() && !decimal(sp < 0 ? arg : arg.substring(0, sp), 20, 8000, hz)) return warn("beep [Hz 20-8000] [sine|square|triangle|saw|noise]");
    if (sp > 0 && !syn::parseWave(lower.substring(sp + 1).c_str(), wave)) return warn("wave: sine square triangle saw noise");
    speaker_.tone(hz, 400, wave);
    note(sv::Note::Info, "beep %.0f Hz %s", hz, syn::waveName(wave));
  }

  void cmdTune(const String &lower) {
    static const struct {
      const char *name, *notes;
      int bpm;
    } kTunes[] = {
        {"hello", "C5/16 E5/16 G5/8", 220},
        {"scale", "C4/8 D4/8 E4/8 F4/8 G4/8 A4/8 B4/8 C5/4", 160},
        {"twinkle", "C5/4 C5/4 G5/4 G5/4 A5/4 A5/4 G5/2 F5/4 F5/4 E5/4 E5/4 D5/4 D5/4 C5/2", 160},
        {"ode", "E5/4 E5/4 F5/4 G5/4 G5/4 F5/4 E5/4 D5/4 C5/4 C5/4 D5/4 E5/4 E5/4. D5/8 D5/2", 150},
        {"alarm", "A5/16 E5/16 A5/16 E5/16 A5/16 E5/16 A5/16 E5/16", 200},
    };
    for (const auto &t : kTunes)
      if (lower == t.name) {
        speaker_.tune(t.notes, t.bpm);
        return note(sv::Note::Info, "playing %s", t.name);
      }
    warn("tune hello | scale | twinkle | ode | alarm");
  }

  void cmdAtten(const String &lower) {
    if (mic_.digital()) return warn("atten is for the KY-037's ADC: the INMP441 has its own converter inside");
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
      // The echo guard: the board's own sounds are not claps, knocks or voices.
      if (spec_.frame && (spec_.hearSpeaker || !speaker_.recentlyLoud(200))) spec_.frame(*this, f, ms);
      if (plot_ && ms - plotAt_ >= 50) plotAt_ = ms, printPlot();
    }
  }

  void printPlot() {
    const snd::LevelReading &r = level_.reading();
    char bar[41];
    const int n = int(snd::meterFraction(r.instAbove(), 60) * 40);
    for (int i = 0; i < 40; i++) bar[i] = i < n ? '#' : (i == int(snd::meterFraction(r.peakDb - r.floorDb, 60) * 40) ? '|' : ' ');
    bar[40] = 0;
    if (mic_.digital()) Serial.printf("%5.1f dB |%s| %3.0f dB SPL\n", r.instAbove(), bar, mic_.splOf(r.instDb));
    else Serial.printf("%5.1f dB |%s| DO %c\n", r.instAbove(), bar, mic_.doHigh() ? '1' : '0');
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
  snd::Speaker speaker_;
  snd::Level level_;
  bool speakerOk_ = false;
  int view_;
  bool ledClaimed_ = false, recording_ = false, plot_ = false;
  uint32_t ledShown_ = 0xFFFFFFFF, plotAt_ = 0;
  double soundMs_ = 0;
  char note_[64] = "";
  sv::Note noteKind_ = sv::Note::Info;
  uint32_t noteAt_ = 0;
};
