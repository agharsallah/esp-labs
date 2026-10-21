// R9's vault: the cryptography, the card format and the verdict for one tap.
// Pure C++17 (no Arduino), so kit/test/host/vault_crypto_test.cpp checks every piece on the Mac,
// including the published SHA-256, HMAC and AES test vectors.
//
// R7 and R8 trust the UID: anyone who can copy a UID (or the data blocks) gets in. R9 makes the
// card carry proof that only this board can write, and that goes stale after every tap:
//
//   block 4   header, AES-128 encrypted: "VLT9", counter, key epoch, clearance, active slot, nonce
//   block 5   slot A: a 16-byte HMAC-SHA256 tag over UID + block 0 + header + label
//   block 6   slot B: the same, for the next header (the two take turns, see below)
//   block 8   label, AES-128 encrypted: the callsign + a check byte
//   block 9   the counter again, as a MIFARE value block (R5's format) so any reader can see it
//
// Every key comes from a 256-bit master secret that never leaves the board, mixed with the UID
// and an epoch (HMAC-SHA256 used as a key derivation function). So data copied onto another
// card is worthless there (other UID, other keys), and an old copy of the same card's data is
// caught because the board remembers the counter it wrote last (rollback).
//
// Dual slots against tearing: a tap rewrites three blocks, and a card lifted halfway must not
// lock its owner out. The new tag goes into the slot that is not in use first, then block 4
// switches to it, then block 9. Cut anywhere and the card still holds one valid header + tag.
//
// The order of the checks in evaluateTap() is part of the lesson:
//   1. Gen1a "magic" answer       -> CloneGen1a      (when the clone guard is strict)
//   2. not in the ledger          -> Unprovisioned (blank) or UnknownCard
//   3. quarantined                -> Revoked
//   4. block 0 changed            -> CloneBlock0     (when the clone guard is strict)
//   5. header doesn't decrypt     -> Unprovisioned (wiped) or CryptoForged
//   6. label or tag wrong         -> CryptoForged    (e.g. data copied from another card)
//   7. counter behind the ledger  -> CloneRollback   (an old copy of this card's data)
//   8. counter one ahead          -> RecoveredTear   (the last tap was cut after block 4)
//   9. value block disagrees      -> ValueDesync
//  10. clearance too low          -> InsufficientRole
//  11. otherwise                  -> Verified        (and the board writes the next counter)
//
// Learning code, not a product: the AES and SHA-256 here are small and readable, not hardened
// against side channels, and MIFARE Classic itself stays a weak card. Real systems use cards that
// do the cryptography themselves (e.g. MIFARE DESFire, with AES on the card).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace vault {

// ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------

struct Sha256 {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t data[64];
  uint32_t datalen;

  static constexpr uint32_t kK[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  };

  static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

  void transform() {
    uint32_t m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4)
      m[i] = (uint32_t(data[j]) << 24) | (uint32_t(data[j + 1]) << 16) | (uint32_t(data[j + 2]) << 8) | data[j + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
      const uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
      m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + m[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    state[0] += a, state[1] += b, state[2] += c, state[3] += d;
    state[4] += e, state[5] += f, state[6] += g, state[7] += h;
  }

  void init() {
    datalen = 0;
    bitlen = 0;
    state[0] = 0x6a09e667, state[1] = 0xbb67ae85, state[2] = 0x3c6ef372, state[3] = 0xa54ff53a;
    state[4] = 0x510e527f, state[5] = 0x9b05688c, state[6] = 0x1f83d9ab, state[7] = 0x5be0cd19;
  }

  void update(const uint8_t *in, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      data[datalen++] = in[i];
      if (datalen == 64) {
        transform();
        bitlen += 512;
        datalen = 0;
      }
    }
  }

