// The 1.9" ST7789 screen plus an off-screen frame buffer, set up the way every lesson since 03 uses it.
// Draw into fb() with the normal Adafruit GFX calls, then push() sends the whole picture at once.
#pragma once

#include <Adafruit_ST7789.h>
#include <SPI.h>

#include "frame_buffer.h"
#include "pins.h"

class Display {
 public:
  static constexpr int16_t kW = 320, kH = 170;  // landscape

  Display() : tft_(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST), fb_(kW, kH) {}

  // Returns false if there is no memory for the frame buffer.
  bool begin(uint32_t spiHz = 40'000'000) {
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
    SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    tft_.init(170, 320);
    tft_.setSPISpeed(spiHz);
    tft_.setRotation(1);
    tft_.fillScreen(0);
    return fb_.begin();
  }

  FrameBuffer &fb() { return fb_; }
  void push() { tft_.drawRGBBitmap(0, 0, fb_.pixels(), kW, kH); }

 private:
  Adafruit_ST7789 tft_;
  FrameBuffer fb_;
};
