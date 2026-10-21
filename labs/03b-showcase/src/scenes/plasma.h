// Scene 2: plasma, a classic 1990s demo effect.
//
// Each pixel's colour is the sum of four sine waves running in different directions,
// one of them in circles around a wandering centre. Summing waves gives the liquid look.
// Sines are slow to compute, so we pre-compute 256 of them once (a lookup table)
// and also a 256-colour palette. Per pixel it is then only a few table lookups.
#pragma once

#include <math.h>

#include "color.h"
#include "../scene.h"

class PlasmaScene : public Scene {
 public:
  PlasmaScene() {
    for (int i = 0; i < 256; i++) {
      sine_[i] = uint8_t(128 + 127 * sinf(i * 6.2831853f / 256));
      // A cosine palette: deep indigo -> magenta -> orange -> pale gold, and back.
      const float f = i / 256.0f;
      auto channel = [f](float offset) { return uint8_t(255 * (0.5f + 0.5f * cosf(6.2831853f * (f + offset)))); };
      palette_[i] = rgb(channel(0.0f), uint8_t(channel(0.18f) * 0.75f), channel(0.42f));
    }
  }

  const char *name() const override { return "Plasma"; }

  void render(FrameBuffer &fb, float t, float) override {
    const int w = fb.width(), h = fb.height();
    const uint8_t t1 = uint8_t(t * 47), t2 = uint8_t(t * 29), shift = uint8_t(t * 21);
    const int cx = int(w / 2 + 110 * sinf(t * 0.63f)), cy = int(h / 2 + 55 * cosf(t * 0.81f));

    uint16_t *p = fb.pixels();
    for (int y = 0; y < h; y++) {
      const uint8_t row = sine_[uint8_t(y * 3 - t2)];
      const int dy = y - cy;
      for (int x = 0; x < w; x++) {
        const int dx = x - cx;
        const int v = sine_[uint8_t(x * 2 + t1)] + row + sine_[uint8_t(x + y + t2)] +
                      sine_[uint8_t(((dx * dx + dy * dy) >> 7) - t1)];
        *p++ = palette_[uint8_t((v >> 2) + shift)];
      }
    }

    fb.setFont(nullptr);
    fb.setTextSize(1);
    fb.setTextColor(pal::ink);
    fb.setCursor(8, 8);
    fb.printf("%d pixels computed every frame", w * h);
  }

 private:
  uint8_t sine_[256];
  uint16_t palette_[256];
};
