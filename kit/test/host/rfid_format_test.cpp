// Host test for kit/lib/rfid-kit/src/rfid_format.h: the byte formats need no hardware, so they are
// checked on the Mac against frames every RFID sniffer shows.
//   c++ -std=c++17 -Wall -I kit/lib/rfid-kit/src kit/test/host/rfid_format_test.cpp -o /tmp/rfid_format_test && /tmp/rfid_format_test
#include <cstdio>
#include <initializer_list>

#include "rfid_format.h"

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++; \
  } while (0)

static bool crcIs(std::initializer_list<uint8_t> bytes, uint8_t lo, uint8_t hi) {
  const uint16_t c = rfid::crcA(bytes.begin(), bytes.size());
  return (c & 0xFF) == lo && (c >> 8) == hi;
}

int main() {
  // CRC_A of well-known frames: HLTA, READ block 0, AUTH key A block 0.
  CHECK(crcIs({0x50, 0x00}, 0x57, 0xCD));
  CHECK(crcIs({0x30, 0x00}, 0x02, 0xA8));
  CHECK(crcIs({0x60, 0x00}, 0xF5, 0x7B));

  const uint8_t uid[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  CHECK(rfid::bcc(uid) == (0xDE ^ 0xAD ^ 0xBE ^ 0xEF));

  // Factory trailer: FF FF FF FF FF FF | FF 07 80 | 69 | FF FF FF FF FF FF
  uint8_t trailer[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69};
  rfid::AccessBits ab;
  CHECK(rfid::AccessBits::decode(trailer, ab));
  for (int b = 0; b < 3; b++) CHECK(ab.condition(b) == 0 && ab.keyAWrites(b));
  CHECK(ab.condition(3) == 1);
  CHECK(ab.keyBReadable());
  uint8_t again[3];
  ab.encode(again);
  CHECK(again[0] == 0xFF && again[1] == 0x07 && again[2] == 0x80);
  trailer[7] = 0x08;  // inverted copy no longer matches
  CHECK(!rfid::AccessBits::decode(trailer, ab));

  // Value block: 100 at block 8.
  uint8_t v[16];
  rfid::ValueBlock::encode(100, 8, v);
  const uint8_t want[16] = {0x64, 0, 0, 0, 0x9B, 0xFF, 0xFF, 0xFF, 0x64, 0, 0, 0, 0x08, 0xF7, 0x08, 0xF7};
  CHECK(memcmp(v, want, 16) == 0);
  int32_t value = 0;
  uint8_t addr = 0;
  CHECK(rfid::ValueBlock::decode(v, value, addr) && value == 100 && addr == 8);
  rfid::ValueBlock::encode(-5, 9, v);
  CHECK(rfid::ValueBlock::decode(v, value, addr) && value == -5 && addr == 9);
  v[5] ^= 1;
  CHECK(!rfid::ValueBlock::decode(v, value, addr));

  // Geometry: 1K trailers, and a 4K big sector.
  CHECK(rfid::isTrailer(3) && rfid::isTrailer(63) && !rfid::isTrailer(4));
  CHECK(rfid::isTrailer(143) && !rfid::isTrailer(131) && rfid::sectorOf(143) == 32);
  CHECK(rfid::trailerOf(2) == 11 && rfid::firstBlock(33) == 144);

  const uint8_t single[2] = {0x04, 0x00}, dbl[2] = {0x44, 0x00};
  CHECK(rfid::atqaUidBytes(single) == 4 && rfid::atqaUidBytes(dbl) == 7);
  CHECK(rfid::classicSectors(0x08) == 16 && rfid::classicSectors(0x00) == 0);

  std::printf(failures ? "%d check(s) failed\n" : "all rfid format checks passed\n", failures);
  return failures ? 1 : 0;
}
