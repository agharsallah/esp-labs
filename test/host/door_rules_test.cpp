// Host test for src/rfid-lab-r7/access_rules.h: every door rule, on the Mac.
//   c++ -std=c++17 -Wall -I src/rfid-lab-r7 test/host/door_rules_test.cpp -o /tmp/door_test && /tmp/door_test
#include <cstdio>

#include "access_rules.h"

using namespace door;

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) std::printf("FAIL line %d: %s\n", __LINE__, #cond), failures++;   \
  } while (0)

int main() {
  Clock day{true, 1'800'000'000, 14};
  Card c;
  CHECK(decide(nullptr, day, false) == Decision::Unknown);
  CHECK(decide(&c, day, false) == Decision::Granted);

  c.blocked = true;  // blocking wins over everything, even for maintenance in lockdown
  c.role = Role::Maintenance;
  CHECK(decide(&c, day, true) == Decision::Blocked);
  c.blocked = false;
  CHECK(decide(&c, day, true) == Decision::Granted);  // maintenance opens in lockdown
  c.role = Role::Resident;
  CHECK(decide(&c, day, true) == Decision::Lockdown);

  c.validUntil = day.now + 60;  // a clock is needed now: fail closed without one
  CHECK(decide(&c, Clock{false, 0, 0}, false) == Decision::NoClock);
  CHECK(decide(&c, day, false) == Decision::Granted);
  c.validUntil = day.now;  // the end date itself is already expired
  CHECK(decide(&c, day, false) == Decision::Expired);
  c.validUntil = 0;

  c.fromHour = 8, c.toHour = 18;
  CHECK(decide(&c, day, false) == Decision::Granted);
  CHECK(decide(&c, Clock{true, day.now, 18}, false) == Decision::OutsideHours);  // [from, to)
  c.fromHour = 22, c.toHour = 6;  // over midnight
  CHECK(inHours(c, 23) && inHours(c, 2) && !inHours(c, 6) && !inHours(c, 14));
  c.fromHour = c.toHour = 0;

  c.role = Role::Guest, c.usesLeft = 0;
  CHECK(decide(&c, day, false) == Decision::NoUsesLeft);
  c.usesLeft = 3;
  CHECK(decide(&c, day, false) == Decision::Granted);

  uint8_t uid[10], len = 0;
  CHECK(uidFromHex("9C4F2A1B", uid, len) && len == 4 && uid[0] == 0x9C && uid[3] == 0x1B);
  CHECK(!uidFromHex("9C4F2A", uid, len) && !uidFromHex("ZZ4F2A1B", uid, len));
  char hex[21];
  uidToHex(uid, 4, hex, sizeof(hex));
  CHECK(strcmp(hex, "9C4F2A1B") == 0);
  Role r;
  CHECK(roleFromName("guest", r) && r == Role::Guest && !roleFromName("admin", r));

  std::printf(failures ? "%d check(s) failed\n" : "all door rule checks passed\n", failures);
  return failures ? 1 : 0;
}
