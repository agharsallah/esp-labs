// Host test for labs/ble-lab-b2/src/beacon_payload.h: B2's packets fit, and read back as they were built.
//   c++ -std=c++17 -Wall -I kit/lib/ble-kit/src -I labs/ble-lab-b2/src labs/ble-lab-b2/test/host/ble_beacon_test.cpp -o /tmp/ble_beacon_test && /tmp/ble_beacon_test
#include <cstdio>

#include "beacon_payload.h"

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++;   \
  } while (0)

int main() {
  b2::Config c;
  b2::Packet adv, rsp;

  // bench: flags (3) + name "Bench B2" (10) + manufacturer FFFF + 7 bytes (11) = 24.
  c.live = {513, -1234, 3600};
  CHECK(b2::build(c, adv, rsp));
  CHECK(adv.n == 24 && rsp.n == 7);
  ad::Parsed p = ad::split(adv.b, adv.n);
  CHECK(p.count == 3 && !p.malformed);
  const ad::Field *m = ad::find(p, ad::type::kManufacturer);
  b2::Live back;
  CHECK(m && b2::parseBench(adv.b + m->offset, m->len, back));
  CHECK(back.presses == 513 && back.tempCenti == -1234 && back.uptime == 3600);
  CHECK(adv.b[m->offset + 3] == 0x01 && adv.b[m->offset + 4] == 0x02);  // 513 = 0x0201, low byte first
  CHECK(ad::guess(adv.b, adv.n) == ad::Kind::Bench);

  // ibeacon: 30 bytes, recognised as an iBeacon with its 1 m power; major/minor big-endian.
  c.mode = b2::Mode::IBeacon;
  c.beacon.major = 0x0102;
  CHECK(b2::build(c, adv, rsp));
  int8_t power = 0;
  CHECK(adv.n == 30 && ad::isIBeacon(adv.b, adv.n, &power) && power == -59);
  CHECK(adv.b[5 + 20] == 0x01 && adv.b[5 + 21] == 0x02);
  CHECK(rsp.n == 10);  // the name moved to the scan response

  // url: fits, service data starts with FEAA (little-endian AA FE) and frame type 10.
  c.mode = b2::Mode::Url;
  c.url = "https://example.com/";
  CHECK(b2::build(c, adv, rsp));
  p = ad::split(adv.b, adv.n);
  const ad::Field *sd = ad::find(p, ad::type::kServiceData16);
  CHECK(sd && adv.b[sd->offset] == 0xAA && adv.b[sd->offset + 1] == 0xFE && adv.b[sd->offset + 2] == 0x10);
  CHECK(int8_t(adv.b[sd->offset + 3]) == -18);  // -59 at 1 m + 41 dB = power at 0 m
  CHECK(adv.n <= 31);
  const char *why = nullptr;
  c.url = "https://this-name-is-far-too-long.example/";
  CHECK(!b2::build(c, adv, rsp, &why) && why);

  // name, scan response off: nothing in the second packet.
  c.mode = b2::Mode::Name;
  c.scanResponse = false;
  CHECK(b2::build(c, adv, rsp));
  CHECK(adv.n == 16 && rsp.n == 0);

  // add() refuses what would not fit.
  b2::Packet full;
  uint8_t big[29] = {};
  CHECK(full.add(0xFF, big, 29) && full.n == 31);
  CHECK(!full.add(0x01, big, 1) && full.n == 31);

  if (failures == 0) std::printf("ble beacon payload: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
