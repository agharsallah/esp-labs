// Drawing pieces every BLE lab shares (320 x 170, landscape):
//   header   "BLE" + the view's name, view dots, and the status cluster: what the radio is doing,
//            a lock when the link is encrypted, a key when the peer is bonded, signal bars
//   footer   the button hint on the left, the latest activity line on the right
//   link     a live connection: who is central, the connection events ticking, MTU, PHY, security
//   log      the activity log, newest at the bottom
//   passkey  the big 6 digits, drawn over any view while pairing waits for you
#pragma once

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>

#include "ble_link.h"
#include "color.h"
#include "frame_buffer.h"

namespace bv {

constexpr int kW = 320, kH = 170, kTop = 30, kBottom = kH - 12;
// Bluetooth's own colours on the bench palette: blue = radio, violet = encrypted.
constexpr uint16_t kBlue = rgb(80, 150, 255);
constexpr uint16_t kViolet = rgb(176, 130, 255);
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

// A label over a value, in a column.
inline void stat(FrameBuffer &fb, int x, int y, const char *label, const char *value, uint16_t c = pal::ink) {
  small(fb, x, y, pal::muted, label);
  bold(fb, x, y + 22, c, value);
}

// Four bars, like a phone's: -50 dBm and up = 4 bars, -90 and below = 0.
inline void signalBars(FrameBuffer &fb, int x, int y, int rssi, uint16_t on = pal::ink) {
  const int bars = rssi == 0 ? 0 : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : rssi >= -90 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    const int h = 3 + i * 3;
    fb.fillRect(x + i * 4, y + 12 - h, 3, h, i < bars ? on : pal::dim);
  }
}

inline uint16_t rssiColour(int rssi) { return rssi >= -67 ? pal::teal : rssi >= -82 ? pal::amber : pal::coral; }

inline void lockIcon(FrameBuffer &fb, int x, int y, uint16_t c) {
  fb.drawRoundRect(x + 2, y, 7, 7, 3, c);
  fb.fillRect(x, y + 5, 11, 8, c);
}

inline void keyIcon(FrameBuffer &fb, int x, int y, uint16_t c) {
  fb.drawCircle(x + 3, y + 6, 3, c);
  fb.drawFastHLine(x + 6, y + 6, 7, c);
  fb.drawFastVLine(x + 11, y + 6, 4, c);
  fb.drawFastVLine(x + 9, y + 6, 3, c);
}

// The radio's state as a small animated glyph centred on (x, y).
inline void radioGlyph(FrameBuffer &fb, int x, int y, ble::Radio r, float t) {
  switch (r) {
    case ble::Radio::Off: fb.drawCircle(x, y, 5, pal::dim); break;
    case ble::Radio::Scanning: {
      // A radar sweep: we are listening.
      fb.drawCircle(x, y, 7, pal::dim);
      const float a = t * 5.0f;
      fb.drawLine(x, y, x + int(cosf(a) * 7), y + int(sinf(a) * 7), pal::teal);
      fb.fillCircle(x, y, 1, pal::teal);
      break;
    }
    case ble::Radio::Advertising: {
      // Rings leaving the antenna: we are shouting.
      fb.fillCircle(x, y, 2, kBlue);
      for (int i = 0; i < 2; i++) {
        const float k = fmodf(t * 1.2f + i * 0.5f, 1.0f);
        fb.drawCircle(x, y, 3 + int(k * 6), mix(kBlue, pal::night, uint8_t(k * 255)));
      }
      break;
    }
    case ble::Radio::Connecting:
      fb.drawCircle(x, y, 6, (int(t * 6) & 1) ? pal::amber : pal::dim);
      break;
    case ble::Radio::Connected:
      // Two linked rings: a connection.
      fb.drawCircle(x - 3, y, 4, kBlue);
      fb.drawCircle(x + 3, y, 4, kBlue);
      break;
  }
}

inline void header(FrameBuffer &fb, const char *labId, const char *view, int index, int count, const ble::LinkState &s, float t) {
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(kBlue);
  fb.setCursor(8, 18);
  fb.print(labId);
  fb.print(" ");
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  fb.print(view);
  // Status cluster, right to left: signal, key, lock, radio.
  int x = 302;
  if (s.radio == ble::Radio::Connected && s.peer.rssi != 0) signalBars(fb, x, 9, s.peer.rssi, rssiColour(s.peer.rssi));
  x -= 18;
  if (s.peer.bonded && s.radio == ble::Radio::Connected) keyIcon(fb, x, 8, pal::amber), x -= 18;
  if (s.peer.encrypted && s.radio == ble::Radio::Connected) lockIcon(fb, x + 2, 7, kViolet), x -= 16;
  radioGlyph(fb, x + 4, 15, s.radio, t);
  const int firstDot = x - 14 - (count - 1) * 9;
  for (int i = 0; i < count; i++) fb.fillCircle(firstDot + i * 9, 15, 3, i == index ? pal::ink : pal::dim);
  fb.drawFastHLine(0, 28, kW, pal::dim);
}

