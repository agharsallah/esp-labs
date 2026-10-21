// Hearing a clap (or a knock) and telling it from a voice.
//
// A clap is a bang: the level jumps from quiet to loud within one 4 ms frame, and is quiet again
// within about a tenth of a second. A word, a whistle or music climbs more slowly and stays loud.
// So the detector watches each frame's level above the noise floor (LevelReading::instAbove):
//
//   armed --(jumps above `riseDb`)--> ONSET --(falls below `releaseDb`)--> how long was it loud?
//                                        |                                   <= maxClapMs: CLAP
//                                        |                                   longer:       LONG (a voice?)
//   ...and it re-arms once the level has stayed below `releaseDb` for `quietMs`.
//
// Onset fires at once (games want that); Clap and Long only when the sound has ended (patterns
// want that). Then Groups collects claps that come close together: "clap clap ... (pause)" = 2.
//
// Pure C++: host-tested in kit/test/host/sound_dsp_test.cpp.
#pragma once

#include <stdint.h>

namespace snd {

struct OnsetConfig {
  float riseDb = 18.0f;      // how far above the quiet a clap must jump
  float releaseDb = 8.0f;    // below this the sound counts as over
  uint32_t maxClapMs = 160;  // loud for longer than this: not a clap
  uint32_t quietMs = 40;     // quiet this long before the next onset can fire
};

class Onsets {
 public:
  enum class Kind : uint8_t { None, Onset, Clap, Long };
  struct Event {
    Kind kind = Kind::None;
    uint32_t ms = 0;         // when it started
    uint32_t lengthMs = 0;   // Clap / Long: how long it stayed loud
    float strengthDb = 0;    // the loudest frame, dB above the floor
  };

  OnsetConfig config;

  // One frame. Returns at most one event (an Onset, or the end of one).
  Event push(float aboveDb, uint32_t ms) {
    Event e;
    switch (state_) {
      case State::Armed:
        if (aboveDb >= config.riseDb) {
          state_ = State::Loud;
          start_ = ms;
          peak_ = aboveDb;
          e = {Kind::Onset, ms, 0, aboveDb};
        }
        break;
      case State::Loud:
        if (aboveDb > peak_) peak_ = aboveDb;
        if (aboveDb < config.releaseDb) {
          state_ = State::Quiet;
          quietSince_ = ms;
          const uint32_t len = ms - start_;
          e = {len <= config.maxClapMs ? Kind::Clap : Kind::Long, start_, len, peak_};
        }
        break;
      case State::Quiet:
        if (aboveDb >= config.releaseDb) quietSince_ = ms;  // still ringing: wait again
        else if (ms - quietSince_ >= config.quietMs) state_ = State::Armed;
        break;
    }
    return e;
  }

  bool loud() const { return state_ == State::Loud; }

 private:
  enum class State : uint8_t { Armed, Loud, Quiet };
  State state_ = State::Armed;
  uint32_t start_ = 0, quietSince_ = 0;
  float peak_ = 0;
};

// Claps close together make a group; a pause ends it. "clap clap" = 2, "clap clap clap" = 3.
class Groups {
 public:
  static constexpr int kMax = 8;
  struct Group {
    int count = 0;             // 0 = no group finished
    uint32_t startMs = 0;
    uint32_t gapsMs[kMax - 1] = {};  // between clap i and i+1
  };

  uint32_t gapMs = 650;  // a pause longer than this ends the group

  void add(uint32_t ms) {
    if (open_.count > 0 && open_.count < kMax) open_.gapsMs[open_.count - 1] = ms - last_;
    if (open_.count == 0) open_.startMs = ms;
    if (open_.count < kMax) open_.count++;
    last_ = ms;
  }

  // Call every frame: returns the finished group once its pause has passed.
  Group poll(uint32_t ms) {
    if (open_.count == 0 || ms - last_ < gapMs) return Group{};
    const Group g = open_;
    open_ = Group{};
    return g;
  }

  int pending() const { return open_.count; }
  uint32_t lastMs() const { return last_; }
  void clear() { open_ = Group{}; }

 private:
  Group open_;
  uint32_t last_ = 0;
};

}  // namespace snd
