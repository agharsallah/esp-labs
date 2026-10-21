// Pure byte formats of the RFID labs: no hardware, no Arduino, so test/rfid_host_test.cpp can check
// them on the Mac. Everything that talks to the chip lives in the other files.
//
//   crcA()          ISO/IEC 14443-3 CRC_A, the 2-byte checksum after most card frames
//   bcc()           the 1-byte check after each UID part during anticollision
//   AccessBits      a MIFARE Classic sector trailer's bytes 6-8: who may read/write each block
//   ValueBlock      MIFARE Classic's built-in "wallet" format: value, ~value, value, address x4
#pragma once

#include <stdint.h>
#include <string.h>

namespace rfid {

// CRC_A (ISO/IEC 14443-3, annex B): start at 0x6363, process each byte LSB first. The result is
// sent low byte first. The MFRC522 has the same calculation in hardware (CalcCRC command); the
// edge lab compares the two.
inline uint16_t crcA(const uint8_t *data, size_t n) {
  uint16_t crc = 0x6363;
  for (size_t i = 0; i < n; i++) {
    uint8_t ch = data[i] ^ uint8_t(crc & 0xFF);
    ch ^= uint8_t(ch << 4);
    crc = uint16_t((crc >> 8) ^ (uint16_t(ch) << 8) ^ (uint16_t(ch) << 3) ^ (ch >> 4));
  }
  return crc;
}

inline void appendCrcA(uint8_t *frame, size_t n) {
  const uint16_t c = crcA(frame, n);
  frame[n] = uint8_t(c & 0xFF);
  frame[n + 1] = uint8_t(c >> 8);
}

// Block Check Character: XOR of the 4 UID bytes of one cascade level.
inline uint8_t bcc(const uint8_t *four) { return four[0] ^ four[1] ^ four[2] ^ four[3]; }

// ---- MIFARE Classic memory geometry ----------------------------------------------------------
// 1K: 16 sectors x 4 blocks x 16 bytes. Block 0 = manufacturer data (UID, read-only). The last
// block of every sector is its trailer: key A (6) · access bits (3) · user byte (1) · key B (6).

constexpr int kBlockSize = 16;
inline bool isTrailer(int block) { return block < 128 ? (block % 4) == 3 : (block % 16) == 15; }
inline int sectorOf(int block) { return block < 128 ? block / 4 : 32 + (block - 128) / 16; }
inline int firstBlock(int sector) { return sector < 32 ? sector * 4 : 128 + (sector - 32) * 16; }
inline int blocksIn(int sector) { return sector < 32 ? 4 : 16; }
inline int trailerOf(int sector) { return firstBlock(sector) + blocksIn(sector) - 1; }

// ---- Access bits (MIFARE Classic EV1 datasheet MF1S50yyX, section 8.7) ------------------------
// Each block b (0-2 data, 3 = trailer) has three bits C1 C2 C3. They are stored twice, once
// inverted, so a half-written trailer is detectable:
//   byte 6 = ~C2 (high nibble) | ~C1 (low nibble)
//   byte 7 =  C1 (high nibble) | ~C3 (low nibble)
//   byte 8 =  C3 (high nibble) |  C2 (low nibble)
// Bit b of each nibble belongs to block b. The default trailer FF 07 80 gives data blocks 000
// ("anything with key A or B") and the trailer 001 ("key A can do everything").
struct AccessBits {
  uint8_t c1 = 0, c2 = 0, c3 = 0;  // low 4 bits used, bit b = block b

  // false when the inverted copies don't match: never write such a trailer, the sector would lock.
  static bool decode(const uint8_t trailer[16], AccessBits &out) {
    const uint8_t b6 = trailer[6], b7 = trailer[7], b8 = trailer[8];
    out.c1 = b7 >> 4;
    out.c2 = b8 & 0x0F;
    out.c3 = b8 >> 4;
    return (b6 & 0x0F) == (~out.c1 & 0x0F) && (b6 >> 4) == (~out.c2 & 0x0F) && (b7 & 0x0F) == (~out.c3 & 0x0F);
  }

  void encode(uint8_t out3[3]) const {
    out3[0] = uint8_t(((~c2 & 0x0F) << 4) | (~c1 & 0x0F));
    out3[1] = uint8_t(((c1 & 0x0F) << 4) | (~c3 & 0x0F));
    out3[2] = uint8_t(((c3 & 0x0F) << 4) | (c2 & 0x0F));
  }

