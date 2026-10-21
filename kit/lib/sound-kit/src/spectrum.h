// Which frequencies are in the sound? Our own FFT (fast Fourier transform), no library.
//
// A sound is many waves at once. The FFT takes N samples (say 512 = 32 ms at 16 kHz) and
// answers, for each of N/2 "bins", how strong the wave at that frequency is. Bin k is
// k x rate / N Hz wide steps: 16000 / 512 = 31.25 Hz apart. More samples = finer bins, but a
// slower answer (you wait N samples for each one).
//
// The window: the FFT pretends the N samples repeat forever. Where the end doesn't meet the
// start, that jump smears energy into every bin ("leakage"). Multiplying by a Hann window (a hump
// that is 0 at both ends) removes the jump.
//
// Radix-2: split the samples into even and odd, solve each half, combine with "butterflies".
// log2(N) passes of N/2 butterflies: 512 points = 9 x 256 = 2304 butterflies, ~0.2 ms on the S3.
//
// Pure C++: host-tested in kit/test/host/sound_dsp_test.cpp.
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>

namespace snd {

class Fft {
 public:
  static constexpr int kMaxN = 1024;

  // n must be a power of two, 64-1024.
  bool setSize(int n) {
    if (n < 64 || n > kMaxN || (n & (n - 1))) return false;
    n_ = n;
    bits_ = 0;
    while ((1 << bits_) < n) bits_++;
    for (int i = 0; i < n / 2; i++) {
      cos_[i] = cosf(2.0f * float(M_PI) * i / n);
      sin_[i] = -sinf(2.0f * float(M_PI) * i / n);
    }
    for (int i = 0; i < n; i++) window_[i] = 0.5f - 0.5f * cosf(2.0f * float(M_PI) * i / (n - 1));
    return true;
  }

  int size() const { return n_; }
  int bins() const { return n_ / 2; }

  // Window + transform n int16 samples. Then magnitude(k) / db(k) for k < bins().
  void run(const int16_t *samples) {
    for (int i = 0; i < n_; i++) re_[reverse(i)] = samples[i] * window_[i], im_[reverse(i)] = 0;
    for (int half = 1, step = n_ / 2; half < n_; half *= 2, step /= 2)
      for (int start = 0; start < n_; start += 2 * half)
        for (int k = 0; k < half; k++) {
          const int a = start + k, b = a + half;
          const float c = cos_[k * step], s = sin_[k * step];
          const float tr = re_[b] * c - im_[b] * s, ti = re_[b] * s + im_[b] * c;
          re_[b] = re_[a] - tr, im_[b] = im_[a] - ti;
          re_[a] += tr, im_[a] += ti;
        }
    // Scale so a full-scale sine reads about 0 dB: 2 / N for one side, x2 for the Hann window's loss.
    const float scale = 4.0f / (n_ * 32768.0f);
    for (int k = 0; k < n_ / 2; k++) mag_[k] = sqrtf(re_[k] * re_[k] + im_[k] * im_[k]) * scale;
  }

  float magnitude(int k) const { return mag_[k]; }
  float db(int k) const { return mag_[k] < 1e-6f ? -120.0f : 20.0f * log10f(mag_[k]); }
  float binHz(int k, float rate) const { return k * rate / n_; }

  // The strongest bin between lo and hi Hz, refined between bins with a parabola through its
  // neighbours. clarity = how far it stands above the average in that range (dB).
  struct Peak {
    float hz = 0;
    float db = -120;
    float clarity = 0;
  };
  Peak peak(float rate, float loHz, float hiHz) const {
    const int lo = std::max(1, int(loHz * n_ / rate)), hi = std::min(n_ / 2 - 2, int(hiHz * n_ / rate));
    Peak p;
    if (hi <= lo) return p;
    int best = lo;
    float sum = 0;
    for (int k = lo; k <= hi; k++) {
      sum += mag_[k];
      if (mag_[k] > mag_[best]) best = k;
    }
    const float a = mag_[best - 1], b = mag_[best], c = mag_[best + 1];
    const float den = a - 2 * b + c;
    const float shift = den != 0 ? 0.5f * (a - c) / den : 0;
    p.hz = (best + shift) * rate / n_;
    p.db = db(best);
    const float avg = sum / (hi - lo + 1);
    p.clarity = avg > 1e-9f ? 20.0f * log10f(b / avg) : 0;
    return p;
  }

  // Bands spaced evenly in octaves (how we hear) from loHz to hiHz: the strongest bin in each, dB.
  void bands(float rate, float loHz, float hiHz, float *outDb, int count) const {
    for (int i = 0; i < count; i++) {
      const float f0 = loHz * powf(hiHz / loHz, float(i) / count), f1 = loHz * powf(hiHz / loHz, float(i + 1) / count);
      int k0 = std::max(1, int(f0 * n_ / rate)), k1 = std::min(n_ / 2 - 1, int(f1 * n_ / rate));
      if (k1 < k0) k1 = k0;
      float m = 0;
      for (int k = k0; k <= k1; k++) m = std::max(m, mag_[k]);
      outDb[i] = m < 1e-6f ? -120.0f : 20.0f * log10f(m);
    }
  }

 private:
  int reverse(int i) const {
    int r = 0;
    for (int b = 0; b < bits_; b++) r |= ((i >> b) & 1) << (bits_ - 1 - b);
    return r;
  }

  int n_ = 0, bits_ = 0;
  float re_[kMaxN], im_[kMaxN], window_[kMaxN], mag_[kMaxN / 2];
  float cos_[kMaxN / 2], sin_[kMaxN / 2];
};

// Musical notes: A4 = 440 Hz by default, every semitone x 2^(1/12). midi 69 = A4.
struct Note {
  int midi = 0;
  float cents = 0;  // -50..+50: how far off the nearest note (100 cents = one semitone)
  char name[8] = "";  // "A4", "C#5"
};

// a4 = the tuning reference (440 Hz is the standard; some orchestras tune to 442).
inline Note noteOf(float hz, float a4 = 440.0f) {
  Note n;
  if (hz <= 0) return n;
  const float m = 69.0f + 12.0f * log2f(hz / a4);
  n.midi = int(lroundf(m));
  n.cents = (m - n.midi) * 100.0f;
  static const char *const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  snprintf(n.name, sizeof(n.name), "%s%d", kNames[((n.midi % 12) + 12) % 12], n.midi / 12 - 1);
  return n;
}

}  // namespace snd
