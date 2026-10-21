// B5's own screens (320 x 170); gatt, link and log come from kit/lib/ble-kit:
//   sketch  what the app draws, live, plus how many commands and bytes arrived
//   live    the four sensor values the app charts, as sparklines over the last 12 s
//   speed   the speed test: a running transfer as packets flying, past runs as kB/s bars
#pragma once

#include "ble_views.h"
#include "extras.h"
#include "sketch.h"

namespace b5 {

// The last 60 sensor samples (one per 200 ms = 12 s).
struct Samples {
  static constexpr int kN = 60;
  float v[4][kN] = {};
  int head = 0;
  void push(float temp, float touch, float heap, float rssi) {
    const int i = head % kN;
    v[0][i] = temp, v[1][i] = touch, v[2][i] = heap, v[3][i] = rssi;
    head++;
  }
  float at(int s, int i) const { return head < kN ? v[s][i] : v[s][(head + i) % kN]; }
  int count() const { return head < kN ? head : kN; }
};

}  // namespace b5

namespace b5v {

using namespace bv;

inline void sketch(FrameBuffer &fb, b5::Sketch &sk, const b5::State &s, bool connected) {
  sk.blit(fb, 29);
  if (sk.commands() == 0) {
    bold(fb, 10, 70, pal::ink, connected ? "Draw in the app's Sketch tab" : "Open the ble-app, connect");
    small(fb, 10, 84, pal::muted, "Every stroke arrives as a write without response");
    small(fb, 10, 96, pal::muted, "to Canvas (ba5e0010) and lands here.");
  }
  char v[48];
  snprintf(v, sizeof(v), "%lu cmds %lu writes %lu B%s", static_cast<unsigned long>(sk.commands()), static_cast<unsigned long>(s.canvasWrites),
           static_cast<unsigned long>(s.canvasBytes), s.dropped ? " DROPPED" : "");
  fb.fillRect(0, 146, kW, 11, pal::night);
  small(fb, 6, 148, s.dropped ? pal::amber : pal::muted, v);
}

inline void sparkline(FrameBuffer &fb, int x, int y, int w, int h, const b5::Samples &sm, int series, uint16_t c) {
  const int n = sm.count();
  if (n < 2) return;
  float lo = sm.at(series, 0), hi = lo;
  for (int i = 1; i < n; i++) lo = std::min(lo, sm.at(series, i)), hi = std::max(hi, sm.at(series, i));
  if (hi - lo < 1e-3f) hi = lo + 1;
  int px = -1, py = 0;
  for (int i = 0; i < n; i++) {
    const int xx = x + i * (w - 1) / (b5::Samples::kN - 1);
    const int yy = y + h - 1 - int((sm.at(series, i) - lo) * (h - 1) / (hi - lo));
    if (px >= 0) fb.drawLine(px, py, xx, yy, c);
    px = xx, py = yy;
  }
  fb.fillCircle(px, py, 2, c);
}

inline void live(FrameBuffer &fb, const b5::Samples &sm, const b5::State &s) {
  static const char *const names[] = {"temperature", "touch GPIO 1", "free heap", "link RSSI"};
  static const uint16_t colours[] = {pal::amber, pal::teal, kViolet, kBlue};
  for (int k = 0; k < 4; k++) {
    const int y = 34 + k * 30;
    const float last = sm.count() ? sm.at(k, sm.count() - 1) : 0;
    char v[24];
    if (k == 0) snprintf(v, sizeof(v), "%.2f C", last);
    else if (k == 1) snprintf(v, sizeof(v), "%lu", static_cast<unsigned long>(last));
    else if (k == 2) snprintf(v, sizeof(v), "%lu kB", static_cast<unsigned long>(last / 1024));
    else snprintf(v, sizeof(v), last != 0 ? "%d dBm" : "-", int(last));
    small(fb, 6, y, pal::muted, names[k]);
    bold(fb, 6, y + 20, colours[k], v);
    fb.drawFastHLine(110, y + 24, 204, pal::dim);
    sparkline(fb, 110, y, 204, 24, sm, k, colours[k]);
  }
  char v[48];
  snprintf(v, sizeof(v), s.subscribed[b5::Sensors] ? "notifying the app: %lu sent" : "app not subscribed to Sensors (%lu sent)",
           static_cast<unsigned long>(s.sensorNotifies));
  small(fb, 110, 150 - 2, s.subscribed[b5::Sensors] ? pal::teal : pal::muted, v);
}

inline void speed(FrameBuffer &fb, const b5::State &s, float t) {
  if (s.running) {
    // A transfer in flight: packets streaming from the board (left) to the phone (right).
    small(fb, 6, 36, pal::amber, "sending...");
    fb.drawRoundRect(6, 50, 20, 30, 3, kBlue);
    fb.drawRoundRect(290, 46, 22, 38, 4, pal::muted);
    for (int i = 0; i < 8; i++) {
      const float k = fmodf(t * 2.0f + i / 8.0f, 1.0f);
      fb.fillRect(30 + int(k * 252), 62, 6, 6, mix(kViolet, pal::night, uint8_t(k * 120)));
    }
    const uint32_t pct = s.target ? s.sent * 100 / s.target : 0;
    fb.drawRect(6, 92, 308, 8, pal::dim);
    fb.fillRect(7, 93, int(306 * pct / 100), 6, kViolet);
    char v[56];
    const uint32_t ms = std::max<uint32_t>(millis() - s.startedAt, 1);
    snprintf(v, sizeof(v), "%lu / %lu B  %.1f kB/s  %lu waits", static_cast<unsigned long>(s.sent), static_cast<unsigned long>(s.target),
             s.sent / float(ms), static_cast<unsigned long>(s.retries));
    small(fb, 6, 106, pal::ink, v);
    small(fb, 6, 120, pal::muted, "A wait = the stack's buffers were full:");
    small(fb, 6, 130, pal::muted, "the radio sends only in connection events.");
    return;
  }
  const int shown = std::min(s.runCount, 6);
  if (shown == 0) {
    bold(fb, 6, 60, pal::ink, "No speed runs yet");
    small(fb, 6, 74, pal::muted, "App: Speed tab, Run. Or serial: speed 50000");
    small(fb, 6, 86, pal::muted, "(the app must subscribe to Speed first).");
    small(fb, 6, 104, pal::muted, "kB/s ~ (MTU - 3) x packets per event / interval");
    return;
  }
  float peak = 1;
  for (int i = 0; i < shown; i++) peak = std::max(peak, s.runs[(s.runCount - shown + i) % 6].kBps());
  static const char *const phy[] = {"?", "1M", "2M", "Cd"};
  for (int i = 0; i < shown; i++) {
    const b5::SpeedRun &r = s.runs[(s.runCount - shown + i) % 6];
    const int y = 36 + i * 20;
    const int w = int(r.kBps() * 150 / peak);
    fb.fillRect(6, y, std::max(w, 1), 12, i == shown - 1 ? kViolet : mix(kViolet, pal::night, 110));
    char v[48];
    snprintf(v, sizeof(v), "%.1f kB/s", r.kBps());
    small(fb, 10 + std::max(w, 1), y + 2, pal::ink, v);
    snprintf(v, sizeof(v), "MTU %u %.1fms %s", r.mtu, r.interval * 1.25f, phy[r.phy & 3]);
    small(fb, 216, y + 2, pal::muted, v);
  }
}

}  // namespace b5v
