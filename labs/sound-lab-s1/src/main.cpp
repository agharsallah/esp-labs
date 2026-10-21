// Sound lab S1 Hear.  uv run pio run -d labs/sound-lab-s1 -e sound-lab-s1 -t upload
// Meet the microphones: what they really send, and how differently two microphones hear the same clap.
//
//   KY-037 (analog)   AO is the microphone's voltage: a small wave on a steady bias, measured
//                     16 000 times a second by the ESP32's ADC with DMA. DO is the LM393
//                     comparator's yes/no against the trim pot.
//   INMP441 (digital) the microphone has its own amplifier and 24-bit converter inside, and sends
//                     numbers over I2S. No bias, no attenuation, no comparator: S1 shows its bits
//                     instead, and a comparator done in software.
//
// Wiring (module label -> ESP32-S3), either or both:
//   KY-037   + -> 3V3 - G -> GND - AO -> GPIO 6 - DO -> GPIO 21. Never 5 V on +: AO and DO would
//            then reach 5 V.
//   INMP441  VDD -> 3V3 - GND -> GND - L/R -> GND - SCK -> GPIO 40 - WS -> GPIO 41 - SD -> GPIO 42.
// `mic auto` (the default) uses the INMP441 when it answers. With both wired, the "mics" view runs
// the other one too, side by side. The screen from lesson 03 stays as it is; the push button on
// GPIO 5 is optional (it does what BOOT does).
//
// Views: scope · mics · bias · comparator · numbers   (click = next, double-click = the action)
// Serial: help lists the commands. Shared code: kit/lib/sound-kit.
#include <esp_heap_caps.h>

#include "sound_lab.h"

