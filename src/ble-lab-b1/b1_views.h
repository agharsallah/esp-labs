// B1's four screens (320 x 170):
//   radar    every device as a blip: distance from its signal, colour = what it probably is
//   list     the strongest devices with name, address type, signal and age
//   inspect  one device's packets byte by byte, each AD structure in its own colour, decoded
//   air      packets per second over the last minute, how many devices, address types
#pragma once

#include <math.h>

#include "ad_format.h"
#include "ble_views.h"
#include "listener.h"

namespace b1v {

using namespace bv;

inline uint16_t kindColour(ad::Kind k) {
  switch (k) {
    case ad::Kind::Phone: return kBlue;
    case ad::Kind::Computer: return rgb(120, 200, 255);
    case ad::Kind::Audio: return pal::amber;
    case ad::Kind::Wearable: return rgb(255, 120, 200);
    case ad::Kind::Input: return pal::teal;
    case ad::Kind::Beacon: return kViolet;
    case ad::Kind::Tag: return pal::coral;
    case ad::Kind::Tv: return rgb(200, 200, 120);
    case ad::Kind::Bench: return rgb(120, 255, 140);
    default: return pal::muted;
  }
}

inline const char *addrShort(ad::AddrKind k) {
  static const char *const names[] = {"pub", "static", "rpa", "nrpa"};
  return names[static_cast<int>(k)];
}

// Same address = same angle, so a blip only moves in and out as its signal changes.
inline float angleOf(const ble::Heard &h) {
  uint32_t x = 2166136261u;
  for (int i = 0; i < 6; i++) x = (x ^ h.addr[i]) * 16777619u;
  return (x % 3600) / 3600.0f * 6.2832f;
}

// Distance in metres to radar pixels, on a log scale: 0.3 m near the middle, 20 m at the rim.
inline int radarRadius(float meters, int rim) {
  const float k = log10f(1 + fminf(meters, 20.0f)) / log10f(21.0f);
  return int(k * rim);
}

inline void radar(FrameBuffer &fb, const ble::Heard *rows, int n, const b1::Pick &pick, const b1::Settings &set, bool scanning, uint16_t pps, float t) {
  const int cx = 82, cy = 98, rim = 64;
  for (float m : {1.0f, 3.0f, 10.0f}) {
    const int r = radarRadius(m, rim);
    fb.drawCircle(cx, cy, r, pal::dim);
    char lbl[16];
    snprintf(lbl, sizeof(lbl), "%gm", m);
    small(fb, cx + r - 6, cy - 9, pal::dim, lbl);
  }
  fb.drawCircle(cx, cy, rim, pal::dim);
  fb.fillCircle(cx, cy, 3, kBlue);  // us
  const float sweep = fmodf(t * 1.6f, 6.2832f);
  if (scanning)
    for (int i = 0; i < 14; i++) {
      const float a = sweep - i * 0.04f;
      fb.drawLine(cx, cy, cx + int(cosf(a) * rim), cy + int(sinf(a) * rim), mix(pal::teal, pal::night, uint8_t(60 + i * 14)));
    }
  const uint32_t now = millis();
  for (int i = 0; i < n; i++) {
    const ble::Heard &h = rows[i];
    const float a = angleOf(h);
    const int r = radarRadius(ad::distanceMeters(int(h.rssiAvg), set.rssiAt1m), rim);
    const int x = cx + int(cosf(a) * r), y = cy + int(sinf(a) * r);
    // Brighter right after the sweep passes it, dimmer when it hasn't been heard for a while.
    float behind = fmodf(sweep - a + 6.2832f, 6.2832f);
    const uint8_t glow = scanning ? uint8_t(fmaxf(0, 1 - behind / 2.5f) * 120) : 0;
    const uint32_t age = now - h.lastSeen;
    uint16_t c = mix(kindColour(h.kind), pal::ink, glow);
    if (age > 5000) c = mix(c, pal::night, 170);
    const int size = h.rssiAvg > -60 ? 4 : h.rssiAvg > -75 ? 3 : 2;
    fb.fillCircle(x, y, size, c);
    if (pick.matches(h)) fb.drawCircle(x, y, size + 4 + int(sinf(t * 6) * 1.5f), pal::ink);
  }
  // Right: a short legend with the nearest five.
  char v[40];
  snprintf(v, sizeof(v), "%d devices", n);
  bold(fb, 170, 50, pal::ink, v);
  snprintf(v, sizeof(v), "%u packets/s  %s", pps, scanning ? (set.active ? "active" : "passive") : "paused");
  small(fb, 170, 56, scanning ? pal::teal : pal::amber, v);
  for (int i = 0; i < n && i < 6; i++) {
    const ble::Heard &h = rows[i];
    const int y = 72 + i * 14;
    fb.fillCircle(174, y + 3, 3, kindColour(h.kind));
    char label[15];
    strlcpy(label, h.label(), sizeof(label));
    small(fb, 182, y, pick.matches(h) ? pal::ink : pal::muted, label);
    const float d = ad::distanceMeters(int(h.rssiAvg), set.rssiAt1m);
    snprintf(v, sizeof(v), d < 10 ? "%.1fm" : "%.0fm", d);
    small(fb, 284, y, pal::muted, v);
  }
  if (n == 0) small(fb, 170, 76, pal::muted, scanning ? "listening..." : "long press: scan on");
}

inline void list(FrameBuffer &fb, const ble::Heard *rows, int n, const b1::Pick &pick) {
  small(fb, 6, 34, pal::dim, "#  name / maker        addr    dBm         age");
  const uint32_t now = millis();
  for (int i = 0; i < n && i < 9; i++) {
    const ble::Heard &h = rows[i];
    const int y = 46 + i * 12;
    const bool sel = pick.matches(h);
    if (sel) fb.fillRect(0, y - 2, kW, 12, kPanel);
    char v[24];
    snprintf(v, sizeof(v), "%d", i + 1);
    small(fb, 6, y, pal::muted, v);
    fb.fillRect(20, y, 6, 7, kindColour(h.kind));
    char label[19];
    strlcpy(label, h.label(), sizeof(label));
    small(fb, 30, y, sel ? pal::ink : pal::muted, label);
    small(fb, 144, y, h.addrKind() == ad::AddrKind::Resolvable ? kViolet : pal::muted, addrShort(h.addrKind()));
    snprintf(v, sizeof(v), "%d", int(h.rssiAvg));
    small(fb, 186, y, rssiColour(int(h.rssiAvg)), v);
    const int w = std::max(0, std::min(60, (int(h.rssiAvg) + 100) * 60 / 60));
    fb.fillRect(210, y + 1, w, 5, rssiColour(int(h.rssiAvg)));
    snprintf(v, sizeof(v), "%lus", static_cast<unsigned long>((now - h.lastSeen) / 1000));
    small(fb, 290, y, pal::muted, v);
  }
  if (n == 0) small(fb, 6, 60, pal::muted, "No devices yet.");
}

// The colour of AD structure i (cycled), shared by the bytes and their meaning below.
inline uint16_t fieldColour(int i) {
  static const uint16_t c[] = {kBlue, pal::amber, pal::teal, kViolet, pal::coral, rgb(120, 255, 140)};
  return c[i % 6];
}

// One packet as 31 little cells, one per byte: the colour says which AD structure it belongs
// to; the length and type bytes are darker than the data.
inline void packetCells(FrameBuffer &fb, int x, int y, const uint8_t *p, uint8_t n, const char *title) {
  const ad::Parsed ps = ad::split(p, n);
  char v[40];
  snprintf(v, sizeof(v), "%s %u/31", title, n);
  small(fb, x, y, pal::muted, v);
  for (int i = 0; i < 31; i++) {
    uint16_t c = pal::dim;
    if (i < n) {
      c = pal::muted;
      for (int f = 0; f < ps.count; f++) {
        const ad::Field &fl = ps.fields[f];
        if (i >= fl.offset - 2 && i < fl.offset + fl.len) c = i < fl.offset ? mix(fieldColour(f), pal::night, 120) : fieldColour(f);
      }
    }
    fb.fillRect(x + 84 + i * 7, y, 6, 7, c);
  }
}

// Each AD structure of a packet as one decoded line.
inline int fields(FrameBuffer &fb, int x, int y, const uint8_t *p, uint8_t n, int maxLines) {
  const ad::Parsed ps = ad::split(p, n);
  for (int f = 0; f < ps.count && maxLines > 0; f++, maxLines--, y += 11) {
    const ad::Field &fl = ps.fields[f];
    const uint8_t *d = p + fl.offset;
    char v[56];
    switch (fl.type) {
      case ad::type::kFlags:
        snprintf(v, sizeof(v), "Flags %02X:%s%s", d[0], d[0] & 0x02 ? " discoverable" : d[0] & 0x01 ? " limited" : "", d[0] & 0x04 ? ", LE only" : "");
        break;
      case ad::type::kName:
      case ad::type::kShortName: snprintf(v, sizeof(v), "%s \"%.*s\"", ad::typeName(fl.type), std::min<int>(fl.len, 26), reinterpret_cast<const char *>(d)); break;
      case ad::type::kTxPower: snprintf(v, sizeof(v), "TX power %d dBm", int8_t(d[0])); break;
      case ad::type::kAppearance: snprintf(v, sizeof(v), "Appearance %04X: %s", ad::le16(d), ad::kindName(ad::kindFromAppearance(ad::le16(d)))); break;
      case ad::type::kManufacturer: {
        const uint16_t id = fl.len >= 2 ? ad::le16(d) : 0;
        const char *who = ad::companyName(id);
        if (id == 0x004C && fl.len >= 3) snprintf(v, sizeof(v), "Apple: %s (%u B)", ad::appleType(d[2]), fl.len - 2);
        else snprintf(v, sizeof(v), "Maker %04X %s (%u B)", id, who ? who : "", fl.len >= 2 ? fl.len - 2 : 0);
        break;
      }
      case ad::type::kUuid16All:
      case ad::type::kUuid16Some:
      case ad::type::kServiceData16: {
        const uint16_t u = fl.len >= 2 ? ad::le16(d) : 0;
        const char *what = ad::service16Name(u);
        snprintf(v, sizeof(v), "%s %04X %s", fl.type == ad::type::kServiceData16 ? "Data for" : "Service", u, what ? what : "");
        break;
      }
      case ad::type::kUuid128All:
      case ad::type::kUuid128Some:
        snprintf(v, sizeof(v), "Service %02x%02x%02x%02x-... (128-bit)", d[15], d[14], d[13], d[12]);
        break;
      default: snprintf(v, sizeof(v), "%02X %s (%u B)", fl.type, ad::typeName(fl.type), fl.len); break;
    }
    fb.fillRect(x, y + 1, 4, 5, fieldColour(f));
    small(fb, x + 8, y, pal::ink, v);
  }
  if (ps.malformed && maxLines > 0) small(fb, x + 8, y, pal::coral, "malformed: a length runs past the end"), y += 11;
  return y;
}

inline void inspect(FrameBuffer &fb, const ble::Heard *h, const b1::Settings &set) {
  if (!h) {
    small(fb, 8, 50, pal::muted, "Pick a device: double-click on the list view,");
    small(fb, 8, 62, pal::muted, "or type pick <n> (the number on the list).");
    return;
  }
  char addr[18], v[60];
  h->addrText(addr, sizeof(addr));
  bold(fb, 6, 48, kindColour(h->kind), h->label());
  snprintf(v, sizeof(v), "%s  %s  %s%s", addr, ad::addrKindName(h->addrKind()), h->connectable ? "connectable" : "broadcast only",
           h->scanResponse ? " +rsp" : "");
  small(fb, 6, 54, pal::muted, v);
  snprintf(v, sizeof(v), "%d dBm (avg %d) ~%.1f m, %lu pkts, every %lu ms", h->rssi, int(h->rssiAvg), ad::distanceMeters(int(h->rssiAvg), set.rssiAt1m),
           static_cast<unsigned long>(h->packets), static_cast<unsigned long>(h->lastInterval));
  small(fb, 6, 64, pal::ink, v);
  packetCells(fb, 6, 78, h->adv, h->advLen, "advert");
  int y = fields(fb, 6, 90, h->adv, h->advLen, h->rspLen ? 3 : 6);
  if (h->rspLen) {
    packetCells(fb, 6, y + 2, h->rsp, h->rspLen, "scan rsp");
    fields(fb, 6, y + 14, h->rsp, h->rspLen, 3);
  }
}

inline void air(FrameBuffer &fb, const b1::AirHistory &hist, const ble::Heard *rows, int n, const b1::Settings &set, bool scanning) {
  // Packets per second, last 60 s, as bars; the device count as a line over them.
  const int x0 = 6, y0 = 120, w = 180, h = 74;
  uint16_t peak = 10;
  for (int i = 0; i < b1::AirHistory::kN; i++) peak = std::max(peak, hist.ppsAt(i));
  uint8_t peakDev = 4;
  for (int i = 0; i < b1::AirHistory::kN; i++) peakDev = std::max(peakDev, hist.devicesAt(i));
  small(fb, x0, 34, pal::muted, "packets / s, last minute");
  fb.drawFastHLine(x0, y0, w, pal::dim);
  int lastX = -1, lastY = 0;
  for (int i = 0; i < b1::AirHistory::kN; i++) {
    const int x = x0 + i * 3;
    const int bh = hist.ppsAt(i) * h / peak;
    fb.fillRect(x, y0 - bh, 2, bh, mix(pal::teal, pal::night, 90));
    const int dy = y0 - hist.devicesAt(i) * h / peakDev;
    if (lastX >= 0) fb.drawLine(lastX, lastY, x, dy, pal::amber);
    lastX = x, lastY = dy;
  }
  char v[64];
  snprintf(v, sizeof(v), "peak %u/s", peak);
  small(fb, x0, y0 + 4, pal::teal, v);
  snprintf(v, sizeof(v), "devices (max %u)", peakDev);
  small(fb, x0 + 80, y0 + 4, pal::amber, v);
  snprintf(v, sizeof(v), "%s, %u/%u ms, %u%% listening", scanning ? (set.active ? "active" : "passive") : "paused", set.windowMs, set.intervalMs,
           set.windowMs * 100 / set.intervalMs);
  small(fb, x0, y0 + 16, pal::muted, v);

  // Right: address types. Most phones hide behind private addresses that change every ~15 min.
  int counts[4] = {};
  for (int i = 0; i < n; i++) counts[static_cast<int>(rows[i].addrKind())]++;
  small(fb, 200, 34, pal::muted, "address types");
  static const uint16_t col[] = {pal::ink, pal::teal, kViolet, pal::muted};
  for (int k = 0; k < 4; k++) {
    const int y = 46 + k * 22;
    snprintf(v, sizeof(v), "%s  %d", ad::addrKindName(static_cast<ad::AddrKind>(k)), counts[k]);
    small(fb, 200, y, pal::muted, v);
    fb.drawRect(200, y + 9, 112, 6, pal::dim);
    fb.fillRect(200, y + 9, n ? counts[k] * 112 / n : 0, 6, col[k]);
  }
  small(fb, 200, 136, kViolet, "rpa = a phone hiding");
}

}  // namespace b1v