inline uint16_t actColour(ble::Act k) {
  switch (k) {
    case ble::Act::Good: return pal::teal;
    case ble::Act::Warn: return pal::amber;
    case ble::Act::Bad: return pal::coral;
    case ble::Act::Rx: return kBlue;
    case ble::Act::Tx: return kViolet;
    default: return pal::muted;
  }
}

inline const char *actMark(ble::Act k) {
  switch (k) {
    case ble::Act::Good: return "+";
    case ble::Act::Warn: return "!";
    case ble::Act::Bad: return "x";
    case ble::Act::Rx: return "<";
    case ble::Act::Tx: return ">";
    default: return "-";
  }
}

// Left: the button hint. Right: the newest activity line, fading after 6 s.
inline void footer(FrameBuffer &fb, const char *hint) {
  fb.fillRect(0, kH - 11, kW, 11, pal::night);
  small(fb, 6, kH - 9, pal::muted, hint);
  ble::Activity &a = ble::activity();
  ble::ActEntry e;
  const uint32_t head = a.head();
  if (head == 0 || !a.get(head - 1, e)) return;
  const uint32_t age = millis() - e.ms;
  if (age > 6000) return;
  char right[30];
  strlcpy(right, e.text, sizeof(right));
  fb.setFont(nullptr);
  const uint16_t c = mix(actColour(e.kind), pal::night, uint8_t(age > 4000 ? (age - 4000) * 200 / 2000 : 0));
  small(fb, kW - 6 - textWidth(fb, right), kH - 9, c, right);
}

// ---- log ----------------------------------------------------------------------------------------

inline void log(FrameBuffer &fb, int y0 = kTop + 4, int rows = 10) {
  ble::Activity &a = ble::activity();
  const uint32_t head = a.head();
  if (head == 0) {
    small(fb, 8, y0 + 4, pal::muted, "Nothing on the air yet.");
    return;
  }
  const uint32_t from = head > uint32_t(rows) ? head - rows : 0;
  ble::ActEntry e;
  int y = y0;
  for (uint32_t i = from; i < head; i++, y += 12) {
    if (!a.get(i, e)) continue;
    char when[12];
    snprintf(when, sizeof(when), "%5lu.%lu", static_cast<unsigned long>(e.ms / 1000), static_cast<unsigned long>(e.ms / 100 % 10));
    small(fb, 4, y, pal::dim, when);
    small(fb, 50, y, actColour(e.kind), actMark(e.kind));
    char text[44];
    strlcpy(text, e.text, sizeof(text));
    small(fb, 60, y, i + 1 == head ? pal::ink : pal::muted, text);
  }
}

// ---- link ---------------------------------------------------------------------------------------

