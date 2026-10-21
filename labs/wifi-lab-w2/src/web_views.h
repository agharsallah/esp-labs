// W2's own screens, on top of the shared ones in kit/lib/wifi-kit/src/views.h.
//   Set-up    while the set-up network is open: what a phone needs to join it
//   Message   text sent from the phone page or the dashboard (view 5)
//   Overlay   firmware update progress
#pragma once

#include "setup_portal.h"
#include "views.h"

namespace views {

// ---- Set-up network: replaces the status view while it's open and we're not joined --------

inline void setupScreen(FrameBuffer &fb, const SetupPortal &portal) {
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(8, 52);
  fb.print("Set up from your phone");
  fb.setFont(nullptr);
  const char *labels[] = {"Wi-Fi", "Password", "Then open"};
  const String values[] = {SetupPortal::kSsid, portal.password(), "http://" + portal.address()};
  for (int i = 0; i < 3; i++) {
    fb.setTextColor(pal::muted);
    fb.setCursor(8, 70 + i * 22);
    fb.print(labels[i]);
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(i == 1 ? pal::amber : pal::teal);
    fb.setCursor(80, 80 + i * 22);
    fb.print(values[i]);
    fb.setFont(nullptr);
  }
  fb.setTextColor(pal::muted);
  fb.setCursor(8, 140);
  fb.print("or in the serial monitor: join <#>");
}

// ---- View 5: a message sent from the web ---------------------------------------------------

inline void message(FrameBuffer &fb, const String &text, uint16_t accent) {
  if (text.isEmpty()) {
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(pal::muted);
    fb.setCursor(8, 70);
    fb.print("No message yet");
    fb.setFont(nullptr);
    fb.setCursor(8, 90);
    fb.print("Send one from http://bench.local or the dashboard");
    return;
  }
  // Word-wrap into up to 3 lines of the big font; fall back to the small font for long text.
  const GFXfont *font = text.length() <= 30 ? &FreeSansBold18pt7b : &FreeSans9pt7b;
  const int lineH = font == &FreeSansBold18pt7b ? 36 : 20, maxW = kW - 24;
  fb.setFont(font);
  fb.setTextColor(accent);
  String line, word;
  int y = font == &FreeSansBold18pt7b ? 72 : 56;
  auto flush = [&]() {
    fb.setCursor(12, y);
    fb.print(line);
    y += lineH;
    line = "";
  };
  for (size_t i = 0; i <= text.length(); i++) {
    const char ch = i < text.length() ? text[i] : ' ';
    if (ch != ' ') {
      word += ch;
      continue;
    }
    const String candidate = line.isEmpty() ? word : line + " " + word;
    int16_t x1, y1;
    uint16_t w, h;
    fb.getTextBounds(candidate, 0, 0, &x1, &y1, &w, &h);
    if (w > maxW && !line.isEmpty()) flush();
    line = line.isEmpty() ? word : line + " " + word;
    word = "";
    if (y > kH - 14) break;
  }
  if (!line.isEmpty() && y <= kH - 14) flush();
}

// ---- Overlay: firmware update progress ------------------------------------------------------

inline void otaOverlay(FrameBuffer &fb, int percent) {
  if (percent < 0) return;
  fb.fillRoundRect(30, 56, kW - 60, 60, 10, pal::night);
  fb.drawRoundRect(30, 56, kW - 60, 60, 10, pal::amber);
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(44, 80);
  fb.printf("Updating firmware  %d%%", percent);
  fb.fillRoundRect(44, 92, kW - 88, 10, 4, pal::dim);
  fb.fillRoundRect(44, 92, (kW - 88) * percent / 100, 10, 4, pal::amber);
}

}  // namespace views
