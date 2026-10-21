// Reading a keyboard's or a mouse's own description of its reports. No Arduino and no radio here,
// so kit/test/host/ble_hid_test.cpp checks it on the Mac with real descriptors.
//
// A HID device doesn't send "the user typed h". It sends a few raw bytes (a report), and it hands
// over a Report Map (characteristic 2A4B) once, which says what every bit of those bytes means:
//
//   05 01        Usage Page (Generic Desktop)
//   09 06        Usage (Keyboard)
//   A1 01        Collection (Application)
//   05 07          Usage Page (Keyboard)
//   19 E0 29 E7    Usage Min E0, Usage Max E7     the 8 modifier keys
//   15 00 25 01    Logical Min 0, Max 1
//   75 01 95 08    Report Size 1 bit, Count 8
//   81 02          Input (Data, Variable)         -> 8 bits, one per modifier
//   ...
//
// Every item is one prefix byte plus 0, 1, 2 or 4 data bytes. The prefix holds the size (bits 0-1,
// where 3 means 4 bytes), the type (bits 2-3: main, global, local) and the tag (bits 4-7). Global
// items (page, sizes, report ID) stay set until changed; local items (usages) are used up by the
// next main item. Every Input item adds fields to the report it belongs to, bit after bit.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace hid {

// One input field: `count` values of `size` bits each, from `bitOffset` in the report (after the
// report ID byte, which BLE never sends inside the value: it lives in descriptor 2908).
struct Field {
  uint8_t reportId = 0;
  uint16_t bitOffset = 0;
  uint8_t size = 0;
  uint8_t count = 1;
  uint16_t page = 0;
  uint16_t usage = 0;      // a variable field: its usage. An array: the lowest usage it can report.
  uint16_t usageMax = 0;   // an array: the highest
  bool constant = false;   // padding
  bool variable = true;    // false = an array of key codes
  bool relative = false;   // mouse movement: since the last report
  int32_t logMin = 0, logMax = 0;
  uint16_t app = 0;        // the application collection it sits in: page << 8 | usage (0x0106 keyboard)
};

struct Map {
  static constexpr int kMaxFields = 48;
  Field fields[kMaxFields];
  int count = 0;
  bool hasReportIds = false;
  bool truncated = false;   // more fields than we keep
  bool malformed = false;   // an item ran past the end
  uint16_t reportBits[8] = {};  // input bits per report: index = position in reportIds
  uint8_t reportIds[8] = {};
  int reports = 0;
};

constexpr uint16_t kPageDesktop = 0x01, kPageKeyboard = 0x07, kPageButton = 0x09, kPageConsumer = 0x0C;
constexpr uint16_t kUsageX = 0x30, kUsageY = 0x31, kUsageWheel = 0x38;
constexpr uint16_t kAppKeyboard = 0x0106, kAppMouse = 0x0102, kAppConsumer = 0x0C01;

inline int32_t itemValue(const uint8_t *d, int n, bool isSigned) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) v |= uint32_t(d[i]) << (8 * i);
  if (!isSigned || n == 0 || n == 4) return int32_t(v);
  const uint32_t sign = 1u << (8 * n - 1);
  return (v & sign) ? int32_t(v | ~(sign * 2 - 1)) : int32_t(v);
}

