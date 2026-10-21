// Sound lab S3 Clap control.  uv run pio run -d labs/sound-lab-s3 -e sound-lab-s3 -t upload
// Switch things with your hands: one clap toggles the LED, two change the RGB colour, three put
// the screen to sleep, four throw a party. A voice, a whistle or music never counts: a clap is a
// bang that is over within a tenth of a second (kit/lib/sound-kit/src/onsets.h), and the claps that
// come close together form one pattern (Groups: a pause of 0.65 s ends it).
//
// Wiring: the screen and a microphone: the KY-037 (+ -> 3V3, G -> GND, AO -> GPIO 6, DO -> GPIO 21)
// or the INMP441 (VDD -> 3V3, GND -> GND, L/R -> GND, SCK -> 40, WS -> 41, SD -> 42); `mic auto`
// picks the one that answers. Optional: lesson 02's LED on GPIO 4 (through its resistor) for the
// `led` action; the MAX98357A + speaker (BCLK 39, LRC 38, DIN 47, VIN 5V) for the confirmation
// sounds and the `tune` action; the push button on GPIO 5 (it does what BOOT does).
//
// Sounds: every pattern answers with a little sound of its own, a pattern with nothing mapped
// with a low "nope". The microphone hears those too, of course: the shell's echo guard
// (kit/lib/sound-kit/src/sound_lab.h) stops listening while the speaker plays and 200 ms after, so
// the board never claps for itself. `sound off` keeps it quiet.
//
// Views: timeline · lamp · patterns · tune   (click = next, double-click = the action in the footer)
// Serial: help lists the commands. Shared code: kit/lib/sound-kit.
#include <Preferences.h>
#include <driver/gpio.h>

#include "clap_actions.h"
#include "onsets.h"
#include "sound_lab.h"

