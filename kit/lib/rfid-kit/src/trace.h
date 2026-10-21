// A sniffer for the RFID labs: every frame the reader sends to the card and every answer, with a
// timestamp, kept in a small ring. mfrc522.h adds the entries; the screen's trace view and the
// serial printer (`trace on`) read them. Safe to use from two tasks.
#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

struct TraceFrame {
  enum class Kind : uint8_t {
    Frame,     // bytes on the air (or their plain text, when Crypto1 is on)
    NoAnswer,  // the card stayed silent until the chip's timer ran out
    Auth,      // the chip ran MIFARE's 3-pass authentication by itself (the key is never stored)
    AuthFail,
    FieldOff,  // antenna switched off (the card loses power)
    FieldOn,
  };
  uint32_t us = 0;  // esp_timer microseconds since boot (low 32 bits: wraps every 71 minutes)
  Kind kind = Kind::Frame;
  bool fromCard = false;  // false: reader -> card (PCD -> PICC)
  bool crypto = false;    // Crypto1 was on: on the air these bytes were encrypted
  uint8_t len = 0;        // whole or partial bytes stored, up to 18
  uint8_t lastBits = 0;   // bits used in the last byte, 0 = all 8
  uint8_t data[18] = {};
};

class Trace {
 public:
  static constexpr int kSize = 64;

  void add(TraceFrame::Kind kind, bool fromCard, const uint8_t *d, uint8_t n, uint8_t lastBits, bool crypto) {
    TraceFrame f;
    f.us = uint32_t(esp_timer_get_time());
    f.kind = kind;
    f.fromCard = fromCard;
    f.crypto = crypto;
    f.len = n > sizeof(f.data) ? sizeof(f.data) : n;
    f.lastBits = lastBits;
    if (d && f.len) memcpy(f.data, d, f.len);
    portENTER_CRITICAL(&mux_);
    frames_[head_ % kSize] = f;
    head_++;
    portEXIT_CRITICAL(&mux_);
  }

  // Sequence number of the next frame to be added: everything before it can be fetched with get().
  uint32_t head() const {
    portENTER_CRITICAL(&mux_);
    const uint32_t h = head_;
    portEXIT_CRITICAL(&mux_);
    return h;
  }

  // Frame number seq, if it is still in the ring.
  bool get(uint32_t seq, TraceFrame &out) const {
    portENTER_CRITICAL(&mux_);
    const bool ok = seq < head_ && head_ - seq <= kSize;
    if (ok) out = frames_[seq % kSize];
    portEXIT_CRITICAL(&mux_);
    return ok;
  }

  // Forget everything added since `seq` (e.g. an empty poll: WUPA, no answer, nothing to show).
  void rollback(uint32_t seq) {
    portENTER_CRITICAL(&mux_);
    if (seq < head_) head_ = seq;
    portEXIT_CRITICAL(&mux_);
  }

  void clear() {
    portENTER_CRITICAL(&mux_);
    head_ = 0;
    portEXIT_CRITICAL(&mux_);
  }

 private:
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  TraceFrame frames_[kSize];
  uint32_t head_ = 0;
};
