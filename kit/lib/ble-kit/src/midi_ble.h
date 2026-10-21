// MIDI over Bluetooth LE, the packet format. No Arduino and no radio here, so
// kit/test/host/ble_midi_test.cpp checks it on the Mac.
//
// Plain MIDI is a stream of 1-3 byte messages: status (what, which channel), then data
// (90 3C 64 = note on, channel 1, middle C, velocity 100). BLE-MIDI wraps them in one
// characteristic write or notification per connection event, with millisecond timestamps so
// the receiver can play them with the original timing even though the radio delivers in bursts:
//
//   [header][ts][status][data..] [ts][status][data..] [ts][data..] ...
//    1 0 hhhhhh   1 lllllll
//
// The header carries the top 6 bits of a 13-bit millisecond clock, each timestamp byte the low 7.
// When a low part is smaller than the one before it, the clock wrapped past 127: add 128.
// Running status: a message with the same status as the one before may leave its status byte
// out ([ts][data..]), and may even leave the timestamp out too ([data..], same time).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace midi {

struct Msg {
  uint8_t status = 0;  // 0x90 = note on, channel 1 (0x9F = channel 16)
  uint8_t d1 = 0, d2 = 0;
  uint16_t ts = 0;     // 13-bit milliseconds, wraps every 8.192 s
};

inline Msg noteOn(uint8_t ch, uint8_t note, uint8_t vel, uint16_t ts) { return {uint8_t(0x90 | (ch & 15)), uint8_t(note & 127), uint8_t(vel & 127), uint16_t(ts & 0x1FFF)}; }
inline Msg noteOff(uint8_t ch, uint8_t note, uint16_t ts) { return {uint8_t(0x80 | (ch & 15)), uint8_t(note & 127), 0, uint16_t(ts & 0x1FFF)}; }
// "All notes off" is a control change: CC 123 on the channel.
inline Msg allNotesOff(uint8_t ch, uint16_t ts) { return {uint8_t(0xB0 | (ch & 15)), 123, 0, uint16_t(ts & 0x1FFF)}; }

inline bool isNoteOn(const Msg &m) { return (m.status & 0xF0) == 0x90 && m.d2 > 0; }
// Many keyboards end a note with "note on, velocity 0" instead of a note off.
inline bool isNoteOff(const Msg &m) { return (m.status & 0xF0) == 0x80 || ((m.status & 0xF0) == 0x90 && m.d2 == 0); }

// How many data bytes follow a status byte. -1 = SysEx or undefined.
inline int dataBytes(uint8_t status) {
  if (status < 0x80) return -1;
  if (status < 0xF0) {
    const uint8_t kind = status & 0xF0;
    return kind == 0xC0 || kind == 0xD0 ? 1 : 2;  // program change, channel pressure: 1
  }
  switch (status) {
    case 0xF1: case 0xF3: return 1;
    case 0xF2: return 2;
    case 0xF6: return 0;
    default: return status >= 0xF8 ? 0 : -1;  // real-time: 0, F0/F4/F5/F7: no
  }
}

// ---- Encode -----------------------------------------------------------------------------------

// Pack messages into one packet. Timestamps must not go backwards. The receiver only notices a
// wrap of the low 7 bits, so a gap of more than 127 ms between two messages can't be told apart
// from a short one: we stop there and the next packet carries the rest with a fresh header.
// *encoded = messages packed. Returns the packet length (0 if not even the first fits).
inline size_t encode(const Msg *m, size_t n, uint8_t *out, size_t cap, bool runningStatus, size_t *encoded) {
  size_t len = 0, done = 0;
  uint8_t last = 0;
  uint16_t prevTs = 0;
  for (; done < n; done++) {
    const Msg &x = m[done];
    const int data = dataBytes(x.status);
    if (data < 0) break;  // no SysEx through here
    if (done == 0) {
      if (cap < 1) break;
      out[len++] = uint8_t(0x80 | ((x.ts >> 7) & 0x3F));
    } else if (uint16_t((x.ts - prevTs) & 0x1FFF) > 127) {
      break;  // the next packet carries it, with a fresh header
    }
    const bool skipStatus = runningStatus && x.status == last && x.status < 0xF0;
    const size_t need = 1 + (skipStatus ? 0 : 1) + size_t(data);
    if (len + need > cap) break;
    out[len++] = uint8_t(0x80 | (x.ts & 0x7F));
    if (!skipStatus) out[len++] = x.status;
    if (data >= 1) out[len++] = x.d1 & 0x7F;
    if (data >= 2) out[len++] = x.d2 & 0x7F;
    if (x.status < 0xF0) last = x.status;
    else if (x.status < 0xF8) last = 0;  // system common cancels running status
    prevTs = x.ts;
  }
  if (encoded) *encoded = done;
  return done ? len : 0;
}

