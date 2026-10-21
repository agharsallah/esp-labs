// How loud is it? Decibels from the microphone's samples, the way a sound level meter shows them.
//
// The sampler (mic.h) cuts the sound into frames of 64 samples (4 ms at 16 kHz) and hands each
// frame's RMS (root mean square: the "average strength" of a wave that swings + and -) and peak.
//
// Decibels: our ears hear ratios, not differences. dB = 20 x log10(level / reference), so every
// +6 dB doubles the wave's height and +20 dB is ten times. Two scales here:
//   dBFS     relative to the ADC's full scale (0 dBFS = the loudest the ADC can show). Always <= 0.
//   above    dB above the quietest your room gets (the noise floor). 0 = silence, 20 = talking
//            close by, 40+ = a clap at arm's length. This is the number the labs mostly use.
// Neither is "dB SPL" (what a calibrated meter shows): that would need a reference microphone.
//
// Fast and slow: real meters smooth the level with a 125 ms ("fast") or 1 s ("slow") time
// constant. We smooth the power (RMS squared) the same way, then convert to dB.
//
// The noise floor follows the quiet: it drops at once to anything quieter and creeps up by
// 0.5 dB a second, so a minute of steady noise (a fan) becomes the new "quiet". `quiet` re-learns it.
//
// Pure C++ (no Arduino): host-tested in kit/test/host/sound_dsp_test.cpp.
#pragma once

#include <math.h>
#include <stdint.h>

namespace snd {

constexpr float kFloorDb = -96.0f;  // what silence (an RMS below 1 of 32768) reads as

inline float toDb(float rms) { return rms < 0.5f ? kFloorDb : 20.0f * log10f(rms / 32768.0f); }
inline float powerToDb(float power) { return power < 0.25f ? kFloorDb : 10.0f * log10f(power / (32768.0f * 32768.0f)); }

// Smoothing factor for a time constant, for frames of frameMs.
inline float alphaFor(float tauMs, float frameMs) { return 1.0f - expf(-frameMs / tauMs); }

struct LevelReading {
  float instDb = kFloorDb;  // this frame alone (4 ms): claps and knocks live here
  float fastDb = kFloorDb;  // 125 ms
  float slowDb = kFloorDb;  // 1 s
  float peakDb = kFloorDb;  // the highest instDb lately, held 1.5 s then falling 20 dB/s
  float floorDb = 0;         // the noise floor
  float above() const { return fastDb > floorDb ? fastDb - floorDb : 0.0f; }
  float instAbove() const { return instDb > floorDb ? instDb - floorDb : 0.0f; }
  float slowAbove() const { return slowDb > floorDb ? slowDb - floorDb : 0.0f; }
};

class Level {
 public:
  explicit Level(float frameMs = 4.0f)
      : frameMs_(frameMs), aFast_(alphaFor(125, frameMs)), aSlow_(alphaFor(1000, frameMs)), floorRise_(0.5f * frameMs / 1000.0f) {}

  // One frame: its RMS (in int16 units) and the time it ended (ms).
  void push(float rms, uint32_t ms) {
    const float power = rms * rms;
    if (frames_ == 0) fastP_ = slowP_ = power;
    fastP_ += aFast_ * (power - fastP_);
    slowP_ += aSlow_ * (power - slowP_);
    r_.instDb = toDb(rms);
    r_.fastDb = powerToDb(fastP_);
    r_.slowDb = powerToDb(slowP_);

    // Peak hold: jump up at once, hold 1.5 s, then fall 20 dB a second.
    if (r_.instDb >= r_.peakDb) r_.peakDb = r_.instDb, peakAt_ = ms;
    else if (ms - peakAt_ > 1500) r_.peakDb -= 20.0f * frameMs_ / 1000.0f;

    // Noise floor: the average power of the first half second, then it follows the quiet.
    if (learned_ < learnFrames()) learnP_ += power, r_.floorDb = powerToDb(learnP_ / float(++learned_));
    else if (r_.fastDb < r_.floorDb) r_.floorDb = r_.fastDb;
    else r_.floorDb += floorRise_;
    frames_++;
  }

  // Forget the floor: the next half second becomes the new quiet.
  void relearn() {
    learned_ = 0;
    learnP_ = 0;
  }
  bool learning() const { return learned_ < learnFrames(); }

  const LevelReading &reading() const { return r_; }

 private:
  uint32_t learnFrames() const { return uint32_t(500 / frameMs_); }

  float frameMs_, aFast_, aSlow_, floorRise_;
  float fastP_ = 0, slowP_ = 0, learnP_ = 0;
  uint32_t frames_ = 0, learned_ = 0, peakAt_ = 0;
  LevelReading r_;
};

// A 0-1 position for a meter that shows `rangeDb` dB above the floor.
inline float meterFraction(float aboveDb, float rangeDb = 60.0f) {
  const float f = aboveDb / rangeDb;
  return f < 0 ? 0 : f > 1 ? 1 : f;
}

}  // namespace snd
