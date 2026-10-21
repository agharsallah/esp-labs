// The RFID lab's six screens (320 x 170):
//   chip     version, self-test, SPI clock, gain, how we wait, bus counters
//   card     the field, the card that's in it, read rate, ATQA / SAK / UID
//   trace    the sniffer: the last frames between reader and card
//   memory   the last dump as 16 sectors x 4 blocks
//   credits  the value block on block 8 and its backup
//   edge     the last edge-lab experiment as a bar chart
#pragma once

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>

#include "color.h"
#include "frame_buffer.h"
#include "snapshot.h"
#include "trace.h"

namespace rv {

constexpr int kW = 320, kH = 170, kTop = 30, kBottom = kH - 12;
constexpr uint16_t kField = rgb(90, 160, 255);
constexpr uint16_t kCard = rgb(236, 240, 246);

inline int textWidth(FrameBuffer &fb, const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  fb.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

inline void small(FrameBuffer &fb, int x, int y, uint16_t c, const char *s) {
  fb.setFont(nullptr);
  fb.setTextColor(c);
  fb.setCursor(x, y);
  fb.print(s);
}

inline void header(FrameBuffer &fb, const char *view, int index, int count, const Snapshot &s) {
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(8, 18);
  fb.print("RFID ");
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  fb.print(view);
  const int firstDot = 286 - (count - 1) * 10;
  for (int i = 0; i < count; i++) fb.fillCircle(firstDot + i * 10, 20, 3, i == index ? pal::ink : pal::dim);
  // Right: is a card in the field right now?
  fb.fillCircle(304, 20, 5, s.present ? pal::teal : pal::dim);
  if (s.present) fb.drawCircle(304, 20, 8, pal::teal);
  fb.drawFastHLine(0, 28, kW, pal::dim);
}

// Left: the button hint. Right: what the reader task is doing, or the last thing it said.
inline void footer(FrameBuffer &fb, const char *hint, const Snapshot &s) {
  fb.fillRect(0, kH - 11, kW, 11, pal::night);
  small(fb, 6, kH - 9, pal::muted, hint);
  char right[64];
  uint16_t color = s.noteBad ? pal::coral : pal::teal;
  if (s.busy) {
    snprintf(right, sizeof(right), "%s %u%%", s.busy, s.progress);
    color = pal::amber;
    fb.fillRect(0, kH - 1, kW * s.progress / 100, 1, pal::amber);
  } else {
    strlcpy(right, s.note, 34);  // keep it to the right half
  }
  fb.setFont(nullptr);
  small(fb, kW - 6 - textWidth(fb, right), kH - 9, color, right);
}

// A label over a value, in a column.
inline void stat(FrameBuffer &fb, int x, int y, const char *label, const char *value, uint16_t c = pal::ink) {
  small(fb, x, y, pal::muted, label);
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(c);
  fb.setCursor(x, y + 22);
  fb.print(value);
}

// ---- 1. chip ------------------------------------------------------------------------------------

inline void chip(FrameBuffer &fb, const Snapshot &s, float t) {
  const bool alive = s.chipVersion != 0x00 && s.chipVersion != 0xFF;
  // The chip as a square with 32 legs; its die glows when it answers.
  const int cx = 52, cy = 92;
  for (int i = 0; i < 8; i++) {
    const int o = -21 + i * 6;
    fb.fillRect(cx + o, cy - 34, 2, 6, pal::muted), fb.fillRect(cx + o, cy + 28, 2, 6, pal::muted);
    fb.fillRect(cx - 34, cy + o, 6, 2, pal::muted), fb.fillRect(cx + 28, cy + o, 6, 2, pal::muted);
  }
  fb.fillRoundRect(cx - 28, cy - 28, 56, 56, 4, rgb(24, 28, 34));
  const uint8_t glow = uint8_t(alive ? 140 + 100 * sinf(t * 3) : 30);
  fb.fillRect(cx - 9, cy - 9, 18, 18, mix(pal::dim, alive ? pal::teal : pal::coral, glow));
  small(fb, cx - 20, cy + 16, pal::muted, "MFRC522");

  char v[32];
  snprintf(v, sizeof(v), "%02Xh %s", s.chipVersion, alive ? "" : "no answer");
  stat(fb, 104, 38, "VersionReg", v, alive ? pal::teal : pal::coral);
  // -1 not run · 0 failed · 1 passed (NXP table) · 2 not supported (clone) · 3 consistent (clone, 2 runs agree)
  const char *const kSelfTest[] = {"not run", "FAILED", "passed", "not supported", "consistent"};
  const char *st = kSelfTest[s.selfTest + 1];
  stat(fb, 220, 38, "self-test", st, s.selfTest == 1 || s.selfTest == 3 ? pal::teal : s.selfTest == 0 ? pal::coral : pal::muted);
  snprintf(v, sizeof(v), "%lu MHz", static_cast<unsigned long>(s.spiHz / 1'000'000));
  stat(fb, 104, 80, "SPI clock (max 10)", v, s.spiHz > 10'000'000 ? pal::amber : pal::ink);
  snprintf(v, sizeof(v), "%s", s.irqMode ? "IRQ wire" : "polling");
  stat(fb, 220, 80, "waits by", v);
  // Gain as a bar of 18..48 dB.
  const uint8_t db[8] = {18, 23, 18, 23, 33, 38, 43, 48};
  const int g = db[s.gain & 7];
  small(fb, 104, 122, pal::muted, "receiver gain");
  fb.drawRect(104, 133, 100, 8, pal::dim);
  fb.fillRect(105, 134, (g - 18) * 98 / 30, 6, kField);
  snprintf(v, sizeof(v), "%d dB", g);
  small(fb, 210, 133, pal::ink, v);
  snprintf(v, sizeof(v), "%lu SPI ops", static_cast<unsigned long>(s.stats.spiOps));
  small(fb, 250, 122, pal::muted, "bus");
  small(fb, 250, 133, pal::ink, v);
  snprintf(v, sizeof(v), "%lu cmds", static_cast<unsigned long>(s.stats.commands));
  small(fb, 250, 144, pal::ink, v);
}

// ---- 2. card ------------------------------------------------------------------------------------

inline void card(FrameBuffer &fb, const Snapshot &s, float t) {
  // The antenna loop (left) and its field: rings that pulse outward. A present card sits in them.
  const int ax = 70, ay = 100;
  for (int i = 0; i < 4; i++) {
    const float k = fmodf(t * 0.6f + i * 0.25f, 1.0f);
    const int r = 14 + int(k * 52);
    fb.drawCircle(ax, ay, r, mix(kField, pal::night, uint8_t(k * 255)));
  }
  fb.drawRoundRect(ax - 22, ay - 16, 44, 32, 4, pal::amber);
  fb.drawRoundRect(ax - 18, ay - 12, 36, 24, 3, pal::amber);
  if (s.present) {
    const int bob = int(sinf(t * 2) * 2);
    fb.fillRoundRect(ax - 30, ay - 52 + bob, 60, 38, 4, kCard);
    fb.drawRoundRect(ax - 26, ay - 48 + bob, 34, 24, 3, pal::muted);  // the card's own coil
    fb.fillRect(ax + 14, ay - 42 + bob, 8, 8, pal::amber);             // its chip
  }

  if (!s.present) {
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(pal::ink);
    fb.setCursor(146, 76);
    fb.print("Hold a card or tag");
    fb.setCursor(146, 96);
    fb.print("flat on the reader");
    char v[40];
    snprintf(v, sizeof(v), "%s, %lu taps so far", s.polling ? "polling 12x/s" : "polling paused", static_cast<unsigned long>(s.taps));
    small(fb, 146, 112, pal::muted, v);
    return;
  }
  char id[32];
  s.uid.format(id, sizeof(id), ' ');
  small(fb, 146, 36, pal::muted, "UID");
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(pal::teal);
  fb.setCursor(146, 58);
  fb.print(id);
  small(fb, 146, 66, pal::ink, rfid::cardType(s.uid.sak));
  char v[24];
  snprintf(v, sizeof(v), "%02X %02X", s.uid.atqa[1], s.uid.atqa[0]);
  stat(fb, 146, 80, "ATQA", v);
  snprintf(v, sizeof(v), "%02X", s.uid.sak);
  stat(fb, 210, 80, "SAK", v);
  snprintf(v, sizeof(v), "%u B", s.uid.size);
  stat(fb, 262, 80, "UID size", v);
  // Read rate: % of the last 20 polls that found it. Low = edge of range.
  small(fb, 146, 122, pal::muted, "found in the last 20 polls");
  fb.drawRect(146, 133, 120, 8, pal::dim);
  fb.fillRect(147, 134, s.rate * 118 / 100, 6, s.rate >= 90 ? pal::teal : s.rate >= 50 ? pal::amber : pal::coral);
  snprintf(v, sizeof(v), "%u%%", s.rate);
  small(fb, 272, 133, pal::ink, v);
  if (s.collisions.n) {
    snprintf(v, sizeof(v), "collision at bit %u", s.collisions.at[0].bit);
    small(fb, 146, 146, pal::amber, v);
  }
}

// ---- 3. trace -----------------------------------------------------------------------------------

inline void trace(FrameBuffer &fb, Trace &tr, bool on) {
  if (!on) {
    small(fb, 8, 40, pal::muted, "Sniffer off. Serial: trace on");
    return;
  }
  const uint32_t head = tr.head();
  constexpr int kRows = 10;
  const uint32_t from = head > kRows ? head - kRows : 0;
  if (head == 0) small(fb, 8, 40, pal::muted, "Nothing yet: tap a card.");
  int row = 0;
  TraceFrame f, prev;
  uint32_t t0 = 0;
  for (uint32_t i = from; i < head; i++, row++) {
    if (!tr.get(i, f)) continue;
    if (!t0) t0 = f.us;
    const int y = 34 + row * 12;
    char line[56];
    snprintf(line, sizeof(line), "%6.1f", (f.us - t0) / 1000.0f);
    small(fb, 4, y, pal::muted, line);
    small(fb, 44, y, f.fromCard ? pal::teal : pal::amber, f.fromCard ? "<" : ">");
    size_t p = 0;
    line[0] = 0;
    for (int k = 0; k < f.len && k < 9; k++) p += snprintf(line + p, sizeof(line) - p, "%02X ", f.data[k]);
    if (f.len > 9) strlcat(line, "..", sizeof(line));
    const char *name = "";
    switch (f.kind) {
      case TraceFrame::Kind::Frame:
        name = f.fromCard ? iso14443a::nameCardFrame(prev.data, prev.len, prev.lastBits, f.len, f.lastBits)
                          : iso14443a::nameReaderFrame(f.data, f.len, f.lastBits);
        break;
      case TraceFrame::Kind::NoAnswer: name = "no answer"; break;
      case TraceFrame::Kind::Auth: name = "auth ok"; break;
      case TraceFrame::Kind::AuthFail: name = "auth refused"; break;
      case TraceFrame::Kind::FieldOff: name = "field off"; break;
      case TraceFrame::Kind::FieldOn: name = "field on"; break;
    }
    small(fb, 54, y, f.crypto ? pal::muted : pal::ink, line);
    small(fb, 228, y, f.fromCard ? pal::teal : pal::amber, name);
    if (!f.fromCard && f.kind == TraceFrame::Kind::Frame) prev = f;
  }
  small(fb, 4, kBottom - 2, pal::dim, "ms   > reader to card   < card to reader");
}

// ---- 4. memory ----------------------------------------------------------------------------------

inline uint16_t kindColor(BlockKind k) {
  switch (k) {
    case BlockKind::Maker: return pal::amber;
    case BlockKind::Empty: return pal::dim;
    case BlockKind::Data: return pal::teal;
    case BlockKind::Value: return kField;
    case BlockKind::Trailer: return pal::coral;
    case BlockKind::Closed: return rgb(70, 70, 80);
    default: return pal::night;
  }
}

inline void memory(FrameBuffer &fb, const Snapshot &s, float t) {
  const MemoryMap &m = s.map;
  if (!m.valid) {
    small(fb, 8, 44, pal::ink, "No dump yet.");
    small(fb, 8, 58, pal::muted, "Hold a card and double-click,");
    small(fb, 8, 70, pal::muted, "or type: dump");
    return;
  }
  // 16 sector columns x 4 block rows. Block 0 top-left, trailers along the bottom.
  const int x0 = 8, y0 = 44, cw = 18, ch = 18;
  for (int sec = 0; sec < 16; sec++) {
    char n[4];
    snprintf(n, sizeof(n), "%d", sec);
    small(fb, x0 + sec * cw + (sec < 10 ? 6 : 3), 34, pal::muted, n);
    for (int b = 0; b < 4; b++) {
      const int blk = sec * 4 + b;
      uint16_t c = sec < m.sectors || m.kind[blk] == BlockKind::Closed ? kindColor(m.kind[blk]) : pal::night;
      fb.fillRect(x0 + sec * cw, y0 + b * ch, cw - 2, ch - 2, c);
    }
  }
  const char *keys[] = {"maker", "empty", "data", "value", "trailer", "closed"};
  const BlockKind kinds[] = {BlockKind::Maker, BlockKind::Empty, BlockKind::Data, BlockKind::Value, BlockKind::Trailer, BlockKind::Closed};
  for (int i = 0; i < 6; i++) {
    const int x = 8 + i * 52;
    fb.fillRect(x, 122, 7, 7, kindColor(kinds[i]));
    small(fb, x + 10, 122, pal::muted, keys[i]);
  }
  char id[32], line[80];
  m.uid.format(id, sizeof(id));
  snprintf(line, sizeof(line), "%s  %d sectors x 4 blocks x 16 B", id, m.sectors);
  small(fb, 8, 138, pal::ink, line);
  (void)t;
}

// ---- 5. credits ---------------------------------------------------------------------------------

inline void credits(FrameBuffer &fb, const Snapshot &s, float t) {
  // A stack of coins whose height follows the balance (one coin per 10 credits, at most 8).
  const int coins = s.creditsValid ? constrain(int(s.credits / 10) + (s.credits > 0), 0, 8) : 0;
  for (int i = 0; i < coins; i++) {
    const int y = 140 - i * 9 + (i == coins - 1 ? int(sinf(t * 3) * 1.5f) : 0);
    fb.fillRoundRect(28, y, 56, 8, 4, pal::amber);
    fb.drawFastHLine(32, y + 2, 48, mix(pal::amber, pal::ink, 120));
  }
  if (!s.creditsValid) {
    small(fb, 110, 50, pal::ink, "No credits read yet.");
    small(fb, 110, 64, pal::muted, "credits init 50   (serial)");
    small(fb, 110, 76, pal::muted, "then: credits add 5 / use 3");
    return;
  }
  small(fb, 110, 40, pal::muted, "credits (block 8, a value block)");
  char v[24];
  snprintf(v, sizeof(v), "%ld", static_cast<long>(s.credits));
  fb.setFont(&FreeSansBold18pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(110, 82);
  fb.print(v);
  snprintf(v, sizeof(v), "backup %ld (block 9)", static_cast<long>(s.backup));
  small(fb, 110, 96, pal::muted, v);
  small(fb, 110, 116, pal::muted, "The card does the maths: you send");
  small(fb, 110, 127, pal::muted, "\"add 5\", it computes, TRANSFER commits.");
}

// ---- 6. edge ------------------------------------------------------------------------------------

inline void edgeChart(FrameBuffer &fb, const Snapshot &s) {
  const EdgeResult &e = s.edge;
  if (e.n == 0) {
    small(fb, 8, 40, pal::ink, "No experiment yet. Serial:");
    const char *lines[] = {"edge spi      SPI clock vs errors", "edge gain     gain vs found", "edge timeout  card reply time",
                           "edge timing   us per step", "edge irq      poll vs IRQ", "edge crc      chip vs CPU", "edge stress   200 read cycles"};
    for (int i = 0; i < 7; i++) small(fb, 14, 54 + i * 12, pal::muted, lines[i]);
    return;
  }
  small(fb, 8, 34, pal::ink, e.title);
  float top = 0;
  for (int i = 0; i < e.n; i++) top = max(top, e.value[i]);
  if (strcmp(e.unit, "%") == 0) top = 100;
  if (top <= 0) top = 1;
  const int x0 = 8, base = 136, hMax = 84, slot = (kW - 16) / e.n, bw = slot - 4;
  fb.drawFastHLine(x0, base, kW - 16, pal::dim);
  for (int i = 0; i < e.n; i++) {
    const int h = int(e.value[i] / top * hMax);
    const int x = x0 + i * slot + 2;
    // Colour by meaning: for success charts tall is good; for errors/time tall is bad.
    const float k = e.value[i] / top;
    const uint16_t c = e.goodHigh ? (k >= 0.9f ? pal::teal : k >= 0.5f ? pal::amber : pal::coral)
                                  : (k <= 0.05f ? pal::teal : k <= 0.5f ? pal::amber : pal::coral);
    fb.fillRect(x, base - h, bw, h, strcmp(e.unit, "us") == 0 || strcmp(e.unit, "ops") == 0 ? kField : c);
    if (i == e.mark) fb.drawRect(x - 2, base - hMax - 2, bw + 4, hMax + 4, pal::ink);
    char v[12];
    snprintf(v, sizeof(v), e.value[i] >= 100 || e.value[i] == int(e.value[i]) ? "%.0f" : "%.1f", e.value[i]);
    fb.setFont(nullptr);
    small(fb, x + (bw - textWidth(fb, v)) / 2, base - h - 9, pal::ink, v);
    small(fb, x + (bw - textWidth(fb, e.label[i])) / 2, base + 3, pal::muted, e.label[i]);
  }
  small(fb, 8, base + 13, pal::teal, e.caption);
}

}  // namespace rv
