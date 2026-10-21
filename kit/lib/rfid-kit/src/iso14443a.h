// ISO/IEC 14443-3 type A: how a reader finds cards and picks one, built on mfrc522.h.
//
//   IDLE --REQA--> READY --anticollision + SELECT--> ACTIVE --(MIFARE auth)--> working
//     ^              (WUPA also wakes HALT)               |
//     +----------------------- HLTA ------------------> HALT (only WUPA wakes it)
//
// Anticollision: every card in the field answers at the same time, bit by bit. Where two cards
// send different bits the reader sees a collision, picks one value (we pick 1), and asks again
// with that prefix: only the cards that match keep answering. A binary-tree walk down the UIDs.
#pragma once

#include "mfrc522.h"

namespace iso14443a {

using rc522::Mfrc522;
using rc522::Status;

constexpr uint8_t REQA = 0x26, WUPA = 0x52, SEL_CL1 = 0x93, SEL_CL2 = 0x95, SEL_CL3 = 0x97, CT = 0x88;

struct Uid {
  uint8_t bytes[10] = {};
  uint8_t size = 0;  // 4, 7 or 10
  uint8_t sak = 0;
  uint8_t atqa[2] = {};

  bool sameAs(const Uid &o) const { return size == o.size && size && memcmp(bytes, o.bytes, size) == 0; }
  // The 4 bytes MIFARE authentication uses: the last 4 of the UID.
  const uint8_t *last4() const { return bytes + (size >= 4 ? size - 4 : 0); }
  // "04:A3:2B:91" style, into out (at least 31 bytes).
  void format(char *out, size_t n, char sep = ':') const {
    size_t p = 0;
    out[0] = 0;
    for (uint8_t i = 0; i < size && p + 4 < n; i++) {
      if (i && sep) out[p++] = sep;
      p += snprintf(out + p, n - p, "%02X", bytes[i]);
    }
  }
};

// One collision the walk resolved: at which cascade level and bit (1-32).
struct Collision {
  uint8_t level, bit;
};
struct Collisions {
  static constexpr int kMax = 8;
  Collision at[kMax];
  int n = 0;
  void add(uint8_t level, uint8_t bit) {
    if (n < kMax) at[n++] = {level, bit};
  }
};

// REQA (idle cards) or WUPA (idle and halted cards): a 7-bit short frame. Answer: 2-byte ATQA.
// Two cards with different ATQAs collide here too; that still means "cards present".
inline Status request(Mfrc522 &chip, uint8_t cmd, uint8_t atqa[2]) {
  chip.clearBits(rc522::CollReg, 0x80);  // ValuesAfterColl = 0: bits after a collision read as 0
  uint8_t len = 2, bits = 7;
  const Status s = chip.transceive(&cmd, 1, atqa, &len, &bits);
  if (s == Status::Collision) return s;
  if (s != Status::Ok) return s;
  if (len != 2 || bits != 0) return Status::ProtocolErr;
  return Status::Ok;
}

// Anticollision + SELECT through every cascade level, until the card says its UID is complete.
// uid.atqa is kept; size, bytes and sak are filled in.
inline Status select(Mfrc522 &chip, Uid &uid, Collisions *collisions = nullptr) {
  chip.clearBits(rc522::CollReg, 0x80);
  uint8_t level = 1;
  for (;;) {
    const uint8_t uidIndex = (level - 1) * 3;
    uint8_t buf[9] = {};  // SEL, NVB, 4 UID bytes (or CT + 3), BCC, CRC_A x2
    buf[0] = level == 1 ? SEL_CL1 : level == 2 ? SEL_CL2 : SEL_CL3;
    int known = 0;  // UID bits of this level we already know
    uint8_t *resp = nullptr;
    uint8_t respLen = 0, bits = 0;
    for (bool selected = false; !selected;) {
      uint8_t sendLen, txLastBits;
      if (known >= 32) {
        // SELECT: all 32 bits known. NVB 0x70 = 7 whole bytes follow the SEL byte's count.
        buf[1] = 0x70;
        buf[6] = rfid::bcc(&buf[2]);
        rfid::appendCrcA(buf, 7);
        txLastBits = 0;
        sendLen = 9;
        resp = &buf[6];  // SAK + CRC_A lands here
        respLen = 3;
      } else {
        // ANTICOLLISION: send what we know; NVB = bytes (high nibble) and extra bits (low nibble).
        txLastBits = known % 8;
        const uint8_t index = 2 + known / 8;
        buf[1] = uint8_t((index << 4) | txLastBits);
        sendLen = index + (txLastBits ? 1 : 0);
        resp = &buf[index];
        respLen = sizeof(buf) - index;
      }
      bits = txLastBits;
      const Status s = chip.transceive(buf, sendLen, resp, &respLen, &bits, txLastBits);
      if (s == Status::Collision) {
        const uint8_t coll = chip.read(rc522::CollReg);
        if (coll & 0x20) return Status::Collision;  // CollPosNotValid
        int pos = coll & 0x1F;
        if (pos == 0) pos = 32;
        if (pos <= known) return Status::ProtocolErr;
        if (collisions) collisions->add(level, uint8_t(pos));
        // Bits before pos arrived intact (and those after read as 0). Choose 1 at pos, ask again.
        known = pos;
        const int bitInByte = (known - 1) % 8;
        const int byteIndex = 1 + known / 8 + (known % 8 ? 1 : 0);
        buf[byteIndex] |= uint8_t(1 << bitInByte);
      } else if (s != Status::Ok) {
        return s;
      } else if (known >= 32) {
        selected = true;
      } else {
        // The whole UID part arrived: check its BCC before selecting.
        if (buf[6] != rfid::bcc(&buf[2])) return Status::BadBcc;
        known = 32;
      }
    }
    // Copy this level's UID bytes. CT (0x88) first means "more bytes at the next level".
    const bool cascade = buf[2] == CT;
    memcpy(&uid.bytes[uidIndex], &buf[cascade ? 3 : 2], cascade ? 3 : 4);
    if (respLen != 3 || bits != 0) return Status::ProtocolErr;
    if (rfid::crcA(resp, 1) != uint16_t(resp[1] | (resp[2] << 8))) return Status::CrcWrong;
    if (resp[0] & 0x04) {  // SAK bit 3: UID not complete
      if (++level > 3) return Status::ProtocolErr;
      continue;
    }
    uid.sak = resp[0];
    uid.size = uint8_t(3 * level + 1);
    return Status::Ok;
  }
}

// HLTA: the selected card goes to HALT. It answers no command except WUPA. No answer = success.
// ISO 14443-3: an answer within 1 ms would mean "not halted", so we only wait 1 ms, not 25.
inline void halt(Mfrc522 &chip) {
  uint8_t cmd[4] = {0x50, 0x00};
  rfid::appendCrcA(cmd, 2);
  const uint32_t keep = chip.timeoutUs();
  chip.setTimeoutUs(1000);
  chip.transceive(cmd, 4, nullptr, nullptr);
  chip.setTimeoutUs(keep);
}

// Wake every card (WUPA) and select one: what the lab does before any card operation.
inline Status wakeAndSelect(Mfrc522 &chip, Uid &uid, Collisions *collisions = nullptr) {
  chip.stopCrypto1();
  Status s = request(chip, WUPA, uid.atqa);
  if (s != Status::Ok && s != Status::Collision) return s;
  return select(chip, uid, collisions);
}

// ---- Naming frames for the sniffer -------------------------------------------------------------

// What a frame from the reader is. `lastBits` 7 means a short frame.
inline const char *nameReaderFrame(const uint8_t *d, uint8_t len, uint8_t lastBits) {
  if (len == 0) return "";
  if (len == 1 && lastBits == 7) return d[0] == REQA ? "REQA" : d[0] == WUPA ? "WUPA" : "short frame";
  switch (d[0]) {
    case SEL_CL1: case SEL_CL2: case SEL_CL3:
      if (len >= 2 && d[1] == 0x70) return d[0] == SEL_CL1 ? "SELECT CL1" : d[0] == SEL_CL2 ? "SELECT CL2" : "SELECT CL3";
      return d[0] == SEL_CL1 ? "ANTICOLL CL1" : d[0] == SEL_CL2 ? "ANTICOLL CL2" : "ANTICOLL CL3";
    case 0x50: return "HLTA";
    case 0x60: return "AUTH key A";
    case 0x61: return "AUTH key B";
    case 0x30: return "READ";
    case 0xA0: return "WRITE";
    case 0xC0: return "DECREMENT";
    case 0xC1: return "INCREMENT";
    case 0xC2: return "RESTORE";
    case 0xB0: return "TRANSFER";
    default: return len == 16 + 2 ? "block data" : len == 4 + 2 ? "value operand" : "";
  }
}

// What a card's answer is, given what the reader sent just before.
inline const char *nameCardFrame(const uint8_t *prev, uint8_t prevLen, uint8_t prevBits, uint8_t len, uint8_t lastBits) {
  if (len == 1 && lastBits == 4) return "ACK/NAK";
  if (prevLen == 1 && prevBits == 7) return "ATQA";
  if (prevLen >= 2 && (prev[0] == SEL_CL1 || prev[0] == SEL_CL2 || prev[0] == SEL_CL3))
    return prev[1] == 0x70 ? "SAK" : "UID + BCC";
  if (prevLen >= 1 && prev[0] == 0x30) return "16 bytes + CRC";
  return "";
}

}  // namespace iso14443a
