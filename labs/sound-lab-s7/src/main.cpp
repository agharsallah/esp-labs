// Sound lab S7 Speaker.  uv run pio run -d labs/sound-lab-s7 -e sound-lab-s7 -t upload
// The board makes sound. Numbers go out over I2S to the MAX98357A, a class-D amplifier: it switches
// the speaker fully on and off ~300 000 times a second, and how long each pulse lasts follows the
// number. The speaker cone is too slow to follow the switching, so it moves with the average: the
// wave. Efficient (little heat), and no analog wire anywhere on our side.
//
// What you hear is drawn on the screen:
//   wave      one note's waveform and its spectrum. Same note, different wave = different timbre:
//             a sine is one frequency, a square only the odd harmonics (1, 3, 5...), a saw all of
//             them. At 16 000 samples a second nothing above 8 kHz exists (Nyquist), so a square or
//             saw's high harmonics fold back down as wrong frequencies ("aliasing"): a slight grit
//             on high notes. Honest limit of this simple synth.
//   keys      two octaves; the notes light while they sound
//   envelope  ADSR: attack, decay, sustain, release. A note that starts or stops at full height
//             makes the cone jump: a click. The envelope fades it in and out.
//   tunes     melodies, scrolling like a piano roll
//   fx        game sounds made from recipes: sweeps, noise, filters
//
// Wiring: MAX98357A VIN -> 5V, GND -> GND, BCLK -> 39, LRC -> 38, DIN -> 47; the speaker on its screw
// terminal (the JST plug's two wires; red to +). GAIN and SD unconnected. The screen from lesson 03.
// Start quiet: `vol 20`. Volume is squared (50 = a quarter of the power of 100), like your ears like it.
//
// Views: wave · keys · envelope · tunes · fx   (click = next view, double-click = the action in the footer)
// Keys: double-click = the next note of the scale, long press = the next chord.
// Tunes and fx: double-click = play, long press = pick the next one.
#include <esp_heap_caps.h>

#include "sound_lab.h"
#include "sounds.h"
#include "spectrum.h"

