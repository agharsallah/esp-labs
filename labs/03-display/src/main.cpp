// Lesson 03: the 1.9" IPS display (ST7789 driver, 170 x 320 pixels, SPI).
//
// Wiring (module label -> ESP32-S3), all set in kit/include/pins.h:
//   GND -> GND      VCC -> 3V3
//   SCL -> 12       SDA -> 11      RES -> 8
//   DC  -> 9        CS  -> 10      BLK -> 7
//
// What happens:
//   1. A colour check: red, green, blue bars. If the colours look wrong, see the notes below.
//   2. A live "bench scope": a sine wave sweeping across the screen with uptime and frame rate.
//
// New ideas in this lesson:
//   - SPI: a fast 4-wire bus. The ESP32 clocks bits out on SDA while toggling SCL.
//   - A frame buffer: we draw the whole picture in PSRAM first, then send it in one go,
//     so the screen never shows a half-drawn frame (no flicker).
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>

#include "pins.h"

namespace {

constexpr int16_t kW = 320;  // Landscape: rotation 1 turns the 170x320 panel sideways.
constexpr int16_t kH = 170;
constexpr uint32_t kSpiHz = 40'000'000;  // 40 MHz. Lower to 20'000'000 if you see noise or garbage.

Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
GFXcanvas16 *frame = nullptr;  // 320 x 170 x 2 bytes = 106 KB, lives in PSRAM.

// Colours are 16-bit "RGB565": 5 bits red, 6 green, 5 blue. color565 converts from 0-255 values.
struct Palette {
  uint16_t ink, paper, grid, trace, accent, muted;
} pal;

void definePalette() {
  pal.paper = tft.color565(12, 18, 28);    // deep blue-black, like an oscilloscope screen
  pal.grid = tft.color565(30, 46, 66);     // faint graticule
  pal.trace = tft.color565(90, 230, 190);  // phosphor green-cyan
  pal.accent = tft.color565(255, 176, 60); // amber, for numbers
  pal.ink = tft.color565(230, 236, 245);   // near-white text
  pal.muted = tft.color565(120, 140, 165); // secondary text
}

void colourCheck() {
  const int16_t bar = kW / 3;
  tft.fillRect(0, 0, bar, kH, ST77XX_RED);
  tft.fillRect(bar, 0, bar, kH, ST77XX_GREEN);
  tft.fillRect(bar * 2, 0, kW - bar * 2, kH, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(30, 76);
  tft.print("RED   GREEN   BLUE");
  log_i("Colour check on screen. Left->right should read red, green, blue.");
  // If the screen is all white/black: check wiring and BLK. If the colours are inverted
  // (e.g. black background looks white), call tft.invertDisplay(false) after tft.init().
}

void drawGraticule(GFXcanvas16 &g) {
  for (int16_t x = 0; x < kW; x += 32) g.drawFastVLine(x, 34, kH - 34, pal.grid);
  for (int16_t y = 34; y < kH; y += 34) g.drawFastHLine(0, y, kW, pal.grid);
}

void drawFrame(GFXcanvas16 &g, uint32_t now, float fps) {
  g.fillScreen(pal.paper);

  // Header
  g.setTextSize(2);
  g.setTextColor(pal.ink);
  g.setCursor(8, 8);
  g.print("hello, bench");
  g.setTextSize(1);
  g.setTextColor(pal.muted);
  g.setCursor(196, 8);
  g.print("ESP32-S3 N16R8");
  g.setCursor(196, 20);
  g.printf("%.0f fps", fps);

  drawGraticule(g);

  // A travelling sine wave with a slower wobble on top, so it looks alive.
  const float t = now / 1000.0f;
  int16_t prevY = -1;
  for (int16_t x = 0; x < kW; x++) {
    float phase = x * 0.045f - t * 3.0f;
    float y = sinf(phase) * 40 * (0.7f + 0.3f * sinf(t * 0.7f)) + sinf(phase * 3.1f) * 6;
    int16_t py = 102 + static_cast<int16_t>(y);
    if (prevY >= 0) g.drawLine(x - 1, prevY, x, py, pal.trace);
    prevY = py;
  }

  // Uptime, bottom right, as hh:mm:ss
  uint32_t s = now / 1000;
  g.setTextColor(pal.accent);
  g.setTextSize(2);
  g.setCursor(196, kH - 22);
  g.printf("%02lu:%02lu:%02lu", static_cast<unsigned long>(s / 3600),
           static_cast<unsigned long>(s / 60 % 60), static_cast<unsigned long>(s % 60));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  // Backlight first, so we can tell "screen dead" apart from "nothing drawn yet".
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

  SPI.begin(PIN_TFT_SCLK, -1 /* no MISO: the screen never talks back */, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(170, 320);
  tft.setSPISpeed(kSpiHz);
  tft.setRotation(1);
  definePalette();
  log_i("Display ready: %dx%d over SPI at %lu MHz", tft.width(), tft.height(),
        static_cast<unsigned long>(kSpiHz / 1'000'000));

  colourCheck();
  delay(2500);

  frame = new GFXcanvas16(kW, kH);
  if (frame == nullptr || frame->getBuffer() == nullptr) {
    log_e("Could not allocate the %d-byte frame buffer. Is PSRAM enabled?", kW * kH * 2);
    return;
  }
  log_i("Frame buffer: %d KB, PSRAM free: %lu KB", kW * kH * 2 / 1024,
        static_cast<unsigned long>(ESP.getFreePsram() / 1024));
}

void loop() {
  if (frame == nullptr || frame->getBuffer() == nullptr) {
    delay(1000);
    return;
  }

  static uint32_t lastFrame = millis();
  static uint32_t lastLog = 0;
  static float fps = 0;

  uint32_t now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;
  if (dt > 0) fps = fps * 0.9f + (1.0f / dt) * 0.1f;  // smoothed, so the number doesn't jitter

  drawFrame(*frame, now, fps);
  tft.drawRGBBitmap(0, 0, frame->getBuffer(), kW, kH);  // send the finished picture

  if (now - lastLog >= 5000) {
    lastLog = now;
    log_i("%.1f fps", fps);
  }
}