  void final(uint8_t hash[32]) {
    uint32_t i = datalen;
    data[i++] = 0x80;
    if (datalen < 56) {
      while (i < 56) data[i++] = 0x00;
    } else {
      while (i < 64) data[i++] = 0x00;
      transform();
      memset(data, 0, 56);
    }
    bitlen += uint64_t(datalen) * 8;
    for (int k = 0; k < 8; ++k) data[63 - k] = uint8_t(bitlen >> (k * 8));
    transform();
    for (int k = 0; k < 8; ++k)
      for (int j = 0; j < 4; ++j) hash[k * 4 + j] = uint8_t(state[k] >> (24 - j * 8));
  }

  static void compute(const uint8_t *in, size_t len, uint8_t out[32]) {
    Sha256 s;
    s.init();
    s.update(in, len);
    s.final(out);
  }
};

// ---- HMAC-SHA256 (RFC 2104) -------------------------------------------------------------------

inline void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[32]) {
  uint8_t k[64] = {};
  if (keyLen > 64) Sha256::compute(key, keyLen, k);
  else memcpy(k, key, keyLen);
  uint8_t pad[64], inner[32];
  Sha256 s;
  for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
  s.init();
  s.update(pad, 64);
  s.update(msg, msgLen);
  s.final(inner);
  for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
  s.init();
  s.update(pad, 64);
  s.update(inner, 32);
  s.final(out);
  memset(k, 0, sizeof(k));
  memset(pad, 0, sizeof(pad));
}

// Looks at every byte whatever the result, so the time it takes doesn't say where they differ.
inline bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= uint8_t(a[i] ^ b[i]);
  return diff == 0;
}

// ---- AES-128 (FIPS 197), one block at a time --------------------------------------------------

namespace aes {

inline constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

inline constexpr uint8_t kInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

inline constexpr uint8_t kRcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

inline uint8_t xtime(uint8_t x) { return uint8_t((x << 1) ^ (((x >> 7) & 1) * 0x1b)); }
inline uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1) p ^= a;
    a = xtime(a);
    b >>= 1;
  }
  return p;
}

// The 16-byte key grows into 11 round keys of 16 bytes.
inline void expandKey(const uint8_t key[16], uint8_t rk[176]) {
  memcpy(rk, key, 16);
  uint8_t t[4];
  for (int i = 4; i < 44; ++i) {
    for (int k = 0; k < 4; ++k) t[k] = rk[(i - 1) * 4 + k];
    if (i % 4 == 0) {
      const uint8_t first = t[0];
      t[0] = kSbox[t[1]] ^ kRcon[i / 4], t[1] = kSbox[t[2]], t[2] = kSbox[t[3]], t[3] = kSbox[first];
    }
    for (int k = 0; k < 4; ++k) rk[i * 4 + k] = rk[(i - 4) * 4 + k] ^ t[k];
  }
}

inline void addRoundKey(uint8_t s[16], const uint8_t *rk) {
  for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}

// The state is 4 columns of 4 bytes: s[0..3] is column 0. Row r is s[r], s[r+4], s[r+8], s[r+12].
inline void encryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  uint8_t rk[176], s[16];
  expandKey(key, rk);
  memcpy(s, in, 16);
  addRoundKey(s, rk);
  for (int round = 1; round <= 10; ++round) {
    for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
    uint8_t t = s[1];  // ShiftRows: row r moves r places to the left
    s[1] = s[5], s[5] = s[9], s[9] = s[13], s[13] = t;
    t = s[2], s[2] = s[10], s[10] = t, t = s[6], s[6] = s[14], s[14] = t;
    t = s[15], s[15] = s[11], s[11] = s[7], s[7] = s[3], s[3] = t;
    if (round < 10) {
      for (int c = 0; c < 16; c += 4) {  // MixColumns
        const uint8_t a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3], all = a0 ^ a1 ^ a2 ^ a3;
        s[c] ^= all ^ xtime(a0 ^ a1), s[c + 1] ^= all ^ xtime(a1 ^ a2);
        s[c + 2] ^= all ^ xtime(a2 ^ a3), s[c + 3] ^= all ^ xtime(a3 ^ a0);
      }
    }
    addRoundKey(s, rk + round * 16);
  }
  memcpy(out, s, 16);
  memset(rk, 0, sizeof(rk));
}

