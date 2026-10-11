# Hardware log

Every command that installs software, talks to the board, or flashes it, with what it returned. Newest at the bottom.

**Rule for this bench:** Claude says what a command does before running it. Anything that installs, resets, reads or flashes the board waits for your go-ahead.

## Overview

| What                      | Value                                                                                                                          |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| Board                     | ESP32-S3-N16R8 DevKitC clone                                                                                                   |
| Chip                      | ESP32-S3 (QFN56) revision v0.2, dual core + LP core, 240 MHz, Wi-Fi + BT 5 (LE), 40 MHz crystal                                |
| Flash                     | 16 MB, quad mode, 3.3 V (manufacturer 0x68, device 0x4018)                                                                     |
| PSRAM                     | 8 MB, embedded in the chip                                                                                                     |
| MAC                       | 28:84:85:9E:3B:E4                                                                                                              |
| USB connection            | `/dev/cu.usbserial-A5069RR4`, FTDI FT232R USB-to-serial (VID:PID 0403:6001)                                                    |
| On-board RGB LED          | GPIO 48 (confirmed: it cycles colours)                                                                                         |
| Display                   | 1.9" ST7789 170 × 320 on SPI (GPIO 12/11/10/9/8/7), working at 22 fps                                                          |
| **Firmware on board now** | **`03b-showcase-button`** (variation 2), as you report. All 4 scenes cycle; push button wire found on GPIO 4, being moved to 5 |
| Flashes so far            | 5 (01-hello by Claude; 02-led, 03-display, 03b v1 and 03b v2 by you)                                                           |

### What is installed on the Mac

| Where                     | What                                                                                                  | Size   |
| ------------------------- | ----------------------------------------------------------------------------------------------------- | ------ |
| `test-esp/.venv`          | PlatformIO 6.2.0 and its Python dependencies (project-local)                                          | 22 MB  |
| `test-esp/.pio`           | build output and the two display libraries (Adafruit GFX 1.12.6, ST7789 1.11.0)                       | 28 MB  |
| `~/.platformio/platforms` | pioarduino espressif32 platform 55.03.312                                                             | 27 MB  |
| `~/.platformio/packages`  | Arduino-ESP32 3.3.12 (+ ESP-IDF 5.5.5 libs 2.4 GB), Xtensa GCC 14.2.0, GDB 17.1, esptool 5.4.0, SCons | 3.8 GB |
| `~/.platformio/tools`     | unpacked copies of the toolchain, GDB, esptool made by the platform's installer                       | 1.2 GB |
| `~/.platformio/.cache`    | download cache                                                                                        | 470 MB |
| `~/.platformio/dist`      | downloaded archives (safe to delete)                                                                  | 213 MB |
| `~/.platformio/penv`      | PlatformIO's own Python environment (runs esptool)                                                    | 71 MB  |

`~/.platformio` totals 5.8 GB and is shared by every PlatformIO project on this Mac. Deleting that folder uninstalls all of it. `~/.platformio/dist` and `~/.platformio/.cache` can be cleared at any time to get about 680 MB back.

### Flash layout written by `01-hello`

| Address | File             | Size      | Purpose                                    |
| ------- | ---------------- | --------- | ------------------------------------------ |
| 0x0000  | `bootloader.bin` | 19,968 B  | second-stage bootloader                    |
| 0x8000  | `partitions.bin` | 3,072 B   | partition table (`default_16MB.csv`)       |
| 0xE000  | `boot_app0.bin`  | 8 KB      | tells the bootloader which app slot to run |
| 0x10000 | `firmware.bin`   | 321,328 B | our program                                |

The 0x10000 write is in the captured output below. The first three are PlatformIO's standard layout for this board and match the files in `.pio/build/01-hello/`. Their lines were cut from the capture.

## Log

Types: `install` adds software to the Mac · `build` compiles, no board involved · `read` asks the board or Mac for information · `reset` restarts the board · `flash` writes to the board's flash memory.

### 1. Install PlatformIO into the project · `install`

```sh
cd test-esp && uv sync && uv run pio --version
```

Why: get the build/flash tool without installing anything globally.

```text
+ platformio==6.2.0  (and 22 dependencies)
PlatformIO Core, version 6.2.0
```

Result: `test-esp/.venv` created (22 MB). `test-esp/uv.lock` pins every version.

### 2. Install the ESP32 platform · `install`

