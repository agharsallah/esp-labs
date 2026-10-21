// Drawing pieces every sound lab shares (320 x 170, landscape):
//   header   the lab id + the view's name, view dots, and the ear: a live level bar, the
//            comparator's DO lamp, and a red REC dot whenever anything is being recorded
//   footer   the button hint on the left, the latest note on the right (fading after 6 s)
//   wave     samples as a waveform in a box, scaled to fit
//   meter    a horizontal level bar with the floor, peak hold and a limit mark
#pragma once

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>

#include "color.h"
#include "frame_buffer.h"
#include "level.h"

namespace sv {

constexpr int kW = 320, kH = 170, kTop = 30, kBottom = kH - 12;
// The sound track's colour on the bench palette, plus one for "recording".
constexpr uint16_t kSound = rgb(255, 128, 200);
constexpr uint16_t kRec = rgb(255, 60, 60);
constexpr uint16_t kPanel = rgb(20, 28, 40);

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

inline void bold(FrameBuffer &fb, int x, int y, uint16_t c, const char *s) {
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(c);
  fb.setCursor(x, y);
  fb.print(s);
}

inline void big(FrameBuffer &fb, int x, int y, uint16_t c, const char *s) {
  fb.setFont(&FreeSansBold24pt7b);
  fb.setTextColor(c);
  fb.setCursor(x, y);
  fb.print(s);
}

// A label over a value, in a column.
inline void stat(FrameBuffer &fb, int x, int y, const char *label, const char *value, uint16_t c = pal::ink) {
  small(fb, x, y, pal::muted, label);
  bold(fb, x, y + 22, c, value);
}

// Green while calm, amber when loud, coral when very loud (dB above the quiet).
inline uint16_t levelColour(float above) { return above < 25 ? pal::teal : above < 40 ? pal::amber : pal::coral; }

// The ear in the header: 10 segments of level, then the DO lamp (KY-037) or a small "D" (the digital
// INMP441), sound waves while the speaker plays, and the REC dot.
inline void ear(FrameBuffer &fb, int x, int y, const snd::LevelReading &r, bool doHigh, bool rec, float t, bool digital, bool speaking) {
  const int lit = int(snd::meterFraction(r.above(), 50) * 10 + 0.5f);
  for (int i = 0; i < 10; i++) fb.fillRect(x + i * 4, y, 3, 10, i < lit ? levelColour(i * 5.0f) : pal::dim);
  if (digital) small(fb, x + 45, y + 2, pal::teal, "D");
  else fb.fillCircle(x + 48, y + 5, 3, doHigh ? pal::amber : pal::dim);
  if (speaking)
    for (int i = 0; i < 2; i++) fb.drawCircleHelper(x - 8 + i * 3, y + 5, 3 + i * 3, 0x2 | 0x4, kSound);
  if (rec && fmodf(t, 1.0f) < 0.6f) fb.fillCircle(x + 60, y + 5, 4, kRec);
}

inline void header(FrameBuffer &fb, const char *labId, const char *view, int index, int count, const snd::LevelReading &r, bool doHigh,
                   bool rec, float t, bool digital = false, bool speaking = false) {
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(kSound);
  fb.setCursor(8, 18);
  fb.print(labId);
  fb.print(" ");
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  fb.print(view);
  ear(fb, kW - 74, 10, r, doHigh, rec, t, digital, speaking);
  const int firstDot = kW - 92 - (count - 1) * 9;
  for (int i = 0; i < count; i++) fb.fillCircle(firstDot + i * 9, 15, 3, i == index ? pal::ink : pal::dim);
  fb.drawFastHLine(0, 28, kW, pal::dim);
}

enum class Note : uint8_t { Info, Good, Warn, Bad };

inline uint16_t noteColour(Note k) {
  switch (k) {
    case Note::Good: return pal::teal;
    case Note::Warn: return pal::amber;
    case Note::Bad: return pal::coral;
    default: return pal::muted;
  }
}

inline const char *noteMark(Note k) {
  switch (k) {
    case Note::Good: return "+";
    case Note::Warn: return "!";
    case Note::Bad: return "x";
    default: return "-";
  }
}

inline void footer(FrameBuffer &fb, const char *hint, const char *note, Note kind, uint32_t noteAgeMs) {
  fb.fillRect(0, kH - 11, kW, 11, pal::night);
  small(fb, 6, kH - 9, pal::muted, hint);
  if (!note || !note[0] || noteAgeMs > 6000) return;
  char right[30];
  strlcpy(right, note, sizeof(right));
  fb.setFont(nullptr);
  const uint16_t c = mix(noteColour(kind), pal::night, uint8_t(noteAgeMs > 4000 ? (noteAgeMs - 4000) * 200 / 2000 : 0));
  small(fb, kW - 6 - textWidth(fb, right), kH - 9, c, right);
}

// Samples as a waveform: one column per pixel, min..max of the samples that fall in it, so nothing
// is hidden between pixels. `scale` = how many int16 units half the box height shows.
inline void wave(FrameBuffer &fb, int x, int y, int w, int h, const int16_t *s, int n, float scale, uint16_t c) {
  const int mid = y + h / 2;
  fb.drawFastHLine(x, mid, w, pal::dim);
  if (n <= 0 || scale <= 0) return;
  const float k = (h / 2 - 1) / scale;
  for (int col = 0; col < w; col++) {
    const int a = col * n / w, b = std::max(a + 1, (col + 1) * n / w);
    int lo = 32767, hi = -32768;
    for (int i = a; i < b && i < n; i++) lo = std::min<int>(lo, s[i]), hi = std::max<int>(hi, s[i]);
    const int y0 = std::max(y, std::min(y + h - 1, mid - int(hi * k))), y1 = std::max(y, std::min(y + h - 1, mid - int(lo * k)));
    fb.drawFastVLine(x + col, y0, y1 - y0 + 1, c);
  }
}

// The biggest |sample| in a block, for auto-scaling a wave (never below `minScale`).
inline float autoScale(const int16_t *s, int n, float minScale = 256) {
  int m = 0;
  for (int i = 0; i < n; i++) m = std::max(m, std::abs(int(s[i])));
  return std::max(minScale, float(m) * 1.1f);
}

// A level bar: fill up to `above` dB (of rangeDb), the peak hold as a tick, an optional limit line.
inline void meter(FrameBuffer &fb, int x, int y, int w, int h, const snd::LevelReading &r, float rangeDb = 60, float limitDb = -1) {
  fb.fillRoundRect(x, y, w, h, 3, kPanel);
  const int fill = int(snd::meterFraction(r.above(), rangeDb) * (w - 2));
  // Coloured in zones, so the colour says how loud even without reading the number.
  for (int i = 0; i < fill; i += 3) fb.fillRect(x + 1 + i, y + 2, 2, h - 4, levelColour(float(i) / (w - 2) * rangeDb));
  const float peakAbove = r.peakDb - r.floorDb;
  const int px = x + 1 + int(snd::meterFraction(peakAbove, rangeDb) * (w - 2));
  fb.drawFastVLine(px, y, h, pal::ink);
  if (limitDb > 0) {
    const int lx = x + 1 + int(snd::meterFraction(limitDb, rangeDb) * (w - 2));
    for (int j = y - 3; j < y + h + 3; j += 2) fb.drawPixel(lx, j, pal::coral);
  }
}

}  // namespace sv
