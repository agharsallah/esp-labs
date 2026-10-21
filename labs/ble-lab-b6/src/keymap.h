// From a character to the key a keyboard would press. No Arduino here, so
// labs/ble-lab-b6/test/host/ble_keymap_test.cpp checks it on the Mac.
//
// A USB or Bluetooth keyboard never sends letters. It sends key numbers ("usages" from the HID
// Keyboard page): 0x04 is the key where a US keyboard prints A, 0x1C the key where it prints Y.
// The computer turns that number into a character with ITS layout setting. So on a Mac set to
// German, key 0x1C types z: the German keyboard has z where the US one has y.
//
// That's why the board needs to know your layout to type "yes": `layout de` picks the German
// table. Characters a layout can only reach with AltGr/Option (@ [ ] { } \ | ~ on German) differ
// between Windows and macOS, so the German table skips them rather than guess.
#pragma once

#include <ctype.h>
#include <stdint.h>
#include <string.h>

namespace keymap {

// Modifier bits in the first byte of a keyboard report.
constexpr uint8_t kCtrl = 0x01, kShift = 0x02, kAlt = 0x04, kGui = 0x08;  // left side
constexpr uint8_t kRightAlt = 0x40;                                          // AltGr on Windows

enum class Layout : uint8_t { US, DE };

inline const char *layoutName(Layout l) { return l == Layout::US ? "US (QWERTY)" : "German (QWERTZ)"; }

// One key press: the modifiers held and the key. key 0 = this layout can't type it.
struct Stroke {
  uint8_t mod = 0;
  uint8_t key = 0;
};

namespace key {
constexpr uint8_t kA = 0x04, kZ = 0x1D, k1 = 0x1E, k0 = 0x27;
constexpr uint8_t kEnter = 0x28, kEsc = 0x29, kBackspace = 0x2A, kTab = 0x2B, kSpace = 0x2C;
constexpr uint8_t kMinus = 0x2D, kEqual = 0x2E, kLeftBracket = 0x2F, kRightBracket = 0x30, kBackslash = 0x31;
constexpr uint8_t kSemicolon = 0x33, kQuote = 0x34, kGrave = 0x35, kComma = 0x36, kDot = 0x37, kSlash = 0x38;
constexpr uint8_t kCapsLock = 0x39, kF1 = 0x3A, kF12 = 0x45;
constexpr uint8_t kHome = 0x4A, kPageUp = 0x4B, kDelete = 0x4C, kEnd = 0x4D, kPageDown = 0x4E;
constexpr uint8_t kRight = 0x4F, kLeft = 0x50, kDown = 0x51, kUp = 0x52;
constexpr uint8_t kNonUsBackslash = 0x64;  // the extra key next to left Shift on ISO keyboards: < > on German
}  // namespace key

// ---- US: what the HID usage tables are named after ---------------------------------------------

inline Stroke usChar(char c) {
  using namespace key;
  if (c >= 'a' && c <= 'z') return {0, uint8_t(kA + (c - 'a'))};
  if (c >= 'A' && c <= 'Z') return {kShift, uint8_t(kA + (c - 'A'))};
  if (c >= '1' && c <= '9') return {0, uint8_t(k1 + (c - '1'))};
  switch (c) {
    case '0': return {0, k0};
    case '\n': return {0, kEnter};
    case '\t': return {0, kTab};
    case ' ': return {0, kSpace};
    // The shifted digits, as printed on a US keyboard: !@#$%^&*()
    case '!': return {kShift, uint8_t(k1 + 0)};
    case '@': return {kShift, uint8_t(k1 + 1)};
    case '#': return {kShift, uint8_t(k1 + 2)};
    case '$': return {kShift, uint8_t(k1 + 3)};
    case '%': return {kShift, uint8_t(k1 + 4)};
    case '^': return {kShift, uint8_t(k1 + 5)};
    case '&': return {kShift, uint8_t(k1 + 6)};
    case '*': return {kShift, uint8_t(k1 + 7)};
    case '(': return {kShift, uint8_t(k1 + 8)};
    case ')': return {kShift, k0};
    case '-': return {0, kMinus};
    case '_': return {kShift, kMinus};
    case '=': return {0, kEqual};
    case '+': return {kShift, kEqual};
    case '[': return {0, kLeftBracket};
    case '{': return {kShift, kLeftBracket};
    case ']': return {0, kRightBracket};
    case '}': return {kShift, kRightBracket};
    case '\\': return {0, kBackslash};
    case '|': return {kShift, kBackslash};
    case ';': return {0, kSemicolon};
    case ':': return {kShift, kSemicolon};
    case '\'': return {0, kQuote};
    case '"': return {kShift, kQuote};
    case '`': return {0, kGrave};
    case '~': return {kShift, kGrave};
    case ',': return {0, kComma};
    case '<': return {kShift, kComma};
    case '.': return {0, kDot};
    case '>': return {kShift, kDot};
    case '/': return {0, kSlash};
    case '?': return {kShift, kSlash};
    default: return {};
  }
}

// ---- German: the same physical keys, other characters ------------------------------------------

inline Stroke deChar(char c) {
  using namespace key;
  // y and z swap places; every other letter stays.
  if (c == 'y') return {0, kZ};
  if (c == 'z') return {0, uint8_t(kA + ('y' - 'a'))};
  if (c == 'Y') return {kShift, kZ};
  if (c == 'Z') return {kShift, uint8_t(kA + ('y' - 'a'))};
  if (isalpha(static_cast<unsigned char>(c)) || (c >= '0' && c <= '9') || c == '\n' || c == '\t' || c == ' ') return usChar(c);
  switch (c) {
    // Shift + digit: !"§$%&/()=
    case '!': return {kShift, uint8_t(k1 + 0)};
    case '"': return {kShift, uint8_t(k1 + 1)};
    case '$': return {kShift, uint8_t(k1 + 3)};
    case '%': return {kShift, uint8_t(k1 + 4)};
    case '&': return {kShift, uint8_t(k1 + 5)};
    case '/': return {kShift, uint8_t(k1 + 6)};
    case '(': return {kShift, uint8_t(k1 + 7)};
    case ')': return {kShift, uint8_t(k1 + 8)};
    case '=': return {kShift, k0};
    case '?': return {kShift, kMinus};          // the ß key
    case '+': return {0, kRightBracket};        // where US has ]
    case '*': return {kShift, kRightBracket};
    case '#': return {0, kBackslash};           // the key left of Enter
    case '\'': return {kShift, kBackslash};
    case ',': return {0, kComma};
    case ';': return {kShift, kComma};
    case '.': return {0, kDot};
    case ':': return {kShift, kDot};
    case '-': return {0, kSlash};               // where US has /
    case '_': return {kShift, kSlash};
    case '<': return {0, kNonUsBackslash};
    case '>': return {kShift, kNonUsBackslash};
    // @ [ ] { } \ | ~ need AltGr (Windows) or Option (Mac) on different keys: skipped.
    // ^ and ` are dead keys (they wait for the next letter): skipped too.
    default: return {};
  }
}

inline Stroke fromChar(char c, Layout l) { return l == Layout::US ? usChar(c) : deChar(c); }

// ---- Named keys and combos: "enter", "f5", "cmd+space", "ctrl+alt+delete" ------------------------

inline bool sameText(const char *a, size_t n, const char *b) { return strlen(b) == n && strncmp(a, b, n) == 0; }

inline bool namedKey(const char *s, size_t n, uint8_t &out) {
  using namespace key;
  struct Name {
    const char *name;
    uint8_t key;
  };
  static const Name names[] = {{"enter", kEnter}, {"return", kEnter}, {"esc", kEsc}, {"escape", kEsc}, {"backspace", kBackspace},
                               {"tab", kTab}, {"space", kSpace}, {"capslock", kCapsLock}, {"home", kHome}, {"pageup", kPageUp},
                               {"delete", kDelete}, {"del", kDelete}, {"end", kEnd}, {"pagedown", kPageDown}, {"right", kRight},
                               {"left", kLeft}, {"down", kDown}, {"up", kUp}};
  for (const Name &nm : names)
    if (sameText(s, n, nm.name)) return out = nm.key, true;
  // f1 ... f12
  if (n >= 2 && n <= 3 && s[0] == 'f' && isdigit(static_cast<unsigned char>(s[1]))) {
    const int f = n == 2 ? s[1] - '0' : (s[1] - '0') * 10 + (s[2] - '0');
    if (n == 3 && !isdigit(static_cast<unsigned char>(s[2]))) return false;
    if (f >= 1 && f <= 12) return out = uint8_t(kF1 + f - 1), true;
  }
  // One letter or digit: the key in its US position (with cmd/ctrl, shortcuts follow the key).
  if (n == 1 && (isalnum(static_cast<unsigned char>(s[0])))) {
    const Stroke st = usChar(char(tolower(static_cast<unsigned char>(s[0]))));
    return out = st.key, st.key != 0;
  }
  return false;
}

inline bool modifierName(const char *s, size_t n, uint8_t &mod) {
  if (sameText(s, n, "cmd") || sameText(s, n, "gui") || sameText(s, n, "win") || sameText(s, n, "super")) return mod |= kGui, true;
  if (sameText(s, n, "ctrl") || sameText(s, n, "control")) return mod |= kCtrl, true;
  if (sameText(s, n, "alt") || sameText(s, n, "option") || sameText(s, n, "opt")) return mod |= kAlt, true;
  if (sameText(s, n, "shift")) return mod |= kShift, true;
  return false;
}

// "cmd+shift+4" -> {Gui|Shift, key 4}. Lower-case input. False if any part is unknown.
inline bool fromName(const char *text, Stroke &out) {
  Stroke st;
  const char *p = text;
  while (*p) {
    const char *plus = strchr(p, '+');
    const size_t n = plus ? size_t(plus - p) : strlen(p);
    if (n == 0) return false;
    if (plus) {
      if (plus[1] == 0 || !modifierName(p, n, st.mod)) return false;  // "cmd+" with nothing after
      p = plus + 1;
    } else {
      // The last part may be a modifier on its own ("shift"): press just that.
      if (!namedKey(p, n, st.key) && !modifierName(p, n, st.mod)) return false;
      p += n;
    }
  }
  if (st.key == 0 && st.mod == 0) return false;
  out = st;
  return true;
}

}  // namespace keymap
