// Host test for labs/sound-lab-s8/src/fx.h: delay line, ring modulator, bitcrush, reverse, howl detector.
//   c++ -std=c++17 -Wall -I labs/sound-lab-s8/src labs/sound-lab-s8/test/host/sound_fx_test.cpp -o /tmp/sound_fx_test && /tmp/sound_fx_test
#include <cmath>
#include <cstdio>

#include "fx.h"

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++; \
  } while (0)

int main() {
  // Delay: one click in, the echo comes out `delay` samples later, then again half as loud.
  static int16_t buf[1000];
  fx::DelayLine d;
  d.begin(buf, 1000);
  d.set(100, 0.5f);
  int16_t out[400];
  for (int i = 0; i < 400; i++) out[i] = d.process(i == 0 ? 10000 : 0);
  CHECK(out[0] == 0 && out[99] == 0);
  CHECK(out[100] == 10000);
  CHECK(out[200] == 5000);
  CHECK(out[300] == 2500);
  CHECK(out[150] == 0);
  d.set(5000, 2.0f);  // clamped: never longer than the buffer, never 100 % feedback
  CHECK(d.delay() == 1000 && d.feedback() < 1.0f);

  // Ring modulation of a constant = the carrier itself (a 50 Hz sine: one cycle in 320 samples).
  fx::RingMod rm;
  rm.set(50, 16000);
  int crossings = 0;
  int16_t prev = 0;
  for (int i = 0; i < 3200; i++) {
    const int16_t s = rm.process(10000);
    if ((prev < 0) != (s < 0)) crossings++;
    prev = s;
  }
  CHECK(crossings >= 19 && crossings <= 21);  // 10 cycles, 2 crossings each

  // Bitcrush: 4 bits keeps only the top 4, hold 3 repeats each value 3 times.
  fx::Bitcrush bc;
  bc.set(4, 3);
  CHECK(bc.process(0x1234) == 0x1000);
  CHECK(bc.process(0x7FFF) == 0x1000);  // held
  CHECK(bc.process(-1) == 0x1000);      // held
  CHECK(bc.process(int16_t(0x7FFF)) == 0x7000);

  int16_t r[4] = {1, 2, 3, 4};
  fx::reverse(r, 4);
  CHECK(r[0] == 4 && r[3] == 1);
  CHECK(fx::peakOf(r, 4) == 4);

  // Howl: a voice wobbling around -30 dB is fine; a level climbing 1 dB per 20 ms block is a howl.
  fx::HowlDetector h;
  bool howl = false;
  for (int i = 0; i < 100; i++) howl |= h.push(-30.0f + ((i % 6) < 3 ? 3.0f : -3.0f), 20);
  CHECK(!howl);
  h.reset();
  for (int i = 0; i < 20 && !howl; i++) howl = h.push(-38.0f + i, 20);
  CHECK(howl);
  // Very loud and staying there.
  h.reset();
  howl = false;
  for (int i = 0; i < 10; i++) howl |= h.push(-6, 20);
  CHECK(!howl);  // 200 ms: not yet
  for (int i = 0; i < 6; i++) howl |= h.push(-6, 20);
  CHECK(howl);
  // Quiet hiss never counts, even when it creeps up.
  h.reset();
  howl = false;
  for (int i = 0; i < 100; i++) howl |= h.push(-70.0f + i * 0.2f, 20);
  CHECK(!howl);

  if (failures == 0) std::printf("sound fx: all checks pass\n");
  return failures ? 1 : 0;
}
