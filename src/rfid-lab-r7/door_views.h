// R7's three screens (320 x 170):
//   door   LOCKED / OPEN (with who and a countdown) / DENIED (with why), enrollment, lockdown
//   cards  the card list: name, role, and what a tap would do right now
//   log    the last access events
#pragma once

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>
#include <time.h>

#include <vector>

#include "access_rules.h"
#include "color.h"
#include "door.h"
#include "frame_buffer.h"

namespace dv {

constexpr int kW = 320, kH = 170;
constexpr uint16_t kOpen = rgb(60, 210, 120), kDenied = rgb(255, 96, 84), kEnroll = rgb(90, 160, 255);

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

inline void header(FrameBuffer &fb, const char *view, int index, int count, const DoorState &s, bool lockdown) {
  text(fb, 8, 18, pal::ink, "DOOR ", &FreeSansBold9pt7b);
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  fb.print(view);
  if (lockdown) {
    fb.fillRoundRect(150, 6, 76, 16, 4, pal::amber);
    text(fb, 158, 10, pal::night, "LOCKDOWN");
  }
  const int first = 286 - (count - 1) * 10;
  for (int i = 0; i < count; i++) fb.fillCircle(first + i * 10, 14, 3, i == index ? pal::ink : pal::dim);
  fb.fillCircle(304, 14, 5, s.present ? pal::teal : pal::dim);
  fb.drawFastHLine(0, 28, kW, pal::dim);
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

// A padlock at (cx, cy): body plus a shackle that lifts and swings open.
inline void padlock(FrameBuffer &fb, int cx, int cy, float open, uint16_t c) {
  const int lift = int(open * 10);
  const int shift = int(open * 14);
  for (int t = 0; t < 4; t++) {
    fb.drawCircleHelper(cx + shift, cy - 18 - lift, 13 - t, 1 | 2, c);  // top half of the shackle
  }
  fb.fillRect(cx + 9 + shift, cy - 18 - lift, 4, 8 + lift, c);  // right leg
  if (open < 0.2f) fb.fillRect(cx - 13, cy - 18, 4, 8, c);       // left leg goes into the body
  fb.fillRoundRect(cx - 20, cy - 10, 40, 32, 5, c);
  fb.fillCircle(cx, cy + 2, 4, pal::night);
  fb.fillRect(cx - 1, cy + 4, 3, 9, pal::night);
}

inline void door(FrameBuffer &fb, const DoorState &s, int cards, float t) {
  const uint32_t now = millis();
  if (s.enrolling) {
    const int left = int(max<int32_t>(0, int32_t(s.enrollUntil - now)) / 1000);
    for (int i = 0; i < 3; i++) {
      const float k = fmodf(t * 0.8f + i / 3.0f, 1.0f);
      fb.drawCircle(64, 96, 14 + int(k * 40), mix(kEnroll, pal::night, uint8_t(k * 255)));
    }
    fb.fillRoundRect(46, 82, 36, 26, 3, pal::ink);  // a card
    text(fb, 128, 64, kEnroll, "ADD A CARD", &FreeSansBold9pt7b);
    char line[48];
    snprintf(line, sizeof(line), "Tap the new card for \"%s\"", s.enrollName);
    text(fb, 128, 78, pal::ink, line);
    snprintf(line, sizeof(line), "%d s left", left);
    text(fb, 128, 94, pal::muted, line);
    return;
  }
  uint16_t c = pal::muted;
  const char *big = "LOCKED";
  float open = 0;
  if (s.mode == DoorState::Mode::Open) c = kOpen, big = "OPEN", open = 1;
  if (s.mode == DoorState::Mode::Denied) c = kDenied, big = "DENIED";
  if (s.mode == DoorState::Mode::Denied && int((now / 120) % 2)) c = mix(kDenied, pal::night, 90);  // blink
  padlock(fb, 62, 104, open, c);
  text(fb, 120, 80, c, big, &FreeSansBold18pt7b);
  char line[48];
  if (s.mode == DoorState::Mode::Open) {
    snprintf(line, sizeof(line), "welcome, %s", s.lastName[0] ? s.lastName : "?");
    text(fb, 122, 100, pal::ink, line, &FreeSans9pt7b);
    const int32_t left = max<int32_t>(0, int32_t(s.modeUntil - now));
    fb.drawRect(122, 112, 180, 6, pal::dim);
    fb.fillRect(123, 113, int(178 * left / float(Door::kOpenMs)), 4, kOpen);
  } else if (s.mode == DoorState::Mode::Denied) {
    text(fb, 122, 100, pal::ink, s.lastWhy, &FreeSans9pt7b);
    snprintf(line, sizeof(line), "%s", s.lastName[0] ? s.lastName : s.lastUid);
    text(fb, 122, 112, pal::muted, line);
  } else {
    snprintf(line, sizeof(line), "%d card%s in the list", cards, cards == 1 ? "" : "s");
    text(fb, 122, 100, pal::muted, line);
    if (s.lastWhy[0]) {
      snprintf(line, sizeof(line), "last: %s, %s", s.lastName[0] ? s.lastName : s.lastUid, s.lastWhy);
      text(fb, 122, 114, pal::muted, line);
    }
  }
}

inline uint16_t nowColor(door::Decision d) {
  return d == door::Decision::Granted ? kOpen : d == door::Decision::Blocked || d == door::Decision::Unknown ? kDenied : pal::amber;
}

inline void cards(FrameBuffer &fb, const std::vector<door::Card> &list, const door::Clock &clk, bool lockdown) {
  if (list.empty()) {
    text(fb, 8, 44, pal::ink, "No cards yet.");
    text(fb, 8, 58, pal::muted, "Add one from the app (Add card),");
    text(fb, 8, 70, pal::muted, "or serial: enroll <name>");
    return;
  }
  const int rows = min<int>(list.size(), 9);
  for (int i = 0; i < rows; i++) {
    const door::Card &c = list[i];
    const door::Decision d = door::decide(&c, clk, lockdown);
    const int y = 36 + i * 13;
    fb.fillCircle(12, y + 3, 3, nowColor(d));
    text(fb, 22, y, pal::ink, c.name);
    text(fb, 160, y, pal::muted, door::roleName(c.role));
    text(fb, 240, y, nowColor(d), door::decisionName(d));
  }
  if (int(list.size()) > rows) {
    char more[24];
    snprintf(more, sizeof(more), "+ %d more in the app", int(list.size()) - rows);
    text(fb, 22, 36 + rows * 13, pal::muted, more);
  }
}

inline void history(FrameBuffer &fb, const std::vector<DoorEvent> &events) {
  if (events.empty()) {
    text(fb, 8, 44, pal::muted, "No events yet: tap a card.");
    return;
  }
  const int n = events.size(), from = max(0, n - 9);
  for (int i = from; i < n; i++) {
    const DoorEvent &e = events[i];
    const int y = 36 + (i - from) * 13;
    char when[8] = "--:--";
    if (e.at) {
      const time_t at = e.at;
      struct tm lt;
      localtime_r(&at, &lt);
      strftime(when, sizeof(when), "%H:%M", &lt);
    }
    text(fb, 6, y, pal::muted, when);
    text(fb, 44, y, pal::ink, e.name[0] ? e.name : e.uid[0] ? e.uid : "-");
    const bool tap = strcmp(e.what, "tap") == 0;
    const uint16_t c = tap ? (strcmp(e.decision, "granted") == 0 ? kOpen : kDenied) : kEnroll;
    text(fb, 180, y, c, tap ? e.decision : e.what);
  }
}

}  // namespace dv
