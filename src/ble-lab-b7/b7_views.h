// B7's screens (320 x 170):
//   keys       the five touch keys (or the button's next notes), a two-octave piano strip with
//              every note sounding right now: ours in violet, the synth side's in blue
//   sequencer  the drum machine: 4 drums x 16 steps, the playhead walking at the tempo
//   roll       a piano roll of the last 6 seconds, both directions
//   link       why BLE-MIDI is "almost live": notes wait for the next connection event
//   log        the activity log
#pragma once

#include "ble_views.h"
#include "midi_ble.h"
#include "midi_service.h"
#include "music.h"

namespace b7 {

struct RollNote {
  uint8_t note = 0;
  bool incoming = false;
  uint32_t on = 0, off = 0;  // off 0 = still sounding
};

struct Screen {
  Scale scale = Scale::Pentatonic;
  int octave = 4;
  int channel = 0;  // 0-15 (shown as 1-16)
  int bpm = 100;
  bool seqOn = false;
  const Beat *beat = nullptr;
  int step = 0;
  bool touchOn = false;
  bool padDown[5] = {};
  int padRise[5] = {};
  uint32_t outBits[4] = {}, inBits[4] = {};  // 128 notes each: sounding now
  const char *tune = nullptr;
  int tunePos = 0, tuneLen = 0;
  RollNote roll[48];
  int rollHead = 0;
  uint32_t presses[8] = {};  // millis() of our last note-ons, for the link view
  int pressHead = 0;
  Stats stats;

  bool out(uint8_t n) const { return (outBits[n >> 5] >> (n & 31)) & 1; }
  bool in(uint8_t n) const { return (inBits[n >> 5] >> (n & 31)) & 1; }
  void setNote(uint8_t n, bool incoming, bool on) {
    uint32_t *bits = incoming ? inBits : outBits;
    if (on) bits[n >> 5] |= 1u << (n & 31);
    else bits[n >> 5] &= ~(1u << (n & 31));
    if (on) {
      roll[rollHead++ % 48] = {n, incoming, millis(), 0};
      if (!incoming) presses[pressHead++ % 8] = millis();
    } else {
      for (int i = 0; i < 48; i++)
        if (roll[i].note == n && roll[i].incoming == incoming && roll[i].off == 0 && roll[i].on) roll[i].off = millis();
    }
  }
};

}  // namespace b7

