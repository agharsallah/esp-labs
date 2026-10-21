// The sketch view's own picture: 320 x 128 pixels that only the app draws on. It lives in its own
// frame buffer, so every screen frame copies it under the status bar instead of redrawing strokes.
#pragma once

#include <math.h>

#include "canvas_protocol.h"
#include "color.h"
#include "frame_buffer.h"

namespace b5 {

class Sketch {
 public:
  Sketch() : fb_(canvas::kW, canvas::kH) {}

  bool begin() {
    if (!fb_.begin()) return false;
    fb_.fillScreen(pal::night);
    return true;
  }

  // Draw one write's commands. Returns what the parser found.
  canvas::Result apply(const uint8_t *p, size_t n) {
    Drawer d{fb_};
    const canvas::Result r = canvas::parse(p, n, d);
    commands_ += r.commands;
    if (r.truncated || r.unknown) errors_++;
    return r;
  }

  void clear() { fb_.fillScreen(pal::night); }

  // Copy into the screen's frame buffer at row y0 (under the header).
  void blit(FrameBuffer &screen, int y0) const {
    uint16_t *dst = screen.pixels();
    const uint16_t *src = const_cast<FrameBuffer &>(fb_).pixels();
    for (int y = 0; y < canvas::kH && y0 + y < screen.height(); y++)
      memcpy(dst + (y0 + y) * screen.width(), src + y * canvas::kW, canvas::kW * sizeof(uint16_t));
  }

  uint32_t commands() const { return commands_; }
  uint32_t errors() const { return errors_; }
  bool ready() { return fb_.pixels() != nullptr; }

 private:
  struct Drawer {
    FrameBuffer &fb;
    void clear(uint16_t c) { fb.fillScreen(c); }
    // A thick line = a row of filled circles along each segment.
    void stroke(uint16_t c, uint8_t w, const canvas::Point *pts, int n) {
      const int r = w / 2;
      if (n == 1) return dot(c, pts[0].x, pts[0].y, std::max(r, 1));
      for (int i = 1; i < n; i++) {
        if (r <= 0) {
          fb.drawLine(pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, c);
          continue;
        }
        const float dx = pts[i].x - pts[i - 1].x, dy = pts[i].y - pts[i - 1].y;
        const int steps = std::max(1, int(sqrtf(dx * dx + dy * dy) / std::max(1, r / 2)));
        for (int s = 0; s <= steps; s++) fb.fillCircle(pts[i - 1].x + int(dx * s / steps), pts[i - 1].y + int(dy * s / steps), r, c);
      }
    }
    void rect(uint16_t c, int16_t x, int16_t y, int16_t w, int16_t h) { fb.fillRect(x, y, w, h, c); }
    void text(uint16_t c, int16_t x, int16_t y, uint8_t size, const char *s, uint8_t len) {
      fb.setFont(nullptr);
      fb.setTextSize(std::max<uint8_t>(1, std::min<uint8_t>(size, 4)));
      fb.setTextColor(c);
      fb.setCursor(x, y);
      for (int i = 0; i < len; i++) fb.write(uint8_t(s[i]) >= 0x20 && uint8_t(s[i]) < 0x7F ? s[i] : '?');
      fb.setTextSize(1);
    }
    void dot(uint16_t c, int16_t x, int16_t y, uint8_t r) { fb.fillCircle(x, y, r, c); }
  };

  FrameBuffer fb_;
  uint32_t commands_ = 0, errors_ = 0;
};

}  // namespace b5
