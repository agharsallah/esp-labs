// The edge lab: experiments that push one setting until things break, so you see where the
// limits really are on your bench. Each fills an EdgeResult (a bar chart for the screen) and
// prints a table. They run inside the reader task, which owns the chip.
//
//   spi      SPI clock 1-40 MHz: when do bytes start coming back wrong? (datasheet: 10 Mbit/s)
//   gain     receiver gain 18-48 dB at a fixed distance: how often is the card found?
//   timeout  shrink the chip's wait timer: how soon after our frame does the card answer?
//   timing   how long each protocol step takes, in microseconds
//   irq      waiting by polling the chip vs sleeping until the IRQ wire fires
//   crc      the chip's CRC coprocessor vs the same maths on the ESP32
//   stress   hundreds of read cycles: how reliable is one read?
#pragma once

#include <esp_random.h>

#include <functional>

#include "mifare_classic.h"
#include "snapshot.h"

namespace edge {

using iso14443a::Uid;
using rc522::Mfrc522;
using rc522::Status;
using Progress = std::function<void(int)>;  // 0-100

// Write a pattern into the 64-byte FIFO and read it back. Returns the bytes that came back wrong.
inline int fifoLoopback(Mfrc522 &chip, const uint8_t *pattern, int n = 64) {
  uint8_t back[64] = {};
  chip.write(rc522::FIFOLevelReg, 0x80);
  chip.writeMany(rc522::FIFODataReg, pattern, n);
  const int level = chip.read(rc522::FIFOLevelReg) & 0x7F;
  chip.readMany(rc522::FIFODataReg, back, n);
  int wrong = level == n ? 0 : 1;
  for (int i = 0; i < n; i++) wrong += back[i] != pattern[i];
  return wrong > n ? n : wrong;
}

inline void spiSweep(Mfrc522 &chip, EdgeResult &r, const Progress &progress) {
  static const uint8_t mhz[] = {1, 2, 4, 8, 10, 13, 16, 20, 26, 40};
  constexpr int kRounds = 20;
  const uint32_t keep = chip.spiHz();
  r = EdgeResult{};
  strlcpy(r.title, "SPI clock vs errors", sizeof(r.title));
  strlcpy(r.unit, "%", sizeof(r.unit));
  r.goodHigh = false;
  Serial.println("\n  MHz   bytes wrong   VersionReg reads wrong");
  int firstBad = -1;
  for (size_t i = 0; i < sizeof(mhz); i++) {
    chip.setSpiHz(uint32_t(mhz[i]) * 1'000'000);
    int wrong = 0, versionWrong = 0;
    uint8_t pattern[64];
    for (int k = 0; k < kRounds; k++) {
      esp_fill_random(pattern, sizeof(pattern));
      wrong += fifoLoopback(chip, pattern);
      const uint8_t v = chip.read(rc522::VersionReg);
      versionWrong += (v == 0x00 || v == 0xFF || v != chip.read(rc522::VersionReg));
    }
    const float pct = 100.0f * wrong / (kRounds * 64);
    char l[7];
    snprintf(l, sizeof(l), "%u", mhz[i]);
    r.add(l, pct);
    if (mhz[i] == 10) r.mark = r.n - 1;
    if (firstBad < 0 && (wrong || versionWrong)) firstBad = mhz[i];
    Serial.printf("  %3u   %5.1f %%        %d / %d\n", mhz[i], pct, versionWrong, kRounds);
    progress(int((i + 1) * 100 / sizeof(mhz)));
  }
  chip.setSpiHz(keep);
  chip.hardReset();  // a garbled write at high speed may have changed a setting: start clean
  if (firstBad < 0) snprintf(r.caption, sizeof(r.caption), "clean up to 40 MHz (spec: 10)");
  else snprintf(r.caption, sizeof(r.caption), "errors from %d MHz (spec: 10)", firstBad);
  Serial.printf("  -> %s. Back to %lu MHz, chip reset.\n", r.caption, static_cast<unsigned long>(keep / 1'000'000));
}

// How many of `tries` wake + select attempts succeed with the current settings.
inline int successRate(Mfrc522 &chip, int tries, bool selectToo) {
  int ok = 0;
  for (int i = 0; i < tries; i++) {
    Uid u;
    chip.stopCrypto1();
    Status s = iso14443a::request(chip, iso14443a::WUPA, u.atqa);
    if ((s == Status::Ok || s == Status::Collision) && selectToo) s = iso14443a::select(chip, u);
    if (s == Status::Ok || (!selectToo && s == Status::Collision)) {
      ok++;
      if (selectToo) iso14443a::halt(chip);
    }
  }
  return ok * 100 / tries;
}

inline void gainSweep(Mfrc522 &chip, EdgeResult &r, const Progress &progress) {
  static const uint8_t codes[] = {0, 1, 4, 5, 6, 7};  // 2 and 3 repeat 18 and 23 dB
  const uint8_t keep = chip.gain();
  r = EdgeResult{};
  strlcpy(r.title, "Receiver gain (dB) vs found", sizeof(r.title));
  strlcpy(r.unit, "%", sizeof(r.unit));
  Serial.println("\n  gain    found (wake + select, 30 tries)");
  int lowest = -1;
  for (size_t i = 0; i < sizeof(codes); i++) {
    chip.setGain(codes[i]);
    const int pct = successRate(chip, 30, true);
    char l[7];
    snprintf(l, sizeof(l), "%u", Mfrc522::gainDb(codes[i]));
    r.add(l, pct);
    if (codes[i] == keep) r.mark = r.n - 1;
    if (lowest < 0 && pct >= 90) lowest = Mfrc522::gainDb(codes[i]);
    Serial.printf("  %2u dB   %3d %%\n", Mfrc522::gainDb(codes[i]), pct);
    progress(int((i + 1) * 100 / sizeof(codes)));
  }
  chip.setGain(keep);
  if (lowest < 0) snprintf(r.caption, sizeof(r.caption), "not reliable at any gain: move closer");
  else snprintf(r.caption, sizeof(r.caption), "reliable from %d dB at this distance", lowest);
  Serial.printf("  -> %s. Lift the card 1 cm and run it again.\n", r.caption);
}

inline void timeoutSweep(Mfrc522 &chip, EdgeResult &r, const Progress &progress) {
  static const uint16_t us[] = {25000, 5000, 1000, 500, 300, 200, 150, 120, 100, 80, 60, 40};
  const uint32_t keep = chip.timeoutUs();
  r = EdgeResult{};
  strlcpy(r.title, "Wait time (us) vs answers", sizeof(r.title));
  strlcpy(r.unit, "%", sizeof(r.unit));
  Serial.println("\n  timer     WUPA answered   wake + select ok   (20 tries each)");
  int shortest = -1;
  for (size_t i = 0; i < sizeof(us) / sizeof(us[0]); i++) {
    chip.setTimeoutUs(us[i]);
    const int wake = successRate(chip, 20, false);
    const int sel = successRate(chip, 20, true);
    char l[7];
    if (us[i] >= 1000) snprintf(l, sizeof(l), "%ums", us[i] / 1000);
    else snprintf(l, sizeof(l), "%u", us[i]);
    r.add(l, wake);
    if (wake >= 90) shortest = us[i];
    Serial.printf("  %5u us   %3d %%            %3d %%\n", us[i], wake, sel);
    progress(int((i + 1) * 100 / (sizeof(us) / sizeof(us[0]))));
  }
  chip.setTimeoutUs(keep);
  if (shortest < 0) snprintf(r.caption, sizeof(r.caption), "no card answered: hold it on the reader");
  else snprintf(r.caption, sizeof(r.caption), "card answers within %d us (timer ticks are 25 us)", shortest);
  Serial.printf("  -> %s. ISO 14443 says ~86 us after REQA/WUPA.\n", r.caption);
}

inline void timing(Mfrc522 &chip, EdgeResult &r, const Progress &progress) {
  constexpr int kTries = 40;
  const char *names[] = {"WUPA", "SELECT", "AUTH", "READ"};
  uint32_t sum[4] = {}, lo[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}, hi[4] = {};
  int n[4] = {};
  for (int t = 0; t < kTries; t++) {
    Uid u;
    chip.stopCrypto1();
    // Each step's own duration comes from the chip driver (start of command to its end).
    uint32_t us[4] = {};
    if (iso14443a::request(chip, iso14443a::WUPA, u.atqa) != Status::Ok) continue;
    us[0] = chip.stats().lastUs;
    if (iso14443a::select(chip, u) != Status::Ok) continue;
    us[1] = chip.stats().lastUs;  // the final SELECT frame (anticollision came just before)
    if (mifare::authA(chip, u, 4, mifare::kFactoryKey) != Status::Ok) continue;
    us[2] = chip.stats().lastUs;
    uint8_t block[16];
    if (mifare::readBlock(chip, 4, block) != Status::Ok) continue;
    us[3] = chip.stats().lastUs;
    iso14443a::halt(chip);
    for (int k = 0; k < 4; k++) {
      sum[k] += us[k], n[k]++;
      lo[k] = min(lo[k], us[k]), hi[k] = max(hi[k], us[k]);
    }
    progress((t + 1) * 100 / kTries);
  }
  chip.stopCrypto1();
  r = EdgeResult{};
  strlcpy(r.title, "One step, average (us)", sizeof(r.title));
  strlcpy(r.unit, "us", sizeof(r.unit));
  r.goodHigh = false;
  Serial.println("\n  step      min      avg      max   (us, start of command to answer)");
  for (int k = 0; k < 4; k++) {
    const float avg = n[k] ? float(sum[k]) / n[k] : 0;
    r.add(names[k], avg);
    if (n[k]) Serial.printf("  %-6s  %6lu   %6.0f   %6lu\n", names[k], static_cast<unsigned long>(lo[k]), avg, static_cast<unsigned long>(hi[k]));
  }
  int slowest = 0;
  for (int k = 1; k < 4; k++) if (r.value[k] > r.value[slowest]) slowest = k;
  if (!n[0]) snprintf(r.caption, sizeof(r.caption), "no complete cycle: hold the card still");
  else snprintf(r.caption, sizeof(r.caption), "%d full cycles; slowest step: %s", n[0], names[slowest]);
  Serial.printf("  -> %s. Air time alone: WUPA ~0.3 ms, READ ~2 ms at 106 kbit/s.\n", r.caption);
}

inline void irqVsPoll(Mfrc522 &chip, EdgeResult &r, const Progress &progress) {
  r = EdgeResult{};
  strlcpy(r.title, "SPI accesses per command", sizeof(r.title));
  strlcpy(r.unit, "ops", sizeof(r.unit));
  r.goodHigh = false;
  if (!chip.hasIrqPin()) {
    snprintf(r.caption, sizeof(r.caption), "IRQ pin not configured");
    Serial.println("  The IRQ wire is not configured (PIN_RFID_IRQ = -1).");
    return;
  }
  const auto keep = chip.waitMode();
  constexpr int kTries = 30;
  float ops[2], ms[2];
  uint32_t missing = 0;
  for (int m = 0; m < 2; m++) {
    chip.setWaitMode(m == 0 ? Mfrc522::WaitMode::Poll : Mfrc522::WaitMode::Irq);
    const uint32_t ops0 = chip.stats().spiOps, missing0 = chip.stats().irqMissing;
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < kTries; i++) {
      uint8_t atqa[2];
      chip.stopCrypto1();
      iso14443a::request(chip, iso14443a::WUPA, atqa);
      progress((m * kTries + i + 1) * 100 / (2 * kTries));
    }
    ops[m] = float(chip.stats().spiOps - ops0) / kTries;
    ms[m] = (esp_timer_get_time() - t0) / 1000.0f / kTries;
    if (m == 1) missing = chip.stats().irqMissing - missing0;
  }
  chip.takeIrqMissed();
  chip.setWaitMode(keep);
  r.add("poll", ops[0]);
  r.add("irq", ops[1]);
  Serial.printf("\n  mode   SPI accesses / WUPA   ms / WUPA\n  poll   %8.1f              %6.2f\n  irq    %8.1f              %6.2f\n",
                ops[0], ms[0], ops[1], ms[1]);
  if (missing) snprintf(r.caption, sizeof(r.caption), "IRQ never fired %lu times: check its wire", static_cast<unsigned long>(missing));
  else snprintf(r.caption, sizeof(r.caption), "IRQ saves %.0f SPI accesses per command", ops[0] - ops[1]);
  Serial.printf("  -> %s. Try it with and without a card on the reader.\n", r.caption);
}

