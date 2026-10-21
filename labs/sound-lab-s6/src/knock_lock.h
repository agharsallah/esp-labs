// The knock lock's rules: collect knocks into a pattern, then compare it with the secret.
// Pure C++ (no Arduino), host-tested in labs/sound-lab-s6/test/host/sound_games_test.cpp.
//
// A pattern ends when nobody has knocked for endMs (1.5 s). Setting the secret and trying to
// open use the same collector; only what happens to the finished pattern differs.
// The comparison itself is snd::matchRhythm (kit/lib/sound-kit/src/rhythm.h): gaps as shares of the
// whole pattern, so the speed doesn't matter, only the rhythm.
#pragma once

#include <stdint.h>

#include "rhythm.h"

namespace knock {

class Collector {
 public:
  uint32_t endMs = 1500;

  // A knock at sound-clock time ms. False when the pattern is already full (12 knocks).
  bool add(uint32_t ms) {
    last_ = ms;
    return open_.add(ms);
  }

  // Call every frame. True once, when the pattern is finished: it is copied to `done`.
  bool poll(uint32_t ms, snd::Rhythm &done) {
    if (open_.count == 0 || ms - last_ < endMs) return false;
    done = open_;
    open_.clear();
    return true;
  }

  const snd::Rhythm &open() const { return open_; }
  void clear() { open_.clear(); }

 private:
  snd::Rhythm open_;
  uint32_t last_ = 0;
};

}  // namespace knock
