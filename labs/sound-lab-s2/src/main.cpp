// Sound lab S2 Loudness.  uv run pio run -d labs/sound-lab-s2 -e sound-lab-s2 -t upload
// A sound level meter: how loud is it? A minute of graph, a ten-minute noise diary, and an alarm
// that lights the LED (and sounds the speaker) when it stays too loud.
//
// Two kinds of number, depending on the microphone:
//   dB above the quiet (any mic, see kit/lib/sound-kit/src/level.h): 0 = your room's quiet, +6 = twice
//     the wave height, +20 = ten times. Relative: the KY-037's sensitivity is unknown.
//   dB SPL (INMP441 only): sound pressure level, the number on a real meter. The INMP441's datasheet
//     gives its sensitivity (-26 dBFS at 94 dB SPL), so its numbers convert directly. Honest caveats:
//     about +-2 dB, and unweighted: real meters show dBA, weighted like the ear, so a low hum reads
//     higher here than on a dBA meter. `units` switches between the two.
//
// Wiring: the screen, and a microphone: the INMP441 (VDD -> 3V3, GND -> GND, L/R -> GND, SCK -> 40,
// WS -> 41, SD -> 42) for dB SPL, or the KY-037 (+ -> 3V3, G -> GND, AO -> GPIO 6, DO -> GPIO 21).
// Optional: lesson 02's LED on GPIO 4 (through its resistor) as the alarm light, the MAX98357A +
// speaker (BCLK 39, LRC 38, DIN 47, VIN 5V) for the alarm sound, the button on GPIO 5.
//
// The echo guard (kit/lib/sound-kit/src/sound_lab.h) pauses the graphs while the alarm sounds, so the
// board never measures its own alarm and keeps itself ringing.
//
// Views: meter · minute · diary · alarm   (click = next, double-click = the action in the footer)
// Serial: help lists the commands. Shared code: kit/lib/sound-kit.
#include <Preferences.h>
#include <driver/gpio.h>

#include "sound_lab.h"

