// Scene 4: a live dashboard with real numbers from your chip.
//   - an arc gauge for the chip's internal temperature sensor
//   - bars for how much internal RAM and PSRAM are in use
//   - a chart of how long the last 120 frames took to draw and send
#pragma once

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <esp_heap_caps.h>
#include <math.h>

#include <algorithm>

#include "color.h"
#include "../scene.h"
#include "../stats.h"

class DashboardScene : public Scene {
 public:
  explicit DashboardScene(const Stats &stats) : stats_(stats) {}

  const char *name() const override { return "Live dashboard"; }

  void render(FrameBuffer &fb, float t, float) override {
    fb.fillScreen(pal::night);
    drawTemperature(fb, t);
    drawMemory(fb);
    drawFrameChart(fb);
  }

 private:
  // ---- Arc gauge -------------------------------------------------------------------------
  void drawTemperature(FrameBuffer &fb, float t) {
    constexpr int cx = 72, cy = 96, rOut = 54, rIn = 42, segments = 36;
    constexpr float start = 2.35f, sweep = 4.71f;  // from lower-left, 270 degrees clockwise
    const float temp = isnan(stats_.tempC) ? 0 : stats_.tempC;
    const float level = std::clamp((temp - 20) / 60, 0.0f, 1.0f);  // the scale runs 20-80 °C
    // Ease the needle in when the scene starts, like a real instrument powering up.
    const float shown = level * std::min(1.0f, t * 1.5f);

    for (int s = 0; s < segments; s++) {
      const float a0 = start + sweep * s / segments, a1 = start + sweep * (s + 0.8f) / segments;
      const float pos = float(s) / segments;
      const uint16_t lit = pos < 0.5f ? mix(pal::teal, pal::amber, uint8_t(pos * 2 * 255))
                                      : mix(pal::amber, pal::coral, uint8_t((pos - 0.5f) * 2 * 255));
      const uint16_t c = pos < shown ? lit : pal::dim;
      const int16_t x0 = cx + cosf(a0) * rOut, y0 = cy + sinf(a0) * rOut, x1 = cx + cosf(a1) * rOut, y1 = cy + sinf(a1) * rOut;
      const int16_t x2 = cx + cosf(a1) * rIn, y2 = cy + sinf(a1) * rIn, x3 = cx + cosf(a0) * rIn, y3 = cy + sinf(a0) * rIn;
      fb.fillTriangle(x0, y0, x1, y1, x2, y2, c);
      fb.fillTriangle(x0, y0, x2, y2, x3, y3, c);
    }

    char text[12];
    snprintf(text, sizeof(text), isnan(stats_.tempC) ? "--" : "%.1f", temp);
    centred(fb, &FreeSansBold12pt7b, text, cx, cy + 6, pal::ink);
    fb.drawCircle(cx + textWidth(fb, &FreeSansBold12pt7b, text) / 2 + 4, cy - 10, 2, pal::ink);  // degree sign
    centred(fb, nullptr, "CHIP TEMP C", cx, cy + 22, pal::muted);
    centred(fb, nullptr, "20", cx - 40, cy + 46, pal::muted);
    centred(fb, nullptr, "80", cx + 40, cy + 46, pal::muted);
  }

  // ---- Memory bars ------------------------------------------------------------------------
  void drawMemory(FrameBuffer &fb) {
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(pal::ink);
    fb.setCursor(150, 22);
    fb.print("ESP32-S3 N16R8");

    const size_t intTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bar(fb, 150, 34, "RAM", intTotal - intFree, intTotal);
    bar(fb, 150, 58, "PSRAM", psTotal - psFree, psTotal);
  }

  void bar(FrameBuffer &fb, int x, int y, const char *label, size_t used, size_t total) {
    constexpr int w = 160, h = 8;
    const int fill = total ? int(w * double(used) / total) : 0;
    fb.setFont(nullptr);
    fb.setTextSize(1);
    fb.setTextColor(pal::muted);
    fb.setCursor(x, y);
    fb.print(label);
    char right[24];
    snprintf(right, sizeof(right), "%u / %u KB", unsigned(used / 1024), unsigned(total / 1024));
    fb.setCursor(x + w - int(strlen(right)) * 6, y);
    fb.print(right);
    fb.fillRoundRect(x, y + 10, w, h, 3, pal::dim);
    if (fill > 0) fb.fillRoundRect(x, y + 10, std::max(fill, 6), h, 3, pal::teal);
  }

  // ---- Frame time chart --------------------------------------------------------------------
  void drawFrameChart(FrameBuffer &fb) {
    constexpr int x0 = 150, y0 = 96, w = 160, h = 60;
    constexpr float maxMs = 80;
    fb.setFont(nullptr);
    fb.setTextColor(pal::muted);
    fb.setCursor(x0, y0 - 10);
    fb.printf("FRAME TIME  %.0f ms", stats_.drawMs + stats_.pushMs);
    for (float ms : {20.0f, 40.0f, 60.0f}) {
      const int y = y0 + h - int(h * ms / maxMs);
      for (int x = x0; x < x0 + w; x += 4) fb.drawPixel(x, y, pal::dim);
    }
    int prevX = -1, prevY = 0;
    for (int i = 0; i < Stats::kHistory; i++) {
      const int x = x0 + i * w / Stats::kHistory;
      const int y = y0 + h - int(h * std::min(stats_.at(i), maxMs) / maxMs);
      fb.drawFastVLine(x, y, y0 + h - y, mix(pal::night, pal::teal, 70));  // area under the line
      if (prevX >= 0) fb.drawLine(prevX, prevY, x, y, pal::teal);
      prevX = x, prevY = y;
    }
    fb.fillCircle(prevX, prevY, 2, pal::amber);  // the newest frame
    fb.setCursor(x0, y0 + h + 4);
    fb.print("last 120 frames, grid every 20 ms");
  }

  // ---- Text helpers ------------------------------------------------------------------------
  static int16_t textWidth(FrameBuffer &fb, const GFXfont *font, const char *s) {
    int16_t x1, y1;
    uint16_t w, h;
    fb.setFont(font);
    fb.setTextSize(1);
    fb.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    return int16_t(w);
  }

  static void centred(FrameBuffer &fb, const GFXfont *font, const char *s, int cx, int baseline, uint16_t c) {
    const int16_t w = textWidth(fb, font, s);
    fb.setTextColor(c);
    // The built-in font draws from its top-left corner, custom fonts from their baseline.
    fb.setCursor(cx - w / 2, font ? baseline : baseline - 7);
    fb.print(s);
  }

  const Stats &stats_;
};
