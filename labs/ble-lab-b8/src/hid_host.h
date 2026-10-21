// Turning a keyboard's or mouse's notifications into text and a moving cursor.
//
// After the central task has read the Report Map and subscribed, every key press arrives as a
// notification on one Report characteristic (2A4D). Which report it is comes from that
// characteristic's Report Reference descriptor (2908: report ID + type), not from the bytes: BLE
// leaves the report ID out of the value. The routes table maps characteristic handle -> report.
//
// onReport() runs in the NimBLE task: it only decodes and updates b8::input(), never draws.
#pragma once

#include <NimBLEDevice.h>

#include "ble_link.h"
#include "hid_report.h"
#include "host_state.h"
#include "ble_link.h"

namespace b8 {

struct Route {
  uint16_t handle = 0;
  uint8_t reportId = 0;
  ReportKind kind = ReportKind::Other;
};

class HidHost {
 public:
  static constexpr int kRoutes = 8;

  // Set by the central task BEFORE it subscribes, then only read: no lock needed for these.
  hid::Map map;
  hid::Keyboard keyboard;
  hid::Mouse mouse;
  int consumerField = -1;
  Route routes[kRoutes];
  int routeCount = 0;

  void reset() {
    map = hid::Map{};
    keyboard = hid::Keyboard{};
    mouse = hid::Mouse{};
    consumerField = -1;
    routeCount = 0;
    lastKeys_ = hid::KeyState{};
  }

  void setMap(const uint8_t *bytes, size_t n) {
    map = hid::parse(bytes, n);
    keyboard = hid::findKeyboard(map);
    mouse = hid::findMouse(map);
    consumerField = -1;
    for (int i = 0; i < map.count && consumerField < 0; i++)
      if (map.fields[i].page == hid::kPageConsumer && !map.fields[i].constant) consumerField = i;
  }

  // What a report ID carries, by the Report Map.
  ReportKind kindOf(uint8_t reportId) const {
    if (keyboard.found && keyboard.reportId == reportId) return ReportKind::Keyboard;
    if (mouse.found && mouse.reportId == reportId) return ReportKind::Mouse;
    if (consumerField >= 0 && map.fields[consumerField].reportId == reportId) return ReportKind::Consumer;
    return ReportKind::Other;
  }

  bool addRoute(uint16_t handle, uint8_t reportId, ReportKind kind) {
    if (routeCount == kRoutes) return false;
    routes[routeCount++] = Route{handle, reportId, kind};
    return true;
  }

  void setLayout(hid::Layout l) { layout_ = l; }
  hid::Layout layout() const { return layout_; }

  // NimBLE task: one notification.
  void onReport(uint16_t handle, const uint8_t *data, size_t len) {
    const Route *r = nullptr;
    for (int i = 0; i < routeCount; i++)
      if (routes[i].handle == handle) r = &routes[i];
    if (!r) return;
    ble::countRx(len);
    RawReport raw;
    raw.ms = millis();
    raw.id = r->reportId;
    raw.kind = r->kind;
    raw.len = uint8_t(std::min<size_t>(len, sizeof(raw.data)));
    memcpy(raw.data, data, raw.len);
    switch (r->kind) {
      case ReportKind::Keyboard: keys(hid::keyboardState(map, keyboard, data, len), raw); break;
      case ReportKind::BootKeyboard: keys(hid::bootKeyboard(data, len), raw); break;
      case ReportKind::Mouse: {
        uint8_t buttons = 0;
        for (int b = 0; b < mouse.buttonCount && b < 8; b++)
          if (hid::value(map.fields[mouse.buttons + b], data, len)) buttons |= uint8_t(1 << b);
        const int dx = hid::value(map.fields[mouse.x], data, len), dy = hid::value(map.fields[mouse.y], data, len);
        const int wheel = mouse.wheel >= 0 ? hid::value(map.fields[mouse.wheel], data, len) : 0;
        pointer(buttons, dx, dy, wheel, raw);
        break;
      }
      case ReportKind::BootMouse:
        if (len >= 3) pointer(data[0], int8_t(data[1]), int8_t(data[2]), len >= 4 ? int8_t(data[3]) : 0, raw);
        break;
      case ReportKind::Consumer: {
        const hid::Field &f = map.fields[consumerField];
        const int32_t usage = f.variable ? (hid::value(f, data, len) ? f.usage : 0) : hid::value(f, data, len);
        const char *name = hid::consumerName(uint16_t(usage));
        snprintf(raw.decoded, sizeof(raw.decoded), "media %s", usage ? (name ? name : "key") : "released");
        if (usage) {
          input().edit([&](Input &in) {
            snprintf(in.media, sizeof(in.media), "%s", name ? name : "media key");
            in.mediaAt = millis();
          });
          BLE_NOTE(Rx, "media key: %s (usage %02lX)", name ? name : "?", static_cast<unsigned long>(usage));
        }
        break;
      }
      case ReportKind::Other: snprintf(raw.decoded, sizeof(raw.decoded), "report %u: not decoded", r->reportId); break;
    }
    input().edit([&](Input &in) { in.reports[in.reportHead++ % Input::kReports] = raw; });
  }