// ---- Decode -----------------------------------------------------------------------------------

// Keeps running status and "inside a SysEx" between packets: a long SysEx spans several.
struct Decoder {
  uint8_t running = 0;
  bool sysex = false;
  uint32_t sysexSkipped = 0;  // bytes thrown away, for the curious

  // Up to max messages into out. Returns how many, or -1 for a packet without a header.
  int decode(const uint8_t *p, size_t n, Msg *out, int max) {
    if (n < 1 || (p[0] & 0xC0) != 0x80) return -1;
    uint8_t hi = p[0] & 0x3F;
    int lastLow = -1;
    uint16_t ts = uint16_t(hi << 7);
    int count = 0;
    size_t i = 1;
    auto stamp = [&](uint8_t b) {
      const int low = b & 0x7F;
      if (lastLow >= 0 && low < lastLow) hi = uint8_t((hi + 1) & 0x3F);
      lastLow = low;
      ts = uint16_t((hi << 7) | low);
    };
    auto emit = [&](uint8_t status, uint8_t d1, uint8_t d2) {
      if (count < max) out[count++] = Msg{status, d1, d2, ts};
    };
    while (i < n) {
      uint8_t b = p[i];
      if (sysex) {
        // Inside a SysEx every data byte is skipped. A byte with the top bit set is a timestamp,
        // followed by F7 (the end) or a real-time message squeezed in between.
        if ((b & 0x80) && i + 1 < n && (p[i + 1] == 0xF7 || p[i + 1] >= 0xF8)) {
          stamp(b);
          if (p[i + 1] == 0xF7) sysex = false;
          else emit(p[i + 1], 0, 0);
          i += 2;
        } else {
          sysexSkipped++;
          i++;
        }
        continue;
      }
      uint8_t status;
      if (b & 0x80) {
        stamp(b);  // a timestamp byte
        if (++i >= n) break;
        b = p[i];
        if (b & 0x80) {
          if (b == 0xF0) {
            sysex = true;
            running = 0;
            i++;
            continue;
          }
          if (b >= 0xF8) {  // real-time: one byte, leaves running status alone
            emit(b, 0, 0);
            i++;
            continue;
          }
          status = b;
          running = b < 0xF0 ? b : 0;
          i++;
        } else {
          status = running;  // [ts][data..]
        }
      } else {
        status = running;  // [data..]: running status, same time
      }
      const int data = dataBytes(status);
      if (status == 0 || data < 0) {  // data with nothing to belong to: skip it
        i++;
        continue;
      }
      if (i + size_t(data) > n) break;  // cut off
      uint8_t d[2] = {0, 0};
      bool bad = false;
      for (int k = 0; k < data; k++) {
        d[k] = p[i + k];
        if (d[k] & 0x80) bad = true;
      }
      if (bad) break;  // a status byte where data should be: give up on this packet
      emit(status, d[0], d[1]);
      i += size_t(data);
    }
    return count;
  }
};

// ---- Names ------------------------------------------------------------------------------------

// 60 -> "C4" (MIDI note 60 is middle C, octave 4).
inline void noteName(uint8_t note, char *out, size_t n) {
  static const char *const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (n < 5) return;
  const int octave = note / 12 - 1;
  const char *nm = names[note % 12];
  size_t i = 0;
  while (*nm && i + 1 < n) out[i++] = *nm++;
  if (octave < 0 && i + 1 < n) out[i++] = '-';
  if (i + 1 < n) out[i++] = char('0' + (octave < 0 ? -octave : octave));
  out[i] = 0;
}

// "C4", "f#3", "Bb2" -> MIDI note; -1 if it isn't one.
inline int parseNote(const char *s) {
  static const int base[] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
  char c = s[0];
  if (c >= 'a' && c <= 'g') c = char(c - 'a' + 'A');
  if (c < 'A' || c > 'G') return -1;
  int note = base[c - 'A'];
  int i = 1;
  if (s[i] == '#') note++, i++;
  else if (s[i] == 'b') note--, i++;
  bool neg = false;
  if (s[i] == '-') neg = true, i++;
  if (s[i] < '0' || s[i] > '9' || s[i + 1] != 0) return -1;
  const int octave = neg ? -(s[i] - '0') : s[i] - '0';
  const int midi = (octave + 1) * 12 + note;
  return midi >= 0 && midi <= 127 ? midi : -1;
}

}  // namespace midi