inline Map parse(const uint8_t *p, size_t n) {
  Map m;
  // Global state (with one level of Push/Pop, enough for real devices).
  struct Globals {
    uint16_t page = 0;
    int32_t logMin = 0, logMax = 0;
    uint8_t size = 0, count = 0, reportId = 0;
  } g, saved;
  // Local state: cleared after every main item.
  uint16_t usages[16];
  int usageCount = 0;
  uint16_t usageMin = 0, usageMax = 0;
  bool haveRange = false;
  uint16_t app = 0;
  int depth = 0;
  size_t i = 0;
  auto bitsFor = [&m](uint8_t id) -> uint16_t & {
    for (int r = 0; r < m.reports; r++)
      if (m.reportIds[r] == id) return m.reportBits[r];
    if (m.reports < 8) {
      m.reportIds[m.reports] = id;
      return m.reportBits[m.reports++];
    }
    return m.reportBits[7];
  };
  while (i < n) {
    const uint8_t prefix = p[i];
    if (prefix == 0xFE) {  // long item: skip it
      if (i + 2 >= n) return m.malformed = true, m;
      i += 3 + p[i + 1];
      continue;
    }
    const int size = (prefix & 3) == 3 ? 4 : (prefix & 3);
    const int type = (prefix >> 2) & 3, tag = prefix >> 4;
    if (i + 1 + size > n) return m.malformed = true, m;
    const uint8_t *d = p + i + 1;
    const int32_t u = itemValue(d, size, false), s = itemValue(d, size, true);
    i += 1 + size;
    if (type == 1) {  // global
      switch (tag) {
        case 0x0: g.page = uint16_t(u); break;
        case 0x1: g.logMin = s; break;
        case 0x2: g.logMax = g.logMin < 0 ? s : u; break;  // unsigned unless the minimum is negative
        case 0x7: g.size = uint8_t(u); break;
        case 0x8: g.reportId = uint8_t(u), m.hasReportIds = true; break;
        case 0x9: g.count = uint8_t(u); break;
        case 0xA: saved = g; break;
        case 0xB: g = saved; break;
      }
      continue;
    }
    if (type == 2) {  // local
      // A 4-byte usage carries its own page in the top 16 bits; we keep the low 16.
      if (tag == 0x0 && usageCount < 16) usages[usageCount++] = uint16_t(u);
      if (tag == 0x1) usageMin = uint16_t(u), haveRange = true;
      if (tag == 0x2) usageMax = uint16_t(u), haveRange = true;
      continue;
    }
    if (type != 0) continue;
    // Main items.
    if (tag == 0xA) {  // Collection: an application collection names what follows
      if (depth == 0 && (u == 1) && usageCount) app = uint16_t((g.page << 8) | usages[0]);
      depth++;
    } else if (tag == 0xC) {
      if (depth) depth--;
    } else if (tag == 0x8) {  // Input
      const bool constant = u & 1, variable = u & 2, relative = u & 4;
      uint16_t &bits = bitsFor(g.reportId);
      if (variable && !constant) {
        // One field per value, each with its own usage.
        for (int k = 0; k < g.count; k++) {
          if (m.count == Map::kMaxFields) {
            m.truncated = true;
            break;
          }
          Field &f = m.fields[m.count++];
          f.reportId = g.reportId;
          f.bitOffset = uint16_t(bits + k * g.size);
          f.size = g.size;
          f.count = 1;
          f.page = g.page;
          f.usage = k < usageCount ? usages[k] : haveRange ? uint16_t(usageMin + k) : usageCount ? usages[usageCount - 1] : 0;
          f.variable = true;
          f.relative = relative;
          f.logMin = g.logMin, f.logMax = g.logMax;
          f.app = app;
        }
      } else if (m.count < Map::kMaxFields) {
        // An array (or padding): one field holding `count` slots.
        Field &f = m.fields[m.count++];
        f.reportId = g.reportId;
        f.bitOffset = bits;
        f.size = g.size;
        f.count = g.count;
        f.page = g.page;
        f.usage = haveRange ? usageMin : usageCount ? usages[0] : 0;
        f.usageMax = haveRange ? usageMax : f.usage;
        f.constant = constant;
        f.variable = false;
        f.relative = relative;
        f.logMin = g.logMin, f.logMax = g.logMax;
        f.app = app;
      } else {
        m.truncated = true;
      }
      bits = uint16_t(bits + g.size * g.count);
    }
    // Output (9) and Feature (B) describe bytes the host sends: they don't move input offsets.
    usageCount = 0;
    haveRange = false;
    usageMin = usageMax = 0;
  }
  return m;
}

// `size` bits from `bitOffset`, least significant bit first (HID's bit order).
inline int32_t extract(const uint8_t *data, size_t len, uint16_t bitOffset, uint8_t size, bool isSigned) {
  uint32_t v = 0;
  for (int b = 0; b < size && b < 32; b++) {
    const size_t bit = bitOffset + b;
    if (bit / 8 >= len) break;
    if (data[bit / 8] & (1 << (bit % 8))) v |= 1u << b;
  }
  if (isSigned && size > 0 && size < 32 && (v & (1u << (size - 1)))) v |= ~((1u << size) - 1);
  return int32_t(v);
}

