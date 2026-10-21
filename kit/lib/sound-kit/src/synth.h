// Making sound from numbers: the pieces the speaker (speaker.h) mixes, 16 000 samples a second.
//
//   Voice    one note: an oscillator (sine, square, triangle, saw, noise) shaped by an envelope.
//            ADSR = attack (fade in), decay (fall to the sustain level), sustain (hold), release
//            (fade out). Without it every note starts and stops with a click.
//   Tune     a melody as text: "C5/8 E5/8 G5/4 r/8 C6/2." = note + octave, / length (4 = quarter,
//            8 = eighth), a dot makes it 1.5x longer, r = rest. Tempo in beats per minute.
//   Player   plays recorded samples, at any speed: step 1.5 plays 1.5x faster, so 1.5x higher
//            (the chipmunk). Between two samples it draws a straight line (linear interpolation).
//
// Everything works in floats from -1 to +1; the speaker turns them into 16-bit numbers at the end.
// Pure C++ (no Arduino): host-tested in kit/test/host/sound_synth_test.cpp.
#pragma once

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

namespace syn {

enum class Wave : uint8_t { Sine, Square, Triangle, Saw, Noise };

inline const char *waveName(Wave w) {
  switch (w) {
    case Wave::Square: return "square";
    case Wave::Triangle: return "triangle";
    case Wave::Saw: return "saw";
    case Wave::Noise: return "noise";
    default: return "sine";
  }
}

inline bool parseWave(const char *s, Wave &out) {
  static const char *const kNames[] = {"sine", "square", "triangle", "saw", "noise"};
  for (int i = 0; i < 5; i++)
    if (strcasecmp(s, kNames[i]) == 0) return out = Wave(i), true;
  return false;
}

// "A4" = 440 Hz, "C#5", "Bb3". 0 for a rest ("r") or anything we can't read.
inline float noteHz(const char *name, float a4 = 440.0f) {
  if (!name || !name[0]) return 0;
  static const int kSemis[] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G from C
  const char letter = char(toupper(name[0]));
  if (letter < 'A' || letter > 'G') return 0;
  int semi = kSemis[letter - 'A'], i = 1;
  if (name[i] == '#') semi++, i++;
  else if (name[i] == 'b') semi--, i++;
  if (!isdigit(name[i])) return 0;
  const int octave = name[i] - '0';
  const int midi = (octave + 1) * 12 + semi;
  return a4 * powf(2.0f, (midi - 69) / 12.0f);
}

struct Envelope {
  float attackMs = 6, decayMs = 80, sustain = 0.65f, releaseMs = 90;
};

class Voice {
 public:
  void start(float hz, float durMs, Wave w, const Envelope &env, float rate, float level = 1.0f) {
    hz_ = hz, wave_ = w, env_ = env, rate_ = rate, level_ = level;
    phase_ = 0;
    t_ = 0;
    dur_ = durMs * rate / 1000.0f;
    active_ = hz > 0 || w == Wave::Noise;
    silentUntil_ = active_ ? 0 : dur_;  // a rest still takes its time
  }
  void release() { dur_ = t_; }
  bool active() const { return active_ || t_ < silentUntil_; }
  float hz() const { return hz_; }

  float next() {
    if (!active_) {
      t_++;
      return 0;
    }
    const float ms = t_ * 1000.0f / rate_;
    const float relMs = t_ > dur_ ? (t_ - dur_) * 1000.0f / rate_ : 0;
    float a;
    if (relMs > 0) a = sustainAt(dur_ * 1000.0f / rate_) * (1.0f - relMs / env_.releaseMs);
    else a = sustainAt(ms);
    if (relMs >= env_.releaseMs || (relMs > 0 && a <= 0)) {  // a = 0 at the very start is the attack, not the end
      active_ = false;
      return 0;
    }
    t_++;
    phase_ += hz_ / rate_;
    phase_ -= floorf(phase_);
    return osc() * a * level_;
  }

 private:
  // The envelope level `ms` into the note, before the release.
  float sustainAt(float ms) const {
    if (ms < env_.attackMs) return ms / env_.attackMs;
    if (ms < env_.attackMs + env_.decayMs) return 1.0f - (1.0f - env_.sustain) * (ms - env_.attackMs) / env_.decayMs;
    return env_.sustain;
  }

  float osc() {
    switch (wave_) {
      case Wave::Square: return phase_ < 0.5f ? 0.6f : -0.6f;  // squares sound twice as loud: tamed
      case Wave::Triangle: return phase_ < 0.5f ? 4 * phase_ - 1 : 3 - 4 * phase_;
      case Wave::Saw: return (2 * phase_ - 1) * 0.7f;
      case Wave::Noise: noise_ = noise_ * 1664525u + 1013904223u; return (int32_t(noise_) >> 8) / 8388608.0f * 0.5f;
      default: return sinf(6.2831853f * phase_);
    }
  }

  float hz_ = 0, rate_ = 16000, level_ = 1, phase_ = 0, t_ = 0, dur_ = 0, silentUntil_ = 0;
  Wave wave_ = Wave::Sine;
  Envelope env_;
  bool active_ = false;
  uint32_t noise_ = 22222;
};

// Reads "C5/8 E5/8 G5/4 r/8 C6/2." one note at a time.
class Tune {
 public:
  void set(const char *text, int bpm) {
    text_ = at_ = text;
    bpm_ = bpm > 0 ? bpm : 120;
  }
  bool done() const { return !at_ || !*at_; }

  // The next note: false at the end. hz = 0 for a rest. ms = how long it lasts.
  bool next(float &hz, float &ms) {
    if (!at_) return false;
    while (*at_ == ' ' || *at_ == ',') at_++;
    if (!*at_) return false;
    char name[6] = {};
    int n = 0;
    while (*at_ && *at_ != '/' && *at_ != ' ' && n < 5) name[n++] = *at_++;
    int len = 4;
    if (*at_ == '/') len = int(strtol(at_ + 1, const_cast<char **>(&at_), 10));
    if (len <= 0) len = 4;
    float beats = 4.0f / len;
    if (*at_ == '.') beats *= 1.5f, at_++;
    while (*at_ && *at_ != ' ') at_++;  // skip anything we don't understand
    hz = (name[0] == 'r' || name[0] == 'R') ? 0 : noteHz(name);
    ms = beats * 60000.0f / bpm_;
    return true;
  }

 private:
  const char *text_ = nullptr, *at_ = nullptr;
  int bpm_ = 120;
};

class Player {
 public:
  void set(const int16_t *samples, uint32_t count, float step = 1.0f, float gain = 1.0f) {
    s_ = samples, n_ = count, step_ = step, gain_ = gain, pos_ = 0;
  }
  void stop() { n_ = 0; }
  bool active() const { return s_ && pos_ + 1 < n_; }
  float progress() const { return n_ ? float(pos_) / n_ : 0; }

  float next() {
    if (!active()) return 0;
    const uint32_t i = uint32_t(pos_);
    const float frac = float(pos_ - i);
    const float v = s_[i] + (s_[i + 1] - s_[i]) * frac;
    pos_ += step_;
    return v / 32768.0f * gain_;
  }

 private:
  const int16_t *s_ = nullptr;
  uint32_t n_ = 0;
  double pos_ = 0;
  float step_ = 1, gain_ = 1;
};

// Keeps the sum of several sounds inside -1..+1 without the harsh edge of plain clipping.
inline float softClip(float x) { return x > 1 ? 1 : x < -1 ? -1 : x * (1.5f - 0.5f * x * x); }

}  // namespace syn
