// Our own off-screen picture, drawn with the normal Adafruit GFX calls.
//
// Lesson 03 used GFXcanvas16, which always lands in PSRAM (big but slow memory outside the chip).
// Here we choose: fast internal RAM when a big enough block is free, PSRAM otherwise.
// The serial log at start-up tells you which one you got.
#pragma once

#include <Adafruit_GFX.h>
#include <esp_heap_caps.h>

#include <algorithm>

class FrameBuffer : public Adafruit_GFX {
 public:
  FrameBuffer(int16_t w, int16_t h) : Adafruit_GFX(w, h) {}
  ~FrameBuffer() override { heap_caps_free(buf_); }

  bool begin() {
    const size_t bytes = bytesUsed();
    buf_ = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    internal_ = buf_ != nullptr;
    if (buf_ == nullptr) buf_ = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return buf_ != nullptr;
  }

  bool internal() const { return internal_; }
  size_t bytesUsed() const { return size_t(WIDTH) * HEIGHT * sizeof(uint16_t); }
  uint16_t *pixels() { return buf_; }

  // ---- The minimum Adafruit GFX needs (drawPixel) plus fast paths for lines and fills ----

  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < 0 || y < 0 || x >= _width || y >= _height) return;
    buf_[y * _width + x] = c;
  }

  void fillScreen(uint16_t c) override { std::fill_n(buf_, size_t(_width) * _height, c); }

  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override {
    if (w < 0) x += w + 1, w = -w;
    if (y < 0 || y >= _height) return;
    if (x < 0) w += x, x = 0;
    if (x + w > _width) w = _width - x;
    if (w > 0) std::fill_n(buf_ + y * _width + x, w, c);
  }

  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override {
    if (h < 0) y += h + 1, h = -h;
    if (x < 0 || x >= _width) return;
    if (y < 0) h += y, y = 0;
    if (y + h > _height) h = _height - y;
    for (uint16_t *p = buf_ + y * _width + x; h-- > 0; p += _width) *p = c;
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
    for (int16_t j = 0; j < h; j++) drawFastHLine(x, y + j, w, c);
  }

 private:
  uint16_t *buf_ = nullptr;
  bool internal_ = false;
};
