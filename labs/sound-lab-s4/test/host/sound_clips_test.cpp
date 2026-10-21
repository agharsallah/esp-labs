// Host test for labs/sound-lab-s4/src/clips.h: the clip store, WAV reading, base64 and CRC-32, on the Mac.
//   c++ -std=c++17 -Wall -I kit/lib/sound-kit/src -I labs/sound-lab-s4/src labs/sound-lab-s4/test/host/sound_clips_test.cpp -o /tmp/sound_clips_test && /tmp/sound_clips_test
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clips.h"

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++; \
  } while (0)

using namespace rec;

static int live = 0;  // allocations not yet freed
static void *countedAlloc(size_t n) { return live++, std::malloc(n); }
static void *countedResize(void *p, size_t n) { return std::realloc(p, n ? n : 1); }
static void countedFree(void *p) {
  if (p) live--;
  std::free(p);
}
static const Allocator kMem{countedAlloc, countedResize, countedFree};

static void testStore() {
  ClipStore s(kMem);
  s.budget = 100000;  // 50 000 samples
  int16_t buf[1000];
  for (int i = 0; i < 1000; i++) buf[i] = int16_t(i % 2 ? 300 : -500);

  ClipStore::Opened a = s.open(16000, 20000, false, 0, 0);
  CHECK(a.clip && a.clip->number == 1 && s.used() == 40000);
  CHECK(s.append(a.clip, buf, 1000) == 1000 && a.clip->peak == 500);
  s.close(a.clip, true);
  CHECK(!a.clip->open && a.clip->capacity == 1000 && s.used() == 2000);  // shrunk to what was recorded
  CHECK(a.clip->overview[0] == 255 && a.clip->overview[47] == 255);    // every slice holds a -500

  ClipStore::Opened m = s.open(16000, 1000, true, 0, 0);  // by hand: never dropped for you
  s.append(m.clip, buf, 1000);
  CHECK(s.append(m.clip, buf, 10) == 0 && s.full(m.clip));  // no room past the capacity
  s.close(m.clip, true);

  // A big new clip: the oldest voice clip (1) goes, the hand-made one (2) stays.
  ClipStore::Opened b = s.open(16000, 48500, false, 0, 0);
  CHECK(b.clip && b.dropped == 1 && b.clip->number == 3 && s.count() == 2);
  s.close(b.clip, false);  // dropped (too short, say): gone at once
  CHECK(s.count() == 1);

  // Nothing left to drop but the hand-made clip: refuse.
  ClipStore::Opened c = s.open(16000, 49900, false, 0, 0);
  CHECK(!c.clip && std::strcmp(c.why, "store full: del or clear some") == 0);
  CHECK(!s.open(16000, 60000, false, 0, 0).clip);  // bigger than the budget

  // A leased clip survives its deletion until released.
  Clip *leased = s.lease(2);
  CHECK(leased && !s.lease(99));
  CHECK(s.remove(2) && s.count() == 0);
  s.collect();
  CHECK(live == 1 && leased->samples == 1000);  // still readable
  ClipStore::release(leased);
  s.collect();
  CHECK(live == 0 && s.used() == 0);

  // Numbers keep counting: never reused.
  ClipStore::Opened d = s.open(16000, 10, false, 0, 0);
  CHECK(d.clip->number == 4);
  s.close(d.clip, false);
}

static void testWavRead() {
  int16_t data[] = {1000, -1000, 0x1234};
  Clip c;
  c.rate = 8000, c.samples = 3, c.data = data;
  CHECK(c.wavBytes() == 50);
  uint8_t whole[64], parts[64];
  CHECK(wavRead(c, 1, whole, sizeof(whole), 0) == 50);
  CHECK(std::memcmp(whole, "RIFF", 4) == 0 && whole[44] == 0xE8 && whole[45] == 0x03);  // 1000 little-endian
  CHECK(whole[48] == 0x34 && whole[49] == 0x12);
  size_t at = 0, n;
  while ((n = wavRead(c, 1, parts + at, 7, at)) > 0) at += n;  // in 7-byte pieces, like the network asks
  CHECK(at == 50 && std::memcmp(whole, parts, 50) == 0);
  CHECK(wavRead(c, 2, whole, sizeof(whole), 44) == 6 && whole[0] == 0xD0 && whole[1] == 0x07);  // x2 = 2000
}

static void testTransfer() {
  char out[16];
  CHECK(base64(reinterpret_cast<const uint8_t *>("Man"), 3, out) == 4 && std::strcmp(out, "TWFu") == 0);
  CHECK(base64(reinterpret_cast<const uint8_t *>("Ma"), 2, out) == 4 && std::strcmp(out, "TWE=") == 0);
  CHECK(base64(reinterpret_cast<const uint8_t *>("M"), 1, out) == 4 && std::strcmp(out, "TQ==") == 0);
  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(crc32(0, check, 9) == 0xCBF43926u);
  CHECK(crc32(crc32(0, check, 4), check + 4, 5) == 0xCBF43926u);  // in pieces = the same
}

int main() {
  testStore();
  testWavRead();
  testTransfer();
  if (failures == 0) std::printf("sound clips: all checks pass\n");
  return failures ? 1 : 0;
}
