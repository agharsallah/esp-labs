// MIFARE Classic memory on top of iso14443a.h: authenticate a sector, read and write 16-byte
// blocks, and use the card's own value-block arithmetic (increment, decrement, restore, transfer).
//
// Guard rails, because a wrong write can lock a sector forever:
//   - block 0 (manufacturer data) and every sector trailer (keys + access bits) are never written
//   - only data blocks whose access bits allow key A writes are written
#pragma once

#include "iso14443a.h"

namespace mifare {

using iso14443a::Uid;
using rc522::Mfrc522;
using rc522::Status;

constexpr uint8_t AUTH_A = 0x60, AUTH_B = 0x61, READ = 0x30, WRITE = 0xA0;
constexpr uint8_t DECREMENT = 0xC0, INCREMENT = 0xC1, RESTORE = 0xC2, TRANSFER = 0xB0;

// The key every new card ships with (key A of every sector). The labs only use cards you own
// that still have it.
constexpr uint8_t kFactoryKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Authenticate the sector that holds `block` with key A.
inline Status authA(Mfrc522 &chip, const Uid &uid, uint8_t block, const uint8_t key[6]) {
  return chip.authenticate(AUTH_A, block, key, uid.last4());
}

// A failed authentication drops the card back to IDLE; wake and select it again before retrying.
inline Status reselect(Mfrc522 &chip, Uid &uid) {
  Uid again = uid;
  const Status s = iso14443a::wakeAndSelect(chip, again);
  if (s == Status::Ok && !again.sameAs(uid)) return Status::Collision;  // a different card answered
  return s;
}

inline Status readBlock(Mfrc522 &chip, uint8_t block, uint8_t out[16]) {
  uint8_t cmd[4] = {READ, block};
  rfid::appendCrcA(cmd, 2);
  uint8_t buf[18], len = sizeof(buf);
  const Status s = chip.transceive(cmd, 4, buf, &len, nullptr, 0, true);
  if (s != Status::Ok) return s;
  if (len != 18) return Status::ProtocolErr;
  memcpy(out, buf, 16);
  return Status::Ok;
}

// MIFARE answers each step with a 4-bit ACK (0xA) or NAK (anything else).
inline Status expectAck(Mfrc522 &chip, const uint8_t *frame, uint8_t len) {
  uint8_t ack[1] = {}, n = 1, bits = 0;
  const Status s = chip.transceive(frame, len, ack, &n, &bits);
  if (s != Status::Ok) return s;
  if (n != 1 || bits != 4) return Status::ProtocolErr;
  return (ack[0] & 0x0F) == 0x0A ? Status::Ok : Status::Nak;
}

// Two steps: "WRITE block" -> ACK, then 16 bytes -> ACK. The card commits all 16 bytes or none.
inline Status writeBlockUnchecked(Mfrc522 &chip, uint8_t block, const uint8_t data[16]) {
  uint8_t cmd[4] = {WRITE, block};
  rfid::appendCrcA(cmd, 2);
  Status s = expectAck(chip, cmd, 4);
  if (s != Status::Ok) return s;
  uint8_t frame[18];
  memcpy(frame, data, 16);
  rfid::appendCrcA(frame, 16);
  // The card writes its EEPROM before it ACKs; the 25 ms timer leaves plenty of room.
  return expectAck(chip, frame, 18);
}

// Refuses block 0 and trailers: those writes are how sectors get locked for good.
inline bool writable(int block) { return block > 0 && block < 256 && !rfid::isTrailer(block); }

inline Status writeBlock(Mfrc522 &chip, uint8_t block, const uint8_t data[16]) {
  if (!writable(block)) return Status::Nak;
  return writeBlockUnchecked(chip, block, data);
}

// Value operations: "OP block" -> ACK, then the 4-byte operand. The card doesn't answer the
// operand: silence means it worked (a "passive ACK"). The result sits in the card's internal
// register until TRANSFER writes it to a block.
inline Status valueOp(Mfrc522 &chip, uint8_t op, uint8_t block, int32_t operand) {
  uint8_t cmd[4] = {op, block};
  rfid::appendCrcA(cmd, 2);
  Status s = expectAck(chip, cmd, 4);
  if (s != Status::Ok) return s;
  uint8_t frame[6];
  for (int i = 0; i < 4; i++) frame[i] = uint8_t(uint32_t(operand) >> (8 * i));
  rfid::appendCrcA(frame, 4);
  const uint32_t keep = chip.timeoutUs();
  chip.setTimeoutUs(5000);
  uint8_t ack[1], n = 1, bits = 0;
  s = chip.transceive(frame, 6, ack, &n, &bits);
  chip.setTimeoutUs(keep);
  if (s == Status::NoAnswer) return Status::Ok;  // passive ACK
  if (s == Status::Ok && n == 1 && bits == 4) return Status::Nak;
  return s;
}

inline Status transfer(Mfrc522 &chip, uint8_t block) {
  if (!writable(block)) return Status::Nak;
  uint8_t cmd[4] = {TRANSFER, block};
  rfid::appendCrcA(cmd, 2);
  return expectAck(chip, cmd, 4);
}

}  // namespace mifare