namespace {

// ---- State ------------------------------------------------------------------------------------

struct Settings {
  bool slow = false;       // which smoothing the meter, graphs and alarm use
  bool spl = true;         // dB SPL when the microphone allows it (INMP441)
  float limitDb = 35;      // the alarm line, dB above the quiet
  float limitSpl = 80;     // the alarm line, dB SPL
  uint32_t holdMs = 1000;  // over the line this long = alarm
  bool alarmOn = true;
  bool alarmSound = true;  // the speaker plays the alarm tune once per alarm
} cfg;

const char *const kAlarmTune = "A5/16 E5/16 A5/16 E5/16 A5/16 E5/16";

// The scale everything is drawn on: 0-60 dB above the quiet, or 30-110 dB SPL.
struct Scale {
  float lo, hi;
  const char *unit;
};

bool splMode(SoundLab &lab) { return cfg.spl && lab.mic().digital(); }
Scale scale(SoundLab &lab) { return splMode(lab) ? Scale{30, 110, "dB SPL"} : Scale{0, 60, "dB"}; }
float limit(SoundLab &lab) { return splMode(lab) ? cfg.limitSpl : cfg.limitDb; }

float value(SoundLab &lab) {
  const snd::LevelReading &r = lab.level();
  if (splMode(lab)) return lab.mic().splOf(cfg.slow ? r.slowDb : r.fastDb);
  return cfg.slow ? r.slowAbove() : r.above();
}
float peakValue(SoundLab &lab) {
  const snd::LevelReading &r = lab.level();
  return splMode(lab) ? lab.mic().splOf(r.peakDb) : r.peakDb - r.floorDb;
}

// Green while calm, amber when loud, coral when very loud. In dB SPL: 85 is where hearing risk
// starts after hours of it; in dB above the quiet the line is a rough guess.
uint16_t colourFor(SoundLab &lab, float v) {
  if (!splMode(lab)) return sv::levelColour(v);
  return v < 65 ? pal::teal : v < 85 ? pal::amber : pal::coral;
}

float fraction(const Scale &s, float v) { return std::max(0.0f, std::min(1.0f, (v - s.lo) / (s.hi - s.lo))); }

// One minute in 240 slots of 250 ms: the loudest moment of each. Values in the current unit.
constexpr int kMinuteSlots = 240;
struct Minute {
  uint8_t v[kMinuteSlots] = {};
  int head = 0, filled = 0;
  float slotMax = 0;
  uint32_t slotStart = 0;
} minute;

// Ten minutes in 60 slots of 10 s: quietest, average and loudest of each.
constexpr int kDiarySlots = 60;
struct DiarySlot {
  uint8_t lo = 0, avg = 0, hi = 0;
  bool over = false;  // the alarm fired in this slot
};
struct Diary {
  DiarySlot s[kDiarySlots];
  int head = 0, filled = 0;
  float lo = 1e9f, hi = 0, sum = 0;
  uint32_t n = 0, slotStart = 0;
  bool over = false;
} diary;

struct Alarm {
  bool active = false;
  uint32_t overSince = 0;  // sound ms when the level went over the line, 0 = under
  uint32_t count = 0, lastMs = 0;
  float worst = 0;         // the loudest level during alarms
} alarm;

// The graphs hold numbers in one unit: switching units starts them again.
void clearHistory() {
  minute = Minute{};
  diary = Diary{};
  alarm.worst = 0;
}

Preferences prefs;

void saveSettings() {
  prefs.begin("loud", false);
  prefs.putBool("slow", cfg.slow);
  prefs.putBool("spl", cfg.spl);
  prefs.putFloat("limit", cfg.limitDb);
  prefs.putFloat("limspl", cfg.limitSpl);
  prefs.putUInt("hold", cfg.holdMs);
  prefs.putBool("alarm", cfg.alarmOn);
  prefs.putBool("asnd", cfg.alarmSound);
  prefs.end();
}

void loadSettings() {
  prefs.begin("loud", true);
  cfg.slow = prefs.getBool("slow", false);
  cfg.spl = prefs.getBool("spl", true);
  cfg.limitDb = prefs.getFloat("limit", 35);
  cfg.limitSpl = prefs.getFloat("limspl", 80);
  cfg.holdMs = prefs.getUInt("hold", 1000);
  cfg.alarmOn = prefs.getBool("alarm", true);
  cfg.alarmSound = prefs.getBool("asnd", true);
  prefs.end();
}

// Roughly what a level sounds like. Above the quiet: a typical room, the mic 30 cm away.
const char *likeWhat(SoundLab &lab, float v) {
  if (splMode(lab)) {
    if (v < 35) return "very quiet room";
    if (v < 50) return "quiet room, whisper";
    if (v < 65) return "talking";
    if (v < 80) return "loud talk, busy street";
    if (v < 95) return "shouting, loud music: limit your time";
    return "very loud: protect your ears";
  }
  if (v < 3) return "quiet";
  if (v < 12) return "rustle, whisper";
  if (v < 25) return "talking";
  if (v < 38) return "loud talk, music";
  return "shout, clap, bang";
}

// ---- Views ------------------------------------------------------------------------------------

// A level bar on any scale: coloured in zones, the peak hold as a tick, the limit as a dotted line.
void bar(SoundLab &lab, FrameBuffer &fb, int x, int y, int w, int h, const Scale &s, float v, float peak) {
  fb.fillRoundRect(x, y, w, h, 3, sv::kPanel);
  const int fill = int(fraction(s, v) * (w - 2));
  for (int i = 0; i < fill; i += 3) fb.fillRect(x + 1 + i, y + 2, 2, h - 4, colourFor(lab, s.lo + float(i) / (w - 2) * (s.hi - s.lo)));
  fb.drawFastVLine(x + 1 + int(fraction(s, peak) * (w - 2)), y, h, pal::ink);
  const int lx = x + 1 + int(fraction(s, limit(lab)) * (w - 2));
  for (int j = y - 3; j < y + h + 3; j += 2) fb.drawPixel(lx, j, pal::coral);
}

void drawMeter(SoundLab &lab, FrameBuffer &fb, float) {
  const snd::LevelReading &r = lab.level();
  const Scale s = scale(lab);
  const float v = value(lab);
  char text[48];
  snprintf(text, sizeof(text), "%.0f", v);
  sv::big(fb, 14, sv::kTop + 46, colourFor(lab, v), text);
  const int numW = sv::textWidth(fb, text);
  sv::bold(fb, 20 + numW, sv::kTop + 46, pal::muted, s.unit);
  sv::small(fb, 20 + numW, sv::kTop + 12, pal::muted, cfg.slow ? "slow (1 s)" : "fast (125 ms)");
  sv::small(fb, 14, sv::kTop + 56, pal::ink, likeWhat(lab, v));
  if (splMode(lab)) snprintf(text, sizeof(text), "%.0f dB above the quiet - unweighted, +-2 dB", r.above());
  else snprintf(text, sizeof(text), "above the quiet - quiet = %.0f dBFS", r.floorDb);
  sv::small(fb, 14, sv::kTop + 70, pal::muted, text);

  const int x = 14, y = sv::kTop + 88, w = 292;
  bar(lab, fb, x, y, w, 22, s, v, peakValue(lab));
  if (splMode(lab)) {
    // Familiar sounds on the dB SPL scale; 85 is the hearing-risk line (hours a day).
    const struct {
      int db;
      const char *label;
    } marks[] = {{30, "30 whisper"}, {60, "60 talk"}, {85, "85 risk"}, {100, "100 loud"}};
    for (const auto &m : marks) {
      const int mx = x + 1 + int(fraction(s, float(m.db)) * (w - 2));
      fb.drawFastVLine(mx, y + 24, 3, m.db == 85 ? pal::coral : pal::muted);
      sv::small(fb, std::min(mx, x + w - sv::textWidth(fb, m.label)), y + 29, m.db == 85 ? pal::coral : pal::muted, m.label);
    }
  } else {
    for (int db = 0; db <= 60; db += 10) {
      const int mx = x + 1 + db * (w - 2) / 60;
      fb.drawFastVLine(mx, y + 24, 3, pal::muted);
      snprintf(text, sizeof(text), "%d", db);
      sv::small(fb, mx - (db ? 5 : 0), y + 29, pal::muted, text);
    }
  }
  snprintf(text, sizeof(text), "peak %.0f", peakValue(lab));
  sv::small(fb, 306 - 6 * int(strlen(text)), sv::kTop + 76, pal::ink, text);
  if (lab.meter().learning()) sv::small(fb, 200, sv::kTop + 12, pal::amber, "learning the quiet");
}

// Grid lines and labels for a graph on the current scale.
void grid(SoundLab &lab, FrameBuffer &fb, int x, int y, int w, int h) {
  const Scale s = scale(lab);
  fb.drawFastHLine(x, y + h, w, pal::dim);
  const float step = 20;
  for (float db = s.lo + step; db <= s.hi; db += step) {
    const int gy = y + h - int(fraction(s, db) * h);
    for (int gx = x; gx < x + w; gx += 4) fb.drawPixel(gx, gy, pal::dim);
    char t[8];
    snprintf(t, sizeof(t), "%.0f", db);
    sv::small(fb, 0, gy - 3, pal::muted, t);
  }
  const int ly = y + h - int(fraction(s, limit(lab)) * h);
  for (int gx = x; gx < x + w; gx += 3) fb.drawPixel(gx, ly, pal::coral);
}

void drawMinute(SoundLab &lab, FrameBuffer &fb, float) {
  const int x = 24, y = sv::kTop + 8, w = 288, h = 100;
  const Scale s = scale(lab);
  grid(lab, fb, x, y, w, h);
  for (int i = 0; i < minute.filled; i++) {
    const int at = (minute.head - minute.filled + i + kMinuteSlots) % kMinuteSlots;
    const int bh = int(fraction(s, minute.v[at]) * h);
    const int x0 = x + w - minute.filled * w / kMinuteSlots + i * w / kMinuteSlots;
    const int x1 = x + w - minute.filled * w / kMinuteSlots + (i + 1) * w / kMinuteSlots;
    if (bh) fb.fillRect(x0, y + h - bh, std::max(1, x1 - x0), bh, colourFor(lab, minute.v[at]));
  }
  sv::small(fb, x, sv::kTop + 114, pal::muted, "-60 s");
  sv::small(fb, 288, sv::kTop + 114, pal::muted, "now");
  char text[48];
  snprintf(text, sizeof(text), "loudest each 1/4 s, %s - limit %.0f", s.unit, limit(lab));
  sv::small(fb, 66, sv::kTop + 114, pal::muted, text);
}

void drawDiary(SoundLab &lab, FrameBuffer &fb, float) {
  // Each 10 s: a thin line from quietest to loudest, a block at the average. Red cap = alarm.
  const int x = 24, y = sv::kTop + 8, w = 288, h = 100;
  const Scale s = scale(lab);
  grid(lab, fb, x, y, w, h);
  auto yOf = [&](uint8_t v) { return y + h - int(fraction(s, v) * h); };
  for (int i = 0; i < diary.filled; i++) {
    const DiarySlot &d = diary.s[(diary.head - diary.filled + i + kDiarySlots) % kDiarySlots];
    const int cx = x + w - (diary.filled - i) * w / kDiarySlots + 2;
    fb.drawFastVLine(cx + 1, yOf(d.hi), yOf(d.lo) - yOf(d.hi) + 1, pal::muted);
    fb.fillRect(cx - 1, yOf(d.avg) - 2, 4, 5, colourFor(lab, d.avg));
    if (d.over) fb.fillRect(cx - 1, yOf(d.hi) - 4, 4, 2, pal::coral);
  }
  sv::small(fb, x, sv::kTop + 114, pal::muted, "-10 min");
  sv::small(fb, 288, sv::kTop + 114, pal::muted, "now");
  char text[48];
  snprintf(text, sizeof(text), "each 10 s, %s: low-high, avg", s.unit);
  sv::small(fb, 84, sv::kTop + 114, pal::muted, text);
}

void drawAlarm(SoundLab &lab, FrameBuffer &fb, float t) {
  const float v = value(lab);
  const Scale s = scale(lab);
  if (alarm.active) {
    // A soft pulse, not a strobe: easy on the eyes.
    const uint8_t k = uint8_t(80 + 60 * sinf(t * 6.0f));
    fb.fillRect(0, sv::kTop, sv::kW, sv::kBottom - sv::kTop, mix(pal::night, pal::coral, k));
    sv::big(fb, 40, sv::kTop + 60, pal::ink, "TOO LOUD");
  } else {
    sv::bold(fb, 14, sv::kTop + 24, cfg.alarmOn ? pal::teal : pal::muted, cfg.alarmOn ? "Alarm armed" : "Alarm off");
    sv::small(fb, 14, sv::kTop + 34, pal::muted, cfg.alarmSound ? "light + sound" : "light only (alarm sound on)");
  }
  char text[56];
  snprintf(text, sizeof(text), "now %.0f %s - limit %.0f for %.1f s", v, s.unit, limit(lab), cfg.holdMs / 1000.0f);
  sv::small(fb, 14, sv::kTop + 76, pal::ink, text);
  // The fuse: how long the level has been over the line, filling up to holdMs.
  const uint32_t over = alarm.overSince ? lab.soundMs() - alarm.overSince : 0;
  const int fw = int(std::min(1.0f, float(over) / float(std::max<uint32_t>(cfg.holdMs, 1))) * 292);
  fb.drawRect(14, sv::kTop + 88, 292, 8, pal::dim);
  if (fw) fb.fillRect(14, sv::kTop + 88, fw, 8, pal::amber);
  snprintf(text, sizeof(text), "%lu alarms", static_cast<unsigned long>(alarm.count));
  sv::stat(fb, 14, sv::kTop + 102, "since start", text);
  if (alarm.count) {
    snprintf(text, sizeof(text), "%lus ago", static_cast<unsigned long>((lab.soundMs() - alarm.lastMs) / 1000));
    sv::stat(fb, 120, sv::kTop + 102, "last", text);
    snprintf(text, sizeof(text), "%.0f", alarm.worst);
    sv::stat(fb, 220, sv::kTop + 102, "loudest", text, pal::coral);
  }
}

const SoundView kViews[] = {
    {"meter", drawMeter, "speed"},
    {"minute", drawMinute, "quiet"},
    {"diary", drawDiary, "diary"},
    {"alarm", drawAlarm, "alarm"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdSpeed(SoundLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "fast" && lower != "slow") return lab.warn("speed | speed fast | speed slow");
  cfg.slow = lower.isEmpty() ? !cfg.slow : lower == "slow";
  saveSettings();
  lab.note(sv::Note::Info, cfg.slow ? "slow: 1 s smoothing" : "fast: 125 ms smoothing");
}

void cmdUnits(SoundLab &lab, const String &, const String &lower) {
  if (lower != "spl" && lower != "quiet" && !lower.isEmpty()) return lab.warn("units | units spl | units quiet");
  const bool want = lower.isEmpty() ? !cfg.spl : lower == "spl";
  if (want && !lab.mic().digital()) return lab.warn("dB SPL needs the INMP441 (its sensitivity is known); the KY-037 shows dB above the quiet");
  cfg.spl = want;
  clearHistory();
  saveSettings();
  lab.note(sv::Note::Info, want ? "units: dB SPL" : "units: dB above the quiet");
}

// limit <n> [spl|quiet]: the alarm line. Without a unit, in the units shown now.
void cmdLimit(SoundLab &lab, const String &, const String &lower) {
  const int sp = lower.indexOf(' ');
  const String num = sp < 0 ? lower : lower.substring(0, sp), unit = sp < 0 ? String() : lower.substring(sp + 1);
  if (!unit.isEmpty() && unit != "spl" && unit != "quiet") return lab.warn("limit <dB> [spl|quiet]");
  const bool spl = unit.isEmpty() ? splMode(lab) : unit == "spl";
  if (spl && !lab.mic().digital()) return lab.warn("a dB SPL limit needs the INMP441; try limit 35 (dB above the quiet)");
  float db = 0;
  if (spl ? !SoundLab::decimal(num, 30, 120, db) : !SoundLab::decimal(num, 3, 60, db))
    return lab.warn("limit <dB> [spl|quiet]: 30-120 dB SPL or 3-60 dB above the quiet (now %.0f %s)", limit(lab), scale(lab).unit);
  (spl ? cfg.limitSpl : cfg.limitDb) = db;
  if (spl != cfg.spl) cfg.spl = spl, clearHistory();
  saveSettings();
  lab.note(sv::Note::Info, "limit %.0f %s", db, spl ? "dB SPL" : "dB above the quiet");
}

void cmdHold(SoundLab &lab, const String &arg, const String &) {
  long ms = 0;
  if (!SoundLab::number(arg, 0, 10000, ms)) return lab.warn("hold <ms>, 0-10000: how long over the limit before the alarm (now %lu)",
                                                           static_cast<unsigned long>(cfg.holdMs));
  cfg.holdMs = uint32_t(ms);
  saveSettings();
}

void cmdAlarm(SoundLab &lab, const String &, const String &lower) {
  if (lower == "sound on" || lower == "sound off" || lower == "sound") {
    cfg.alarmSound = lower == "sound" ? !cfg.alarmSound : lower == "sound on";
    saveSettings();
    return lab.note(sv::Note::Info, cfg.alarmSound ? "alarm sound on" : "alarm sound off");
  }
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("alarm [on|off] | alarm sound [on|off]");
  cfg.alarmOn = lower.isEmpty() ? !cfg.alarmOn : lower == "on";
  saveSettings();
  lab.note(sv::Note::Info, cfg.alarmOn ? "alarm armed" : "alarm off");
}

void cmdDiary(SoundLab &lab, const String &, const String &) {
  const Scale s = scale(lab);
  Serial.printf("Noise diary, newest last. Each row = 10 s, %s. Limit %.0f.\n", splMode(lab) ? "dB SPL (unweighted)" : "dB above the quiet",
                limit(lab));
  Serial.println("  ago    low  avg  high");
  for (int i = 0; i < diary.filled; i++) {
    const DiarySlot &d = diary.s[(diary.head - diary.filled + i + kDiarySlots) % kDiarySlots];
    char line[31];
    const int n = int(fraction(s, d.avg) * 30);
    for (int j = 0; j < 30; j++) line[j] = j < n ? '=' : ' ';
    line[30] = 0;
    Serial.printf("  %3ds   %3d  %3d  %3d  |%s|%s\n", (diary.filled - i) * 10, d.lo, d.avg, d.hi, line, d.over ? " ALARM" : "");
  }
}

const SoundCommand kCommands[] = {
    {"speed", "[fast|slow]", "smoothing: 125 ms or 1 s, like a real meter", cmdSpeed},
    {"units", "[spl|quiet]", "dB SPL (INMP441) or dB above the quiet", cmdUnits},
    {"limit", "<dB> [spl|quiet]", "the alarm line (default 80 dB SPL / 35 dB above the quiet)", cmdLimit},
    {"hold", "<ms>", "how long over the line before the alarm (default 1000)", cmdHold},
    {"alarm", "[on|off] | sound [on|off]", "the too-loud alarm: LED on GPIO 4, screen, speaker", cmdAlarm},
    {"diary", "", "print the last 10 minutes, 10 s per row", cmdDiary},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(SoundLab &lab) {
  loadSettings();
  // Lesson 02's LED on GPIO 4, with the drive strength turned down as a safety net (in case it has
  // no resistor). Nothing connected? Then this pin simply does nothing.
  pinMode(PIN_LED, OUTPUT);
  gpio_set_drive_capability(gpio_num_t(PIN_LED), GPIO_DRIVE_CAP_0);
  digitalWrite(PIN_LED, LOW);
  Serial.printf("Units: %s\n", splMode(lab) ? "dB SPL (INMP441: unweighted, about +-2 dB)" : "dB above the quiet");
}

void frame(SoundLab &lab, const snd::Frame &, uint32_t ms) {
  if (lab.meter().learning()) return;  // the first half second only learns the quiet
  const float v = value(lab);
  const float lim = limit(lab);
  const uint8_t v8 = uint8_t(std::max(0.0f, std::min(255.0f, v)));

  // The minute graph.
  if (minute.slotStart == 0) minute.slotStart = ms;
  minute.slotMax = std::max(minute.slotMax, v);
  if (ms - minute.slotStart >= 250) {
    minute.v[minute.head] = uint8_t(std::max(0.0f, std::min(255.0f, minute.slotMax)));
    minute.head = (minute.head + 1) % kMinuteSlots;
    minute.filled = std::min(minute.filled + 1, kMinuteSlots);
    minute.slotMax = 0;
    minute.slotStart = ms;
  }

  // The alarm: over the line for holdMs.
  if (v >= lim) {
    if (!alarm.overSince) alarm.overSince = ms;
    if (cfg.alarmOn && !alarm.active && ms - alarm.overSince >= cfg.holdMs) {
      alarm.active = true;
      alarm.count++;
      alarm.lastMs = ms;
      diary.over = true;
      lab.note(sv::Note::Bad, "too loud: %.0f %s", v, scale(lab).unit);
      digitalWrite(PIN_LED, HIGH);
      if (cfg.alarmSound) lab.speaker().tune(kAlarmTune, 200, syn::Wave::Square);
    }
    if (alarm.active) alarm.worst = std::max(alarm.worst, v);
  } else if (v < lim - 3) {  // 3 dB of hysteresis: no flicker right at the line
    alarm.overSince = 0;
    if (alarm.active) {
      alarm.active = false;
      digitalWrite(PIN_LED, LOW);
      lab.note(sv::Note::Good, "calm again");
    }
  }

  // The diary.
  if (diary.slotStart == 0) diary.slotStart = ms;
  diary.lo = std::min(diary.lo, float(v8)), diary.hi = std::max(diary.hi, float(v8)), diary.sum += v8, diary.n++;
  if (ms - diary.slotStart >= 10000) {
    DiarySlot &d = diary.s[diary.head];
    d.lo = uint8_t(diary.lo);
    d.hi = uint8_t(diary.hi);
    d.avg = uint8_t(diary.sum / diary.n);
    d.over = diary.over;
    diary.head = (diary.head + 1) % kDiarySlots;
    diary.filled = std::min(diary.filled + 1, kDiarySlots);
    diary.lo = 1e9f, diary.hi = 0, diary.sum = 0, diary.n = 0, diary.over = false;
    diary.slotStart = ms;
  }
}

// While the alarm rings, any button press silences it (it re-arms when the room calms down).
bool gesture(SoundLab &lab, Gesture, bool) {
  if (!alarm.active) return false;
  alarm.active = false;
  alarm.overSince = 0;
  digitalWrite(PIN_LED, LOW);
  lab.speaker().stop();
  lab.note(sv::Note::Info, "alarm silenced");
  return true;
}

const SoundLabSpec kSpec{
    "sound-lab-s2", "S2", "Loudness", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, frame, nullptr, gesture, "quiet",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
