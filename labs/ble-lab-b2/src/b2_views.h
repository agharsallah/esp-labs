// B2's four screens (320 x 170):
//   packet  the advertisement and the scan response as 31 cells each, one colour per AD structure,
//           decoded underneath. Bytes that changed in the last update flash white (the live values).
//   air     what the radio does with it: one advertising event every interval + advDelay, each event
//           one packet on channel 37, then 38, then 39. Drawn from the settings (the controller
//           doesn't report single events), so it's the timing, not a capture.
//   budget  what this costs a coin-cell beacon: an estimate from the interval, TX power and size
//   log     the activity log
#pragma once

#include <math.h>

#include "ad_format.h"
#include "beacon.h"
#include "ble_views.h"

namespace b2v {

using namespace bv;

inline uint16_t fieldColour(int i) {
  static const uint16_t c[] = {kBlue, pal::amber, pal::teal, kViolet, pal::coral};
  return c[i % 5];
}

// 31 cells for one packet; the length and type bytes darker than the data.
inline void cells(FrameBuffer &fb, int x, int y, const b2::Packet &p, const uint32_t *changedAt, const char *title) {
  const ad::Parsed ps = ad::split(p.b, p.n);
  const uint32_t now = millis();
  char v[24];
  snprintf(v, sizeof(v), "%s %u/31", title, p.n);
  small(fb, x, y + 1, p.n ? pal::muted : pal::dim, v);
  for (int i = 0; i < 31; i++) {
    uint16_t c = pal::dim;
    for (int f = 0; f < ps.count; f++) {
      const ad::Field &fl = ps.fields[f];
      if (i >= fl.offset - 2 && i < fl.offset + fl.len) c = i < fl.offset ? mix(fieldColour(f), pal::night, 120) : fieldColour(f);
    }
    if (changedAt && changedAt[i] && i < p.n) {
      const uint32_t age = now - changedAt[i];
      if (age < 700) c = mix(pal::ink, c, uint8_t(age * 255 / 700));
    }
    fb.fillRect(x + 76 + i * 7, y, 6, 9, c);
  }
}

// One decoded line per AD structure. Returns the next free y.
inline int decode(FrameBuffer &fb, int x, int y, const b2::Packet &p, const b2::Settings &s, int maxLines) {
  const ad::Parsed ps = ad::split(p.b, p.n);
  for (int f = 0; f < ps.count && maxLines > 0; f++, maxLines--, y += 11) {
    const ad::Field &fl = ps.fields[f];
    const uint8_t *d = p.b + fl.offset;
    char v[60];
    b2::Live live;
    if (fl.type == ad::type::kFlags) snprintf(v, sizeof(v), "Flags %02X: discoverable, LE only", d[0]);
    else if (fl.type == ad::type::kName) snprintf(v, sizeof(v), "Name \"%.*s\"", fl.len, reinterpret_cast<const char *>(d));
    else if (fl.type == ad::type::kTxPower) snprintf(v, sizeof(v), "TX power %d dBm", int8_t(d[0]));
    else if (fl.type == ad::type::kAppearance) snprintf(v, sizeof(v), "Appearance %04X (generic sensor)", ad::le16(d));
    else if (fl.type == ad::type::kUuid16All) snprintf(v, sizeof(v), "Services: %04X (Eddystone)", ad::le16(d));
    else if (fl.type == ad::type::kServiceData16) snprintf(v, sizeof(v), "Eddystone-URL %.32s", s.url);
    else if (fl.type == ad::type::kManufacturer && b2::parseBench(d, fl.len, live))
      snprintf(v, sizeof(v), "FFFF: %u presses %.2fC up %us", live.presses, live.tempCenti / 100.0f, live.uptime);
    else if (fl.type == ad::type::kManufacturer && fl.len == 25)
      snprintf(v, sizeof(v), "iBeacon major %u minor %u, %d dBm at 1 m", s.beacon.major, s.beacon.minor, s.beacon.power);
    else snprintf(v, sizeof(v), "%02X %s (%u B)", fl.type, ad::typeName(fl.type), fl.len);
    fb.fillRect(x, y + 1, 4, 5, fieldColour(f));
    small(fb, x + 8, y, pal::ink, v);
  }
  return y;
}

inline void packet(FrameBuffer &fb, const b2::Beacon &b, bool on) {
  const b2::Settings &s = b.settings;
  char v[64];
  snprintf(v, sizeof(v), "%s  %s  every %u ms", b2::modeName(s.mode), b2::pduName(s), s.intervalMs);
  bold(fb, 6, 48, on ? kBlue : pal::muted, v);
  if (!on) small(fb, 230, 36, pal::amber, "advertising off");
  cells(fb, 6, 56, b.adv, b.changedAt, "advert");
  int y = decode(fb, 6, 70, b.adv, s, 4);
  cells(fb, 6, y + 3, b.rsp, nullptr, "scan rsp");
  if (b.rsp.n) decode(fb, 6, y + 17, b.rsp, s, 2);
  else small(fb, 6, y + 17, pal::muted, s.scanResponse ? "(nothing left to put here)" : "off: a scanner that asks gets no answer");
}

// ---- air ----------------------------------------------------------------------------------------

// The advertising events, as the controller would time them from our settings.
struct AirSim {
  static constexpr int kN = 48;
  uint32_t at[kN] = {};  // event start times, ms
  uint8_t delay[kN] = {};
  int head = 0;
  uint32_t next = 0;

