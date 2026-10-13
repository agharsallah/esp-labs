// Sound lab S2 Loudness.  uv run pio run -e sound-lab-s2 -t upload
// A sound level meter: how loud is it, compared with the quiet of your room? A minute of graph,
// a ten-minute noise diary, and an alarm that lights the LED when it stays too loud.
//
// The numbers are dB above the noise floor (see lib/sound-kit/src/level.h): 0 = your room's
// quiet, +6 = twice the wave height, +20 = ten times. Not dB SPL: that needs a calibrated mic.
//
// Wiring: S1's (KY-037: + -> 3V3, G -> GND, AO -> GPIO 6, DO -> GPIO 21) and the screen.
// Optional: lesson 02's LED on GPIO 4 (through its resistor) as the alarm light; button on GPIO 5.
//
// Views: meter · minute · diary · alarm   (click = next, double-click = the action in the footer)
// Serial: help lists the commands. Shared code: lib/sound-kit.
#include <Preferences.h>
#include <driver/gpio.h>

#include "sound_lab.h"

namespace {

// ---- State ------------------------------------------------------------------------------------

struct Settings {
  bool slow = false;      // which smoothing the meter, graphs and alarm use
  float limitDb = 35;     // the alarm line, dB above the quiet
  uint32_t holdMs = 1000; // over the line this long = alarm
  bool alarmOn = true;
} cfg;

float value(const snd::LevelReading &r) { return cfg.slow ? r.slowAbove() : r.above(); }

// One minute in 240 slots of 250 ms: the loudest moment of each.
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

Preferences prefs;

void saveSettings() {
  prefs.begin("loud", false);
  prefs.putBool("slow", cfg.slow);
  prefs.putFloat("limit", cfg.limitDb);
  prefs.putUInt("hold", cfg.holdMs);
  prefs.putBool("alarm", cfg.alarmOn);
  prefs.end();
}

void loadSettings() {
  prefs.begin("loud", true);
  cfg.slow = prefs.getBool("slow", false);
  cfg.limitDb = prefs.getFloat("limit", 35);
  cfg.holdMs = prefs.getUInt("hold", 1000);
  cfg.alarmOn = prefs.getBool("alarm", true);
  prefs.end();
}

// Roughly what a level above the quiet sounds like, for a typical room and the mic 30 cm away.
const char *likeWhat(float above) {
  if (above < 3) return "quiet";
  if (above < 12) return "rustle, whisper";
  if (above < 25) return "talking";
  if (above < 38) return "loud talk, music";
  return "shout, clap, bang";
}

// ---- Views ------------------------------------------------------------------------------------

void drawMeter(SoundLab &lab, FrameBuffer &fb, float) {
  const snd::LevelReading &r = lab.level();
  const float v = value(r);
  char text[48];
  snprintf(text, sizeof(text), "%.0f", v);
  sv::big(fb, 14, sv::kTop + 46, sv::levelColour(v), text);
  const int numW = sv::textWidth(fb, text);
  sv::bold(fb, 20 + numW, sv::kTop + 46, pal::muted, "dB");
  sv::small(fb, 20 + numW, sv::kTop + 12, pal::muted, cfg.slow ? "slow (1 s)" : "fast (125 ms)");
  sv::small(fb, 14, sv::kTop + 56, pal::ink, likeWhat(v));
  snprintf(text, sizeof(text), "above the quiet - quiet = %.0f dBFS", r.floorDb);
  sv::small(fb, 14, sv::kTop + 70, pal::muted, text);

  sv::meter(fb, 14, sv::kTop + 88, 292, 22, r, 60, cfg.limitDb);
  for (int db = 0; db <= 60; db += 10) {
    const int x = 15 + db * 290 / 60;
    fb.drawFastVLine(x, sv::kTop + 112, 3, pal::muted);
    snprintf(text, sizeof(text), "%d", db);
    sv::small(fb, x - (db ? 5 : 0), sv::kTop + 117, pal::muted, text);
  }
  snprintf(text, sizeof(text), "peak %.0f", r.peakDb - r.floorDb);
  sv::small(fb, 306 - 6 * int(strlen(text)), sv::kTop + 76, pal::ink, text);
  if (lab.meter().learning()) sv::small(fb, 200, sv::kTop + 12, pal::amber, "learning the quiet");
}

// A bar graph of `n` values (dB above the quiet, 0-60) from oldest to newest, with the limit line.
void graph(FrameBuffer &fb, int x, int y, int w, int h, const uint8_t *v, int n, int head, int filled) {
  fb.drawFastHLine(x, y + h, w, pal::dim);
  for (int db = 20; db <= 60; db += 20) {
    const int gy = y + h - db * h / 60;
    for (int gx = x; gx < x + w; gx += 4) fb.drawPixel(gx, gy, pal::dim);
  }
  for (int i = 0; i < filled; i++) {
    const int at = (head - filled + i + n) % n;
    const int bh = std::min(h, int(v[at]) * h / 60);
    const int x0 = x + w - filled * w / n + i * w / n, x1 = x + w - filled * w / n + (i + 1) * w / n;
    if (bh) fb.fillRect(x0, y + h - bh, std::max(1, x1 - x0), bh, sv::levelColour(v[at]));
  }
  const int ly = y + h - int(cfg.limitDb) * h / 60;
  for (int gx = x; gx < x + w; gx += 3) fb.drawPixel(gx, ly, pal::coral);
}

void drawMinute(SoundLab &, FrameBuffer &fb, float) {
  graph(fb, 24, sv::kTop + 8, 288, 100, minute.v, kMinuteSlots, minute.head, minute.filled);
  sv::small(fb, 2, sv::kTop + 6, pal::muted, "60");
  sv::small(fb, 2, sv::kTop + 40, pal::muted, "40");
  sv::small(fb, 2, sv::kTop + 73, pal::muted, "20");
  sv::small(fb, 24, sv::kTop + 114, pal::muted, "-60 s");
  sv::small(fb, 288, sv::kTop + 114, pal::muted, "now");
  char text[40];
  snprintf(text, sizeof(text), "loudest each 1/4 s - limit %.0f dB", cfg.limitDb);
  sv::small(fb, 90, sv::kTop + 114, pal::muted, text);
}

void drawDiary(SoundLab &, FrameBuffer &fb, float) {
  // Each 10 s: a thin line from quietest to loudest, a block at the average. Red cap = alarm.
  const int x = 24, y = sv::kTop + 8, w = 288, h = 100;
  fb.drawFastHLine(x, y + h, w, pal::dim);
  const int ly = y + h - int(cfg.limitDb) * h / 60;
  for (int gx = x; gx < x + w; gx += 3) fb.drawPixel(gx, ly, pal::coral);
  for (int i = 0; i < diary.filled; i++) {
    const DiarySlot &d = diary.s[(diary.head - diary.filled + i + kDiarySlots) % kDiarySlots];
    const int cx = x + w - (diary.filled - i) * w / kDiarySlots + 2;
    const int yLo = y + h - std::min(h, d.lo * h / 60), yHi = y + h - std::min(h, d.hi * h / 60), yAvg = y + h - std::min(h, d.avg * h / 60);
    fb.drawFastVLine(cx + 1, yHi, yLo - yHi + 1, pal::muted);
    fb.fillRect(cx - 1, yAvg - 2, 4, 5, sv::levelColour(d.avg));
    if (d.over) fb.fillRect(cx - 1, yHi - 4, 4, 2, pal::coral);
  }
  sv::small(fb, 2, sv::kTop + 6, pal::muted, "60");
  sv::small(fb, 2, sv::kTop + 73, pal::muted, "20");
  sv::small(fb, 24, sv::kTop + 114, pal::muted, "-10 min");
  sv::small(fb, 288, sv::kTop + 114, pal::muted, "now");
  sv::small(fb, 100, sv::kTop + 114, pal::muted, "each 10 s: quietest-loudest, avg");
}

void drawAlarm(SoundLab &lab, FrameBuffer &fb, float t) {
  const float v = value(lab.level());
  if (alarm.active) {
    // A soft pulse, not a strobe: easy on the eyes.
    const uint8_t k = uint8_t(80 + 60 * sinf(t * 6.0f));
    fb.fillRect(0, sv::kTop, sv::kW, sv::kBottom - sv::kTop, mix(pal::night, pal::coral, k));
    sv::big(fb, 40, sv::kTop + 60, pal::ink, "TOO LOUD");
  } else {
    sv::bold(fb, 14, sv::kTop + 24, cfg.alarmOn ? pal::teal : pal::muted, cfg.alarmOn ? "Alarm armed" : "Alarm off");
  }
  char text[48];
  snprintf(text, sizeof(text), "now %.0f dB - limit %.0f dB for %.1f s", v, cfg.limitDb, cfg.holdMs / 1000.0f);
  sv::small(fb, 14, sv::kTop + 76, pal::ink, text);
  // The fuse: how long the level has been over the line, filling up to holdMs.
  const uint32_t over = alarm.overSince ? lab.soundMs() - alarm.overSince : 0;
  const int fw = int(std::min(1.0f, float(over) / float(cfg.holdMs)) * 292);
  fb.drawRect(14, sv::kTop + 88, 292, 8, pal::dim);
  if (fw) fb.fillRect(14, sv::kTop + 88, fw, 8, pal::amber);
  snprintf(text, sizeof(text), "%lu alarms", static_cast<unsigned long>(alarm.count));
  sv::stat(fb, 14, sv::kTop + 102, "since start", text);
  if (alarm.count) {
    snprintf(text, sizeof(text), "%lus ago", static_cast<unsigned long>((lab.soundMs() - alarm.lastMs) / 1000));
    sv::stat(fb, 120, sv::kTop + 102, "last", text);
    snprintf(text, sizeof(text), "%.0f dB", alarm.worst);
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

void cmdLimit(SoundLab &lab, const String &arg, const String &) {
  float db = 0;
  if (!SoundLab::decimal(arg, 3, 60, db)) return lab.warn("limit <dB above the quiet>, 3-60 (now %.0f)", cfg.limitDb);
  cfg.limitDb = db;
  saveSettings();
  lab.note(sv::Note::Info, "limit %.0f dB", db);
}

void cmdHold(SoundLab &lab, const String &arg, const String &) {
  long ms = 0;
  if (!SoundLab::number(arg, 0, 10000, ms)) return lab.warn("hold <ms>, 0-10000: how long over the limit before the alarm (now %lu)",
                                                           static_cast<unsigned long>(cfg.holdMs));
  cfg.holdMs = uint32_t(ms);
  saveSettings();
}

void cmdAlarm(SoundLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("alarm | alarm on | alarm off");
  cfg.alarmOn = lower.isEmpty() ? !cfg.alarmOn : lower == "on";
  saveSettings();
  lab.note(sv::Note::Info, cfg.alarmOn ? "alarm armed" : "alarm off");
}

void cmdDiary(SoundLab &, const String &, const String &) {
  Serial.printf("Noise diary, newest last. Each row = 10 s, dB above the quiet. Limit %.0f dB.\n", cfg.limitDb);
  Serial.println("  ago    low  avg  high");
  for (int i = 0; i < diary.filled; i++) {
    const DiarySlot &d = diary.s[(diary.head - diary.filled + i + kDiarySlots) % kDiarySlots];
    char bar[31];
    const int n = std::min(30, d.avg / 2);
    for (int j = 0; j < 30; j++) bar[j] = j < n ? '=' : ' ';
    bar[30] = 0;
    Serial.printf("  %3ds   %3d  %3d  %3d  |%s|%s\n", (diary.filled - i) * 10, d.lo, d.avg, d.hi, bar, d.over ? " ALARM" : "");
  }
}

const SoundCommand kCommands[] = {
    {"speed", "[fast|slow]", "smoothing: 125 ms or 1 s, like a real meter", cmdSpeed},
    {"limit", "<dB>", "the alarm line, dB above the quiet (default 35)", cmdLimit},
    {"hold", "<ms>", "how long over the line before the alarm (default 1000)", cmdHold},
    {"alarm", "[on|off]", "the too-loud alarm (LED on GPIO 4 + screen)", cmdAlarm},
    {"diary", "", "print the last 10 minutes, 10 s per row", cmdDiary},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(SoundLab &) {
  loadSettings();
  // Lesson 02's LED on GPIO 4, with the drive strength turned down as a safety net (in case it has
  // no resistor). Nothing connected? Then this pin simply does nothing.
  pinMode(PIN_LED, OUTPUT);
  gpio_set_drive_capability(gpio_num_t(PIN_LED), GPIO_DRIVE_CAP_0);
  digitalWrite(PIN_LED, LOW);
}

void frame(SoundLab &lab, const snd::Frame &, uint32_t ms) {
  const float v = value(lab.level());
  if (lab.meter().learning()) return;  // the first half second only learns the quiet

  // The minute graph.
  if (minute.slotStart == 0) minute.slotStart = ms;
  minute.slotMax = std::max(minute.slotMax, v);
  if (ms - minute.slotStart >= 250) {
    minute.v[minute.head] = uint8_t(std::min(60.0f, minute.slotMax));
    minute.head = (minute.head + 1) % kMinuteSlots;
    minute.filled = std::min(minute.filled + 1, kMinuteSlots);
    minute.slotMax = 0;
    minute.slotStart = ms;
  }

  // The alarm: over the line for holdMs.
  if (v >= cfg.limitDb) {
    if (!alarm.overSince) alarm.overSince = ms;
    if (cfg.alarmOn && !alarm.active && ms - alarm.overSince >= cfg.holdMs) {
      alarm.active = true;
      alarm.count++;
      alarm.lastMs = ms;
      diary.over = true;
      lab.note(sv::Note::Bad, "too loud: %.0f dB", v);
      digitalWrite(PIN_LED, HIGH);
    }
    if (alarm.active) alarm.worst = std::max(alarm.worst, v);
  } else if (v < cfg.limitDb - 3) {  // 3 dB of hysteresis: no flicker right at the line
    alarm.overSince = 0;
    if (alarm.active) {
      alarm.active = false;
      digitalWrite(PIN_LED, LOW);
      lab.note(sv::Note::Good, "calm again");
    }
  }

  // The diary.
  if (diary.slotStart == 0) diary.slotStart = ms;
  diary.lo = std::min(diary.lo, v), diary.hi = std::max(diary.hi, v), diary.sum += v, diary.n++;
  if (ms - diary.slotStart >= 10000) {
    DiarySlot &d = diary.s[diary.head];
    d.lo = uint8_t(std::min(60.0f, diary.lo));
    d.hi = uint8_t(std::min(60.0f, diary.hi));
    d.avg = uint8_t(std::min(60.0f, diary.sum / diary.n));
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