// The connection drawn as two devices and the connection events between them: one exchange every
// interval, the dot's speed is real time slowed down 20x so 7.5-50 ms intervals stay visible.
inline void link(FrameBuffer &fb, const ble::LinkState &s, float t, const char *usLabel = "this board", const char *themLabel = nullptr) {
  const bool up = s.radio == ble::Radio::Connected;
  const int ly = 62, lx = 30, rx = 150;
  fb.fillRoundRect(lx - 22, ly - 22, 44, 44, 6, kPanel);
  fb.drawRoundRect(lx - 22, ly - 22, 44, 44, 6, kBlue);
  fb.fillRoundRect(rx - 16, ly - 26, 32, 52, 6, kPanel);
  fb.drawRoundRect(rx - 16, ly - 26, 32, 52, 6, up ? kBlue : pal::dim);
  small(fb, lx - 18, ly + 28, pal::muted, usLabel);
  small(fb, rx - 18, ly + 28, pal::muted, themLabel ? themLabel : (up && s.peer.name[0] ? s.peer.name : "peer"));
  // Who leads: the central sets the timing, the peripheral answers in each event.
  small(fb, lx - 18, ly - 4, pal::ink, s.peer.central || !up ? "periph" : "central");
  small(fb, rx - 14, ly - 4, up ? pal::ink : pal::dim, s.peer.central ? "central" : "periph");
  if (up) {
    const float intervalMs = s.peer.interval * 1.25f;
    const float period = fmaxf(intervalMs * 20.0f / 1000.0f, 0.15f);
    const float k = fmodf(t, period) / period;
    const int x = lx + 24 + int(k * (rx - lx - 42));
    fb.drawFastHLine(lx + 24, ly - 6, rx - lx - 42, pal::dim);
    fb.drawFastHLine(lx + 24, ly + 6, rx - lx - 42, pal::dim);
    fb.fillCircle(x, ly - 6, 3, s.peer.encrypted ? kViolet : kBlue);
    fb.fillCircle(lx + 24 + (rx - lx - 42) - (x - lx - 24), ly + 6, 3, pal::teal);
    if (s.peer.encrypted) lockIcon(fb, (lx + rx) / 2 - 5, ly - 26, kViolet);
  } else {
    small(fb, lx + 30, ly - 3, pal::muted, s.advertising ? "waiting..." : "no link");
  }

  char v[40];
  const int cx = 190;
  if (!up) {
    stat(fb, cx, 38, "last link ended", s.lastDisconnect < 0 ? "-" : "", pal::muted);
    if (s.lastDisconnect >= 0) {
      snprintf(v, sizeof(v), "reason 0x%02X", s.lastDisconnect & 0xFF);
      bold(fb, cx, 60, pal::ink, v);
      char why[34];
      strlcpy(why, ble::disconnectReason(s.lastDisconnect), sizeof(why));
      small(fb, cx, 68, pal::muted, why);
    }
    return;
  }
  snprintf(v, sizeof(v), "%.2f ms", s.peer.interval * 1.25f);
  stat(fb, cx, 34, "interval", v, kBlue);
  snprintf(v, sizeof(v), "%u B", s.peer.mtu);
  stat(fb, cx + 74, 34, "MTU", v);
  snprintf(v, sizeof(v), "%u", s.peer.latency);
  stat(fb, cx, 70, "latency", v);
  snprintf(v, sizeof(v), "%u ms", s.peer.timeout * 10);
  stat(fb, cx + 74, 70, "timeout", v);
  static const char *const phy[] = {"?", "1M", "2M", "Coded"};
  snprintf(v, sizeof(v), "%s/%s", phy[s.peer.txPhy & 3], phy[s.peer.rxPhy & 3]);
  stat(fb, cx, 106, "PHY tx/rx", v);
  snprintf(v, sizeof(v), "%d dBm", s.peer.rssi);
  stat(fb, cx + 74, 106, "RSSI", s.peer.rssi ? v : "-", s.peer.rssi ? rssiColour(s.peer.rssi) : pal::muted);
  // Security row under the drawing.
  const uint16_t sc = s.peer.encrypted ? kViolet : pal::muted;
  snprintf(v, sizeof(v), "%s%s%s", s.peer.encrypted ? "encrypted" : "plain text", s.peer.authenticated ? " + MITM" : "", s.peer.bonded ? " + bonded" : "");
  small(fb, 8, 114, sc, v);
  snprintf(v, sizeof(v), "%s  up %lus", s.peer.addr, static_cast<unsigned long>((millis() - s.connectedAt) / 1000));
  small(fb, 8, 126, pal::muted, v);
  snprintf(v, sizeof(v), "rx %lu ops %lu B", static_cast<unsigned long>(s.rxOps), static_cast<unsigned long>(s.rxBytes));
  small(fb, 8, 138, kBlue, v);
  snprintf(v, sizeof(v), "tx %lu ops %lu B", static_cast<unsigned long>(s.txOps), static_cast<unsigned long>(s.txBytes));
  small(fb, 8, 148, kViolet, v);
}

// ---- passkey ------------------------------------------------------------------------------------

// Drawn over the current view while pairing waits for a person.
inline void passkey(FrameBuffer &fb, const ble::LinkState &s, float t) {
  fb.fillRoundRect(16, 34, kW - 32, 118, 10, kPanel);
  fb.drawRoundRect(16, 34, kW - 32, 118, 10, (int(t * 2) & 1) ? pal::amber : kViolet);
  small(fb, 30, 44, pal::amber, s.passkeyCompare ? "NUMERIC COMPARISON" : "PASSKEY ENTRY");
  small(fb, 30, 56, pal::muted, s.passkeyCompare ? "Same number on the other screen?" : "Type this on the other device:");
  char digits[8];
  snprintf(digits, sizeof(digits), "%06lu", static_cast<unsigned long>(s.passkey));
  fb.setFont(&FreeSansBold24pt7b);
  fb.setTextColor(pal::ink);
  const int w = textWidth(fb, digits);
  fb.setCursor((kW - w) / 2, 112);
  fb.print(digits);
  const uint32_t left = 30 - min<uint32_t>(30, (millis() - s.passkeySince) / 1000);
  fb.fillRect(30, 124, (kW - 60) * left / 30, 3, pal::amber);
  small(fb, 30, 134, pal::muted, s.passkeyCompare ? "click = yes, it matches   long press = no" : "the other side has 30 s");
}

}  // namespace bv
