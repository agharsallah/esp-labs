// B7's little music engine: scales, three public-domain tunes, a 16-step drum machine, and a
// scheduler that turns "note at time t" into MIDI messages at the right moment.
//
// Nothing here makes sound. Every note becomes a MIDI message for the synth on the other side
// (GarageBand, the ble-app synth) and a line on the board's piano roll.
#pragma once

#include <Arduino.h>

#include "midi_ble.h"

namespace b7 {

// ---- Scales ------------------------------------------------------------------------------------

enum class Scale : uint8_t { Pentatonic, Minor, Major, Blues };

struct ScaleInfo {
  const char *name;
  uint8_t steps[7];
  uint8_t count;
};

inline const ScaleInfo &scaleInfo(Scale s) {
  // Semitones above the root. The major pentatonic has no "wrong" notes: any order sounds fine.
  static const ScaleInfo scales[] = {
      {"pentatonic", {0, 2, 4, 7, 9}, 5},
      {"minor pentatonic", {0, 3, 5, 7, 10}, 5},
      {"major", {0, 2, 4, 5, 7, 9, 11}, 7},
      {"blues", {0, 3, 5, 6, 7, 10}, 6},
  };
  return scales[static_cast<int>(s)];
}

// Degree 0, 1, 2 ... of the scale, climbing into higher octaves.
inline uint8_t degreeNote(Scale s, int octave, int degree) {
  const ScaleInfo &i = scaleInfo(s);
  const int note = (octave + 1) * 12 + (degree / i.count) * 12 + i.steps[degree % i.count];
  return uint8_t(constrain(note, 0, 127));
}

// ---- Tunes (public domain) ------------------------------------------------------------------------

struct Step {
  int8_t note;   // semitones from C of the chosen octave; -1 = rest
  uint8_t eighths;  // length in eighth notes
};

struct Tune {
  const char *name;
  const Step *steps;
  uint8_t count;
};

namespace tunes {
// Twinkle Twinkle Little Star
constexpr Step kTwinkle[] = {{0, 2}, {0, 2}, {7, 2}, {7, 2}, {9, 2}, {9, 2}, {7, 4}, {5, 2}, {5, 2}, {4, 2}, {4, 2}, {2, 2}, {2, 2}, {0, 4}};
// Ode to Joy (Beethoven)
constexpr Step kOde[] = {{4, 2}, {4, 2}, {5, 2}, {7, 2}, {7, 2}, {5, 2}, {4, 2}, {2, 2}, {0, 2}, {0, 2}, {2, 2}, {4, 2}, {4, 3}, {2, 1}, {2, 4}};
// Frere Jacques
constexpr Step kFrere[] = {{0, 2}, {2, 2}, {4, 2}, {0, 2}, {0, 2}, {2, 2}, {4, 2}, {0, 2}, {4, 2}, {5, 2}, {7, 4}, {4, 2}, {5, 2}, {7, 4}};
}  // namespace tunes

inline const Tune *findTune(const String &name) {
  static const Tune all[] = {{"twinkle", tunes::kTwinkle, sizeof(tunes::kTwinkle) / sizeof(Step)},
                             {"ode", tunes::kOde, sizeof(tunes::kOde) / sizeof(Step)},
                             {"frere", tunes::kFrere, sizeof(tunes::kFrere) / sizeof(Step)}};
  for (const Tune &t : all)
    if (name == t.name) return &t;
  return nullptr;
}

// ---- Drum machine ------------------------------------------------------------------------------

// General MIDI drums live on channel 10 (index 9): each note number is one drum.
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kDrumNotes[4] = {36, 38, 42, 39};  // kick, snare, closed hi-hat, clap
constexpr const char *kDrumNames[4] = {"kick", "snare", "hat", "clap"};

struct Beat {
  const char *name;
  uint16_t rows[4];  // bit i = step i (of 16 sixteenth notes)
};

inline const Beat *findBeat(const String &name) {
  // Read the bits right to left: step 0 is bit 0.
  static const Beat beats[] = {
      {"rock", {0x0505, 0x1010, 0x5555, 0x0000}},
      {"funk", {0x2481, 0x9010, 0xFFFF, 0x0000}},
      {"house", {0x1111, 0x0000, 0x4444, 0x1010}},
      {"boom", {0x0281, 0x1010, 0x5555, 0x0000}},
  };
  for (const Beat &b : beats)
    if (name == b.name) return &b;
  return nullptr;
}

// ---- The scheduler -----------------------------------------------------------------------------

// Notes waiting for their moment: loop() asks for the due ones every frame and sends them in one go.
class Scheduler {
 public:
  bool at(uint32_t when, const midi::Msg &m) {
    if (count_ >= kMax) return false;
    q_[count_++] = {when, m};
    return true;
  }
  // Due messages into out (up to max), in time order. Stamps them with the send time.
  int due(uint32_t now, midi::Msg *out, int max) {
    int n = 0;
    while (n < max) {
      int best = -1;
      for (int i = 0; i < count_; i++)
        if (int32_t(now - q_[i].when) >= 0 && (best < 0 || int32_t(q_[i].when - q_[best].when) < 0)) best = i;
      if (best < 0) break;
      out[n] = q_[best].m;
      out[n].ts = uint16_t(now & 0x1FFF);
      n++;
      q_[best] = q_[--count_];
    }
    return n;
  }
  // Drop everything still waiting on a channel (or every channel, -1).
  void clear(int channel = -1) {
    for (int i = 0; i < count_;)
      if (channel < 0 || (q_[i].m.status & 15) == channel) q_[i] = q_[--count_];
      else i++;
  }
  int size() const { return count_; }

 private:
  static constexpr int kMax = 96;
  struct Item {
    uint32_t when;
    midi::Msg m;
  };
  Item q_[kMax];
  int count_ = 0;
};

}  // namespace b7
