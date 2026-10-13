// The MFRC522 reader chip (the black square on the blue RC522 board), driven register by register.
// No library: every byte that crosses the SPI bus is written in this file.
//
// Layers, each built on the one before:
//   mfrc522.h         this file: SPI framing, registers, FIFO, commands, timer, waiting, transceive
//   iso14443a.h       the card protocol: wake (REQA/WUPA), anticollision, select, halt
//   mifare_classic.h  the card's memory: authenticate, read, write, value blocks
//
// Section numbers refer to the NXP MFRC522 datasheet rev 3.9:
// https://www.nxp.com/docs/en/data-sheet/MFRC522.pdf
//
// Waiting for the card: the chip raises bits in ComIrqReg when a frame was received or its timer
// ran out. We can learn that two ways (WaitMode):
//   Poll  keep reading ComIrqReg over SPI: simple, but the bus is busy the whole time
//   Irq   sleep until the IRQ wire pulls low: no bus traffic, needs the 7th wire
// The edge lab measures the difference.
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "rfid_format.h"
#include "trace.h"

namespace rc522 {

// Registers used here (section 9.2).
enum Reg : uint8_t {
  CommandReg = 0x01, ComIEnReg = 0x02, DivIEnReg = 0x03, ComIrqReg = 0x04, DivIrqReg = 0x05,
  ErrorReg = 0x06, Status1Reg = 0x07, Status2Reg = 0x08, FIFODataReg = 0x09, FIFOLevelReg = 0x0A,
  ControlReg = 0x0C, BitFramingReg = 0x0D, CollReg = 0x0E,
  ModeReg = 0x11, TxModeReg = 0x12, RxModeReg = 0x13, TxControlReg = 0x14, TxASKReg = 0x15,
  CRCResultRegH = 0x21, CRCResultRegL = 0x22, ModWidthReg = 0x24, RFCfgReg = 0x26,
  TModeReg = 0x2A, TPrescalerReg = 0x2B, TReloadRegH = 0x2C, TReloadRegL = 0x2D,
  AutoTestReg = 0x36, VersionReg = 0x37,
};

// Commands written to CommandReg (section 10.3).
enum Cmd : uint8_t { Idle = 0x00, Mem = 0x01, CalcCRC = 0x03, Transceive = 0x0C, MFAuthent = 0x0E, SoftReset = 0x0F };

// ComIrqReg bits (section 9.3.1.5).
constexpr uint8_t kTxIRq = 0x40, kRxIRq = 0x20, kIdleIRq = 0x10, kErrIRq = 0x02, kTimerIRq = 0x01;

enum class Status : uint8_t {
  Ok,
  NoAnswer,     // the chip's timer ran out: no card answered (or it answered too late)
  Collision,    // two or more cards answered at once and their bits clashed
  CrcWrong,     // the frame arrived but its checksum doesn't match
  ProtocolErr,  // parity, framing or length wrong: a weak or disturbed signal
  Overflow,     // more bytes arrived than fit in the FIFO
  NoRoom,       // more bytes arrived than our buffer holds
  Nak,          // MIFARE said no (4-bit NAK): wrong key, not allowed, wrong block
  BadBcc,       // a UID part's check byte is wrong
  ChipSilent,   // the chip itself never finished: wiring, power or reset problem
};

inline const char *statusName(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::NoAnswer: return "no answer";
    case Status::Collision: return "collision";
    case Status::CrcWrong: return "CRC wrong";
    case Status::ProtocolErr: return "protocol error";
    case Status::Overflow: return "FIFO overflow";
    case Status::NoRoom: return "answer too long";
    case Status::Nak: return "NAK";
    case Status::BadBcc: return "BCC wrong";
    case Status::ChipSilent: return "chip silent";
  }
  return "?";
}

struct Pins {
  int8_t ss, sck, mosi, miso, rst, irq;  // irq = -1: not wired
};

