// Sound lab S5 Spectrum.  uv run pio run -d labs/sound-lab-s5 -e sound-lab-s5 -t upload
// Which frequencies are in the sound? Our own FFT (kit/lib/sound-kit/spectrum.h) runs on the newest
// 512 samples every screen frame (~25 times a second; each picture shares most of its samples with
// the one before: that overlap keeps the bars smooth). Then: bands, a waterfall, a tuner, and a
// game you steer by whistling.
//
// Honest limits, worth knowing before you whistle:
//   - The KY-037 has no amplifier: quiet sounds sit close to the hiss. Whistle or hum near the mic.
//     The INMP441 (24-bit, its own amplifier) hears far more: the tuner locks on sooner and the
//     waterfall shows voices and music cleanly, not only whistles.
//   - Resolution = rate / N: 16000 / 512 = 31 Hz between bins. Finer (fft 1024) = slower answers.
//   - Nyquist: sampling at 16 kHz can only show up to 8 kHz. Higher sounds fold back as fakes.
//
// The speaker: `ref` plays a reference note (the tuner should read it at 0 cents), `sweep` glides
// a tone from 100 Hz to 8 kHz so you watch it climb the bars and the waterfall. S5 reads the
// sample ring directly, not the shell's frames, so the echo guard doesn't apply here: on purpose,
// this lab is meant to hear its own speaker.
//
// Wiring: the screen and a microphone: the INMP441 (VDD -> 3V3, GND -> GND, L/R -> GND, SCK -> 40,
// WS -> 41, SD -> 42) or the KY-037 (+ -> 3V3, G -> GND, AO -> GPIO 6, DO -> GPIO 21). Optional:
// the MAX98357A + speaker (BCLK 39, LRC 38, DIN 47, VIN 5V); the push button on GPIO 5.
//
// Views: bars · waterfall · tuner · steer   (click = next, double-click = the action in the footer,
//        long press = hold)
// Serial: help lists the commands. Shared code: kit/lib/sound-kit.
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "pitch.h"
#include "sound_lab.h"
#include "spectrum.h"
#include "synth.h"

