// Host test for kit/lib/sound-kit's pure parts (level, onsets, rhythm, spectrum, wav, trigger), on the Mac.
//   c++ -std=c++17 -Wall -I kit/lib/sound-kit/src kit/test/host/sound_dsp_test.cpp -o /tmp/sound_dsp_test && /tmp/sound_dsp_test
#include <cmath>
#include <cstdio>
#include <cstring>

#include "level.h"
#include "onsets.h"
#include "rhythm.h"
#include "spectrum.h"
#include "trigger.h"
#include "wav.h"

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++; \
  } while (0)

using namespace snd;

// Feed `ms` of frames at a constant RMS into a Level, 4 ms per frame.
static uint32_t feed(Level &l, float rms, uint32_t ms, uint32_t t) {
  for (uint32_t end = t + ms; t < end; t += 4) l.push(rms, t);
  return t;
}

static void testLevel() {
  CHECK(std::fabs(toDb(32768) - 0) < 0.01f);
  CHECK(std::fabs(toDb(16384) + 6.02f) < 0.05f);  // half the height = -6 dB
  CHECK(toDb(0) == kFloorDb);

  Level l;
  uint32_t t = feed(l, 100, 1000, 0);  // a quiet room: the floor learns it
  CHECK(!l.learning());
  CHECK(std::fabs(l.reading().floorDb - toDb(100)) < 0.5f);
  CHECK(l.reading().above() < 1.0f);
  t = feed(l, 1000, 1000, t);  // 10x louder = +20 dB
  CHECK(std::fabs(l.reading().above() - 20.0f) < 1.5f);
  CHECK(std::fabs(l.reading().peakDb - toDb(1000)) < 0.1f);
  t = feed(l, 30, 1500, t);  // quieter than before: the floor follows it down
  CHECK(l.reading().floorDb < toDb(100) - 5);
  l.relearn();
  CHECK(l.learning());
  feed(l, 500, 600, t);
  CHECK(std::fabs(l.reading().floorDb - toDb(500)) < 1.5f);
}

