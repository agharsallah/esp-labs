// Frame timing and chip readings, shared by the HUD, the serial log and the dashboard scene.
#pragma once

#include <math.h>

struct Stats {
  static constexpr int kHistory = 120;  // frames kept for the dashboard chart

  float fps = 0;
  float drawMs = 0;  // time spent drawing the picture in memory
  float pushMs = 0;  // time spent sending it over SPI
  float tempC = NAN;
  float frameMs[kHistory] = {};
  int head = 0;

  void add(float draw, float push, float dtSeconds) {
    drawMs = drawMs * 0.9f + draw * 0.1f;  // smoothed so the numbers don't jitter
    pushMs = pushMs * 0.9f + push * 0.1f;
    if (dtSeconds > 0) fps = fps * 0.9f + (1.0f / dtSeconds) * 0.1f;
    frameMs[head] = draw + push;
    head = (head + 1) % kHistory;
  }

  // i = 0 is the oldest sample, kHistory - 1 the newest.
  float at(int i) const { return frameMs[(head + i) % kHistory]; }
};