```sh
uv run pio pkg install -g --platform "https://github.com/pioarduino/platform-espressif32/releases/download/55.03.312-1/platform-espressif32.zip"
```

Why: the platform tells PlatformIO how to build for and flash ESP32 chips. `-g` installs it into `~/.platformio`, PlatformIO's shared folder.

```text
Unpacking 100%
Tool Manager: tool-esp_install@5.3.5 has been installed!
```

Result: platform espressif32 55.03.312 and its installer in `~/.platformio`.

### 3. First build of all three lessons · `install` + `build`

```sh
uv run pio run -e 01-hello -e 02-led -e 03-display
```

Why: check that the code compiles. The first build downloads the compiler and the Arduino framework (about 4 GB unpacked, see the table above).

```text
01-hello    SUCCESS  00:16:46   RAM 6.9%  Flash 4.8% (317,128 of 6,553,600 bytes)
02-led      SUCCESS  00:00:08   RAM 7.0%  Flash 5.0%
03-display  SUCCESS  00:00:22   RAM 7.4%  Flash 5.7%
```

Result: no warnings. Nothing was sent to the board.

### 4. Find the board's serial port · `read` (Mac only)

```sh
uv run pio device list
```

Run by you first, then by Claude.

```text
/dev/cu.usbserial-A5069RR4
Hardware ID: USB VID:PID=0403:6001 SER=A5069RR4 LOCATION=2-1.4
Description: FT232R USB UART
```

Result: FTDI FT232R USB-to-serial chip. That's an unusual choice for an S3 board, so step 7 checks what is behind it.

### 5. List USB devices · `read` (Mac only)

```sh
system_profiler SPUSBDataType | grep -iE "FT232|ESP|303a|JTAG|CH34|CP210"
```

Why: look for a second, native-USB port. Result: none. The board only talks through the FT232R.

### 6. Ask the chip who it is (first attempt) · failed, board untouched

```sh
uv run --project test-esp python -m esptool --port /dev/cu.usbserial-A5069RR4 chip-id
```

```text
ModuleNotFoundError: No module named 'rich_click'
```

Result: wrong Python environment. It failed before opening the port.

### 7. Ask the chip who it is · `read` + `reset`

```sh
~/.platformio/penv/bin/python -m esptool --port /dev/cu.usbserial-A5069RR4 chip-id
```

What it does: pulses the DTR/RTS lines to restart the chip into its ROM bootloader, loads a small helper program into **RAM only** (nothing is written to flash), reads the chip info, then restarts the chip normally.

```text
esptool v5.4.0
Detecting chip type... ESP32-S3
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
MAC:                28:84:85:9e:3b:e4
Uploading stub flasher... Running stub flasher... Stub flasher running.
WARNING: ESP32-S3 has no chip ID. Reading MAC address instead.
Hard resetting via RTS pin...
```

Result: a genuine ESP32-S3 with 8 MB PSRAM.

### 8. Read the flash chip · `read` + `reset`

```sh
~/.platformio/penv/bin/python -m esptool --port /dev/cu.usbserial-A5069RR4 flash-id
```

Same mechanism as step 7, RAM only.

```text
Manufacturer: 68
Device: 4018
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse: 3.3V
```

Result: 16 MB flash, so the board really is an N16R8.

### 9. Flash lesson 01 · `flash`

```sh
ESP_PORT=/dev/cu.usbserial-A5069RR4 uv run pio run -e 01-hello -t upload
```

What it does: compiles `01-hello`, restarts the chip into its bootloader and writes the four regions in the flash layout table. Everything else on the flash is left as it was.

```text
Writing at 0x0005e730 [██████████████████████████████] 100.0% 178.48kB/178.48kB [2s]
Wrote 321328 bytes (182765 compressed) at 0x00010000 in 2.9 seconds (871.7 kbit/s).
Verifying written data...
Hash of data verified.
Hard resetting via RTS pin...
01-hello  SUCCESS  00:00:18.481
```

Result: lesson 01 is on the board and the write was verified.

### 10. Watch the board start · `reset` + `read`

```sh
~/.platformio/penv/bin/python scratchpad/readserial.py /dev/cu.usbserial-A5069RR4 9
```

What it does: a small script using pyserial. It restarts the board through the RTS line, then prints 9 seconds of serial output at 115200 baud. It is the same as `uv run pio device monitor`, which needs an interactive terminal.

