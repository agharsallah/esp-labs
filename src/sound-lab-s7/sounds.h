// S7's sound factory: notes, chords and effects rendered into a buffer of 16-bit samples, which the
// speaker then plays (speaker.play). Rendering ahead instead of live lets us play chords (the
// speaker's own synth has one voice) and draw exactly what you hear. Pure C++, no Arduino.
//
// Effects are "procedural": no recordings, only recipes. A frequency that glides (a sweep), noise
// that gets darker (a low-pass filter closing), a pitch that wobbles (a siren). The classic 8-bit
// game sounds are all made this way.
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>

#include "synth.h"

namespace s7 {

constexpr float kRate = 16000;
constexpr float kTau = 6.2831853f;

inline int16_t toSample(float v) { return int16_t(syn::softClip(v) * 32000.0f); }

// Add a float sample onto an int16 buffer without wrapping around.
inline void addInto(int16_t &dst, float v) {
  const float s = dst + v * 32000.0f;
  dst = int16_t(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

inline int midiOf(float hz) { return hz > 0 ? int(lroundf(69 + 12 * log2f(hz / 440.0f))) : -1; }
inline float hzOf(int midi) { return 440.0f * powf(2.0f, (midi - 69) / 12.0f); }

// One sounding note in a rendered piece: for lighting keys and drawing the piano roll.
struct Event {
  uint32_t startMs, durMs;
  int midi;
};

// "Cmaj", "Am", "G7", "F#m7", "Bbdim", "Dsus4": root (octave 4) + quality. Returns the note count.
inline int chordNotes(const char *name, int midis[4]) {
  static const int kSemis[] = {9, 11, 0, 2, 4, 5, 7};
  if (!name || name[0] < 'A' || name[0] > 'G') return 0;
  int root = 60 + kSemis[name[0] - 'A'], i = 1;
  if (name[i] == '#') root++, i++;
  else if (name[i] == 'b') root--, i++;
  const char *q = name + i;
  static const struct {
    const char *q;
    int n, iv[4];
  } kQualities[] = {
      {"", 3, {0, 4, 7}},      {"maj", 3, {0, 4, 7}},   {"m", 3, {0, 3, 7}},      {"min", 3, {0, 3, 7}},
      {"7", 4, {0, 4, 7, 10}}, {"maj7", 4, {0, 4, 7, 11}}, {"m7", 4, {0, 3, 7, 10}}, {"dim", 3, {0, 3, 6}},
      {"aug", 3, {0, 4, 8}},   {"sus4", 3, {0, 5, 7}},  {"sus2", 3, {0, 2, 7}},
  };
  for (const auto &k : kQualities)
    if (strcmp(q, k.q) == 0) {
      for (int j = 0; j < k.n; j++) midis[j] = root + k.iv[j];
      return k.n;
    }
  return 0;
}

// Render one note (adding onto whatever is already there) starting at sample `at`.
inline uint32_t renderNote(int16_t *buf, uint32_t cap, uint32_t at, float hz, float ms, syn::Wave w, const syn::Envelope &env,
                           float level) {
  syn::Voice v;
  v.start(hz, std::max(1.0f, ms - env.releaseMs), w, env, kRate, level);
  uint32_t i = at;
  while (v.active() && i < cap) addInto(buf[i++], v.next());
  return i;
}

// ---- Effects ---------------------------------------------------------------------------------

struct Fx {
  const char *name;
  const char *what;
  uint32_t (*render)(int16_t *buf, uint32_t cap);
};

inline float square(float phase) { return phase - floorf(phase) < 0.5f ? 1.0f : -1.0f; }

struct Noise {
  uint32_t s = 12345;
  float next() {
    s = s * 1664525u + 1013904223u;
    return (int32_t(s) >> 8) / 8388608.0f;
  }
};

// Laser: a square wave diving from 2.4 kHz to 150 Hz in a third of a second.
inline uint32_t laser(int16_t *b, uint32_t cap) {
  const uint32_t n = std::min<uint32_t>(cap, uint32_t(0.35f * kRate));
  float ph = 0;
  for (uint32_t i = 0; i < n; i++) {
    const float t = i / kRate;
    ph += (2400 * expf(-t * 9) + 150) / kRate;
    b[i] = toSample(square(ph) * 0.35f * (1 - t / 0.35f));
  }
  return n;
}

// Coin: two square notes, B5 then E6, the second one ringing out.
inline uint32_t coin(int16_t *b, uint32_t cap) {
  const uint32_t n = std::min<uint32_t>(cap, uint32_t(0.42f * kRate));
  float ph = 0;
  for (uint32_t i = 0; i < n; i++) {
    const float t = i / kRate;
    ph += (t < 0.07f ? 987.8f : 1318.5f) / kRate;
    b[i] = toSample(square(ph) * 0.3f * (t < 0.07f ? 1 : expf(-(t - 0.07f) * 8)));
  }
  return n;
}

// Jump: a quick upward glide.
inline uint32_t jump(int16_t *b, uint32_t cap) {
  const uint32_t n = std::min<uint32_t>(cap, uint32_t(0.22f * kRate));
  float ph = 0;
  for (uint32_t i = 0; i < n; i++) {
    const float t = i / kRate;
    ph += (250 + 650 * t / 0.22f) / kRate;
    b[i] = toSample(square(ph) * 0.3f * (1 - 0.6f * t / 0.22f));
  }
  return n;
}

// Power-up: three rising arpeggios, each a step higher.
inline uint32_t powerUp(int16_t *b, uint32_t cap) {
  syn::Envelope env;
  env.attackMs = 2, env.decayMs = 10, env.sustain = 0.8f, env.releaseMs = 10;
  std::fill_n(b, cap, int16_t(0));
  uint32_t end = 0;
  const int arp[] = {0, 4, 7, 12};
  for (int round = 0; round < 3; round++)
    for (int k = 0; k < 4; k++) {
      const uint32_t at = uint32_t((round * 4 + k) * 0.045f * kRate);
      end = std::max(end, renderNote(b, cap, at, hzOf(72 + round * 2 + arp[k]), 50, syn::Wave::Triangle, env, 0.5f));
    }
  return end;
}

// Explosion: noise through a low-pass filter that closes (darker and darker) while it fades.
inline uint32_t explosion(int16_t *b, uint32_t cap) {
  const uint32_t n = std::min<uint32_t>(cap, uint32_t(0.9f * kRate));
  Noise noise;
  float lp = 0, ph = 0;
  for (uint32_t i = 0; i < n; i++) {
    const float t = i / kRate;
    const float cutoff = 0.5f * expf(-t * 4) + 0.01f;  // the filter's "how fast it follows" 0-1
    lp += cutoff * (noise.next() - lp);
    ph += 55 / kRate;
    b[i] = toSample((lp * 1.6f + 0.25f * sinf(kTau * ph)) * expf(-t * 3.5f));
  }
  return n;
}

// Siren: a sine wobbling between 550 and 1250 Hz a little faster than once a second.
inline uint32_t siren(int16_t *b, uint32_t cap) {
  const uint32_t n = std::min<uint32_t>(cap, uint32_t(2.2f * kRate));
  float ph = 0;
  for (uint32_t i = 0; i < n; i++) {
    const float t = i / kRate;
    ph += (900 + 350 * sinf(kTau * 1.2f * t)) / kRate;
    const float fade = std::min(1.0f, std::min(t, 2.2f - t) * 20);
    b[i] = toSample(sinf(kTau * ph) * 0.5f * fade);
  }
  return n;
}

// Modem: a dial tone (350 + 440 Hz), a burst of two-tone chirps (FSK: 1200 Hz = 1, 2200 Hz = 0),
// then a hiss: how computers talked over phone lines.
inline uint32_t modem(int16_t *b, uint32_t cap) {
  const uint32_t n = std::min<uint32_t>(cap, uint32_t(1.4f * kRate));
  Noise noise;
  float a = 0, c = 0;
  uint32_t bits = 0xB5E3;
  for (uint32_t i = 0; i < n; i++) {
    const float t = i / kRate;
    float v;
    if (t < 0.3f) {
      a += 350 / kRate, c += 440 / kRate;
      v = 0.25f * (sinf(kTau * a) + sinf(kTau * c));
    } else if (t < 0.9f) {
      const int bit = (bits >> (int((t - 0.3f) / 0.04f) % 16)) & 1;
      a += (bit ? 1200.0f : 2200.0f) / kRate;
      v = 0.4f * sinf(kTau * a);
    } else {
      v = 0.3f * noise.next() * expf(-(t - 0.9f) * 6);
    }
    b[i] = toSample(v);
  }
  return n;
}

inline const Fx *effects(int &count) {
  static const Fx kFx[] = {
      {"laser", "a square wave diving 2.4 kHz -> 150 Hz", laser},
      {"coin", "B5 then E6, square, ringing out", coin},
      {"jump", "a quick glide up, 250 -> 900 Hz", jump},
      {"powerup", "three rising arpeggios", powerUp},
      {"explosion", "noise through a closing low-pass filter", explosion},
      {"siren", "a sine wobbling 550-1250 Hz", siren},
      {"modem", "dial tone, two-tone chirps, hiss", modem},
  };
  count = sizeof(kFx) / sizeof(kFx[0]);
  return kFx;
}

}  // namespace s7
