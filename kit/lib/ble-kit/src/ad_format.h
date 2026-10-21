// The advertising packet, byte by byte. No Arduino and no radio here, so kit/test/host/ble_ad_test.cpp
// checks it on the Mac.
//
// An advertisement is at most 31 bytes (legacy advertising) made of AD structures:
//
//   [len][type][data ...] [len][type][data ...] ...
//
// len counts the type byte plus the data. 02 01 06 = 2 bytes follow, type 01 (Flags), value 06
// (LE General Discoverable + BR/EDR not supported). A scan response is a second packet of 31
// bytes in the same format, sent only when a scanner asks for it (an active scan).
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ad {

// ---- Splitting a packet ------------------------------------------------------------------------

struct Field {
  uint8_t type = 0;
  uint8_t offset = 0;  // where the data starts (after len and type)
  uint8_t len = 0;     // data bytes only
};

struct Parsed {
  Field fields[16];
  int count = 0;
  bool malformed = false;  // a length that runs past the end of the packet
};

inline Parsed split(const uint8_t *p, size_t n) {
  Parsed out;
  size_t i = 0;
  while (i < n && out.count < 16) {
    const uint8_t len = p[i];
    if (len == 0) break;  // padding: the rest of the packet is empty
    if (i + 1 + len > n) {
      out.malformed = true;
      break;
    }
    Field &f = out.fields[out.count++];
    f.type = p[i + 1];
    f.offset = uint8_t(i + 2);
    f.len = uint8_t(len - 1);
    i += 1 + len;
  }
  return out;
}

inline const Field *find(const Parsed &ps, uint8_t type) {
  for (int i = 0; i < ps.count; i++)
    if (ps.fields[i].type == type) return &ps.fields[i];
  return nullptr;
}

// ---- Names -----------------------------------------------------------------------------------

namespace type {
constexpr uint8_t kFlags = 0x01, kUuid16Some = 0x02, kUuid16All = 0x03, kUuid128Some = 0x06, kUuid128All = 0x07;
constexpr uint8_t kShortName = 0x08, kName = 0x09, kTxPower = 0x0A, kConnInterval = 0x12;
constexpr uint8_t kServiceData16 = 0x16, kAppearance = 0x19, kAdvInterval = 0x1A, kUri = 0x24;
constexpr uint8_t kManufacturer = 0xFF;
}  // namespace type

inline const char *typeName(uint8_t t) {
  switch (t) {
    case 0x01: return "Flags";
    case 0x02: return "16-bit services (some)";
    case 0x03: return "16-bit services";
    case 0x04: return "32-bit services (some)";
    case 0x05: return "32-bit services";
    case 0x06: return "128-bit services (some)";
    case 0x07: return "128-bit services";
    case 0x08: return "Short name";
    case 0x09: return "Name";
    case 0x0A: return "TX power";
    case 0x0D: return "Class of device";
    case 0x10: return "Device ID";
    case 0x12: return "Wanted conn. interval";
    case 0x14: return "Solicits 16-bit";
    case 0x15: return "Solicits 128-bit";
    case 0x16: return "Service data";
    case 0x17: return "Target address";
    case 0x19: return "Appearance";
    case 0x1A: return "Adv. interval";
    case 0x1B: return "LE address";
    case 0x1C: return "LE role";
    case 0x20: return "Service data 32";
    case 0x21: return "Service data 128";
    case 0x24: return "URI";
    case 0x2C: return "BIG info";
    case 0x2D: return "Broadcast code";
    case 0xFF: return "Manufacturer data";
    default: return "Other";
  }
}

// A few of the Bluetooth SIG's company identifiers: the first two bytes (little-endian) of the
// manufacturer data. 0xFFFF is reserved for tests: our own B2 beacon uses it.
inline const char *companyName(uint16_t id) {
  switch (id) {
    case 0x0006: return "Microsoft";
    case 0x004C: return "Apple";
    case 0x0057: return "Harman (JBL)";
    case 0x0059: return "Nordic";
    case 0x0075: return "Samsung";
    case 0x0087: return "Garmin";
    case 0x009E: return "Bose";
    case 0x00E0: return "Google";
    case 0x012D: return "Sony";
    case 0x0171: return "Amazon";
    case 0x01DA: return "Logitech";
    case 0x027D: return "Huawei";
    case 0x02E5: return "Espressif";
    case 0x038F: return "Xiaomi";
    case 0xFFFF: return "test (bench)";
    default: return nullptr;
  }
}

// Standard 16-bit service UUIDs you meet in the wild (and in our labs).
inline const char *service16Name(uint16_t uuid) {
  switch (uuid) {
    case 0x1800: return "Generic Access";
    case 0x1801: return "Generic Attribute";
    case 0x180A: return "Device Information";
    case 0x180D: return "Heart Rate";
    case 0x180F: return "Battery";
    case 0x1812: return "HID (keyboard/mouse)";
    case 0x1816: return "Cycling Speed";
    case 0x181A: return "Environmental Sensing";
    case 0x1826: return "Fitness Machine";
    case 0xFD6F: return "Exposure Notification";
    case 0xFE2C: return "Google Fast Pair";
    case 0xFE9F: return "Google";
    case 0xFEAA: return "Eddystone";
    default: return nullptr;
  }
}

// Apple's manufacturer data carries a "Continuity" message type in its first byte.
inline const char *appleType(uint8_t t) {
  switch (t) {
    case 0x02: return "iBeacon";
    case 0x05: return "AirDrop";
    case 0x07: return "AirPods pairing";
    case 0x09: return "AirPlay target";
    case 0x0C: return "Handoff";
    case 0x0F: return "Nearby action";
    case 0x10: return "Nearby info";
    case 0x12: return "Find My";
    default: return "Continuity";
  }
}

// ---- What kind of thing is it? -----------------------------------------------------------------

enum class Kind : uint8_t { Unknown, Phone, Computer, Audio, Wearable, Input, Beacon, Tag, Tv, Bench };

inline const char *kindName(Kind k) {
  static const char *const names[] = {"unknown", "phone", "computer", "audio", "wearable", "keyboard/mouse", "beacon", "tag", "TV", "bench"};
  return names[static_cast<int>(k)];
}

// The appearance value's top 10 bits are a category (SIG Assigned Numbers, "Appearance").
inline Kind kindFromAppearance(uint16_t appearance) {
  switch (appearance >> 6) {
    case 0x001: return Kind::Phone;
    case 0x002: return Kind::Computer;
    case 0x003: return Kind::Wearable;  // watch
    case 0x005: return Kind::Tv;        // display
    case 0x008:                          // tag
    case 0x009: return Kind::Tag;        // keyring
    case 0x00F: return Kind::Input;      // HID
    case 0x021:                          // audio sink (speaker)
    case 0x022:                          // audio source
    case 0x025: return Kind::Audio;      // wearable audio (earbuds, headphones)
    default: return Kind::Unknown;
  }
}

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }

