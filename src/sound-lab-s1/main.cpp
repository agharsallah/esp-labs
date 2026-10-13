// Sound lab S1 Hear.  uv run pio run -e sound-lab-s1 -t upload
// Meet the KY-037: what its two outputs really send. AO is the microphone's voltage, sampled
// 16 000 times a second by the ADC's DMA; DO is the LM393 comparator's yes/no against the trim pot.
//
// Wiring (module label -> ESP32-S3): + -> 3V3 · G -> GND · AO -> GPIO 6 · DO -> GPIO 21.
// Never 5 V on +: AO and DO would then reach 5 V. The screen from lesson 03 stays as it is; the
// push button on GPIO 5 is optional (it does what BOOT does).
//
// Views: scope · bias · comparator · numbers   (click = next, double-click = the action in the footer)
// Serial: help lists the commands. Shared code: lib/sound-kit.
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
  bool high[kSlots] = {};     // DO was high, or rose, during the slot
  int head = 0;
  float slotPeak = 0;
  uint32_t risesSeen = 0;
  uint32_t risesPerSec = 0, risesAt = 0, risesThen = 0;
} strip;

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
  // The vertical scale in millivolts at the pin: int16 units / 16 = ADC steps.
  const int mv = lab.mic().toMv(int(lab.mic().biasRaw() + scope.scale / 16)) - lab.mic().biasMv();
  char text[40];
  snprintf(text, sizeof(text), "+%d", mv);
  sv::small(fb, 2, y, pal::muted, text);
  sv::small(fb, 2, y + h / 2 - 3, pal::muted, "0");
  snprintf(text, sizeof(text), "-%d", mv);
  sv::small(fb, 2, y + h - 8, pal::muted, text);
  sv::small(fb, 2, y + h - 18, pal::muted, "mV");
  snprintf(text, sizeof(text), "%d ms across - %s%s", scope.spanMs, scope.trigger ? "trigger on" : "free-running", scope.frozen ? " - FROZEN" : "");
  sv::small(fb, x, y + h + 6, scope.frozen ? pal::amber : pal::muted, text);
  snprintf(text, sizeof(text), "%.0f dB", lab.level().above());
  sv::small(fb, x + w - sv::textWidth(fb, text), y + h + 6, sv::levelColour(lab.level().above()), text);
}

void drawBias(SoundLab &lab, FrameBuffer &fb, float) {
  snd::Mic &m = lab.mic();
  // A vertical 0-3.3 V ruler: the ADC's range shaded, the bias as a line, the wave's swing as a band.
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
  if (m.rawLow() <= 0 || m.rawHigh() >= 4095) hint = "CLIPPING: the wave hits the range's edge", c = pal::coral;
  else if (bias > top - 150) hint = "bias near the top: use a bigger atten", c = pal::amber;
  else if (m.atten() == ADC_ATTEN_DB_12 && hi < 1600) hint = "fits in 0-1.75 V: try atten 6 (2x finer)", c = pal::teal;
  else if (m.atten() == ADC_ATTEN_DB_6 && hi < 1150) hint = "fits in 0-1.25 V: try atten 2.5", c = pal::teal;
  else hint = "the wave fits the range: good";
  sv::small(fb, 92, sv::kTop + 92, c, hint);
  sv::small(fb, 92, sv::kTop + 106, pal::muted, "clap: the band grows around the line");
}

void drawComparator(SoundLab &lab, FrameBuffer &fb, float t) {
  snd::Mic &m = lab.mic();
  // The lamp: what DO says right now (the module's own LED2 does the same).
  const bool high = m.doHigh();
  fb.fillCircle(34, sv::kTop + 30, 18, high ? pal::amber : sv::kPanel);
  fb.drawCircle(34, sv::kTop + 30, 18, pal::muted);
  sv::small(fb, 18, sv::kTop + 54, pal::muted, high ? "DO high" : "DO low");
  char text[40];
  snprintf(text, sizeof(text), "%lu/s", static_cast<unsigned long>(strip.risesPerSec));
  sv::stat(fb, 68, sv::kTop + 8, "rising edges", text, strip.risesPerSec ? pal::amber : pal::ink);
  snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(m.doRises()));
  sv::stat(fb, 168, sv::kTop + 8, "total", text);
  // The strip: the loudness above, DO below, newest on the right. Is DO lit exactly when it's loud?
  const int x = 10, y = sv::kTop + 66, w = 300;
  for (int i = 0; i < kSlots; i++) {
    const int at = (strip.head + i) % kSlots;
    const int px = x + i * w / kSlots;
    const int h = std::min(30, int(strip.loud[at]) * 30 / 50);
    if (h) fb.fillRect(px, y + 30 - h, 2, h, sv::levelColour(strip.loud[at]));
    if (strip.high[at]) fb.fillRect(px, y + 36, 2, 10, pal::amber);
  }
  fb.drawFastHLine(x, y + 31, w, pal::dim);
  fb.drawFastHLine(x, y + 47, w, pal::dim);
  sv::small(fb, x, y + 50, pal::muted, "loudness (top) vs DO (bottom), last 6 s");
  // Tuning advice from the last two seconds.
  int highSlots = 0, loudSlots = 0;
  for (int i = kSlots - 50; i < kSlots; i++) {
    const int at = (strip.head + i) % kSlots;
    highSlots += strip.high[at];
    loudSlots += strip.loud[at] >= 15;
  }
  const char *hint = "clap: DO should light with each clap";
  uint16_t c = pal::muted;
  if (highSlots > 45) hint = "DO always on: turn the pot the other way", c = pal::amber;
  else if (loudSlots > 3 && highSlots == 0) hint = "loud but DO stays off: turn the pot back", c = pal::amber;
  else if (highSlots > 0 && highSlots <= loudSlots + 2) hint = "DO follows the loud moments: tuned", c = pal::teal;
  sv::small(fb, 68, sv::kTop + 42, c, hint);
  (void)t;
}

