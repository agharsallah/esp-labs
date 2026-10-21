// B8's screens (320 x 170):
//   devices   connectable devices around you; keyboards and mice marked HID, bonded ones with a key
//   explore   the connected device's GATT tree: services, characteristics (props + read values), descriptors
//   keyboard  a little text editor: what you type on YOUR keyboard appears here, modifiers light up
//   mouse     a paint toy: left button draws, right changes colour, middle clears, wheel = brush size
//   reports   the raw input reports, each byte coloured by the Report Map field it belongs to
//   (link and log come from kit/lib/ble-kit)
#pragma once

#include "ble_views.h"
#include "central.h"
#include "host_state.h"

namespace b8v {

using namespace bv;

struct Pick {
  uint8_t addr[6] = {};
  uint8_t type = 0;
  bool set = false;
  bool matches(const ble::Heard &h) const { return set && h.addrType == type && memcmp(h.addr, addr, 6) == 0; }
  void take(const ble::Heard &h) {
    memcpy(addr, h.addr, 6);
    type = h.addrType;
    set = true;
  }
};

inline void hidGlyph(FrameBuffer &fb, int x, int y, uint16_t c) {
  fb.drawRect(x, y, 13, 8, c);
  for (int i = 0; i < 3; i++) fb.drawFastHLine(x + 2 + i * 3, y + 2, 2, c), fb.drawFastHLine(x + 3 + i * 3, y + 4, 2, c);
  fb.drawFastHLine(x + 4, y + 6, 5, c);
}

// A busy phase as a panel over the view: what the central task is waiting for right now.
inline void busy(FrameBuffer &fb, const b8::Progress &p, float t) {
  if (p.phase == b8::Phase::Idle || p.phase == b8::Phase::Hosting) return;
  fb.fillRoundRect(40, 74, 240, 46, 8, kPanel);
  fb.drawRoundRect(40, 74, 240, 46, 8, pal::amber);
  char v[48];
  snprintf(v, sizeof(v), "%s %s", b8::phaseName(p.phase), p.phase == b8::Phase::Connecting ? p.target : "");
  bold(fb, 52, 94, pal::ink, v);
  // Packets going back and forth while we wait.
  for (int i = 0; i < 3; i++) {
    const float k = fmodf(t * 0.9f + i / 3.0f, 1.0f);
    fb.fillCircle(56 + int(k * 208), 108, 2, i == 0 ? kBlue : mix(kBlue, kPanel, 120));
  }
  snprintf(v, sizeof(v), "%lus", static_cast<unsigned long>((millis() - p.since) / 1000));
  small(fb, 250, 82, pal::muted, v);
}

inline void devices(FrameBuffer &fb, const ble::Heard *rows, int n, const Pick &pick, b8::Central &c, const b8::Progress &p, float t) {
  small(fb, 6, 34, pal::dim, "#  device (connectable only)       signal  dBm");
  for (int i = 0; i < n && i < 9; i++) {
    const ble::Heard &h = rows[i];
    const int y = 46 + i * 12;
    const bool sel = pick.matches(h);
    if (sel) fb.fillRect(0, y - 2, kW, 12, kPanel);
    char v[24];
    snprintf(v, sizeof(v), "%d", i + 1);
    small(fb, 6, y, pal::muted, v);
    if (h.kind == ad::Kind::Input) hidGlyph(fb, 20, y - 1, pal::teal);
    char label[22];
    strlcpy(label, h.label(), sizeof(label));
    small(fb, 38, y, sel ? pal::ink : pal::muted, label);
    if (c.isBonded(h.addr, h.addrType)) keyIcon(fb, 168, y - 3, pal::amber);
    signalBars(fb, 230, y - 4, int(h.rssiAvg), rssiColour(int(h.rssiAvg)));
    snprintf(v, sizeof(v), "%d", int(h.rssiAvg));
    small(fb, 256, y, rssiColour(int(h.rssiAvg)), v);
    if (h.kind == ad::Kind::Input) small(fb, 284, y, pal::teal, "HID");
  }
  if (n == 0) {
    small(fb, 6, 60, pal::muted, "Nothing connectable yet. Put your keyboard or");
    small(fb, 6, 72, pal::muted, "mouse in pairing mode (hold its Bluetooth key).");
  }
  busy(fb, p, t);
}

inline void explore(FrameBuffer &fb, const b8::Tree &tr, int scroll, const b8::Progress &p, float t) {
  if (tr.count == 0) {
    small(fb, 6, 44, pal::muted, "Connect to a device first: devices view,");
    small(fb, 6, 56, pal::muted, "pick it (double-click), then long press.");
    small(fb, 6, 76, pal::muted, "Explore lists what it offers. We only READ the");
    small(fb, 6, 88, pal::muted, "public basics (name, maker, model, battery).");
    return busy(fb, p, t);
  }
  char v[56];
  snprintf(v, sizeof(v), "%.18s  %d services %d chars", tr.device[0] ? tr.device : "device", tr.services, tr.characteristics);
  small(fb, 6, 34, pal::ink, v);
  const int rows = (tr.vendorOnly || tr.hasHid) ? 8 : 10;
  for (int r = 0; r < rows && scroll + r < tr.count; r++) {
    const b8::Node &nd = tr.nodes[scroll + r];
    const int y = 46 + r * 11, x = 6 + nd.level * 10;
    const uint16_t c = nd.level == 0 ? kBlue : nd.level == 1 ? pal::ink : pal::dim;
    if (nd.uuid16) snprintf(v, sizeof(v), "%04X %s", nd.uuid16, nd.name);
    else snprintf(v, sizeof(v), "%s", nd.name);
    small(fb, x, y, c, v);
    if (nd.level == 1) {
      char props[8] = "";
      int k = 0;
      if (nd.props & 0x02) props[k++] = 'R';
      if (nd.props & 0x0C) props[k++] = 'W';
      if (nd.props & 0x10) props[k++] = 'N';
      if (nd.props & 0x20) props[k++] = 'I';
      props[k] = 0;
      small(fb, 160, y, pal::muted, props);
    }
    if (nd.value[0]) {
      char val[24];
      strlcpy(val, nd.value, sizeof(val));
      small(fb, 186, y, nd.level == 0 ? pal::dim : pal::teal, val);
    }
  }
  if (tr.count > rows) {
    snprintf(v, sizeof(v), "%d-%d of %d", scroll + 1, std::min(scroll + rows, tr.count), tr.count);
    small(fb, 252, 34, pal::muted, v);
  }
  if (tr.hasHid) small(fb, 6, 140, pal::teal, "A keyboard or mouse (HID 1812): long press = host");
  else if (tr.vendorOnly && tr.audio) {
    small(fb, 6, 134, pal::amber, "This is the JBL app's own service. The audio");
    small(fb, 6, 144, pal::amber, "goes over Classic Bluetooth: this chip has none.");
  } else if (tr.vendorOnly) {
    small(fb, 6, 140, pal::amber, "Only the maker's own services: its app speaks them.");
  }
  busy(fb, p, t);
}

inline void modCaps(FrameBuffer &fb, int y, uint8_t mods) {
  static const char *const caps[] = {"Ctl", "Sft", "Alt", "Cmd", "Ctl", "Sft", "AGr", "Cmd"};
  for (int i = 0; i < 8; i++) {
    const int x = 6 + i * 26 + (i >= 4 ? 8 : 0);
    const bool on = mods & (1 << i);
    fb.fillRoundRect(x, y, 23, 12, 2, on ? pal::amber : kPanel);
    small(fb, x + 3, y + 2, on ? pal::night : pal::muted, caps[i]);
  }
}

inline void hint(FrameBuffer &fb, const b8::Progress &p, bool connected, const char *what) {
  char v[56];
  if (!connected) snprintf(v, sizeof(v), "No %s yet: connect one on the devices view.", what);
  else if (p.phase != b8::Phase::Hosting) snprintf(v, sizeof(v), "Connected. Long press (or host) to use the %s.", what);
  else snprintf(v, sizeof(v), "This device sends no %s reports.", what);
  small(fb, 6, 60, pal::muted, v);
}

inline void keyboard(FrameBuffer &fb, const b8::Input &in, const b8::Progress &p, bool connected, hid::Layout layout, float t) {
  if (!p.keyboard) {
    hint(fb, p, connected, "keyboard");
    return busy(fb, p, t);
  }
  // The text in the screen's built-in 6x8 font at size 2 (12 x 16 px): 26 columns, 5 lines.
  // cp437 mode = its 256 glyphs include the German umlauts.
  constexpr int kCols = 26, kLines = 5;
  // Where each line starts; only the last kLines are kept (a ring), however much was typed.
  int starts[kLines], lines = 0, col = 0;
  starts[lines++ % kLines] = 0;
  for (int i = 0; i < in.len; i++) {
    if (in.text[i] == '\n' || col == kCols) {
      starts[lines++ % kLines] = in.text[i] == '\n' ? i + 1 : i;
      col = in.text[i] == '\n' ? 0 : 1;
    } else {
      col++;
    }
  }
  const int shown = std::min(lines, kLines);
  fb.setFont(nullptr);
  fb.cp437(true);
  fb.setTextSize(2);
  fb.setTextColor(pal::ink);
  int cx = 6, cy = 36;
  for (int l = 0; l < shown; l++) {
    const int line = lines - shown + l;
    const int from = starts[line % kLines], to = l + 1 < shown ? starts[(line + 1) % kLines] : in.len;
    cy = 36 + l * 17;
    fb.setCursor(6, cy);
    for (int i = from; i < to; i++)
      if (in.text[i] != '\n') fb.write(in.text[i]);
    cx = fb.getCursorX();
    if (to > from && in.text[to - 1] == '\n' && l + 1 == shown) cx = 6;
  }
  fb.setTextSize(1);
  fb.cp437(false);
  if (int(t * 2) & 1) fb.fillRect(cx, cy, 10, 15, pal::teal);  // the cursor
  modCaps(fb, 124, in.mods);
  char v[48];
  int at = snprintf(v, sizeof(v), "keys");
  for (int i = 0; i < in.heldN && at < 40; i++) at += snprintf(v + at, sizeof(v) - at, " %02X", in.held[i]);
  small(fb, 228, 126, in.heldN ? pal::teal : pal::dim, v);
  snprintf(v, sizeof(v), "layout %s  %lu keys  %s", layout == hid::Layout::De ? "DE" : "US", static_cast<unsigned long>(in.keystrokes),
           p.boot ? "boot protocol" : "report protocol");
  small(fb, 6, 144, pal::muted, v);
  if (in.mediaAt && millis() - in.mediaAt < 2500) {
    fb.fillRoundRect(196, 140, 118, 14, 3, kViolet);
    small(fb, 202, 143, pal::night, in.media);
  }
}

inline uint16_t paint(uint8_t i) {
  static const uint16_t c[] = {kBlue, pal::teal, pal::amber, pal::coral, kViolet, pal::ink};
  return c[i % 6];
}

inline void mouse(FrameBuffer &fb, const b8::Input &in, const b8::Progress &p, bool connected, float t) {
  if (!p.mouse) {
    hint(fb, p, connected, "mouse");
    return busy(fb, p, t);
  }
  // The drawing: consecutive points with the button down are joined.
  const uint32_t n = std::min<uint32_t>(in.strokeHead, b8::Input::kStrokes);
  for (uint32_t i = in.strokeHead - n; i < in.strokeHead; i++) {
    const b8::Stroke &s = in.strokes[i % b8::Input::kStrokes];
    if (!s.down) continue;
    fb.fillCircle(s.x, s.y, s.size / 2, paint(s.colour));
    if (i > in.strokeHead - n) {
      const b8::Stroke &prev = in.strokes[(i - 1) % b8::Input::kStrokes];
      if (prev.down)
        for (int w = -(s.size / 2); w <= s.size / 2; w++) fb.drawLine(prev.x, prev.y + w, s.x, s.y + w, paint(s.colour));
    }
  }
  // Trail: where the cursor was, fading.
  const uint32_t tn = std::min<uint32_t>(in.trailHead, b8::Input::kTrail);
  for (uint32_t i = in.trailHead - tn; i < in.trailHead; i++) {
    const uint8_t age = uint8_t((in.trailHead - i) * 255 / (b8::Input::kTrail + 1));
    fb.fillCircle(in.trailX[i % b8::Input::kTrail], in.trailY[i % b8::Input::kTrail], 1, mix(pal::muted, pal::night, age));
  }
  // A click: a ripple where it happened.
  const uint32_t since = millis() - in.clickAt;
  if (in.clickAt && since < 500) fb.drawCircle(in.clickX, in.clickY, 4 + since / 20, mix(pal::ink, pal::night, uint8_t(since / 2)));
  // The cursor: an arrow.
  const int x = in.mx, y = in.my;
  fb.fillTriangle(x, y, x, y + 12, x + 8, y + 9, pal::ink);
  fb.drawLine(x + 3, y + 9, x + 6, y + 15, pal::ink);
  // Palette and brush.
  for (int i = 0; i < 6; i++) {
    fb.fillRect(6 + i * 12, 146, 10, 8, paint(i));
    if (i == in.colour) fb.drawRect(5 + i * 12, 145, 12, 10, pal::ink);
  }
  fb.fillCircle(90, 150, in.brush / 2 + 1, paint(in.colour));
  char v[48];
  snprintf(v, sizeof(v), "L draw  R colour  M clear  wheel %u", in.brush);
  small(fb, 104, 147, pal::muted, v);
}

// The colour of byte b in a report: which Report Map field starts in it.
inline uint16_t byteColour(const b8::HidHost &h, const b8::RawReport &r, int b) {
  if (r.kind == b8::ReportKind::BootKeyboard) return b == 0 ? pal::amber : b == 1 ? pal::dim : pal::teal;
  if (r.kind == b8::ReportKind::BootMouse) return b == 0 ? pal::amber : b == 1 ? kBlue : b == 2 ? kViolet : pal::coral;
  for (int i = 0; i < h.map.count; i++) {
    const hid::Field &f = h.map.fields[i];
    if (f.reportId != r.id) continue;
    const int from = f.bitOffset, to = f.bitOffset + f.size * f.count;
    if (b * 8 + 7 < from || b * 8 >= to) continue;
    if (f.constant) return pal::dim;
    if (f.page == hid::kPageKeyboard) return f.variable ? pal::amber : pal::teal;
    if (f.page == hid::kPageButton) return pal::amber;
    if (f.page == hid::kPageDesktop) return f.usage == hid::kUsageX ? kBlue : f.usage == hid::kUsageY ? kViolet : pal::coral;
    if (f.page == hid::kPageConsumer) return pal::teal;
    return pal::muted;
  }
  return pal::muted;
}

inline void reports(FrameBuffer &fb, const b8::Input &in, const b8::HidHost &h, const b8::Progress &p, bool connected, float t) {
  if (p.phase != b8::Phase::Hosting) {
    hint(fb, p, connected, "keyboard or mouse");
    return busy(fb, p, t);
  }
  char v[48];
  snprintf(v, sizeof(v), "Report Map %d B, %d fields%s", p.mapBytes, p.fields, p.boot ? " (boot: fixed)" : "");
  small(fb, 6, 34, pal::muted, v);
  const uint32_t n = std::min<uint32_t>(in.reportHead, b8::Input::kReports);
  static const char *const kinds[] = {"kbd", "mouse", "media", "other", "bootK", "bootM"};
  for (uint32_t k = 0; k < n; k++) {
    const b8::RawReport &r = in.reports[(in.reportHead - n + k) % b8::Input::kReports];
    const int y = 46 + k * 19;
    snprintf(v, sizeof(v), "#%u %s", r.id, kinds[static_cast<int>(r.kind)]);
    small(fb, 6, y, pal::ink, v);
    for (int b = 0; b < r.len && b < 12; b++) {
      snprintf(v, sizeof(v), "%02X", r.data[b]);
      small(fb, 66 + b * 16, y, byteColour(h, r, b), v);
    }
    small(fb, 66, y + 9, pal::muted, r.decoded);
  }
  if (n == 0) small(fb, 6, 60, pal::muted, "Press a key or move the mouse.");
}

}  // namespace b8v