```text
ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0xb (SPI_FAST_FLASH_BOOT)
entry 0x403c88b8
+---------------- ESP32 identity card ----------------+
| chip      ESP32-S3 rev 2, 2 cores @ 240 MHz
| flash     16 MB   (the N16 part: expect 16)
| psram     8 MB   (the R8 part: expect ~8)
| free heap 337 KB
| mac       E43B9E858428
| sdk       v5.5.5
+-----------------------------------------------------+
[  1529][I][main.cpp:60] setup(): RGB LED on GPIO 48. You should see a slow rainbow.
[  5002][I][main.cpp:76] loop(): alive for 5s, hue=174
```

Result: the program runs. The MAC line shows the bytes reversed. That's a print bug in lesson 01, fixed in the source after this run. The board keeps the old version until the next flash.

### 11. Look at the board · observation by you

The on-board RGB LED cycles through colours, so `PIN_RGB 48` is correct.

### 12. Flash lesson 02 · `flash` · run by you

```sh
uv run pio run -e 02-led -t upload
```

What it does: compiles `02-led` and writes it over `01-hello` (same four flash regions; the program at 0x10000 is now 332,672 bytes). The build finished at 14:04:06.

Response: not captured here; you ran it in your own terminal. Paste it in if you want it recorded.

Result: you see the LED on the breadboard lighting up, so the circuit (GPIO 4 → resistor → LED → GND) is wired correctly. Lesson 02 starts in `fade` mode (a slow breathe), and the on-board RGB LED is switched off.

### 13. Flash lesson 03 · `flash` · run by you

```sh
uv run pio run -e 03-display -t upload
```

What it does: compiles `03-display` (with the Adafruit GFX and ST7789 libraries) and writes it over `02-led`. The program at 0x10000 is now 377,696 bytes. The build finished at 14:33:21.

Response: not captured here; you ran it in your own terminal.

Result: the screen works. The colour check passed, and the bench scope runs at **22 fps** (your reading). The 8 display wires are correct.

### 14. Build lesson 03b · `install` + `build` · no board

```sh
uv run pio run -e 03b-showcase
```

Why: compile the new display showcase. The new environment installed its own copy of the display libraries into `test-esp/.pio/libdeps/03b-showcase`.

```text
Library Manager: Adafruit GFX Library@1.12.6 has been installed!
Library Manager: Adafruit BusIO@1.17.4 has been installed!
Library Manager: Adafruit ST7735 and ST7789 Library@1.11.0 has been installed!
Library Manager: Adafruit seesaw Library@1.7.9 has been installed!
Library Manager: SD@1.3.0 has been installed!
RAM:   18.3% (used 60,128 of 327,680 bytes)
Flash:  6.4% (used 418,832 of 6,553,600 bytes)
03b-showcase   SUCCESS   00:00:23
```

Result: no warnings. BusIO, seesaw and SD came in unpinned, as dependencies of the ST7789 library.

### 15. Pin the library versions · `build` · no board

```sh
uv run pio run -e 03-display -e 03b-showcase
```

First try: listing all three extra libraries in `lib_deps` made PlatformIO compile SD, which does not build for this chip.

```text
*** [.pio/build/03b-showcase/libf37/SD/utility/SdFile.cpp.o] Error 1
03-display     FAILED
03b-showcase   FAILED
```

Fix: pin only `Adafruit BusIO@1.17.4`, which the display uses. Put `SD` and `Adafruit seesaw Library` (meant for other Adafruit boards) in `lib_ignore` so they are never compiled.

```text
03-display     SUCCESS   Flash 371,172 bytes (identical to before)
03b-showcase   SUCCESS   Flash 418,832 bytes
```

Result: both build. Nothing was sent to the board.

### 16. Build lesson 03b with the push button · `build` · no board

```sh
uv run pio run -e 03b-showcase
```

Why: compile the showcase after adding the GPIO 5 button (interrupt + debounce, click / double-click / long press).

```text
RAM:   18.5% (used 60,776 of 327,680 bytes)
Flash:  6.4% (used 422,028 of 6,553,600 bytes)
03b-showcase   SUCCESS   00:00:16
```

Result: no warnings. Nothing was sent to the board.

### 17. Split lesson 03b into two variations · `build` · no board

```sh
uv run pio run -e 03b-showcase -e 03b-showcase-button
```