// Counters the edge lab reads.
struct Stats {
  uint32_t spiOps = 0;      // chip-select cycles (one register access or one burst)
  uint32_t spiBytes = 0;    // bytes clocked over SPI, both directions at once
  uint32_t polls = 0;       // ComIrqReg reads spent waiting (Poll mode)
  uint32_t irqWakes = 0;    // times the IRQ wire woke us (Irq mode)
  uint32_t irqMissing = 0;  // times it should have and didn't (the chip had finished anyway)
  uint32_t commands = 0;    // Transceive / MFAuthent runs
  uint32_t lastUs = 0;      // duration of the last command, start to finish
};

class Mfrc522 {
 public:
  enum class WaitMode : uint8_t { Poll, Irq };
  static constexpr uint32_t kDefaultHz = 4'000'000;   // well inside the chip's 10 Mbit/s
  static constexpr uint32_t kMaxHz = 10'000'000;      // section 8.1.2
  static constexpr uint32_t kTimerTickUs = 25;        // 13.56 MHz / (2 x 169 + 1) = 40 kHz
  static constexpr uint32_t kDefaultTimeoutUs = 25'000;

  // Bring the chip up: SPI on its own bus, a hard reset through the RST pin, then our settings.
  // Returns the VersionReg value (0x91 / 0x92 for a genuine chip, 0x00 or 0xFF when nothing answers).
  uint8_t begin(SPIClass &spi, const Pins &pins, Trace *trace = nullptr) {
    spi_ = &spi;
    pins_ = pins;
    trace_ = trace;
    pinMode(pins_.ss, OUTPUT);
    digitalWrite(pins_.ss, HIGH);
    spi_->begin(pins_.sck, pins_.miso, pins_.mosi, -1);  // we drive SS ourselves
    if (pins_.irq >= 0) {
      done_ = xSemaphoreCreateBinary();
      pinMode(pins_.irq, INPUT_PULLUP);
      attachInterruptArg(digitalPinToInterrupt(pins_.irq), onIrq, this, FALLING);
    }
    hardReset();
    return read(VersionReg);
  }

  // RST low = hard power-down (NRSTPD). Rising edge = reset; the oscillator then needs a moment.
  void hardReset() {
    pinMode(pins_.rst, OUTPUT);
    digitalWrite(pins_.rst, LOW);
    delayMicroseconds(100);
    digitalWrite(pins_.rst, HIGH);
    delay(50);
    configure();
  }

  void softReset() {
    write(CommandReg, SoftReset);
    delay(50);
    for (int i = 0; i < 3 && (read(CommandReg) & 0x10); i++) delay(50);  // PowerDown bit clears when ready
    configure();
  }

  // ---- Registers over SPI (section 8.1.2) ----------------------------------------------------
  // One address byte, MSB first: bit 7 = 1 read / 0 write, bits 6-1 = register, bit 0 = 0.

  static uint8_t readAddress(uint8_t reg) { return uint8_t(0x80 | (reg << 1)); }
  static uint8_t writeAddress(uint8_t reg) { return uint8_t((reg << 1) & 0x7E); }

  uint8_t read(uint8_t reg) {
    select();
    spi_->transfer(readAddress(reg));
    const uint8_t v = spi_->transfer(0);
    deselect(2);
    return v;
  }

  void write(uint8_t reg, uint8_t value) {
    select();
    spi_->transfer(writeAddress(reg));
    spi_->transfer(value);
    deselect(2);
  }

  // Burst write: one address byte, then every data byte (into the FIFO, n of them).
  void writeMany(uint8_t reg, const uint8_t *data, size_t n) {
    select();
    spi_->transfer(writeAddress(reg));
    for (size_t i = 0; i < n; i++) spi_->transfer(data[i]);
    deselect(1 + n);
  }