// Best guess from the payload alone. Phones rarely say "I am a phone", so this is a heuristic:
// the inspect view always shows the bytes it was based on.
inline Kind guess(const uint8_t *p, size_t n) {
  const Parsed ps = split(p, n);
  if (const Field *a = find(ps, type::kAppearance); a && a->len >= 2) {
    const Kind k = kindFromAppearance(le16(p + a->offset));
    if (k != Kind::Unknown) return k;
  }
  if (const Field *m = find(ps, type::kManufacturer); m && m->len >= 2) {
    const uint8_t *d = p + m->offset;
    const uint16_t company = le16(d);
    if (company == 0xFFFF) return Kind::Bench;
    if (company == 0x004C && m->len >= 3) {
      if (d[2] == 0x02) return Kind::Beacon;
      if (d[2] == 0x07) return Kind::Audio;
      if (d[2] == 0x12) return Kind::Tag;
      if (d[2] == 0x10 || d[2] == 0x0C || d[2] == 0x0F) return Kind::Phone;  // iPhone, iPad or Mac
    }
    if (company == 0x0006) return Kind::Computer;
    if (company == 0x0057 || company == 0x009E) return Kind::Audio;
  }
  for (int i = 0; i < ps.count; i++) {
    const Field &f = ps.fields[i];
    if ((f.type == type::kUuid16All || f.type == type::kUuid16Some || f.type == type::kServiceData16) && f.len >= 2) {
      const uint16_t u = le16(p + f.offset);
      if (u == 0x1812) return Kind::Input;
      if (u == 0xFEAA) return Kind::Beacon;
      if (u == 0x180D) return Kind::Wearable;
      if (u == 0xFE2C) return Kind::Audio;
    }
  }
  return Kind::Unknown;
}

