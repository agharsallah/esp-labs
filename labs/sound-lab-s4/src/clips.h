// S4's clip store: the recordings, kept in PSRAM (8 MB of RAM outside the chip; it forgets
// everything on reset or power-off: nothing here is written to flash).
//
// Each clip is plain 16-bit samples at the mic's rate, the same numbers the sampler put in its
// ring. While recording, a clip has room for the longest allowed recording; when it ends, the
// buffer is shrunk to what was actually recorded, so short clips cost little.
//
// Limits: 12 clips and a 6 MB budget. When a new recording doesn't fit, the oldest
// voice-activated clips go first; clips you started by hand (rec) are never dropped for you.
//
// Two tasks use the store: loop() records and deletes; the web server's task streams clips out.
// A download can take seconds, so a clip being sent is "leased": deleting it only takes it off
// the list, and the memory is freed by collect() once the last download has finished with it.
//
// Clip numbers count up from 1 and are never reused, so "del 3" always means the clip shown as 3.
// Pure C++ (no Arduino): host-tested in labs/sound-lab-s4/test/host/sound_clips_test.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <mutex>

#include "wav.h"

namespace rec {

struct Clip {
  uint32_t number = 0;
  uint32_t rate = 16000;
  uint32_t samples = 0;   // recorded so far
  uint32_t capacity = 0;  // room for this many
  uint32_t startMs = 0;   // on the lab's sound clock
  int64_t wall = 0;       // Unix time when it started, 0 = the clock wasn't set (no NTP yet)
  int16_t *data = nullptr;
  int peak = 0;           // the biggest |sample|
  bool manual = false;    // started by hand (rec) rather than by the voice trigger
  bool open = true;       // still recording
  uint8_t overview[48] = {};  // the clip's shape for the screen: 48 slices, each its loudest |sample|, 0-255
  std::atomic<int> readers{0};

  uint32_t durationMs() const { return rate ? uint32_t(uint64_t(samples) * 1000 / rate) : 0; }
  uint32_t wavBytes() const { return snd::kWavHeader + samples * 2; }
};

// Where clip memory comes from: PSRAM on the board, malloc in the host test.
struct Allocator {
  void *(*alloc)(size_t);
  void *(*resize)(void *, size_t);
  void (*release)(void *);
};

class ClipStore {
 public:
  static constexpr int kMax = 12;
  static constexpr int kGrave = 12;
  size_t budget = 6u * 1024 * 1024;

  explicit ClipStore(Allocator a) : mem_(a) {}

  struct Opened {
    Clip *clip = nullptr;
    int dropped = 0;          // old voice clips removed to make room
    const char *why = "";     // when clip is nullptr
  };

  // Start a clip with room for `capacity` samples. Drops the oldest voice clips if needed.
  Opened open(uint32_t rate, uint32_t capacity, bool manual, uint32_t startMs, int64_t wall) {
    Opened o;
    const size_t bytes = size_t(capacity) * 2;
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > budget) return o.why = "longer than the whole store", o;
    while (count_ >= kMax || usedLocked() + bytes > budget) {
      const int i = oldestVoiceLocked();
      if (i < 0) return o.why = count_ >= kMax ? "12 clips kept: del or clear some" : "store full: del or clear some", o;
      removeAtLocked(i);
      o.dropped++;
    }
    Clip *c = new Clip;
    c->data = static_cast<int16_t *>(mem_.alloc(bytes));
    if (!c->data) {
      delete c;
      return o.why = "no PSRAM left", o;
    }
    c->number = ++lastNumber_;
    c->rate = rate, c->capacity = capacity, c->startMs = startMs, c->wall = wall, c->manual = manual;
    clips_[count_++] = c;
    version_++;
    o.clip = c;
    return o;
  }

  // Add samples to an open clip (loop() only). Returns how many fitted.
  uint32_t append(Clip *c, const int16_t *s, uint32_t n) {
    if (!c || !c->open) return 0;
    if (n > c->capacity - c->samples) n = c->capacity - c->samples;
    for (uint32_t i = 0; i < n; i++) {
      const int a = s[i] < 0 ? -int(s[i]) : s[i];
      if (a > c->peak) c->peak = a;
    }
    memcpy(c->data + c->samples, s, n * 2);
    c->samples += n;
    return n;
  }
  bool full(const Clip *c) const { return c && c->samples >= c->capacity; }