namespace {

// ---- State ------------------------------------------------------------------------------------

struct Scope {
  int spanMs = 20;        // how much time the screen shows
  bool trigger = true;    // line up each picture on a rising zero crossing, so the wave stands still
  bool frozen = false;
  int16_t shown[2048];    // the samples on screen
  int count = 0;
  float scale = 256;
} scope;

// The comparator strip: one slot per screen frame (~40 ms), the last ~6 s.
constexpr int kSlots = 150;
struct Strip {
  uint8_t loud[kSlots] = {};  // the loudest frame in the slot, dB above the quiet
  bool high[kSlots] = {};     // the comparator was high, or rose, during the slot
  int head = 0;
  float slotPeak = 0;
  uint32_t risesSeen = 0;
  uint32_t risesPerSec = 0, risesAt = 0, risesThen = 0;
} strip;

// With the INMP441 there is no DO pin: the same yes/no, computed from the level.
struct SoftComparator {
  float threshDb = 15;  // dB above the quiet
  bool high = false;
  uint32_t rises = 0;
} soft;

// The other microphone, for the "mics" view: started at set-up when it is there.
snd::Mic other;
snd::Level otherLevel;
bool otherOn = false;
uint32_t otherMs = 0;
struct Compare {
  int16_t a[320], b[320];       // 20 ms of each: a = the main mic, b = the other
  float scaleA = 256, scaleB = 256;
  float holdA = 0, holdB = 0;   // the loudest "above the quiet" lately, falling 10 dB/s
} cmp;

bool comparatorHigh(SoundLab &lab) { return lab.mic().digital() ? soft.high : lab.mic().doHigh(); }
uint32_t comparatorRises(SoundLab &lab) { return lab.mic().digital() ? soft.rises : lab.mic().doRises(); }

// ---- Views ------------------------------------------------------------------------------------

void takeScope(SoundLab &lab) {
  if (scope.frozen) return;
  const uint32_t rate = lab.mic().rate();
  const int span = std::min<int>(int(rate * scope.spanMs / 1000), 2048);
  static int16_t look[4096];
  const int lookN = span * 2;
  if (!lab.mic().latest(look, lookN)) return;
  int start = lookN - span;  // no trigger: simply the newest samples
  if (scope.trigger) {
    // Search back from the newest possible start for a rising crossing through 0 with some swing.
    const int16_t hyst = int16_t(std::max(32.0f, scope.scale * 0.08f));
    for (int i = lookN - span; i > 1; i--)
      if (look[i - 1] < 0 && look[i] >= 0 && look[std::max(0, i - 8)] < -hyst) {
        start = i;
        break;
      }
  }
  memcpy(scope.shown, look + start, span * sizeof(int16_t));
  scope.count = span;
  // Scale: follow the wave up at once, back down slowly, so quiet moments don't jitter.
  const float want = sv::autoScale(scope.shown, span, 128);
  scope.scale = want > scope.scale ? want : scope.scale * 0.95f + want * 0.05f;
}

void drawScope(SoundLab &lab, FrameBuffer &fb, float) {
  takeScope(lab);
  const int x = 30, y = sv::kTop + 6, w = 282, h = 108;
  fb.drawRect(x - 1, y - 1, w + 2, h + 2, pal::dim);
  for (int i = 1; i < 10; i++)
    for (int j = y; j < y + h; j += 4) fb.drawPixel(x + i * w / 10, j, pal::dim);
  sv::wave(fb, x, y, w, h, scope.shown, scope.count, scope.scale, sv::kSound);
  char text[48];
  if (lab.mic().digital()) {
    // The vertical scale as a share of the INMP441's full scale: our int16 = its 24 bits / 64,
    // so 32768 here is a quarter of what the microphone could send.
    const float pct = scope.scale / 32768.0f * 25.0f;
    snprintf(text, sizeof(text), "+%.2g", pct);
    sv::small(fb, 2, y, pal::muted, text);
    snprintf(text, sizeof(text), "-%.2g", pct);
    sv::small(fb, 2, y + h - 8, pal::muted, text);
    sv::small(fb, 2, y + h - 18, pal::muted, "%FS");
  } else {
    // The vertical scale in millivolts at the pin: int16 units / 16 = ADC steps.
    const int mv = lab.mic().toMv(int(lab.mic().biasRaw() + scope.scale / 16)) - lab.mic().biasMv();
    snprintf(text, sizeof(text), "+%d", mv);
    sv::small(fb, 2, y, pal::muted, text);
    snprintf(text, sizeof(text), "-%d", mv);
    sv::small(fb, 2, y + h - 8, pal::muted, text);
    sv::small(fb, 2, y + h - 18, pal::muted, "mV");
  }
  sv::small(fb, 2, y + h / 2 - 3, pal::muted, "0");
  snprintf(text, sizeof(text), "%d ms across - %s%s", scope.spanMs, scope.trigger ? "trigger on" : "free-running", scope.frozen ? " - FROZEN" : "");
  sv::small(fb, x, y + h + 6, scope.frozen ? pal::amber : pal::muted, text);
  snprintf(text, sizeof(text), "%s %.0f dB", snd::sourceShort(lab.mic().source()), lab.level().above());
  sv::small(fb, x + w - sv::textWidth(fb, text), y + h + 6, sv::levelColour(lab.level().above()), text);
}

// One microphone's lane on the mics view: its name, its quiet, how loud it hears it now, its wave.
void lane(FrameBuffer &fb, int y, const char *name, const snd::LevelReading &r, float hold, const int16_t *s, float &scale) {
  const int x = 92, w = 222, h = 44;
  fb.drawRect(x - 1, y - 1, w + 2, h + 2, pal::dim);
  sv::bold(fb, 4, y + 14, sv::kSound, name);
  const float want = sv::autoScale(s, 320, 128);
  scale = want > scale ? want : scale * 0.9f + want * 0.1f;
  sv::wave(fb, x, y, w, h, s, 320, scale, sv::kSound);
  char text[32];
  snprintf(text, sizeof(text), "quiet %.0f dBFS", r.floorDb);
  sv::small(fb, 4, y + 22, pal::muted, text);
  snprintf(text, sizeof(text), "now +%.0f dB", hold);
  sv::small(fb, 4, y + 33, sv::levelColour(hold), text);
}

void drawMics(SoundLab &lab, FrameBuffer &fb, float) {
  const bool primaryDigital = lab.mic().digital();
  if (!otherOn) {
    sv::bold(fb, 8, sv::kTop + 20, pal::ink, "Only one microphone found.");
    if (primaryDigital) {
      sv::small(fb, 8, sv::kTop + 34, pal::muted, "Wire a KY-037 too, to compare:");
      sv::small(fb, 8, sv::kTop + 48, pal::ink, "+ -> 3V3   G -> GND   AO -> GPIO 6   DO -> 21");
    } else {
      sv::small(fb, 8, sv::kTop + 34, pal::muted, "Wire an INMP441 too, to compare:");
      sv::small(fb, 8, sv::kTop + 48, pal::ink, "VDD -> 3V3   GND -> GND   L/R -> GND");
      sv::small(fb, 8, sv::kTop + 60, pal::ink, "SCK -> 40   WS -> 41   SD -> 42");
    }
    sv::small(fb, 8, sv::kTop + 80, pal::muted, "then press RST: S1 looks for it at start-up.");
    return;
  }
  // The INMP441 always on top, so the comparison reads the same whichever is the main mic.
  lab.mic().latest(cmp.a, 320);
  other.latest(cmp.b, 320);
  const snd::LevelReading &main = lab.level(), &second = otherLevel.reading();
  const float holdDigital = primaryDigital ? cmp.holdA : cmp.holdB, holdAnalog = primaryDigital ? cmp.holdB : cmp.holdA;
  if (primaryDigital) {
    lane(fb, sv::kTop + 4, "INMP441", main, cmp.holdA, cmp.a, cmp.scaleA);
    lane(fb, sv::kTop + 54, "KY-037", second, cmp.holdB, cmp.b, cmp.scaleB);
  } else {
    lane(fb, sv::kTop + 4, "INMP441", second, cmp.holdB, cmp.b, cmp.scaleB);
    lane(fb, sv::kTop + 54, "KY-037", main, cmp.holdA, cmp.a, cmp.scaleA);
  }
  // The same sound, heard by both: how far above its own hiss did each one hear it?
  char text[56];
  const bool heard = holdDigital >= 6 || holdAnalog >= 6;
  if (!heard) snprintf(text, sizeof(text), "clap or talk: how far does each rise above its hiss?");
  else if (holdDigital >= holdAnalog) snprintf(text, sizeof(text), "INMP441 hears it %.0f dB more clearly", holdDigital - holdAnalog);
  else snprintf(text, sizeof(text), "KY-037 hears it %.0f dB more clearly (closer?)", holdAnalog - holdDigital);
  sv::small(fb, 4, sv::kTop + 108, heard ? pal::ink : pal::muted, text);
  sv::small(fb, 4, sv::kTop + 120, pal::muted, "each wave scaled to fit: compare the hiss");
}

// KY-037: a 0-3.3 V ruler with the bias, the wave's swing and the ADC's range.
void drawBiasAnalog(SoundLab &lab, FrameBuffer &fb) {
  snd::Mic &m = lab.mic();
  const int x = 22, y = sv::kTop + 8, h = 112, w = 46;
  auto yOf = [&](int mv) { return y + h - mv * h / 3300; };
  fb.drawRect(x, y, w, h, pal::dim);
  const int top = snd::attenTopMv(m.atten());
  fb.fillRect(x + 1, yOf(top), w - 2, y + h - yOf(top) - 1, sv::kPanel);
  const int lo = m.toMv(m.rawLow()), hi = m.toMv(m.rawHigh()), bias = m.biasMv();
  fb.fillRect(x + 6, yOf(hi), w - 12, std::max(2, yOf(lo) - yOf(hi)), mix(sv::kSound, pal::night, 120));
  fb.drawFastHLine(x - 4, yOf(bias), w + 8, sv::kSound);
  for (int v = 0; v <= 3300; v += 500) {
    fb.drawFastHLine(x - 3, yOf(v), 3, pal::muted);
    char t[8];
    snprintf(t, sizeof(t), "%.1f", v / 1000.0f);
    if (v % 1000 == 0) sv::small(fb, 0, yOf(v) - 3, pal::muted, t);
  }
  char text[48];
  snprintf(text, sizeof(text), "%d mV", bias);
  sv::stat(fb, 92, sv::kTop + 10, "bias (the steady part)", text, sv::kSound);
  snprintf(text, sizeof(text), "%d mV", std::max(0, hi - lo));
  sv::stat(fb, 92, sv::kTop + 48, "swing, last 0.1 s", text);
  const float mvPerStep = top / 4095.0f;
  snprintf(text, sizeof(text), "%.2f mV", mvPerStep);
  sv::stat(fb, 212, sv::kTop + 48, "one ADC step", text);
  snprintf(text, sizeof(text), "atten %s", snd::attenName(m.atten()));
  sv::small(fb, 212, sv::kTop + 10, pal::muted, text);
  // The advice: clipping first, then whether a smaller range would fit.
  const char *hint;
  uint16_t c = pal::muted;
  if (m.clipping()) hint = "CLIPPING: the wave hits the range's edge", c = pal::coral;
  else if (bias > top - 150) hint = "bias near the top: use a bigger atten", c = pal::amber;
  else if (m.atten() == ADC_ATTEN_DB_12 && hi < 1600) hint = "fits in 0-1.75 V: try atten 6 (2x finer)", c = pal::teal;
  else if (m.atten() == ADC_ATTEN_DB_6 && hi < 1150) hint = "fits in 0-1.25 V: try atten 2.5", c = pal::teal;
  else hint = "the wave fits the range: good";
  sv::small(fb, 92, sv::kTop + 92, c, hint);
  sv::small(fb, 92, sv::kTop + 106, pal::muted, "clap: the band grows around the line");
}

// INMP441: no bias, no volts. A ladder of its 24 bits: how many the sound actually uses.
void drawBiasDigital(SoundLab &lab, FrameBuffer &fb) {
  snd::Mic &m = lab.mic();
  const int lo = m.rawLow(), hi = m.rawHigh();
  const int64_t swing = int64_t(hi) - lo;
  const float bits = swing > 0 ? log2f(float(swing)) : 0;  // the wave spans about 2^bits steps
  const int biggest = std::max(std::abs(lo), std::abs(hi));
  const float headroom = biggest > 0 ? 20.0f * log10f(float(m.rawMax()) / float(biggest)) : 138;
  // The ladder: bit 0 at the bottom, bit 23 (the sign) at the top. Lit = in use.
  const int x = 14, y = sv::kTop + 6, cell = 5;
  for (int b = 0; b < 24; b++) {
    const int cy = y + (23 - b) * cell;
    const bool used = b < int(bits + 0.5f);
    const uint16_t c = b >= 22 ? pal::coral : b >= 18 ? pal::amber : sv::kSound;
    fb.fillRect(x, cy, 34, cell - 1, used ? c : sv::kPanel);
  }
  sv::small(fb, x + 38, y - 2, pal::muted, "bit 23");
  sv::small(fb, x + 38, y + 23 * cell - 2, pal::muted, "bit 0");
  char text[48];
  snprintf(text, sizeof(text), "%.1f of 24", bits);
  sv::stat(fb, 104, sv::kTop + 8, "bits the sound uses", text, sv::kSound);
  snprintf(text, sizeof(text), "%.0f dB", headroom);
  sv::stat(fb, 216, sv::kTop + 8, "headroom", text, headroom < 6 ? pal::coral : pal::ink);
  snprintf(text, sizeof(text), "%d", int(m.biasRaw()));
  sv::stat(fb, 104, sv::kTop + 46, "offset (24-bit)", text);
  snprintf(text, sizeof(text), "%d", int(swing));
  sv::stat(fb, 216, sv::kTop + 46, "swing, last 0.1 s", text);
  const char *hint = "digital: no bias, the converter is inside";
  uint16_t c = pal::muted;
  if (m.clipping()) hint = "CLIPPING: louder than the mic can count", c = pal::coral;
  else if (bits > 6) hint = "each bit = 6 dB: a clap lights a dozen more", c = pal::teal;
  sv::small(fb, 104, sv::kTop + 92, c, hint);
  sv::small(fb, 104, sv::kTop + 106, pal::muted, "nothing to tune: no attenuation, no pot");
}

void drawBias(SoundLab &lab, FrameBuffer &fb, float) {
  if (lab.mic().digital()) drawBiasDigital(lab, fb);
  else drawBiasAnalog(lab, fb);
}

void drawComparator(SoundLab &lab, FrameBuffer &fb, float) {
  const bool digital = lab.mic().digital();
  // The lamp: what the comparator says right now (the KY-037's own LED2 does the same).
  const bool high = comparatorHigh(lab);
  fb.fillCircle(34, sv::kTop + 30, 18, high ? pal::amber : sv::kPanel);
  fb.drawCircle(34, sv::kTop + 30, 18, pal::muted);
  sv::small(fb, 14, sv::kTop + 54, pal::muted, digital ? (high ? "soft high" : "soft low") : (high ? "DO high" : "DO low"));
  char text[48];
  snprintf(text, sizeof(text), "%lu/s", static_cast<unsigned long>(strip.risesPerSec));
  sv::stat(fb, 68, sv::kTop + 8, "rising edges", text, strip.risesPerSec ? pal::amber : pal::ink);
  if (digital) snprintf(text, sizeof(text), "%.0f dB", soft.threshDb);
  else snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(comparatorRises(lab)));
  sv::stat(fb, 168, sv::kTop + 8, digital ? "threshold" : "total", text);
  // The strip: the loudness above, the comparator below, newest on the right. Lit exactly when it's loud?
  const int x = 10, y = sv::kTop + 66, w = 300;
  for (int i = 0; i < kSlots; i++) {
    const int at = (strip.head + i) % kSlots;
    const int px = x + i * w / kSlots;
    const int h = std::min(30, int(strip.loud[at]) * 30 / 50);
    if (h) fb.fillRect(px, y + 30 - h, 2, h, sv::levelColour(strip.loud[at]));
    if (strip.high[at]) fb.fillRect(px, y + 36, 2, 10, pal::amber);
  }
  if (digital) {  // the software threshold, drawn on the loudness half
    const int ty = y + 30 - std::min(30, int(soft.threshDb) * 30 / 50);
    for (int i = x; i < x + w; i += 3) fb.drawPixel(i, ty, pal::ink);
  }
  fb.drawFastHLine(x, y + 31, w, pal::dim);
  fb.drawFastHLine(x, y + 47, w, pal::dim);
  sv::small(fb, x, y + 50, pal::muted, digital ? "loudness vs the software comparator, 6 s" : "loudness (top) vs DO (bottom), last 6 s");
  // Tuning advice from the last two seconds.
  int highSlots = 0, loudSlots = 0;
  for (int i = kSlots - 50; i < kSlots; i++) {
    const int at = (strip.head + i) % kSlots;
    highSlots += strip.high[at];
    loudSlots += strip.loud[at] >= 15;
  }
  const char *hint = digital ? "INMP441: no DO pin, so this one is code" : "clap: DO should light with each clap";
  uint16_t c = pal::muted;
  if (highSlots > 45) hint = digital ? "always on: raise it (thresh 25)" : "DO always on: turn the pot the other way", c = pal::amber;
  else if (loudSlots > 3 && highSlots == 0) hint = digital ? "loud but off: lower it (thresh 10)" : "loud but DO stays off: turn the pot back", c = pal::amber;
  else if (highSlots > 0 && highSlots <= loudSlots + 2) hint = digital ? "follows the loud moments: tuned" : "DO follows the loud moments: tuned", c = pal::teal;
  sv::small(fb, 68, sv::kTop + 42, c, hint);
}

