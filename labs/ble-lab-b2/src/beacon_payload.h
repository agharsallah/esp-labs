// B2's packets, built byte by byte. No Arduino and no radio here, so labs/ble-lab-b2/test/host/ble_beacon_test.cpp
// checks them on the Mac.
//
// One advertisement is at most 31 bytes of AD structures ([len][type][data...], see ad_format.h).
// Each mode fills that budget differently:
//
//   bench    Flags · Name "Bench B2" · Manufacturer data FFFF + our live values     24 of 31 bytes
//   ibeacon  Flags · Manufacturer data 004C 02 15 + UUID, major, minor, power      30 of 31
//   url      Flags · 16-bit service list FEAA · Service data FEAA + Eddystone-URL   up to 31
//   name     Flags · Name · TX power                                               16 of 31
//
// The scan response (a second 31-byte packet, sent only when an active scanner asks) carries what
// did not fit: the name for ibeacon and url, the TX power and appearance for bench.
#pragma once

#include <stdint.h>
#include <string.h>

#include "ad_format.h"

namespace b2 {

enum class Mode : uint8_t { Bench, IBeacon, Url, Name };

inline const char *modeName(Mode m) {
  static const char *const names[] = {"bench", "ibeacon", "url", "name"};
  return names[static_cast<int>(m)];
}

// One packet being filled. add() refuses a structure that would not fit in 31 bytes.
struct Packet {
  uint8_t b[31] = {};
  uint8_t n = 0;

  bool add(uint8_t type, const void *data, uint8_t len) {
    if (n + 2 + len > 31) return false;
    b[n++] = uint8_t(len + 1);  // the length counts the type byte too
    b[n++] = type;
    memcpy(b + n, data, len);
    n += len;
    return true;
  }
};

// The live values the bench mode broadcasts. Everything little-endian, like the rest of BLE.
struct Live {
  uint16_t presses = 0;
  int16_t tempCenti = 0;  // °C x 100
  uint16_t uptime = 0;    // seconds, wraps after 18 hours
};

constexpr uint16_t kTestCompany = 0xFFFF;  // reserved by the Bluetooth SIG for tests: ours to use
constexpr uint8_t kBenchVersion = 1;
constexpr uint8_t kBenchLen = 9;  // company (2) + version (1) + presses (2) + temp (2) + uptime (2)

inline void benchData(const Live &l, uint8_t out[kBenchLen]) {
  out[0] = kTestCompany & 0xFF;
  out[1] = kTestCompany >> 8;
  out[2] = kBenchVersion;
  out[3] = l.presses & 0xFF;
  out[4] = l.presses >> 8;
  out[5] = uint16_t(l.tempCenti) & 0xFF;
  out[6] = uint16_t(l.tempCenti) >> 8;
  out[7] = l.uptime & 0xFF;
  out[8] = l.uptime >> 8;
}

// The reverse, as a phone app (or B1's inspect view) would read it. False if it isn't ours.
inline bool parseBench(const uint8_t *d, uint8_t len, Live &out) {
  if (len != kBenchLen || ad::le16(d) != kTestCompany || d[2] != kBenchVersion) return false;
  out.presses = ad::le16(d + 3);
  out.tempCenti = int16_t(ad::le16(d + 5));
  out.uptime = ad::le16(d + 7);
  return true;
}

struct IBeacon {
  // The proximity UUID: which "fleet" of beacons this is. Ours is the Bench service's UUID.
  uint8_t uuid[16] = {0xBA, 0x5E, 0x00, 0x00, 0xB1, 0xE5, 0x4A, 0xB1, 0x9E, 0x5C, 0xBE, 0x7C, 0xBE, 0x7C, 0xB0, 0xA7};
  uint16_t major = 1, minor = 2;  // which beacon in the fleet (a shop, a shelf)
  int8_t power = -59;             // the RSSI a phone should measure at 1 m: the distance reference
};

// Apple's layout: 4C 00 (Apple) · 02 (iBeacon) · 15 (21 bytes follow) · UUID · major · minor · power.
// Major and minor are big-endian: Apple's choice, the one exception in an otherwise little-endian packet.
inline void iBeaconData(const IBeacon &ib, uint8_t out[25]) {
  out[0] = 0x4C;
  out[1] = 0x00;
  out[2] = 0x02;
  out[3] = 0x15;
  memcpy(out + 4, ib.uuid, 16);
  out[20] = ib.major >> 8;
  out[21] = ib.major & 0xFF;
  out[22] = ib.minor >> 8;
  out[23] = ib.minor & 0xFF;
  out[24] = uint8_t(ib.power);
}

struct Config {
  Mode mode = Mode::Bench;
  const char *name = "Bench B2";
  Live live;
  IBeacon beacon;
  const char *url = "https://espressif.com";
  int8_t txPower = 9;
  bool scanResponse = true;
};

constexpr uint8_t kFlagsGeneral = 0x06;  // LE General Discoverable + BR/EDR (Classic) not supported
constexpr uint16_t kAppearanceSensor = 0x0540;

// Both packets for a configuration. False (and why) when it doesn't fit, e.g. a URL that's too long.
inline bool build(const Config &c, Packet &adv, Packet &rsp, const char **why = nullptr) {
  adv = Packet{};
  rsp = Packet{};
  const uint8_t flags = kFlagsGeneral;
  adv.add(ad::type::kFlags, &flags, 1);
  const uint8_t nameLen = uint8_t(strlen(c.name));
  switch (c.mode) {
    case Mode::Bench: {
      uint8_t d[kBenchLen];
      benchData(c.live, d);
      adv.add(ad::type::kName, c.name, nameLen);
      adv.add(ad::type::kManufacturer, d, kBenchLen);
      const uint8_t app[2] = {kAppearanceSensor & 0xFF, kAppearanceSensor >> 8};
      rsp.add(ad::type::kTxPower, &c.txPower, 1);
      rsp.add(ad::type::kAppearance, app, 2);
      break;
    }
    case Mode::IBeacon: {
      uint8_t d[25];
      iBeaconData(c.beacon, d);
      adv.add(ad::type::kManufacturer, d, 25);
      rsp.add(ad::type::kName, c.name, nameLen);
      break;
    }
    case Mode::Url: {
      // Service data = the 16-bit UUID FEAA, then the Eddystone frame.
      uint8_t d[2 + 20] = {0xAA, 0xFE};
      // Eddystone wants the power at 0 m: by convention the 1 m reference + 41 dB.
      const size_t n = ad::encodeEddystoneUrl(c.url, int8_t(c.beacon.power + 41), d + 2, 20);
      if (n == 0) {
        if (why) *why = "the URL doesn't fit: http(s)://, then at most 17 bytes (.com/ .org/ ... count as 1)";
        return false;
      }
      const uint8_t list[2] = {0xAA, 0xFE};
      adv.add(ad::type::kUuid16All, list, 2);
      if (!adv.add(ad::type::kServiceData16, d, uint8_t(n + 2))) {
        if (why) *why = "the URL doesn't fit in the 31 bytes";
        return false;
      }
      rsp.add(ad::type::kName, c.name, nameLen);
      break;
    }
    case Mode::Name:
      adv.add(ad::type::kName, c.name, nameLen);
      adv.add(ad::type::kTxPower, &c.txPower, 1);
      break;
  }
  if (!c.scanResponse) rsp = Packet{};
  return true;
}

}  // namespace b2
