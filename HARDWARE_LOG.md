# Hardware log

Every command that installs software, talks to the board, or flashes it, with what it returned. Newest at the bottom.

**Rule for this bench:** Claude says what a command does before running it. Anything that installs, resets, reads or flashes the board waits for your go-ahead.

## Overview

| What                      | Value                                                                                                                                                                                           |
| ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Board                     | ESP32-S3-N16R8 DevKitC clone                                                                                                                                                                    |
| Chip                      | ESP32-S3 (QFN56) revision v0.2, dual core + LP core, 240 MHz, Wi-Fi + BT 5 (LE), 40 MHz crystal                                                                                                 |
| Flash                     | 16 MB, quad mode, 3.3 V (manufacturer 0x68, device 0x4018)                                                                                                                                      |
| PSRAM                     | 8 MB, embedded in the chip                                                                                                                                                                      |
| MAC                       | 28:84:85:9E:3B:E4                                                                                                                                                                               |
| USB connection            | `/dev/cu.usbserial-A5069RR4`, FTDI FT232R USB-to-serial (VID:PID 0403:6001)                                                                                                                     |
| On-board RGB LED          | GPIO 48 (confirmed: it cycles colours)                                                                                                                                                          |
| RFID reader               | RC522 board with an **FM17522 clone** chip (VersionReg B2h), not an NXP MFRC522; SPI3 on GPIO 15/16/17/18, RST 14, IRQ 13                                                                       |
| Display                   | 1.9" ST7789 170 × 320 on SPI (GPIO 12/11/10/9/8/7), working at 22 fps                                                                                                                           |
| **Firmware on board now** | **`rfid-lab`** with the entry 37–38 fixes, flashed by you: R1 all working. Built, not flashed: `rfid-lab-r1` … `rfid-lab-r6`, `wifi-lab-w1`, `wifi-lab-w2`, `wifi-lab-w2-remote`, `wifi-lab-w3` |
| Flashes so far            | 12 (01-hello by Claude; 02-led, 03-display, 03b v1, 03b v2, wifi-lab W2 five times, 03-display again, rfid-lab by you)                                                                          |

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

### 23. Build lab W2 (phone control) · `install` + `build` · no board

```sh
uv run pio run -e wifi-lab
```

Why: W2 adds a REST API + WebSocket, the built-in phone page, the `bench-setup` network with a set-up page, firmware updates over HTTP and an admin token. Three pinned libraries were downloaded into `test-esp/.pio/libdeps/wifi-lab`.

```text
Library Manager: ESPAsyncWebServer@3.12.1 has been installed!
Library Manager: AsyncTCP@3.5.0 has been installed!
Library Manager: ArduinoJson@7.4.3 has been installed!
```

First try: a raw string `R"(...)"` contained `(admin)"`, which ends it early, so every later line failed. Fixed with a named delimiter `R"JSON(...)JSON"`. Second try: one `-Wvolatile` warning, fixed.

```text
RAM:   16.2% (used 53,100 of 327,680 bytes)
Flash: 18.1% (used 1,184,658 of 6,553,600 bytes)
wifi-lab       SUCCESS   00:00:24
```

Result: no warnings. Nothing was sent to the board.

### 24. Install and check the desktop dashboard · `install` + `build` · no board

```sh
cd test-esp/dashboard && pnpm install && pnpm typecheck && pnpm test && pnpm build
```

Why: the React + Node dashboard is its own pnpm project (own `pnpm-workspace.yaml` and lockfile), so the monorepo's lockfile is untouched. 129 packages went into `test-esp/dashboard/node_modules`.

pnpm 11 refuses releases younger than its minimum age by default; `zod 4.6.6` was published the same day, and pnpm added an exception on its own. Removed the exception and pinned `zod 4.6.5` instead (four weeks old).

```text
typecheck: Blob rejected a Node Buffer (SharedArrayBuffer typing) -> copy into Uint8Array
typecheck: missing CSS module types -> "types": ["vite/client"]
Test Files  2 passed (2)   Tests  6 passed (6)
dist/web/assets/index-*.js   236.97 kB │ gzip: 74.52 kB
```

Smoke test with no board (board URL pointed at a closed local port):

```text
GET /healthz            {"ok":true,"esp":"down","clients":0}
PUT /api/led r=300      {"error":"Too big: expected number to be <=255"}
POST /api/reboot        {"error":"Admin actions are off: set ESP_TOKEN in test-esp/dashboard/.env ..."}
GET /                   200 text/html (the built React app)
```

