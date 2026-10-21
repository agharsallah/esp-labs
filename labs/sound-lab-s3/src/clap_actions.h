// S3's switchboard: what each clap pattern does. Pure C++ (no Arduino), host-tested in
// labs/sound-lab-s6/test/host/sound_games_test.cpp.
//
// A pattern is simply the number of claps in a group (1-4, see snd::Groups). Each one maps to an
// action. The map lives in NVS so it survives a reset; these are the defaults.
#pragma once

#include <stdint.h>
#include <string.h>

namespace claps {

// New actions go at the end (before Count): the map in NVS stores these numbers.
enum class Action : uint8_t { None, Led, Colour, Dark, Screen, View, Party, Tune, Count };

struct ActionInfo {
  const char *name;  // what you type: map 2 colour
  const char *what;  // for `maps` and the patterns view
};

inline const ActionInfo &info(Action a) {
  static const ActionInfo kInfo[] = {
      {"none", "nothing"},
      {"led", "toggle the LED on GPIO 4"},
      {"colour", "next colour on the RGB LED"},
      {"dark", "RGB LED off"},
      {"screen", "backlight off / on"},
      {"view", "next screen view"},
      {"party", "2 s rainbow"},
      {"tune", "play a tune on the speaker"},
  };
  const int i = int(a) < int(Action::Count) ? int(a) : 0;
  return kInfo[i];
}

// "color" is accepted too. Returns false for an unknown word.
inline bool parse(const char *word, Action &out) {
  if (strcmp(word, "color") == 0) return out = Action::Colour, true;
  for (int i = 0; i < int(Action::Count); i++)
    if (strcmp(word, info(Action(i)).name) == 0) return out = Action(i), true;
  return false;
}

constexpr int kPatterns = 4;  // 1, 2, 3 or 4 claps

struct Map {
  Action action[kPatterns + 1] = {Action::None, Action::Led, Action::Colour, Action::Screen, Action::Party};
  // The action for a group of n claps: more than 4 counts as none (probably applause).
  Action forClaps(int n) const { return n >= 1 && n <= kPatterns ? action[n] : Action::None; }
};

// Learn: the threshold that still catches your softest clap, with room to spare (60 %), kept
// between 8 dB (anything quieter would fire on speech) and 40 dB.
inline float suggestRise(const float *strengths, int n) {
  if (n <= 0) return 18.0f;
  float soft = strengths[0];
  for (int i = 1; i < n; i++) soft = strengths[i] < soft ? strengths[i] : soft;
  const float s = soft * 0.6f;
  return s < 8 ? 8 : s > 40 ? 40 : s;
}

}  // namespace claps
