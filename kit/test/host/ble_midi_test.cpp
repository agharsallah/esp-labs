// Host test for kit/lib/ble-kit/src/midi_ble.h: BLE-MIDI packets in and out, on the Mac.
//   c++ -std=c++17 -Wall -I kit/lib/ble-kit/src kit/test/host/ble_midi_test.cpp -o /tmp/ble_midi_test && /tmp/ble_midi_test
#include <cstdio>
#include <cstring>

#include "midi_ble.h"

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++;   \
  } while (0)

using namespace midi;

int main() {
  uint8_t pkt[32];
  size_t done = 0;

  // One note on at 4660 ms (0x1234): header 80|24, timestamp 80|34, then 90 3C 64.
  Msg one = noteOn(0, 60, 100, 0x1234);
  size_t n = encode(&one, 1, pkt, sizeof(pkt), false, &done);
  const uint8_t want1[] = {0xA4, 0xB4, 0x90, 0x3C, 0x64};
  CHECK(n == 5 && done == 1 && std::memcmp(pkt, want1, 5) == 0);

  // A chord with running status: the 2nd and 3rd notes drop their status byte.
  Msg chord[3] = {noteOn(0, 60, 100, 10), noteOn(0, 64, 100, 10), noteOn(0, 67, 100, 12)};
  n = encode(chord, 3, pkt, sizeof(pkt), true, &done);
  const uint8_t want2[] = {0x80, 0x8A, 0x90, 60, 100, 0x8A, 64, 100, 0x8C, 67, 100};
  CHECK(n == sizeof(want2) && done == 3 && std::memcmp(pkt, want2, n) == 0);

  // A gap over 127 ms stops the packet; the rest goes in the next one.
  Msg far[2] = {noteOn(0, 60, 100, 0), noteOff(0, 60, 200)};
  n = encode(far, 2, pkt, sizeof(pkt), false, &done);
  CHECK(done == 1 && n == 5);
  // Too small a buffer: nothing.
  CHECK(encode(&one, 1, pkt, 3, false, &done) == 0 && done == 0);

  // Round trip through the decoder.
  Decoder dec;
  Msg out[8];
  n = encode(chord, 3, pkt, sizeof(pkt), true, &done);
  int c = dec.decode(pkt, n, out, 8);
  CHECK(c == 3 && out[1].status == 0x90 && out[1].d1 == 64 && out[2].ts == 12);

  // Running status with no timestamp: [data] shares the time of the message before.
  const uint8_t rs[] = {0x80, 0x80, 0x90, 0x3C, 0x64, 0x40, 0x64};
  c = dec.decode(rs, sizeof(rs), out, 8);
  CHECK(c == 2 && out[1].status == 0x90 && out[1].d1 == 0x40 && out[1].ts == 0);

  // The low 7 bits wrap: 7E then 02 means 128 ms later, not 124 ms earlier.
  const uint8_t wrap[] = {0x81, 0xFE, 0x90, 60, 100, 0x82, 60, 0};
  c = dec.decode(wrap, sizeof(wrap), out, 8);
  CHECK(c == 2 && out[0].ts == 254 && out[1].ts == 258 && isNoteOff(out[1]));

  // SysEx is skipped, the note after it is kept.
  const uint8_t sx[] = {0x80, 0x80, 0xF0, 0x01, 0x02, 0x80, 0xF7, 0x80, 0x90, 0x40, 0x7F};
  c = dec.decode(sx, sizeof(sx), out, 8);
  CHECK(c == 1 && out[0].d1 == 0x40 && !dec.sysex);
  // A SysEx that runs into the next packet.
  const uint8_t sx1[] = {0x80, 0x80, 0xF0, 0x01, 0x02};
  const uint8_t sx2[] = {0x80, 0x03, 0x04, 0x81, 0xF7, 0x81, 0x80, 0x3C, 0x00};
  CHECK(dec.decode(sx1, sizeof(sx1), out, 8) == 0 && dec.sysex);
  c = dec.decode(sx2, sizeof(sx2), out, 8);
  CHECK(c == 1 && (out[0].status & 0xF0) == 0x80 && !dec.sysex);

  // Program change has one data byte; real-time (F8 clock) has none.
  const uint8_t pc[] = {0x80, 0x80, 0xC0, 0x05, 0x80, 0xF8};
  c = dec.decode(pc, sizeof(pc), out, 8);
  CHECK(c == 2 && out[0].status == 0xC0 && out[0].d1 == 5 && out[1].status == 0xF8);

  // No header (top bit clear, or bit 6 set): not a BLE-MIDI packet.
  const uint8_t bad[] = {0x3C, 0x64};
  const uint8_t bad2[] = {0xC0, 0x80, 0x90, 0x3C, 0x64};
  CHECK(dec.decode(bad2, sizeof(bad2), out, 8) == -1);
  CHECK(dec.decode(bad, sizeof(bad), out, 8) == -1);

  // Note names.
  char nm[8];
  noteName(60, nm, sizeof(nm));
  CHECK(std::strcmp(nm, "C4") == 0);
  noteName(70, nm, sizeof(nm));
  CHECK(std::strcmp(nm, "A#4") == 0);
  CHECK(parseNote("C4") == 60 && parseNote("a4") == 69 && parseNote("F#3") == 54 && parseNote("Bb2") == 46);
  CHECK(parseNote("H4") == -1 && parseNote("C") == -1 && parseNote("C44") == -1);

  if (failures == 0) std::printf("ble midi: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