// iBeacon: Apple, type 02, length 15h: 16-byte UUID, major, minor, measured power at 1 m.
inline bool isIBeacon(const uint8_t *p, size_t n, int8_t *measuredPower = nullptr) {
  const Parsed ps = split(p, n);
  const Field *m = find(ps, type::kManufacturer);
  if (!m || m->len != 25) return false;
  const uint8_t *d = p + m->offset;
  if (le16(d) != 0x004C || d[2] != 0x02 || d[3] != 0x15) return false;
  if (measuredPower) *measuredPower = int8_t(d[24]);
  return true;
}

// ---- Addresses -------------------------------------------------------------------------------

// A random address's two top bits say what kind it is.
enum class AddrKind : uint8_t { Public, Static, Resolvable, NonResolvable };

inline AddrKind addrKind(uint8_t addrType, uint8_t mostSignificantByte) {
  if (addrType == 0) return AddrKind::Public;
  switch (mostSignificantByte >> 6) {
    case 0b11: return AddrKind::Static;
    case 0b01: return AddrKind::Resolvable;
    default: return AddrKind::NonResolvable;
  }
}

inline const char *addrKindName(AddrKind k) {
  static const char *const names[] = {"public", "random static", "private (RPA)", "private (NRPA)"};
  return names[static_cast<int>(k)];
}

// ---- Distance from signal strength -------------------------------------------------------------

// Log-distance path loss: rssi = rssiAt1m - 10 * n * log10(d). n is 2 in free space, 2.5-4 indoors
// with walls and bodies. A rough guess: the same phone at the same spot moves by ±5 dB.
inline float distanceMeters(int rssi, int rssiAt1m = -59, float n = 2.5f) {
  return powf(10.0f, float(rssiAt1m - rssi) / (10.0f * n));
}

// ---- Eddystone-URL ----------------------------------------------------------------------------

// Eddystone-URL squeezes a link into service data FEAA: a frame type 10, TX power, a scheme byte
// (0 http://www. 1 https://www. 2 http:// 3 https://) and the rest, with common endings as one
// byte (00 ".com/", 07 ".com" ...). Returns the bytes written, 0 if it doesn't fit (17 max).
inline size_t encodeEddystoneUrl(const char *url, int8_t txPower, uint8_t *out, size_t cap) {
  static const char *const schemes[] = {"http://www.", "https://www.", "http://", "https://"};
  static const char *const endings[] = {".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/", ".com", ".org", ".edu", ".net", ".info", ".biz", ".gov"};
  if (cap < 3) return 0;
  int scheme = -1;
  for (int i = 0; i < 4 && scheme < 0; i++)
    if (strncmp(url, schemes[i], strlen(schemes[i])) == 0) scheme = i;
  if (scheme < 0) return 0;
  size_t n = 0;
  out[n++] = 0x10;
  out[n++] = uint8_t(txPower);
  out[n++] = uint8_t(scheme);
  const char *s = url + strlen(schemes[scheme]);
  while (*s) {
    int ending = -1;
    for (int i = 0; i < 14 && ending < 0; i++)
      if (strncmp(s, endings[i], strlen(endings[i])) == 0) ending = i;
    if (n >= cap || n >= 20) return 0;  // 3 header bytes + 17 of URL
    if (ending >= 0) {
      out[n++] = uint8_t(ending);
      s += strlen(endings[ending]);
    } else {
      if (uint8_t(*s) <= 0x20 || uint8_t(*s) >= 0x7F) return 0;
      out[n++] = uint8_t(*s++);
    }
  }
  return n;
}

}  // namespace ad
