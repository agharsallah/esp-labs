// The screens every Wi-Fi lab shares (W2 adds its own in labs/wifi-lab-w2/src/web_views.h).
//   List      strongest first: signal bars, name, channel, lock, dBm
//   Channels  every network as a hump over the channels it occupies (the classic analyser view)
//   Radar     stronger = closer to the centre, with a sweeping beam
//   Status    the network we joined: clock, IP, bench.local, live signal chart
#pragma once

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>

#include "color.h"
#include "frame_buffer.h"
#include "connection.h"
#include "scanner.h"

namespace views {

constexpr int kW = 320, kH = 170;
constexpr int kRows = 5;  // list rows per page

inline uint16_t strengthColor(int32_t rssi) {
  if (rssi >= -60) return pal::teal;
  if (rssi >= -75) return pal::amber;
  return pal::coral;
}

// A stable colour per network, so its hump/dot keeps its colour between scans.
inline uint16_t networkColor(const String &bssid) {
  uint32_t h = 2166136261u;
  for (char c : bssid) h = (h ^ uint8_t(c)) * 16777619u;
  return hsv(uint8_t(h), 170, 255);
}

// Shorten text to fit maxW pixels in the current font, ending with ".." when cut.
inline String fit(FrameBuffer &fb, String s, int maxW) {
  int16_t x1, y1;
  uint16_t w, h;
  fb.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  if (w <= maxW) return s;
  while (s.length() > 1) {
    s.remove(s.length() - 1);
    fb.getTextBounds(s + "..", 0, 0, &x1, &y1, &w, &h);
    if (w <= maxW) return s + "..";
  }
  return s;
}

inline String label(const ScanResult &n) { return n.ssid.isEmpty() ? String("(hidden)") : n.ssid; }

inline void drawBars(FrameBuffer &fb, int x, int baseline, int32_t rssi) {
  const uint8_t lit = signalBars(rssi);
  for (int b = 0; b < 4; b++) {
    const int h = 4 + b * 3;
    fb.fillRect(x + b * 5, baseline - h, 3, h, b < lit ? strengthColor(rssi) : pal::dim);
  }
}

inline void drawLock(FrameBuffer &fb, int x, int y, uint16_t c) {
  fb.drawRoundRect(x + 1, y, 6, 6, 3, c);  // shackle
  fb.fillRect(x, y + 4, 8, 6, c);          // body
}

// ---- Header and footer shared by all views ----------------------------------------------

inline uint16_t connectionColor(Connection::State st) {
  switch (st) {
    case Connection::State::Connected: return pal::teal;
    case Connection::State::Connecting: return pal::amber;
    case Connection::State::Failed: return pal::coral;
    default: return pal::dim;
  }
}

// One dot per view (viewCount of them), the current one bright, right-aligned next to the link dot.
inline void header(FrameBuffer &fb, const Scanner &s, int view, int viewCount, const char *viewName, Connection::State st) {
  fb.setFont(&FreeSansBold9pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(8, 18);
  fb.print("Wi-Fi ");
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  fb.print(viewName);

  fb.setFont(nullptr);
  fb.setTextSize(1);
  fb.setCursor(186, 6);
  fb.setTextColor(pal::muted);
  if (s.scans() == 0 && s.scanning()) fb.print("first scan...");
  else fb.printf("%u networks  %lu ms", unsigned(s.networks().size()), static_cast<unsigned long>(s.lastScanMs()));
  const int firstDot = 298 - (viewCount - 1) * 10;
  for (int i = 0; i < viewCount; i++) fb.fillCircle(firstDot + i * 10, 20, 3, i == view ? pal::ink : pal::dim);
  // Wi-Fi link indicator: teal joined, amber joining, coral failed, grey not set up
  fb.fillCircle(312, 20, 4, connectionColor(st));
  fb.drawFastHLine(0, 28, kW, pal::dim);
}

inline void footer(FrameBuffer &fb, const char *hint, bool autoScan, const Scanner &s, uint32_t intervalS) {
  fb.fillRect(0, kH - 11, kW, 11, pal::night);
  fb.setFont(nullptr);
  fb.setTextSize(1);
  fb.setTextColor(pal::muted);
  fb.setCursor(6, kH - 9);
  fb.print(hint);
  fb.setCursor(kW - 66, kH - 9);
  if (s.scanning()) {
    fb.setTextColor(pal::amber);
    fb.print("scanning");
    const int dots = (millis() / 250) % 4;
    for (int i = 0; i < dots; i++) fb.print('.');
  } else {
    fb.setTextColor(autoScan ? pal::teal : pal::muted);
    if (autoScan) fb.printf("auto %lu s", static_cast<unsigned long>(intervalS));
    else fb.print("auto off");
  }
}

// ---- View 1: list -------------------------------------------------------------------------

inline void list(FrameBuffer &fb, const std::vector<ScanResult> &nets, int page) {
  if (nets.empty()) {
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(pal::muted);
    fb.setCursor(8, 90);
    fb.print("No networks yet");
    return;
  }
  const int pages = (int(nets.size()) + kRows - 1) / kRows;
  page %= pages;
  for (int r = 0; r < kRows; r++) {
    const int i = page * kRows + r;
    if (i >= int(nets.size())) break;
    const ScanResult &n = nets[i];
    const int y = 32 + r * 25;  // row top
    if (r % 2 == 1) fb.fillRect(0, y, kW, 25, rgb(16, 22, 32));
    drawBars(fb, 8, y + 18, n.rssi);
    fb.setFont(&FreeSans9pt7b);
    fb.setTextColor(n.ssid.isEmpty() ? pal::muted : pal::ink);
    fb.setCursor(34, y + 17);
    fb.print(fit(fb, label(n), 176));
    fb.setFont(nullptr);
    fb.setTextColor(pal::muted);
    fb.setCursor(216, y + 9);
    fb.printf("ch%2u", n.channel);
    if (n.auth != WIFI_AUTH_OPEN) drawLock(fb, 248, y + 7, pal::muted);
    else {
      fb.setTextColor(pal::coral);
      fb.setCursor(244, y + 9);
      fb.print("open");
    }
    fb.setTextColor(strengthColor(n.rssi));
    fb.setCursor(276, y + 9);
    fb.printf("%4ld", static_cast<long>(n.rssi));
    fb.setTextColor(pal::muted);
    fb.print("dB");
  }
  if (pages > 1) {
    fb.setFont(nullptr);
    fb.setTextColor(pal::muted);
    fb.setCursor(kW - 70, kH - 22);
    fb.printf("page %d/%d", page + 1, pages);
  }
}

// ---- View 2: channels ---------------------------------------------------------------------

inline void channels(FrameBuffer &fb, const std::vector<ScanResult> &nets) {
  constexpr int x0 = 22, x1 = 312, base = 140, top = 40;
  constexpr int slot = (x1 - x0) / 14;  // channels 1-13 plus a margin
  auto cx = [&](int ch) { return x0 + ch * slot; };
  auto hy = [&](int32_t rssi) { return base - int((std::clamp<int32_t>(rssi, -95, -30) + 95) * (base - top) / 65); };

  // dBm grid
  fb.setFont(nullptr);
  fb.setTextColor(pal::muted);
  for (int db : {-40, -60, -80}) {
    const int y = hy(db);
    for (int x = x0; x < x1; x += 4) fb.drawPixel(x, y, pal::dim);
    fb.setCursor(0, y - 3);
    fb.print(db);
  }
  // Each network occupies about 22 MHz = 4.4 channels, so its hump spans 2.2 channels each side.
  const int shown = std::min<int>(nets.size(), 24);
  for (int k = shown - 1; k >= 0; k--) {  // weakest first, strongest drawn on top
    const ScanResult &n = nets[k];
    if (n.channel < 1 || n.channel > 13) continue;
    const int c = cx(n.channel), peak = hy(n.rssi), hw = int(slot * 2.2f);
    const uint16_t col = networkColor(n.bssid);
    int px = c - hw, py = base;
    for (int dx = -hw + 2; dx <= hw; dx += 2) {
      const float f = float(dx) / hw;
      const int y = base - int((base - peak) * (1 - f * f));
      fb.drawLine(px, py, c + dx, y, col);
      px = c + dx, py = y;
    }
    if (k < 3) {  // name the three strongest at their peaks
      fb.setTextColor(col);
      const String name = fit(fb, label(n), 64);
      fb.setCursor(std::clamp(c - int(name.length()) * 3, 0, kW - int(name.length()) * 6), peak - 10);
      fb.print(name);
    }
  }
  // Channel axis: 1, 6 and 11 are the three that don't overlap each other.
  fb.drawFastHLine(x0, base, x1 - x0, pal::muted);
  for (int ch = 1; ch <= 13; ch++) {
    const bool clean = ch == 1 || ch == 6 || ch == 11;
    int count = 0;
    for (const ScanResult &n : nets) count += n.channel == ch;
    fb.setTextColor(clean ? pal::amber : pal::muted);
    fb.setCursor(cx(ch) - (ch < 10 ? 3 : 6), base + 4);
    fb.print(ch);
    if (count) {
      fb.setTextColor(pal::ink);
      fb.setCursor(cx(ch) - 3, base + 14);
      fb.print(count);
    }
  }
}

// ---- View 3: radar ------------------------------------------------------------------------

inline void radar(FrameBuffer &fb, const std::vector<ScanResult> &nets, float t, bool scanning) {
  constexpr int cx = 222, cy = 98, rMax = 66;
  auto radius = [](int32_t rssi) { return 6 + int((-30 - std::clamp<int32_t>(rssi, -95, -30)) * (rMax - 6) / 65); };

  for (int db : {-50, -70, -90}) fb.drawCircle(cx, cy, radius(db), pal::dim);
  fb.drawFastHLine(cx - rMax, cy, rMax * 2, pal::dim);
  fb.drawFastVLine(cx, cy - rMax, rMax * 2, pal::dim);
  fb.setFont(nullptr);
  fb.setTextColor(pal::muted);
  for (int db : {-50, -70, -90}) {
    fb.setCursor(cx + 2, cy - radius(db) - 9);
    fb.print(db);
  }

  // The sweeping beam with a fading tail. Faster while a scan is running.
  const float a = t * (scanning ? 4.0f : 1.2f);
  for (int k = 0; k < 10; k++) {
    const float ak = a - k * 0.06f;
    fb.drawLine(cx, cy, cx + int(cosf(ak) * rMax), cy + int(sinf(ak) * rMax), mix(pal::night, pal::teal, uint8_t(200 - k * 20)));
  }

  // Each network sits at a fixed angle (from its BSSID) and a distance from its signal strength.
  for (int k = std::min<int>(nets.size(), 30) - 1; k >= 0; k--) {
    const ScanResult &n = nets[k];
    uint32_t h = 0;
    for (char c : n.bssid) h = h * 31 + uint8_t(c);
    const float ang = (h % 628) / 100.0f;
    const int r = radius(n.rssi), x = cx + int(cosf(ang) * r), y = cy + int(sinf(ang) * r);
    fb.fillCircle(x, y, k < 3 ? 4 : 2, strengthColor(n.rssi));
  }

  // Left column: the closest few, by name.
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(8, 50);
  fb.print("Closest");
  fb.setFont(nullptr);
  for (int k = 0; k < std::min<int>(nets.size(), 5); k++) {
    const ScanResult &n = nets[k];
    fb.fillCircle(11, 64 + k * 16, 3, strengthColor(n.rssi));
    fb.setTextColor(pal::ink);
    fb.setCursor(20, 61 + k * 16);
    fb.print(fit(fb, label(n), 102));
    fb.setTextColor(pal::muted);
    fb.setCursor(126, 61 + k * 16);
    fb.print(n.rssi);
  }
}

// ---- View 4: status of the network we joined ----------------------------------------------

inline void status(FrameBuffer &fb, const Connection &c, float t) {
  using State = Connection::State;
  fb.setFont(&FreeSans9pt7b);
  if (c.state() == State::NoCredentials) {
    fb.setTextColor(pal::ink);
    fb.setCursor(8, 54);
    fb.print("Not joined to a network yet");
    fb.setFont(nullptr);
    fb.setTextColor(pal::muted);
    const char *steps[] = {"1. Open the serial monitor", "2. Find your network in the scan table",
                           "3. Type: join <number>", "4. Type the password on the next line",
                           "   (stored on the chip only, never logged)"};
    for (int i = 0; i < 5; i++) {
      fb.setCursor(8, 70 + i * 14);
      fb.print(steps[i]);
    }
    return;
  }
  if (c.state() != State::Connected) {
    const bool failed = c.state() == State::Failed;
    fb.setTextColor(failed ? pal::coral : pal::amber);
    fb.setCursor(8, 54);
    fb.print(failed ? "Could not join" : "Joining");
    fb.setTextColor(pal::ink);
    fb.setCursor(8, 76);
    fb.print(fit(fb, c.ssid(), 220));
    fb.setFont(nullptr);
    fb.setTextColor(pal::muted);
    fb.setCursor(8, 92);
    if (failed) fb.printf("last reason: %s", Connection::reasonHint(c.lastReason()));
    else fb.printf("%lu s ...", static_cast<unsigned long>(c.connectingFor() / 1000));
    fb.setCursor(8, 106);
    if (failed) fb.print("retrying every 30 s. 'forget' erases it.");
    // spinner
    for (int k = 0; k < 8; k++) {
      const float a = t * 6 + k * 0.785f;
      fb.fillCircle(270 + int(cosf(a) * 18), 80 + int(sinf(a) * 18), k == 0 ? 4 : 2, failed ? pal::coral : mix(pal::night, pal::amber, uint8_t(255 - k * 28)));
    }
    return;
  }

  // Connected: clock on the left, link details on the right, signal chart along the bottom.
  struct tm now;
  const bool synced = c.timeSynced() && getLocalTime(&now, 0);
  fb.setFont(&FreeSansBold24pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(8, 72);
  if (synced) fb.printf("%02d:%02d", now.tm_hour, now.tm_min);
  else fb.print("--:--");
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::muted);
  if (synced) {
    fb.printf(" %02d", now.tm_sec);
    char date[24];
    strftime(date, sizeof(date), "%a %d %b %Y", &now);
    fb.setCursor(8, 94);
    fb.print(date);
  } else {
    fb.setCursor(8, 94);
    fb.print("syncing time...");
  }

  fb.setFont(nullptr);
  const int x = 188;
  fb.setTextColor(pal::teal);
  fb.setCursor(x, 38);
  fb.print(fit(fb, c.ssid(), 124));
  fb.setTextColor(pal::ink);
  fb.setCursor(x, 52);
  fb.print(WiFi.localIP().toString());
  fb.setTextColor(pal::muted);
  fb.setCursor(x, 66);
  fb.printf("%s.local", Connection::kHostname);
  fb.setCursor(x, 80);
  fb.printf("ch %d  %ld dBm", static_cast<int>(WiFi.channel()), static_cast<long>(WiFi.RSSI()));
  fb.setCursor(x, 94);
  fb.printf("up %lu min", static_cast<unsigned long>(c.connectedFor() / 60000));

  // Signal over the last minute
  constexpr int x0 = 30, x1 = 312, y0 = 108, y1 = 150;
  auto yOf = [&](int dbm) { return y1 - (std::clamp(dbm, -95, -30) + 95) * (y1 - y0) / 65; };
  for (int db : {-50, -70, -90}) {
    for (int xx = x0; xx < x1; xx += 4) fb.drawPixel(xx, yOf(db), pal::dim);
    fb.setTextColor(pal::muted);
    fb.setCursor(2, yOf(db) - 3);
    fb.print(db);
  }
  int px = -1, py = 0;
  for (int i = 0; i < c.samples(); i++) {
    const int xx = x1 - (c.samples() - 1 - i) * (x1 - x0) / (Connection::kHistory - 1);
    const int yy = yOf(c.rssiAt(i));
    if (px >= 0) fb.drawLine(px, py, xx, yy, strengthColor(c.rssiAt(i)));
    px = xx, py = yy;
  }
  if (px >= 0) fb.fillCircle(px, py, 2, pal::amber);
}

}  // namespace views
