// Host test for labs/ble-lab-b5/src/canvas_protocol.h: the sketch bytes the app writes, parsed on the Mac.
//   c++ -std=c++17 -Wall -I labs/ble-lab-b5/src labs/ble-lab-b5/test/host/b5_canvas_test.cpp -o /tmp/b5_canvas_test && /tmp/b5_canvas_test
#include <cstdio>
#include <string>

#include "canvas_protocol.h"

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++;   \
  } while (0)

// Records every call as text, so the test reads like the protocol table.
struct Recorder {
  std::string log;
  void clear(uint16_t c) { log += "clear " + std::to_string(c) + ";"; }
  void stroke(uint16_t c, uint8_t w, const canvas::Point *p, int n) {
    log += "stroke " + std::to_string(c) + " w" + std::to_string(w) + " n" + std::to_string(n);
    for (int i = 0; i < n; i++) log += " " + std::to_string(p[i].x) + "," + std::to_string(p[i].y);
    log += ";";
  }
  void rect(uint16_t c, int16_t x, int16_t y, int16_t w, int16_t h) {
    log += "rect " + std::to_string(c) + " " + std::to_string(x) + "," + std::to_string(y) + " " + std::to_string(w) + "x" + std::to_string(h) + ";";
  }
  void text(uint16_t c, int16_t x, int16_t y, uint8_t s, const char *t, uint8_t n) {
    log += "text " + std::to_string(c) + " " + std::to_string(x) + "," + std::to_string(y) + " s" + std::to_string(s) + " " + std::string(t, n) + ";";
  }
  void dot(uint16_t c, int16_t x, int16_t y, uint8_t r) {
    log += "dot " + std::to_string(c) + " " + std::to_string(x) + "," + std::to_string(y) + " r" + std::to_string(r) + ";";
  }
};

int main() {
  // The header's example: a red dot at 160, 64, radius 6.
  {
    const uint8_t b[] = {0x05, 0x00, 0xF8, 0xA0, 0x00, 0x40, 0x06};
    Recorder r;
    const canvas::Result res = canvas::parse(b, sizeof(b), r);
    CHECK(res.commands == 1 && !res.truncated && !res.unknown);
    CHECK(r.log == "dot 63488 160,64 r6;");
  }
  // Several commands in one write: clear, a 2-point stroke at x 300 (needs the 2nd x byte), text.
  {
    const uint8_t b[] = {0x01, 0x00, 0x00,                                     //
                         0x02, 0xFF, 0xFF, 0x04, 0x02, 0x2C, 0x01, 0x10, 0x0A, 0x00, 0x20,  //
                         0x04, 0x1F, 0x00, 0x05, 0x00, 0x07, 0x02, 0x02, 'h', 'i'};
    Recorder r;
    const canvas::Result res = canvas::parse(b, sizeof(b), r);
    CHECK(res.commands == 3);
    CHECK(r.log == "clear 0;stroke 65535 w4 n2 300,16 10,32;text 31 5,7 s2 hi;");
  }
  // A rect cut off by the end of the write: draw nothing of it, report truncated.
  {
    const uint8_t b[] = {0x03, 0x00, 0x00, 0x01, 0x00};
    Recorder r;
    const canvas::Result res = canvas::parse(b, sizeof(b), r);
    CHECK(res.commands == 0 && res.truncated && r.log.empty());
  }
  // Unknown opcode: stop there, keep what came before.
  {
    const uint8_t b[] = {0x01, 0x00, 0x00, 0x7E, 0x01};
    Recorder r;
    const canvas::Result res = canvas::parse(b, sizeof(b), r);
    CHECK(res.commands == 1 && res.unknown);
  }
  // A stroke claiming 0 or too many points is refused.
  {
    const uint8_t b[] = {0x02, 0x00, 0x00, 0x01, 0x00};
    Recorder r;
    CHECK(canvas::parse(b, sizeof(b), r).truncated);
  }

  if (failures == 0) std::printf("b5 canvas protocol: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