  // Burst read (Table 6): send the address again for every byte but the last, which gets 00.
  // rxAlign > 0: the first byte only fills bits rxAlign..7; bits below it keep what out[0] had
  // (the bits we sent ourselves during anticollision).
  void readMany(uint8_t reg, uint8_t *out, size_t n, uint8_t rxAlign = 0) {
    if (n == 0) return;
    const uint8_t a = readAddress(reg);
    select();
    spi_->transfer(a);
    for (size_t i = 0; i < n; i++) {
      const uint8_t v = spi_->transfer(i + 1 < n ? a : 0);
      if (i == 0 && rxAlign) {
        const uint8_t mask = uint8_t(0xFF << rxAlign);
        out[0] = uint8_t((out[0] & ~mask) | (v & mask));
      } else {
        out[i] = v;
      }
    }
    deselect(1 + n);
  }

  void setBits(uint8_t reg, uint8_t mask) { write(reg, read(reg) | mask); }
  void clearBits(uint8_t reg, uint8_t mask) { write(reg, read(reg) & ~mask); }

  // ---- Settings -------------------------------------------------------------------------------

  uint32_t spiHz() const { return hz_; }
  void setSpiHz(uint32_t hz) { hz_ = hz; }

  // Receiver gain, RFCfgReg bits 6-4 (Table 98). 0-7; 2 and 3 repeat 18 and 23 dB.
  static uint8_t gainDb(uint8_t code) {
    static const uint8_t db[8] = {18, 23, 18, 23, 33, 38, 43, 48};
    return db[code & 7];
  }
  uint8_t gain() const { return gain_; }
  void setGain(uint8_t code) {
    gain_ = code & 7;
    write(RFCfgReg, uint8_t(gain_ << 4));
  }

  void antenna(bool on) {
    if (on) setBits(TxControlReg, 0x03);  // Tx1RFEn + Tx2RFEn: both antenna drivers on
    else clearBits(TxControlReg, 0x03);
    antennaOn_ = on;
    if (trace_) trace_->add(on ? TraceFrame::Kind::FieldOn : TraceFrame::Kind::FieldOff, false, nullptr, 0, 0, false);
  }
  bool antennaOn() const { return antennaOn_; }

  // How long the chip waits for the card's answer before giving up (its own timer, TAuto: starts
  // at the end of our frame). 25 ms by default; the edge lab shrinks it to find the card's limit.
  uint32_t timeoutUs() const { return timeoutUs_; }
  void setTimeoutUs(uint32_t us) {
    timeoutUs_ = us;
    uint32_t ticks = us / kTimerTickUs;
    if (ticks < 1) ticks = 1;
    if (ticks > 0xFFFF) ticks = 0xFFFF;
    write(TReloadRegH, uint8_t(ticks >> 8));
    write(TReloadRegL, uint8_t(ticks & 0xFF));
  }

  WaitMode waitMode() const { return mode_; }
  bool hasIrqPin() const { return pins_.irq >= 0; }
  // Irq mode needs the wire; returns false (and stays in Poll) when IRQ isn't configured.
  bool setWaitMode(WaitMode m) {
    if (m == WaitMode::Irq && !hasIrqPin()) return false;
    mode_ = m;
    // IRQ pin: push-pull (DivIEnReg bit 7), active low (ComIEnReg IRqInv, bit 7).
    write(DivIEnReg, m == WaitMode::Irq ? 0x80 : 0x00);
    write(ComIEnReg, 0x80);
    return true;
  }

  bool crypto1On() { return read(Status2Reg) & 0x08; }
  void stopCrypto1() { clearBits(Status2Reg, 0x08); }

  Stats &stats() { return stats_; }
  Trace *trace() { return trace_; }

  // ---- The CRC coprocessor (section 10.3.1.4) -------------------------------------------------

