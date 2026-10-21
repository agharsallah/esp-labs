// R7's access rules: who may open which lock, decided by the board from its own card list.
// Pure C++ (no Arduino), so labs/rfid-lab-r7/test/host/door_rules_test.cpp checks every rule on the Mac.
//
// The card only contributes its UID: everything else (name, role, blocked, dates, hours, uses)
// lives in the board's list, which only the app with the admin token can change.
//
// Two locks: the front door (every role) and the admin room (admin cards only). The one reader
// is mounted at one of them at a time; the app or serial `at front|admin` moves it.
//
// The order of the checks is part of the lesson:
//   1. unknown card              -> Unknown
//   2. blocked                   -> Blocked       (blocking always wins)
//   3. admin room, not an admin  -> AdminsOnly    (the role decides, not the rules below)
//   4. lockdown, not staff       -> Lockdown      (maintenance and admin cards still open)
//   5. needs a clock, none yet   -> NoClock       (fail closed: no time, no time-limited access)
//   6. past its end date         -> Expired
//   7. outside its hours         -> OutsideHours  (hours may wrap past midnight, e.g. 22-6)
//   8. a guest with 0 uses       -> NoUsesLeft
//   9. otherwise                 -> Granted       (a guest's uses go down by one)
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

namespace door {

enum class Role : uint8_t { Resident, Guest, Maintenance, Admin };  // stored as a byte: only append
enum class Decision : uint8_t { Granted, Unknown, Blocked, AdminsOnly, Lockdown, NoClock, Expired, OutsideHours, NoUsesLeft };
enum class Lock : uint8_t { Front, AdminRoom };
constexpr int kLocks = 2;

struct Card {
  uint8_t uid[10] = {};
  uint8_t uidLen = 0;
  char name[24] = "";
  Role role = Role::Resident;
  bool blocked = false;
  uint32_t validUntil = 0;           // unix seconds; 0 = no end date
  uint8_t fromHour = 0, toHour = 0;  // allowed local hours [from, to); equal = any time
  int16_t usesLeft = -1;             // -1 = unlimited; guests usually get a number
  uint32_t added = 0, lastSeen = 0;  // unix seconds (0 when the clock wasn't set yet)
  uint32_t taps = 0;
};

struct Clock {
  bool synced = false;  // internet time arrived (NTP)
  uint32_t now = 0;     // unix seconds
  uint8_t hour = 0;     // local hour, 0-23
};

inline bool needsClock(const Card &c) { return c.validUntil != 0 || c.fromHour != c.toHour; }

inline bool inHours(const Card &c, uint8_t hour) {
  if (c.fromHour == c.toHour) return true;
  if (c.fromHour < c.toHour) return hour >= c.fromHour && hour < c.toHour;
  return hour >= c.fromHour || hour < c.toHour;  // wraps past midnight
}

inline Decision decide(const Card *c, const Clock &clk, bool lockdown, Lock lock = Lock::Front) {
  if (!c) return Decision::Unknown;
  if (c->blocked) return Decision::Blocked;
  if (lock == Lock::AdminRoom && c->role != Role::Admin) return Decision::AdminsOnly;
  if (lockdown && c->role != Role::Maintenance && c->role != Role::Admin) return Decision::Lockdown;
  if (needsClock(*c) && !clk.synced) return Decision::NoClock;
  if (c->validUntil && clk.now >= c->validUntil) return Decision::Expired;
  if (!inHours(*c, clk.hour)) return Decision::OutsideHours;
  if (c->usesLeft == 0) return Decision::NoUsesLeft;
  return Decision::Granted;
}

inline const char *decisionName(Decision d) {
  switch (d) {
    case Decision::Granted: return "granted";
    case Decision::Unknown: return "unknown card";
    case Decision::Blocked: return "blocked";
    case Decision::AdminsOnly: return "admins only";
    case Decision::Lockdown: return "lockdown";
    case Decision::NoClock: return "no clock yet";
    case Decision::Expired: return "expired";
    case Decision::OutsideHours: return "outside hours";
    case Decision::NoUsesLeft: return "no uses left";
  }
  return "?";
}

inline const char *roleName(Role r) {
  switch (r) {
    case Role::Resident: return "resident";
    case Role::Guest: return "guest";
    case Role::Maintenance: return "maintenance";
    case Role::Admin: return "admin";
  }
  return "?";
}

inline bool roleFromName(const char *s, Role &out) {
  for (Role r : {Role::Resident, Role::Guest, Role::Maintenance, Role::Admin})
    if (strcmp(s, roleName(r)) == 0) return out = r, true;
  return false;
}

// "front" / "admin" in the API and on serial; "Front door" / "Admin room" for people.
inline const char *lockId(Lock l) { return l == Lock::AdminRoom ? "admin" : "front"; }
inline const char *lockTitle(Lock l) { return l == Lock::AdminRoom ? "Admin room" : "Front door"; }

inline bool lockFromId(const char *s, Lock &out) {
  for (Lock l : {Lock::Front, Lock::AdminRoom})
    if (strcmp(s, lockId(l)) == 0) return out = l, true;
  return false;
}

// UIDs travel as hex text in the API: "9C4F2A1B".
inline void uidToHex(const uint8_t *uid, uint8_t len, char *out, size_t n) {
  size_t p = 0;
  out[0] = 0;
  for (uint8_t i = 0; i < len && p + 2 < n; i++) p += snprintf(out + p, n - p, "%02X", uid[i]);
}

inline bool uidFromHex(const char *hex, uint8_t *out, uint8_t &len) {
  const size_t n = strlen(hex);
  if (n != 8 && n != 14 && n != 20) return false;  // 4, 7 or 10 bytes
  for (size_t i = 0; i < n; i += 2) {
    unsigned v = 0;
    if (sscanf(hex + i, "%2x", &v) != 1) return false;
    out[i / 2] = uint8_t(v);
  }
  len = uint8_t(n / 2);
  return true;
}

}  // namespace door
