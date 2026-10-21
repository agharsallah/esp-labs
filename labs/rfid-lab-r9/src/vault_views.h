// R9's four screens (320 x 170):
//   shield   idle: a radar sweep, the policy and the ledger size. After a tap: the verdict on a
//            coloured band, the callsign, why, and the counter moving on (#12 -> #13).
//            While a job waits for a card: what it will do, and a 20 s countdown bar.
//   crypto   the last tap through the pipeline: the keys, block 4 as ciphertext and as header,
//            the two tag slots (which one is active), the value block, and what was written.
//   x-ray    the clone checks: the Gen1a answer, block 0 now vs at provisioning (changed bytes
//            in coral), UID/BCC/SAK, the four timings against the saved baseline, the risk.
//   ledger   every provisioned card: number, callsign, clearance, counter, epoch, quarantine.
#pragma once

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>

#include <vector>

#include "color.h"
#include "frame_buffer.h"
#include "vault_crypto.h"
#include "vault_reader.h"

namespace vv {

constexpr int kW = 320, kH = 170;
constexpr uint32_t kVerdictMs = 9000;  // a verdict stays on the shield this long

inline void text(FrameBuffer &fb, int x, int y, uint16_t c, const char *s, const GFXfont *f = nullptr) {
  fb.setFont(f);
  fb.setTextColor(c);
  fb.setCursor(x, y);
  fb.print(s);
}

inline int width(FrameBuffer &fb, const char *s, const GFXfont *f) {
  fb.setFont(f);
  int16_t x1, y1;
  uint16_t w, h;
  fb.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

inline void centered(FrameBuffer &fb, int cx, int y, uint16_t c, const char *s, const GFXfont *f) {
  text(fb, cx - width(fb, s, f) / 2, y, c, s, f);
}

// Teal: in. Amber: not now, but nothing wrong. Coral: someone tampered with a card.
inline uint16_t verdictColor(vault::Verdict v) {
  if (vault::granted(v)) return pal::teal;
  if (vault::isAlert(v)) return pal::coral;
  return pal::amber;
}

inline void header(FrameBuffer &fb, const char *view, int index, int count, bool present) {
  text(fb, 8, 18, pal::ink, "VAULT ", &FreeSansBold9pt7b);
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  fb.print(view);
  const int first = 286 - (count - 1) * 10;
  for (int i = 0; i < count; i++) fb.fillCircle(first + i * 10, 14, 3, i == index ? pal::ink : pal::dim);
  fb.fillCircle(304, 14, 5, present ? pal::teal : pal::dim);
}

inline void footer(FrameBuffer &fb, const char *left, const char *right, uint16_t rightColor) {
  fb.fillRect(0, kH - 11, kW, 11, pal::night);
  text(fb, 6, kH - 9, pal::muted, left);
  fb.setFont(nullptr);
  int16_t x1, y1;
  uint16_t w, h;
  fb.getTextBounds(right, 0, 0, &x1, &y1, &w, &h);
  text(fb, kW - 6 - w, kH - 9, rightColor, right);
}

// The detail line in the small font, wrapped at a space onto at most two lines of `cols` characters.
inline void wrapped(FrameBuffer &fb, int x, int y, uint16_t c, const char *s, int cols) {
  const int n = int(strlen(s));
  if (n <= cols) return text(fb, x, y, c, s);
  int cut = cols;
  while (cut > cols / 2 && s[cut] != ' ') cut--;
  char first[64];
  strlcpy(first, s, size_t(std::min(cut + 1, int(sizeof(first)))));
  text(fb, x, y, c, first);
  char second[64];
  strlcpy(second, s + cut + (s[cut] == ' ' ? 1 : 0), size_t(std::min(cols + 1, int(sizeof(second)))));
  text(fb, x, y + 10, c, second);
}

inline void bar(FrameBuffer &fb, int x, int y, int w, int h, float k, uint16_t c) {
  fb.drawRect(x, y, w, h, pal::dim);
  const int fill = int((w - 2) * (k < 0 ? 0 : k > 1 ? 1 : k));
  if (fill > 0) fb.fillRect(x + 1, y + 1, fill, h - 2, c);
}

// ---- shield -----------------------------------------------------------------------------------

inline void radar(FrameBuffer &fb, int cx, int cy, int r, float t, uint16_t c) {
  for (int k = 1; k <= 3; k++) fb.drawCircle(cx, cy, r * k / 3, pal::dim);
  const float a = fmodf(t * 2.2f, 6.2832f);
  for (int i = 0; i < 10; i++) {  // the sweep and its fading tail
    const float b = a - i * 0.07f;
    fb.drawLine(cx, cy, cx + int(cosf(b) * r), cy + int(sinf(b) * r), mix(c, pal::night, uint8_t(i * 25)));
  }
  // A shield outline in the middle.
  fb.fillTriangle(cx - 9, cy - 8, cx + 9, cy - 8, cx, cy + 12, c);
  fb.fillRect(cx - 9, cy - 11, 19, 4, c);
}

inline void job(FrameBuffer &fb, const Scene &s) {
  const float left = std::max<int32_t>(0, int32_t(s.jobUntil - millis())) / float(VaultReader::kJobMs);
  const bool blink = (millis() / 400) % 2;
  fb.drawRoundRect(10, 30, 300, 108, 8, blink ? pal::amber : pal::dim);
  const char *what = s.job == Scene::Job::Provision ? "LAY A CARD TO PROVISION" : s.job == Scene::Job::Rekey ? "LAY THE CARD TO REKEY" : "LAY THE CARD TO WIPE";
  centered(fb, 160, 56, pal::amber, what, &FreeSansBold9pt7b);
  if (s.job == Scene::Job::Provision) {
    char line[48];
    snprintf(line, sizeof(line), "%s  -  %s", s.jobCallsign, vault::clearanceName(s.jobClearance));
    centered(fb, 160, 84, pal::ink, line, &FreeSansBold12pt7b);
  } else {
    centered(fb, 160, 84, pal::ink, s.job == Scene::Job::Rekey ? "new epoch, new keys" : "blocks 4-6 and 8-9 to zeros", &FreeSans9pt7b);
  }
  text(fb, 22, 100, pal::muted, "writes blocks 4-6, 8-9 only: never block 0 or a trailer");
  bar(fb, 22, 116, 276, 8, left, pal::amber);
}

inline void shield(FrameBuffer &fb, const Scene &s, const vault::Policy &policy, int cards, float t) {
  if (s.job != Scene::Job::None) return job(fb, s);
  const uint32_t since = millis() - s.at;
  if (!s.seq || since > kVerdictMs) {
    radar(fb, 70, 90, 50, t, pal::teal);
    text(fb, 140, 52, pal::ink, "Tap a card", &FreeSansBold12pt7b);
    char line[48];
    snprintf(line, sizeof(line), "needs %s or above", vault::clearanceName(policy.minClearance));
    text(fb, 140, 78, pal::muted, line, &FreeSans9pt7b);
    snprintf(line, sizeof(line), "clone guard %s", policy.strictCloneGuard ? "strict" : "audit only");
    text(fb, 140, 100, policy.strictCloneGuard ? pal::teal : pal::amber, line, &FreeSans9pt7b);
    snprintf(line, sizeof(line), "%d card%s in the vault", cards, cards == 1 ? "" : "s");
    text(fb, 140, 122, pal::muted, line, &FreeSans9pt7b);
    return;
  }
  const uint16_t c = verdictColor(s.verdict);
  // The band slides in, and alerts pulse.
  const int w = int(std::min(1.0f, since / 250.0f) * 304);
  const bool pulse = vault::isAlert(s.verdict) && since < 3000 && (since / 250) % 2;
  fb.fillRoundRect(8, 28, w, 40, 6, pulse ? mix(c, pal::night, 110) : c);
  if (w > 200) text(fb, 18, 56, pal::night, vault::verdictName(s.verdict), &FreeSansBold12pt7b);
  char line[48];
  text(fb, 12, 90, pal::ink, s.callsign[0] ? s.callsign : s.uid, &FreeSansBold9pt7b);
  if (s.number) {
    snprintf(line, sizeof(line), "card %u", s.number);
    text(fb, 300 - width(fb, line, &FreeSans9pt7b), 90, pal::muted, line, &FreeSans9pt7b);
  }
  wrapped(fb, 12, 100, pal::muted, s.detail, 50);
  if (s.wrote) {
    snprintf(line, sizeof(line), "#%lu", static_cast<unsigned long>(s.header.counter));
    text(fb, 12, 140, pal::muted, line, &FreeSans9pt7b);
    // The counter moving on: an arrow that grows, then the new number.
    const float k = std::min(1.0f, since / 900.0f);
    const int x0 = 18 + width(fb, line, &FreeSans9pt7b), x1 = x0 + int(36 * k);
    fb.drawFastHLine(x0, 135, x1 - x0, pal::teal);
    fb.fillTriangle(x1, 131, x1, 139, x1 + 5, 135, pal::teal);
    if (k >= 1) {
      snprintf(line, sizeof(line), "#%lu  slot %c", static_cast<unsigned long>(s.next.counter), s.next.slot ? 'B' : 'A');
      text(fb, x1 + 10, 140, pal::teal, line, &FreeSansBold9pt7b);
    }
  }
  snprintf(line, sizeof(line), "risk %u%%", s.forensics.risk);
  text(fb, 236, 128, pal::muted, line);
  bar(fb, 236, 138, 72, 6, s.forensics.risk / 100.0f, s.forensics.risk >= 40 ? pal::coral : pal::teal);
}

// ---- crypto -----------------------------------------------------------------------------------

inline void hexRow(FrameBuffer &fb, int x, int y, const uint8_t *b, int n, uint16_t c) {
  char hex[40];
  vault::toHex(b, size_t(n), hex, sizeof(hex), ' ');
  text(fb, x, y, c, hex);
}

inline void crypto(FrameBuffer &fb, const Scene &s, const char *fingerprint) {
  if (!s.seq) {
    centered(fb, 160, 90, pal::muted, "Tap a card to follow it through", &FreeSans9pt7b);
    return;
  }
  char line[64];
  text(fb, 8, 30, pal::muted, "KEYS");
  snprintf(line, sizeof(line), "HMAC(master %s, UID %s, epoch %u)", fingerprint, s.uid, s.headerOk ? s.header.epoch : 0);
  text(fb, 44, 30, pal::ink, line);

  text(fb, 8, 46, pal::muted, "B4");
  hexRow(fb, 44, 46, s.block4, 8, pal::amber);
  text(fb, 236, 46, pal::muted, "AES ...");
  if (s.headerOk) {
    snprintf(line, sizeof(line), "\"VLT9\" #%lu  %s  slot %c  nonce %08lX", static_cast<unsigned long>(s.header.counter),
             vault::clearanceName(s.header.clearance), s.header.slot ? 'B' : 'A', static_cast<unsigned long>(s.header.nonce));
    text(fb, 44, 58, pal::teal, line);
  } else {
    text(fb, 44, 58, pal::coral, "doesn't decrypt to a vault header");
  }

  const bool aActive = s.headerOk && s.header.slot == 0;
  text(fb, 8, 76, pal::muted, "B5 A");
  hexRow(fb, 44, 76, s.slotA, 8, aActive ? pal::ink : pal::muted);
  text(fb, 236, 76, aActive ? pal::teal : pal::muted, aActive ? "in use" : "spare");
  text(fb, 8, 88, pal::muted, "B6 B");
  hexRow(fb, 44, 88, s.slotB, 8, s.headerOk && !aActive ? pal::ink : pal::muted);
  text(fb, 236, 88, s.headerOk && !aActive ? pal::teal : pal::muted, s.headerOk && !aActive ? "in use" : "spare");

  text(fb, 8, 104, pal::muted, "B9");
  if (s.valueValid) snprintf(line, sizeof(line), "value block %ld", static_cast<long>(s.value));
  else snprintf(line, sizeof(line), "not a value block");
  text(fb, 44, 104, s.valueValid ? pal::ink : pal::amber, line);

  fb.drawFastHLine(8, 118, 304, pal::dim);
  if (s.wrote) {
    // The three writes, in their order, ticking in one after the other.
    const uint32_t since = millis() - s.at;
    const char *steps[3] = {"1 tag -> spare slot", "2 header -> B4", "3 counter -> B9"};
    for (int i = 0; i < 3; i++) {
      const bool done = since > uint32_t(300 + i * 350);
      const int x = 8 + i * 104;
      fb.fillRoundRect(x, 124, 100, 18, 4, done ? mix(pal::teal, pal::night, 150) : pal::night);
      fb.drawRoundRect(x, 124, 100, 18, 4, done ? pal::teal : pal::dim);
      text(fb, x + 6, 129, done ? pal::ink : pal::muted, steps[i]);
    }
    snprintf(line, sizeof(line), "now #%lu, tag in slot %c", static_cast<unsigned long>(s.next.counter), s.next.slot ? 'B' : 'A');
    text(fb, 8, 148, pal::teal, line);
  } else {
    wrapped(fb, 8, 126, vault::isAlert(s.verdict) ? pal::coral : pal::muted, s.detail, 52);
  }
}

// ---- x-ray ------------------------------------------------------------------------------------

inline void xray(FrameBuffer &fb, const Scene &s) {
  if (!s.seq) {
    centered(fb, 160, 90, pal::muted, "Tap a card to inspect it", &FreeSans9pt7b);
    return;
  }
  const vault::Forensics &f = s.forensics;
  fb.fillRoundRect(8, 26, 112, 16, 4, f.gen1a ? pal::coral : mix(pal::teal, pal::night, 150));
  text(fb, 14, 30, f.gen1a ? pal::night : pal::ink, f.gen1a ? "GEN1A: ANSWERED" : "GEN1A: SILENT");
  char line[48];
  snprintf(line, sizeof(line), "UID=B0 %s  BCC %s  SAK %s", f.uidMatches ? "ok" : "NO", f.bccOk ? "ok" : "BAD", f.sakMatches ? "ok" : "odd");
  text(fb, 128, 30, f.uidMatches && f.bccOk && f.sakMatches ? pal::muted : pal::amber, line);

  // Block 0, byte by byte. Second row: as saved at provisioning, changed bytes in coral.
  text(fb, 8, 50, pal::muted, "block 0 now");
  text(fb, 8, 64, pal::muted, s.hasSaved ? "saved" : "(not saved)");
  for (int i = 0; i < 16; i++) {
    const int x = 82 + i * 14;
    char b[3];
    snprintf(b, sizeof(b), "%02X", s.block0[i]);
    const bool diff = s.hasSaved && s.block0[i] != s.saved0[i];
    if (diff) fb.fillRect(x - 1, 48, 13, 25, pal::coral);
    text(fb, x, 50, diff ? pal::night : pal::ink, b);
    if (s.hasSaved) {
      snprintf(b, sizeof(b), "%02X", s.saved0[i]);
      text(fb, x, 64, diff ? pal::night : pal::muted, b);
    }
  }

  // The four timings, with a tick where the provisioned card was.
  const char *names[4] = {"WUPA", "SELECT", "AUTH", "READ"};
  const uint16_t now[4] = {s.timing.wupaUs, s.timing.selectUs, s.timing.authUs, s.timing.readUs};
  const uint16_t base[4] = {s.baseline.wupaUs, s.baseline.selectUs, s.baseline.authUs, s.baseline.readUs};
  for (int i = 0; i < 4; i++) {
    const int y = 82 + i * 13;
    text(fb, 8, y, pal::muted, names[i]);
    const int w = std::min(170, std::max(2, int(now[i]) / 25));
    fb.fillRect(56, y, w, 7, f.timingDrift && i >= 2 ? pal::amber : pal::teal);
    if (base[i]) fb.drawFastVLine(56 + std::min(170, int(base[i]) / 25), y - 2, 11, pal::ink);
    snprintf(line, sizeof(line), "%u us", now[i]);
    text(fb, 236, y, pal::ink, line);
  }
  snprintf(line, sizeof(line), "risk %u%%", f.risk);
  text(fb, 8, 140, f.risk >= 40 ? pal::coral : pal::muted, line);
  bar(fb, 64, 139, 244, 8, f.risk / 100.0f, f.risk >= 40 ? pal::coral : pal::teal);
}

// ---- ledger -----------------------------------------------------------------------------------

inline void ledger(FrameBuffer &fb, const std::vector<vault::LedgerCard> &cards, const Scene &s) {
  if (cards.empty()) {
    centered(fb, 160, 82, pal::muted, "The vault is empty", &FreeSansBold9pt7b);
    centered(fb, 160, 106, pal::muted, "long press, or: provision <callsign>", &FreeSans9pt7b);
    return;
  }
  text(fb, 8, 28, pal::muted, " #  CALLSIGN        CLEARANCE  COUNTER EPOCH");
  const size_t shown = std::min<size_t>(cards.size(), 8);
  for (size_t i = 0; i < shown; i++) {
    const vault::LedgerCard &c = cards[i];
    const int y = 42 + int(i) * 13;
    char uid[21];
    vault::toHex(c.uid, c.uidLen, uid, sizeof(uid));
    if (s.present && strcmp(uid, s.presentUid) == 0) fb.fillRect(4, y - 3, 312, 13, pal::dim);
    char line[64];
    snprintf(line, sizeof(line), "%2u  %-15s %-10s %7lu %5u", c.number, c.callsign, c.quarantined ? "QUARANTINE" : vault::clearanceName(c.clearance),
             static_cast<unsigned long>(c.counter), c.epoch);
    text(fb, 8, y, c.quarantined ? pal::coral : pal::ink, line);
  }
  if (cards.size() > shown) {
    char more[24];
    snprintf(more, sizeof(more), "+%u more (serial: cards)", unsigned(cards.size() - shown));
    text(fb, 8, 146, pal::muted, more);
  }
}

}  // namespace vv
