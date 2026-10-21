// Host test for labs/sound-lab-s5/src/pitch.h: notes with a tuning reference, steering, band floors.
//   c++ -std=c++17 -Wall -I kit/lib/sound-kit/src -I labs/sound-lab-s5/src labs/sound-lab-s5/test/host/sound_spectrum_test.cpp -o /tmp/sound_spectrum_test && /tmp/sound_spectrum_test
#include <cmath>
#include <cstdio>
#include <cstring>

#include "pitch.h"

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++; \
  } while (0)

int main() {
  CHECK(std::strcmp(s5::noteFor(440).name, "A4") == 0);
  CHECK(std::strcmp(s5::noteFor(442, 442).name, "A4") == 0 && std::fabs(s5::noteFor(442, 442).cents) < 0.1f);
  CHECK(s5::noteFor(440, 442).cents < -7 && s5::noteFor(440, 442).cents > -9);  // 440 is ~8 cents flat of A=442
  CHECK(std::strcmp(s5::noteFor(1046.5f).name, "C6") == 0);
  CHECK(std::strcmp(s5::noteFor(277.18f).name, "C#4") == 0);
  CHECK(std::fabs(s5::midiOf(880) - 81) < 0.01f);

  CHECK(s5::steerFraction(300, 600, 2400) == 0 && s5::steerFraction(5000, 600, 2400) == 1);
  CHECK(std::fabs(s5::steerFraction(1200, 600, 2400) - 0.5f) < 0.001f);  // log scale: the octave in the middle

  s5::BandFloors<2> f;
  const float quiet[2] = {-70, -80}, tone[2] = {-40, -80};
  for (int i = 0; i < 10; i++) f.update(quiet, 0.04f);
  CHECK(std::fabs(f.floor[0] + 70) < 0.01f && f.above(0) == 0);
  f.update(tone, 0.04f);
  CHECK(std::fabs(f.above(0) - 30) < 0.1f && f.above(1) < 0.1f);
  f.relearn();
  f.update(tone, 0.04f);
  CHECK(std::fabs(f.floor[0] + 40) < 0.01f);

  if (failures == 0) std::printf("sound spectrum: all checks pass\n");
  return failures ? 1 : 0;
}
