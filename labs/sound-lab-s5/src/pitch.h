// S5's pure helpers: notes against any tuning reference, pitch -> a 0-1 position, band noise floors.
//
// Tuning: most music tunes A4 to 440 Hz, some orchestras to 442 or 443. `a4` moves every note.
// Steering: we hear pitch in ratios (an octave = x2), so the position is logarithmic: halfway
// between 600 and 2400 Hz is 1200 Hz, not 1500.
//
// Pure C++: host-tested in labs/sound-lab-s5/test/host/sound_spectrum_test.cpp.
#pragma once

#include <math.h>
#include <stdio.h>

#include "spectrum.h"

namespace s5 {

// Like snd::noteOf, with a tuning reference for A4.
inline snd::Note noteFor(float hz, float a4 = 440.0f) {
  snd::Note n;
  if (hz <= 0 || a4 <= 0) return n;
  const float m = 69.0f + 12.0f * log2f(hz / a4);
  n.midi = int(lroundf(m));
  n.cents = (m - n.midi) * 100.0f;
  static const char *const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  // Octave 0-9 as one character, so the name always fits ("C#9" + end).
  const int octave = n.midi < 12 ? 0 : n.midi > 131 ? 9 : n.midi / 12 - 1;
  snprintf(n.name, sizeof(n.name), "%s%c", kNames[((n.midi % 12) + 12) % 12], char('0' + octave));
  return n;
}

// The continuous pitch in semitones (69 = A4), for trails.
inline float midiOf(float hz, float a4 = 440.0f) { return hz > 0 ? 69.0f + 12.0f * log2f(hz / a4) : 0; }

// Where hz sits between lo and hi on a log scale: 0 at lo, 1 at hi, clamped.
inline float steerFraction(float hz, float lo, float hi) {
  if (hz <= lo) return 0;
  if (hz >= hi) return 1;
  return logf(hz / lo) / logf(hi / lo);
}

// The noise floor of each band: what that band reads when nothing is playing. A fan hums low, a
// laptop whines high, so each band learns its own quiet: it drops at once and creeps up 0.5 dB/s.
// The views then show dB above each band's quiet, which hides the mic's flat hiss.
template <int N>
struct BandFloors {
  float db[N] = {};     // newest reading per band, dBFS
  float floor[N] = {};  // the learned quiet per band, dBFS
  int learned = 0;

  void update(const float *now, float dtSeconds) {
    for (int i = 0; i < N; i++) {
      db[i] = now[i];
      if (learned < 10) floor[i] += (now[i] - floor[i]) / float(learned + 1);  // the average of the first 10
      else if (now[i] < floor[i]) floor[i] = now[i];
      else floor[i] += 0.5f * dtSeconds;
    }
    if (learned < 10) learned++;
  }
  float above(int i) const { return db[i] > floor[i] ? db[i] - floor[i] : 0; }
  void relearn() { learned = 0; }
};

}  // namespace s5