Result: ready. Nothing was sent to the board or to your network.

### 25. W2 on the board, but unreachable: diagnosis · `read` + `reset` · run by Claude, with your go

You flashed `wifi-lab` (W2) and joined your Wi-Fi: the status view showed a teal dot and IP `192.168.178.54`. Phone and Mac both reported "site unreachable", for `bench.local`, `http://bench.local` and the IP.

Checks on the Mac only: the Mac is on the same network (`192.168.178.31`, router `192.168.178.1`), and the built `firmware.bin` contains the W2 web server.

```sh
ping -c 3 192.168.178.54
curl -s -m 5 http://192.168.178.54/api/status
dscacheutil -q host -a name bench.local
uv run python tools/read_serial.py /dev/cu.usbserial-A5069RR4 15
```

```text
ping: 3 packets transmitted, 0 packets received, 100.0% packet loss
curl: HTTP 000 (timeout after 5 s, then "couldn't connect")
bench.local -> 192.168.178.54   (the board's mDNS announcements did get through)
[  2233][I][web_api.h:138] begin(): Web server on port 80 ...
[  2285][E][scanner.h:37] start(): Could not start a scan
[  3879][I][connection.h:105] loop(): Reachable as bench.local ...
[ 13233][I][main.cpp:400] loop(): scan 1 done: 10 networks in 3227 ms
[ 13298][I][main.cpp:191] scanNow(): scan started (auto)      <- 65 ms later, should be 10 s
```

**Cause (bug in the firmware, also present in W1):** the loop read the clock before checking whether the scan had finished. The finish time was later than that reading, so `now - finishedAt` wrapped around to a huge number and the board started a new scan immediately, forever. While scanning, the radio keeps leaving the router's channel, so pings and web requests were lost.

**Fixes (built, not flashed yet):** compare against `millis()` after the check; when joined, auto-scan every 60 s instead of 10 s (each scan takes the radio away for ~3 s); no start-up scan while joining; no error log when no time zone is saved.

**Correction:** the serial capture began with `rst:0x1 (POWERON)`. Opening the port restarted the board even without `--reset`, because the FT232R on macOS pulses the reset line while the port opens. Harmless; `tools/read_serial.py` now says so.

### 26. Scan-loop fix on the board, still unreachable · `flash` run by you + `read` (Mac only) by Claude

You flashed the fix from entry 25. The serial log shows it working: the board joined `FRITZ!Box 5530 GK` (IP `192.168.178.54`, channel 11, -54 dBm) and the next auto-scan came 60 s after the previous one instead of 65 ms. The page was still unreachable.

```text
[165947][I][connection.h:100] loop(): Connected to 'FRITZ!Box 5530 GK': IP 192.168.178.54, channel 11, -54 dBm
E (165956) mdns_responder: mdns_service_add_for_host_base(865): Service already exists
[186751][I][main.cpp:198] scanNow(): scan started (auto)
[189973][I][main.cpp:408] loop(): scan 3 done: 10 networks in 3222 ms
```

Checks on the Mac only (no USB, the board is not touched):

```sh
ifconfig; route -n get 192.168.178.54; ping -c 3 192.168.178.54; arp -n 192.168.178.54
curl -s -m 5 http://192.168.178.54/api/status
arp -an; dns-sd -G v4 bench.local; system_profiler SPAirPortDataType
```

```text
Mac en0 192.168.178.31/24, route to .54 via en0 (same network, no VPN in the way)
ping: 3 transmitted, 0 received
arp: 192.168.178.54 at (incomplete)       <- the board never answered "who has .54?"
curl: exit 7, couldn't connect (0.002 s, no address to send to)
Mac Wi-Fi: 5 GHz channel 116, WPA2. Board: 2.4 GHz channel 11
Only the router (192.168.178.1) in the Mac's ARP table
bench.local -> 192.168.178.54, flag 0x40000000 = answered from the Mac's cache, not live
```

**Reading:** the problem sits below the web server. The board doesn't even answer ARP, so either its radio misses the requests or the router doesn't pass them from one Wi-Fi device to another.

**Firmware changes (built, not flashed yet, 1,191,178 bytes, no warnings):**

- `WiFi.setSleep(false)` once joined. ESP32 power save lets the radio doze between router beacons; with FRITZ!Box routers this is a known cause of "joined, but no ping or ARP".
- mDNS starts once, which removes the `Service already exists` error on reconnects.
- New serial command `ping` (the router) or `ping <ip>` (e.g. the Mac, `ping 192.168.178.31`) so the board can test the network from its side.