  Status calcCrc(const uint8_t *data, size_t n, uint8_t out[2]) {
    write(CommandReg, Idle);
    write(DivIrqReg, 0x04);     // clear CRCIRq
    write(FIFOLevelReg, 0x80);  // flush the FIFO
    writeMany(FIFODataReg, data, n);
    write(CommandReg, CalcCRC);
    const int64_t deadline = esp_timer_get_time() + 90'000;
    while (!(read(DivIrqReg) & 0x04)) {
      if (esp_timer_get_time() > deadline) return Status::ChipSilent;
    }
    write(CommandReg, Idle);
    out[0] = read(CRCResultRegL);
    out[1] = read(CRCResultRegH);
    return Status::Ok;
  }

  // ---- Talking to the card ----------------------------------------------------------------------
  //
  // transceive(): put `send` in the FIFO, send it, wait, read the answer.
  //   validBits  in: bits used in the last byte we send (7 for REQA, 0 = all 8)
  //             out: bits used in the last byte received
  //   rxAlign    where the first received bit lands in back[0] (anticollision only)
  //   checkCrc   the answer must end in a correct CRC_A (and a 4-bit answer means NAK)
  Status transceive(const uint8_t *send, uint8_t sendLen, uint8_t *back, uint8_t *backLen,
                    uint8_t *validBits = nullptr, uint8_t rxAlign = 0, bool checkCrc = false) {
    return communicate(Transceive, kRxIRq | kIdleIRq, send, sendLen, back, backLen, validBits, rxAlign, checkCrc);
  }

  // MIFARE authentication, done inside the chip (section 10.3.1.9): we hand it command, block,
  // the 6-byte key and the card's UID; it runs the 3-pass handshake and switches Crypto1 on.
  Status authenticate(uint8_t command, uint8_t block, const uint8_t key[6], const uint8_t uid4[4]) {
    uint8_t buf[12] = {command, block};
    memcpy(buf + 2, key, 6);
    memcpy(buf + 8, uid4, 4);
    // communicate() only traces Transceive frames, so the key never lands in the trace.
    Status s = communicate(MFAuthent, kIdleIRq, buf, sizeof(buf), nullptr, nullptr, nullptr, 0, false);
    memset(buf, 0, sizeof(buf));
    if (s == Status::Ok && !crypto1On()) s = Status::Nak;
    if (trace_ && traceOn_) {
      const uint8_t head[2] = {command, block};
      trace_->add(s == Status::Ok ? TraceFrame::Kind::Auth : TraceFrame::Kind::AuthFail, false, head, 2, 0, false);
    }
    return s;
  }

  // Send `send` and cut the field `cutAfterUs` microseconds after its last bit left the antenna.
  // The edge lab's tear test uses it to interrupt a card in the middle of writing its memory.
  void transmitThenCut(const uint8_t *send, uint8_t sendLen, uint32_t cutAfterUs) {
    write(CommandReg, Idle);
    write(ComIrqReg, 0x7F);
    write(FIFOLevelReg, 0x80);
    writeMany(FIFODataReg, send, sendLen);
    write(BitFramingReg, 0x00);
    if (trace_ && traceOn_) trace_->add(TraceFrame::Kind::Frame, false, send, sendLen, 0, crypto1On());
    write(CommandReg, Transceive);
    setBits(BitFramingReg, 0x80);
    const int64_t deadline = esp_timer_get_time() + 20'000;
    while (!(read(ComIrqReg) & kTxIRq) && esp_timer_get_time() < deadline) {
    }
    const int64_t sent = esp_timer_get_time();
    while (esp_timer_get_time() - sent < int64_t(cutAfterUs)) {
    }
    antenna(false);
    write(CommandReg, Idle);
  }

  // True once if a command finished while the IRQ wire stayed quiet (then cleared).
  bool takeIrqMissed() {
    const bool m = irqMissed_;
    irqMissed_ = false;
    return m;
  }

