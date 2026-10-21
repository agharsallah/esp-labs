// S8's effects: the pure arithmetic, no Arduino (host-tested in labs/sound-lab-s8/test/host/sound_fx_test.cpp).
//
//   DelayLine   an echo. Every sample goes into a ring; what comes out is the sample from `delay`
//               ago. Feed some of the output back in (feedback) and each echo has its own echo,
//               quieter each time: 50 % feedback = every repeat 6 dB softer. At 100 % it never dies.
//   RingMod     the robot. Multiply the voice by a slow sine (the carrier, 30-80 Hz): every
//               frequency f in the voice becomes two, f - carrier and f + carrier. Those new
//               frequencies are not in harmony with the voice any more: it sounds metallic.
//   bitcrush    fewer bits (a coarser staircase) and holding each value for k samples (a lower rate
//               without a filter): the gritty sound of old game consoles.
//   reverse     the recording backwards.
//   HowlDetector  the microphone hears the speaker. If what comes out is louder than what went in,
//               the sound goes round and round, louder each time: the howl (acoustic feedback).
//               It shows up as a level that keeps climbing, or stays very loud. Caught here.
//
// Changing the speed of a recording changes its pitch AND its length together (the chipmunk speaks
// fast). Shifting pitch while keeping the speed needs more DSP (cutting the sound into overlapping
// grains, or a phase vocoder): not here.
#pragma once

#include <math.h>
#include <stdint.h>

#include <algorithm>

namespace fx {

inline int16_t clamp16(float v) { return int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

class DelayLine {
 public:
  // buf must hold at least maxSamples int16 values; it is owned by the caller (PSRAM on the board).
  void begin(int16_t *buf, uint32_t maxSamples) {
    buf_ = buf, max_ = maxSamples, at_ = 0;
    clear();
  }
  void clear() {
    if (buf_) std::fill_n(buf_, max_, int16_t(0));
  }
  void set(uint32_t delaySamples, float feedback) {
    delay_ = std::min(std::max<uint32_t>(delaySamples, 1), max_);
    feedback_ = std::min(std::max(feedback, 0.0f), 0.95f);
  }
  uint32_t delay() const { return delay_; }
  float feedback() const { return feedback_; }

  // One sample in, the echo out (only the delayed part: the dry voice is already in the room).
  int16_t process(int16_t x) {
    const uint32_t readAt = (at_ + max_ - delay_) % max_;
    const int16_t echo = buf_[readAt];
    buf_[at_] = clamp16(x + echo * feedback_);
    at_ = (at_ + 1) % max_;
    return echo;
  }

 private:
  int16_t *buf_ = nullptr;
  uint32_t max_ = 1, at_ = 0, delay_ = 1;
  float feedback_ = 0;
};

class RingMod {
 public:
  void set(float carrierHz, float rate) { step_ = carrierHz / rate; }
  int16_t process(int16_t x) {
    phase_ += step_;
    phase_ -= floorf(phase_);
    return int16_t(x * sinf(6.2831853f * phase_));
  }

 private:
  float phase_ = 0, step_ = 50.0f / 16000.0f;
};

// Keep `bits` bits of 16 and hold every value for `hold` samples. Stateful: call per sample.
class Bitcrush {
 public:
  void set(int bits, int hold) {
    bits_ = std::min(std::max(bits, 1), 16);
    hold_ = std::max(hold, 1);
  }
  int16_t process(int16_t x) {
    if (count_++ % hold_ == 0) {
      const int shift = 16 - bits_;
      held_ = int16_t((x >> shift) << shift);
    }
    return held_;
  }

 private:
  int bits_ = 16, hold_ = 1, count_ = 0;
  int16_t held_ = 0;
};

inline void reverse(int16_t *s, uint32_t n) { std::reverse(s, s + n); }

// The biggest |sample| (for normalising).
inline int peakOf(const int16_t *s, uint32_t n) {
  int p = 0;
  for (uint32_t i = 0; i < n; i++) p = std::max(p, std::abs(int(s[i])));
  return p;
}

// Watches the microphone level (dBFS) of every block while the live effects run.
// Howl = the level has kept climbing for `riseMs`, or stayed above `loudDb` for `loudMs`.
class HowlDetector {
 public:
  float loudDb = -12;       // this loud for this long = howl
  uint32_t loudMs = 300;
  uint32_t riseMs = 300;    // climbing this long = howl
  float minDb = -40;        // quieter than this is never a howl

  // One block: its level and its length. True once a howl is found (until reset()).
  bool push(float db, uint32_t blockMs) {
    if (db >= loudDb) loudFor_ += blockMs;
    else loudFor_ = 0;
    // "Climbing" allows small dips: a voice wobbles 3 dB, a howl only goes up.
    if (db > minDb && db >= best_ - 1.0f) {
      risingFor_ += blockMs;
      best_ = std::max(best_, db);
    } else {
      risingFor_ = 0;
      best_ = db;
    }
    if (loudFor_ >= loudMs || (risingFor_ >= riseMs && best_ - startOfRise() >= 9)) howl_ = true;
    if (risingFor_ == blockMs || risingFor_ == 0) riseStart_ = db;
    return howl_;
  }
  bool howling() const { return howl_; }
  void reset() {
    howl_ = false;
    loudFor_ = risingFor_ = 0;
    best_ = riseStart_ = -120;
  }

 private:
  float startOfRise() const { return riseStart_; }
  bool howl_ = false;
  uint32_t loudFor_ = 0, risingFor_ = 0;
  float best_ = -120, riseStart_ = -120;
};

}  // namespace fx