inline void crcRace(Mfrc522 &chip, EdgeResult &r, const Progress &progress) {
  constexpr int kFrames = 300;
  int mismatches = 0;
  int64_t hwUs = 0, swUs = 0;
  for (int i = 0; i < kFrames; i++) {
    uint8_t frame[32], hw[2] = {};
    const int len = 1 + esp_random() % 32;
    esp_fill_random(frame, len);
    int64_t t = esp_timer_get_time();
    const bool done = chip.calcCrc(frame, len, hw) == Status::Ok;
    hwUs += esp_timer_get_time() - t;
    t = esp_timer_get_time();
    const uint16_t sw = rfid::crcA(frame, len);
    swUs += esp_timer_get_time() - t;
    if (!done || sw != uint16_t(hw[0] | (hw[1] << 8))) mismatches++;
    if (i % 30 == 29) progress((i + 1) * 100 / kFrames);
  }
  r = EdgeResult{};
  strlcpy(r.title, "CRC_A time per frame (us)", sizeof(r.title));
  strlcpy(r.unit, "us", sizeof(r.unit));
  r.goodHigh = false;
  r.add("chip", float(hwUs) / kFrames);
  r.add("ESP32", float(swUs) / kFrames);
  snprintf(r.caption, sizeof(r.caption), "%d of %d disagree; the CPU wins", mismatches, kFrames);
  Serial.printf("\n  chip coprocessor %.1f us/frame (mostly SPI), ESP32 %.2f us/frame, %d mismatches\n",
                float(hwUs) / kFrames, float(swUs) / kFrames, mismatches);
}