inline void decryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  uint8_t rk[176], s[16];
  expandKey(key, rk);
  memcpy(s, in, 16);
  addRoundKey(s, rk + 160);
  for (int round = 9; round >= 0; --round) {
    uint8_t t = s[13];  // InvShiftRows: row r moves r places to the right
    s[13] = s[9], s[9] = s[5], s[5] = s[1], s[1] = t;
    t = s[2], s[2] = s[10], s[10] = t, t = s[6], s[6] = s[14], s[14] = t;
    t = s[3], s[3] = s[7], s[7] = s[11], s[11] = s[15], s[15] = t;
    for (int i = 0; i < 16; ++i) s[i] = kInvSbox[s[i]];
    addRoundKey(s, rk + round * 16);
    if (round > 0) {
      for (int c = 0; c < 16; c += 4) {  // InvMixColumns
        const uint8_t a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
        s[c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
        s[c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
        s[c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
        s[c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
      }
    }
  }
  memcpy(out, s, 16);
  memset(rk, 0, sizeof(rk));
}

// One 16-byte block in CBC mode: XOR with the IV, then encrypt. Each block on the card has its
// own key-derived IV, and the header carries a fresh random nonce, so equal data never looks equal.
inline void encryptCbc16(const uint8_t key[16], const uint8_t iv[16], const uint8_t plain[16], uint8_t cipher[16]) {
  uint8_t x[16];
  for (int i = 0; i < 16; ++i) x[i] = plain[i] ^ iv[i];
  encryptBlock(key, x, cipher);
}

inline void decryptCbc16(const uint8_t key[16], const uint8_t iv[16], const uint8_t cipher[16], uint8_t plain[16]) {
  decryptBlock(key, cipher, plain);
  for (int i = 0; i < 16; ++i) plain[i] ^= iv[i];
}

}  // namespace aes

// ---- Card keys --------------------------------------------------------------------------------

// Stored as a byte: only append.
enum class Clearance : uint8_t { Observer, Operator, SecOps, Chief };
constexpr int kClearances = 4;

inline const char *clearanceName(Clearance c) {
  switch (c) {
    case Clearance::Observer: return "observer";
    case Clearance::Operator: return "operator";
    case Clearance::SecOps: return "secops";
    case Clearance::Chief: return "chief";
  }
  return "?";
}

inline bool clearanceFromName(const char *s, Clearance &out) {
  for (int i = 0; i < kClearances; ++i)
    if (strcmp(s, clearanceName(Clearance(i))) == 0) return out = Clearance(i), true;
  return false;
}

struct DerivedKeys {
  uint8_t aesKey[16];
  uint8_t headerIv[16];
  uint8_t labelIv[16];
  uint8_t macKey[32];
};

// HMAC-SHA256(master, UID || length || epoch || purpose), once per purpose (the "expand" half of
// HKDF, RFC 5869). Same master, other UID or other epoch: unrelated keys.
inline void deriveCardKeys(const uint8_t master[32], const uint8_t *uid, uint8_t uidLen, uint16_t epoch, DerivedKeys &out) {
  uint8_t info[17] = {};
  const uint8_t n = uidLen > 10 ? 10 : uidLen;
  memcpy(info, uid, n);
  info[10] = n;
  info[11] = uint8_t(epoch), info[12] = uint8_t(epoch >> 8);
  uint8_t okm[32];
  memcpy(info + 13, "ENC1", 4);
  hmacSha256(master, 32, info, sizeof(info), okm);
  memcpy(out.aesKey, okm, 16);
  memcpy(out.headerIv, okm + 16, 16);
  memcpy(info + 13, "MAC1", 4);
  hmacSha256(master, 32, info, sizeof(info), out.macKey);
  memcpy(info + 13, "LBL1", 4);
  hmacSha256(master, 32, info, sizeof(info), okm);
  memcpy(out.labelIv, okm, 16);
  memset(okm, 0, sizeof(okm));
}

inline void wipe(DerivedKeys &k) {
  volatile uint8_t *p = reinterpret_cast<volatile uint8_t *>(&k);
  for (size_t i = 0; i < sizeof(k); ++i) p[i] = 0;
}

// ---- What the card holds ----------------------------------------------------------------------

constexpr uint8_t kHeaderBlock = 4, kSlotA = 5, kSlotB = 6, kLabelBlock = 8, kValueBlock = 9;
constexpr uint8_t kVaultBlocks[] = {kHeaderBlock, kSlotA, kSlotB, kLabelBlock, kValueBlock};

struct Header {
  uint32_t counter = 0;
  uint16_t epoch = 1;
  Clearance clearance = Clearance::Operator;
  uint8_t slot = 0;  // 0: the tag is in block 5 (slot A), 1: block 6 (slot B)
  uint32_t nonce = 0;

  uint8_t slotBlock() const { return slot ? kSlotB : kSlotA; }

  void pack(uint8_t p[16]) const {
    memcpy(p, "VLT9", 4);
    for (int i = 0; i < 4; ++i) p[4 + i] = uint8_t(counter >> (i * 8));
    p[8] = uint8_t(epoch), p[9] = uint8_t(epoch >> 8);
    p[10] = uint8_t(clearance), p[11] = slot & 1;
    for (int i = 0; i < 4; ++i) p[12 + i] = uint8_t(nonce >> (i * 8));
  }

  static bool unpack(const uint8_t p[16], Header &out) {
    if (memcmp(p, "VLT9", 4) != 0 || p[10] >= kClearances || p[11] > 1) return false;
    out.counter = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
    out.epoch = uint16_t(p[8] | p[9] << 8);
    out.clearance = Clearance(p[10]);
    out.slot = p[11];
    out.nonce = uint32_t(p[12]) | uint32_t(p[13]) << 8 | uint32_t(p[14]) << 16 | uint32_t(p[15]) << 24;
    return true;
  }
};

// The callsign, up to 15 characters, and a check byte.
inline void packLabel(const char *callsign, uint8_t p[16]) {
  memset(p, 0, 16);
  const size_t n = strlen(callsign);
  memcpy(p, callsign, n > 15 ? 15 : n);
  uint8_t chk = 0x5A;
  for (int i = 0; i < 15; ++i) chk ^= p[i];
  p[15] = chk;
}

inline bool unpackLabel(const uint8_t p[16], char out[16]) {
  uint8_t chk = 0x5A;
  for (int i = 0; i < 15; ++i) chk ^= p[i];
  if (p[15] != chk) return false;
  memcpy(out, p, 15);
  out[15] = 0;
  return true;
}

// The tag: HMAC over everything that makes this card this card. Block 0 is in it, so the data
// only fits the chip it was written to.
inline void computeTag(const DerivedKeys &k, const uint8_t *uid, uint8_t uidLen, const uint8_t block0[16], const Header &h,
                       const uint8_t label[16], uint8_t tag[16]) {
  uint8_t msg[11 + 16 + 16 + 16] = {};
  const uint8_t n = uidLen > 10 ? 10 : uidLen;
  memcpy(msg, uid, n);
  msg[10] = n;
  memcpy(msg + 11, block0, 16);
  h.pack(msg + 27);
  memcpy(msg + 43, label, 16);
  uint8_t full[32];
  hmacSha256(k.macKey, 32, msg, sizeof(msg), full);
  memcpy(tag, full, 16);
}

// The blocks to write for one header: block 4, the tag (for header.slotBlock()) and block 8.
struct Prepared {
  uint8_t header[16];
  uint8_t tag[16];
  uint8_t label[16];
};

inline Prepared prepare(const uint8_t master[32], const uint8_t *uid, uint8_t uidLen, const uint8_t block0[16], const Header &h,
                        const char *callsign) {
  Prepared p{};
  DerivedKeys k{};
  deriveCardKeys(master, uid, uidLen, h.epoch, k);
  uint8_t plainHeader[16], plainLabel[16];
  h.pack(plainHeader);
  packLabel(callsign, plainLabel);
  aes::encryptCbc16(k.aesKey, k.headerIv, plainHeader, p.header);
  aes::encryptCbc16(k.aesKey, k.labelIv, plainLabel, p.label);
  computeTag(k, uid, uidLen, block0, h, plainLabel, p.tag);
  wipe(k);
  return p;
}

// ---- Clone forensics --------------------------------------------------------------------------

struct Timing {
  uint16_t wupaUs = 0, selectUs = 0, authUs = 0, readUs = 0;
};

struct Forensics {
  bool gen1a = false;          // answered the Gen1a "magic" wake-up (0x40 after HLTA)
  bool bccOk = true;           // block 0 byte 4 = XOR of the UID bytes (4-byte UIDs)
  bool uidMatches = true;      // block 0 starts with the UID the card announced
  bool sakMatches = true;      // block 0 byte 5 = the SAK the card answered
  bool block0Changed = false;  // block 0 differs from the one saved when it was provisioned
  bool timingDrift = false;    // AUTH or READ took 900+ us more or less than at provisioning
  uint8_t risk = 0;            // 0-100, a sum of the above
};

// Hints, not proof: plenty of genuine cards have odd block 0 bytes, so only the Gen1a answer and
// a changed block 0 can decide a tap (and only with the clone guard on). The rest adds risk.
inline Forensics inspect(const uint8_t *uid, uint8_t uidLen, uint8_t sak, const uint8_t block0[16], const uint8_t *saved,
                         bool gen1a, const Timing &now, const Timing *baseline) {
  Forensics f{};
  int risk = 0;
  f.gen1a = gen1a;
  if (gen1a) risk += 100;
  if (uidLen == 4) {
    f.uidMatches = memcmp(block0, uid, 4) == 0;
    f.bccOk = block0[4] == uint8_t(block0[0] ^ block0[1] ^ block0[2] ^ block0[3]);
    f.sakMatches = (block0[5] & 0x7F) == (sak & 0x7F);
    if (!f.uidMatches || !f.bccOk) risk += 45;
    if (!f.sakMatches) risk += 15;
  }
  if (saved && !constantTimeEqual(block0, saved, 16)) f.block0Changed = true, risk += 85;
  if (baseline && baseline->authUs && now.authUs) {
    const int dAuth = int(now.authUs) - int(baseline->authUs), dRead = int(now.readUs) - int(baseline->readUs);
    if (dAuth > 900 || dAuth < -900 || dRead > 900 || dRead < -900) f.timingDrift = true, risk += 20;
  }
  f.risk = uint8_t(risk > 100 ? 100 : risk);
  return f;
}

// ---- The verdict ------------------------------------------------------------------------------

// Stored in events: only append.
enum class Verdict : uint8_t {
  Verified,
  RecoveredTear,
  Unprovisioned,
  UnknownCard,
  Revoked,
  InsufficientRole,
  CloneGen1a,
  CloneBlock0,
  CloneRollback,
  CryptoForged,
  ValueDesync,
  IoError,
};

inline const char *verdictName(Verdict v) {
  switch (v) {
    case Verdict::Verified: return "VERIFIED";
    case Verdict::RecoveredTear: return "TEAR HEALED";
    case Verdict::Unprovisioned: return "BLANK CARD";
    case Verdict::UnknownCard: return "UNKNOWN CARD";
    case Verdict::Revoked: return "QUARANTINED";
    case Verdict::InsufficientRole: return "LOW CLEARANCE";
    case Verdict::CloneGen1a: return "MAGIC CARD";
    case Verdict::CloneBlock0: return "BLOCK 0 CHANGED";
    case Verdict::CloneRollback: return "ROLLBACK";
    case Verdict::CryptoForged: return "TAG WRONG";
    case Verdict::ValueDesync: return "VALUE TAMPER";
    case Verdict::IoError: return "CARD LIFTED";
  }
  return "?";
}

// Lower case, for JSON and the app.
inline const char *verdictId(Verdict v) {
  switch (v) {
    case Verdict::Verified: return "verified";
    case Verdict::RecoveredTear: return "tear_healed";
    case Verdict::Unprovisioned: return "blank";
    case Verdict::UnknownCard: return "unknown";
    case Verdict::Revoked: return "quarantined";
    case Verdict::InsufficientRole: return "low_clearance";
    case Verdict::CloneGen1a: return "magic_card";
    case Verdict::CloneBlock0: return "block0_changed";
    case Verdict::CloneRollback: return "rollback";
    case Verdict::CryptoForged: return "tag_wrong";
    case Verdict::ValueDesync: return "value_tamper";
    case Verdict::IoError: return "card_lifted";
  }
  return "?";
}

inline bool granted(Verdict v) { return v == Verdict::Verified || v == Verdict::RecoveredTear; }

// The ones that mean someone tampered with a card, not just "not allowed".
inline bool isAlert(Verdict v) {
  return v == Verdict::CloneGen1a || v == Verdict::CloneBlock0 || v == Verdict::CloneRollback || v == Verdict::CryptoForged ||
         v == Verdict::ValueDesync;
}

// The board's record of one card (kept in NVS). Stored as bytes: bump VaultStore::kFormat when it changes.
struct LedgerCard {
  uint8_t uid[10] = {};
  uint8_t uidLen = 0;
  uint8_t sak = 0;
  uint8_t number = 0;  // 1, 2, 3 ... in the order the cards were provisioned
  char callsign[16] = "";
  Clearance clearance = Clearance::Operator;  // the board decides from this, not from the card
  bool quarantined = false;
  uint16_t epoch = 1;
  uint32_t counter = 0;  // the counter the board wrote last
  uint32_t nonce = 0;
  uint8_t block0[16] = {};
  Timing baseline{};
  uint32_t taps = 0, tears = 0, alerts = 0;
};

struct Policy {
  Clearance minClearance = Clearance::Operator;
  bool strictCloneGuard = true;  // a magic card or a changed block 0 is refused (off: only logged)
  bool autoQuarantine = true;    // a clone or rollback quarantines the card it pretends to be
};

// What the reader read from the card on the field.
struct TapInput {
  const uint8_t *uid = nullptr;
  uint8_t uidLen = 0;
  uint8_t sak = 0;
  const uint8_t *block0 = nullptr, *block4 = nullptr, *block5 = nullptr, *block6 = nullptr, *block8 = nullptr;
  bool valueValid = false;
  int32_t value = 0;
  bool gen1a = false;
  Timing timing{};
};

struct Outcome {
  Verdict verdict = Verdict::UnknownCard;
  Header header{};  // what block 4 said (when it decrypted)
  bool headerOk = false;
  char callsign[16] = "";
  Forensics forensics{};
  char detail[72] = "";
};

inline bool allZero(const uint8_t *b) {
  for (int i = 0; i < 16; ++i)
    if (b[i]) return false;
  return true;
}

inline Outcome evaluateTap(const uint8_t master[32], const Policy &policy, const TapInput &in, const LedgerCard *card) {
  Outcome out{};
  out.forensics = inspect(in.uid, in.uidLen, in.sak, in.block0, card ? card->block0 : nullptr, in.gen1a, in.timing,
                          card ? &card->baseline : nullptr);
  auto say = [&](Verdict v, const char *fmt, auto... args) {
    out.verdict = v;
    snprintf(out.detail, sizeof(out.detail), fmt, args...);
    return out;
  };

  if (in.gen1a && policy.strictCloneGuard) return say(Verdict::CloneGen1a, "%s", "answered the Gen1a magic wake-up: a rewritable clone card");
  if (!card) {
    if (allZero(in.block4)) return say(Verdict::Unprovisioned, "%s", "blank card: provision it to put it in the vault");
    return say(Verdict::UnknownCard, "%s", "not in the vault ledger");
  }
  if (card->quarantined) return say(Verdict::Revoked, "%s", "quarantined: rekey it to trust it again");
  if (out.forensics.block0Changed && policy.strictCloneGuard)
    return say(Verdict::CloneBlock0, "%s", "block 0 differs from the one saved at provisioning");

  DerivedKeys k{};
  deriveCardKeys(master, in.uid, in.uidLen, card->epoch, k);
  uint8_t plainHeader[16], plainLabel[16];
  aes::decryptCbc16(k.aesKey, k.headerIv, in.block4, plainHeader);
  aes::decryptCbc16(k.aesKey, k.labelIv, in.block8, plainLabel);
  out.headerOk = Header::unpack(plainHeader, out.header);
  const bool labelOk = unpackLabel(plainLabel, out.callsign);
  uint8_t expected[16] = {};
  if (out.headerOk) computeTag(k, in.uid, in.uidLen, in.block0, out.header, plainLabel, expected);
  wipe(k);

  if (!out.headerOk) {
    if (allZero(in.block4)) return say(Verdict::Unprovisioned, "%s", "vault blocks wiped since it was provisioned");
    return say(Verdict::CryptoForged, "%s", "block 4 doesn't decrypt with this card's key: data from another card?");
  }
  if (!labelOk) return say(Verdict::CryptoForged, "%s", "block 8 doesn't decrypt to a valid label");
  const uint8_t *tag = out.header.slot ? in.block6 : in.block5;
  if (!constantTimeEqual(expected, tag, 16)) return say(Verdict::CryptoForged, "tag in slot %c doesn't match", out.header.slot ? 'B' : 'A');

  const unsigned long have = out.header.counter, want = card->counter;
  if (out.header.counter < card->counter)
    return say(Verdict::CloneRollback, "counter %lu, the board wrote %lu: an old copy of this card", have, want);
  if (out.header.counter > card->counter + 1) return say(Verdict::CloneRollback, "counter %lu is ahead of the board's %lu", have, want);
  const bool tear = out.header.counter == card->counter + 1;
  if (!tear && out.header.nonce != card->nonce) return say(Verdict::CloneRollback, "%s", "same counter, other nonce: not the data the board wrote last");
  // After a tear, block 9 still holds the old counter (or the new one, if only the NVS save failed).
  const bool valueOk = in.valueValid && (uint32_t(in.value) == out.header.counter || (tear && uint32_t(in.value) == card->counter));
  if (!valueOk) return say(Verdict::ValueDesync, "value block %ld, header counter %lu", long(in.valueValid ? in.value : -1), have);
  if (uint8_t(card->clearance) < uint8_t(policy.minClearance))
    return say(Verdict::InsufficientRole, "needs %s, card is %s", clearanceName(policy.minClearance), clearanceName(card->clearance));
  if (tear) return say(Verdict::RecoveredTear, "last tap was cut after block 4: picked up at #%lu", have);
  return say(Verdict::Verified, "#%lu, %s", have, clearanceName(card->clearance));
}

// ---- Hex --------------------------------------------------------------------------------------

inline void toHex(const uint8_t *in, size_t len, char *out, size_t outSize, char sep = 0) {
  size_t p = 0;
  if (!outSize) return;
  out[0] = 0;
  for (size_t i = 0; i < len && p + 3 <= outSize; ++i) {
    if (sep && i && p + 4 <= outSize) out[p++] = sep;
    p += size_t(snprintf(out + p, outSize - p, "%02X", in[i]));
  }
}

inline bool uidFromHex(const char *hex, uint8_t *out, uint8_t &len) {
  const size_t n = strlen(hex);
  if (n != 8 && n != 14 && n != 20) return false;
  for (size_t i = 0; i < n; i += 2) {
    unsigned v = 0;
    if (sscanf(hex + i, "%2x", &v) != 1) return false;
    out[i / 2] = uint8_t(v);
  }
  len = uint8_t(n / 2);
  return true;
}

}  // namespace vault