namespace {

constexpr float kLoHz = 100, kHiHz = 8000;  // the bars' and waterfall's range
constexpr int kBars = 24;                     // log-spaced bands for the bars
constexpr int kRows = 48;                     // finer bands for the waterfall
constexpr int kCols = 140;                    // waterfall history: one column per screen frame, ~6 s
constexpr int kTrail = 120;                   // tuner trail and steer ribbon length

snd::Fft fft;  // ~18 KB of tables and buffers: static, never on the stack
int16_t samples[snd::Fft::kMaxN];

struct State {
  bool hold = false;
  float a4 = 440;
  float lowHz = 600, highHz = 2500;  // the steer range
  uint32_t fftUs = 0;                // the last transform's time
  uint32_t lastTick = 0;
  bool fresh = false;  // a spectrum was computed this frame
  uint32_t refUntil = 0;  // millis() while the reference note plays
  char refName[8] = "";
  float refHz = 0;
} st;

// The sweep: 4 s of a sine gliding 100 Hz -> 8 kHz, made once into PSRAM (the speaker plays it
// from there, so it must stay alive).
constexpr uint32_t kSweepSamples = snd::Speaker::kRate * 4;
int16_t *sweep = nullptr;

s5::BandFloors<kBars> bars;
s5::BandFloors<kRows> rows;
float caps[kBars] = {};  // the falling peak caps, dB above the quiet

// The waterfall: kCols columns of kRows bytes (0-255 = 0-40 dB above the quiet), in PSRAM.
uint8_t *history = nullptr;
int histHead = 0;
uint16_t heat[64];  // colour map: night -> pink -> white

// The pitch the tuner and steer views follow.
struct Pitch {
  bool heard = false;  // a clear tone right now
  float hz = 0;
  float clarity = 0;
  uint32_t lastHeard = 0;
  float trail[kTrail] = {};  // midi (continuous semitones), 0 = nothing heard
  int head = 0;
} pitch;

struct Steer {
  float y = 0.5f;          // the dot, 0 = bottom, 1 = top
  float target = 0.6f;     // the ring's height
  uint32_t inSince = 0;    // when the dot entered the ring (0 = outside)
  int score = 0;
  float ribbon[kTrail] = {};
  int head = 0;
  bool ledOn = false;
  uint8_t ledHue = 0;
} steer;

// ---- The spectrum, every screen frame -----------------------------------------------------------

void analyse(SoundLab &lab) {
  st.fresh = false;
  if (st.hold) return;
  const int n = fft.size();
  if (!lab.mic().latest(samples, n)) return;
  const int64_t t0 = esp_timer_get_time();
  fft.run(samples);
  st.fftUs = uint32_t(esp_timer_get_time() - t0);
  const uint32_t now = millis();
  const float dt = st.lastTick ? (now - st.lastTick) / 1000.0f : 0.04f;
  st.lastTick = now;
  const float rate = lab.mic().rate();
  const float hi = std::min(kHiHz, rate / 2 - 1);  // never past Nyquist

  float b[kRows];
  fft.bands(rate, kLoHz, hi, b, kBars);
  bars.update(b, dt);
  for (int i = 0; i < kBars; i++) caps[i] = std::max(caps[i] - 30.0f * dt, bars.above(i));  // caps fall 30 dB/s

  fft.bands(rate, kLoHz, hi, b, kRows);
  rows.update(b, dt);
  if (history) {
    uint8_t *col = history + histHead * kRows;
    for (int r = 0; r < kRows; r++) col[r] = uint8_t(std::min(255.0f, rows.above(r) * 255.0f / 40.0f));
    histHead = (histHead + 1) % kCols;
  }

  // Pitch: the strongest bin from 80 Hz to 4 kHz, but only if it stands well clear of its
  // neighbours (a whistle or a hummed note does; speech and noise don't) and the room isn't quiet.
  const snd::Fft::Peak p = fft.peak(rate, 80, std::min(4000.0f, hi));
  pitch.heard = p.clarity >= 12.0f && lab.level().above() >= 8.0f;
  if (pitch.heard) pitch.hz = p.hz, pitch.clarity = p.clarity, pitch.lastHeard = now;
  pitch.trail[pitch.head] = pitch.heard ? s5::midiOf(p.hz, st.a4) : 0;
  pitch.head = (pitch.head + 1) % kTrail;
  st.fresh = true;
}

// ---- Views ------------------------------------------------------------------------------------

int xOfHz(float hz, int x, int w) { return x + int(logf(hz / kLoHz) / logf(kHiHz / kLoHz) * w); }

void freqAxis(FrameBuffer &fb, int x, int w, int y) {
  const struct {
    float hz;
    const char *label;
  } marks[] = {{100, "100"}, {250, "250"}, {1000, "1k"}, {4000, "4k"}, {8000, "8k"}};
  for (const auto &m : marks) {
    const int px = std::min(x + w - 1, xOfHz(m.hz, x, w));
    fb.drawFastVLine(px, y, 3, pal::muted);
    const int tw = sv::textWidth(fb, m.label);
    sv::small(fb, std::min(x + w - tw, std::max(x, px - tw / 2)), y + 4, pal::muted, m.label);
  }
}

void drawBars(SoundLab &lab, FrameBuffer &fb, float) {
  const int x = 14, w = 296, yTop = sv::kTop + 22, yBot = 132, h = yBot - yTop;
  const int step = w / kBars;
  for (int i = 0; i < kBars; i++) {
    const float a = bars.above(i);
    const int bh = int(std::min(1.0f, a / 40.0f) * h);
    // Hue ramp: cyan for the lows through blue and violet to pink for the highs.
    const uint8_t hue = uint8_t(128 + i * 110 / kBars);
    const uint8_t val = uint8_t(120 + std::min(135.0f, a * 4));
    if (bh > 0) fb.fillRect(x + i * step, yBot - bh, step - 2, bh, hsv(hue, 200, val));
    const int cy = yBot - int(std::min(1.0f, caps[i] / 40.0f) * h);
    fb.drawFastHLine(x + i * step, cy - 1, step - 2, pal::ink);
  }
  fb.drawFastHLine(x, yBot + 1, w, pal::dim);
  freqAxis(fb, x, w, yBot + 2);

  char text[48];
  const float rate = lab.mic().rate();
  const snd::Fft::Peak p = fft.peak(rate, kLoHz, std::min(kHiHz, rate / 2 - 1));
  if (lab.level().above() >= 6) {
    const snd::Note nt = s5::noteFor(p.hz, st.a4);
    if (p.hz >= 1000) snprintf(text, sizeof(text), "loudest %.2f kHz (%s)", p.hz / 1000, nt.name);
    else snprintf(text, sizeof(text), "loudest %.0f Hz (%s)", p.hz, nt.name);
  } else {
    snprintf(text, sizeof(text), "quiet: hum, whistle or play music");
  }
  sv::small(fb, 10, sv::kTop + 6, st.hold ? pal::amber : pal::ink, st.hold ? "HOLD" : text);
  snprintf(text, sizeof(text), "N %d - %.0f Hz/bin - %lu us", fft.size(), rate / fft.size(), static_cast<unsigned long>(st.fftUs));
  sv::small(fb, 318 - sv::textWidth(fb, text), sv::kTop + 6, pal::muted, text);
}

void drawWaterfall(SoundLab &lab, FrameBuffer &fb, float) {
  const int x = 30, yBot = 140, cw = 2, rh = 2;  // 140 columns x 2 px, 48 rows x 2 px
  if (!history) return sv::small(fb, 10, 60, pal::coral, "no PSRAM for the waterfall");
  for (int c = 0; c < kCols; c++) {
    const uint8_t *col = history + ((histHead + c) % kCols) * kRows;  // oldest on the left
    for (int r = 0; r < kRows; r++)
      if (col[r] > 8) fb.fillRect(x + c * cw, yBot - (r + 1) * rh, cw, rh, heat[col[r] >> 2]);
  }
  fb.drawRect(x - 1, yBot - kRows * rh - 1, kCols * cw + 2, kRows * rh + 2, pal::dim);
  // Frequency labels up the left side (log scale, like the rows).
  const struct {
    float hz;
    const char *label;
  } marks[] = {{100, "100"}, {500, "500"}, {1000, "1k"}, {2000, "2k"}, {4000, "4k"}, {8000, "8k"}};
  for (const auto &m : marks) {
    const int y = yBot - int(logf(m.hz / kLoHz) / logf(kHiHz / kLoHz) * kRows * rh);
    fb.drawFastHLine(x - 4, y, 3, pal::muted);
    sv::small(fb, 2, std::max(sv::kTop + 2, y - 3), pal::muted, m.label);
  }
  // Colour key, bottom right.
  for (int i = 0; i < 64; i++) fb.drawFastVLine(232 + i, 146, 6, heat[i]);
  sv::small(fb, 30, 146, pal::muted, st.hold ? "HOLD" : "now ->");
  sv::small(fb, 300, 146, pal::muted, "40");
  sv::small(fb, 224, 146, pal::muted, "0");
  (void)lab;
}

void drawTuner(SoundLab &lab, FrameBuffer &fb, float) {
  const bool live = pitch.heard || millis() - pitch.lastHeard < 400;  // keep the last note briefly
  const snd::Note nt = s5::noteFor(pitch.hz, st.a4);
  char text[32];
  if (live && pitch.hz > 0) {
    const bool inTune = fabsf(nt.cents) <= 5;
    sv::big(fb, 14, sv::kTop + 52, inTune ? pal::teal : pal::ink, nt.name);
    snprintf(text, sizeof(text), "%.1f Hz", pitch.hz);
    sv::bold(fb, 14, sv::kTop + 78, pal::muted, text);
    snprintf(text, sizeof(text), "%+.0f cents", nt.cents);
    sv::small(fb, 14, sv::kTop + 88, inTune ? pal::teal : pal::amber, text);
  } else {
    sv::bold(fb, 14, sv::kTop + 40, pal::muted, "whistle");
    sv::bold(fb, 14, sv::kTop + 60, pal::muted, "or hum");
    sv::small(fb, 14, sv::kTop + 80, pal::muted, "close to the mic");
  }
  // The needle: -50 to +50 cents over 120 degrees, a green zone of +-5.
  const int cx = 232, cy = sv::kTop + 92, r = 62;
  auto at = [&](float cents, int rad, int &px, int &py) {
    const float a = (cents / 50.0f) * 1.047f;  // +-60 degrees
    px = cx + int(sinf(a) * rad), py = cy - int(cosf(a) * rad);
  };
  for (int c = -50; c <= 50; c += 10) {
    int x0, y0, x1, y1;
    at(float(c), r, x0, y0);
    at(float(c), r - (c == 0 ? 12 : 6), x1, y1);
    fb.drawLine(x0, y0, x1, y1, c == 0 ? pal::ink : pal::muted);
  }
  for (float c = -5; c <= 5; c += 0.5f) {
    int px, py;
    at(c, r + 3, px, py);
    fb.fillCircle(px, py, 1, pal::teal);
  }
  if (live && pitch.hz > 0) {
    int px, py;
    at(std::max(-50.0f, std::min(50.0f, nt.cents)), r - 4, px, py);
    fb.drawLine(cx, cy, px, py, fabsf(nt.cents) <= 5 ? pal::teal : sv::kSound);
  }
  fb.fillCircle(cx, cy, 3, pal::muted);
  sv::small(fb, cx - r - 4, cy + 4, pal::muted, "flat");
  sv::small(fb, cx + r - 18, cy + 4, pal::muted, "sharp");

  // The trail: the last ~5 s of pitch, +-2 semitones around the note you hold now.
  const int ty = 140, th = 14;
  fb.drawFastHLine(10, ty, 300, pal::dim);
  if (nt.midi) {
    for (int i = 0; i < kTrail; i++) {
      const float m = pitch.trail[(pitch.head + i) % kTrail];
      if (m == 0) continue;
      const float d = m - nt.midi;
      if (fabsf(d) > 2) continue;
      fb.fillRect(10 + i * 300 / kTrail, ty - int(d / 2 * th), 2, 2, fabsf(d) < 0.05f ? pal::teal : sv::kSound);
    }
  }
  if (millis() < st.refUntil) snprintf(text, sizeof(text), "speaker: %s %.1f Hz", st.refName, st.refHz);
  else snprintf(text, sizeof(text), "A4 = %.0f Hz", st.a4);
  sv::small(fb, 318 - sv::textWidth(fb, text), sv::kTop + 4, millis() < st.refUntil ? sv::kSound : pal::muted, text);
  (void)lab;
}

void drawSteer(SoundLab &lab, FrameBuffer &fb, float t) {
  const int yTop = sv::kTop + 10, yBot = 150, h = yBot - yTop;
  // The range bar: low at the bottom, high at the top, log-spaced like the pitch.
  fb.fillRoundRect(8, yTop, 8, h, 3, sv::kPanel);
  const float f = s5::steerFraction(pitch.hz, st.lowHz, st.highHz);
  if (pitch.heard) fb.fillCircle(12, yBot - int(f * h), 3, hsv(uint8_t(f * 200), 220, 255));
  char text[24];
  snprintf(text, sizeof(text), "%.0f", st.highHz);
  sv::small(fb, 20, yTop, pal::muted, text);
  snprintf(text, sizeof(text), "%.0f Hz", st.lowHz);
  sv::small(fb, 20, yBot - 7, pal::muted, text);

  // The ribbon: where the dot has been, fading to the left.
  const int dx = 220;
  for (int i = 1; i < kTrail; i++) {
    const float a = steer.ribbon[(steer.head + i - 1) % kTrail], b = steer.ribbon[(steer.head + i) % kTrail];
    const int x0 = dx - (kTrail - i) * 160 / kTrail, x1 = dx - (kTrail - i - 1) * 160 / kTrail;
    fb.drawLine(x0, yBot - int(a * h), x1, yBot - int(b * h), mix(pal::night, sv::kSound, uint8_t(i * 255 / kTrail)));
  }
  // The target ring, and how long you've held it.
  const int ty = yBot - int(steer.target * h);
  const uint32_t held = steer.inSince ? millis() - steer.inSince : 0;
  fb.drawCircle(dx, ty, 14, held ? pal::teal : pal::amber);
  fb.drawCircle(dx, ty, 15, held ? pal::teal : pal::amber);
  if (held) fb.fillRect(dx - 14, ty + 19, int(28 * std::min(1.0f, held / 1000.0f)), 3, pal::teal);
  // The glowing dot.
  const int y = yBot - int(steer.y * h);
  const uint16_t c = pitch.heard ? hsv(uint8_t(steer.y * 200), 220, 255) : pal::muted;
  for (int g = 3; g >= 1; g--) fb.fillCircle(dx, y, 5 + g * 3, mix(pal::night, c, uint8_t(60 / g)));
  fb.fillCircle(dx, y, 5, c);

  snprintf(text, sizeof(text), "%d", steer.score);
  sv::bold(fb, 270, yTop + 14, pal::ink, text);
  sv::small(fb, 262, yTop + 20, pal::muted, "score");
  sv::small(fb, 248, yBot - 7, pal::muted, pitch.heard ? "" : (fmodf(t, 2) < 1 ? "whistle!" : ""));
  (void)lab;
}

const SoundView kViews[] = {
    {"bars", drawBars, "hold"},
    {"waterfall", drawWaterfall, "hold"},
    {"tuner", drawTuner, "ref"},
    {"steer", drawSteer, "target"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdFft(SoundLab &lab, const String &arg, const String &) {
  long n = 0;
  if (!SoundLab::number(arg, 256, 1024, n) || !fft.setSize(int(n)))
    return lab.warn("fft 256 | 512 | 1024   (now %d: %.0f Hz between bins, a new answer every %.0f ms of sound)", fft.size(),
                    lab.mic().rate() / float(fft.size()), fft.size() * 1000.0f / lab.mic().rate());
  bars.relearn();
  rows.relearn();
  lab.note(sv::Note::Info, "FFT %ld: %.1f Hz per bin, %.0f ms window", n, lab.mic().rate() / float(n), n * 1000.0f / lab.mic().rate());
}

void cmdRange(SoundLab &lab, const String &arg, const String &) {
  const int sp = arg.indexOf(' ');
  long lo = 0, hi = 0;
  if (sp < 0 || !SoundLab::number(arg.substring(0, sp), 100, 7000, lo) || !SoundLab::number(arg.substring(sp + 1), 150, 7900, hi) ||
      hi < lo * 3 / 2)
    return lab.warn("range <low Hz> <high Hz>, 100-7900, high at least 1.5x low (now %.0f %.0f)", st.lowHz, st.highHz);
  st.lowHz = float(lo), st.highHz = float(hi);
  lab.note(sv::Note::Info, "steer range %ld-%ld Hz", lo, hi);
}

void cmdHold(SoundLab &lab, const String &, const String &lower) {
  st.hold = lower.isEmpty() ? !st.hold : lower != "off";
  lab.note(sv::Note::Info, st.hold ? "hold: the picture stays (hold again to go on)" : "live");
}

void cmdPeaks(SoundLab &lab, const String &, const String &) {
  // Local maxima (a bin louder than both neighbours), the five strongest.
  const float rate = lab.mic().rate();
  struct P {
    int k;
    float m;
  } top[5] = {};
  for (int k = 2; k < fft.bins() - 1; k++) {
    const float m = fft.magnitude(k);
    if (m <= fft.magnitude(k - 1) || m < fft.magnitude(k + 1)) continue;
    for (int i = 0; i < 5; i++)
      if (m > top[i].m) {
        for (int j = 4; j > i; j--) top[j] = top[j - 1];
        top[i] = {k, m};
        break;
      }
  }
  Serial.printf("Strongest peaks (FFT %d at %.0f Hz: bins %.1f Hz apart, up to %.0f Hz):\n", fft.size(), rate, rate / fft.size(), rate / 2);
  for (const P &p : top) {
    if (p.m <= 0) break;
    // Refine between bins: a parabola through the peak and its neighbours.
    const float a = fft.magnitude(p.k - 1), b = p.m, c = fft.magnitude(p.k + 1);
    const float den = a - 2 * b + c;
    const float hz = (p.k + (den != 0 ? 0.5f * (a - c) / den : 0)) * rate / fft.size();
    const snd::Note nt = s5::noteFor(hz, st.a4);
    Serial.printf("  %7.1f Hz  %6.1f dBFS  %-4s %+3.0f cents\n", hz, fft.db(p.k), nt.name, nt.cents);
  }
}

void cmdA4(SoundLab &lab, const String &arg, const String &) {
  float hz = 0;
  if (!SoundLab::decimal(arg, 400, 480, hz)) return lab.warn("a4 <Hz>, 400-480 (now %.1f; most music: 440)", st.a4);
  st.a4 = hz;
  lab.note(sv::Note::Info, "A4 = %.1f Hz", hz);
}

void newTarget() {
  float t;
  do t = 0.1f + (esp_random() % 800) / 1000.0f;
  while (fabsf(t - steer.target) < 0.25f);  // somewhere new, worth moving to
  steer.target = t;
  steer.inSince = 0;
}

void cmdTarget(SoundLab &lab, const String &, const String &lower) {
  if (lower == "reset") steer.score = 0;
  newTarget();
  lab.note(sv::Note::Info, "new target%s", lower == "reset" ? ", score 0" : "");
}

// ref: the nearest note to what you sing (or A4 when nothing is heard); ref C5; ref 523.
void cmdRef(SoundLab &lab, const String &arg, const String &) {
  float hz = 0;
  if (arg.isEmpty()) {
    const bool recent = pitch.hz > 0 && millis() - pitch.lastHeard < 2000;
    const int midi = recent ? s5::noteFor(pitch.hz, st.a4).midi : 69;
    hz = st.a4 * powf(2.0f, (midi - 69) / 12.0f);
  } else if (!SoundLab::decimal(arg, 50, 4000, hz)) {
    hz = syn::noteHz(arg.c_str(), st.a4);
    if (hz < 50 || hz > 4000) return lab.warn("ref [note|Hz]: ref A4, ref C#5, ref 523 (50-4000 Hz)");
  }
  const snd::Note nt = s5::noteFor(hz, st.a4);
  strlcpy(st.refName, nt.name, sizeof(st.refName));
  st.refHz = hz;
  st.refUntil = millis() + 1000;
  lab.speaker().tone(hz, 1000, syn::Wave::Sine, 0.8f);
  lab.note(sv::Note::Info, "ref %s %.1f Hz", nt.name, hz);
}

// A log sweep: the frequency doubles at a steady pace, so each octave takes the same time and
// it climbs the log-scaled bars at an even speed. Phase = integral of the frequency.
void cmdSweep(SoundLab &lab, const String &, const String &) {
  if (!sweep) {
    sweep = static_cast<int16_t *>(heap_caps_malloc(kSweepSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!sweep) return lab.warn("no PSRAM for the sweep");
    const double f0 = 100, f1 = 8000 * 0.98, T = 4.0, k = log(f1 / f0), rate = snd::Speaker::kRate;
    for (uint32_t i = 0; i < kSweepSamples; i++) {
      const double t = i / rate;
      const double phase = 2 * M_PI * f0 * T / k * (exp(t / T * k) - 1);
      const double fade = std::min(1.0, std::min(t, T - t) / 0.02);  // 20 ms in and out: no clicks
      sweep[i] = int16_t(sin(phase) * fade * 16000);
    }
  }
  lab.speaker().play(sweep, kSweepSamples);
  lab.showView("waterfall");
  lab.note(sv::Note::Info, "sweep 100 Hz -> 8 kHz, 4 s");
}

void cmdLimits(SoundLab &lab, const String &, const String &) {
  const float rate = lab.mic().rate();
  Serial.printf("What this FFT can and can't see (rate %.0f Hz, N %d):\n", rate, fft.size());
  Serial.printf("  frequencies up to %.0f Hz only (Nyquist: half the sample rate)\n", rate / 2);
  Serial.printf("  bins %.1f Hz apart; each answer looks at %.0f ms of sound\n", rate / fft.size(), fft.size() * 1000 / rate);
  if (lab.mic().digital()) Serial.println("  the INMP441: 24 bits and its own amplifier; voices and music show clearly");
  else Serial.println("  the KY-037 has no amplifier: whistle or hum within a hand's width, quiet sounds drown in the hiss");
}

const SoundCommand kCommands[] = {
    {"fft", "<256|512|1024>", "FFT size: finer bins vs faster answers (default 512)", cmdFft},
    {"range", "<low> <high>", "the steer view's pitch range in Hz (default 600 2500)", cmdRange},
    {"hold", "[off]", "freeze the bars and waterfall (long press too)", cmdHold},
    {"peaks", "", "print the 5 strongest frequencies, with notes", cmdPeaks},
    {"ref", "[note|Hz]", "play a reference note on the speaker (tuner double-click)", cmdRef},
    {"sweep", "", "a 4 s tone sweep 100 Hz -> 8 kHz on the speaker", cmdSweep},
    {"a4", "<Hz>", "tuning reference for note names (default 440)", cmdA4},
    {"target", "[reset]", "steer: a new target (reset: score back to 0)", cmdTarget},
    {"limits", "", "what this FFT can and can't see", cmdLimits},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(SoundLab &lab) {
  fft.setSize(512);
  history = static_cast<uint8_t *>(heap_caps_calloc(kCols * kRows, 1, MALLOC_CAP_SPIRAM));
  if (!history) log_w("No PSRAM for the waterfall");
  // Colour map: night -> pink (the sound track's colour) -> white.
  for (int i = 0; i < 64; i++) heat[i] = i < 40 ? mix(pal::night, sv::kSound, uint8_t(i * 255 / 40)) : mix(sv::kSound, pal::ink, uint8_t((i - 40) * 255 / 23));
  newTarget();
  cmdLimits(lab, String(), String());
}

void tick(SoundLab &lab) {
  analyse(lab);
  if (!st.fresh) return;

  // Steer: glide the dot toward the pitch (log scale), score by staying in the ring for a second.
  if (pitch.heard) steer.y += (s5::steerFraction(pitch.hz, st.lowHz, st.highHz) - steer.y) * 0.4f;
  steer.ribbon[steer.head] = steer.y;
  steer.head = (steer.head + 1) % kTrail;
  const bool inRing = pitch.heard && fabsf(steer.y - steer.target) < 0.09f;
  if (!inRing) steer.inSince = 0;
  else if (!steer.inSince) steer.inSince = millis();
  else if (millis() - steer.inSince >= 1000) {
    steer.score++;
    lab.note(sv::Note::Good, "target %d!", steer.score);
    lab.chime(Chime::Score);
    newTarget();
  }

  // The RGB LED follows the pitch as a hue, only while a whistle is heard on the steer view.
  const bool want = pitch.heard && strcmp(lab.spec().views[lab.view()].name, "steer") == 0;
  if (want) {
    const uint8_t hue = uint8_t(int(steer.y * 200) & ~7);  // 8-step hue: rewrite the LED only on a change
    if (!steer.ledOn || hue != steer.ledHue) {
      if (!steer.ledOn) lab.claimLed(true);
      const uint16_t c = hsv(hue, 255, 60);  // RGB565 back to 8-bit channels for the WS2812
      rgbLedWrite(PIN_RGB, uint8_t((c >> 11) << 3), uint8_t(((c >> 5) & 63) << 2), uint8_t((c & 31) << 3));
      steer.ledOn = true, steer.ledHue = hue;
    }
  } else if (steer.ledOn && millis() - pitch.lastHeard > 300) {
    steer.ledOn = false;
    lab.claimLed(false);
  }
}

const SoundLabSpec kSpec{
    "sound-lab-s5", "S5", "Spectrum", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, nullptr, tick, nullptr, "hold",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
