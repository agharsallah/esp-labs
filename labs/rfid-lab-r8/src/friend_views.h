// R8's two screens (320 x 170):
//   stage    idle: "tap a card" with the cast walking by. After a tap: the card's friend pops out
//            with a burst of stars in its colours, its name and how many times it came to play.
//            A new card says "new card!", a card with nobody on it says "who lives here?".
//   friends  every card with its friend, number and taps
#pragma once

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>

#include <vector>

#include "characters.h"
#include "color.h"
#include "frame_buffer.h"
#include "friends.h"
#include "tap_reader.h"

namespace fv {

constexpr int kW = 320, kH = 170;
constexpr uint32_t kShowMs = 6000;  // a show stays this long, then back to idle

inline void text(FrameBuffer &fb, int x, int y, uint16_t c, const char *s, const GFXfont *f = nullptr) {
  fb.setFont(f);
  fb.setTextColor(c);
  fb.setCursor(x, y);
  fb.print(s);
}

inline int width(FrameBuffer &fb, const char *s, const GFXfont *f) {
  fb.setFont(f);
  int16_t x1, y1;
  uint16_t w, h;
  fb.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

inline void centered(FrameBuffer &fb, int cx, int y, uint16_t c, const char *s, const GFXfont *f) {
  text(fb, cx - width(fb, s, f) / 2, y, c, s, f);
}

// A five-point star (two triangles and a pentagon's worth of fill is plenty at this size).
inline void star(FrameBuffer &fb, int x, int y, int r, uint16_t c) {
  fb.fillTriangle(x, y - r, x - r * 6 / 10, y + r * 8 / 10, x + r * 6 / 10, y + r * 8 / 10, c);
  fb.fillTriangle(x - r, y - r / 4, x + r, y - r / 4, x, y + r / 2, c);
  fb.fillTriangle(x, y + r / 2, x - r * 6 / 10, y + r * 8 / 10, x + r * 6 / 10, y + r * 8 / 10, c);
}

inline void header(FrameBuffer &fb, const char *view, int index, int count, bool present) {
  text(fb, 8, 18, pal::ink, "FRIENDS ", &FreeSansBold9pt7b);
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  fb.print(view);
  const int first = 286 - (count - 1) * 10;
  for (int i = 0; i < count; i++) fb.fillCircle(first + i * 10, 14, 3, i == index ? pal::ink : pal::dim);
  fb.fillCircle(304, 14, 5, present ? pal::teal : pal::dim);
}

inline void footer(FrameBuffer &fb, const char *left, const char *right, uint16_t rightColor) {
  fb.fillRect(0, kH - 11, kW, 11, pal::night);
  text(fb, 6, kH - 9, pal::muted, left);
  fb.setFont(nullptr);
  int16_t x1, y1;
  uint16_t w, h;
  fb.getTextBounds(right, 0, 0, &x1, &y1, &w, &h);
  text(fb, kW - 6 - w, kH - 9, rightColor, right);
}

// Easing for the pop: overshoots a little, then settles at 1.
inline float popIn(float k) {
  if (k >= 1) return 1;
  const float c = 1.7f;
  const float x = k - 1;
  return 1 + (c + 1) * x * x * x + c * x * x;
}

// Idle: a card outline that breathes, and the friends already living on cards walking by.
inline void idle(FrameBuffer &fb, const std::vector<friends::Card> &cards, float t) {
  const float breathe = 0.5f + 0.5f * sinf(t * 2.4f);
  const uint16_t glow = mix(pal::dim, pal::teal, uint8_t(breathe * 200));
  fb.drawRoundRect(118, 40, 84, 54, 8, glow);
  fb.drawRoundRect(119, 41, 82, 52, 7, glow);
  for (int i = 0; i < 3; i++) {
    const float k = fmodf(t * 0.7f + i / 3.0f, 1.0f);
    fb.drawCircle(160, 67, 10 + int(k * 26), mix(pal::teal, pal::night, uint8_t(k * 255)));
  }
  centered(fb, 160, 116, pal::ink, "Tap a card!", &FreeSansBold9pt7b);

  std::vector<const cast::Character *> walkers;
  for (const friends::Card &c : cards)
    if (const cast::Character *who = cast::find(c.character)) walkers.push_back(who);
  if (walkers.empty()) {
    centered(fb, 160, 140, pal::muted, "a friend will pop out of it", &FreeSans9pt7b);
    return;
  }
  const int n = int(walkers.size());
  for (int i = 0; i < n; i++) {
    const float x = fmodf(t * 26 + i * (360.0f / n), 360.0f) - 20;
    const float hop = fabsf(sinf(t * 5 + i));
    cast::draw(fb, *walkers[i], int(x), 152 - int(hop * 6), 12, t, 1.0f + (1 - hop) * 0.12f);
  }
}

// After a tap: the friend pops out with stars; a new or empty card asks for one.
inline void show(FrameBuffer &fb, const Stage &s, float t) {
  const float since = (millis() - s.showAt) / 1000.0f;
  const cast::Character *who = cast::find(s.character);
  const uint16_t c = who ? who->body : pal::teal;
  // A soft glow in the friend's colour behind it.
  for (int r = 70; r > 20; r -= 10) fb.fillCircle(84, 100, r, mix(pal::night, c, uint8_t(40 + (70 - r) * 1.2f)));

  // Stars fly out from the card, then fall.
  for (int i = 0; i < 14; i++) {
    const float a = i * 0.449f + 0.3f;
    const float k = since * (0.9f + (i % 4) * 0.15f);
    if (k > 1.6f) continue;
    const int x = 84 + int(cosf(a) * k * 110);
    const int y = 100 - int(sinf(a) * k * 90 - k * k * 40);
    star(fb, x, y, 3 + i % 3, i % 2 ? (who ? who->accent : pal::amber) : pal::ink);
  }

  if (s.full) {
    centered(fb, 220, 80, pal::amber, "12 cards!", &FreeSansBold9pt7b);
    centered(fb, 220, 104, pal::muted, "forget one in the app", &FreeSans9pt7b);
    return;
  }
  if (!who) {
    // Nobody lives here yet: an empty card with a question mark that wobbles.
    const int wob = int(sinf(t * 6) * 4);
    fb.fillRoundRect(52, 68 + wob, 64, 44, 6, pal::ink);
    centered(fb, 84, 102 + wob, pal::night, "?", &FreeSansBold18pt7b);
    char line[24];
    snprintf(line, sizeof(line), "card %u", s.number);
    centered(fb, 228, 66, pal::muted, line, &FreeSans9pt7b);
    centered(fb, 228, 94, pal::ink, s.isNew ? "New card!" : "Who lives", &FreeSansBold9pt7b);
    centered(fb, 228, 116, pal::ink, s.isNew ? "Give it a friend" : "here?", s.isNew ? &FreeSans9pt7b : &FreeSansBold9pt7b);
    centered(fb, 228, 140, pal::muted, "drag one in the app", &FreeSans9pt7b);
    return;
  }

  // Jump out of the card: scale pops in, then a happy bounce.
  const float pop = popIn(since / 0.6f);
  const float hop = since > 0.6f ? fabsf(sinf((since - 0.6f) * 6)) : 0;
  const float squash = since > 0.6f ? 1.0f + (1 - hop) * 0.15f : 1.0f;
  cast::draw(fb, *who, 84, 146 - int(hop * 14), max(4, int(34 * pop)), t, squash);

  const char *name = s.name[0] ? s.name : who->name;
  centered(fb, 228, 72, mix(c, pal::ink, 120), s.isNew ? "Hello!" : "Hi again!", &FreeSans9pt7b);
  centered(fb, 228, 104, pal::ink, name, &FreeSansBold18pt7b);
  char line[32];
  if (s.fromApp) snprintf(line, sizeof(line), "card %u", s.number);
  else snprintf(line, sizeof(line), "card %u  -  %lu visit%s", s.number, static_cast<unsigned long>(s.taps), s.taps == 1 ? "" : "s");
  centered(fb, 228, 130, pal::muted, line, &FreeSans9pt7b);
}

inline void stage(FrameBuffer &fb, const Stage &s, const std::vector<friends::Card> &cards, float t) {
  if (s.showSeq && millis() - s.showAt < kShowMs) show(fb, s, t);
  else idle(fb, cards, t);
}

// Every card with its friend: up to 12 in a 6 x 2 grid.
inline void list(FrameBuffer &fb, const std::vector<friends::Card> &cards, const Stage &s, float t) {
  if (cards.empty()) {
    centered(fb, 160, 90, pal::muted, "No cards yet: tap one!", &FreeSans9pt7b);
    return;
  }
  for (size_t i = 0; i < cards.size(); i++) {
    const friends::Card &c = cards[i];
    const int col = int(i % 6), row = int(i / 6);
    const int x = 8 + col * 51, y = 34 + row * 62;
    char uid[21];
    friends::uidToHex(c.uid, c.uidLen, uid, sizeof(uid));
    const bool onReader = s.present && strcmp(uid, s.presentUid) == 0;
    fb.drawRoundRect(x, y, 47, 58, 6, onReader ? pal::teal : pal::dim);
    if (const cast::Character *who = cast::find(c.character)) {
      cast::draw(fb, *who, x + 23, y + 40, 11, t + i);
      const char *name = c.name[0] ? c.name : who->name;
      char shortName[8];
      strlcpy(shortName, name, sizeof(shortName));
      fb.setFont(nullptr);
      text(fb, x + 23 - int(strlen(shortName)) * 3, y + 46, pal::ink, shortName);
    } else {
      text(fb, x + 20, y + 22, pal::muted, "?", &FreeSansBold9pt7b);
    }
    char n[8];
    snprintf(n, sizeof(n), "%u", c.number);
    text(fb, x + 4, y + 4, pal::muted, n);
  }
}

}  // namespace fv
