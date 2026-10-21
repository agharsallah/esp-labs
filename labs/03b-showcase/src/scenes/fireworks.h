// Scene 3: fireworks, a particle system.
//
// A particle is just a position, a speed and a remaining life. Each frame we apply
// gravity and air drag, move it, and draw it dimmer as its life runs out.
// The trails come from never clearing the picture: each frame every pixel is darkened
// a little instead, so old positions fade out slowly.
//
// The trails need their own layer that survives between frames (the shared frame buffer
// also gets the HUD and the title banner drawn on top). It lives in PSRAM: 106 KB there is nothing.
#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "color.h"
#include "../scene.h"

class FireworksScene : public Scene {
 public:
  const char *name() const override { return "Fireworks"; }

  void enter(FrameBuffer &fb) override {
    const size_t count = size_t(fb.width()) * fb.height();
    if (layer_ == nullptr) {
      layer_ = static_cast<uint16_t *>(heap_caps_malloc(count * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
      if (layer_ == nullptr) log_e("Fireworks: no PSRAM for the trail layer");
    }
    if (layer_ != nullptr) std::fill_n(layer_, count, uint16_t(0));
    for (Particle &p : particles_) p.life = 0;
    nextLaunch_ = 0;
  }

  void render(FrameBuffer &fb, float t, float dt) override {
    const int w = fb.width(), h = fb.height();
    if (layer_ == nullptr) {
      fb.fillScreen(pal::night);
      return;
    }
    dt = std::min(dt, 0.05f);  // a slow frame should not make particles jump

    // Fade: scale each channel by 13/16. Small values reach 0, so trails fully disappear.
    for (int k = 0; k < w * h; k++) {
      const uint16_t c = layer_[k];
      if (c == 0) continue;
      const uint16_t r = ((c >> 11) * 13) >> 4, g = (((c >> 5) & 63) * 13) >> 4, b = ((c & 31) * 13) >> 4;
      layer_[k] = uint16_t((r << 11) | (g << 5) | b);
    }

    if (t >= nextLaunch_) {
      spawn(random(40, w - 40), h, random(-25, 25), -random(150, 205), 2.0f, uint8_t(random(256)), true);
      nextLaunch_ = t + random(350, 900) / 1000.0f;
    }

    for (Particle &p : particles_) {
      if (p.life <= 0) continue;
      p.vy += kGravity * dt;
      if (!p.rocket) p.vx *= 0.985f, p.vy *= 0.985f;  // air drag on sparks
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      p.life -= dt;

      if (p.rocket) {
        plot(w, h, p.x, p.y, rgb(255, 240, 210), 2);
        if (p.vy > -25) explode(p);  // near the top of its arc
        continue;
      }
      const float k = std::max(0.0f, p.life / p.maxLife);
      const uint16_t c = hsv(p.hue, uint8_t(255 - 120 * (1 - k) * (1 - k)), uint8_t(255 * k));
      plot(w, h, p.x, p.y, c, k > 0.55f ? 2 : 1);
    }

    std::copy_n(layer_, w * h, fb.pixels());
    fb.setFont(nullptr);
    fb.setTextSize(1);
    fb.setTextColor(pal::muted);
    fb.setCursor(8, 8);
    fb.printf("%d particles", alive());
  }

 private:
  struct Particle {
    float x, y, vx, vy, life, maxLife;
    uint8_t hue;
    bool rocket;
  };
  static constexpr int kMax = 800;
  static constexpr float kGravity = 95;  // pixels per second, per second

  void spawn(float x, float y, float vx, float vy, float life, uint8_t hue, bool rocket) {
    for (int n = 0; n < kMax; n++) {
      Particle &p = particles_[cursor_];
      cursor_ = (cursor_ + 1) % kMax;
      if (p.life > 0) continue;
      p = {x, y, vx, vy, life, life, hue, rocket};
      return;
    }
  }

  void explode(Particle &rocket) {
    rocket.life = 0;
    const int sparks = random(70, 120);
    for (int s = 0; s < sparks; s++) {
      const float angle = random(0, 6283) / 1000.0f, speed = random(25, 115);
      const uint8_t hue = rocket.hue + random(-12, 12);
      spawn(rocket.x, rocket.y, cosf(angle) * speed, sinf(angle) * speed, random(110, 210) / 100.0f, hue, false);
    }
  }

  void plot(int w, int h, float fx, float fy, uint16_t c, int size) {
    const int x = int(fx), y = int(fy);
    for (int dy = 0; dy < size; dy++)
      for (int dx = 0; dx < size; dx++)
        if (x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h) layer_[(y + dy) * w + x + dx] = c;
  }

  int alive() const {
    int n = 0;
    for (const Particle &p : particles_) n += p.life > 0;
    return n;
  }

  Particle particles_[kMax] = {};
  int cursor_ = 0;
  float nextLaunch_ = 0;
  uint16_t *layer_ = nullptr;
};
