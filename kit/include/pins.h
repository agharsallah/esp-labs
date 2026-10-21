// The one place that says which wire goes where.
// Change a number here (or pass -DPIN_xxx=N as a build flag) and every lesson follows.
//
// Pins to keep away from on the ESP32-S3-N16R8:
//   0, 3, 45, 46   strapping pins: they decide how the chip boots
//   19, 20         native USB D- / D+
//   26-32          wired to the flash chip
//   33-37          wired to the octal PSRAM (that's the "R8" in N16R8)
//   43, 44         UART0 TX / RX: the serial monitor on the "COM" port
#pragma once

// On-board addressable RGB LED (WS2812). Most N16R8 DevKitC clones use 48;
// v1.1 boards use 38. If lesson 01 shows no colour, try 38.
#ifndef PIN_RGB
#define PIN_RGB 48
#endif

// The BOOT button already on the board. It only matters at power-on (hold it to enter flash mode);
// once our program runs, GPIO 0 is a normal input. Pressed = LOW. Used from lesson 03b.
#ifndef PIN_BOOT_BTN
#define PIN_BOOT_BTN 0
#endif

// Lesson 03b: external push button. GPIO 5 -> one leg, the diagonally opposite leg -> GND.
// The internal pull-up makes the pin read HIGH when up and LOW when pressed.
#ifndef PIN_BUTTON
#define PIN_BUTTON 5
#endif

// Lesson 02: external LED. GPIO 4 -> 220-330 ohm resistor -> LED long leg (+), short leg (-) -> GND.
#ifndef PIN_LED
#define PIN_LED 4
#endif

// Lesson 03: 1.9" ST7789 SPI display. These are the S3's native SPI2 (FSPI) pins, the fastest route.
// Module label -> ESP32-S3 pin
#ifndef PIN_TFT_SCLK
#define PIN_TFT_SCLK 12  // SCL / SCK / CLK
#endif
#ifndef PIN_TFT_MOSI
#define PIN_TFT_MOSI 11  // SDA / MOSI / DIN
#endif
#ifndef PIN_TFT_CS
#define PIN_TFT_CS 10    // CS
#endif
#ifndef PIN_TFT_DC
#define PIN_TFT_DC 9     // DC / RS / A0
#endif
#ifndef PIN_TFT_RST
#define PIN_TFT_RST 8    // RES / RST
#endif
#ifndef PIN_TFT_BL
#define PIN_TFT_BL 7     // BLK / BL / LED (backlight)
#endif

// RFID track: the RC522 board (MFRC522 chip) on the S3's second SPI bus (SPI3), so it never shares
// wires or timing with the screen on SPI2. The four SPI pins sit side by side on the left header
// in the same order as the module's pins: SDA, SCK, MOSI, MISO.
// Module label -> ESP32-S3 pin. 3.3V only: the MFRC522 is not 5 V tolerant.
#ifndef PIN_RFID_SS
#define PIN_RFID_SS 15    // SDA (in SPI mode this is chip select, NSS)
#endif
#ifndef PIN_RFID_SCK
#define PIN_RFID_SCK 16   // SCK
#endif
#ifndef PIN_RFID_MOSI
#define PIN_RFID_MOSI 17  // MOSI
#endif
#ifndef PIN_RFID_MISO
#define PIN_RFID_MISO 18  // MISO
#endif
#ifndef PIN_RFID_RST
#define PIN_RFID_RST 14   // RST (low = hard power-down)
#endif
#ifndef PIN_RFID_IRQ
#define PIN_RFID_IRQ 13   // IRQ (optional: -1 = not wired, the lab then polls)
#endif

// Sound track: the KY-037 microphone module (electret capsule + LM393 comparator + a blue trim pot).
// Power it from 3V3, never 5 V: its AO and DO outputs swing up to its supply, and the ESP32-S3 pins
// take 3.3 V at most. Module label -> ESP32-S3 pin.
//   AO must be an ADC1 pin (GPIO 1-10): ADC2 is busy while Wi-Fi is on. 6 = ADC1 channel 5.
//   DO is the comparator's yes/no output: any digital pin.
#ifndef PIN_MIC_AO
#define PIN_MIC_AO 6   // AO (analog: the microphone's voltage)
#endif
#ifndef PIN_MIC_DO
#define PIN_MIC_DO 21  // DO (digital: high = louder than the pot's threshold)
#endif

// Sound track, digital: the INMP441 MEMS microphone (I2S, 24-bit). VDD -> 3V3, GND -> GND,
// L/R -> GND (we read the left slot). Three wires in a row on the right header.
#ifndef PIN_I2S_MIC_SCK
#define PIN_I2S_MIC_SCK 40  // SCK (bit clock)
#endif
#ifndef PIN_I2S_MIC_WS
#define PIN_I2S_MIC_WS 41   // WS (word select: left/right)
#endif
#ifndef PIN_I2S_MIC_SD
#define PIN_I2S_MIC_SD 42   // SD (the data)
#endif

// Sound track, out loud: the MAX98357A I2S amplifier + a 4 ohm speaker on its screw terminal.
// VIN -> 5V (louder; 3V3 works, quieter), GND -> GND, GAIN and SD unconnected (9 dB, mono mix).
#ifndef PIN_I2S_SPK_BCLK
#define PIN_I2S_SPK_BCLK 39  // BCLK
#endif
#ifndef PIN_I2S_SPK_LRC
#define PIN_I2S_SPK_LRC 38   // LRC (word select)
#endif
#ifndef PIN_I2S_SPK_DIN
#define PIN_I2S_SPK_DIN 47   // DIN (the data)
#endif
