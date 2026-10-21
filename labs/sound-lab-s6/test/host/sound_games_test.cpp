// Host test for S3's clap map and S6's Clap Bird and knock collector, on the Mac.
//   c++ -std=c++17 -Wall -I kit/lib/sound-kit/src -I labs/sound-lab-s3/src -I labs/sound-lab-s6/src labs/sound-lab-s6/test/host/sound_games_test.cpp -o /tmp/sound_games_test && /tmp/sound_games_test
#include <cstdio>
#include <cstring>

#include "bird.h"
#include "clap_actions.h"
#include "knock_lock.h"

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++; \
  } while (0)

static void testClapMap() {
  claps::Map m;
  CHECK(m.forClaps(1) == claps::Action::Led && m.forClaps(4) == claps::Action::Party);
  CHECK(m.forClaps(0) == claps::Action::None && m.forClaps(5) == claps::Action::None);
  claps::Action a;
  CHECK(claps::parse("color", a) && a == claps::Action::Colour);
  CHECK(claps::parse("screen", a) && a == claps::Action::Screen);
  CHECK(!claps::parse("disco", a));
  const float strengths[] = {30, 22, 40, 35, 28};
  CHECK(claps::suggestRise(strengths, 5) > 13.1f && claps::suggestRise(strengths, 5) < 13.3f);  // 22 x 0.6
  const float soft[] = {9, 10};
  CHECK(claps::suggestRise(soft, 2) == 8);  // never below 8 dB
}

static void testBird() {
  bird::Game g(42);
  CHECK(g.state() == bird::Game::State::Ready);
  g.step(1.0f);  // Ready: nothing moves until the first clap
  CHECK(g.y() == (bird::kTop + bird::kBottom) / 2);

  // A flap goes up, then gravity brings it back down.
  g.flap();
  const float y0 = g.y();
  g.step(1 / 60.0f);
  CHECK(g.y() < y0 && g.state() == bird::Game::State::Playing);
  for (int i = 0; i < 30; i++) g.step(1 / 60.0f);
  CHECK(g.vy() > 0);

  // No flaps: it falls to the ground and the round ends.
  for (int i = 0; i < 300 && g.state() == bird::Game::State::Playing; i++) g.step(1 / 60.0f);
  CHECK(g.state() == bird::Game::State::Over);
  g.flap();  // ignored once over
  CHECK(g.state() == bird::Game::State::Over);

  // A pipe whose gap is right at the bird: pass it and score; one with the gap far away: crash.
  g.reset();
  g.flap();
  const float mid = g.y();
  // Flapping whenever it sinks 10 px below mid, it swings between ~mid - 27 and mid + 12.
  g.setPipe(0, {bird::kBirdX + 10, mid - 9, false});
  g.setPipe(1, {1000, mid, false});
  g.setPipe(2, {1100, mid, false});
  for (int i = 0; i < 60; i++) {
    if (g.y() > mid + 10) g.flap();  // keep near the middle: a flap lifts it ~27 px
    g.step(1 / 60.0f);
  }
  CHECK(g.state() == bird::Game::State::Playing && g.score() == 1);
  g.reset();
  g.flap();
  g.setPipe(0, {bird::kBirdX + 2, bird::kTop + bird::kGap / 2 + 1, false});  // gap at the very top
  g.step(1 / 60.0f);
  CHECK(g.state() == bird::Game::State::Over);
}

static void testKnocks() {
  knock::Collector c;
  snd::Rhythm done;
  c.add(1000), c.add(1200), c.add(1400);
  CHECK(!c.poll(2000, done));  // 0.6 s quiet: not finished
  CHECK(c.poll(2900, done) && done.count == 3 && done.atMs[2] == 1400);
  CHECK(c.open().count == 0 && !c.poll(5000, done));
  for (int i = 0; i < 12; i++) CHECK(c.add(10000 + i * 100));
  CHECK(!c.add(20000));  // full
}

int main() {
  testClapMap();
  testBird();
  testKnocks();
  if (failures == 0) std::printf("sound games: all checks pass\n");
  return failures ? 1 : 0;
}
