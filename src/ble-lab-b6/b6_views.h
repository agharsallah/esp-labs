// B6's screens (320 x 170). The first two are modes: there the button IS the remote.
//   media   transport and volume icons; the last key pressed ripples, and a dot runs the sound
//           path: board -(BLE HID)-> phone/Mac -(Classic audio)-> speaker
//   slides  a slide deck: right arrow, left arrow, b for a black screen (Keynote, PowerPoint, Slides)
//   typer   what `type` is sending, the layout, the snippet, the computer's Caps/Num Lock lights
//   keys    the last reports byte by byte: modifier bits, the key numbers, what they type
#pragma once

#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "ble_views.h"
#include "hid_keyboard.h"
#include "keymap.h"

namespace b6 {

struct Screen {
  keymap::Layout layout = keymap::Layout::US;
  uint16_t lastMedia = 0;
  uint32_t lastMediaAt = 0;
  bool playing = false;  // our guess: every play/pause flips it
  bool muted = false;
  int volumeSteps = 0;   // up minus down since power-on (the computer knows the real volume)
  int slide = 1;
  int slideDir = 0;
  uint32_t slideAt = 0;
  bool black = false;
  char typed[48] = "";   // the tail of what `type` queued
  int queued = 0, total = 0;
  char snippet[64] = "";
  bool touchOn = false;
  int touchRise[2] = {};
  bool ready = false;    // connected + encrypted
};

}  // namespace b6

