// Screens for the Bench service (B3, B4, B5):
//   gatt     the service as a table, one row per characteristic, with its properties and value.
//            Every operation lights its row and sends a packet across: in from the phone for a
//            read or write, out to the phone for a notify. Subscribed rows carry a bell.
//   message  the last message written from the phone, big, and the LED's colour
#pragma once

#include <Fonts/FreeSansBold12pt7b.h>

#include "bench_service.h"
#include "ble_views.h"

namespace bv {

inline uint16_t opColour(bench::Op op) {
  switch (op) {
    case bench::Op::Read: return pal::teal;
    case bench::Op::Write: return kBlue;
    case bench::Op::Notify: return kViolet;
    case bench::Op::Subscribe: return pal::amber;
    case bench::Op::Denied: return pal::coral;
    default: return pal::dim;
  }
}

inline const char *opName(bench::Op op) {
  static const char *const names[] = {"", "read", "write", "notify", "subscribe", "refused"};
  return names[static_cast<int>(op)];
}

inline void bell(FrameBuffer &fb, int x, int y, uint16_t c) {
  fb.fillRoundRect(x, y, 7, 6, 2, c);
  fb.drawFastHLine(x - 1, y + 6, 9, c);
  fb.fillCircle(x + 3, y + 8, 1, c);
}

inline void gatt(FrameBuffer &fb, const bench::State &s, float t) {
  const uint32_t now = millis();
  small(fb, 6, 34, pal::muted, "Bench service ba5e0000-...");
  // The phone, on the right edge.
  const int px = 292, py = 92;
  fb.drawRoundRect(px, py - 26, 22, 44, 4, pal::muted);
  fb.fillRect(px + 8, py + 13, 6, 2, pal::muted);
  struct Row {
    bench::Chr c;
    const char *props;
  };
  const Row rows[] = {{bench::Led, "R W"}, {bench::Message, "R W"}, {bench::ButtonChr, "R N"}, {bench::Temperature, "R N"}, {bench::Uptime, "R"}, {bench::Vault, "R W lock"}};
  const int count = s.hasVault ? 6 : 5;
  for (int i = 0; i < count; i++) {
    const int y = 46 + i * 18;
    const bench::Chr c = rows[i].c;
    const uint32_t age = now - s.opAt[c];
    const bool hot = s.opAt[c] && age < 900;
    const uint16_t oc = opColour(s.op[c]);
    fb.fillRoundRect(4, y - 2, 238, 16, 3, hot ? mix(oc, kPanel, uint8_t(age * 255 / 900)) : kPanel);
    small(fb, 10, y + 2, pal::ink, bench::chrName(c));
    small(fb, 84, y + 2, pal::muted, rows[i].props);
    if (s.subscribed[c]) bell(fb, 126, y + 1, pal::amber);
    char v[24] = "";
    switch (c) {
      case bench::Led:
        snprintf(v, sizeof(v), "%02X %02X %02X", s.led[0], s.led[1], s.led[2]);
        fb.fillRect(222, y + 1, 14, 10, rgb(s.led[0], s.led[1], s.led[2]));
        fb.drawRect(222, y + 1, 14, 10, pal::muted);
        break;
      case bench::Message: snprintf(v, sizeof(v), "\"%.14s\"", s.message); break;
      case bench::ButtonChr: snprintf(v, sizeof(v), "#%u", s.buttonCount); break;
      case bench::Temperature: snprintf(v, sizeof(v), "%.2f C", s.tempC); break;
      case bench::Uptime: snprintf(v, sizeof(v), "%lus", static_cast<unsigned long>(now / 1000)); break;
      case bench::Vault: snprintf(v, sizeof(v), "%s", "encrypted"); break;
      default: break;
    }
    small(fb, 140, y + 2, hot ? pal::ink : pal::muted, v);
    // The packet in flight: from the phone into the row (read request, write, subscribe) or out of
    // the row to the phone (notify). 600 ms for the trip, so the eye can follow it.
    if (hot && age < 600) {
      const float k = age / 600.0f;
      const bool out = s.op[c] == bench::Op::Notify;
      const int x0 = 244, x1 = px - 2;
      const int x = out ? x0 + int(k * (x1 - x0)) : x1 - int(k * (x1 - x0));
      const int yy = y + 6 + int((py - y - 6) * (out ? k : 1 - k));
      fb.fillCircle(x, yy, 3, oc);
      small(fb, 248, y + 2, oc, opName(s.op[c]));
    }
  }
  (void)t;
}

inline void message(FrameBuffer &fb, const bench::State &s, float t) {
  const uint16_t led = rgb(s.led[0], s.led[1], s.led[2]);
  // The LED as a glowing disc.
  for (int r = 26; r > 0; r -= 2) fb.fillCircle(270, 96, r, mix(pal::night, led, uint8_t(255 - r * 8)));
  small(fb, 246, 134, pal::muted, s.ledWritten ? "LED from phone" : "LED: write 0001");
  if (!s.message[0]) {
    bold(fb, 10, 70, pal::ink, "Write a message");
    small(fb, 10, 84, pal::muted, "to characteristic ba5e0002 in nRF Connect");
    small(fb, 10, 96, pal::muted, "or LightBlue (UTF-8 text, up to 40 bytes)");
    return;
  }
  // Wrap the message over up to three lines of 12 pt.
  fb.setFont(&FreeSansBold12pt7b);
  fb.setTextColor(pal::ink);
  const char *p = s.message;
  int y = 66;
  char line[41];
  while (*p && y < 140) {
    int take = 0, lastSpace = -1;
    while (p[take]) {
      strlcpy(line, p, take + 2);
      if (textWidth(fb, line) > 220) break;
      if (p[take] == ' ') lastSpace = take;
      take++;
    }
    if (p[take] && lastSpace > 0) take = lastSpace + 1;
    strlcpy(line, p, take + 1);
    fb.setCursor(10, y);
    fb.print(line);
    p += take;
    y += 28;
  }
  const uint32_t age = (millis() - s.messageAt) / 1000;
  char v[32];
  snprintf(v, sizeof(v), "written %lus ago", static_cast<unsigned long>(age));
  small(fb, 10, 146, pal::muted, v);
  (void)t;
}

}  // namespace bv
