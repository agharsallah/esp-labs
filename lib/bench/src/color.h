// Colour helpers for the 16-bit RGB565 format the ST7789 speaks: 5 bits red, 6 green, 5 blue.
#pragma once

#include <stdint.h>

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Hue 0-255 around the colour wheel, saturation and value 0-255.
inline uint16_t hsv(uint8_t h, uint8_t s, uint8_t v) {
  const uint8_t region = h / 43;
  const uint8_t rem = (h - region * 43) * 6;
  const uint8_t p = (v * (255 - s)) >> 8;
  const uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
  const uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
  switch (region) {
    case 0: return rgb(v, t, p);
    case 1: return rgb(q, v, p);
    case 2: return rgb(p, v, t);
    case 3: return rgb(p, q, v);
    case 4: return rgb(t, p, v);
    default: return rgb(v, p, q);
  }
}

// Mix two colours: k = 0 gives a, k = 255 gives b.
inline uint16_t mix(uint16_t a, uint16_t b, uint8_t k) {
  const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  const int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return uint16_t(((ar + ((br - ar) * k >> 8)) << 11) | ((ag + ((bg - ag) * k >> 8)) << 5) | (ab + ((bb - ab) * k >> 8)));
}

// The palette every scene shares, so the showcase reads as one piece.
namespace pal {
constexpr uint16_t ink = rgb(232, 238, 246);
constexpr uint16_t muted = rgb(118, 138, 162);
constexpr uint16_t dim = rgb(34, 46, 62);
constexpr uint16_t night = rgb(10, 14, 22);
constexpr uint16_t teal = rgb(70, 220, 190);
constexpr uint16_t amber = rgb(255, 176, 60);
constexpr uint16_t coral = rgb(255, 96, 84);
}  // namespace pal