void drawNumbers(SoundLab &lab, FrameBuffer &fb, float) {
  snd::Mic &m = lab.mic();
  const snd::LevelReading &r = lab.level();
  char a[24], b[24], c[24], d[24];
  snprintf(a, sizeof(a), "%lu Hz", static_cast<unsigned long>(m.measuredRate()));
  snprintf(b, sizeof(b), "%lu / %lu", static_cast<unsigned long>(m.overruns()), static_cast<unsigned long>(m.framesDropped()));
  snprintf(c, sizeof(c), "%.1f dBFS", r.fastDb);
  snprintf(d, sizeof(d), "%.1f dBFS", r.floorDb);
  sv::stat(fb, 10, sv::kTop + 6, "samples / second", a, sv::kSound);
  sv::stat(fb, 170, sv::kTop + 6, "lost: DMA / frames", b, m.overruns() || m.framesDropped() ? pal::amber : pal::ink);
  sv::stat(fb, 10, sv::kTop + 46, "level (fast)", c);
  sv::stat(fb, 170, sv::kTop + 46, "quiet (floor)", d);
  snprintf(a, sizeof(a), "%.1f dB", r.above());
  snprintf(b, sizeof(b), "%u KB", unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  sv::stat(fb, 10, sv::kTop + 86, "above the quiet", a, sv::levelColour(r.above()));
  sv::stat(fb, 170, sv::kTop + 86, "free PSRAM", b);
}

const SoundView kViews[] = {
    {"scope", drawScope, "freeze"},
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
  Serial.println("Tuning the comparator (the blue trim pot, a small screwdriver):");
  Serial.println("  1. Stay quiet. If the module's second LED (and DO) is on, turn the screw until it just goes off.");
  Serial.println("  2. Clap. DO should flash with each clap: the comparator view shows DO under the loudness.");
  Serial.println("  3. Lit all the time: turn it back a little. Never lights: turn it further toward the edge.");
  Serial.println("  Which way is 'more sensitive' differs between module makers: watch the lamp, not the direction.");
  lab.showView("comparator");
}

void cmdDump(SoundLab &lab, const String &arg, const String &) {
  long n = 0;
  if (arg.isEmpty()) n = 320;
  else if (!SoundLab::number(arg, 16, 4096, n)) return lab.warn("dump [n], 16-4096 samples (default 320 = 20 ms)");
  static int16_t s[4096];
  if (!lab.mic().latest(s, n)) return lab.warn("not enough sound yet");
  Serial.printf("# %ld samples at %lu Hz, int16 after removing the bias (one ADC step = 16). Paste into a spreadsheet.\n", n,
                static_cast<unsigned long>(lab.mic().rate()));
  for (long i = 0; i < n; i++) Serial.printf("%d%c", s[i], (i % 16 == 15 || i == n - 1) ? '\n' : ',');
}

const SoundCommand kCommands[] = {
    {"freeze", "[off]", "hold the scope picture (double-click on scope)", cmdFreeze},
    {"span", "<ms>", "time across the scope, 2-120 ms (default 20)", cmdSpan},
    {"trigger", "[on|off]", "line the wave up on a rising zero crossing", cmdTrigger},
    {"pot", "", "how to tune the comparator's trim pot", cmdPot},
    {"dump", "[n]", "print the newest n samples as numbers", cmdDump},
};

// ---- Hooks ------------------------------------------------------------------------------------

void frame(SoundLab &lab, const snd::Frame &, uint32_t) { strip.slotPeak = std::max(strip.slotPeak, lab.level().instAbove()); }

void tick(SoundLab &lab) {
  snd::Mic &m = lab.mic();
  const uint32_t rises = m.doRises();
  strip.loud[strip.head] = uint8_t(std::min(255.0f, strip.slotPeak));
  strip.high[strip.head] = m.doHigh() || rises != strip.risesSeen;
  strip.head = (strip.head + 1) % kSlots;
  strip.risesSeen = rises;
  strip.slotPeak = 0;
  if (millis() - strip.risesAt >= 1000) strip.risesPerSec = rises - strip.risesThen, strip.risesThen = rises, strip.risesAt = millis();
}

// The bias view's double-click: the next attenuation, so you can watch the ruler change.
bool gesture(SoundLab &lab, Gesture g, bool) {
  if (g != Gesture::DoubleClick || strcmp(lab.spec().views[lab.view()].name, "bias") != 0) return false;
  const adc_atten_t a = lab.mic().atten();
  const char *next = a == ADC_ATTEN_DB_12 ? "6" : a == ADC_ATTEN_DB_6 ? "2.5" : a == ADC_ATTEN_DB_2_5 ? "0" : "12";
  Serial.printf("> atten %s   (double-click)\n", next);
  lab.runLine(String("atten ") + next);
  return true;
}

const SoundLabSpec kSpec{
    "sound-lab-s1", "S1", "Hear", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), nullptr, frame, tick, gesture, "freeze",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
