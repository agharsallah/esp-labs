// The sketch protocol: how the app draws on the board's screen over one characteristic.
// No Arduino and no radio here, so labs/ble-lab-b5/test/host/b5_canvas_test.cpp checks it on the Mac.
//
// The app writes to the Canvas characteristic (ba5e0010) "without response": the fastest kind of
// write, because the board never answers each one. One write can carry several commands back to
// back, up to MTU - 3 bytes. The drawing area is the screen under the status bar: 320 x 128 pixels,
// x 0-319, y 0-127. Colours are RGB565 (5 bits red, 6 green, 5 blue), the screen's own format.
// Every number is little-endian.
//
//   01 cc cc                              clear          the whole area with colour cc cc
//   02 cc cc w n [xx xx yy] x n           stroke         n points (1-80), joined by lines w px wide
//   03 cc cc xx xx yy ww ww hh            rect           a filled rectangle
//   04 cc cc xx xx yy s len [text]        text           len ASCII bytes, size s (1-4) at x, y (top left)
//   05 cc cc xx xx yy r                   dot            a filled circle, radius r
//
// Example: a red dot (F800) at x 160, y 64, radius 6. The bytes are 05, the colour little-endian
// (00 F8), x 00A0 little-endian (A0 00), y 40, r 06:
//   05 00 F8 A0 00 40 06
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace canvas {

constexpr int kW = 320, kH = 128;
constexpr int kMaxPoints = 80;

enum Op : uint8_t { Clear = 0x01, Stroke = 0x02, Rect = 0x03, Text = 0x04, Dot = 0x05 };

struct Point {
  int16_t x;
  int16_t y;
};

// What a parse found: commands drawn, and whether the write ended in the middle of a command.
struct Result {
  int commands = 0;
  bool truncated = false;  // a command ran past the end of the write: the rest is ignored
  bool unknown = false;    // an opcode we don't know: the rest is ignored
};

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }

// Walks one write and calls the drawer for each command. Drawer needs:
//   clear(c) · stroke(c, width, points, n) · rect(c, x, y, w, h) · text(c, x, y, size, chars, len) · dot(c, x, y, r)
template <typename Drawer>
Result parse(const uint8_t *p, size_t n, Drawer &d) {
  Result r;
  size_t i = 0;
  while (i < n) {
    const uint8_t op = p[i];
    const size_t left = n - i;
    switch (op) {
      case Clear:
        if (left < 3) return r.truncated = true, r;
        d.clear(le16(p + i + 1));
        i += 3;
        break;
      case Stroke: {
        if (left < 5) return r.truncated = true, r;
        const uint8_t count = p[i + 4];
        if (count == 0 || count > kMaxPoints || left < 5 + size_t(count) * 3) return r.truncated = true, r;
        Point pts[kMaxPoints];
        for (int k = 0; k < count; k++) {
          const uint8_t *q = p + i + 5 + k * 3;
          pts[k] = Point{int16_t(le16(q)), int16_t(q[2])};
        }
        d.stroke(le16(p + i + 1), p[i + 3], pts, count);
        i += 5 + count * 3;
        break;
      }
      case Rect:
        if (left < 10) return r.truncated = true, r;
        d.rect(le16(p + i + 1), int16_t(le16(p + i + 3)), int16_t(p[i + 5]), int16_t(le16(p + i + 6)), int16_t(p[i + 8]));
        i += 10;
        break;
      case Text: {
        if (left < 8) return r.truncated = true, r;
        const uint8_t len = p[i + 7];
        if (left < 8 + size_t(len)) return r.truncated = true, r;
        d.text(le16(p + i + 1), int16_t(le16(p + i + 3)), int16_t(p[i + 5]), p[i + 6], reinterpret_cast<const char *>(p + i + 8), len);
        i += 8 + len;
        break;
      }
      case Dot:
        if (left < 7) return r.truncated = true, r;
        d.dot(le16(p + i + 1), int16_t(le16(p + i + 3)), int16_t(p[i + 5]), p[i + 6]);
        i += 7;
        break;
      default: return r.unknown = true, r;
    }
    r.commands++;
  }
  return r;
}

}  // namespace canvas
