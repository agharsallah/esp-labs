// Host test for kit/lib/sound-kit/src/synth.h: notes, tunes, voices and the sample player, on the Mac.
//   c++ -std=c++17 -Wall -I kit/lib/sound-kit/src kit/test/host/sound_synth_test.cpp -o /tmp/sound_synth_test && /tmp/sound_synth_test
#include <cmath>
#include <cstdio>

#include "synth.h"

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++; \
  } while (0)

using namespace syn;

int main() {
  CHECK(std::fabs(noteHz("A4") - 440) < 0.01f);
  CHECK(std::fabs(noteHz("C4") - 261.63f) < 0.05f);
  CHECK(std::fabs(noteHz("C#5") - noteHz("Db5")) < 0.01f);
  CHECK(std::fabs(noteHz("A5") - 880) < 0.05f);
  CHECK(noteHz("r") == 0 && noteHz("H4") == 0 && noteHz("C") == 0);
  Wave w;
  CHECK(parseWave("Square", w) && w == Wave::Square && !parseWave("pulse", w));

  // A tune: C5 quarter at 120 BPM = 500 ms, a dotted eighth = 375 ms, a rest.
  Tune t;
  t.set("C5/4 E5/8. r/2", 120);
  float hz = 0, ms = 0;
  CHECK(t.next(hz, ms) && std::fabs(hz - noteHz("C5")) < 0.01f && std::fabs(ms - 500) < 0.1f);
  CHECK(t.next(hz, ms) && std::fabs(ms - 375) < 0.1f);
  CHECK(t.next(hz, ms) && hz == 0 && std::fabs(ms - 1000) < 0.1f);
  CHECK(!t.next(hz, ms) && t.done());

  // A 1 kHz sine note of 100 ms: crosses zero twice a cycle, stays within -1..1, ends after its release.
  Voice v;
  Envelope env;
  v.start(1000, 100, Wave::Sine, env, 16000);
  int crossings = 0, n = 0;
  float prev = 0, peak = 0;
  while (v.active() && n < 16000) {
    const float s = v.next();
    if ((prev < 0) != (s < 0)) crossings++;
    peak = std::fmax(peak, std::fabs(s));
    prev = s, n++;
  }
  CHECK(crossings > 340 && crossings < 400);  // 2 per cycle, 190 ms with the release
  CHECK(peak <= 1.0f && peak > 0.9f);
  CHECK(n > 1600 && n < 1600 + 16 * 100);  // 100 ms + the 90 ms release
  // A rest is silent but takes its time.
  v.start(0, 50, Wave::Sine, env, 16000);
  n = 0;
  bool silent = true;
  while (v.active() && n < 16000) silent &= v.next() == 0, n++;
  CHECK(silent);
  CHECK(n == 800);

  // The player: step 2 = twice as fast, half as many output samples; interpolates between samples.
  int16_t ramp[101];
  for (int i = 0; i <= 100; i++) ramp[i] = int16_t(i * 100);
  Player p;
  p.set(ramp, 101, 0.5f);
  p.next();
  CHECK(std::fabs(p.next() * 32768 - 50) < 1);  // halfway between 0 and 100
  p.set(ramp, 101, 2.0f);
  n = 0;
  while (p.active()) p.next(), n++;
  CHECK(n == 50);
  CHECK(softClip(3) == 1 && softClip(-3) == -1 && std::fabs(softClip(0.5f) - 0.6875f) < 1e-4f);

  if (failures == 0) std::printf("sound synth: all checks pass\n");
  return failures ? 1 : 0;
}
