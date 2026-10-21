// Wi-Fi lab W1: scan & join.  uv run pio run -d labs/wifi-lab-w1 -e wifi-lab-w1 -t upload
// The ESP32 lists every 2.4 GHz network nearby, then joins yours: pick it from the scan table and
// type the password once (stored on the chip in NVS, never in code or the log). Once joined: internet
// time, bench.local and a live signal chart. `ping` tests the network from the board's side.
//
// Wiring: the screen from lesson 03. The push button on GPIO 5 is optional: it does exactly what
// BOOT does, and with nothing wired its internal pull-up keeps it quiet.
//
// Views: networks · channels · radar · status
// Button or BOOT: click = next view · double-click = scan now · long press = auto-scan on/off
// Serial: scan | view <1-4> | auto on|off | join <n> | forget | reconnect | status | tz <TZ> |
//         ping [ip] | help
//
// The saved network is shared with W2 (same NVS namespace), so switching labs keeps it.
#include <Arduino.h>
#include <WiFi.h>

#include "button.h"
#include "color.h"
#include "connection.h"
#include "display.h"
#include "pinger.h"
#include "pins.h"
#include "scanner.h"
#include "views.h"

namespace {

constexpr uint32_t kAutoScanMs = 10'000;  // pause between automatic scans when not joined
// When joined, every scan takes the radio off the router's channel for ~3 s. Scan less often then.
constexpr uint32_t kAutoScanJoinedMs = 60'000;
constexpr uint32_t kPageFlipMs = 4'000;  // list view: next page after this long
constexpr int kStatusView = 3;
const char *const kViewNames[] = {"networks", "channels", "radar", "status"};
constexpr int kViews = sizeof(kViewNames) / sizeof(kViewNames[0]);

Display display;
Scanner scanner;
Connection connection;
Pinger pinger;
Button external(PIN_BUTTON, "button");
Button boot(PIN_BOOT_BTN, "BOOT");

struct {
  int view = 0;
  int page = 0;
  bool autoScan = true;
  uint32_t pageFlippedAt = 0;
  String pendingSsid;  // set by `join <n>`: the next serial line is the password
  bool awaitingPassword = false;
} state;

// ---- Serial output ---------------------------------------------------------------------

void printResults() {
  const auto &nets = scanner.networks();
  Serial.printf("\n--- scan %lu: %u networks in %lu ms ---\n", static_cast<unsigned long>(scanner.scans()),
                unsigned(nets.size()), static_cast<unsigned long>(scanner.lastScanMs()));
  Serial.println(" #   dBm  ch  security    bssid              ssid");
  for (size_t i = 0; i < nets.size(); i++) {
    const ScanResult &n = nets[i];
    Serial.printf("%2u  %4ld  %2u  %-10s  %s  %s\n", unsigned(i + 1), static_cast<long>(n.rssi), n.channel,
                  authName(n.auth), n.bssid.c_str(), n.ssid.isEmpty() ? "(hidden)" : n.ssid.c_str());
  }
  Serial.println("To join one: join <#>");
}

void printStatus() {
  using State = Connection::State;
  switch (connection.state()) {
    case State::NoCredentials: Serial.println("Not joined. Scan, then: join <#>"); break;
    case State::Connecting: Serial.printf("Joining '%s'...\n", connection.ssid().c_str()); break;
    case State::Failed: Serial.printf("Could not join '%s': %s\n", connection.ssid().c_str(), Connection::reasonHint(connection.lastReason())); break;
    case State::Connected:
      Serial.printf("Joined '%s' | IP %s | %s.local | ch %d | %ld dBm | time %s\n", connection.ssid().c_str(),
                    WiFi.localIP().toString().c_str(), Connection::kHostname, static_cast<int>(WiFi.channel()),
                    static_cast<long>(WiFi.RSSI()), connection.timeSynced() ? "synced" : "not yet");
      break;
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Commands: scan | view <1-4> | auto on|off | join <#> | forget | reconnect | status | tz <TZ>");
  Serial.println("          ping (the router) | ping <ip> (e.g. your computer: can the board reach it?) | help");
  Serial.println("Button (GPIO 5, optional) or BOOT: click = next view, double-click = scan now, long press = auto-scan on/off");
}

// ---- Actions (shared by the buttons and serial) -----------------------------------------

uint32_t autoScanInterval() {
  return connection.state() == Connection::State::Connected ? kAutoScanJoinedMs : kAutoScanMs;
}

void scanNow(const char *why) {
  if (scanner.start()) log_i("scan started (%s)", why);
}

void setView(int v) {
  state.view = (v % kViews + kViews) % kViews;
  state.page = 0;
  state.pageFlippedAt = millis();
  log_i("view %d: %s", state.view + 1, kViewNames[state.view]);
}

void setAuto(bool on) {
  state.autoScan = on;
  log_i("auto-scan %s", on ? "on" : "off");
}

void startJoin(long number) {
  const auto &nets = scanner.networks();
  if (number < 1 || number > long(nets.size())) {
    log_w("join needs a number from the last scan table (1-%u)", unsigned(nets.size()));
    return;
  }
  const ScanResult &n = nets[number - 1];
  if (n.ssid.isEmpty()) {
    log_w("That network is hidden; its name is unknown, so it can't be joined from the list.");
    return;
  }
  if (n.auth == WIFI_AUTH_OPEN) {
    log_i("'%s' is open, joining without a password", n.ssid.c_str());
    connection.save(n.ssid, "");
    return;
  }
  state.pendingSsid = n.ssid;
  state.awaitingPassword = true;
  Serial.printf("Password for '%s' (the monitor shows what you type; it is stored on the chip only):\n", n.ssid.c_str());
}

// ---- Input -----------------------------------------------------------------------------

void handleButton(Button &b) {
  switch (b.poll()) {
    case Button::Event::Click: setView(state.view + 1); break;
    case Button::Event::DoubleClick: scanNow(b.name()); break;
    case Button::Event::LongPress: setAuto(!state.autoScan); break;
    case Button::Event::None: break;
  }
}

void handleCommand(String raw) {
  // A password is taken exactly as typed: no lower-casing, no trimming of inner spaces.
  if (state.awaitingPassword) {
    state.awaitingPassword = false;
    connection.save(state.pendingSsid, raw);
    state.pendingSsid = "";
    setView(kStatusView);
    return;
  }
  String line = raw;
  line.trim();
  String lower = line;
  lower.toLowerCase();
  if (lower.isEmpty()) return;
  if (lower == "scan") return scanNow("serial");
  if (lower == "auto on" || lower == "auto off") return setAuto(lower == "auto on");
  if (lower == "forget") return connection.forget();
  if (lower == "reconnect") return connection.connect();
  if (lower == "status") return printStatus();
  if (lower == "ping" || lower.startsWith("ping ")) {
    IPAddress target = WiFi.gatewayIP();
    if (connection.state() != Connection::State::Connected) log_w("ping needs a network: join one first");
    else if (lower.length() > 5 && !target.fromString(line.substring(5))) log_w("not an IP address: %s", line.substring(5).c_str());
    else pinger.start(target);
    return;
  }
  if (lower.startsWith("join ")) return startJoin(lower.substring(5).toInt());
  if (lower.startsWith("tz ")) return connection.setTimezone(line.substring(3));
  if (lower.startsWith("view ")) {
    const long v = lower.substring(5).toInt();
    if (v < 1 || v > kViews) {
      log_w("view needs 1-%d, got '%s'", kViews, lower.substring(5).c_str());
      return;
    }
    return setView(v - 1);
  }
  if (lower == "help" || lower == "?") return printHelp();
  log_w("unknown command '%s' (type help)", line.c_str());
}

void pollSerial() {
  static String buffer;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!buffer.isEmpty() || state.awaitingPassword) handleCommand(buffer);
      buffer = "";
    } else if (buffer.length() < 96) {  // WPA passwords can be 63 characters
      buffer += c;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  if (!display.begin()) {
    log_e("No memory for the frame buffer. Stopping.");
    return;
  }
  rgbLedWrite(PIN_RGB, 0, 0, 0);
  external.begin();
  boot.begin();
  scanner.begin();
  connection.begin();
  log_i("wifi-lab-w1: MAC %s. Radio: 2.4 GHz only.", WiFi.macAddress().c_str());
  printHelp();
  // A scan can't start while the radio is busy joining; the loop scans once we're settled.
  if (connection.state() != Connection::State::Connecting) scanNow("start-up");
}

void loop() {
  FrameBuffer &fb = display.fb();
  if (fb.pixels() == nullptr) {
    delay(1000);
    return;
  }
  const uint32_t now = millis();

  pollSerial();
  handleButton(external);
  handleButton(boot);
  connection.loop();
  if (connection.takeJustConnected()) setView(kStatusView);

  if (scanner.poll()) {
    if (scanner.failed()) log_w("scan failed, will retry");
    else log_i("scan %lu done: %u networks in %lu ms", static_cast<unsigned long>(scanner.scans()),
               unsigned(scanner.networks().size()), static_cast<unsigned long>(scanner.lastScanMs()));
    printResults();
  }
  // No automatic scans while joining: the radio must stay on the router's channel.
  const bool joining = connection.state() == Connection::State::Connecting;
  // millis() here, not `now`: a scan that finished during poll() above has a finish time later
  // than `now`, and `now - finishedAt` would wrap around to a huge number and scan again at once.
  if (state.autoScan && !joining && !scanner.scanning() && millis() - scanner.finishedAt() >= autoScanInterval())
    scanNow("auto");

  if (state.view == 0 && now - state.pageFlippedAt >= kPageFlipMs) {
    state.page++;
    state.pageFlippedAt = now;
  }

  fb.fillScreen(pal::night);
  const auto &nets = scanner.networks();
  switch (state.view) {
    case 0: views::list(fb, nets, state.page); break;
    case 1: views::channels(fb, nets); break;
    case 2: views::radar(fb, nets, now / 1000.0f, scanner.scanning()); break;
    default: views::status(fb, connection, now / 1000.0f); break;
  }
  views::header(fb, scanner, state.view, kViews, kViewNames[state.view], connection.state());
  views::footer(fb, "click: view  double: scan  hold: auto", state.autoScan, scanner, autoScanInterval() / 1000);
  display.push();
}