static void testOnsets() {
  Onsets o;
  // A clap: quiet, one loud 4 ms frame at 30 dB, ringing down over ~40 ms.
  const float clap[] = {0, 0, 30, 25, 18, 12, 9, 6, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  int onsets = 0, claps = 0;
  uint32_t t = 0;
  for (float db : clap) {
    const Onsets::Event e = o.push(db, t += 4);
    onsets += e.kind == Onsets::Kind::Onset;
    if (e.kind == Onsets::Kind::Clap) {
      claps++;
      CHECK(e.lengthMs <= 40 && e.strengthDb == 30);
    }
  }
  CHECK(onsets == 1 && claps == 1);

  // A word: loud for 400 ms. One onset, then Long, not Clap.
  int longs = 0;
  claps = 0;
  for (int i = 0; i < 100; i++) {
    const Onsets::Event e = o.push(i < 100 && i >= 2 ? 22.0f : 0.0f, t += 4);
    claps += e.kind == Onsets::Kind::Clap;
  }
  for (int i = 0; i < 20; i++) {
    const Onsets::Event e = o.push(0, t += 4);
    longs += e.kind == Onsets::Kind::Long;
    claps += e.kind == Onsets::Kind::Clap;
  }
  CHECK(longs == 1 && claps == 0);

  // Groups: claps at 0, 300, 600 ms, then silence = one group of 3 with 300 ms gaps.
  Groups g;
  g.add(1000), g.add(1300), g.add(1600);
  CHECK(g.poll(1700).count == 0);  // the pause hasn't passed yet
  const Groups::Group done = g.poll(1600 + g.gapMs);
  CHECK(done.count == 3 && done.gapsMs[0] == 300 && done.gapsMs[1] == 300);
  CHECK(g.pending() == 0);
}

static void testRhythm() {
  Rhythm secret, slower, wrong, few;
  const uint32_t pattern[] = {0, 200, 400, 1000, 1200};  // knock knock knock ... knock knock
  for (uint32_t ms : pattern) secret.add(ms), slower.add(5000 + ms * 13 / 10), few.add(ms);
  few.count = 4;
  const uint32_t off[] = {0, 500, 600, 800, 1200};
  for (uint32_t ms : off) wrong.add(ms);
  CHECK(matchRhythm(secret, slower).ok);  // 30 % slower: same rhythm
  CHECK(matchRhythm(secret, slower).score > 90);
  CHECK(!matchRhythm(secret, wrong).ok);
  CHECK(!matchRhythm(secret, few).ok && std::strcmp(matchRhythm(secret, few).why, "too few knocks") == 0);

  BeatTracker b;
  uint32_t t = 0;
  int beats = 0;
  for (int i = 0; i < 2000; i++, t += 4) {  // 8 s of 120 BPM: a 20 ms kick every 500 ms over a hum
    const bool kick = (t % 500) < 20;
    beats += b.push(kick ? 3000.0f : 300.0f, kick ? 30.0f : 10.0f, t);
  }
  CHECK(beats >= 14 && beats <= 17);
  CHECK(b.bpm() >= 118 && b.bpm() <= 122);
}

static void testSpectrum() {
  static Fft f;
  CHECK(!f.setSize(500) && f.setSize(512));
  int16_t s[512];
  const float rate = 16000;
  for (int i = 0; i < 512; i++) s[i] = int16_t(16000 * std::sin(2 * M_PI * 1000.0 * i / rate));
  f.run(s);
  const Fft::Peak p = f.peak(rate, 200, 6000);
  CHECK(std::fabs(p.hz - 1000) < 8);                      // between bins 31 and 33, found by the parabola
  CHECK(std::fabs(p.db - toDb(16000)) < 2.0f);            // half scale = about -6 dB
  CHECK(p.clarity > 15);
  float bands[16];
  f.bands(rate, 100, 8000, bands, 16);
  int loudest = 0;
  for (int i = 1; i < 16; i++)
    if (bands[i] > bands[loudest]) loudest = i;
  CHECK(loudest == 8);  // 1 kHz sits in band 8 of 16 between 100 Hz and 8 kHz

  CHECK(std::strcmp(noteOf(440).name, "A4") == 0 && std::fabs(noteOf(440).cents) < 0.1f);
  CHECK(std::strcmp(noteOf(261.63f).name, "C4") == 0);
  CHECK(noteOf(446.4f).cents > 24 && noteOf(446.4f).cents < 26);  // a quarter semitone sharp of A4
}

static void testWav() {
  uint8_t h[kWavHeader];
  wavHeader(h, 16000, 100);
  CHECK(std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVEfmt ", 8) == 0 && std::memcmp(h + 36, "data", 4) == 0);
  CHECK(h[4] == 236 && h[5] == 0);                    // 36 + 200
  CHECK(h[24] == 0x80 && h[25] == 0x3E);              // 16000 = 0x3E80
  CHECK(h[40] == 200 && h[34] == 16 && h[22] == 1);   // data bytes, bits, mono
  CHECK(gainFor(0) == 1 && gainFor(29000) == 1 && gainFor(100) == 64 && std::fabs(gainFor(2900) - 10) < 0.01f);
  CHECK(applyGain(20000, 4) == 32767 && applyGain(-20000, 4) == -32768 && applyGain(100, 2.5f) == 250);
}

static void testTrigger() {
  Trigger tr(16000);
  CHECK(tr.push(40, 1000).action == Trigger::Action::None);  // off: ignores everything
  tr.arm(true);
  uint32_t n = 16000 * 10;  // ten seconds in
  const Trigger::Result start = tr.push(20, n);
  CHECK(start.action == Trigger::Action::Start && start.from == n - 8000);  // 500 ms pre-roll
  Trigger::Result r;
  for (int i = 1; i <= 600 && r.action == Trigger::Action::None; i++) r = tr.push(i < 100 ? 20.0f : 0.0f, n + i * 64);  // loud 0.4 s
  CHECK(r.action == Trigger::Action::Save);
  CHECK(r.to - r.from < 16000 * 2);
  CHECK(tr.state() == Trigger::State::Armed);

  // A bang: loud for one frame only = dropped.
  n += 16000 * 5;
  tr.push(30, n);
  r = {};
  for (int i = 1; i <= 400 && r.action == Trigger::Action::None; i++) r = tr.push(0, n + i * 64);
  CHECK(r.action == Trigger::Action::Drop);

  // By hand: nothing stops it but stop().
  tr.arm(false);
  CHECK(tr.start(n * 2).action == Trigger::Action::Start && tr.manual());
  CHECK(tr.push(0, n * 2 + 16000 * 5).action == Trigger::Action::None);
  CHECK(tr.stop(n * 2 + 16000 * 6).action == Trigger::Action::Save && tr.state() == Trigger::State::Off);
}

int main() {
  testLevel();
  testOnsets();
  testRhythm();
  testSpectrum();
  testWav();
  testTrigger();
  if (failures == 0) std::printf("sound dsp: all checks pass\n");
  return failures ? 1 : 0;
}