  // The 3-bit condition of block b (0-3), as the number C1C2C3 (0-7).
  uint8_t condition(int b) const {
    return uint8_t((((c1 >> b) & 1) << 2) | (((c2 >> b) & 1) << 1) | ((c3 >> b) & 1));
  }

  // What a data block's condition allows: read / write / increment / decrement+transfer+restore.
  static const char *dataRule(uint8_t c) {
    static const char *const rules[8] = {
        "read AB write AB inc AB dec AB",  // 000  transport configuration
        "read AB dec AB (value, no top-up)",  // 001
        "read AB, read-only",                // 010
        "read B write B",                    // 011
        "read AB write B",                   // 100
        "read B, read-only",                 // 101
        "read AB write B inc B dec AB",      // 110  value block, top-up with key B
        "locked: nothing",                   // 111
    };
    return rules[c & 7];
  }

  static const char *trailerRule(uint8_t c) {
    static const char *const rules[8] = {
        "A writes keys, bits fixed; B readable",   // 000
        "A writes all (transport)",                // 001
        "read-only; B readable",                   // 010
        "B writes keys + bits",                    // 011
        "B writes keys",                           // 100
        "B writes access bits",                    // 101
        "read-only",                               // 110
        "read-only",                               // 111
    };
    return rules[c & 7];
  }

  // Data blocks that may be written with key A (our lab never uses key B).
  bool keyAWrites(int b) const { return condition(b) == 0; }
  // With these trailer conditions key B is readable, so it can't be used as a key at all.
  bool keyBReadable() const {
    const uint8_t t = condition(3);
    return t == 0 || t == 1 || t == 2;
  }
};

// ---- Value blocks ------------------------------------------------------------------------------
// A signed 32-bit value stored three times (once inverted) plus a 1-byte address four times.
// The card checks this format itself before increment / decrement / restore.
struct ValueBlock {
  static void encode(int32_t value, uint8_t addr, uint8_t out[16]) {
    const uint32_t v = uint32_t(value), nv = ~v;
    for (int i = 0; i < 4; i++) {
      out[i] = uint8_t(v >> (8 * i));
      out[4 + i] = uint8_t(nv >> (8 * i));
      out[8 + i] = uint8_t(v >> (8 * i));
    }
    out[12] = addr, out[13] = uint8_t(~addr), out[14] = addr, out[15] = uint8_t(~addr);
  }

  // false when the block is not a valid value block.
  static bool decode(const uint8_t b[16], int32_t &value, uint8_t &addr) {
    uint32_t v = 0, nv = 0, v2 = 0;
    for (int i = 0; i < 4; i++) {
      v |= uint32_t(b[i]) << (8 * i);
      nv |= uint32_t(b[4 + i]) << (8 * i);
      v2 |= uint32_t(b[8 + i]) << (8 * i);
    }
    if (v != v2 || v != ~nv) return false;
    if (b[12] != b[14] || b[13] != b[15] || uint8_t(~b[12]) != b[13]) return false;
    value = int32_t(v);
    addr = b[12];
    return true;
  }
};

// ---- Card identification -----------------------------------------------------------------------

// From SAK, the select acknowledge (NXP AN10833).
inline const char *cardType(uint8_t sak) {
  switch (sak & 0x7F) {
    case 0x09: return "MIFARE Mini";
    case 0x08: return "MIFARE Classic 1K";
    case 0x18: return "MIFARE Classic 4K";
    case 0x00: return "Ultralight / NTAG";
    case 0x10: case 0x11: return "MIFARE Plus";
    case 0x01: return "TNP3xxx";
    case 0x20: return "ISO 14443-4 (DESFire, phone, bank card)";
    case 0x40: return "NFC peer (ISO 18092)";
    default: return (sak & 0x20) ? "ISO 14443-4" : "unknown";
  }
}

// Sectors our lab can read on this card type, 0 = not a MIFARE Classic.
inline int classicSectors(uint8_t sak) {
  switch (sak & 0x7F) {
    case 0x09: return 5;
    case 0x08: case 0x88: return 16;
    case 0x18: return 40;
    default: return 0;
  }
}

// ATQA arrives low byte first. Bits 7-6 of the low byte: UID size; bits 4-0: anticollision.
inline int atqaUidBytes(const uint8_t atqa[2]) {
  switch ((atqa[0] >> 6) & 3) {
    case 0: return 4;
    case 1: return 7;
    case 2: return 10;
    default: return 0;  // reserved
  }
}

}  // namespace rfid
