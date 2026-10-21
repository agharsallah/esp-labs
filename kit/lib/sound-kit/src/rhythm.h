// Rhythm: a secret knock (S6's knock lock) and the beat of music (S6's party).
//
// Knock lock: the secret is the gaps between your knocks, not the knocks themselves, and not the
// speed: we divide every gap by the whole pattern's length, so "knock-knock ... knock" played a bit
// faster or slower still matches. Each gap may be off by `tolerance` (a share of the whole length).
//
// Beat: music's kick drum makes the level jump above its own recent average. A beat is a frame
// whose short-term energy is `ratio` times the last second's average (and clearly above the
// quiet), at least `minGapMs` after the previous one. The tempo is the median gap of the last 8.
//
// Pure C++: host-tested in kit/test/host/sound_dsp_test.cpp.
#pragma once

#include <math.h>
#include <stdint.h>

#include <algorithm>

namespace snd {

struct Rhythm {
  static constexpr int kMax = 12;  // knocks
  int count = 0;
  uint32_t atMs[kMax] = {};

  void clear() { count = 0; }
  bool add(uint32_t ms) {
    if (count >= kMax) return false;
    atMs[count++] = ms;
    return true;
  }
  uint32_t lengthMs() const { return count > 1 ? atMs[count - 1] - atMs[0] : 0; }
  // Gap i (0-based) as a share of the whole pattern, 0-1.
  float share(int i) const { return lengthMs() ? float(atMs[i + 1] - atMs[i]) / float(lengthMs()) : 0; }
};

struct RhythmMatch {
  bool ok = false;
  int score = 0;          // 0-100
  float worst = 1;        // the biggest gap error, as a share of the length
  const char *why = "";   // when not ok
};

inline RhythmMatch matchRhythm(const Rhythm &secret, const Rhythm &attempt, float tolerance = 0.12f) {
  RhythmMatch m;
  if (secret.count < 2) return m.why = "no secret yet", m;
  if (attempt.count != secret.count) return m.why = attempt.count < secret.count ? "too few knocks" : "too many knocks", m;
  float worst = 0, sum = 0;
  for (int i = 0; i + 1 < secret.count; i++) {
    const float err = fabsf(secret.share(i) - attempt.share(i));
    worst = std::max(worst, err);
    sum += err;
  }
  m.worst = worst;
  m.score = int(std::max(0.0f, 100.0f - sum / float(secret.count - 1) * 400.0f));
  m.ok = worst <= tolerance;
  m.why = m.ok ? "" : "the rhythm is off";
  return m;
}

class BeatTracker {
 public:
  float ratio = 1.6f;       // energy jump needed
  float minAboveDb = 10;    // and this far above the quiet
  uint32_t minGapMs = 280;  // fastest tempo: ~214 BPM

  // One frame: its RMS and level above the floor. True on a beat.
  bool push(float rms, float aboveDb, uint32_t ms) {
    const float e = rms * rms;
    // Short-term energy: the last ~3 frames. Long-term: about one second.
    shortE_ += 0.35f * (e - shortE_);
    longE_ += 0.004f * (e - longE_);
    const bool beat = shortE_ > ratio * longE_ && aboveDb >= minAboveDb && ms - last_ >= minGapMs;
    if (!beat) return false;
    if (last_ != 0 && ms - last_ < 2000) gaps_[n_++ % kGaps] = ms - last_;
    last_ = ms;
    beats_++;
    return true;
  }

  // The tempo from the median gap of the last few beats, 0 while unsure.
  int bpm() const {
    const int n = std::min<uint32_t>(n_, kGaps);
    if (n < 4) return 0;
    uint32_t g[kGaps];
    std::copy(gaps_, gaps_ + n, g);
    std::nth_element(g, g + n / 2, g + n);
    return g[n / 2] ? int(60000 / g[n / 2]) : 0;
  }
  uint32_t lastMs() const { return last_; }
  uint32_t beats() const { return beats_; }
  void reset() {  // the state only; the settings stay
    shortE_ = longE_ = 0;
    last_ = beats_ = n_ = 0;
  }

 private:
  static constexpr int kGaps = 8;
  float shortE_ = 0, longE_ = 0;
  uint32_t last_ = 0, beats_ = 0, n_ = 0;
  uint32_t gaps_[kGaps] = {};
};

}  // namespace snd
