// The reader task: the only code that touches the RC522. It runs on core 0 and does two things:
//   - between jobs, polls for cards about 12 times a second (wake, select, halt)
//   - runs one job at a time from its mailbox (dump, write, an edge test, ...)
// loop() on core 1 never waits for the card: it submits jobs and draws the latest Snapshot.
// One owner per bus: two tasks talking to the same chip would interleave their SPI bytes.
#pragma once

#include <freertos/queue.h>

#include <mutex>

#include "edge.h"
#include "mifare_classic.h"
#include "pins.h"
#include "snapshot.h"

class Reader {
 public:
  enum class Job : uint8_t {
    Chip, Reset, SelfTest, Loopback, Regs, Spi, Gain, Irq, Timeout, Poll, Trace,
    Inventory, Dump, Read, Write, Credits, CreditsInit, CreditsAdd, CreditsUse,
    EdgeSpi, EdgeGain, EdgeTimeout, EdgeTiming, EdgeIrq, EdgeCrc, EdgeStress,
  };
  struct Request {
    Job job;
    int32_t a = 0;
    char text[17] = "";
  };

  static constexpr uint8_t kCreditsBlock = 8, kBackupBlock = 9;  // sector 2

  void begin() {
    mailbox_ = xQueueCreate(4, sizeof(Request));
    xTaskCreatePinnedToCore(task, "rfid", 12 * 1024, this, 2, nullptr, 0);
  }

  // From loop(). false when 4 jobs are already waiting.
  bool submit(const Request &r) { return xQueueSend(mailbox_, &r, 0) == pdTRUE; }