**Router setting to check (FRITZ!Box, `http://fritz.box`):** WLAN → Sicherheit (Security) → "Die unten angezeigten WLAN-Geräte dürfen untereinander kommunizieren" ("WLAN devices may communicate with each other") must be ticked. Also check that the board appears under Heimnetz → Netzwerk as `bench`.

### 27. Power-save fix on the board, cause found: the router isolates Wi-Fi devices · `flash` + `read` run by you, `read` (Mac only) by Claude

You flashed the power-save fix and used the new `ping` command in the serial monitor:

```text
ping
[102867][I][pinger.h:41] start(): ping 192.168.178.1 (4 packets)...
[106872][I][pinger.h:64] onEnd(): ping done: 4 sent, 4 answered      (1-2 ms each)
ping 192.168.178.31
[116515][I][pinger.h:41] start(): ping 192.168.178.31 (4 packets)...
E (131935) ping_sock: send error=0                                    <- no ARP answer from the Mac
[120541][I][pinger.h:64] onEnd(): ping done: 3 sent, 0 answered
```

Checks on the Mac only:

```sh
ping -c 3 192.168.178.54; arp -n 192.168.178.54; curl -s -m 5 http://192.168.178.54/api/status
/usr/libexec/ApplicationFirewall/socketfilterfw --getglobalstate --getstealthmode
```

```text
ping: 0 of 3 answered · arp: (incomplete) · curl: exit 7
Firewall is disabled. Stealth mode is off.
```

**Cause (not the firmware):** both devices reach the router, but neither can even find the other's address (ARP fails in both directions), and the Mac's firewall is off. The FRITZ!Box is not passing traffic between Wi-Fi devices.

**Fix on the router:** `http://fritz.box` → WLAN → Sicherheit → tick "Die unten angezeigten WLAN-Geräte dürfen untereinander kommunizieren" → Übernehmen (Apply). Works right away, no reflash.

**Workaround without touching the router:** type `setup` in the serial monitor, join the Wi-Fi `bench-setup` from the phone or Mac (password shown on the screen) and open `http://192.168.4.1`.

### 28. Router setting changed: phone works, Mac browser doesn't · `read` (Mac only) by Claude

After the FRITZ!Box setting change the phone page opened on your phone. From the Mac:

```sh
ping -c 3 192.168.178.54; arp -n 192.168.178.54
curl http://192.168.178.54/api/status; curl http://192.168.178.54/; curl http://bench.local/; curl https://192.168.178.54/
dns-sd -G v4 bench.local; scutil --proxy
```

```text
ping: 3 of 3 answered (4-9 ms) · arp: 28:84:85:9e:3b:e4 (the board's MAC)
/api/status -> 200 in 0.017 s · / -> 200, 7,946 bytes (the phone page)
http://bench.local/ -> curl gave up resolving after 5 s; dns-sd resolves it to 192.168.178.54
https://... -> refused (the board only speaks http)
no proxy configured
```

**Reading:** the Mac's network path to the board works now; whatever blocks it is in the browser. Likely candidates: macOS Local Network permission for Chrome/Firefox (System Settings → Privacy & Security → Local Network), the browser switching to `https://`, or a cached error page.

### 29. Reboot from the app, reflash: screen lit but blank · `flash` run by you, `read` (Mac only) by Claude

You pressed Reboot in the app, then reflashed. The serial monitor shows the Wi-Fi messages, so the firmware runs; the screen's backlight is on but the screen shows nothing. Setup stops before Wi-Fi if the screen can't get its frame buffer, so the firmware believes it is drawing.

```sh
curl -s -m 5 http://192.168.178.54/api/status
```

```text
{"endpoints": ["GET /api/status", "GET /api/networks", ...]}      <- the API index, not the status
```