 private:
  void keys(const hid::KeyState &now, RawReport &raw) {
    uint8_t fresh[8];
    const int n = hid::newlyPressed(lastKeys_, now, fresh);
    lastKeys_ = now;
    int at = snprintf(raw.decoded, sizeof(raw.decoded), "mods %02X", now.mods);
    for (int i = 0; i < now.n && at < int(sizeof(raw.decoded)) - 4; i++) at += snprintf(raw.decoded + at, sizeof(raw.decoded) - at, " %02X", now.keys[i]);
    const hid::Layout layout = layout_;
    input().edit([&](Input &in) {
      in.mods = now.mods;
      in.heldN = now.n;
      memcpy(in.held, now.keys, sizeof(in.held));
      for (int i = 0; i < n; i++) {
        const uint8_t code = fresh[i];
        in.keystrokes++;
        if (code == hid::key::kBackspace) {
          if (in.len) in.len--;
        } else if (code == hid::key::kEscape) {
          in.len = 0;
        } else if (const uint8_t c = hid::toChar(code, now.mods, layout)) {
          if (in.len == Input::kText) memmove(in.text, in.text + 40, in.len -= 40);  // full: drop the oldest line
          in.text[in.len++] = c == '\t' ? ' ' : c;
        }
      }
    });
  }

  void pointer(uint8_t buttons, int dx, int dy, int wheel, RawReport &raw) {
    snprintf(raw.decoded, sizeof(raw.decoded), "buttons %02X x %+d y %+d wheel %+d", buttons, dx, dy, wheel);
    input().edit([&](Input &in) {
      const uint8_t pressed = buttons & ~in.buttons;
      // Mice count in their own units (often 800+ per inch): halve, so the screen isn't crossed in a flick.
      in.mx = int16_t(std::max(2, std::min(317, in.mx + dx / 2)));
      in.my = int16_t(std::max(32, std::min(155, in.my + dy / 2)));
      if (wheel) in.brush = uint8_t(std::max(1, std::min(9, in.brush + (wheel > 0 ? 1 : -1))));
      if (pressed & 2) in.colour = uint8_t((in.colour + 1) % 6);  // right button: next colour
      if (pressed & 4) in.strokeHead = 0;                         // middle button: clear
      if (pressed) in.clickX = in.mx, in.clickY = in.my, in.clickAt = millis();
      if ((buttons & 1) || (in.buttons & 1)) {
        Stroke &s = in.strokes[in.strokeHead++ % Input::kStrokes];
        s = Stroke{in.mx, in.my, in.brush, in.colour, bool(buttons & 1)};
      }
      in.trailX[in.trailHead % Input::kTrail] = in.mx;
      in.trailY[in.trailHead++ % Input::kTrail] = in.my;
      in.buttons = buttons;
      in.moves++;
    });
  }

  hid::KeyState lastKeys_;
  volatile hid::Layout layout_ = hid::Layout::Us;
};

}  // namespace b8
