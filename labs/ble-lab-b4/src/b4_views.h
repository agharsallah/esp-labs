// B4's own screens (320 x 170); gatt, link and log come from kit/lib/ble-kit:
//   ceremony  the eight phases of pairing on a track. Reported phases fill teal, inferred ones get a
//             teal ring, the one waiting pulses amber, a failure stops the track in coral with why.
//   bonds     who the board remembers: identity addresses from the bond store, and the trick the IRK
//             plays: the phone's private address changes, the board still knows it's the same phone
#pragma once

#include <math.h>

#include "ble_views.h"
#include "ceremony.h"

namespace b4v {

using namespace bv;

struct Settings {
  uint8_t method = 1;  // 0 justworks, 1 passkey, 2 compare
  bool lock = false;
  bool allowBondedOnly = false;
};

inline const char *methodName(uint8_t m) {
  static const char *const names[] = {"Just Works", "Passkey", "Compare"};
  return names[m % 3];
}

// One or two lines about the phase in focus.
inline void explain(FrameBuffer &fb, int y, int phase, b4::How how) {
  static const char *const lines[][2] = {
      {"Connected, nothing secret yet: anyone listening", "can read every byte. Touch the Vault or type secure."},
      {"Pairing Request / Response: what each side can", "do (screen? keys? yes/no?), bond?, MITM?"},
      {"The two answers pick the method (see the guide's", "matrix). This board asks for MITM protection."},
      {"Each side sends a P-256 public key. Both compute", "the same secret (ECDH); it never goes on air."},
      {"A person proves no one sits in between: type the", "6 digits, or confirm both screens match."},
      {"Encrypted with AES-CCM. From here an eavesdropper", "sees noise."},
      {"Keys to remember go over the encrypted link: LTK", "(the session key) and IRK (who you really are)."},
      {"Bonded: saved in flash. Next connection skips", "straight to encrypted, no person needed."},
  };
  if (how == b4::How::JustWorks && phase == b4::Check) {
    small(fb, 6, y, pal::amber, "Just Works: no check. Stops eavesdroppers, but a");
    small(fb, 6, y + 10, pal::amber, "man in the middle could have done the same.");
    return;
  }
  small(fb, 6, y, pal::ink, lines[phase][0]);
  small(fb, 6, y + 10, pal::muted, lines[phase][1]);
}

inline void ceremony(FrameBuffer &fb, const b4::Ceremony &c, const ble::LinkState &s, const Settings &set, float t) {
  const bool up = s.radio == ble::Radio::Connected;
  const int y = 70;
  auto nodeX = [](int i) { return 22 + i * 39; };
  // The furthest phase reached decides where the dot runs to.
  int last = -1;
  for (int i = 0; i < b4::kPhases; i++)
    if (c.at[i]) last = i;
  if (!up && !c.failed) last = -1;
  fb.drawFastHLine(nodeX(0), y, nodeX(b4::kPhases - 1) - nodeX(0), pal::dim);
  if (last > 0) fb.drawFastHLine(nodeX(0), y, nodeX(last) - nodeX(0), pal::teal);
  const int waiting = last + 1 < b4::kPhases ? last + 1 : -1;
  for (int i = 0; i < b4::kPhases; i++) {
    const int x = nodeX(i);
    const bool reached = up && c.at[i];
    if (c.failed && i == waiting) {
      fb.fillCircle(x, y, 8, pal::coral);
      fb.drawLine(x - 4, y - 4, x + 4, y + 4, pal::night);
      fb.drawLine(x - 4, y + 4, x + 4, y - 4, pal::night);
    } else if (reached && c.inferred[i]) {
      fb.fillCircle(x, y, 7, pal::night);
      fb.drawCircle(x, y, 7, pal::teal);
      fb.fillCircle(x, y, 2, pal::teal);
    } else if (reached) {
      fb.fillCircle(x, y, 7, i == b4::Encrypted || i == b4::Bonded ? kViolet : pal::teal);
    } else if (up && i == waiting) {
      const uint8_t k = uint8_t(128 + 120 * sinf(t * 5));
      fb.fillCircle(x, y, 7, mix(pal::night, pal::amber, k));
    } else {
      fb.fillCircle(x, y, 6, pal::dim);
    }
    if (i == b4::Encrypted && reached) lockIcon(fb, x - 5, y - 6, pal::night);
    // Labels would collide 39 px apart: even phases over the line, odd ones under it.
    const int lw = int(strlen(b4::phaseName(i))) * 6;
    small(fb, std::max(0, std::min(kW - lw, x - lw / 2)), y + (i % 2 ? 12 : -19), reached ? pal::ink : pal::muted, b4::phaseName(i));
  }
  char v[80];
  if (!up) {
    small(fb, 6, 40, pal::muted, s.advertising ? "Waiting for a phone to connect..." : "not advertising");
    if (c.failed) {
      snprintf(v, sizeof(v), "last attempt failed: %s", c.why);
      small(fb, 6, 100, pal::coral, v);
    } else if (c.how != b4::How::Unknown) {
      snprintf(v, sizeof(v), "last time: %s", b4::howName(c.how));
      small(fb, 6, 100, pal::muted, v);
    }
  } else {
    snprintf(v, sizeof(v), "%s%s", b4::howName(c.how), c.how == b4::How::Stored ? ": bonded before, no ceremony" : "");
    bold(fb, 6, 42, c.failed ? pal::coral : c.how == b4::How::Unknown ? pal::muted : kViolet, c.how == b4::How::Unknown ? "plain text" : v);
    if (c.failed) {
      snprintf(v, sizeof(v), "failed: %s", c.why);
      small(fb, 6, 100, pal::coral, v);
      small(fb, 6, 110, pal::muted, "the phone may need to forget the board first");
    } else {
      explain(fb, 100, waiting >= 0 ? waiting : b4::Bonded, c.how);
    }
  }
  snprintf(v, sizeof(v), "board asks: %s, lock %s, %s", methodName(set.method), set.lock ? "on" : "off", set.allowBondedOnly ? "bonded only" : "anyone");
  small(fb, 6, 128, pal::muted, v);
  small(fb, 6, 140, pal::dim, "ring = inferred: NimBLE doesn't report that step");
}

// ---- bonds --------------------------------------------------------------------------------------

struct BondRow {
  char addr[18];
  bool random;
};

inline void bonds(FrameBuffer &fb, const BondRow *rows, int n, const ble::LinkState &s, float t) {
  char v[64];
  snprintf(v, sizeof(v), "%d bonded", n);
  bold(fb, 6, 46, n ? pal::ink : pal::muted, v);
  for (int i = 0; i < n && i < 5; i++) {
    const int y = 56 + i * 12;
    const bool here = s.radio == ble::Radio::Connected && (strcasecmp(rows[i].addr, s.peer.idAddr) == 0 || strcasecmp(rows[i].addr, s.peer.addr) == 0);
    snprintf(v, sizeof(v), "%d %s %s", i + 1, rows[i].addr, rows[i].random ? "static" : "public");
    small(fb, 6, y, here ? kViolet : pal::muted, v);
    if (here) small(fb, 160, y, kViolet, "<- here");
  }
  if (n == 0) small(fb, 6, 58, pal::muted, "Nobody yet: pair a phone (touch the Vault).");

  // The privacy trick, animated: the phone's address on air changes every few seconds (in real life
  // every ~15 minutes); with the IRK the board resolves each one to the same identity.
  const int px = 220, py = 92;
  fb.drawRoundRect(px, py - 34, 26, 52, 5, pal::muted);
  const uint32_t slot = uint32_t(t / 3.0f);
  uint32_t h = slot * 2654435761u;
  snprintf(v, sizeof(v), "%02X:%02X:%02X", unsigned(0x40 | ((h >> 24) & 0x3F)), unsigned((h >> 16) & 0xFF), unsigned((h >> 8) & 0xFF));
  small(fb, 254, 62, pal::amber, "on air:");
  small(fb, 254, 72, pal::amber, v);
  const float k = fmodf(t, 3.0f) / 3.0f;
  fb.fillCircle(px + 26 + int(k * 40), py - 4, 2, pal::amber);
  small(fb, 254, 92, pal::teal, n ? "IRK says:" : "no IRK:");
  small(fb, 254, 102, pal::teal, n ? "same phone" : "a stranger");
  small(fb, 6, 128, pal::muted, "Identity address + IRK come with bonding. The IRK turns");
  small(fb, 6, 138, pal::muted, "each private address back into the identity: allow bonded");
  small(fb, 6, 148, pal::muted, "uses that to let only your phone connect.");
}

}  // namespace b4v
