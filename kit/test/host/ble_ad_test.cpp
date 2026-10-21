// Host test for kit/lib/ble-kit/src/ad_format.h: advertising packets need no radio, so they are
// checked on the Mac against packets as nRF Connect shows them.
//   c++ -std=c++17 -Wall -I kit/lib/ble-kit/src kit/test/host/ble_ad_test.cpp -o /tmp/ble_ad_test && /tmp/ble_ad_test
#include <cmath>
#include <cstdio>

#include "ad_format.h"

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++;   \
  } while (0)

int main() {
  // Flags 06, name "Bench B2", manufacturer FFFF + 3 bytes.
  const uint8_t bench[] = {0x02, 0x01, 0x06, 0x09, 0x09, 'B', 'e', 'n', 'c', 'h', ' ', 'B', '2', 0x06, 0xFF, 0xFF, 0xFF, 0x01, 0x02, 0x03};
  ad::Parsed p = ad::split(bench, sizeof(bench));
  CHECK(p.count == 3 && !p.malformed);
  CHECK(p.fields[0].type == ad::type::kFlags && p.fields[0].len == 1 && bench[p.fields[0].offset] == 0x06);
  CHECK(p.fields[1].type == ad::type::kName && p.fields[1].len == 8 && p.fields[1].offset == 5);
  CHECK(p.fields[2].type == ad::type::kManufacturer && p.fields[2].len == 5);
  CHECK(ad::guess(bench, sizeof(bench)) == ad::Kind::Bench);

  // A length that runs past the end is malformed; the fields before it still count.
  const uint8_t broken[] = {0x02, 0x01, 0x06, 0x09, 0x09, 'B'};
  p = ad::split(broken, sizeof(broken));
  CHECK(p.count == 1 && p.malformed);

  // Zero length = padding: stop there, not malformed.
  const uint8_t padded[] = {0x02, 0x01, 0x06, 0x00, 0x00, 0x00};
  p = ad::split(padded, sizeof(padded));
  CHECK(p.count == 1 && !p.malformed);

  // iBeacon: Apple 4C 00, type 02, length 15, UUID, major 0001, minor 0002, -59 dBm at 1 m.
  uint8_t ib[30] = {0x02, 0x01, 0x06, 0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15};
  ib[29] = uint8_t(int8_t(-59));
  int8_t power = 0;
  CHECK(ad::isIBeacon(ib, sizeof(ib), &power) && power == -59);
  CHECK(ad::guess(ib, sizeof(ib)) == ad::Kind::Beacon);

  // Apple "Nearby info" (10) = an iPhone, iPad or Mac nearby.
  const uint8_t iphone[] = {0x02, 0x01, 0x1A, 0x07, 0xFF, 0x4C, 0x00, 0x10, 0x02, 0x0B, 0x00};
  CHECK(ad::guess(iphone, sizeof(iphone)) == ad::Kind::Phone);

  // A keyboard says so with the HID service (1812) or its appearance (03C1).
  const uint8_t kb[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x12, 0x18};
  CHECK(ad::guess(kb, sizeof(kb)) == ad::Kind::Input);
  const uint8_t kb2[] = {0x03, 0x19, 0xC1, 0x03};
  CHECK(ad::guess(kb2, sizeof(kb2)) == ad::Kind::Input);
  CHECK(ad::kindFromAppearance(0x0841) == ad::Kind::Audio);

  // Random addresses: the top two bits of the most significant byte.
  CHECK(ad::addrKind(0, 0x4C) == ad::AddrKind::Public);
  CHECK(ad::addrKind(1, 0xC4) == ad::AddrKind::Static);
  CHECK(ad::addrKind(1, 0x5A) == ad::AddrKind::Resolvable);
  CHECK(ad::addrKind(1, 0x1A) == ad::AddrKind::NonResolvable);

  // Distance: at the 1 m reference = 1 m; 25 dB weaker with n = 2.5 = 10 m.
  CHECK(std::fabs(ad::distanceMeters(-59, -59) - 1.0f) < 0.01f);
  CHECK(std::fabs(ad::distanceMeters(-84, -59, 2.5f) - 10.0f) < 0.05f);

  // Eddystone-URL: "https://" = scheme 03, ".com/" = one byte 00.
  uint8_t url[24];
  const size_t n = ad::encodeEddystoneUrl("https://example.com/", -20, url, sizeof(url));
  CHECK(n == 11);
  CHECK(url[0] == 0x10 && int8_t(url[1]) == -20 && url[2] == 0x03 && url[3] == 'e' && url[10] == 0x00);
  CHECK(ad::encodeEddystoneUrl("ftp://x.com", 0, url, sizeof(url)) == 0);                          // unknown scheme
  CHECK(ad::encodeEddystoneUrl("https://a-very-long-name-here.com/", 0, url, sizeof(url)) == 0);  // over 17 bytes

  if (failures == 0) std::printf("ble ad format: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