  void tick(uint16_t intervalMs, bool on) {
    const uint32_t now = millis();
    if (!on) {
      next = now;
      return;
    }
    while (int32_t(now - next) >= 0) {
      const uint8_t d = uint8_t(esp_random() % 11);  // advDelay: 0-10 ms, fresh every event
      at[head % kN] = next;
      delay[head % kN] = d;
      head++;
      next += intervalMs + d;
    }
  }
  uint32_t last() const { return head ? at[(head - 1) % kN] : 0; }
};

inline void air(FrameBuffer &fb, const b2::Beacon &b, const AirSim &sim, bool on, float t) {
  const b2::Settings &s = b.settings;
  const uint32_t now = millis();
  // The board, sending rings when an event fires.
  const int bx = 26, by = 66;
  fb.fillRoundRect(bx - 12, by - 9, 24, 18, 3, kPanel);
  fb.drawRoundRect(bx - 12, by - 9, 24, 18, 3, kBlue);
  small(fb, bx - 9, by - 3, pal::ink, "B2");
  const uint32_t since = now - sim.last();
  if (on && since < 400) {
    for (int i = 0; i < 3; i++) {
      const int r = 14 + int(since / 400.0f * 22) + i * 5;
      fb.drawCircle(bx, by, r, mix(kBlue, pal::night, uint8_t(std::min<uint32_t>(255, since * 255 / 400 + i * 50))));
    }
  }
  // Timeline: the last few intervals, newest on the right. Three lanes, one per channel.
  const uint32_t span = std::max<uint32_t>(120, std::min<uint32_t>(6000, s.intervalMs * 3 + 60));
  const int x0 = 70, x1 = 312, w = x1 - x0;
  static const char *const ch[] = {"37", "38", "39"};
  static const char *const mhz[] = {"2402", "2426", "2480"};
  for (int c = 0; c < 3; c++) {
    const int y = 44 + c * 16;
    fb.drawFastHLine(x0, y + 6, w, pal::dim);
    small(fb, 46, y + 2, pal::muted, ch[c]);
  }
  // Each packet takes ~0.4 ms on air + switching: at any span here they sit almost on top of each
  // other, so each lane gets a 2 px nudge and the inset below shows one event magnified.
  for (int i = 0; i < AirSim::kN && i < sim.head; i++) {
    const int k = (sim.head - 1 - i) % AirSim::kN;
    const uint32_t age = now - sim.at[k];
    if (age > span) break;
    const int x = x1 - int(age * w / span);
    for (int c = 0; c < 3; c++) fb.fillRect(x + c * 2, 44 + c * 16 + 2, 2, 9, c == 0 ? kBlue : c == 1 ? pal::teal : kViolet);
    if (i < 3) {
      char d[8];
      snprintf(d, sizeof(d), "+%u", sim.delay[k]);
      small(fb, x - 6, 92, pal::dim, d);
    }
  }
  char v[64];
  snprintf(v, sizeof(v), "last %lu ms", static_cast<unsigned long>(span));
  small(fb, x0, 102, pal::dim, v);
  small(fb, x1 - 18, 102, pal::dim, "now");

  // Inset: one event magnified. Each packet = 16 B of header and checksum + our payload, 8 us per byte
  // at 1 Mbit/s, then ~150 us for the radio to hop to the next channel.
  const int airUs = (16 + b.adv.n) * 8;
  const int iy = 116;
  small(fb, 6, iy, pal::muted, "one event:");
  const float k = 200.0f / ((airUs + 150) * 3);
  for (int c = 0; c < 3; c++) {
    const int x = 70 + int(c * (airUs + 150) * k);
    fb.fillRect(x, iy - 1, std::max(2, int(airUs * k)), 9, c == 0 ? kBlue : c == 1 ? pal::teal : kViolet);
    char lbl[16];
    snprintf(lbl, sizeof(lbl), "ch%s %s", ch[c], mhz[c]);
    small(fb, x, iy + 11, pal::muted, lbl);
  }
  snprintf(v, sizeof(v), "%d us each, %d us per event", airUs, (airUs + 150) * 3);
  small(fb, 6, iy + 24, pal::ink, v);
  const float perSec = 1000.0f / (s.intervalMs + 5);
  snprintf(v, sizeof(v), "%.1f events/s = %.0f packets/s%s", perSec, perSec * 3, on ? "" : "  (off)");
  small(fb, 6, iy + 34, on ? pal::teal : pal::muted, v);
  (void)t;
}

// ---- budget -------------------------------------------------------------------------------------

// Radio current while sending, by TX power, for a typical coin-cell beacon chip (nRF52-class, 3 V,
// DC/DC on). Above +8 dBm such chips need an external amplifier: those rows are rougher still.
inline float txMilliamps(int dbm) {
  static const int8_t p[] = {-24, -12, -8, -4, 0, 4, 8, 12, 16, 20};
  static const float ma[] = {3.2f, 4.0f, 4.3f, 4.8f, 5.3f, 7.5f, 10.0f, 14.0f, 19.0f, 26.0f};
  if (dbm <= p[0]) return ma[0];
  for (int i = 1; i < 10; i++)
    if (dbm <= p[i]) return ma[i - 1] + (ma[i] - ma[i - 1]) * (dbm - p[i - 1]) / float(p[i] - p[i - 1]);
  return ma[9];
}

struct Budget {
  float txUa, rxUa, wakeUa, sleepUa, totalUa, days;
};

// Charge per event = 3 packets at the TX current + 3 short listens (for scan requests) + waking up,
// spread over the interval, plus sleep. CR2032: 220 mAh, 80 % usable.
inline Budget estimate(uint16_t intervalMs, int dbm, int payloadBytes) {
  const float airMs = (16 + payloadBytes) * 8 / 1000.0f;
  Budget b;
  b.txUa = 3 * airMs * txMilliamps(dbm) / (intervalMs + 5) * 1000;
  b.rxUa = 3 * 0.15f * 5.0f / (intervalMs + 5) * 1000;
  b.wakeUa = 0.4f * 2.0f / (intervalMs + 5) * 1000;
  b.sleepUa = 2.0f;
  b.totalUa = b.txUa + b.rxUa + b.wakeUa + b.sleepUa;
  b.days = 220.0f * 0.8f * 1000 / b.totalUa / 24;
  return b;
}

inline void budget(FrameBuffer &fb, const b2::Beacon &b) {
  const int dbm = NimBLEDevice::getPower();
  const b2::Settings &s = b.settings;
  const Budget e = estimate(s.intervalMs, dbm, b.adv.n);
  char v[64];
  small(fb, 6, 34, pal::amber, "ESTIMATE for a coin-cell beacon chip, not this board");
  // Stacked bar of where the current goes.
  const float parts[] = {e.txUa, e.rxUa, e.wakeUa, e.sleepUa};
  static const char *const names[] = {"sending", "listening", "waking", "sleeping"};
  const uint16_t cols[] = {kBlue, pal::teal, pal::amber, pal::muted};
  int x = 6;
  for (int i = 0; i < 4; i++) {
    const int w = int(parts[i] / e.totalUa * 300);
    fb.fillRect(x, 46, w, 10, cols[i]);
    x += w;
  }
  for (int i = 0; i < 4; i++) {
    snprintf(v, sizeof(v), "%s %.1f uA", names[i], parts[i]);
    fb.fillRect(6 + i * 78, 61, 5, 5, cols[i]);
    small(fb, 14 + i * 78, 60, pal::muted, v);
  }
  snprintf(v, sizeof(v), "%.1f uA", e.totalUa);
  stat(fb, 6, 72, "average", v, pal::ink);
  if (e.days > 730) snprintf(v, sizeof(v), "%.1f years", e.days / 365);
  else snprintf(v, sizeof(v), "%.0f days", e.days);
  stat(fb, 100, 72, "on a CR2032", v, e.days > 365 ? pal::teal : e.days > 60 ? pal::amber : pal::coral);
  snprintf(v, sizeof(v), "%d dBm, %u ms", dbm, s.intervalMs);
  stat(fb, 210, 72, "settings", v);

  // Life against interval, 20 ms to 10 s (log scale), at this power; the dot = now.
  const int gx = 30, gy = 150, gw = 280, gh = 42;
  fb.drawFastHLine(gx, gy, gw, pal::dim);
  float maxDays = estimate(10000, dbm, b.adv.n).days;
  int lastX = -1, lastY = 0;
  for (int i = 0; i <= 40; i++) {
    const float ms = 20 * powf(500, i / 40.0f);
    const int px = gx + i * gw / 40;
    const int py = gy - int(estimate(uint16_t(ms), dbm, b.adv.n).days / maxDays * gh);
    if (lastX >= 0) fb.drawLine(lastX, lastY, px, py, kBlue);
    lastX = px, lastY = py;
  }
  const float pos = logf(s.intervalMs / 20.0f) / logf(500);
  const int cx = gx + int(pos * gw);
  fb.fillCircle(cx, gy - int(e.days / maxDays * gh), 3, pal::amber);
  small(fb, 4, 112, pal::dim, "life");
  small(fb, gx - 2, gy + 2, pal::dim, "20ms");
  small(fb, gx + gw - 24, gy + 2, pal::dim, "10s");
  small(fb, gx + gw / 2 - 30, gy + 2, pal::dim, "interval (log)");
  small(fb, 4, 98, pal::muted, "this board draws ~80 mA: far too much for a coin cell");
}

}  // namespace b2v