namespace b6v {

using namespace bv;

// ---- small icons, centred on (x, y) ----

inline void play(FrameBuffer &fb, int x, int y, int s, uint16_t c) { fb.fillTriangle(x - s / 2, y - s, x - s / 2, y + s, x + s, y, c); }
inline void pause(FrameBuffer &fb, int x, int y, int s, uint16_t c) {
  fb.fillRect(x - s + 1, y - s, s * 2 / 3, s * 2, c);
  fb.fillRect(x + s / 3, y - s, s * 2 / 3, s * 2, c);
}
inline void skip(FrameBuffer &fb, int x, int y, int s, bool forward, uint16_t c) {
  const int d = forward ? 1 : -1;
  fb.fillTriangle(x - d * s, y - s, x - d * s, y + s, x, y, c);
  fb.fillTriangle(x, y - s, x, y + s, x + d * s, y, c);
  fb.fillRect(forward ? x + s : x - s - 2, y - s, 3, s * 2 + 1, c);
}
inline void speaker(FrameBuffer &fb, int x, int y, int waves, bool muted, uint16_t c) {
  fb.fillRect(x - 10, y - 4, 6, 8, c);
  fb.fillTriangle(x - 5, y - 4, x + 2, y - 10, x + 2, y + 10, c);
  fb.fillTriangle(x - 5, y + 4, x + 2, y - 10, x + 2, y + 10, c);
  if (muted) {
    fb.drawLine(x + 6, y - 5, x + 14, y + 5, pal::coral);
    fb.drawLine(x + 6, y + 5, x + 14, y - 5, pal::coral);
    return;
  }
  for (int i = 0; i < waves; i++) fb.drawCircleHelper(x + 2, y, 5 + i * 4, 0x2 | 0x4, c);
}

// The legend at the bottom of a mode view: what each gesture does here.
inline void legend(FrameBuffer &fb, const char *text) {
  fb.drawFastHLine(0, 144, kW, pal::dim);
  small(fb, 6, 148, pal::muted, text);
}

// board -> phone -> speaker: the first hop is ours (Bluetooth LE), the second is the phone's own
// Classic Bluetooth audio link. A dot runs it after every media key.
inline void soundPath(FrameBuffer &fb, int y, uint32_t sinceKey, bool ready) {
  const int xs[3] = {20, 150, 280};
  const char *const labels[3] = {"this board", "phone / Mac", "your speaker"};
  for (int i = 0; i < 3; i++) {
    fb.drawRoundRect(xs[i] - 16, y - 7, 32, 14, 3, i == 0 ? kBlue : pal::muted);
    small(fb, xs[i] - int(strlen(labels[i])) * 3, y + 10, pal::muted, labels[i]);
  }
  fb.drawFastHLine(xs[0] + 17, y, xs[1] - xs[0] - 34, ready ? kBlue : pal::dim);
  fb.drawFastHLine(xs[1] + 17, y, xs[2] - xs[1] - 34, pal::amber);
  small(fb, 58, y - 10, kBlue, "BLE HID");
  small(fb, 180, y - 10, pal::amber, "Classic audio");
  if (sinceKey < 900) {
    const float k = sinceKey / 900.0f;
    const int x = k < 0.5f ? xs[0] + 17 + int(k * 2 * (xs[1] - xs[0] - 34)) : xs[1] + 17 + int((k - 0.5f) * 2 * (xs[2] - xs[1] - 34));
    fb.fillCircle(x, y, 3, k < 0.5f ? kBlue : pal::amber);
  }
}

inline void media(FrameBuffer &fb, const b6::Screen &s, float t) {
  const uint32_t since = millis() - s.lastMediaAt;
  struct Btn {
    int x;
    uint16_t usage;
  };
  const Btn btns[] = {{36, b6::media::kPrevious}, {96, b6::media::kPlayPause}, {156, b6::media::kNext}, {222, b6::media::kVolumeDown}, {282, b6::media::kVolumeUp}};
  const int y = 66;
  for (const Btn &b : btns) {
    const bool hot = s.lastMedia == b.usage && since < 500;
    const uint16_t c = hot ? pal::ink : s.ready ? pal::muted : pal::dim;
    if (hot) fb.drawCircle(b.x, y, 20 + int(since / 25), mix(kBlue, pal::night, uint8_t(since * 255 / 500)));
    fb.drawCircle(b.x, y, b.usage == b6::media::kPlayPause ? 22 : 17, hot ? kBlue : pal::dim);
    switch (b.usage) {
      case b6::media::kPrevious: skip(fb, b.x, y, 7, false, c); break;
      case b6::media::kNext: skip(fb, b.x, y, 7, true, c); break;
      case b6::media::kPlayPause: s.playing ? pause(fb, b.x, y, 9, c) : play(fb, b.x, y, 10, c); break;
      case b6::media::kVolumeDown: speaker(fb, b.x + 2, y, 1, false, c), fb.drawFastHLine(b.x - 6, y + 13, 10, c); break;
      case b6::media::kVolumeUp: speaker(fb, b.x, y, 2, false, c), fb.drawFastHLine(b.x - 6, y + 13, 10, c), fb.drawFastVLine(b.x - 1, y + 9, 9, c); break;
    }
  }
  char v[48];
  if (s.lastMedia && since < 4000) snprintf(v, sizeof(v), "sent: %s%s", b6::mediaName(s.lastMedia), s.muted && s.lastMedia == b6::media::kMute ? " (toggle)" : "");
  else if (!s.ready) snprintf(v, sizeof(v), "pair me: Bluetooth settings > Bench B6");
  else snprintf(v, sizeof(v), "volume %+d steps since power-on%s", s.volumeSteps, s.touchOn ? ", touch 1/2" : "");
  small(fb, 8, 96, s.ready ? pal::ink : pal::amber, v);
  soundPath(fb, 120, since, s.ready);
  legend(fb, "click play/pause, 2x next, hold mute, hold 2 s: view");
  (void)t;
}

inline void slides(FrameBuffer &fb, const b6::Screen &s, float t) {
  // The deck: the current slide slides in from the side we moved to.
  const uint32_t since = millis() - s.slideAt;
  const float k = since < 300 ? 1 - since / 300.0f : 0;
  const int off = int(k * k * 120) * s.slideDir;
  const int x = 90 + off, y = 40;
  if (s.black) {
    fb.fillRoundRect(90, y, 140, 82, 4, 0);
    fb.drawRoundRect(90, y, 140, 82, 4, pal::dim);
    small(fb, 112, y + 38, pal::muted, "black screen (b)");
  } else {
    fb.drawRoundRect(x - 6, y + 4, 140, 82, 4, pal::dim);  // the next slide, peeking
    fb.fillRoundRect(x, y, 140, 82, 4, kPanel);
    fb.drawRoundRect(x, y, 140, 82, 4, pal::ink);
    fb.fillRect(x + 12, y + 12, 70, 6, pal::muted);
    fb.fillRect(x + 12, y + 26, 100, 3, pal::dim);
    fb.fillRect(x + 12, y + 34, 90, 3, pal::dim);
    char n[8];
    snprintf(n, sizeof(n), "%d", s.slide);
    fb.setFont(&FreeSansBold18pt7b);
    fb.setTextColor(pal::ink);
    fb.setCursor(x + 98, y + 72);
    fb.print(n);
  }
  // Arrows: which key went out.
  const bool hot = since < 500;
  fb.fillTriangle(60, 81, 72, 71, 72, 91, hot && s.slideDir < 0 ? kBlue : pal::dim);
  fb.fillTriangle(260, 81, 248, 71, 248, 91, hot && s.slideDir > 0 ? kBlue : pal::dim);
  small(fb, 8, 128, s.ready ? pal::muted : pal::amber, s.ready ? "keys: right arrow, left arrow, b" : "pair me first, then open your slides");
  legend(fb, "click next, 2x back, hold black, hold 2 s: view");
  (void)t;
}

inline void typer(FrameBuffer &fb, const b6::Screen &s, uint8_t hostLeds, float t) {
  char v[64];
  stat(fb, 8, 34, "layout (layout us|de)", keymap::layoutName(s.layout), kBlue);
  // The computer's lock lights, sent back to us in the output report.
  small(fb, 196, 34, pal::muted, "computer's lights");
  const char *const names[] = {"Num", "Caps", "Scroll"};
  for (int i = 0; i < 3; i++) {
    const bool on = (hostLeds >> i) & 1;
    fb.fillCircle(202 + i * 40, 52, 4, on ? pal::teal : pal::dim);
    small(fb, 209 + i * 40, 49, on ? pal::ink : pal::muted, names[i]);
  }
  // The text box, with a blinking cursor after what was typed.
  fb.drawRoundRect(6, 66, 308, 26, 4, pal::dim);
  small(fb, 12, 75, s.typed[0] ? pal::ink : pal::muted, s.typed[0] ? s.typed : "serial: type hello bench");
  if (int(t * 2) & 1) fb.fillRect(14 + int(strlen(s.typed)) * 6, 73, 2, 10, kBlue);
  if (s.total) {
    const int done = s.total - s.queued / 2;
    fb.fillRect(6, 96, 308 * done / s.total, 3, s.queued ? pal::amber : pal::teal);
    snprintf(v, sizeof(v), s.queued ? "typing: %d of %d keys" : "typed %d keys", done, s.total);
    small(fb, 6, 104, s.queued ? pal::amber : pal::muted, v);
  }
  snprintf(v, sizeof(v), "snippet: %.40s", s.snippet[0] ? s.snippet : "(none: snippet <text>)");
  small(fb, 6, 122, pal::muted, v);
}

inline void keys(FrameBuffer &fb, const b6::Keyboard &kb, keymap::Layout layout, float t) {
  const uint32_t n = kb.sentCount();
  if (n == 0) {
    small(fb, 8, 46, pal::muted, "No reports yet. Pair, then press the button");
    small(fb, 8, 58, pal::muted, "or type: key enter, type hi");
    return;
  }
  const b6::Sent s = kb.last(0);
  const uint32_t age = millis() - s.at;
  // The newest report as boxes; a packet runs to the computer on the right.
  small(fb, 6, 34, pal::muted, s.consumer ? "report 2 (media): usage, little-endian" : "report 1 (keyboard): mod, 0, key x6");
  const int bytes = s.consumer ? 2 : 8;
  for (int i = 0; i < bytes; i++) {
    const int x = 6 + i * 30;
    const bool live = s.report[i] != 0;
    fb.fillRoundRect(x, 46, 27, 18, 3, live ? (s.press ? kBlue : kPanel) : kPanel);
    char h[4];
    snprintf(h, sizeof(h), "%02X", s.report[i]);
    small(fb, x + 8, 52, live ? pal::ink : pal::muted, h);
  }
  fb.drawRoundRect(276, 40, 38, 28, 3, pal::muted);
  small(fb, 280, 50, pal::muted, "host");
  if (age < 400) fb.fillCircle(6 + bytes * 30 + int((276 - 6 - bytes * 30) * age / 400), 55, 3, s.press ? kBlue : kViolet);
  if (!s.consumer) {
    // Modifier byte, bit by bit: right GUI ... left Control.
    const char *const mods[] = {"LCtl", "LShf", "LAlt", "LGui", "RCtl", "RShf", "RAlt", "RGui"};
    for (int b = 0; b < 8; b++) {
      const bool on = (s.report[0] >> b) & 1;
      small(fb, 6 + b * 38, 70, on ? pal::amber : pal::dim, mods[b]);
    }
  }
  // The last few, newest first: press/release, bytes, what the host makes of it.
  for (uint32_t i = 0; i < 5 && i < n; i++) {
    const b6::Sent r = kb.last(i);
    char v[64];
    if (r.consumer)
      snprintf(v, sizeof(v), "%s media %02X %02X  %s", r.press ? "down" : "up  ", r.report[0], r.report[1],
               r.press ? b6::mediaName(uint16_t(r.report[0] | (r.report[1] << 8))) : "(released)");
    else if (r.press)
      snprintf(v, sizeof(v), "down mod %02X key %02X  %s%c%s", r.report[0], r.report[2], r.shown ? "types '" : "", r.shown ? r.shown : ' ',
               r.shown ? "'" : "");
    else
      snprintf(v, sizeof(v), "up   all zero: every key released");
    small(fb, 6, 84 + int(i) * 11, i == 0 ? pal::ink : pal::muted, v);
  }
  char v[48];
  snprintf(v, sizeof(v), "layout on this side: %s", keymap::layoutName(layout));
  small(fb, 6, 142, kBlue, v);
  (void)t;
}

}  // namespace b6v