namespace b7v {

using namespace bv;

constexpr uint16_t kOut = kViolet, kIn = kBlue;

// A piano strip: white keys then black keys on top. Lit by what's sounding.
inline void piano(FrameBuffer &fb, const b7::Screen &s, int x, int y, int lowNote, int octaves) {
  static const int whiteSteps[7] = {0, 2, 4, 5, 7, 9, 11};
  static const int blackSteps[5] = {1, 3, 6, 8, 10};
  static const int blackAfter[5] = {0, 1, 3, 4, 5};  // which white key a black key sits after
  const int ww = 13, wh = 34;
  for (int o = 0; o < octaves; o++)
    for (int w = 0; w < 7; w++) {
      const uint8_t n = uint8_t(lowNote + o * 12 + whiteSteps[w]);
      const int kx = x + (o * 7 + w) * ww;
      fb.fillRect(kx, y, ww - 1, wh, s.out(n) ? kOut : s.in(n) ? kIn : pal::ink);
      if (whiteSteps[w] == 0) {
        char nm[6];
        midi::noteName(n, nm, sizeof(nm));
        small(fb, kx + 1, y + wh - 9, pal::night, nm);
      }
    }
  for (int o = 0; o < octaves; o++)
    for (int b = 0; b < 5; b++) {
      const uint8_t n = uint8_t(lowNote + o * 12 + blackSteps[b]);
      const int kx = x + (o * 7 + blackAfter[b]) * ww + ww - 4;
      fb.fillRect(kx, y, 8, wh * 3 / 5, s.out(n) ? kOut : s.in(n) ? kIn : pal::night);
    }
}

inline void keys(FrameBuffer &fb, const b7::Screen &s, float t) {
  // Five pads, the notes of the pentatonic scale at the chosen octave.
  for (int i = 0; i < 5; i++) {
    const uint8_t n = b7::degreeNote(b7::Scale::Pentatonic, s.octave, i);
    const int x = 8 + i * 62, y = 34;
    const bool on = s.padDown[i] || s.out(n);
    fb.fillRoundRect(x, y, 56, 40, 5, on ? kOut : kPanel);
    fb.drawRoundRect(x, y, 56, 40, 5, s.touchOn ? pal::muted : pal::dim);
    char nm[6];
    midi::noteName(n, nm, sizeof(nm));
    bold(fb, x + 8, y + 22, on ? pal::ink : pal::muted, nm);
    static const uint8_t pins[5] = {1, 2, 6, 13, 14};
    char lbl[10];
    snprintf(lbl, sizeof(lbl), "GPIO %u", pins[i]);
    small(fb, x + 6, y + 30, pal::dim, lbl);
    if (s.touchOn) fb.fillRect(x + 4, y + 36, constrain(s.padRise[i], 0, 100) * 48 / 100, 2, pal::amber);
  }
  piano(fb, s, 22, 82, (s.octave) * 12, 2);
  char v[64];
  snprintf(v, sizeof(v), "%s, octave %d, ch %d%s%s", b7::scaleInfo(s.scale).name, s.octave, s.channel + 1, s.touchOn ? ", touch on" : "",
           s.tune ? ", playing" : "");
  small(fb, 8, 122, pal::muted, v);
  fb.fillRect(8, 136, 8, 6, kOut);
  small(fb, 20, 135, pal::muted, "this board");
  fb.fillRect(90, 136, 8, 6, kIn);
  small(fb, 102, 135, pal::muted, "from the synth");
  small(fb, 196, 135, s.stats.subscribed ? pal::teal : pal::amber, s.stats.subscribed ? "synth listening" : "no synth yet");
  (void)t;
}

inline void sequencer(FrameBuffer &fb, const b7::Screen &s, float t) {
  const uint16_t *rows = s.beat ? s.beat->rows : nullptr;
  for (int r = 0; r < 4; r++) {
    const int y = 38 + r * 22;
    small(fb, 6, y + 6, pal::muted, b7::kDrumNames[r]);
    for (int st = 0; st < 16; st++) {
      const int x = 44 + st * 17;
      const bool hit = rows && ((rows[r] >> st) & 1);
      const bool head = s.seqOn && st == s.step;
      uint16_t c = hit ? (head ? pal::amber : kOut) : (st % 4 == 0 ? pal::dim : kPanel);
      fb.fillRoundRect(x, y, 15, 18, 2, c);
      if (head) fb.drawRoundRect(x - 1, y - 1, 17, 20, 3, pal::ink);
    }
  }
  char v[64];
  snprintf(v, sizeof(v), "%s, %d bpm, %s, channel 10 (drums)", s.beat ? s.beat->name : "no beat", s.bpm, s.seqOn ? "playing" : "stopped");
  small(fb, 6, 130, s.seqOn ? pal::ink : pal::muted, v);
  small(fb, 6, 142, pal::dim, "beat rock|funk|house|boom, seq on|off, bpm <n>");
  (void)t;
}

inline void roll(FrameBuffer &fb, const b7::Screen &s, float t) {
  const uint32_t now = millis(), span = 6000;
  const int x0 = 6, x1 = 314, y0 = 34, y1 = 150;
  // Pitch lines every C.
  for (int n = 36; n <= 96; n += 12) {
    const int y = y1 - (n - 36) * (y1 - y0) / 60;
    fb.drawFastHLine(x0, y, x1 - x0, pal::dim);
    char nm[6];
    midi::noteName(uint8_t(n), nm, sizeof(nm));
    small(fb, x0, y - 9, pal::dim, nm);
  }
  for (const b7::RollNote &r : s.roll) {
    if (!r.on || now - r.on > span + 2000) continue;
    const uint32_t end = r.off ? r.off : now;
    if (now - end > span) continue;
    const int xa = x1 - int(int64_t(now - r.on) * (x1 - x0) / span), xb = x1 - int(int64_t(now - end) * (x1 - x0) / span);
    const int n = constrain(int(r.note), 36, 96);
    const int y = y1 - (n - 36) * (y1 - y0) / 60;
    fb.fillRect(std::max(xa, x0), y - 2, std::max(xb - std::max(xa, x0), 2), 4, r.incoming ? kIn : kOut);
  }
  fb.drawFastVLine(x1, y0, y1 - y0, pal::muted);
  char v[64];
  snprintf(v, sizeof(v), "out %lu msgs in %lu packets, in %lu, up to %u per packet", static_cast<unsigned long>(s.stats.msgsOut),
           static_cast<unsigned long>(s.stats.packetsOut), static_cast<unsigned long>(s.stats.msgsIn), s.stats.mostPerPacket);
  small(fb, 6, 152, pal::muted, v);
  (void)t;
}

// Connection events tick at the interval. A note pressed between two of them waits for the next:
// that wait (0 to one interval) is BLE-MIDI's latency. The timestamps inside the packet tell the
// synth when each note really happened, so a fast run of notes keeps its rhythm.
inline void link(FrameBuffer &fb, const ble::LinkState &ls, const b7::Screen &s, float t) {
  const bool up = ls.radio == ble::Radio::Connected;
  char v[64];
  const float interval = up ? ls.peer.interval * 1.25f : 15.0f;
  snprintf(v, sizeof(v), "%.2f ms", interval);
  stat(fb, 6, 34, up ? "connection interval" : "interval (not connected)", v, up ? kBlue : pal::muted);
  snprintf(v, sizeof(v), "0 - %.1f ms", interval);
  stat(fb, 130, 34, "added wait", v, pal::amber);
  snprintf(v, sizeof(v), "%u B", ls.peer.mtu);
  stat(fb, 250, 34, "MTU", up ? v : "-");
  // The timeline: 160 ms drawn across the screen. Ticks = connection events, one per interval.
  // Four presses at fixed moments, lit one after the other: each drops to the next tick.
  const int x0 = 6, x1 = 314, yNote = 88, yRadio = 120;
  const float window = 160.0f;
  auto xAt = [&](float ms) { return x0 + int(ms * (x1 - x0) / window); };
  small(fb, x0, yNote - 16, kOut, "you press");
  small(fb, x0, yRadio + 6, kBlue, "the radio sends");
  fb.drawFastHLine(x0, yRadio, x1 - x0, pal::dim);
  for (float e = 0; e <= window; e += interval) fb.drawFastVLine(xAt(e), yRadio - 4, 8, kBlue);
  const float presses[4] = {12, 49, 93, 131};
  const int lit = int(t * 1.2f) % 4;
  for (int i = 0; i < 4; i++) {
    const float sent = ceilf(presses[i] / interval) * interval;
    const int px = xAt(presses[i]), sx = xAt(sent);
    const bool on = i == lit;
    fb.fillCircle(px, yNote, on ? 4 : 2, on ? kOut : pal::dim);
    if (!on) continue;
    const float m = std::min(1.0f, fmodf(t * 1.2f, 1.0f) * 2);
    fb.drawLine(px, yNote, px + int((sx - px) * m), yNote + int((yRadio - yNote) * m), pal::amber);
    fb.drawFastHLine(px, yNote + 7, std::max(sx - px, 1), pal::amber);
    char w[24];
    snprintf(w, sizeof(w), "+%.1f ms", sent - presses[i]);
    small(fb, std::min(px, x1 - 50), yNote - 8, pal::amber, w);
  }
  snprintf(v, sizeof(v), "%s, %lu notes sent, timestamps keep the rhythm", up ? (ls.peer.encrypted ? "encrypted" : "plain") : "advertising",
           static_cast<unsigned long>(s.stats.msgsOut));
  small(fb, 6, 146, pal::muted, v);
}

}  // namespace b7v