**Bug found on the way (firmware, not the screen):** a route registered as `/api` also matches every `/api/...` URL, and it was registered first, so `GET /api/status` and `GET /api/networks` returned the index. PUT/POST routes were unaffected (that's why Reboot worked). Fixed with `AsyncURIMatcher::exact("/")` and `exact("/api")`; built (1,191,350 bytes, no warnings), not flashed.

**Screen:** same firmware that drew fine earlier tonight, backlight works, so most likely a data wire (SCL 12, SDA 11, CS 10, DC 9, RES 8) lost contact, or the panel needs a real power cycle (a reboot or flash keeps it powered).

### 30. Screen still blank, also with lesson 03 · `flash` run by you · pin check built by Claude

You reflashed `wifi-lab`, then the known-good `03-display`: both leave the screen black with the backlight on. You haven't touched the wires.

**Reading:** two different programs that both drew fine before now show nothing, so it's not the firmware. A reboot can't cause it either (`ESP.restart()` is a software restart and nothing about the screen is saved). That leaves a contact on one of the data wires (screw terminal, jumper end, header on the screen) or the screen module itself. Plugging and unplugging USB moves the board enough to loosen a jumper.

**New diagnostic env `pin-check` (built, not flashed, 321,144 bytes):**

- short test at start: finds a screen data pin touching GND, 3V3 or a neighbouring data pin;
- probe mode: each data pin (SCL 12, SDA 11, CS 10, DC 9, RES 8) blinks for 8 s in turn, so an LED with resistor held between the screen's pin and GND shows whether the signal arrives at the screen end. Serial: `next`, `short`, `help`.

```sh
uv run pio run -e pin-check
```

### 31. Screen back · W1 and W2 split into their own firmwares · `build` · no board

You got the screen working again and confirmed it with `03-display` (a contact issue: same firmware, nothing changed in code).

Then the Wi-Fi labs were split, at your request, so each has its own code and command:

| Lab | Command                                   | Code               |
| --- | ----------------------------------------- | ------------------ |
| W1  | `uv run pio run -e wifi-lab-w1 -t upload` | `src/wifi-lab-w1/` |
| W2  | `uv run pio run -e wifi-lab-w2 -t upload` | `src/wifi-lab-w2/` |

Shared code (scanner, connection, ping, the four scanner/status screens) moved to `lib/wifi-kit`. W1 has no web server, set-up network or token. Both keep the saved network in the same NVS namespace, so switching labs keeps it. The push button on GPIO 5 is optional in both: it does what BOOT does, and with nothing wired the internal pull-up keeps it quiet. The old `wifi-lab` env is gone. The W2 build includes the `/api` route fix from entry 29.

```sh
uv run pio run -e wifi-lab-w1 -e wifi-lab-w2
cd dashboard && pnpm typecheck && pnpm test
```

```text
wifi-lab-w1  RAM 15.8% (51,908 B)  Flash 16.3% (1,069,914 B)  SUCCESS
wifi-lab-w2  RAM 16.2% (53,108 B)  Flash 18.2% (1,191,490 B)  SUCCESS
no warnings · dashboard: typecheck ok, 6 tests passed
```

### 32. W2 variation 2, remote control: built on the Mac, firmware build waiting · `build` + `read` · no board

You asked for remote control through a server the board connects to: a tunnel today, a hosted server later. Design: the board dials **out** (`wss://<public host>/device`, signed in with its admin token) and keeps the connection open; the dashboard server accepts it and relays requests down it. Nothing opens on the router.

Checks before writing code (read-only, Mac only):

```sh
which brew cloudflared ngrok; cloudflared --version; ngrok version
ls "~/Library/Application Support/ngrok/ngrok.yml"     # only checked that an authtoken line exists, not printed
uv run pio pkg show links2004/WebSockets               # registry lookup, nothing installed
grep CONFIG_MBEDTLS_CERTIFICATE_BUNDLE .../sdkconfig.h; nm libmbedtls.a | grep x509_crt_bundle
gh api repos/Links2004/arduinoWebSockets/contents/src/WebSocketsClient.h   # API check, saved to the scratchpad
```

```text
cloudflared 2026.7.1 · ngrok 3.39.8 (account linked, no static domain configured)
links2004/WebSockets 2.7.3 (published 2026-01-07): beginSslWithBundle(host, port, url, bundle, size)
Arduino core: full Mozilla CA bundle built in (CERTIFICATE_BUNDLE_DEFAULT_FULL), cert dates not checked
```

What changed:

| Where                                                            | What                                                                                                                                               |
| ---------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/wifi-lab-w2/api_rules.h`                                    | request rules shared by the local web server and the remote link                                                                                   |
| `src/wifi-lab-w2/remote.h`, `remote/off.cpp`, `remote/relay.cpp` | the remote link (own FreeRTOS task, TLS with the CA bundle, heartbeat, reconnect), off in variation 1                                              |
| `src/wifi-lab-w2/main.cpp`                                       | serial `relay` / `relay <url>` / `relay off`, `remote.state` in the status, teal ring around the link dot                                          |
| `platformio.ini`                                                 | `wifi-lab-w2` (variation 1) and `wifi-lab-w2-remote` (+ `links2004/WebSockets@2.7.3`)                                                              |
| `dashboard/server`                                               | `BoardLink` with LAN and relay links, `/device` (token checked before the upgrade), sign-in with `DASHBOARD_KEY`, `TRUST_PROXY` for hosted servers |
| `dashboard/web`                                                  | sign-in screen, "Live · remote" badge                                                                                                              |

```sh
uv run pio run -e wifi-lab-w2
cd dashboard && pnpm typecheck && pnpm test && pnpm build
# smoke test, Mac only: relay server on :8799 with throwaway secrets + a fake board script on /device
```

```text
wifi-lab-w2  Flash 18.2% (1,192,206 B)  SUCCESS, no warnings
dashboard: typecheck ok · 12 tests passed · build ok
smoke: wrong board token -> 401 · no sign-in -> 401 · wrong key -> 401 · right key -> cookie
       PUT /api/led -> fake board got the request, its reply came back (200)
       OTA in relay mode -> 501 · board gone -> 502 with an explanation
```

**Not done yet:** building `wifi-lab-w2-remote` downloads `links2004/WebSockets` 2.7.3 into `test-esp/.pio`, so it waits for your go. Nothing was sent to the board.

### 33. Build `wifi-lab-w2-remote` · `install` + `build` · first try by you, fix by Claude · no board

You ran the build: it downloaded `links2004/WebSockets` 2.7.3 into `test-esp/.pio/libdeps/wifi-lab-w2-remote/` and then failed on my code:

```text
src/wifi-lab-w2/remote/relay.cpp:152:12: error: 'class String' has no member named 'swap'
wifi-lab-w2-remote  FAILED    00:00:53.143
```

Arduino's `String` has no `swap()` (the standard library's strings do). Fixed with `std::move` plus resetting the mailbox strings, then rebuilt:

```sh
uv run pio run -e wifi-lab-w2-remote
```

```text
RAM:   16.4% (used 53,884 of 327,680 bytes)
Flash: 21.2% (used 1,390,454 of 6,553,600 bytes)      <- +198 KB over variation 1: TLS client + WebSocket library
wifi-lab-w2-remote  SUCCESS   00:00:21
```

No warnings. Nothing was sent to the board.

### 34. Build lab W3, Internet & AI · `build` + `read` · no board

Your request: HTTPS + JSON from the internet (weather), plus an AI API for something creative. New firmware `wifi-lab-w3` (own folder and command, button optional, Wi-Fi from W1/W2):

- weather from Open-Meteo (no key): city lookup, now / 24 hours / 7 days, ArduinoJson filter;
- the Bench Oracle: Claude through the Messages API with one forced tool (`show_on_bench`), so the answer is JSON (title, lines, LED colour, scene); haiku or `ask <question>`;
- all HTTPS in a worker task (`worker.h`) with the firmware's CA bundle (`lib/wifi-kit/src/https_client.h`, shared);
- API key in NVS via serial, never logged (last 4 characters only).

Checks on the Mac (read-only, sample location Berlin, no key involved):

```sh
curl "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=Berlin"
curl "https://api.open-meteo.com/v1/forecast?latitude=52.5244&longitude=13.4105&...&forecast_hours=24&...&forecast_days=7"
uv run pio run -e wifi-lab-w3
```

```text
geocoding: Berlin, Germany, 52.52437, 13.41053
forecast: HTTP 200, 1,701 bytes, 0.06 s · 24 hourly values · 7 days · time strings = 30 % of the answer
first build: "return-statement with a value" (return log_w(...) in void functions) -> split into log + return
wifi-lab-w3  RAM 16.3% (53,412 B)  Flash 20.1% (1,319,606 B)  SUCCESS, no warnings
```

**Not tested yet:** the Claude request (needs your API key on the board). Nothing was sent to the board.

### 35. W3 through LiteLLM (any provider), clearer request diagram · `build` + `read` · no board

Your request: use LiteLLM for the AI so any provider and model works, and make the guide's animations and diagrams self-explanatory.

- **Board:** the oracle now sends one OpenAI-style request (`POST /v1/chat/completions`, a forced `show_on_bench` function) to a LiteLLM gateway; the answer's `tool_calls[0].function.arguments` (a JSON string) is parsed a second time. New serial `ai url <url>`; `ai model <name>` picks a `model_name` from the gateway's config (default `oracle`). The HTTP helper now also does plain `http://` for a gateway on your network. Provider keys never reach the chip.
- **`test-esp/ai-gateway/`** (new uv project): `litellm[proxy]==1.101.2`, `config.yaml` (`oracle` → Claude Opus, `oracle-fast` → Claude Haiku, commented OpenAI / Gemini / Ollama), `.env.example` (`LITELLM_MASTER_KEY`, provider keys), README. Not installed yet.
- **Guide:** the W3 step list became an animated sequence diagram (lanes for loop(), the worker, the service; play / back / next; each step's message, explanation and real payload; a live frame counter and a "worker waiting" bar showing the screen never waits).

Checks (read-only):

```sh
curl https://pypi.org/pypi/litellm/json        # releases and dates
WebFetch docs.litellm.ai/docs/proxy/configs    # config keys
uv run pio run -e wifi-lab-w3
```

```text
litellm newest: 1.105.0 (2026-10-11). Pinned 1.101.2 (2026-09-24): newest release at least two weeks old
docs confirm: model_list + os.environ/..., general_settings.master_key, litellm_settings.drop_params
wifi-lab-w3  Flash 20.2% (1,322,878 B)  SUCCESS, no warnings
```

**Waiting for your go:** `cd test-esp/ai-gateway && uv sync` (installs LiteLLM into `test-esp/ai-gateway/.venv`). Nothing was sent to the board.

### 36. RFID lab R1–R6: firmware, driver and guide chapter · `build` + `read` · no board

Your request: a new advanced, step-by-step learning chapter for the RC522 with its white card and key tag, with edge testing, lots of interaction and animation, and 3D.

- **Driver `lib/rfid-kit`** (no RFID library): `mfrc522.h` (SPI address byte, registers, FIFO, timer, polling or IRQ waits, transceive, CRC coprocessor), `iso14443a.h` (WUPA/REQA, anticollision through cascade levels, select, halt), `mifare_classic.h` (factory key A only, read, write, value operations; never writes block 0 or trailers), `rfid_format.h` (CRC_A, BCC, access bits, value blocks), `trace.h` (frame sniffer; keys never recorded).
- **Firmware `rfid-lab`**: the reader in its own task on core 0 on SPI3 (the screen keeps SPI2), six views (chip, card, trace, memory, credits, edge), serial commands for R1–R6, seven edge experiments (SPI clock, gain, timeout, timing, IRQ vs polling, CRC, stress). Pins: SDA 15, SCK 16, MOSI 17, MISO 18, RST 14, IRQ 13 (optional), 3.3V on the second 3V3 pin.
- **Guide**: a new "RFID & NFC" theme with lessons R1–R6: a 3D field model (three.js) with a range chart, a 3D memory map, bit-level SPI and anticollision walk-throughs, access-bit and value-block decoders, step-by-step frame diagrams (the W3 diagram, now reusable), edge charts that plot pasted serial output, and a simulator.

Checks (read-only or local):

```sh
# NXP MFRC522 datasheet rev 3.9: self-test tables (16.1.1), VersionReg 91h/92h, SPI 10 Mbit/s, 2.5–3.6 V, RxGain table, IRQ bits
uv run pio run -e rfid-lab
c++ -std=c++17 -Wall -I lib/rfid-kit/src test/host/rfid_format_test.cpp -o <scratch>/rfid_test && <scratch>/rfid_test
node check.mjs   # the repo's Playwright 1.63, headless Chromium, guide lessons R1–R6 at 1280 and 390 px, light and dark
```

```text
first build: return-statement with a value (return log_w(...) in void functions) -> warn() helper
rfid-lab  RAM 9.4% (30,764 B)  Flash 6.7% (436,695 B)  SUCCESS, no warnings
all rfid format checks passed
guide: every RFID lesson renders, three.js 0.160.0 loads from jsDelivr, no page errors, no sideways scroll at 390 px
```

**Waiting for your wiring and your go:** `uv run pio run -e rfid-lab -t upload`. No new libraries, nothing installed. Nothing was sent to the board.

### 37. rfid-lab on the board: an FM17522 clone, and a chip that went quiet · `flash` + `read` + `build`

You flashed `rfid-lab` (`uv run pio run -e rfid-lab -t upload`) and ran R1 in the monitor. Your output:

```text
00:17:08  VersionReg 0xB2: FM17522 (clone)
00:17:39  VersionReg 0xB2: FM17522 (clone)
00:17:56  unknown command 'chip' (type help)
00:17:59  VersionReg 0xB2: FM17522 (clone)
00:18:22  card removed
00:18:32  Self-test (datasheet 16.1.1), chip 0x00, FIFO level 0:   64 × 00
          self-test: no reference for chip 0x00 (a clone)
```

What it means:

- **The reader chip is a clone.** B2h is an FM17522 (Fudan), not NXP's MFRC522 (91h/92h). The datasheet's self-test table is for NXP chips only, so the old firmware could never say "passed" on this board.
- **The chip went quiet between 00:17:59 and 00:18:32.** The self-test reads VersionReg before it starts, and it read 00h: nothing was driving MISO. The only thing that happened in between was handling the card. With polling on (the default), a silent chip also produces `card removed` after 3 missed polls. Most likely a header pin lost contact. Polling itself can't interfere: polls and commands run one after the other in the same task.
- **`unknown command 'chip'`:** a backspace typed in the monitor arrived as a character inside the line.

Fix (built on the Mac, not flashed):

- a VersionReg watchdog every 2 s: if the chip goes silent it says so, hard-resets it, and reports "it had hung" or "still 0x00: check 3.3V, GND, MISO"
- a new `reset` command (RST pulse, set-up, VersionReg)
- `selftest` refuses to run on a silent chip, and on clones runs twice and compares (result "consistent", or "not supported" if the clone has no self-test)
- backspace and other control characters handled in serial input
- guide R1: the self-test card says "card doesn't matter", new `reset` card, the clone results

```sh
uv run pio run -e rfid-lab
```

```text
rfid-lab  RAM 9.4% (30,772 B)  Flash 6.7% (438,307 B)  SUCCESS, no warnings
```

**Waiting for your go:** flash the fix (`uv run pio run -e rfid-lab -t upload`). Before that, `chip` on the current firmware tells you whether the chip answers again by itself.

### 38. rfid-lab: the LED shows every card read · `build` · no board

Your request: the LED must be on when a card is read. Before, the on-board RGB LED gave a 150 ms white flash for a new card only, then a dim teal (about 15 % brightness).

- **Bright green flash (300 ms)** every time a card is read: a new card on the reader, or any command that reaches the card (`dump`, `read`, `write`, `credits`, and `inventory` once per card it finds). The edge-lab experiments don't flash it: they read the card hundreds of times.
- **Steady green** while a card lies on the reader.
- **Red** when the reader chip stops answering (VersionReg 00h/FFh), so a loose wire shows at a glance.
- **Off** with no card. The LED is only rewritten when its colour changes.

```sh
uv run pio run -e rfid-lab
```

```text
rfid-lab  RAM 9.4% (30,780 B)  Flash 6.7%  SUCCESS, no warnings
```

**Waiting for your go:** flash it (`uv run pio run -e rfid-lab -t upload`); it includes the entry 37 fixes. Nothing was sent to the board.

### 39. One firmware per RFID lab · `build` · no board

Your request: each lab has its own code and its own command, like the Wi-Fi labs. Your R1 run before this: everything working.

- The single `rfid-lab` env became `rfid-lab-r1` … `rfid-lab-r6`. Each `src/rfid-lab-rN/main.cpp` lists that lab's screen views (with its double-click action) and serial commands; `help` shows only those.
- The shared code moved to `lib/rfid-lab-kit`: the reader task, views, edge experiments, every command (`lab_commands.h`) and the lab shell (`lab.h`: screen, button, LED, serial). The driver stays in `lib/rfid-kit`.
- Guide: each RFID lesson has its own flash command and a flash card at the top of its command list; the preview only knows that lab's views and commands.

```sh
uv run pio run -e rfid-lab-r1 -e rfid-lab-r2 -e rfid-lab-r3 -e rfid-lab-r4 -e rfid-lab-r5 -e rfid-lab-r6
```

```text
rfid-lab-r1  Flash 437,519 B  SUCCESS
rfid-lab-r2  Flash 437,875 B  SUCCESS
rfid-lab-r3  Flash 437,963 B  SUCCESS
rfid-lab-r4  Flash 438,227 B  SUCCESS
rfid-lab-r5  Flash 439,191 B  SUCCESS
rfid-lab-r6  Flash 439,959 B  SUCCESS   no warnings in any
```

Nothing was sent to the board. For R2: `uv run pio run -e rfid-lab-r2 -t upload`.

### 40. R7 Card manager: firmware, React app, guide lesson · `build` (Mac) · waiting for your go

Your request: a lab that controls the RFID module from a simple React app to manage your cards: add, reset, add information, block, open a door, and other scenarios.

- **Firmware `rfid-lab-r7`** (`src/rfid-lab-r7`): a card list in NVS (`registry.h`, up to 20 cards: name, role, blocked, end date, hours, uses left), the access rules (`access_rules.h`: unknown → blocked → lockdown → no clock → expired → outside hours → no uses left → granted), a reader task that decides each tap and runs the app's jobs (`door.h`: enroll, label, wipe, inspect, open; factory key and data blocks of sectors 1-2 only, never keys, trailers or block 0), a JSON API + WebSocket (`door_api.h`, admin token for every change), three screen views (door, cards, log) and LED states.
- **The admin token** moved from `src/wifi-lab-w2` to `lib/wifi-kit/src/api_token.h` so W2 and R7 share it.
- **App `test-esp/door-app`**: React 19 + Vite 8 + zod (the dashboard's exact versions); Vite's proxy adds the token from `.env`. Door panel, cards with scenario presets, add-card flow, live log.
- **Guide R7**: goal and commands, a "Which rule decides?" checker, step-through diagrams (a tap, a block, a new card), a door preview.

Checks on the Mac (nothing installed: the app was type-checked and tested through a temporary link to the dashboard's identical packages, and shown against a mock board script; link and mock removed afterwards):

```sh
c++ -std=c++17 -Wall -I src/rfid-lab-r7 test/host/door_rules_test.cpp -o <scratch>/door_test && <scratch>/door_test
uv run pio run -e wifi-lab-w2 -e wifi-lab-w2-remote      # after moving api_token.h
door-app: tsc -p . · vitest run · vite + mock board + headless Chromium (1280 / 390 px)
```

```text
all door rule checks passed
wifi-lab-w2 SUCCESS · wifi-lab-w2-remote SUCCESS (no downloads)
door-app: typecheck clean · 6 tests passed · no page errors, no sideways scroll
```

**Waiting for your go:**

1. `uv run pio run -e rfid-lab-r7` (first build: copies ESPAsyncWebServer 3.12.1, AsyncTCP 3.5.0 and ArduinoJson 7.4.3, the libraries W2 already uses, into `test-esp/.pio/libdeps/rfid-lab-r7`). The firmware is not compiled yet.
2. `cd test-esp/door-app && pnpm install` (the same packages the dashboard has).

Nothing was sent to the board.

### 41. R7: copy, save and replace cards · `build` (Mac) · waiting for your go

Your request: operations like copying one card to another and saving a card's information.

- **Firmware `rfid-lab-r7`**: a snapshot store (`snapshots.h`, NVS namespace `snaps`, up to 8) and new reader jobs in `door.h`: save the card on the reader, write a snapshot back (restore or onto another card), copy card → card (read the first card, wait 20 s for a different one, write and read back each block), replace a lost card (a new card takes over its name and rules; the old one stays blocked as "… old"). One shared "waiting for a card" state for enroll, replace and copy; `POST /api/cancel` stops any of them. New endpoints: `GET /api/snapshots`, `POST /api/save`, `POST /api/import`, `POST /api/restore?id=`, `DELETE /api/snapshot?id=`, `POST /api/copy`, `POST /api/replace?uid=`. Serial: `saved`, `save [name]`, `restore <id>`, `copy`, `cancel`. Screen: REPLACE A CARD and COPY: 2ND CARD; LED blinks violet while a copy waits.
- **Same guard rails**: factory key only, lab blocks 4-6 and 8-10 only, never block 0 (the UID) or a trailer, only where the access bits let key A write. A copy keeps its own UID, so the door sees a different card.
- **App**: a Card data panel (save, copy with live steps, import, snapshots with a block strip, write back, compare, download, delete), Copy rules from… and Replace with a new card in the editor.
- **Guide R7**: a "What a copy moves" panel, a copy flow in the step-through diagram, a copy button in the door preview, seven new command cards.

Checks on the Mac (nothing installed; the app through the temporary link to the dashboard's packages, removed afterwards):

```sh
c++ -std=c++17 -Wall -I src/rfid-lab-r7 test/host/door_rules_test.cpp -o <scratch>/door_test && <scratch>/door_test
door-app: tsc -p . · vitest run · vite + mock board + headless Chromium (1280 / 390 px, light / dark, mid-copy)
guide: headless Chromium, #r7 (copy panel, copy flow, preview) at 1280 / 390 px
```

```text
all door rule checks passed
door-app: typecheck clean · 10 tests passed · no page errors, no sideways scroll
guide: no page errors, no sideways scroll
```

**Still waiting for your go** (same as entry 40): `uv run pio run -e rfid-lab-r7` (first build, copies W2's three web libraries into `.pio/libdeps/rfid-lab-r7`), then `pnpm install` in `test-esp/door-app`. The R7 firmware, with these additions, has not been compiled yet. Nothing was sent to the board.
