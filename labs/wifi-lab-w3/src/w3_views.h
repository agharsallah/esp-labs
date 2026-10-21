// W3's screens (the status view comes from kit/lib/wifi-kit).
//   Now       animated sky + temperature, feels-like, wind, humidity
//   Forecast  next 24 hours: temperature line, and rain chance bars below it (two charts, two scales)
//   Week      7 days: each day's low-to-high range on one shared scale
//   Oracle    the AI's haiku or answer, typed out, in its chosen colour
#pragma once

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>
#include <time.h>

#include <algorithm>

#include "views.h"
#include "worker.h"

namespace w3 {

using views::kH;
using views::kW;
constexpr uint16_t kRain = rgb(90, 160, 255);
constexpr uint16_t kCloud = rgb(186, 198, 214);
constexpr uint16_t kCloudDark = rgb(92, 104, 124);
constexpr uint16_t kMoon = rgb(236, 232, 200);

// ---- Animated sky, drawn around (cx, cy) at size r -------------------------------------------

inline void cloud(FrameBuffer &fb, int x, int y, int r, uint16_t c) {
  fb.fillCircle(x - r, y + r / 4, r * 3 / 4, c);
  fb.fillCircle(x, y - r / 3, r, c);
  fb.fillCircle(x + r, y + r / 4, r * 2 / 3, c);
  fb.fillRoundRect(x - r * 7 / 4, y + r / 4, r * 7 / 2, r * 3 / 4, r * 3 / 8, c);
}

inline void scene(FrameBuffer &fb, Scene s, int cx, int cy, int r, float t) {
  switch (s) {
    case Scene::Sun:
      for (int i = 0; i < 12; i++) {
        const float a = t * 0.6f + i * 0.5236f;
        fb.drawLine(cx + cosf(a) * r * 1.15f, cy + sinf(a) * r * 1.15f, cx + cosf(a) * r * 1.65f, cy + sinf(a) * r * 1.65f, pal::amber);
      }
      fb.fillCircle(cx, cy, r * 0.85f, pal::amber);
      break;
    case Scene::Night:
      for (int i = 0; i < 7; i++) {
        const int sx = cx - r * 1.6f + (i * 37 % 100) * r * 3.2f / 100, sy = cy - r * 1.3f + (i * 53 % 100) * r * 2.6f / 100;
        fb.drawPixel(sx, sy, mix(pal::night, pal::ink, uint8_t(128 + 127 * sinf(t * 2.5f + i))));
      }
      fb.fillCircle(cx, cy, r * 0.8f, kMoon);
      fb.fillCircle(cx + r * 0.38f, cy - r * 0.22f, r * 0.68f, pal::night);  // bite out of the moon
      break;
    case Scene::Clouds: {
      const int drift = int(sinf(t * 0.5f) * r * 0.25f);
      cloud(fb, cx + r / 2 - drift, cy - r / 3, r * 2 / 3, kCloudDark);
      cloud(fb, cx - r / 4 + drift, cy + r / 5, r * 3 / 4, kCloud);
      break;
    }
    case Scene::Rain:
    case Scene::Storm: {
      const bool storm = s == Scene::Storm;
      for (int i = 0; i < 9; i++) {
        const int x = cx - r * 1.3f + i * r * 2.6f / 9;
        const int y = cy + int(fmodf(t * r * 2.2f + i * 13.0f, r * 1.3f));
        fb.drawLine(x, y, x - 2, y + 6, kRain);
      }
      cloud(fb, cx, cy - r / 2, r * 3 / 4, storm ? kCloudDark : kCloud);
      if (storm && int(t * 3) % 4 == 0) {  // a flash now and then
        fb.fillTriangle(cx + 2, cy - r / 4, cx - r / 3, cy + r / 2, cx + 1, cy + r / 2, pal::amber);
        fb.fillTriangle(cx + 1, cy + r / 3, cx + r / 4, cy + r / 3, cx - r / 6, cy + r * 1.1f, pal::amber);
      }
      break;
    }
    case Scene::Snow:
      for (int i = 0; i < 10; i++) {
        const int x = cx - r * 1.3f + i * r * 2.6f / 10 + int(sinf(t * 1.5f + i) * 3);
        const int y = cy + int(fmodf(t * r * 0.8f + i * 11.0f, r * 1.3f));
        fb.fillCircle(x, y, 1, pal::ink);
      }
      cloud(fb, cx, cy - r / 2, r * 3 / 4, kCloud);
      break;
    case Scene::Fog:
      for (int row = 0; row < 5; row++) {
        const int y = cy - r + row * r / 2;
        for (int x = cx - r * 3 / 2; x < cx + r * 3 / 2; x += 2) {
          const int wave = int(sinf(x * 0.12f + t * 1.2f + row) * 2);
          fb.drawPixel(x, y + wave, row % 2 ? kCloud : kCloudDark);
        }
      }
      break;
  }
}

inline uint16_t sceneDot(Scene s) {
  switch (s) {
    case Scene::Sun: return pal::amber;
    case Scene::Night: return kMoon;
    case Scene::Rain: return kRain;
    case Scene::Storm: return pal::coral;
    case Scene::Snow: return pal::ink;
    default: return kCloud;
  }
}

// Draws a small degree ring at the current cursor position, then moves the cursor past it.
inline void degree(FrameBuffer &fb, int capTop, int r, uint16_t c) {
  const int x = fb.getCursorX() + r + 2;
  fb.drawCircle(x, capTop + r, r, c);
  fb.setCursor(x + r + 3, fb.getCursorY());
}

inline String ago(uint32_t at) {
  if (!at) return "never";
  const uint32_t s = (millis() - at) / 1000;
  if (s < 60) return "just now";
  return String(s / 60) + " min ago";
}

// ---- Header and footer -----------------------------------------------------------------------

inline void header(FrameBuffer &fb, const char *title, const char *view, int index, int count, const Place &place,
                   Connection::State st) {
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(8, 18);
  fb.print(title);
  fb.print(' ');
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  fb.print(view);
  fb.setFont(nullptr);
  fb.setTextColor(pal::muted);
  fb.setCursor(186, 6);
  fb.print(views::fit(fb, place.valid() ? place.name : String("no city yet"), 130));
  const int firstDot = 298 - (count - 1) * 10;
  for (int i = 0; i < count; i++) fb.fillCircle(firstDot + i * 10, 20, 3, i == index ? pal::ink : pal::dim);
  fb.fillCircle(312, 20, 4, views::connectionColor(st));
  fb.drawFastHLine(0, 28, kW, pal::dim);
}

// Left: the button hint. Right: what the network is doing, or how the last request went.
inline void footer(FrameBuffer &fb, const char *hint, const Worker::Outcome &o) {
  fb.fillRect(0, kH - 11, kW, 11, pal::night);
  fb.setFont(nullptr);
  fb.setTextColor(pal::muted);
  fb.setCursor(6, kH - 9);
  fb.print(hint);
  char right[40];
  uint16_t color = pal::muted;
  if (o.busy) {
    snprintf(right, sizeof(right), "%s%.*s", o.busy, int(millis() / 300 % 4), "...");
    color = pal::amber;
  } else if (!o.error.isEmpty()) {
    snprintf(right, sizeof(right), "%s failed", o.last.what);
    color = pal::coral;
  } else if (o.last.ms) {
    snprintf(right, sizeof(right), "%s %lu ms %.1f KB", o.last.what, static_cast<unsigned long>(o.last.ms), o.last.bytes / 1024.0f);
    color = pal::teal;
  } else {
    right[0] = 0;
  }
  int16_t x1, y1;
  uint16_t w, h;
  fb.getTextBounds(right, 0, 0, &x1, &y1, &w, &h);
  fb.setTextColor(color);
  fb.setCursor(kW - 6 - w, kH - 9);
  fb.print(right);
}

inline void hint(FrameBuffer &fb, int y, const char *const *lines, int n) {
  fb.setFont(nullptr);
  fb.setTextColor(pal::muted);
  for (int i = 0; i < n; i++) {
    fb.setCursor(8, y + i * 14);
    fb.print(lines[i]);
  }
}

// ---- View 1: now -----------------------------------------------------------------------------

inline void now(FrameBuffer &fb, const Worker::Outcome &o, bool hasPlace, float t) {
  if (!o.weatherAt) {
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(pal::ink);
    fb.setCursor(8, 54);
    fb.print(hasPlace ? "Fetching the weather..." : "Which city?");
    const char *noPlace[] = {"In the serial monitor type:", "  city Hamburg", "(any city name; it's saved on the chip)"};
    if (!hasPlace) hint(fb, 70, noPlace, 3);
    if (!o.error.isEmpty()) {
      fb.setFont(nullptr);
      fb.setTextColor(pal::coral);
      fb.setCursor(8, 130);
      fb.print(views::fit(fb, o.error, 300));
    }
    return;
  }
  const Weather &w = o.weather;
  scene(fb, weather::sceneFor(w.code, w.day), 66, 92, 30, t);

  const int x = 142;
  fb.setFont(&FreeSansBold24pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(x, 78);
  fb.print(int(lroundf(w.temp)));
  degree(fb, 44, 5, pal::ink);
  fb.setFont(&FreeSans9pt7b);
  fb.print("C");
  fb.setTextColor(pal::ink);
  fb.setCursor(x, 100);
  fb.print(weather::describe(w.code));

  fb.setFont(nullptr);
  fb.setTextColor(pal::muted);
  fb.setCursor(x, 112);
  fb.printf("feels %d  wind %d km/h", int(lroundf(w.feels)), int(lroundf(w.wind)));
  fb.setCursor(x, 124);
  fb.printf("humidity %d%%", w.humidity);
  if (w.days > 0) {
    fb.setCursor(x, 136);
    fb.printf("today %d to %d", int(lroundf(w.dayMin[0])), int(lroundf(w.dayMax[0])));
  }
  fb.setCursor(x, 148);
  fb.print("updated " + ago(o.weatherAt));
}

// ---- View 2: the next 24 hours ---------------------------------------------------------------

inline void forecast(FrameBuffer &fb, const Worker::Outcome &o) {
  const Weather &w = o.weather;
  if (!o.weatherAt || w.hours < 2) {
    const char *lines[] = {"No forecast yet.", "Double-click to fetch it."};
    hint(fb, 60, lines, 2);
    return;
  }
  constexpr int x0 = 34, x1 = 312;
  const float lo = floorf(*std::min_element(w.hourTemp, w.hourTemp + w.hours)) - 1;
  const float hi = ceilf(*std::max_element(w.hourTemp, w.hourTemp + w.hours)) + 1;
  auto xOf = [&](int i) { return x0 + i * (x1 - x0) / (w.hours - 1); };

  // Chart 1: temperature, its own scale (lo..hi)
  constexpr int ty0 = 38, ty1 = 92;
  auto yOf = [&](float v) { return int(ty1 - (v - lo) * (ty1 - ty0) / (hi - lo)); };
  fb.setFont(nullptr);
  for (float v : {lo, hi}) {
    for (int x = x0; x < x1; x += 4) fb.drawPixel(x, yOf(v), pal::dim);
    fb.setTextColor(pal::muted);
    fb.setCursor(4, yOf(v) - 3);
    fb.printf("%d", int(v));
  }
  for (int i = 1; i < w.hours; i++) fb.drawLine(xOf(i - 1), yOf(w.hourTemp[i - 1]), xOf(i), yOf(w.hourTemp[i]), pal::amber);
  fb.fillCircle(xOf(0), yOf(w.hourTemp[0]), 3, pal::amber);
  fb.setTextColor(pal::ink);
  fb.setCursor(xOf(0) + 6, yOf(w.hourTemp[0]) - 10);
  fb.printf("%.0f C now", w.hourTemp[0]);

  // Chart 2: chance of rain, 0-100 %
  constexpr int ry0 = 104, ry1 = 144;
  fb.setTextColor(pal::muted);
  fb.setCursor(4, ry0);
  fb.print("rain");
  fb.setCursor(4, ry1 - 7);
  fb.print("0%");
  const int bw = std::max(2, (x1 - x0) / w.hours - 2);
  for (int i = 0; i < w.hours; i++) {
    const int h = w.hourRain[i] * (ry1 - ry0) / 100;
    fb.fillRect(xOf(i) - bw / 2, ry1 - h, bw, std::max(h, 1), h ? kRain : pal::dim);
  }
  const int peak = int(std::max_element(w.hourRain, w.hourRain + w.hours) - w.hourRain);
  if (w.hourRain[peak] > 0) {
    fb.setTextColor(kRain);
    fb.setCursor(std::min(xOf(peak) - 8, x1 - 24), ry0 - 2);
    fb.printf("%d%%", w.hourRain[peak]);
  }

  // Hours along the bottom, every 6 hours from the current hour
  struct tm lt;
  const int hour = getLocalTime(&lt, 0) ? lt.tm_hour : 0;
  fb.setTextColor(pal::muted);
  for (int i = 0; i < w.hours; i += 6) {
    fb.setCursor(xOf(i) - 6, 148);
    fb.printf("%02dh", (hour + i) % 24);
  }
}

// ---- View 3: the week ------------------------------------------------------------------------

inline void week(FrameBuffer &fb, const Worker::Outcome &o) {
  const Weather &w = o.weather;
  if (!o.weatherAt || w.days == 0) {
    const char *lines[] = {"No forecast yet.", "Double-click to fetch it."};
    hint(fb, 60, lines, 2);
    return;
  }
  const float lo = floorf(*std::min_element(w.dayMin, w.dayMin + w.days));
  const float hi = ceilf(*std::max_element(w.dayMax, w.dayMax + w.days));
  constexpr int bx0 = 112, bx1 = 270;
  auto xOf = [&](float v) { return int(bx0 + (v - lo) * (bx1 - bx0) / std::max(1.0f, hi - lo)); };
  static const char *const names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  struct tm lt;
  const int today = getLocalTime(&lt, 0) ? lt.tm_wday : -1;
  fb.setFont(nullptr);
  for (int i = 0; i < w.days; i++) {
    const int y = 36 + i * 17;
    if (i == 0) fb.fillRoundRect(2, y - 3, kW - 4, 15, 4, pal::dim);
    fb.setTextColor(i == 0 ? pal::ink : pal::muted);
    fb.setCursor(8, y + 1);
    fb.print(i == 0 ? "Today" : today >= 0 ? names[(today + i) % 7] : "+");
    fb.fillCircle(62, y + 4, 4, sceneDot(weather::sceneFor(w.dayCode[i], true)));
    fb.setTextColor(pal::muted);
    fb.setCursor(bx0 - 22, y + 1);
    fb.printf("%3d", int(lroundf(w.dayMin[i])));
    fb.drawFastHLine(bx0, y + 4, bx1 - bx0, pal::dim);
    fb.fillRoundRect(xOf(w.dayMin[i]), y + 2, std::max(4, xOf(w.dayMax[i]) - xOf(w.dayMin[i])), 5, 2, pal::amber);
    fb.setTextColor(pal::ink);
    fb.setCursor(bx1 + 6, y + 1);
    fb.printf("%d", int(lroundf(w.dayMax[i])));
  }
}

// ---- View 4: the oracle ----------------------------------------------------------------------

inline void oracleView(FrameBuffer &fb, const Worker::Outcome &o, bool ready, const String &model, float t) {
  if (!ready) {
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(pal::ink);
    fb.setCursor(8, 52);
    fb.print("The oracle needs its gateway");
    const char *lines[] = {"1. On the Mac: esp-labs/apps/ai-gateway (LiteLLM)", "2. Serial:  ai url http://<Mac IP>:4000",
                           "3. Serial:  ai key <LITELLM_MASTER_KEY>", "   Stored on the chip only, never logged.",
                           "4. Double-click here for a haiku"};
    hint(fb, 68, lines, 5);
    return;
  }
  const bool asking = o.busy && strcmp(o.busy, "asking the ai") == 0;
  if (asking || !o.answerAt) {
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(pal::ink);
    fb.setCursor(8, 56);
    fb.print(asking ? "Asking " : "Ask the Bench Oracle");
    if (asking) fb.print(model);
    if (asking)
      for (int i = 0; i < 3; i++) fb.fillCircle(fb.getCursorX() + 8 + i * 12, 52, 3, mix(pal::dim, pal::teal, uint8_t(128 + 127 * sinf(t * 5 - i))));
    const String using_ = "model: " + model + " (ai model <name> to switch)";
    const char *lines[] = {"Double-click: a haiku about right now", "Long press: the same, from any view",
                           "Serial: ask should I bike to work?", using_.c_str()};
    hint(fb, 74, lines, 4);
    return;
  }
  const OracleAnswer &a = o.answer;
  scene(fb, a.scene, 290, 50, 11, t);
  if (!o.question.isEmpty()) {
    fb.setFont(nullptr);
    fb.setTextColor(pal::muted);
    fb.setCursor(8, 34);
    fb.print(views::fit(fb, "> " + o.question, 250));
  }
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(mix(rgb(a.r, a.g, a.b), pal::ink, 70));  // its colour, lifted so it reads on the dark screen
  fb.setCursor(8, 58);
  fb.print(views::fit(fb, a.title, 250));
  // Typewriter: reveal the lines at ~35 characters per second.
  int budget = int((millis() - o.answerAt) * 35 / 1000);
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::ink);
  for (int i = 0; i < a.lineCount && budget > 0; i++) {
    const String line = views::fit(fb, a.lines[i], 304);
    fb.setCursor(8, 84 + i * 19);
    fb.print(line.substring(0, std::min<int>(budget, line.length())));
    budget -= line.length();
  }
  fb.setFont(nullptr);
  fb.setTextColor(pal::muted);
  fb.setCursor(8, 150);
  fb.printf("%s  %.1f s  %lu > %lu tokens", a.model.c_str(), a.ms / 1000.0f, static_cast<unsigned long>(a.inputTokens),
            static_cast<unsigned long>(a.outputTokens));
}

}  // namespace w3