void drawNumbers(SoundLab &lab, FrameBuffer &fb, float) {
  snd::Mic &m = lab.mic();
  const snd::LevelReading &r = lab.level();
  char a[24], b[48], c[24], d[24];
  snprintf(a, sizeof(a), "%lu Hz", static_cast<unsigned long>(m.measuredRate()));
  sv::stat(fb, 10, sv::kTop + 6, "microphone", snd::sourceShort(m.source()), sv::kSound);
  sv::stat(fb, 170, sv::kTop + 6, "samples / second", a);
  snprintf(c, sizeof(c), "%.1f dBFS", r.fastDb);
  snprintf(d, sizeof(d), "%.1f dBFS", r.floorDb);
  sv::stat(fb, 10, sv::kTop + 46, "level (fast)", c);
  sv::stat(fb, 170, sv::kTop + 46, "quiet (floor)", d);
  snprintf(a, sizeof(a), "%.1f dB", r.above());
  sv::stat(fb, 10, sv::kTop + 86, "above the quiet", a, sv::levelColour(r.above()));
  if (m.digital()) {
    snprintf(b, sizeof(b), "%.0f dB SPL", lab.spl());
    sv::stat(fb, 170, sv::kTop + 86, "sound level (unweighted)", b);
  } else {
    snprintf(b, sizeof(b), "%u KB", unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    sv::stat(fb, 170, sv::kTop + 86, "free PSRAM", b);
  }
  snprintf(b, sizeof(b), "lost: DMA %lu, frames %lu", static_cast<unsigned long>(m.overruns()), static_cast<unsigned long>(m.framesDropped()));
  sv::small(fb, 10, sv::kTop + 120, m.overruns() || m.framesDropped() ? pal::amber : pal::muted, b);
}

const SoundView kViews[] = {
    {"scope", drawScope, "freeze"},
    {"mics", drawMics, "status"},
    {"bias", drawBias, "atten next"},
    {"comparator", drawComparator, "pot"},
    {"numbers", drawNumbers, "status"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdFreeze(SoundLab &lab, const String &, const String &lower) {
  scope.frozen = lower.isEmpty() ? !scope.frozen : lower != "off";
  lab.note(sv::Note::Info, scope.frozen ? "scope frozen (freeze again to go on)" : "scope running");
}

void cmdSpan(SoundLab &lab, const String &arg, const String &) {
  long ms = 0;
  if (!SoundLab::number(arg, 2, 120, ms)) return lab.warn("span <ms>, 2-120 (now %d). 20 ms = one cycle of 50 Hz", scope.spanMs);
  scope.spanMs = int(ms);
  scope.frozen = false;
}

void cmdTrigger(SoundLab &lab, const String &, const String &lower) {
  scope.trigger = lower.isEmpty() ? !scope.trigger : lower == "on";
  lab.note(sv::Note::Info, scope.trigger ? "trigger on: a steady tone stands still" : "free-running");
}

void cmdPot(SoundLab &lab, const String &, const String &) {
  if (lab.mic().digital()) {
    Serial.println("The INMP441 has no comparator and no pot: S1 compares in software instead.");
    Serial.printf("  thresh <dB> sets the line (now %.0f dB above the quiet). Same idea: quiet = off, clap = on.\n", soft.threshDb);
    Serial.println("  To tune a KY-037's pot, make it the main microphone: mic ky037 (restarts).");
  } else {
    Serial.println("Tuning the comparator (the blue trim pot, a small screwdriver):");
    Serial.println("  1. Stay quiet. If the module's second LED (and DO) is on, turn the screw until it just goes off.");
    Serial.println("  2. Clap. DO should flash with each clap: the comparator view shows DO under the loudness.");
    Serial.println("  3. Lit all the time: turn it back a little. Never lights: turn it further toward the edge.");
    Serial.println("  Which way is 'more sensitive' differs between module makers: watch the lamp, not the direction.");
  }
  lab.showView("comparator");
}

void cmdThresh(SoundLab &lab, const String &arg, const String &) {
  float db = 0;
  if (!SoundLab::decimal(arg, 3, 50, db))
    return lab.warn("thresh <dB>, 3-50: the software comparator's line above the quiet (now %.0f)", soft.threshDb);
  soft.threshDb = db;
  lab.note(sv::Note::Info, "software comparator at %.0f dB", db);
  if (!lab.mic().digital()) Serial.println("  (only used with the INMP441: the KY-037 has its hardware comparator on DO)");
}

void cmdDump(SoundLab &lab, const String &arg, const String &) {
  long n = 0;
  if (arg.isEmpty()) n = 320;
  else if (!SoundLab::number(arg, 16, 4096, n)) return lab.warn("dump [n], 16-4096 samples (default 320 = 20 ms)");
  static int16_t s[4096];
  if (!lab.mic().latest(s, n)) return lab.warn("not enough sound yet");
  Serial.printf("# %ld samples at %lu Hz, int16 after removing the %s (%s). Paste into a spreadsheet.\n", n,
                static_cast<unsigned long>(lab.mic().rate()), lab.mic().digital() ? "offset" : "bias",
                lab.mic().digital() ? "one unit = 64 of the INMP441's 24-bit steps" : "one ADC step = 16");
  for (long i = 0; i < n; i++) Serial.printf("%d%c", s[i], (i % 16 == 15 || i == n - 1) ? '\n' : ',');
}

const SoundCommand kCommands[] = {
    {"freeze", "[off]", "hold the scope picture (double-click on scope)", cmdFreeze},
    {"span", "<ms>", "time across the scope, 2-120 ms (default 20)", cmdSpan},
    {"trigger", "[on|off]", "line the wave up on a rising zero crossing", cmdTrigger},
    {"pot", "", "how to tune the comparator (trim pot, or thresh)", cmdPot},
    {"thresh", "<dB>", "the INMP441's software comparator line (default 15)", cmdThresh},
    {"dump", "[n]", "print the newest n samples as numbers", cmdDump},
};

// ---- Hooks ------------------------------------------------------------------------------------

// The other microphone: a KY-037 next to the INMP441 is always started (an unwired GPIO 6 just
// shows noise); an INMP441 next to the KY-037 only when it answers the probe. The probe sets the
// I2S pins up and frees them again (~0.35 s), before anything else uses them: fine at set-up.
void setup(SoundLab &lab) {
  if (lab.mic().digital()) otherOn = other.begin(snd::Source::Ky037);
  else if (snd::Mic::probeInmp441()) otherOn = other.begin(snd::Source::Inmp441);
  Serial.printf("Second microphone for the mics view: %s\n", otherOn ? snd::sourceName(other.source()) : "none found");
}

void frame(SoundLab &lab, const snd::Frame &, uint32_t) {
  const float above = lab.level().instAbove();
  strip.slotPeak = std::max(strip.slotPeak, above);
  // The software comparator: on above the line, off 3 dB under it (hysteresis, like the LM393's).
  if (!soft.high && above >= soft.threshDb) soft.high = true, soft.rises++;
  else if (soft.high && above < soft.threshDb - 3) soft.high = false;
  cmp.holdA = std::max(cmp.holdA - 10.0f * 0.004f, above);  // falls 10 dB/s
}

void tick(SoundLab &lab) {
  const uint32_t rises = comparatorRises(lab);
  strip.loud[strip.head] = uint8_t(std::min(255.0f, strip.slotPeak));
  strip.high[strip.head] = comparatorHigh(lab) || rises != strip.risesSeen;
  strip.head = (strip.head + 1) % kSlots;
  strip.risesSeen = rises;
  strip.slotPeak = 0;
  if (millis() - strip.risesAt >= 1000) strip.risesPerSec = rises - strip.risesThen, strip.risesThen = rises, strip.risesAt = millis();
  // The other microphone's frames: its own level meter, its own quiet.
  if (otherOn) {
    snd::Frame f;
    while (other.frame(f)) {
      otherMs += snd::kChunk * 1000 / other.rate();
      otherLevel.push(f.rms, otherMs);
      cmp.holdB = std::max(cmp.holdB - 10.0f * 0.004f, otherLevel.reading().instAbove());
    }
  }
}

// The bias view's double-click: the next attenuation, so you can watch the ruler change.
bool gesture(SoundLab &lab, Gesture g, bool) {
  if (g != Gesture::DoubleClick || strcmp(lab.spec().views[lab.view()].name, "bias") != 0) return false;
  if (lab.mic().digital()) {
    lab.note(sv::Note::Info, "INMP441: fixed 24-bit range, nothing to switch");
    return true;
  }
  const adc_atten_t a = lab.mic().atten();
  const char *next = a == ADC_ATTEN_DB_12 ? "6" : a == ADC_ATTEN_DB_6 ? "2.5" : a == ADC_ATTEN_DB_2_5 ? "0" : "12";
  Serial.printf("> atten %s   (double-click)\n", next);
  lab.runLine(String("atten ") + next);
  return true;
}

const SoundLabSpec kSpec{
    "sound-lab-s1", "S1", "Hear", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, frame, tick, gesture, "freeze",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