  // A dip in its 3.3 V resets the chip to its defaults without a word: VersionReg still answers,
  // but the timer prescaler is back to 00h and the antenna drivers are off, so no card is ever
  // found. True when that happened; restoreSettings() puts our configuration back.
  bool settingsLost() {
    return read(TPrescalerReg) != 0xA9 || (read(TxControlReg) & 0x03) != (antennaOn_ ? 0x03 : 0x00);
  }
  void restoreSettings() { configure(); }

  bool traceOn() const { return traceOn_; }
  void setTraceOn(bool on) { traceOn_ = on; }

 private:
  void configure() {
    write(TxModeReg, 0x00);      // 106 kBd, no CRC added by hardware (we add it ourselves)
    write(RxModeReg, 0x00);
    write(ModWidthReg, 0x26);    // modulation pulse width (reset value)
    write(TModeReg, 0x80);       // TAuto: the timer starts when a frame has been sent
    write(TPrescalerReg, 0xA9);  // 13.56 MHz / (2 x 169 + 1) = 40 kHz: one tick = 25 us
    setTimeoutUs(timeoutUs_);
    write(TxASKReg, 0x40);       // Force100ASK: the 100 % ASK pauses ISO 14443-A wants
    write(ModeReg, 0x3D);        // CRC preset 6363h, like CRC_A
    setGain(gain_);
    setWaitMode(mode_);
    antenna(true);
  }

  Status communicate(uint8_t command, uint8_t waitIrq, const uint8_t *send, uint8_t sendLen, uint8_t *back,
                     uint8_t *backLen, uint8_t *validBits, uint8_t rxAlign, bool checkCrc) {
    const uint8_t txLastBits = validBits ? *validBits : 0;
    const bool crypto = command == Transceive && crypto1On();
    stats_.commands++;

    write(CommandReg, Idle);           // stop anything still running
    write(ComIrqReg, 0x7F);            // clear every interrupt bit
    write(FIFOLevelReg, 0x80);         // flush the FIFO
    writeMany(FIFODataReg, send, sendLen);
    write(BitFramingReg, uint8_t((rxAlign << 4) | txLastBits));
    if (mode_ == WaitMode::Irq) {
      write(ComIEnReg, uint8_t(0x80 | waitIrq | kTimerIRq));  // let these reach the IRQ pin
      xSemaphoreTake(done_, 0);                                // forget an old wake-up
    }
    if (trace_ && traceOn_ && command == Transceive)
      trace_->add(TraceFrame::Kind::Frame, false, send, sendLen, txLastBits, crypto);

    const int64_t t0 = esp_timer_get_time();
    write(CommandReg, command);
    if (command == Transceive) setBits(BitFramingReg, 0x80);  // StartSend

    uint8_t irq = 0;
    Status s = wait(waitIrq, irq);
    stats_.lastUs = uint32_t(esp_timer_get_time() - t0);
    if (mode_ == WaitMode::Irq) write(ComIEnReg, 0x80);
    if (s != Status::Ok) {
      write(CommandReg, Idle);
      if (s == Status::NoAnswer && trace_ && traceOn_ && command == Transceive)
        trace_->add(TraceFrame::Kind::NoAnswer, true, nullptr, 0, 0, crypto);
      return s;
    }

    const uint8_t err = read(ErrorReg);
    if (err & 0x10) return Status::Overflow;
    if (err & 0x03) return Status::ProtocolErr;  // ParityErr or ProtocolErr

    uint8_t gotBits = 0;
    if (back && backLen) {
      const uint8_t n = read(FIFOLevelReg) & 0x7F;
      if (n > *backLen) return Status::NoRoom;
      *backLen = n;
      readMany(FIFODataReg, back, n, rxAlign);
      gotBits = read(ControlReg) & 0x07;  // RxLastBits
      if (validBits) *validBits = gotBits;
      if (trace_ && traceOn_ && command == Transceive)
        trace_->add(TraceFrame::Kind::Frame, true, back, n, gotBits, crypto);
    }
    if (err & 0x08) return Status::Collision;

    if (back && backLen && checkCrc) {
      if (*backLen == 1 && gotBits == 4) return Status::Nak;  // MIFARE 4-bit NAK
      if (*backLen < 2 || gotBits != 0) return Status::CrcWrong;
      if (rfid::crcA(back, *backLen - 2) != uint16_t(back[*backLen - 2] | (back[*backLen - 1] << 8))) return Status::CrcWrong;
    }
    return Status::Ok;
  }