namespace {

// ---- State ------------------------------------------------------------------------------------

struct Synth {
  syn::Wave wave = syn::Wave::Triangle;
  syn::Envelope env;
  int bpm = 120;
  float lastHz = 440;
} cfg;

// Two render buffers in PSRAM, used in turn: the speaker may still read the old one for a block
// while we write the new one.
constexpr uint32_t kCap = uint32_t(s7::kRate * 30);  // 30 s
int16_t *bufs[2] = {nullptr, nullptr};
int which = 0;

constexpr int kMaxEvents = 160;
struct Piece {
  s7::Event ev[kMaxEvents];
  int count = 0;
  uint32_t totalMs = 0;
  uint32_t startedAt = 0;  // millis() when it started playing
  bool playing = false;
  char title[24] = "";
  const int16_t *samples = nullptr;
  uint32_t n = 0;
} piece;

int scaleStep = 0, chordStep = 0, tuneIdx = 0, fxIdx = 0;

struct TuneDef {
  const char *name;
  const char *notes;
  int bpm;
};
const TuneDef kTunes[] = {
    {"twinkle", "C5/4 C5/4 G5/4 G5/4 A5/4 A5/4 G5/2 F5/4 F5/4 E5/4 E5/4 D5/4 D5/4 C5/2", 150},
    {"ode", "E5/4 E5/4 F5/4 G5/4 G5/4 F5/4 E5/4 D5/4 C5/4 C5/4 D5/4 E5/4 E5/4. D5/8 D5/2", 150},
    {"happy", "C5/8. C5/16 D5/4 C5/4 F5/4 E5/2 C5/8. C5/16 D5/4 C5/4 G5/4 F5/2 C5/8. C5/16 C6/4 A5/4 F5/4 E5/4 D5/4 Bb5/8. Bb5/16 A5/4 F5/4 G5/4 F5/2", 120},
    {"korobeiniki", "E5/4 B4/8 C5/8 D5/4 C5/8 B4/8 A4/4 A4/8 C5/8 E5/4 D5/8 C5/8 B4/4. C5/8 D5/4 E5/4 C5/4 A4/4 A4/2 r/8 D5/4. F5/8 A5/4 G5/8 F5/8 E5/4. C5/8 E5/4 D5/8 C5/8 B4/4 B4/8 C5/8 D5/4 E5/4 C5/4 A4/4 A4/2", 150},
    {"scale", "C4/8 D4/8 E4/8 F4/8 G4/8 A4/8 B4/8 C5/8 D5/8 E5/8 F5/8 G5/8 A5/8 B5/8 C6/4", 160},
    {"arpeggio", "C4/16 E4/16 G4/16 C5/16 E5/16 G5/16 C6/8 G5/16 E5/16 C5/16 G4/16 E4/16 C4/4 F4/16 A4/16 C5/16 F5/16 A5/16 C6/16 F6/8", 110},
};
constexpr int kTuneCount = sizeof(kTunes) / sizeof(kTunes[0]);
const char *const kChords[] = {"Cmaj", "Fmaj", "Gmaj", "Am", "Dm", "G7"};
const int kScale[] = {60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83};  // C major, C4-B5

const char *noteName(int midi, char *out, size_t len) {
  static const char *const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  snprintf(out, len, "%s%d", kNames[((midi % 12) + 12) % 12], midi / 12 - 1);
  return out;
}

// ---- Rendering and playing ---------------------------------------------------------------------

int16_t *fresh() {
  which ^= 1;
  std::fill_n(bufs[which], kCap, int16_t(0));
  return bufs[which];
}

void start(SoundLab &lab, int16_t *b, uint32_t n, const char *title) {
  lab.speaker().stop();
  lab.speaker().play(b, n);
  piece.samples = b, piece.n = n;
  piece.totalMs = n * 1000 / uint32_t(s7::kRate);
  piece.startedAt = millis();
  piece.playing = true;
  strlcpy(piece.title, title, sizeof(piece.title));
}

// Notes one after another (hz, ms pairs from a Tune), each with the current wave and envelope.
void playTune(SoundLab &lab, const char *notes, int bpm, const char *title) {
  int16_t *b = fresh();
  syn::Tune t;
  t.set(notes, bpm);
  float hz = 0, ms = 0;
  uint32_t atMs = 0, end = 0;
  piece.count = 0;
  while (t.next(hz, ms)) {
    if (hz > 0) {
      const uint32_t at = uint32_t(atMs * s7::kRate / 1000);
      if (at >= kCap) break;
      end = std::max(end, s7::renderNote(b, kCap, at, hz, ms * 0.95f, cfg.wave, cfg.env, 0.55f));
      if (piece.count < kMaxEvents) piece.ev[piece.count++] = {atMs, uint32_t(ms), s7::midiOf(hz)};
      cfg.lastHz = hz;
    }
    atMs += uint32_t(ms);
  }
  start(lab, b, std::max(end, uint32_t(atMs * s7::kRate / 1000)), title);
}

// Several notes at once.
void playChord(SoundLab &lab, const int *midis, int n, uint32_t ms, const char *title) {
  int16_t *b = fresh();
  piece.count = 0;
  uint32_t end = 0;
  for (int i = 0; i < n; i++) {
    // Each voice a bit quieter, so three together don't clip: 1/sqrt(n) keeps the loudness even.
    end = std::max(end, s7::renderNote(b, kCap, 0, s7::hzOf(midis[i]), float(ms), cfg.wave, cfg.env, 0.7f / sqrtf(float(n))));
    piece.ev[piece.count++] = {0, ms, midis[i]};
  }
  cfg.lastHz = s7::hzOf(midis[0]);
  start(lab, b, end, title);
}

void playNote(SoundLab &lab, float hz, uint32_t ms) {
  int16_t *b = fresh();
  const uint32_t end = s7::renderNote(b, kCap, 0, hz, float(ms), cfg.wave, cfg.env, 0.7f);
  piece.count = 0;
  piece.ev[piece.count++] = {0, ms, s7::midiOf(hz)};
  cfg.lastHz = hz;
  char t[24];
  snprintf(t, sizeof(t), "%.0f Hz", hz);
  start(lab, b, end, t);
}

void playFx(SoundLab &lab, int idx) {
  int count = 0;
  const s7::Fx *fx = s7::effects(count);
  fxIdx = (idx % count + count) % count;
  int16_t *b = fresh();
  const uint32_t n = fx[fxIdx].render(b, kCap);
  piece.count = 0;
  start(lab, b, n, fx[fxIdx].name);
  lab.note(sv::Note::Info, "fx %s: %s", fx[fxIdx].name, fx[fxIdx].what);
}

uint32_t elapsedMs() { return piece.playing ? millis() - piece.startedAt : 0; }

// ---- Views ------------------------------------------------------------------------------------

// The wave view's picture: one steady stretch of the current note, and its spectrum.
struct WavePic {
  int16_t s[512];
  float bins[256];
  syn::Wave wave = syn::Wave(255);
  float hz = 0;
} pic;
snd::Fft fft;

void renderPic() {
  if (pic.wave == cfg.wave && pic.hz == cfg.lastHz) return;
  pic.wave = cfg.wave, pic.hz = cfg.lastHz;
  syn::Voice v;
  syn::Envelope flat;
  flat.attackMs = 0.1f, flat.decayMs = 0.1f, flat.sustain = 1;
  v.start(cfg.lastHz, 1000, cfg.wave, flat, s7::kRate, 0.9f);
  for (int i = 0; i < 64; i++) v.next();  // past the (tiny) attack
  for (int i = 0; i < 512; i++) pic.s[i] = int16_t(v.next() * 30000);
  fft.run(pic.s);
  for (int k = 0; k < 256; k++) pic.bins[k] = fft.db(k);
}

void drawWave(SoundLab &, FrameBuffer &fb, float) {
  renderPic();
  // Top: four periods of the wave (or 10 ms of noise).
  const int periods = cfg.wave == syn::Wave::Noise ? 0 : 4;
  const int span = periods ? std::min(512, int(periods * s7::kRate / cfg.lastHz)) : 160;
  sv::wave(fb, 10, sv::kTop + 4, 300, 56, pic.s, std::max(span, 8), 32000, sv::kSound);
  char text[48];
  char nn[16];
  snprintf(text, sizeof(text), "%s  %.0f Hz (%s)", syn::waveName(cfg.wave), cfg.lastHz, noteName(s7::midiOf(cfg.lastHz), nn, sizeof(nn)));
  sv::small(fb, 10, sv::kTop + 62, pal::ink, text);
  // Bottom: the spectrum 0-8 kHz, one column per bin pair; harmonics stand up as lines.
  const int x0 = 10, y0 = sv::kTop + 74, h = 46, w = 300;
  for (int col = 0; col < w; col++) {
    const int k0 = col * 256 / w, k1 = std::max(k0 + 1, (col + 1) * 256 / w);
    float m = -120;
    for (int k = k0; k < k1; k++) m = std::max(m, pic.bins[k]);
    const int bh = std::max(0, std::min(h, int((m + 60) * h / 60)));  // -60..0 dB
    if (bh) fb.drawFastVLine(x0 + col, y0 + h - bh, bh, mix(sv::kSound, pal::amber, uint8_t(col * 255 / w)));
  }
  fb.drawFastHLine(x0, y0 + h, w, pal::dim);
  sv::small(fb, x0, y0 + h + 3, pal::muted, "0");
  sv::small(fb, x0 + w / 2 - 6, y0 + h + 3, pal::muted, "4k");
  sv::small(fb, x0 + w - 18, y0 + h + 3, pal::muted, "8kHz");
  const char *why = cfg.wave == syn::Wave::Sine       ? "one frequency: pure"
                    : cfg.wave == syn::Wave::Square   ? "odd harmonics 1,3,5..: hollow"
                    : cfg.wave == syn::Wave::Triangle ? "odd harmonics, soft: flute"
                    : cfg.wave == syn::Wave::Saw      ? "every harmonic: bright, buzzy"
                                                      : "every frequency: hiss";
  sv::small(fb, 120, y0 + h + 3, pal::muted, why);
}

// Keys C4..B5: 14 white keys, 10 black.
void drawKeys(SoundLab &, FrameBuffer &fb, float) {
  bool lit[128] = {};
  const uint32_t e = elapsedMs();
  if (piece.playing)
    for (int i = 0; i < piece.count; i++)
      if (e >= piece.ev[i].startMs && e < piece.ev[i].startMs + piece.ev[i].durMs && piece.ev[i].midi >= 0 && piece.ev[i].midi < 128)
        lit[piece.ev[i].midi] = true;
  const int x0 = 13, y0 = sv::kTop + 18, kw = 21, kh = 80;
  const int whiteSemis[] = {0, 2, 4, 5, 7, 9, 11};
  for (int i = 0; i < 14; i++) {
    const int midi = 60 + (i / 7) * 12 + whiteSemis[i % 7];
    const int x = x0 + i * kw;
    fb.fillRect(x, y0, kw - 2, kh, lit[midi] ? sv::kSound : pal::ink);
    if (i % 7 == 0) {
      char t[6];
      snprintf(t, sizeof(t), "C%d", 4 + i / 7);
      sv::small(fb, x + 4, y0 + kh - 10, pal::night, t);
    }
  }
  const int blackAfter[] = {0, 1, 3, 4, 5};  // black keys sit after C, D, F, G, A
  const int blackSemis[] = {1, 3, 6, 8, 10};
  for (int o = 0; o < 2; o++)
    for (int j = 0; j < 5; j++) {
      const int midi = 60 + o * 12 + blackSemis[j];
      const int x = x0 + (o * 7 + blackAfter[j]) * kw + kw - 7;
      fb.fillRect(x, y0, 12, kh * 6 / 10, lit[midi] ? sv::kSound : pal::night);
    }
  char text[64];
  snprintf(text, sizeof(text), "%s - A%.0f D%.0f S%.0f%% R%.0f ms", syn::waveName(cfg.wave), cfg.env.attackMs, cfg.env.decayMs, cfg.env.sustain * 100,
           cfg.env.releaseMs);
  sv::small(fb, 10, sv::kTop + 4, pal::muted, text);
  sv::small(fb, 10, sv::kTop + 104, pal::muted, "2x: next note   long: next chord");
  if (piece.playing) sv::small(fb, 220, sv::kTop + 104, sv::kSound, piece.title);
}

void drawEnvelope(SoundLab &, FrameBuffer &fb, float) {
  const syn::Envelope &e = cfg.env;
  const float holdMs = 250;
  const float total = e.attackMs + e.decayMs + holdMs + e.releaseMs;
  const int x0 = 14, y0 = sv::kTop + 14, w = 292, h = 84;
  auto X = [&](float ms) { return x0 + int(ms / total * w); };
  auto Y = [&](float a) { return y0 + h - int(a * h); };
  const float pts[5][2] = {{0, 0}, {e.attackMs, 1}, {e.attackMs + e.decayMs, e.sustain}, {e.attackMs + e.decayMs + holdMs, e.sustain}, {total, 0}};
  fb.drawFastHLine(x0, y0 + h, w, pal::dim);
  const uint16_t colours[4] = {pal::teal, pal::amber, sv::kSound, pal::coral};
  const char *labels[4] = {"A", "D", "S", "R"};
  for (int i = 0; i < 4; i++) {
    for (int dy = 0; dy < 2; dy++) fb.drawLine(X(pts[i][0]), Y(pts[i][1]) + dy, X(pts[i + 1][0]), Y(pts[i + 1][1]) + dy, colours[i]);
    sv::small(fb, (X(pts[i][0]) + X(pts[i + 1][0])) / 2 - 2, y0 + h + 4, colours[i], labels[i]);
  }
  // The dot rides the curve while a note plays: attack, decay, sustain while held, then release.
  if (piece.playing && piece.count == 1) {
    const float t = float(elapsedMs());
    const float held = float(piece.ev[0].durMs) - e.releaseMs;
    float ms, a;
    if (t < e.attackMs) ms = t, a = t / e.attackMs;
    else if (t < e.attackMs + e.decayMs) ms = t, a = 1 - (1 - e.sustain) * (t - e.attackMs) / e.decayMs;
    else if (t < held) ms = std::min(t, e.attackMs + e.decayMs + holdMs * 0.99f), a = e.sustain;
    else ms = e.attackMs + e.decayMs + holdMs + (t - held), a = e.sustain * std::max(0.0f, 1 - (t - held) / e.releaseMs);
    fb.fillCircle(X(std::min(ms, total)), Y(a), 4, pal::ink);
  }
  char text[64];
  snprintf(text, sizeof(text), "adsr %.0f %.0f %.0f %.0f   (ms, ms, %%, ms)", e.attackMs, e.decayMs, e.sustain * 100, e.releaseMs);
  sv::small(fb, x0, sv::kTop + 112, pal::ink, text);
  sv::small(fb, x0, sv::kTop + 2, pal::muted, "fade in, fall, hold, fade out: no clicks");
}

void drawTunes(SoundLab &, FrameBuffer &fb, float) {
  // Left: the list.
  for (int i = 0; i < kTuneCount; i++) {
    const bool sel = i == tuneIdx;
    if (sel) fb.fillRect(4, sv::kTop + 4 + i * 16, 92, 15, sv::kPanel);
    sv::small(fb, 8, sv::kTop + 8 + i * 16, sel ? sv::kSound : pal::muted, kTunes[i].name);
  }
  sv::small(fb, 8, sv::kTop + 112, pal::muted, "2x: play");
  sv::small(fb, 8, sv::kTop + 122, pal::muted, "long: next");
  // Right: a piano roll of the playing tune (or the picked one, from the start). Pitch up, time right.
  const int x0 = 104, y0 = sv::kTop + 4, w = 210, h = 124;
  fb.drawRect(x0, y0, w, h, pal::dim);
  const bool live = piece.playing && strcmp(piece.title, kTunes[tuneIdx].name) == 0;
  // Not playing it? Parse the picked tune just for the picture (never touching what plays).
  static s7::Event preview[kMaxEvents];
  const s7::Event *ev = piece.ev;
  int count = piece.count;
  if (!live) {
    syn::Tune t;
    t.set(kTunes[tuneIdx].notes, kTunes[tuneIdx].bpm);
    float hz = 0, ms = 0;
    uint32_t at = 0;
    count = 0;
    while (t.next(hz, ms) && count < kMaxEvents) {
      if (hz > 0) preview[count++] = {at, uint32_t(ms), s7::midiOf(hz)};
      at += uint32_t(ms);
    }
    ev = preview;
  }
  int lo = 127, hi = 0;
  for (int i = 0; i < count; i++) lo = std::min(lo, ev[i].midi), hi = std::max(hi, ev[i].midi);
  if (hi < lo) return;
  lo -= 1, hi += 1;
  const uint32_t window = 4000;  // ms across the roll
  const uint32_t now = live ? elapsedMs() : 0;
  const int playX = x0 + w / 4;
  const float pxPerMs = float(w) / window;
  for (int i = 0; i < count; i++) {
    const s7::Event &e = ev[i];
    const int x = playX + int((float(e.startMs) - float(now)) * pxPerMs);
    const int len = std::max(2, int(e.durMs * pxPerMs) - 1);
    if (x + len < x0 + 1 || x > x0 + w - 2) continue;
    const int y = y0 + h - 4 - (e.midi - lo) * (h - 8) / std::max(1, hi - lo);
    const bool on = live && now >= e.startMs && now < e.startMs + e.durMs;
    const int xa = std::max(x, x0 + 1), xb = std::min(x + len, x0 + w - 2);
    fb.fillRect(xa, y - 2, xb - xa, 5, on ? pal::ink : mix(sv::kSound, pal::night, 60));
  }
  fb.drawFastVLine(playX, y0 + 1, h - 2, live ? pal::amber : pal::dim);
}

void drawFx(SoundLab &, FrameBuffer &fb, float) {
  int count = 0;
  const s7::Fx *fx = s7::effects(count);
  for (int i = 0; i < count; i++) {
    const bool sel = i == fxIdx;
    if (sel) fb.fillRect(4, sv::kTop + 4 + i * 16, 82, 15, sv::kPanel);
    sv::small(fb, 8, sv::kTop + 8 + i * 16, sel ? sv::kSound : pal::muted, fx[i].name);
  }
  // The last effect's whole waveform, with a playhead.
  const int x0 = 94, y0 = sv::kTop + 6, w = 218, h = 96;
  fb.drawRect(x0 - 1, y0 - 1, w + 2, h + 2, pal::dim);
  if (piece.samples && piece.count == 0 && piece.n) {
    sv::wave(fb, x0, y0, w, h, piece.samples, int(piece.n), 33000, sv::kSound);
    if (piece.playing) fb.drawFastVLine(x0 + int(std::min(1.0f, float(elapsedMs()) / std::max<uint32_t>(1, piece.totalMs)) * w), y0, h, pal::ink);
    char t[40];
    snprintf(t, sizeof(t), "%s, %lu ms", piece.title, static_cast<unsigned long>(piece.totalMs));
    sv::small(fb, x0, y0 + h + 5, pal::ink, t);
  } else {
    sv::small(fb, x0 + 10, y0 + h / 2 - 3, pal::muted, "double-click to play an effect");
  }
  sv::small(fb, x0, y0 + h + 17, pal::muted, fx[fxIdx].what);
}

const SoundView kViews[] = {
    {"wave", drawWave, "wave next"},
    {"keys", drawKeys, "next note"},
    {"envelope", drawEnvelope, "note A4 600"},
    {"tunes", drawTunes, "tune picked"},
    {"fx", drawFx, "fx picked"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdWave(SoundLab &lab, const String &, const String &lower) {
  syn::Wave w;
  if (lower == "next") w = syn::Wave((int(cfg.wave) + 1) % 5);
  else if (!syn::parseWave(lower.c_str(), w)) return lab.warn("wave sine | square | triangle | saw | noise | next");
  cfg.wave = w;
  playNote(lab, cfg.lastHz, 500);
  lab.note(sv::Note::Info, "wave %s", syn::waveName(w));
}

// "play C4 E4 G4" or with lengths "play C4/8 E4/8 G4/2": one after another at the current bpm.
void cmdPlay(SoundLab &lab, const String &arg, const String &) {
  if (arg.isEmpty()) return lab.warn("play <notes>, e.g. play C4 E4 G4 C5/2   (/4 = quarter, /8 = eighth, r = rest)");
  static char text[200];
  strlcpy(text, arg.c_str(), sizeof(text));
  playTune(lab, text, cfg.bpm, "play");
}

void cmdChord(SoundLab &lab, const String &arg, const String &) {
  int m[4];
  const int n = s7::chordNotes(arg.c_str(), m);
  if (!n) return lab.warn("chord <root><quality>: Cmaj Am G7 Fmaj7 Dm7 Bdim Caug Dsus4 Esus2 (root may have # or b)");
  playChord(lab, m, n, 1200, arg.c_str());
  char a[16], b[16], c[16];
  lab.note(sv::Note::Info, "%s = %s %s %s%s", arg.c_str(), noteName(m[0], a, 16), noteName(m[1], b, 16), noteName(m[2], c, 16), n > 3 ? " +7th" : "");
}

void cmdNote(SoundLab &lab, const String &arg, const String &) {
  const int sp = arg.indexOf(' ');
  const String what = sp < 0 ? arg : arg.substring(0, sp);
  long ms = 600;
  if (sp > 0 && !SoundLab::number(arg.substring(sp + 1), 20, 10000, ms)) return lab.warn("note <name|Hz> [ms 20-10000]");
  float hz = syn::noteHz(what.c_str());
  if (hz <= 0 && !SoundLab::decimal(what, 20, 8000, hz)) return lab.warn("note A4 | note C#5 300 | note 1000");
  if (hz > 7000) lab.note(sv::Note::Warn, "close to 8 kHz: the limit at 16 kHz");
  playNote(lab, hz, uint32_t(ms));
}

void cmdAdsr(SoundLab &lab, const String &arg, const String &) {
  float v[4];
  int n = 0, from = 0;
  while (n < 4 && from <= int(arg.length())) {
    int sp = arg.indexOf(' ', from);
    if (sp < 0) sp = arg.length();
    if (sp > from && !SoundLab::decimal(arg.substring(from, sp), 0, 5000, v[n++])) n = -10;
    from = sp + 1;
  }
  if (n != 4 || v[2] > 100) {
    lab.warn("adsr <attack ms> <decay ms> <sustain %%> <release ms>, e.g. adsr 5 80 65 90 (a plucked string: adsr 1 300 0 50)");
    return;
  }
  cfg.env.attackMs = std::max(0.5f, v[0]);
  cfg.env.decayMs = std::max(0.5f, v[1]);
  cfg.env.sustain = v[2] / 100.0f;
  cfg.env.releaseMs = std::max(1.0f, v[3]);
  lab.showView("envelope");
  playNote(lab, cfg.lastHz, 600);
}

void cmdTune(SoundLab &lab, const String &, const String &lower) {
  if (lower == "picked") return playTune(lab, kTunes[tuneIdx].notes, kTunes[tuneIdx].bpm, kTunes[tuneIdx].name);
  for (int i = 0; i < kTuneCount; i++)
    if (lower == kTunes[i].name) {
      tuneIdx = i;
      lab.showView("tunes");
      return playTune(lab, kTunes[i].notes, kTunes[i].bpm, kTunes[i].name);
    }
  Serial.print("tunes:");
  for (const TuneDef &t : kTunes) Serial.printf(" %s", t.name);
  Serial.println();
  if (!lower.isEmpty()) lab.warn("no tune '%s'", lower.c_str());
}

void cmdBpm(SoundLab &lab, const String &arg, const String &) {
  long b = 0;
  if (!SoundLab::number(arg, 30, 300, b)) return lab.warn("bpm <30-300> (now %d): the tempo for play", cfg.bpm);
  cfg.bpm = int(b);
}

void cmdFx(SoundLab &lab, const String &, const String &lower) {
  int count = 0;
  const s7::Fx *fx = s7::effects(count);
  if (lower == "picked") return playFx(lab, fxIdx);
  for (int i = 0; i < count; i++)
    if (lower == fx[i].name) {
      lab.showView("fx");
      return playFx(lab, i);
    }
  Serial.print("effects:");
  for (int i = 0; i < count; i++) Serial.printf(" %s", fx[i].name);
  Serial.println();
}

void cmdStop(SoundLab &lab, const String &, const String &) {
  lab.speaker().stop();
  piece.playing = false;
}

void cmdNextNote(SoundLab &lab, const String &, const String &) {
  const int midi = kScale[scaleStep++ % 14];
  playNote(lab, s7::hzOf(midi), 450);
}

const SoundCommand kCommands[] = {
    {"wave", "<name|next>", "sine square triangle saw noise: the timbre", cmdWave},
    {"play", "<notes>", "notes one after another: play C4 E4 G4/2 r/4 C5", cmdPlay},
    {"chord", "<name>", "notes together: Cmaj Am G7 Fmaj7 Bdim Dsus4", cmdChord},
    {"note", "<name|Hz> [ms]", "one note: note A4, note 1000 250", cmdNote},
    {"adsr", "<a> <d> <s%> <r>", "the envelope, ms ms % ms (default 6 80 65 90)", cmdAdsr},
    {"tune", "<name>", "twinkle ode happy korobeiniki scale arpeggio", cmdTune},
    {"bpm", "<n>", "tempo for play (default 120)", cmdBpm},
    {"fx", "<name>", "laser coin jump powerup explosion siren modem", cmdFx},
    {"stop", "", "silence", cmdStop},
    {"next", "", "the next note of the C major scale", cmdNextNote},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(SoundLab &lab) {
  for (int i = 0; i < 2; i++) bufs[i] = static_cast<int16_t *>(heap_caps_calloc(kCap, sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (!bufs[0] || !bufs[1]) log_e("S7: no PSRAM for the render buffers");
  fft.setSize(512);
  lab.note(sv::Note::Info, "start quiet: vol 20, then raise it");
}

void tick(SoundLab &lab) {
  if (piece.playing && !lab.speaker().playing() && elapsedMs() > piece.totalMs) piece.playing = false;
}

bool gesture(SoundLab &lab, Gesture g, bool) {
  const char *v = lab.spec().views[lab.view()].name;
  if (g == Gesture::DoubleClick && strcmp(v, "keys") == 0) return cmdNextNote(lab, "", ""), true;
  if (g == Gesture::LongPress && strcmp(v, "keys") == 0) {
    int m[4];
    const char *name = kChords[chordStep++ % (sizeof(kChords) / sizeof(kChords[0]))];
    playChord(lab, m, s7::chordNotes(name, m), 1000, name);
    return true;
  }
  if (g == Gesture::LongPress && strcmp(v, "tunes") == 0) return tuneIdx = (tuneIdx + 1) % kTuneCount, true;
  if (g == Gesture::LongPress && strcmp(v, "fx") == 0) {
    int count = 0;
    s7::effects(count);
    fxIdx = (fxIdx + 1) % count;
    return true;
  }
  return false;
}

const SoundLabSpec kSpec{
    "sound-lab-s7", "S7", "Speaker", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, nullptr, tick, gesture, "stop",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