Why: keep the BOOT-only showcase as variation 1 (`03b-showcase`) and put the push button in variation 2 (`03b-showcase-button`). Each build compiles only its own controls file.

```text
Compiling .pio/build/03b-showcase/src/03b-showcase/controls/boot_only.cpp.o
RAM:   18.3% (used 60,128 bytes)   Flash 419,200 bytes
Compiling .pio/build/03b-showcase-button/src/03b-showcase/controls/with_button.cpp.o
RAM:   18.5% (used 60,776 bytes)   Flash 422,244 bytes
03b-showcase         SUCCESS   00:00:16
03b-showcase-button  SUCCESS   00:00:18
```

Result: no warnings. Variation 1 uses exactly the same RAM as the original BOOT version (60,128 bytes). Nothing was sent to the board.

### 18. Flash lesson 03b, variation 1 · `flash` · run by you

```sh
uv run pio run -e 03b-showcase -t upload
```

What it does: writes the BOOT-only showcase over `03-display`. The firmware file is 426,384 bytes, built at 15:08:54.

Response: not captured here; you ran it in your own terminal.

Result: **it worked at first, then the screen went dark.** Cause not known yet; next step is reading the serial log without resetting the board.

### 19. Variation 2 on the board, push button not reacting · `flash` + observation · run by you

```sh
uv run pio run -e 03b-showcase-button -t upload
```

You report running variation 2. The firmware file (429,504 bytes) was not rebuilt after my build at 15:09:12, so the exact flash time is unknown. Response: not captured here.

What you see: the four scenes (3D torus, Plasma, Fireworks, Dashboard) cycle every 12 s, so the dark screen from step 18 is solved; most likely a jumper had moved, fixed by unplugging and replugging. **Pressing the push button gets no reaction.**

Next checks: whether the press dot shows in the bottom-right corner (variation 2 only) and whether it lights on a press. Then the start-up log (`tools/read_serial.py … --reset`) to confirm the variation and see button events.

### 20. Cause found: button signal wire on GPIO 4 · observation by you

Your photo showed the ESP32 sits in a green screw-terminal expansion board, not in the breadboard. The guide's "free hole in the GPIO 5 row" instruction therefore did not apply: breadboard holes are not connected to the ESP32 here.

You traced the wires: the GND wire ended on GND (correct), but the signal wire ended on **GPIO 4**, the socket next to 5. Variation 2 listens on GPIO 5, so presses were never seen. GPIO 4 is not driven by variation 2, so nothing was harmed.

Fix: move the signal wire to socket 5 (USB unplugged). Guide and README now describe wiring to the ESP32's own sockets.

### 21. Build lesson W1 (Wi-Fi scanner) · `build` · no board

```sh
uv run pio run -e w1-scan -e 03b-showcase -e 03b-showcase-button
```

Why: first lesson of the Wi-Fi track. The screen, button and frame-buffer code moved into `lib/bench/` so every lesson shares it; the two 03b variations were rebuilt to prove they still compile.

First try failed on a name clash: Arduino-ESP32 3.x already has a global object called `Network`.

```text
src/w1-scan/main.cpp:56:11: error: 'Network' does not name a type
w1-scan              FAILED
03b-showcase         SUCCESS
03b-showcase-button  SUCCESS
```

Fix: the scan result type is now `ScanResult`.

```text
RAM:   14.5% (used 47,660 of 327,680 bytes)
Flash: 14.7% (used 962,951 of 6,553,600 bytes)   (the Wi-Fi stack adds about 540 KB)
w1-scan        SUCCESS   00:00:14
```

Result: no warnings. Nothing was sent to the board.

### 22. Merge the Wi-Fi labs: build W1 Scan & join · `build` · no board

```sh
uv run pio run -e wifi-lab
```

Why: the Wi-Fi track is now 3 labs plus a bonus, all growing one firmware (`wifi-lab`, which replaces the never-flashed `w1-scan`). W1 adds joining a network (password typed over serial, stored in NVS, never logged), automatic reconnect with plain-language disconnect reasons, internet time, `bench.local` via mDNS, and a status view.

First build: two format warnings (`WiFi.channel()` returns a 32-bit long), fixed with casts.

```text
RAM:   15.8% (used 51,756 of 327,680 bytes)
Flash: 16.0% (used 1,051,143 of 6,553,600 bytes)
wifi-lab       SUCCESS   00:00:13
```

Result: no warnings. Nothing was sent to the board.