  // From loop(): copy the snapshot into `mine` if it changed.
  bool update(Snapshot &mine) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mine.version == snap_.version) return false;
    mine = snap_;
    return true;
  }

  Trace &trace() { return trace_; }

 private:
  static void task(void *self) { static_cast<Reader *>(self)->run(); }

  void run() {
    rfidSpi_ = new SPIClass(HSPI);  // the S3's SPI3: the screen keeps SPI2 to itself
    const uint8_t v = chip_.begin(*rfidSpi_, {PIN_RFID_SS, PIN_RFID_SCK, PIN_RFID_MOSI, PIN_RFID_MISO, PIN_RFID_RST, PIN_RFID_IRQ}, &trace_);
    publish([&](Snapshot &s) { s.chipVersion = v; });
    printChip(v);
    if (chip_.hasIrqPin()) chip_.setWaitMode(rc522::Mfrc522::WaitMode::Irq);
    syncSettings();
    for (;;) {
      Request r;
      if (xQueueReceive(mailbox_, &r, pdMS_TO_TICKS(polling_ ? 80 : 250)) == pdTRUE) {
        const uint32_t mark = trace_.head();
        perform(r);
        printTrace(mark);
        afterJob();
      } else {
        // Every 2 s: is the chip still answering? Then poll for cards (pointless while it's silent).
        if (millis() - lastHealth_ >= 2000) health();
        if (polling_ && !chipLost_) pollOnce();
      }
    }
  }

  // ---- Is the chip still there? -------------------------------------------------------------
  // VersionReg never changes on a working chip. 00h or FFh means nobody answers on MISO: a wire
  // lost contact, the chip lost power, or it hung. A hard reset tells the last case apart.

  static bool alive(uint8_t v) { return v != 0x00 && v != 0xFF; }

  void health() {
    lastHealth_ = millis();
    const uint8_t v = chip_.read(rc522::VersionReg);
    if (alive(v)) {
      if (chipLost_) {
        chipLost_ = false;
        publish([&](Snapshot &s) { s.chipVersion = v; });
        say(false, "chip answering again: VersionReg 0x%02X", v);
      }
      return;
    }
    if (chipLost_) return;  // already reported; keep checking quietly
    chipLost_ = true;
    publish([&](Snapshot &s) { s.chipVersion = v, s.present = false; });
    present_ = false;
    say(true, "chip stopped answering (VersionReg 0x%02X): hard reset", v);
    chip_.hardReset();
    const uint8_t again = chip_.read(rc522::VersionReg);
    if (alive(again)) {
      chipLost_ = false;
      publish([&](Snapshot &s) { s.chipVersion = again; });
      say(true, "chip back after a hard reset (0x%02X): it had hung", again);
    } else {
      say(true, "still 0x%02X after a reset: check 3.3V, GND, MISO", again);
      Serial.println("The chip doesn't answer on SPI. 0x00: no power or MISO held low (3.3V, GND). 0xFF: MISO (18) or SDA (15)");
      Serial.println("not connected. Press the RC522's header pins in (or solder them), then type: reset");
    }
  }

  void reset() {
    busy("reset");
    chip_.hardReset();
    const uint8_t v = chip_.read(rc522::VersionReg);
    chipLost_ = !alive(v);
    printChip(v);
    say(!alive(v), "hard reset: VersionReg 0x%02X", v);
  }

  // ---- Publishing --------------------------------------------------------------------------

  template <typename F>
  void publish(F change) {
    std::lock_guard<std::mutex> lock(mutex_);
    change(snap_);
    snap_.version++;
  }

  void say(bool bad, const char *fmt, ...) __attribute__((format(printf, 3, 4))) {
    char text[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (bad) log_w("%s", text);
    else log_i("%s", text);
    publish([&](Snapshot &s) { strlcpy(s.note, text, sizeof(s.note)), s.noteBad = bad; });
  }

  void busy(const char *what, int pct = 0) {
    publish([&](Snapshot &s) { s.busy = what, s.progress = uint8_t(pct); });
  }

  void syncSettings() {
    publish([&](Snapshot &s) {
      s.spiHz = chip_.spiHz();
      s.gain = chip_.gain();
      s.timeoutUs = chip_.timeoutUs();
      s.irqMode = chip_.waitMode() == rc522::Mfrc522::WaitMode::Irq;
      s.irqPin = chip_.hasIrqPin();
      s.polling = polling_;
      s.stats = chip_.stats();
    });
  }

  void afterJob() {
    chip_.stopCrypto1();
    checkIrq();
    publish([&](Snapshot &s) { s.busy = nullptr, s.progress = 0; });
    syncSettings();
  }

  // The IRQ wire stayed quiet although the chip finished: fall back to polling, and say so.
  void checkIrq() {
    if (!chip_.takeIrqMissed() || chip_.waitMode() != rc522::Mfrc522::WaitMode::Irq) return;
    chip_.setWaitMode(rc522::Mfrc522::WaitMode::Poll);
    say(true, "IRQ wire silent on GPIO %d: polling instead", PIN_RFID_IRQ);
  }

  // ---- Polling for cards -----------------------------------------------------------------------

  void pollOnce() {
    const uint32_t mark = trace_.head();
    iso14443a::Uid u;
    iso14443a::Collisions c;
    const rc522::Status st = iso14443a::wakeAndSelect(chip_, u, &c);
    const bool found = st == rc522::Status::Ok;
    if (found) iso14443a::halt(chip_);
    checkIrq();
    window_ = (window_ << 1) | (found ? 1 : 0);
    const uint8_t rate = uint8_t(__builtin_popcount(window_ & 0xFFFFF) * 5);  // last 20 polls

    const bool isNew = found && (!present_ || !u.sameAs(current_));
    if (isNew) {
      present_ = true, misses_ = 0, current_ = u;
      char id[32];
      u.format(id, sizeof(id));
      log_i("card %s · %s · ATQA %02X%02X SAK %02X%s", id, rfid::cardType(u.sak), u.atqa[1], u.atqa[0], u.sak,
            c.n ? " · collision resolved" : "");
      printTrace(mark);  // show how this card was found
    } else {
      trace_.rollback(mark);  // nothing new: keep the sniffer for interesting frames
    }
    if (found) misses_ = 0;
    // Three misses in a row (~0.3 s) before "gone": one weak poll at the edge of range isn't removal.
    const bool gone = !found && present_ && ++misses_ >= 3;
    if (gone) {
      present_ = false;
      log_i("card removed");
    }
    publish([&](Snapshot &s) {
      s.rate = rate;
      s.stats = chip_.stats();
      if (found) s.seenAt = millis(), s.uid = u, s.collisions = c;
      if (isNew) s.taps++, s.reads++, s.present = true;
      if (gone) s.present = false;
    });
  }

  // ---- Jobs ------------------------------------------------------------------------------------

  void perform(const Request &r) {
    using J = Job;
    switch (r.job) {
      case J::Chip: return printChip(chip_.read(rc522::VersionReg));
      case J::Reset: return reset();
      case J::SelfTest: return selfTest();
      case J::Loopback: return loopback();
      case J::Regs: return regs();
      case J::Spi:
        chip_.setSpiHz(uint32_t(r.a) * 1'000'000);
        return say(r.a > 10, "SPI clock %ld MHz%s", static_cast<long>(r.a), r.a > 10 ? ": above the chip's 10 MHz spec" : "");
      case J::Gain:
        chip_.setGain(uint8_t(r.a));
        return say(false, "receiver gain %u dB", rc522::Mfrc522::gainDb(uint8_t(r.a)));
      case J::Irq:
        if (!chip_.setWaitMode(r.a ? rc522::Mfrc522::WaitMode::Irq : rc522::Mfrc522::WaitMode::Poll))
          return say(true, "IRQ pin not configured");
        return say(false, "waiting by %s", r.a ? "IRQ wire" : "polling");
      case J::Timeout:
        chip_.setTimeoutUs(uint32_t(r.a));
        return say(false, "chip timer %ld us", static_cast<long>(r.a));
      case J::Poll:
        polling_ = r.a;
        return say(false, "card polling %s", polling_ ? "on" : "paused");
      case J::Trace:
        chip_.setTraceOn(r.a);
        return say(false, "sniffer %s", r.a ? "on" : "off");
      case J::Inventory: return inventory();
      case J::Dump: return dump();
      case J::Read: return readOne(uint8_t(r.a));
      case J::Write: return writeText(uint8_t(r.a), r.text);
      case J::Credits: return credits(0, 0);
      case J::CreditsInit: return credits(1, r.a);
      case J::CreditsAdd: return credits(2, r.a);
      case J::CreditsUse: return credits(3, r.a);
      case J::EdgeSpi: return runEdge("SPI sweep", [&](EdgeResult &e, const edge::Progress &p) { edge::spiSweep(chip_, e, p); });
      case J::EdgeGain: return runEdge("gain sweep", [&](EdgeResult &e, const edge::Progress &p) { edge::gainSweep(chip_, e, p); });
      case J::EdgeTimeout: return runEdge("timeout sweep", [&](EdgeResult &e, const edge::Progress &p) { edge::timeoutSweep(chip_, e, p); });
      case J::EdgeTiming: return runEdge("timing", [&](EdgeResult &e, const edge::Progress &p) { edge::timing(chip_, e, p); });
      case J::EdgeIrq: return runEdge("poll vs irq", [&](EdgeResult &e, const edge::Progress &p) { edge::irqVsPoll(chip_, e, p); });
      case J::EdgeCrc: return runEdge("crc race", [&](EdgeResult &e, const edge::Progress &p) { edge::crcRace(chip_, e, p); });
      case J::EdgeStress: {
        const int n = r.a > 0 ? r.a : 200;
        return runEdge("stress", [&](EdgeResult &e, const edge::Progress &p) { edge::stress(chip_, n, e, p); });
      }
    }
  }

  template <typename F>
  void runEdge(const char *name, F test) {
    busy(name);
    const bool keepTrace = chip_.traceOn();
    chip_.setTraceOn(false);  // hundreds of frames would flood the sniffer
    EdgeResult result;
    test(result, [&](int pct) { busy(name, pct); });
    chip_.setTraceOn(keepTrace);
    publish([&](Snapshot &s) { s.edge = result; });
    say(false, "%s: %s", name, result.caption);
  }

  // ---- R1: the chip ------------------------------------------------------------------------------

  void printChip(uint8_t v) {
    Serial.printf("\nRC522 on SPI3: SS %d SCK %d MOSI %d MISO %d RST %d IRQ %d · %lu MHz\n", PIN_RFID_SS, PIN_RFID_SCK,
                  PIN_RFID_MOSI, PIN_RFID_MISO, PIN_RFID_RST, PIN_RFID_IRQ, static_cast<unsigned long>(chip_.spiHz() / 1'000'000));
    Serial.printf("VersionReg 0x%02X: %s\n", v, rc522::versionName(v));
    if (v == 0x00 || v == 0xFF) {
      Serial.println("No answer. Check: 3.3V and GND, MISO on 18, SDA(SS) on 15, RST on 14 (not floating).");
      Serial.println("0x00 = MISO held low or chip unpowered · 0xFF = MISO not connected or chip not selected.");
    }
    publish([&](Snapshot &s) { s.chipVersion = v; });
  }

  // Datasheet section 16.1.1: a digital self-test that fills the FIFO with 64 known bytes.
  void selfTest() {
    static const uint8_t v1[64] = {
        0x00, 0xC6, 0x37, 0xD5, 0x32, 0xB7, 0x57, 0x5C, 0xC2, 0xD8, 0x7C, 0x4D, 0xD9, 0x70, 0xC7, 0x73,
        0x10, 0xE6, 0xD2, 0xAA, 0x5E, 0xA1, 0x3E, 0x5A, 0x14, 0xAF, 0x30, 0x61, 0xC9, 0x70, 0xDB, 0x2E,
        0x64, 0x22, 0x72, 0xB5, 0xBD, 0x65, 0xF4, 0xEC, 0x22, 0xBC, 0xD3, 0x72, 0x35, 0xCD, 0xAA, 0x41,
        0x1F, 0xA7, 0xF3, 0x53, 0x14, 0xDE, 0x7E, 0x02, 0xD9, 0x0F, 0xB5, 0x5E, 0x25, 0x1D, 0x29, 0x79};
    static const uint8_t v2[64] = {
        0x00, 0xEB, 0x66, 0xBA, 0x57, 0xBF, 0x23, 0x95, 0xD0, 0xE3, 0x0D, 0x3D, 0x27, 0x89, 0x5C, 0xDE,
        0x9D, 0x3B, 0xA7, 0x00, 0x21, 0x5B, 0x89, 0x82, 0x51, 0x3A, 0xEB, 0x02, 0x0C, 0xA5, 0x00, 0x49,
        0x7C, 0x84, 0x4D, 0xB3, 0xCC, 0xD2, 0x1B, 0x81, 0x5D, 0x48, 0x76, 0xD5, 0x71, 0x61, 0x21, 0xA9,
        0x86, 0x96, 0x83, 0x38, 0xCF, 0x9D, 0x5B, 0x6D, 0xDC, 0x15, 0xBA, 0x3E, 0x7D, 0x95, 0x3B, 0x2F};
    busy("self-test");
    const uint8_t version = chip_.read(rc522::VersionReg);
    if (!alive(version)) {
      publish([&](Snapshot &s) { s.selfTest = 0, s.chipVersion = version; });
      return say(true, "self-test skipped: chip silent (0x%02X). Check wires, reset", version);
    }
    const uint8_t *ref = version == 0x91 ? v1 : version == 0x92 ? v2 : nullptr;
    // A genuine NXP chip is compared with the datasheet's table. A clone has no table, so it runs
    // twice and the two runs are compared: a healthy chip gives the same 64 bytes every time.
    uint8_t got[64] = {}, again[64] = {};
    const int level = runSelfTest(got);
    const int level2 = ref ? 64 : runSelfTest(again);
    const uint8_t *compare = ref ? ref : again;
    int wrong = 0;
    Serial.printf("\nSelf-test (datasheet 16.1.1), chip 0x%02X %s, FIFO level %d:\n", version, rc522::versionName(version), level);
    for (int i = 0; i < 64; i++) {
      const bool bad = got[i] != compare[i];
      wrong += bad;
      Serial.printf("%02X%c%s", got[i], bad ? '!' : ' ', i % 16 == 15 ? "\n" : "");
    }
    Serial.println(ref ? "(! = differs from the datasheet's table)" : "(no datasheet table for this chip: ! = differs between two runs)");
    int8_t result;
    if (ref) result = wrong == 0 ? 1 : 0;
    else if (level < 64 || level2 < 64) result = 2;  // this clone doesn't run the self-test
    else result = wrong == 0 ? 3 : 0;
    publish([&](Snapshot &s) { s.selfTest = result; });
    switch (result) {
      case 1: return say(false, "self-test passed: 64/64 bytes match");
      case 3: return say(false, "self-test consistent: 2 runs, same 64 bytes");
      case 2: return say(true, "clone 0x%02X doesn't run the self-test (FIFO %d)", version, level);
      default: return say(true, "self-test FAILED: %d of 64 bytes differ (!)", wrong);
    }
  }

  // Steps 1-7 of section 16.1.1. Returns how many bytes the chip put in the FIFO (64 when it ran).
  int runSelfTest(uint8_t out[64]) {
    chip_.softReset();                                  // 1. soft reset
    uint8_t zeros[25] = {};
    chip_.write(rc522::FIFOLevelReg, 0x80);
    chip_.writeMany(rc522::FIFODataReg, zeros, 25);     // 2. 25 zero bytes into the internal buffer
    chip_.write(rc522::CommandReg, rc522::Mem);
    chip_.write(rc522::AutoTestReg, 0x09);              // 3. enable the self-test
    chip_.write(rc522::FIFODataReg, 0x00);              // 4. one more zero byte
    chip_.write(rc522::CommandReg, rc522::CalcCRC);     // 5. start
    int level = 0;
    for (int i = 0; i < 100 && level < 64; i++, delay(1)) level = chip_.read(rc522::FIFOLevelReg) & 0x7F;
    chip_.write(rc522::CommandReg, rc522::Idle);
    chip_.readMany(rc522::FIFODataReg, out, 64);        // 7. the 64 result bytes
    chip_.write(rc522::AutoTestReg, 0x00);
    chip_.hardReset();                                  // back to normal operation
    return level;
  }

  void loopback() {
    busy("FIFO loopback");
    int wrong = 0;
    uint8_t pattern[64];
    for (int k = 0; k < 50; k++) {
      for (int i = 0; i < 64; i++) pattern[i] = k % 2 ? uint8_t(i * 37 + k) : (i % 2 ? 0x55 : 0xAA);
      wrong += edge::fifoLoopback(chip_, pattern);
    }
    say(wrong != 0, "FIFO loopback: %d of 3200 bytes wrong at %lu MHz", wrong, static_cast<unsigned long>(chip_.spiHz() / 1'000'000));
  }

  void regs() {
    struct R {
      uint8_t reg;
      const char *name, *what;
    };
    static const R list[] = {
        {rc522::CommandReg, "CommandReg", "bits 3-0: running command (0 = idle)"},
        {rc522::ComIrqReg, "ComIrqReg", "Tx Rx Idle HiA LoA Err Timer"},
        {rc522::ErrorReg, "ErrorReg", "WrErr Temp - Ovfl Coll CRC Parity Proto"},
        {rc522::Status1Reg, "Status1Reg", "CRCOk CRCReady IRq TRunning"},
        {rc522::Status2Reg, "Status2Reg", "bit 3 MFCrypto1On, bits 2-0 modem state"},
        {rc522::FIFOLevelReg, "FIFOLevelReg", "bytes waiting in the FIFO"},
        {rc522::ControlReg, "ControlReg", "bits 2-0: valid bits in the last byte"},
        {rc522::TxControlReg, "TxControlReg", "bits 1-0: antenna drivers TX2 TX1"},
        {rc522::RFCfgReg, "RFCfgReg", "bits 6-4: receiver gain"},
        {rc522::TModeReg, "TModeReg", "bit 7 TAuto"},
        {rc522::TPrescalerReg, "TPrescalerReg", "timer prescaler (A9h = 25 us ticks)"},
        {rc522::TReloadRegH, "TReloadRegH", "timer reload, high byte"},
        {rc522::TReloadRegL, "TReloadRegL", "timer reload, low byte"},
        {rc522::AutoTestReg, "AutoTestReg", "40h normal, 09h self-test"},
        {rc522::VersionReg, "VersionReg", "91h v1.0 · 92h v2.0"},
    };
    Serial.println("\n  addr  register        value  bits      meaning");
    for (const R &r : list) {
      const uint8_t v = chip_.read(r.reg);
      char bits[9];
      for (int b = 0; b < 8; b++) bits[b] = (v >> (7 - b)) & 1 ? '1' : '0';
      bits[8] = 0;
      Serial.printf("  %02Xh   %-15s %02Xh    %s  %s\n", r.reg, r.name, v, bits, r.what);
    }
    Serial.printf("  SPI address bytes: read = 0x80 | reg << 1, write = reg << 1 (e.g. VersionReg read = 0x%02X)\n",
                  rc522::Mfrc522::readAddress(rc522::VersionReg));
  }

  // ---- R3: who's there ---------------------------------------------------------------------------

  void inventory() {
    busy("inventory");
    // Field off for 10 ms: every card loses power and restarts in IDLE (polling had halted them).
    chip_.antenna(false);
    delay(10);
    chip_.antenna(true);
    delay(5);
    iso14443a::Uid found[4];
    int n = 0;
    Serial.println("\nInventory: REQA, select one, halt it, repeat until nobody answers.");
    while (n < 4) {
      iso14443a::Uid u;
      rc522::Status s = iso14443a::request(chip_, iso14443a::REQA, u.atqa);
      if (s == rc522::Status::NoAnswer) break;
      iso14443a::Collisions c;
      s = iso14443a::select(chip_, u, &c);
      if (s != rc522::Status::Ok) {
        say(true, "inventory: select failed (%s)", rc522::statusName(s));
        break;
      }
      iso14443a::halt(chip_);  // halted cards ignore REQA: the next round finds the others
      char id[32];
      u.format(id, sizeof(id));
      Serial.printf("  #%d  %s  %s  SAK %02X", n + 1, id, rfid::cardType(u.sak), u.sak);
      for (int i = 0; i < c.n; i++) Serial.printf("  collision at level %u bit %u -> chose 1", c.at[i].level, c.at[i].bit);
      Serial.println();
      found[n++] = u;
      publish([&](Snapshot &s) { s.reads++; });  // one LED flash per card found
    }
    publish([&](Snapshot &s) {
      s.inventoryN = n;
      for (int i = 0; i < n; i++) s.inventory[i] = found[i];
    });
    say(false, "inventory: %d card%s in the field", n, n == 1 ? "" : "s");
  }

  // ---- R4 / R5: the card's memory -----------------------------------------------------------------

  bool card(iso14443a::Uid &u) {
    const rc522::Status s = iso14443a::wakeAndSelect(chip_, u);
    if (s == rc522::Status::Ok) {
      publish([&](Snapshot &sn) { sn.reads++; });  // the LED flashes for every read
      return true;
    }
    if (s == rc522::Status::ChipSilent) say(true, "the chip isn't answering: check wires, reset");
    else say(true, "no card: hold it on the reader (%s)", rc522::statusName(s));
    return false;
  }

  bool open(iso14443a::Uid &u, uint8_t block) {
    const rc522::Status s = mifare::authA(chip_, u, block, mifare::kFactoryKey);
    if (s == rc522::Status::Ok) return true;
    say(true, "sector %d won't open with the factory key (%s)", rfid::sectorOf(block), rc522::statusName(s));
    mifare::reselect(chip_, u);
    return false;
  }

  static void printBlock(int block, const uint8_t *d, const char *tag) {
    Serial.printf("  %3d  ", block);
    for (int i = 0; i < 16; i++) Serial.printf("%02X ", d[i]);
    Serial.print(" |");
    for (int i = 0; i < 16; i++) Serial.print(d[i] >= 32 && d[i] < 127 ? char(d[i]) : '.');
    Serial.printf("|  %s\n", tag);
  }

  void dump() {
    iso14443a::Uid u;
    busy("dump");
    if (!card(u)) return;
    const int total = rfid::classicSectors(u.sak);
    if (total == 0) return say(true, "%s: not a MIFARE Classic, no sectors to dump", rfid::cardType(u.sak));
    const int sectors = total > 16 ? 16 : total;
    MemoryMap map;
    map.uid = u;
    char id[32];
    u.format(id, sizeof(id));
    Serial.printf("\nDump of %s (%s), factory key A, sectors 0-%d\n  blk  data                                             ascii\n", id,
                  rfid::cardType(u.sak), sectors - 1);
    const bool keepTrace = chip_.traceOn();
    chip_.setTraceOn(false);
    for (int sec = 0; sec < sectors; sec++) {
      const int first = rfid::firstBlock(sec), trailer = rfid::trailerOf(sec);
      busy("dump", (sec + 1) * 100 / sectors);
      if (!open(u, uint8_t(first))) {
        for (int b = first; b <= trailer; b++) map.kind[b] = BlockKind::Closed;
        continue;
      }
      uint8_t blocks[4][16] = {};
      bool ok = true;
      for (int b = 0; b < 4 && ok; b++) ok = mifare::readBlock(chip_, uint8_t(first + b), blocks[b]) == rc522::Status::Ok;
      if (!ok) {
        for (int b = first; b <= trailer; b++) map.kind[b] = BlockKind::Closed;
        mifare::reselect(chip_, u);
        continue;
      }
      rfid::AccessBits ab;
      const bool bitsOk = rfid::AccessBits::decode(blocks[3], ab);
      for (int b = 0; b < 4; b++) {
        const int blk = first + b;
        memcpy(map.data[blk], blocks[b], 16);
        map.cond[blk] = ab.condition(b);
        int32_t value;
        uint8_t addr;
        bool empty = true;
        for (int i = 0; i < 16; i++) empty &= blocks[b][i] == 0;
        map.kind[blk] = b == 3 ? BlockKind::Trailer : blk == 0 ? BlockKind::Maker
                      : rfid::ValueBlock::decode(blocks[b], value, addr) ? BlockKind::Value
                      : empty ? BlockKind::Empty : BlockKind::Data;
        char tag[48];
        if (b == 3) snprintf(tag, sizeof(tag), "trailer %s: %s", bitsOk ? "ok" : "BITS BROKEN", rfid::AccessBits::trailerRule(ab.condition(3)));
        else if (blk == 0) snprintf(tag, sizeof(tag), "maker: UID, BCC, SAK, ATQA");
        else if (map.kind[blk] == BlockKind::Value) snprintf(tag, sizeof(tag), "value %ld · %s", static_cast<long>(value), rfid::AccessBits::dataRule(ab.condition(b)));
        else snprintf(tag, sizeof(tag), "%s", rfid::AccessBits::dataRule(ab.condition(b)));
        printBlock(blk, blocks[b], tag);
      }
      map.sectors = sec + 1;
    }
    chip_.setTraceOn(keepTrace);
    iso14443a::halt(chip_);
    map.valid = true;
    Serial.println("  Key A always reads back as 00: the card never reveals it.");
    publish([&](Snapshot &s) { s.map = map; });
    say(false, "dump: %d sectors read", map.sectors);
  }

  void readOne(uint8_t block) {
    iso14443a::Uid u;
    busy("read");
    if (!card(u) || !open(u, block)) return;
    uint8_t d[16];
    const rc522::Status s = mifare::readBlock(chip_, block, d);
    if (s != rc522::Status::Ok) return say(true, "read block %u: %s", block, rc522::statusName(s));
    int32_t value;
    uint8_t addr;
    printBlock(block, d, rfid::ValueBlock::decode(d, value, addr) ? "value block" : "");
    say(false, "block %u read", block);
  }

  void writeText(uint8_t block, const char *text) {
    if (!mifare::writable(block) || block >= 64)
      return say(true, "block %u: not writable here (block 0 and trailers are off limits)", block);
    iso14443a::Uid u;
    busy("write");
    if (!card(u) || !open(u, block)) return;
    uint8_t t[16];
    rc522::Status s = mifare::readBlock(chip_, uint8_t(rfid::trailerOf(rfid::sectorOf(block))), t);
    rfid::AccessBits ab;
    if (s != rc522::Status::Ok || !rfid::AccessBits::decode(t, ab) || !ab.keyAWrites(block % 4))
      return say(true, "block %u: the access bits don't allow key A writes", block);
    uint8_t data[16] = {};
    strncpy(reinterpret_cast<char *>(data), text, 16);
    s = mifare::writeBlock(chip_, block, data);
    uint8_t back[16] = {};
    if (s == rc522::Status::Ok) s = mifare::readBlock(chip_, block, back);
    if (s != rc522::Status::Ok) return say(true, "write block %u: %s", block, rc522::statusName(s));
    printBlock(block, back, memcmp(back, data, 16) == 0 ? "written and read back" : "READ BACK DIFFERS");
    say(memcmp(back, data, 16) != 0, "block %u: \"%.16s\"", block, text);
  }

  // Credits on the card as a MIFARE value block (block 8), with a backup copy (block 9).
  // op 0 show · 1 init(n) · 2 add(n) · 3 use(n)
  void credits(int op, int32_t n) {
    iso14443a::Uid u;
    busy("credits");
    if (!card(u) || !open(u, kCreditsBlock)) return;
    uint8_t d[16];
    int32_t value = 0, backup = 0;
    uint8_t addr;
    rc522::Status s = rc522::Status::Ok;
    auto readBoth = [&]() {
      const bool a = mifare::readBlock(chip_, kCreditsBlock, d) == rc522::Status::Ok && rfid::ValueBlock::decode(d, value, addr);
      const bool b = mifare::readBlock(chip_, kBackupBlock, d) == rc522::Status::Ok && rfid::ValueBlock::decode(d, backup, addr);
      return a && b;
    };
    if (op == 1) {
      rfid::ValueBlock::encode(n, kCreditsBlock, d);
      s = mifare::writeBlock(chip_, kCreditsBlock, d);
      if (s == rc522::Status::Ok) {
        rfid::ValueBlock::encode(n, kBackupBlock, d);
        s = mifare::writeBlock(chip_, kBackupBlock, d);
      }
    } else if (op >= 2) {
      if (!readBoth()) return say(true, "no credits on this card yet: credits init <n>");
      if (op == 3 && value < n) return say(true, "only %ld credits, can't use %ld", static_cast<long>(value), static_cast<long>(n));
      // 1. copy the current value into the backup block (restore = load, transfer = store)
      s = mifare::valueOp(chip_, mifare::RESTORE, kCreditsBlock, 0);
      if (s == rc522::Status::Ok) s = mifare::transfer(chip_, kBackupBlock);
      // 2. the card does the maths itself, then commits it in one step
      if (s == rc522::Status::Ok) s = mifare::valueOp(chip_, op == 2 ? mifare::INCREMENT : mifare::DECREMENT, kCreditsBlock, n);
      if (s == rc522::Status::Ok) s = mifare::transfer(chip_, kCreditsBlock);
    }
    if (s != rc522::Status::Ok) return say(true, "credits: %s", rc522::statusName(s));
    const bool valid = readBoth();
    publish([&](Snapshot &sn) { sn.creditsValid = valid, sn.credits = value, sn.backup = backup; });
    if (!valid) return say(true, "block 8 is not a value block: credits init <n>");
    Serial.printf("  credits %ld (block 8) · backup %ld (block 9)\n", static_cast<long>(value), static_cast<long>(backup));
    say(false, "credits: %ld", static_cast<long>(value));
  }

  // ---- The sniffer on serial ------------------------------------------------------------------

  void printTrace(uint32_t from) {
    if (!chip_.traceOn()) return;
    const uint32_t to = trace_.head();
    if (from >= to) return;
    TraceFrame f, prev;
    uint32_t t0 = 0;
    bool havePrev = false;
    for (uint32_t i = from; i < to; i++) {
      if (!trace_.get(i, f)) continue;
      if (!t0) t0 = f.us;
      char hex[64] = "";
      size_t p = 0;
      for (int k = 0; k < f.len && p + 4 < sizeof(hex); k++) p += snprintf(hex + p, sizeof(hex) - p, "%02X ", f.data[k]);
      const char *name = "";
      switch (f.kind) {
        case TraceFrame::Kind::Frame:
          name = f.fromCard ? (havePrev ? iso14443a::nameCardFrame(prev.data, prev.len, prev.lastBits, f.len, f.lastBits) : "")
                            : iso14443a::nameReaderFrame(f.data, f.len, f.lastBits);
          break;
        case TraceFrame::Kind::NoAnswer: name = "(no answer)"; break;
        case TraceFrame::Kind::Auth: name = "3-pass auth inside the chip: ok"; break;
        case TraceFrame::Kind::AuthFail: name = "3-pass auth: refused"; break;
        case TraceFrame::Kind::FieldOff: name = "field OFF"; break;
        case TraceFrame::Kind::FieldOn: name = "field ON"; break;
      }
      Serial.printf("  %8.3f ms  %s %-40s %s%s%s\n", (f.us - t0) / 1000.0f, f.fromCard ? "  card >" : "reader >", hex, name,
                    f.lastBits ? " (short)" : "", f.crypto ? " [encrypted on air]" : "");
      if (!f.fromCard && f.kind == TraceFrame::Kind::Frame) prev = f, havePrev = true;
    }
  }

  rc522::Mfrc522 chip_;
  SPIClass *rfidSpi_ = nullptr;
  Trace trace_;
  QueueHandle_t mailbox_ = nullptr;
  std::mutex mutex_;
  Snapshot snap_;
  // Poll state, only touched by the task.
  bool polling_ = true, present_ = false, chipLost_ = false;
  uint32_t lastHealth_ = 0;
  int misses_ = 0;
  uint32_t window_ = 0;
  iso14443a::Uid current_;
};
