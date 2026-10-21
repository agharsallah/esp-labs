// When does the voice-activated recorder (S4) start and stop?
//
//   off ──arm──> ARMED ──louder than startDb──> RECORDING ──quiet for holdMs──> clip saved, ARMED again
//                  │                               │ (or maxMs reached, or `stop`)
//                  └──── rec (by hand) ────────────┘
//
// Pre-roll: the first syllable is already over by the time the level crosses startDb. The
// sampler keeps the last 4 s of sound in a ring anyway, so the clip simply starts prerollMs
// *before* the trigger. Hold: speech has short pauses; waiting holdMs of quiet keeps one
// sentence in one clip. Clips shorter than minMs (a door slam) are thrown away when automatic.
//
// Everything is counted in samples (the sampler's running sample number), so the recorder can
// copy exactly that stretch out of the ring. Pure C++: host-tested in kit/test/host/sound_dsp_test.cpp.
#pragma once

#include <stdint.h>

namespace snd {

struct TriggerConfig {
  float startDb = 15;        // dB above the quiet that starts a recording
  uint32_t holdMs = 1200;    // quiet for this long ends it
  uint32_t prerollMs = 500;  // keep this much from before the trigger
  uint32_t maxMs = 20000;    // longest clip
  uint32_t minMs = 400;      // automatic clips shorter than this are dropped
};

class Trigger {
 public:
  enum class State : uint8_t { Off, Armed, Recording };
  enum class Action : uint8_t { None, Start, Save, Drop };
  struct Result {
    Action action = Action::None;
    uint32_t from = 0, to = 0;  // sample numbers: Start = from; Save/Drop = from..to
  };

  TriggerConfig config;

  explicit Trigger(uint32_t rate) : rate_(rate) {}
  void setRate(uint32_t rate) { rate_ = rate; }

  void arm(bool on) {
    if (state_ != State::Recording) state_ = on ? State::Armed : State::Off;
    armed_ = on;
  }
  bool armed() const { return armed_; }
  State state() const { return state_; }
  bool manual() const { return manual_; }
  uint32_t from() const { return from_; }

  // Start now, by hand: also takes the pre-roll. `now` = the newest sample number.
  Result start(uint32_t now) {
    if (state_ == State::Recording) return {};
    manual_ = true;
    return begin(now);
  }

  // Stop now, by hand (or because the clip store is full).
  Result stop(uint32_t now) {
    if (state_ != State::Recording) return {};
    return end(now, Action::Save);
  }

  // One frame: its level above the quiet, and the sample number at its end.
  Result push(float aboveDb, uint32_t now) {
    switch (state_) {
      case State::Off: return {};
      case State::Armed:
        if (aboveDb >= config.startDb) {
          manual_ = false;
          return begin(now);
        }
        return {};
      case State::Recording: {
        if (aboveDb >= config.startDb * 0.6f) lastLoud_ = now;  // a little below the start: speech dips
        if (now - from_ >= samples(config.maxMs)) return end(now, Action::Save);
        if (!manual_ && now - lastLoud_ >= samples(config.holdMs)) {
          const uint32_t loudFor = lastLoud_ - trigger_;
          // Keep the hold's quiet tail short: half a second after the last loud frame is enough.
          const uint32_t to = lastLoud_ + samples(500) < now ? lastLoud_ + samples(500) : now;
          return end(to, loudFor + samples(100) >= samples(config.minMs) ? Action::Save : Action::Drop);
        }
        return {};
      }
    }
    return {};
  }

 private:
  uint32_t samples(uint32_t ms) const { return uint32_t(uint64_t(ms) * rate_ / 1000); }

  Result begin(uint32_t now) {
    const uint32_t pre = samples(config.prerollMs);
    from_ = now > pre ? now - pre : 0;
    trigger_ = lastLoud_ = now;
    state_ = State::Recording;
    return {Action::Start, from_, now};
  }

  Result end(uint32_t to, Action a) {
    state_ = armed_ ? State::Armed : State::Off;
    manual_ = false;
    return {a, from_, to};
  }

  uint32_t rate_;
  State state_ = State::Off;
  bool armed_ = false, manual_ = false;
  uint32_t from_ = 0, trigger_ = 0, lastLoud_ = 0;
};

}  // namespace snd