  // End a clip: keep it (shrunk to size) or throw it away.
  void close(Clip *c, bool keep) {
    if (!c) return;
    if (!keep || c->samples == 0) {
      remove(c->number);
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (c->samples < c->capacity) {
      void *smaller = mem_.resize(c->data, size_t(c->samples) * 2);
      if (smaller) c->data = static_cast<int16_t *>(smaller), c->capacity = c->samples;
    }
    // The overview: the loudest sample in each of 48 slices, relative to the whole clip's peak.
    for (int b = 0; b < 48; b++) {
      const uint32_t from = uint64_t(c->samples) * b / 48, to = uint64_t(c->samples) * (b + 1) / 48;
      int m = 0;
      for (uint32_t i = from; i < to; i++) {
        const int a = c->data[i] < 0 ? -int(c->data[i]) : c->data[i];
        if (a > m) m = a;
      }
      c->overview[b] = uint8_t(c->peak ? m * 255 / c->peak : 0);
    }
    c->open = false;
    version_++;
  }

  bool remove(uint32_t number) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < count_; i++)
      if (clips_[i]->number == number) return removeAtLocked(i), true;
    return false;
  }

  // Remove every finished clip (an open one keeps recording).
  int clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    int removed = 0;
    for (int i = count_ - 1; i >= 0; i--)
      if (!clips_[i]->open) removeAtLocked(i), removed++;
    return removed;
  }

  // Lease a finished clip for reading from another task. nullptr if there is no such clip.
  Clip *lease(uint32_t number) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < count_; i++)
      if (clips_[i]->number == number && !clips_[i]->open) return clips_[i]->readers++, clips_[i];
    return nullptr;
  }
  static void release(Clip *c) {
    if (c) c->readers--;
  }

  // Free deleted clips nobody reads any more (loop() calls this every frame).
  void collect() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = graveCount_ - 1; i >= 0; i--)
      if (grave_[i]->readers.load() == 0) {
        freeClip(grave_[i]);
        grave_[i] = grave_[--graveCount_];
      }
  }

  // Look at the list under the lock: f(const Clip &) for each, oldest first.
  template <typename F>
  void each(F f) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < count_; i++) f(*clips_[i]);
  }
  // A copy of clip i's facts (not its samples), for drawing. False when i is past the end.
  bool facts(int i, Clip &out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (i < 0 || i >= count_) return false;
    const Clip &c = *clips_[i];
    out.number = c.number, out.rate = c.rate, out.samples = c.samples, out.capacity = c.capacity, out.startMs = c.startMs;
    out.wall = c.wall, out.data = c.data, out.peak = c.peak, out.manual = c.manual, out.open = c.open;
    memcpy(out.overview, c.overview, sizeof(out.overview));
    return true;
  }

  int count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }
  size_t used() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usedLocked();
  }
  uint32_t version() const { return version_.load(); }  // changes whenever the list does

 private:
  size_t usedLocked() const {
    size_t n = 0;
    for (int i = 0; i < count_; i++) n += size_t(clips_[i]->capacity) * 2;
    for (int i = 0; i < graveCount_; i++) n += size_t(grave_[i]->capacity) * 2;
    return n;
  }

  int oldestVoiceLocked() const {
    for (int i = 0; i < count_; i++)
      if (!clips_[i]->manual && !clips_[i]->open) return i;
    return -1;
  }

  void removeAtLocked(int i) {
    Clip *c = clips_[i];
    for (int j = i; j + 1 < count_; j++) clips_[j] = clips_[j + 1];
    count_--;
    version_++;
    if (c->readers.load() == 0 || graveCount_ >= kGrave) freeClip(c);  // a full grave: free anyway (never happens in practice)
    else grave_[graveCount_++] = c;
  }

  void freeClip(Clip *c) {
    mem_.release(c->data);
    delete c;
  }

  Allocator mem_;
  mutable std::mutex mutex_;
  Clip *clips_[kMax] = {};
  Clip *grave_[kGrave] = {};
  int count_ = 0, graveCount_ = 0;
  uint32_t lastNumber_ = 0;
  std::atomic<uint32_t> version_{0};
};

// ---- Reading a clip as a WAV file ----------------------------------------------------------------

// Copy up to maxLen bytes of the clip's WAV file, starting at byte `index`, with `gain` applied
// (snd::gainFor(peak) to normalise, 1 for the raw numbers). Returns the bytes written, 0 at the end.
// Used by both the web download (in pieces, as the network asks) and the serial `send`.
inline size_t wavRead(const Clip &c, float gain, uint8_t *buf, size_t maxLen, size_t index) {
  const size_t total = c.wavBytes();
  if (index >= total) return 0;
  size_t n = 0;
  if (index < size_t(snd::kWavHeader)) {
    uint8_t h[snd::kWavHeader];
    snd::wavHeader(h, c.rate, c.samples);
    while (index < size_t(snd::kWavHeader) && n < maxLen) buf[n++] = h[index++];
  }
  while (index < total && n < maxLen) {
    const size_t at = index - snd::kWavHeader;
    const int16_t s = snd::applyGain(c.data[at / 2], gain);
    buf[n++] = (at & 1) ? uint8_t(uint16_t(s) >> 8) : uint8_t(s);  // little-endian: low byte first
    index++;
  }
  return n;
}

// ---- The serial transfer: base64 lines + a CRC-32 ------------------------------------------------

// Base64: every 3 bytes become 4 printable characters, so binary survives a text-only serial line.
inline size_t base64(const uint8_t *in, size_t n, char *out) {
  static const char kAbc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = uint32_t(in[i]) << 16 | (i + 1 < n ? uint32_t(in[i + 1]) << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = kAbc[(v >> 18) & 63];
    out[o++] = kAbc[(v >> 12) & 63];
    out[o++] = i + 1 < n ? kAbc[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? kAbc[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

// CRC-32 (the one zip, PNG and Python's zlib.crc32 use): start with crc = 0, feed every byte.
inline uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

}  // namespace rec