  // Wait until one of `waitIrq` is set, the chip's timer runs out, or the chip itself goes quiet.
  Status wait(uint8_t waitIrq, uint8_t &irq) {
    // The chip's timer covers the card; this software deadline covers the chip (a loose wire).
    const int64_t start = esp_timer_get_time();
    const int64_t deadline = start + timeoutUs_ + 40'000;
    if (mode_ == WaitMode::Irq) {
      for (;;) {
        const int64_t left = deadline - esp_timer_get_time();
        const bool woke = left > 0 && xSemaphoreTake(done_, pdMS_TO_TICKS(left / 1000 + 1)) == pdTRUE;
        irq = read(ComIrqReg);
        if (woke) stats_.irqWakes++;
        // Finished without a wake-up: the IRQ wire is missing. The answer is still good; the
        // caller sees irqMissed() and can fall back to Poll.
        if (!woke && (irq & (waitIrq | kTimerIRq))) stats_.irqMissing++, irqMissed_ = true;
        if (irq & waitIrq) return Status::Ok;
        if (irq & kTimerIRq) return Status::NoAnswer;
        if (!woke) return Status::ChipSilent;
        // Woken by an old edge: wait again.
      }
    }
    // Poll: spin for the first 2 ms (most answers come within 1 ms), then check once per tick so
    // the rest of the system can run while we wait out a long timeout.
    for (;;) {
      irq = read(ComIrqReg);
      stats_.polls++;
      if (irq & waitIrq) return Status::Ok;
      if (irq & kTimerIRq) return Status::NoAnswer;
      const int64_t now = esp_timer_get_time();
      if (now > deadline) return Status::ChipSilent;
      if (now - start > 2'000) vTaskDelay(1);
    }
  }

  void select() {
    spi_->beginTransaction(SPISettings(hz_, MSBFIRST, SPI_MODE0));  // section 8.1.2: mode 0
    digitalWrite(pins_.ss, LOW);
  }
  void deselect(size_t bytes) {
    digitalWrite(pins_.ss, HIGH);
    spi_->endTransaction();
    stats_.spiOps++;
    stats_.spiBytes += bytes;
  }

  static void IRAM_ATTR onIrq(void *self) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<Mfrc522 *>(self)->done_, &woken);
    if (woken) portYIELD_FROM_ISR();
  }

  SPIClass *spi_ = nullptr;
  Pins pins_{};
  Trace *trace_ = nullptr;
  SemaphoreHandle_t done_ = nullptr;
  uint32_t hz_ = kDefaultHz;
  uint32_t timeoutUs_ = kDefaultTimeoutUs;
  uint8_t gain_ = 4;  // 33 dB, the reset value
  WaitMode mode_ = WaitMode::Poll;
  bool antennaOn_ = false;
  bool traceOn_ = true;
  bool irqMissed_ = false;
  Stats stats_;
};

// What VersionReg says (section 9.3.4.8), plus the clones sold on cheap RC522 boards.
inline const char *versionName(uint8_t v) {
  switch (v) {
    case 0x91: return "MFRC522 v1.0";
    case 0x92: return "MFRC522 v2.0";
    case 0x88: return "FM17522 (clone)";
    case 0x89: return "FM17522E (clone)";
    case 0xB2: return "FM17522 (clone)";
    case 0x12: return "counterfeit MFRC522";
    case 0x00: return "nothing (MISO stuck low?)";
    case 0xFF: return "nothing (MISO floating?)";
    default: return "unknown chip";
  }
}

}  // namespace rc522