inline int32_t value(const Field &f, const uint8_t *data, size_t len, int slot = 0) {
  return extract(data, len, uint16_t(f.bitOffset + slot * f.size), f.size, f.logMin < 0);
}

// ---- The two layouts we use ---------------------------------------------------------------------

struct Keyboard {
  bool found = false;
  uint8_t reportId = 0;
  int modifiers = -1;  // the field index of modifier bit E0 (the other seven follow it)
  int keys = -1;       // the field index of the key code array
};

struct Mouse {
  bool found = false;
  uint8_t reportId = 0;
  int buttons = -1;  // the field index of button 1 (the others follow)
  int buttonCount = 0;
  int x = -1, y = -1, wheel = -1;
};

inline Keyboard findKeyboard(const Map &m) {
  Keyboard k;
  for (int i = 0; i < m.count; i++) {
    const Field &f = m.fields[i];
    if (f.page != kPageKeyboard) continue;
    if (f.variable && f.usage == 0xE0 && k.modifiers < 0) k.modifiers = i, k.reportId = f.reportId;
    if (!f.variable && !f.constant && f.size == 8 && k.keys < 0) k.keys = i, k.reportId = f.reportId;
  }
  k.found = k.keys >= 0;
  return k;
}

inline Mouse findMouse(const Map &m) {
  Mouse ms;
  for (int i = 0; i < m.count; i++) {
    const Field &f = m.fields[i];
    if (f.page == kPageButton && f.variable && ms.buttons < 0) ms.buttons = i, ms.reportId = f.reportId;
    if (f.page == kPageButton && f.variable && f.reportId == ms.reportId) ms.buttonCount++;
    if (f.page == kPageDesktop && f.usage == kUsageX && ms.x < 0) ms.x = i, ms.reportId = f.reportId;
    if (f.page == kPageDesktop && f.usage == kUsageY && ms.y < 0) ms.y = i;
    if (f.page == kPageDesktop && f.usage == kUsageWheel && ms.wheel < 0) ms.wheel = i;
  }
  ms.found = ms.x >= 0 && ms.y >= 0;
  return ms;
}

// The 8 key codes a keyboard report can hold (6 in boot format), decoded.
struct KeyState {
  uint8_t mods = 0;  // bit 0 L-Ctrl, 1 L-Shift, 2 L-Alt, 3 L-GUI, 4 R-Ctrl, 5 R-Shift, 6 R-Alt (AltGr), 7 R-GUI
  uint8_t keys[8] = {};
  int n = 0;
};

inline KeyState keyboardState(const Map &m, const Keyboard &k, const uint8_t *data, size_t len) {
  KeyState s;
  if (k.modifiers >= 0)
    for (int b = 0; b < 8 && k.modifiers + b < m.count; b++) {
      const Field &f = m.fields[k.modifiers + b];
      if (f.page == kPageKeyboard && f.usage >= 0xE0 && f.usage <= 0xE7 && value(f, data, len)) s.mods |= uint8_t(1 << (f.usage - 0xE0));
    }
  if (k.keys >= 0) {
    const Field &f = m.fields[k.keys];
    for (int i = 0; i < f.count && s.n < 8; i++) {
      const int32_t code = value(f, data, len, i);
      if (code > 3) s.keys[s.n++] = uint8_t(code);  // 0 none, 1-3 error codes (too many keys)
    }
  }
  return s;
}

// Boot protocol: a fixed 8-byte keyboard report: modifiers, reserved, 6 key codes.
inline KeyState bootKeyboard(const uint8_t *data, size_t len) {
  KeyState s;
  if (len < 3) return s;
  s.mods = data[0];
  for (size_t i = 2; i < len && i < 8; i++)
    if (data[i] > 3) s.keys[s.n++] = data[i];
  return s;
}

// The keys in `now` that were not down in `before`: what was just pressed.
inline int newlyPressed(const KeyState &before, const KeyState &now, uint8_t *out) {
  int n = 0;
  for (int i = 0; i < now.n; i++) {
    bool held = false;
    for (int j = 0; j < before.n; j++) held |= before.keys[j] == now.keys[i];
    if (!held) out[n++] = now.keys[i];
  }
  return n;
}