inline void stress(Mfrc522 &chip, int cycles, EdgeResult &r, const Progress &progress) {
  // Outcome buckets, in chart order.
  const char *names[] = {"ok", "absent", "CRC", "proto", "coll", "NAK", "other"};
  int count[7] = {};
  const int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < cycles; i++) {
    Uid u;
    Status s = iso14443a::wakeAndSelect(chip, u);
    if (s == Status::Ok) s = mifare::authA(chip, u, 4, mifare::kFactoryKey);
    uint8_t block[16];
    if (s == Status::Ok) s = mifare::readBlock(chip, 4, block);
    if (s == Status::Ok) iso14443a::halt(chip);
    chip.stopCrypto1();
    const int b = s == Status::Ok ? 0 : s == Status::NoAnswer ? 1 : s == Status::CrcWrong ? 2 : s == Status::ProtocolErr ? 3
                : s == Status::Collision ? 4 : s == Status::Nak ? 5 : 6;
    count[b]++;
    if (i % 10 == 9) progress((i + 1) * 100 / cycles);
  }
  const float secs = (esp_timer_get_time() - t0) / 1e6f;
  r = EdgeResult{};
  snprintf(r.title, sizeof(r.title), "%d read cycles", cycles);
  strlcpy(r.unit, "", sizeof(r.unit));
  for (int k = 0; k < 7; k++) r.add(names[k], count[k]);
  r.mark = 0;
  snprintf(r.caption, sizeof(r.caption), "%.1f %% ok, %.0f reads/s", 100.0f * count[0] / cycles, cycles / secs);
  Serial.printf("\n  wake + select + auth + read block 4, %d times in %.1f s\n", cycles, secs);
  for (int k = 0; k < 7; k++) Serial.printf("  %-7s %4d\n", names[k], count[k]);
  Serial.printf("  -> %s. Try moving or tilting the card while it runs.\n", r.caption);
}

}  // namespace edge