namespace {

using claps::Action;

// ---- State ------------------------------------------------------------------------------------

snd::Onsets ears;
snd::Groups groups;
claps::Map map;

// The timeline: every frame's level (dB above the quiet), the last 4 s, plus the events on it.
constexpr int kHistory = 1000;  // frames: 4 s at 4 ms
uint8_t history[kHistory];
int historyHead = 0;
uint32_t historyMs = 0;  // the sound clock at the newest frame

struct Mark {
  snd::Onsets::Kind kind;
  uint32_t ms, lengthMs;
};
constexpr int kMarks = 24;
Mark marks[kMarks];
int markHead = 0;

// The last finished pattern, for every view's label.
struct LastGroup {
  int count = 0;
  Action action = Action::None;
  uint32_t at = 0;  // millis()
} last;

// What the actions switch.
struct Outputs {
  bool led = false;
  int colour = -2;  // index into kColours; -1 = dark; -2 = the shell's level colour (not chosen yet)
  bool screenOff = false;
  uint32_t screenOffAt = 0;
  uint32_t partyUntil = 0;
} out;

struct Colour {
  const char *name;
  uint8_t r, g, b;
};
const Colour kColours[] = {{"warm white", 90, 60, 30}, {"red", 110, 0, 0},  {"orange", 110, 40, 0}, {"green", 0, 100, 10},
                           {"cyan", 0, 80, 90},        {"blue", 0, 20, 120}, {"violet", 70, 0, 110}};
constexpr int kColourCount = sizeof(kColours) / sizeof(kColours[0]);

// Learn: five claps, measured.
struct Learn {
  bool on = false;
  int n = 0;
  float strength[5] = {};
  uint32_t length[5] = {};
  int longs = 0;
  float suggested = 0;
} learn;

bool testing = false;  // print every detector event
bool sounds = true;    // confirmation sounds (NVS)

// The `tune` action's song, and each action's answer.
const char *const kTwinkle = "C5/4 C5/4 G5/4 G5/4 A5/4 A5/4 G5/2 F5/4 F5/4 E5/4 E5/4 D5/4 D5/4 C5/2";

void answer(SoundLab &lab, Action a, bool on) {
  if (!sounds) return;
  switch (a) {
    case Action::Led: lab.chime(on ? Chime::Start : Chime::Stop); break;  // up = on, down = off
    case Action::Colour: lab.speaker().tune("E6/32 G6/32 B6/16", 260, syn::Wave::Sine); break;
    case Action::Dark: lab.chime(Chime::Stop); break;
    case Action::Screen: lab.chime(Chime::Click); break;
    case Action::View: lab.chime(Chime::Click); break;
    case Action::Party: lab.chime(Chime::Score); break;
    case Action::Tune: lab.speaker().tune(kTwinkle, 160); break;
    default: lab.chime(Chime::No); break;
  }
}

// ---- Settings in NVS --------------------------------------------------------------------------

void save() {
  Preferences p;
  p.begin("claps", false);
  uint8_t a[claps::kPatterns];
  for (int i = 0; i < claps::kPatterns; i++) a[i] = uint8_t(map.action[i + 1]);
  p.putBytes("map", a, sizeof(a));
  p.putFloat("rise", ears.config.riseDb);
  p.putUInt("gap", groups.gapMs);
  p.putBool("sound", sounds);
  p.end();
}

void load() {
  Preferences p;
  p.begin("claps", true);
  uint8_t a[claps::kPatterns];
  if (p.isKey("map") && p.getBytes("map", a, sizeof(a)) == sizeof(a))
    for (int i = 0; i < claps::kPatterns; i++) map.action[i + 1] = a[i] < uint8_t(Action::Count) ? Action(a[i]) : Action::None;
  if (p.isKey("rise")) ears.config.riseDb = p.getFloat("rise", ears.config.riseDb);
  if (p.isKey("gap")) groups.gapMs = p.getUInt("gap", groups.gapMs);
  sounds = p.getBool("sound", true);
  p.end();
}

// ---- Actions ----------------------------------------------------------------------------------

void showColour(SoundLab &lab) {
  lab.claimLed(true);
  if (out.colour < 0) rgbLedWrite(PIN_RGB, 0, 0, 0);
  else rgbLedWrite(PIN_RGB, kColours[out.colour].r, kColours[out.colour].g, kColours[out.colour].b);
}

void setScreen(SoundLab &lab, bool off) {
  out.screenOff = off;
  out.screenOffAt = millis();
  lab.display().setBacklight(off ? 0 : 255);
}

void run(SoundLab &lab, Action a) {
  switch (a) {
    case Action::None: break;
    case Action::Led:
      out.led = !out.led;
      digitalWrite(PIN_LED, out.led ? HIGH : LOW);
      lab.note(sv::Note::Good, "LED %s", out.led ? "on" : "off");
      break;
    case Action::Colour:
      out.colour = out.colour < 0 ? 0 : (out.colour + 1) % kColourCount;
      showColour(lab);
      lab.note(sv::Note::Good, "colour: %s", kColours[out.colour].name);
      break;
    case Action::Dark:
      out.colour = -1;
      showColour(lab);
      lab.note(sv::Note::Good, "RGB off");
      break;
    case Action::Screen:
      setScreen(lab, !out.screenOff);
      lab.note(sv::Note::Good, out.screenOff ? "screen asleep (back in 30 s, or press a button)" : "screen on");
      break;
    case Action::View: lab.setView(lab.view() + 1); break;
    case Action::Party:
      out.partyUntil = millis() + 2000;
      lab.claimLed(true);
      lab.note(sv::Note::Good, "party!");
      break;
    case Action::Tune: lab.note(sv::Note::Good, "twinkle, twinkle (claps wait until it ends)"); break;
    case Action::Count: break;
  }
  answer(lab, a, out.led);
}

// ---- Detection (every 4 ms frame) -------------------------------------------------------------

void addMark(const snd::Onsets::Event &e) {
  marks[markHead] = {e.kind, e.ms, e.lengthMs};
  markHead = (markHead + 1) % kMarks;
}

void frame(SoundLab &lab, const snd::Frame &, uint32_t ms) {
  const float above = lab.level().instAbove();
  history[historyHead] = uint8_t(std::min(above, 255.0f));
  historyHead = (historyHead + 1) % kHistory;
  historyMs = ms;
  if (lab.meter().learning()) return;  // no claps while the quiet is being learnt

  const snd::Onsets::Event e = ears.push(above, ms);
  if (e.kind != snd::Onsets::Kind::None) {
    addMark(e);
    if (testing && e.kind != snd::Onsets::Kind::Onset)
      Serial.printf("  %s at %lu ms: %.1f dB, %lu ms long\n", e.kind == snd::Onsets::Kind::Clap ? "clap" : "LONG (not a clap)",
                    static_cast<unsigned long>(e.ms), e.strengthDb, static_cast<unsigned long>(e.lengthMs));
  }
  if (e.kind == snd::Onsets::Kind::Clap) {
    if (learn.on) {
      learn.strength[learn.n] = e.strengthDb;
      learn.length[learn.n] = e.lengthMs;
      if (++learn.n == 5) {
        learn.on = false;
        learn.suggested = claps::suggestRise(learn.strength, 5);
        ears.config.riseDb = learn.suggested;
        save();
        lab.note(sv::Note::Good, "learnt: sens %.0f dB (saved)", learn.suggested);
      } else {
        lab.note(sv::Note::Info, "clap %d of 5: %.0f dB", learn.n, e.strengthDb);
      }
    } else {
      groups.add(e.ms);
    }
  }
  if (e.kind == snd::Onsets::Kind::Long && learn.on) learn.longs++;

  const snd::Groups::Group g = groups.poll(ms);
  if (g.count == 0) return;
  last = {g.count, map.forClaps(g.count), millis()};
  if (testing) {
    Serial.printf("  group of %d", g.count);
    for (int i = 0; i + 1 < g.count && i < snd::Groups::kMax - 1; i++) Serial.printf("%s%lu ms", i ? ", " : ": gaps ", static_cast<unsigned long>(g.gapsMs[i]));
    Serial.println();
  }
  if (g.count > claps::kPatterns) lab.note(sv::Note::Warn, "%d claps: applause? nothing to do", g.count);
  else if (last.action == Action::None) lab.note(sv::Note::Info, "%d claps: not mapped", g.count);
  else lab.note(sv::Note::Info, "%d clap%s -> %s", g.count, g.count > 1 ? "s" : "", claps::info(last.action).name);
  if (last.action == Action::None) answer(lab, Action::None, false);  // the "nope" sound
  else run(lab, last.action);
}

// ---- Views ------------------------------------------------------------------------------------

// "2 claps -> colour", fading out over 3 s; while a group is open: "clap clap ..."
void groupLabel(FrameBuffer &fb, int x, int y) {
  char text[40];
  if (groups.pending()) {
    snprintf(text, sizeof(text), "%d clap%s ...", groups.pending(), groups.pending() > 1 ? "s" : "");
    sv::bold(fb, x, y, pal::amber, text);
    return;
  }
  const uint32_t age = millis() - last.at;
  if (last.count == 0 || age > 3000) return;
  snprintf(text, sizeof(text), "%d clap%s -> %s", last.count, last.count > 1 ? "s" : "", claps::info(last.action).name);
  sv::bold(fb, x, y, mix(pal::teal, pal::night, uint8_t(age > 2000 ? (age - 2000) * 220 / 1000 : 0)), text);
}

void drawTimeline(SoundLab &, FrameBuffer &fb, float) {
  const int x = 8, y = sv::kTop + 26, w = 304, h = 92;
  const float range = 60;
  auto yOf = [&](float db) { return y + h - int(snd::meterFraction(db, range) * h); };
  // The envelope: one column per pixel, the loudest frame that falls in it.
  for (int col = 0; col < w; col++) {
    const int a = col * kHistory / w, b = (col + 1) * kHistory / w;
    uint8_t m = 0;
    for (int i = a; i < b; i++) m = std::max(m, history[(historyHead + i) % kHistory]);
    if (m) fb.drawFastVLine(x + col, yOf(m), y + h - yOf(m), sv::levelColour(m));
  }
  // The two thresholds: the jump that makes an onset, the level that ends the sound.
  for (int i = 0; i < w; i += 4) {
    fb.drawPixel(x + i, yOf(ears.config.riseDb), pal::ink);
    fb.drawPixel(x + i, yOf(ears.config.releaseDb), pal::muted);
  }
  char text[32];
  snprintf(text, sizeof(text), "rise %.0f", ears.config.riseDb);
  sv::small(fb, x + w - sv::textWidth(fb, text), yOf(ears.config.riseDb) - 9, pal::ink, text);
  // The events: a clap is a teal flag, a long sound an amber bar with "long".
  for (const Mark &m : marks) {
    if (m.kind == snd::Onsets::Kind::None || historyMs - m.ms > 4000) continue;
    const int mx = x + w - int((historyMs - m.ms) * w / 4000);
    if (m.kind == snd::Onsets::Kind::Clap) {
      fb.drawFastVLine(mx, y - 4, h + 4, pal::teal);
      fb.fillTriangle(mx, y - 4, mx + 7, y - 1, mx, y + 2, pal::teal);
    } else if (m.kind == snd::Onsets::Kind::Long) {
      const int len = int(m.lengthMs * w / 4000);
      fb.fillRect(mx, y + h + 2, std::max(2, len), 3, pal::amber);
      sv::small(fb, mx, y + h + 6, pal::amber, "long");
    } else {
      fb.drawFastVLine(mx, y + h - 6, 6, pal::muted);
    }
  }
  fb.drawFastHLine(x, y + h, w, pal::dim);
  sv::small(fb, x, y + h + 6, pal::muted, "-4 s");
  sv::small(fb, x + w - 18, y + h + 6, pal::muted, "now");
  groupLabel(fb, x, sv::kTop + 18);
}

void drawLamp(SoundLab &, FrameBuffer &fb, float t) {
  // Left: the LED on GPIO 4 as a bulb with rays.
  const int bx = 80, by = sv::kTop + 52;
  const uint16_t bulb = out.led ? pal::amber : sv::kPanel;
  if (out.led)
    for (int i = 0; i < 10; i++) {
      const float a = i * 0.628f + t * 0.6f, r0 = 30 + 3 * sinf(t * 4 + i), r1 = r0 + 10;
      fb.drawLine(bx + int(cosf(a) * r0), by + int(sinf(a) * r0), bx + int(cosf(a) * r1), by + int(sinf(a) * r1), pal::amber);
    }
  fb.fillCircle(bx, by, 22, bulb);
  fb.drawCircle(bx, by, 22, pal::muted);
  fb.fillRect(bx - 10, by + 22, 21, 14, pal::dim);
  for (int j = 0; j < 3; j++) fb.drawFastHLine(bx - 10, by + 25 + j * 4, 21, pal::muted);
  sv::small(fb, bx - 30, by + 44, pal::muted, out.led ? "LED GPIO 4: on" : "LED GPIO 4: off");

  // Right: the RGB LED as a glowing orb in its colour, breathing.
  const int ox = 236, oy = sv::kTop + 52;
  uint16_t c = sv::kPanel;
  const bool party = millis() < out.partyUntil;
  if (party) c = hsv(uint8_t(t * 300), 255, 255);
  else if (out.colour >= 0) {
    const Colour &k = kColours[out.colour];
    // The LED values are kept dim (it is bright); scale them up for the screen.
    c = rgb(std::min(255, k.r * 2 + 20), std::min(255, k.g * 2 + 20), std::min(255, k.b * 2 + 20));
  }
  if (out.colour >= 0 || party)
    for (int r = 26; r <= 36; r += 3) fb.drawCircle(ox, oy, r + int(2 * sinf(t * 3)), mix(c, pal::night, uint8_t(80 + (r - 26) * 15)));
  fb.fillCircle(ox, oy, 24, out.colour >= 0 || party ? c : sv::kPanel);
  fb.drawCircle(ox, oy, 24, pal::muted);
  sv::small(fb, ox - 40, oy + 44, pal::muted,
            party ? "RGB: party!" : out.colour >= 0 ? kColours[out.colour].name : out.colour == -1 ? "RGB: off" : "RGB: level meter");
  if (party)
    for (int i = 0; i < 14; i++) {
      const float a = i * 0.449f + t * 2;
      const int r = 40 + int(fmodf(t * 60 + i * 9, 30));
      fb.fillCircle(ox + int(cosf(a) * r), oy + int(sinf(a) * r * 0.7f), 2, hsv(uint8_t(i * 18 + t * 200), 255, 255));
    }
  groupLabel(fb, 112, sv::kTop + 120);
}

void drawPatterns(SoundLab &, FrameBuffer &fb, float) {
  const uint32_t age = millis() - last.at;
  for (int n = 1; n <= claps::kPatterns; n++) {
    const int y = sv::kTop + 6 + (n - 1) * 26;
    const bool hit = last.count == n && age < 3000;
    if (hit) fb.fillRoundRect(4, y - 2, 312, 24, 4, mix(pal::teal, pal::night, 190));
    for (int i = 0; i < n; i++) {  // the claps as pairs of hands (two dots meeting)
      const int cx = 18 + i * 18;
      fb.fillCircle(cx - 3, y + 10, 4, hit ? pal::teal : pal::muted);
      fb.fillCircle(cx + 3, y + 10, 4, hit ? pal::ink : pal::dim);
    }
    const Action a = map.action[n];
    sv::bold(fb, 96, y + 15, a == Action::None ? pal::dim : hit ? pal::ink : sv::kSound, claps::info(a).name);
    sv::small(fb, 172, y + 7, pal::muted, claps::info(a).what);
  }
  char text[48];
  snprintf(text, sizeof(text), "sens %.0f dB - pause %lu ms ends a pattern", ears.config.riseDb, static_cast<unsigned long>(groups.gapMs));
  sv::small(fb, 8, sv::kTop + 112, pal::muted, text);
}

void drawTune(SoundLab &, FrameBuffer &fb, float t) {
  char text[48];
  if (learn.on) snprintf(text, sizeof(text), "clap %d of 5, as you normally would", learn.n + 1);
  else if (learn.suggested > 0) snprintf(text, sizeof(text), "softest x 60%% = sens %.0f dB (saved)", learn.suggested);
  else snprintf(text, sizeof(text), "double-click, then clap 5 times");
  sv::small(fb, 8, sv::kTop + 6, learn.on ? pal::amber : pal::muted, text);
  // Five bars: how loud each clap was (dB above the quiet) and how long it rang.
  const int y0 = sv::kTop + 104, hMax = 72;
  for (int i = 0; i < 5; i++) {
    const int x = 16 + i * 44;
    fb.drawRect(x, y0 - hMax, 30, hMax, pal::dim);
    if (i < learn.n) {
      const int h = int(snd::meterFraction(learn.strength[i], 60) * hMax);
      fb.fillRect(x + 1, y0 - h, 28, h, sv::levelColour(learn.strength[i]));
      snprintf(text, sizeof(text), "%.0f", learn.strength[i]);
      sv::small(fb, x + 8, y0 - h - 10, pal::ink, text);
      snprintf(text, sizeof(text), "%lums", static_cast<unsigned long>(learn.length[i]));
      sv::small(fb, x, y0 + 4, pal::muted, text);
    } else if (learn.on && i == learn.n && fmodf(t, 1) < 0.5f) {
      fb.fillRect(x + 1, y0 - 4, 28, 3, pal::amber);
    }
  }
  // The threshold line across the bars.
  const int ry = y0 - int(snd::meterFraction(ears.config.riseDb, 60) * hMax);
  for (int x = 12; x < 236; x += 3) fb.drawPixel(x, ry, pal::ink);
  snprintf(text, sizeof(text), "%.0f dB", ears.config.riseDb);
  sv::stat(fb, 244, sv::kTop + 24, "sens now", text, sv::kSound);
  snprintf(text, sizeof(text), "%d", learn.longs);
  sv::stat(fb, 244, sv::kTop + 66, "long (voice?)", text, learn.longs ? pal::amber : pal::ink);
}

const SoundView kViews[] = {
    {"timeline", drawTimeline, "test"},
    {"lamp", drawLamp, "colour"},
    {"patterns", drawPatterns, "maps"},
    {"tune", drawTune, "learn"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdMap(SoundLab &lab, const String &arg, const String &lower) {
  const int sp = lower.indexOf(' ');
  long n = 0;
  Action a;
  if (sp < 0 || !SoundLab::number(arg.substring(0, sp), 1, claps::kPatterns, n) || !claps::parse(lower.substring(sp + 1).c_str(), a))
    return lab.warn("map <1-4> <led|colour|dark|screen|view|party|tune|none>");
  map.action[n] = a;
  save();
  lab.note(sv::Note::Good, "%ld clap%s -> %s (saved)", n, n > 1 ? "s" : "", claps::info(a).name);
  lab.showView("patterns");
}

void printMaps() {
  for (int n = 1; n <= claps::kPatterns; n++)
    Serial.printf("  %d clap%s -> %-7s %s\n", n, n > 1 ? "s" : " ", claps::info(map.action[n]).name, claps::info(map.action[n]).what);
}

void cmdSens(SoundLab &lab, const String &arg, const String &) {
  float db = 0;
  if (!SoundLab::decimal(arg, 6, 50, db)) return lab.warn("sens <dB>, 6-50: how far above the quiet a clap must jump (now %.0f)", ears.config.riseDb);
  ears.config.riseDb = db;
  save();
  lab.note(sv::Note::Good, "sens %.0f dB (saved)", db);
}

void cmdGap(SoundLab &lab, const String &arg, const String &) {
  long ms = 0;
  if (!SoundLab::number(arg, 250, 2000, ms))
    return lab.warn("gap <ms>, 250-2000: the pause that ends a pattern (now %lu)", static_cast<unsigned long>(groups.gapMs));
  groups.gapMs = uint32_t(ms);
  save();
  lab.note(sv::Note::Good, "pause %ld ms ends a pattern (saved)", ms);
}

void cmdLearn(SoundLab &lab, const String &, const String &) {
  learn = Learn{};
  learn.on = true;
  groups.clear();
  lab.showView("tune");
  lab.note(sv::Note::Info, "clap 5 times, as you normally would");
}

void cmdTest(SoundLab &lab, const String &, const String &lower) {
  testing = lower.isEmpty() ? !testing : lower != "off";
  lab.note(sv::Note::Info, testing ? "test on: every clap, long sound and group printed" : "test off");
}

// Run an action by hand, to check the wiring: `do led`.
void cmdDo(SoundLab &lab, const String &, const String &lower) {
  Action a;
  if (!claps::parse(lower.c_str(), a)) return lab.warn("do <led|colour|dark|screen|view|party|tune>");
  run(lab, a);
}

void cmdMaps(SoundLab &, const String &, const String &) { printMaps(); }

void cmdSound(SoundLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("sound | sound on | sound off");
  sounds = lower.isEmpty() ? !sounds : lower == "on";
  save();
  lab.note(sv::Note::Info, sounds ? "sounds on (the echo guard keeps them from counting as claps)" : "sounds off");
}

const SoundCommand kCommands[] = {
    {"map", "<1-4> <action>", "what n claps do: led colour dark screen view party tune none", cmdMap},
    {"maps", "", "print the pattern map", cmdMaps},
    {"sens", "<dB>", "how far above the quiet a clap must jump (default 18)", cmdSens},
    {"gap", "<ms>", "the pause that ends a pattern (default 650)", cmdGap},
    {"learn", "", "clap 5 times: sets sens from your softest clap", cmdLearn},
    {"test", "[off]", "print every clap, long sound and group", cmdTest},
    {"do", "<action>", "run an action now, without clapping", cmdDo},
    {"sound", "[on|off]", "a little sound per pattern on the speaker (saved)", cmdSound},
    {"colour", "", "next RGB colour (the lamp view's double-click)", [](SoundLab &lab, const String &, const String &) { run(lab, Action::Colour); }},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(SoundLab &) {
  pinMode(PIN_LED, OUTPUT);
  // Lesson 02's safety net: the weakest drive (~5 mA), in case the LED has no resistor.
  gpio_set_drive_capability(static_cast<gpio_num_t>(PIN_LED), GPIO_DRIVE_CAP_0);
  digitalWrite(PIN_LED, LOW);
  load();
  printMaps();
}

void tick(SoundLab &lab) {
  if (out.screenOff && millis() - out.screenOffAt > 30000) setScreen(lab, false);
  // The party: the RGB LED runs round the colour wheel, then back to the chosen colour.
  static bool wasParty = false;
  const bool party = millis() < out.partyUntil;
  if (party) {
    const uint16_t c = hsv(uint8_t(millis() / 4), 255, 120);  // RGB565 back to 8 bits per colour
    rgbLedWrite(PIN_RGB, uint8_t((c >> 11) << 3), uint8_t(((c >> 5) & 63) << 2), uint8_t((c & 31) << 3));
  } else if (wasParty) {
    if (out.colour == -2) lab.claimLed(false);  // back to the level colour
    else showColour(lab);
  }
  wasParty = party;
}

// Any button press while the screen sleeps only wakes it.
bool gesture(SoundLab &lab, Gesture, bool) {
  if (!out.screenOff) return false;
  setScreen(lab, false);
  lab.note(sv::Note::Info, "screen on");
  return true;
}

const SoundLabSpec kSpec{
    "sound-lab-s3", "S3", "Clap control", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, frame, tick, gesture, "learn",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
