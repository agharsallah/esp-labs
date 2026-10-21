// A WAV file: a 44-byte header, then the samples, 16-bit little-endian, mono.
//
//   "RIFF" <size of everything after this> "WAVE"
//   "fmt " 16  format 1 (PCM)  1 channel  <rate>  <bytes per second>  2 (bytes per sample)  16 (bits)
//   "data" <bytes of samples>  ...samples...
//
// The KY-037 has no amplifier, so a voice uses a small part of the ADC's range. `gainFor` finds the
// gain that lifts the clip's loudest sample to about -1 dBFS ("normalising"), applied while sending.
//
// Pure C++: host-tested in kit/test/host/sound_dsp_test.cpp.
#pragma once

#include <stdint.h>
#include <string.h>

namespace snd {

constexpr int kWavHeader = 44;

inline void wavHeader(uint8_t *h, uint32_t rate, uint32_t samples) {
  auto u32 = [&](int at, uint32_t v) {
    for (int i = 0; i < 4; i++) h[at + i] = uint8_t(v >> (8 * i));
  };
  auto u16 = [&](int at, uint16_t v) { h[at] = uint8_t(v), h[at + 1] = uint8_t(v >> 8); };
  const uint32_t data = samples * 2;
  memcpy(h, "RIFF", 4);
  u32(4, 36 + data);
  memcpy(h + 8, "WAVEfmt ", 8);
  u32(16, 16);
  u16(20, 1);
  u16(22, 1);
  u32(24, rate);
  u32(28, rate * 2);
  u16(32, 2);
  u16(34, 16);
  memcpy(h + 36, "data", 4);
  u32(40, data);
}

// Gain (x1-x64) that brings `peak` to ~-1 dBFS. 1 for silence.
inline float gainFor(int peak) {
  if (peak < 16) return 1.0f;
  const float g = 29000.0f / float(peak);
  return g < 1 ? 1 : g > 64 ? 64 : g;
}

inline int16_t applyGain(int16_t s, float gain) {
  const float v = s * gain;
  return int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

}  // namespace snd