// ---- Key code -> character ----------------------------------------------------------------------
// A key code says WHICH KEY, not which letter: the host's layout decides. The same key 1C is "y" on
// a US keyboard and "z" on a German one. The result is a CP437 byte (the screen's built-in font):
// 0x81 ü, 0x84 ä, 0x94 ö, 0x9A Ü, 0x8E Ä, 0x99 Ö, 0xE1 ß, 0x15 §, 0xF8 °. 0 = no character.

enum class Layout : uint8_t { Us, De };

namespace key {
constexpr uint8_t kEnter = 0x28, kEscape = 0x29, kBackspace = 0x2A, kTab = 0x2B, kRight = 0x4F, kLeft = 0x50, kDown = 0x51, kUp = 0x52;
}

inline uint8_t toChar(uint8_t code, uint8_t mods, Layout layout) {
  const bool shift = mods & 0x22, altGr = mods & 0x40;
  const bool de = layout == Layout::De;
  if (code >= 0x04 && code <= 0x1D) {
    uint8_t c = uint8_t('a' + code - 0x04);
    if (de && c == 'y') c = 'z';
    else if (de && c == 'z') c = 'y';
    if (de && altGr) return c == 'q' ? '@' : 0;
    return shift ? uint8_t(c - 32) : c;
  }
  if (code == 0x2C) return ' ';
  if (code == key::kTab) return '\t';
  if (code == key::kEnter || code == 0x58) return '\n';
  if (code >= 0x1E && code <= 0x27) {
    const int digit = code == 0x27 ? 0 : code - 0x1D;
    if (!shift && !(de && altGr)) return uint8_t('0' + digit);
    if (de && altGr) return digit == 7 ? '{' : digit == 8 ? '[' : digit == 9 ? ']' : digit == 0 ? '}' : 0;
    static const char usShift[] = ")!@#$%^&*(";
    static const uint8_t deShift[] = {'=', '!', '"', 0x15, '$', '%', '&', '/', '(', ')'};
    return de ? deShift[digit] : uint8_t(usShift[digit]);
  }
  // Punctuation keys 2D-38, plus 64 (the extra ISO key left of Y/Z).
  struct Pair {
    uint8_t code, us, usShift, de, deShift, deAltGr;
  };
  static const Pair kPunct[] = {
      {0x2D, '-', '_', 0xE1, '?', '\\'}, {0x2E, '=', '+', 0, 0, 0},  // DE: ´ and ` are dead keys
      {0x2F, '[', '{', 0x81, 0x9A, 0},   {0x30, ']', '}', '+', '*', '~'},
      {0x31, '\\', '|', '#', '\'', 0},   {0x32, '#', '~', '#', '\'', 0},  // 32 = the ISO # key
      {0x33, ';', ':', 0x94, 0x99, 0},   {0x34, '\'', '"', 0x84, 0x8E, 0},
      {0x35, '`', '~', '^', 0xF8, 0},    {0x36, ',', '<', ',', ';', 0},
      {0x37, '.', '>', '.', ':', 0},     {0x38, '/', '?', '-', '_', 0},
      {0x64, '\\', '|', '<', '>', '|'},
  };
  for (const Pair &p : kPunct)
    if (p.code == code) {
      if (de) return altGr ? p.deAltGr : shift ? p.deShift : p.de;
      return shift ? p.usShift : p.us;
    }
  return 0;
}

// Names for the consumer keys a keyboard's media row sends (page 0C).
inline const char *consumerName(uint16_t usage) {
  switch (usage) {
    case 0xB5: return "next track";
    case 0xB6: return "previous track";
    case 0xB7: return "stop";
    case 0xCD: return "play/pause";
    case 0xE2: return "mute";
    case 0xE9: return "volume up";
    case 0xEA: return "volume down";
    case 0x6F: return "brightness up";
    case 0x70: return "brightness down";
    default: return nullptr;
  }
}

inline const char *modifierName(int bit) {
  static const char *const names[] = {"L-Ctrl", "L-Shift", "L-Alt", "L-GUI", "R-Ctrl", "R-Shift", "AltGr", "R-GUI"};
  return names[bit & 7];
}

}  // namespace hid
