// R8's cast: the friends that can live on a card. The ids are shared with the app
// (esp-labs/apps/friends-app/src/friends/characters.ts, which checks this file in its tests): the board
// only stores the id, the app draws the 3D version, and this file draws the screen version.
//
// Each friend is a few circles and triangles in its own colours, so a 320 x 170 screen at 20+ fps
// can bounce, blink and squash them without any bitmap in flash.
#pragma once

#include <math.h>
#include <string.h>

#include "color.h"
#include "frame_buffer.h"

namespace cast {

enum class Kind : uint8_t { Fox, Robot, Dragon, Penguin, Alien, Cat, Frog, Unicorn };

struct Character {
  const char *id;    // what the board stores and the app sends
  const char *name;  // the friend's own name, shown when the card has no name of its own
  Kind kind;
  uint16_t body, accent, belly;
};

inline constexpr Character kCast[] = {
    {"fox", "Foxy", Kind::Fox, rgb(250, 130, 40), rgb(60, 36, 28), rgb(255, 240, 225)},
    {"robot", "Bolt", Kind::Robot, rgb(120, 200, 220), rgb(255, 210, 60), rgb(225, 240, 245)},
    {"dragon", "Ember", Kind::Dragon, rgb(90, 200, 110), rgb(255, 90, 70), rgb(240, 230, 150)},
    {"penguin", "Pip", Kind::Penguin, rgb(50, 70, 120), rgb(255, 160, 40), rgb(245, 248, 255)},
    {"alien", "Zib", Kind::Alien, rgb(170, 240, 80), rgb(150, 90, 230), rgb(220, 255, 190)},
    {"cat", "Mochi", Kind::Cat, rgb(255, 160, 200), rgb(120, 60, 90), rgb(255, 235, 245)},
    {"frog", "Hopper", Kind::Frog, rgb(60, 180, 90), rgb(255, 110, 140), rgb(200, 240, 150)},
    {"unicorn", "Luna", Kind::Unicorn, rgb(245, 235, 255), rgb(255, 200, 70), rgb(190, 150, 255)},
};
inline constexpr int kCount = sizeof(kCast) / sizeof(kCast[0]);

// nullptr for "" (no friend yet) or an id this firmware doesn't know.
inline const Character *find(const char *id) {
  if (!id || !id[0]) return nullptr;
  for (const Character &c : kCast)
    if (strcmp(c.id, id) == 0) return &c;
  return nullptr;
}

// One friend standing at (cx, ground): `s` is its size (head radius in pixels), `t` seconds for
// the idle bounce and blinking, `squash` > 1 makes it wide and short (landing), < 1 tall (jump).
inline void draw(FrameBuffer &fb, const Character &c, int cx, int ground, int s, float t, float squash = 1.0f) {
  const int rx = int(s * squash), ry = int(s / squash);
  const int cy = ground - ry;
  const bool blink = fmodf(t + cx * 0.013f, 3.2f) < 0.12f;
  fb.fillEllipse(cx, ground + 2, rx, max(2, s / 6), pal::dim);  // shadow

  // What sits behind the head: ears, horns, wings, antennae.
  switch (c.kind) {
    case Kind::Fox:
    case Kind::Cat:
      for (int side = -1; side <= 1; side += 2) {
        const int ex = cx + side * rx * 6 / 10;
        fb.fillTriangle(ex - s / 3, cy - ry * 6 / 10, ex + s / 3, cy - ry * 6 / 10, ex + side * s / 8, cy - ry - s / 2, c.body);
        fb.fillTriangle(ex - s / 7, cy - ry * 7 / 10, ex + s / 7, cy - ry * 7 / 10, ex + side * s / 12, cy - ry - s / 5,
                        c.kind == Kind::Fox ? c.accent : c.belly);
      }
      if (c.kind == Kind::Fox) fb.fillEllipse(cx + rx + s / 4, cy + ry / 3, s / 2, s / 4, c.body);  // tail
      break;
    case Kind::Robot: {
      fb.drawFastVLine(cx, cy - ry - s / 2, s / 2, c.accent);
      fb.fillCircle(cx, cy - ry - s / 2, max(2, s / 7), (int(t * 3) % 2) ? c.accent : pal::coral);
      break;
    }
    case Kind::Dragon:
      for (int side = -1; side <= 1; side += 2) {
        const float flap = sinf(t * 6) * s * 0.25f;
        fb.fillTriangle(cx + side * rx * 7 / 10, cy, cx + side * (rx + s * 9 / 10), int(cy - s * 0.7f - flap), cx + side * (rx + s / 3),
                        cy + s / 3, c.accent);
        fb.fillTriangle(cx + side * s / 3, cy - ry * 8 / 10, cx + side * s / 6, cy - ry * 8 / 10, cx + side * s / 2, cy - ry - s / 3, c.belly);
      }
      break;
    case Kind::Alien:
      for (int side = -1; side <= 1; side += 2) {
        const int tx = cx + side * s / 2 + int(sinf(t * 4 + side) * s / 8);
        fb.drawLine(cx + side * s / 4, cy - ry * 8 / 10, tx, cy - ry - s / 2, c.body);
        fb.fillCircle(tx, cy - ry - s / 2, max(2, s / 7), c.accent);
      }
      break;
    case Kind::Unicorn:
      for (int i = 0; i < 4; i++) fb.fillCircle(cx - rx * 7 / 10 - i * s / 8, cy - ry / 2 + i * s / 4, s / 4, hsv(uint8_t(170 + i * 25), 140, 255));
      fb.fillTriangle(cx - s / 7, cy - ry * 8 / 10, cx + s / 7, cy - ry * 8 / 10, cx, cy - ry - s * 3 / 4, c.accent);
      break;
    case Kind::Frog:
      for (int side = -1; side <= 1; side += 2) fb.fillCircle(cx + side * s / 2, cy - ry * 8 / 10, s / 3, c.body);
      break;
    case Kind::Penguin: break;
  }

  // Head and body in one round shape, plus a lighter face or belly.
  if (c.kind == Kind::Robot) {
    fb.fillRoundRect(cx - rx, cy - ry, rx * 2, ry * 2, s / 3, c.body);
    fb.fillRoundRect(cx - rx * 7 / 10, cy - ry / 2, rx * 14 / 10, ry * 11 / 10, s / 5, c.belly);
  } else {
    fb.fillEllipse(cx, cy, rx, ry, c.body);
    fb.fillEllipse(cx, cy + ry / 4, rx * 7 / 10, ry * 6 / 10, c.belly);
  }

  // Eyes: the alien has one big one, the frog's sit on top.
  const int eyeY = c.kind == Kind::Frog ? cy - ry * 8 / 10 : cy - ry / 6;
  const int eyeR = max(2, s / 6);
  if (c.kind == Kind::Alien) {
    fb.fillCircle(cx, eyeY, eyeR * 2, pal::ink);
    if (!blink) fb.fillCircle(cx + s / 12, eyeY, eyeR, pal::night);
  } else {
    for (int side = -1; side <= 1; side += 2) {
      const int ex = cx + side * s * (c.kind == Kind::Frog ? 5 : 3) / 10;
      if (blink) {
        fb.fillRect(ex - eyeR, eyeY, eyeR * 2, max(1, s / 16), pal::night);
      } else {
        if (c.kind == Kind::Robot) fb.fillRect(ex - eyeR, eyeY - eyeR, eyeR * 2, eyeR * 2, pal::night);
        else fb.fillCircle(ex, eyeY, eyeR, pal::night);
        fb.fillCircle(ex + eyeR / 3, eyeY - eyeR / 3, max(1, eyeR / 3), pal::ink);
      }
    }
  }

  // Mouth or beak, and cheeks.
  const int my = cy + ry / 4;
  switch (c.kind) {
    case Kind::Penguin: fb.fillTriangle(cx - s / 6, my - s / 12, cx + s / 6, my - s / 12, cx, my + s / 6, c.accent); break;
    case Kind::Fox: fb.fillCircle(cx, my - s / 12, max(2, s / 10), c.accent); break;
    case Kind::Robot: fb.fillRect(cx - s / 4, my, s / 2, max(2, s / 10), c.accent); break;
    default: fb.drawCircleHelper(cx, my - s / 8, s / 4, 4 | 8, pal::night); break;
  }
  if (c.kind != Kind::Robot && c.kind != Kind::Alien) {
    const uint16_t cheek = mix(c.body, pal::coral, 150);
    fb.fillCircle(cx - rx * 6 / 10, my, max(2, s / 9), cheek);
    fb.fillCircle(cx + rx * 6 / 10, my, max(2, s / 9), cheek);
  }
}

}  // namespace cast
