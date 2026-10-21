// Wi-Fi lab W2: phone control.
//   uv run pio run -d labs/wifi-lab-w2 -e wifi-lab-w2 -t upload          variation 1: control on your Wi-Fi
//   uv run pio run -d labs/wifi-lab-w2 -e wifi-lab-w2-remote -t upload   variation 2: also from anywhere, through your
//                                                    dashboard server and a tunnel (remote.h)
// Everything from W1 (scanner views, join, status) plus a web server: REST API + WebSocket,
// a phone page at http://bench.local, a set-up network for joining without a cable, firmware
// updates over HTTP, and the desktop dashboard in esp-labs/apps/dashboard.
//
// Wiring: the screen from lesson 03. The push button on GPIO 5 is optional: it does exactly what
// BOOT does, and with nothing wired its internal pull-up keeps it quiet.
//
// Views: networks · channels · radar · status · message
// Button or BOOT: click = next view · double-click = scan now · long press = auto-scan on/off
// Serial: scan | view <1-5> | auto on|off | join <n> | forget | reconnect | status | tz <TZ> |
//         ping [ip] | setup | token | token new | relay [url|off] | help
//
// Web: http://bench.local (phone page), /api (REST), /ws (live). See web_api.h.
// The saved network is shared with W1 (same NVS namespace), so switching labs keeps it.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "api_rules.h"
#include "api_token.h"
#include "button.h"
#include "color.h"
#include "commands.h"
#include "connection.h"
#include "display.h"
#include "pinger.h"
#include "pins.h"
#include "remote.h"
#include "scanner.h"
#include "setup_portal.h"
#include "views.h"
#include "web_api.h"
#include "web_views.h"

namespace {

constexpr uint32_t kAutoScanMs = 10'000;        // pause between automatic scans when not joined
// When joined, every scan takes the radio off the router's channel for ~3 s, and web requests
// arriving meanwhile get lost. So scan much less often once we're on a network.
constexpr uint32_t kAutoScanJoinedMs = 60'000;
constexpr uint32_t kPageFlipMs = 4'000;    // list view: next page after this long
constexpr uint32_t kStatusPushMs = 1'000;  // live status to web clients
constexpr uint32_t kPortalAfterFailMs = 60'000;
constexpr uint32_t kPortalCloseAfterJoinMs = 30'000;
constexpr int kStatusView = 3, kMessageView = 4;
const char *const kViewNames[] = {"networks", "channels", "radar", "status", "message"};
constexpr int kViews = sizeof(kViewNames) / sizeof(kViewNames[0]);
constexpr const char *kFirmware = "wifi-lab-w2";

Display display;
Scanner scanner;
Connection connection;
SetupPortal portal;
ApiToken token;
Pinger pinger;
CommandQueue commands;
ApiRules rules(commands, connection, kViewNames, kViews);
WebApi web(rules, commands, token);
Button external(PIN_BUTTON, "button");
Button boot(PIN_BOOT_BTN, "BOOT");

struct {
  int view = 0;
  int page = 0;
  bool autoScan = true;
  uint8_t backlight = 255;
  uint8_t r = 0, g = 0, b = 0;  // on-board RGB LED
  String message;
  uint32_t pageFlippedAt = 0;
  String pendingSsid;  // set by `join <n>`: the next serial line is the password
  bool awaitingPassword = false;
  uint32_t rebootAt = 0;  // set by a reboot command; 0 = none
  float tempC = NAN;
} state;

// ---- JSON snapshots for the web --------------------------------------------------------

const char *wifiStateName(Connection::State s) {
  switch (s) {
    case Connection::State::Connected: return "connected";
    case Connection::State::Connecting: return "connecting";
    case Connection::State::Failed: return "failed";
    default: return "not-set-up";
  }
}

String statusJson() {
  JsonDocument doc;
  doc["firmware"] = kFirmware;
  doc["uptimeS"] = millis() / 1000;
  doc["heapFree"] = ESP.getFreeHeap();
  doc["psramFree"] = ESP.getFreePsram();
  if (!isnan(state.tempC)) doc["tempC"] = serialized(String(state.tempC, 1));
  doc["clients"] = web.clients();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["state"] = wifiStateName(connection.state());
  wifi["ssid"] = connection.ssid();
  wifi["hostname"] = String(Connection::kHostname) + ".local";
  if (connection.state() == Connection::State::Connected) {
    wifi["ip"] = WiFi.localIP().toString();
    wifi["rssi"] = WiFi.RSSI();
    wifi["channel"] = WiFi.channel();
  }
  wifi["reason"] = Connection::reasonHint(connection.lastReason());

  JsonObject link = doc["remote"].to<JsonObject>();
  link["state"] = remote::stateName(remote::state());

  JsonObject setup = doc["setup"].to<JsonObject>();
  setup["active"] = portal.active();
  setup["ssid"] = SetupPortal::kSsid;  // the password is only on the screen, never in the API

  JsonObject screen = doc["screen"].to<JsonObject>();
  screen["view"] = kViewNames[state.view];
  JsonArray names = screen["views"].to<JsonArray>();
  for (const char *n : kViewNames) names.add(n);
  screen["autoScan"] = state.autoScan;
  screen["backlight"] = state.backlight;
  screen["message"] = state.message;

  JsonObject led = doc["led"].to<JsonObject>();
  led["r"] = state.r, led["g"] = state.g, led["b"] = state.b;

  JsonObject scan = doc["scan"].to<JsonObject>();
  scan["running"] = scanner.scanning();
  scan["count"] = scanner.networks().size();
  scan["durationMs"] = scanner.lastScanMs();

  String out;
  serializeJson(doc, out);
  return out;
}

String networksJson() {
  JsonDocument doc;
  doc["scans"] = scanner.scans();
  doc["durationMs"] = scanner.lastScanMs();
  JsonArray list = doc["networks"].to<JsonArray>();
  for (const ScanResult &n : scanner.networks()) {
    JsonObject o = list.add<JsonObject>();
    o["ssid"] = n.ssid;  // hardware addresses (BSSIDs) stay off the web on purpose
    o["rssi"] = n.rssi;
    o["channel"] = n.channel;
    o["auth"] = authName(n.auth);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void pushEvent(const char *source, const char *gesture) {
  JsonDocument doc;
  doc["source"] = source;
  doc["gesture"] = gesture;
  doc["uptimeS"] = millis() / 1000;
  String out;
  serializeJson(doc, out);
  web.publishEvent(out);
  remote::publish("event", out);
}

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

void printRelay() {
  const String url = remote::url();
  Serial.printf("Remote link: %s%s%s\n", remote::stateName(remote::state()), url.isEmpty() ? "" : " | ", url.c_str());
}

void printStatus() {
  using State = Connection::State;
  switch (connection.state()) {
    case State::NoCredentials: Serial.println("Not joined. Scan, then: join <#>"); break;
    case State::Connecting: Serial.printf("Joining '%s'...\n", connection.ssid().c_str()); break;
    case State::Failed: Serial.printf("Could not join '%s': %s\n", connection.ssid().c_str(), Connection::reasonHint(connection.lastReason())); break;
    case State::Connected:
      Serial.printf("Joined '%s' | IP %s | http://%s.local | ch %d | %ld dBm | time %s | web clients %u\n",
                    connection.ssid().c_str(), WiFi.localIP().toString().c_str(), Connection::kHostname,
                    static_cast<int>(WiFi.channel()), static_cast<long>(WiFi.RSSI()),
                    connection.timeSynced() ? "synced" : "not yet", unsigned(web.clients()));
      break;
  }
  if (portal.active()) Serial.printf("Set-up network '%s' is open (password on the screen)\n", SetupPortal::kSsid);
  if (remote::builtIn()) printRelay();
}

void printHelp() {
  Serial.println();
  Serial.println("Commands: scan | view <1-5> | auto on|off | join <#> | forget | reconnect | status | tz <TZ>");
  Serial.println("          ping (the router) | ping <ip> (e.g. your computer: can the board reach it?)");
  Serial.println("          setup (open the set-up network) | token (show admin token) | token new | help");
  if (remote::builtIn()) Serial.println("          relay (show) | relay wss://<tunnel host> (save + connect) | relay off");
  Serial.println("Button (GPIO 5) or BOOT: click = next view, double-click = scan now, long press = auto-scan on/off");
}

// ---- Actions (shared by the button, serial and web) -------------------------------------

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

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  state.r = r, state.g = g, state.b = b;
  rgbLedWrite(PIN_RGB, r, g, b);
}

void applyCommand(const Command &c) {
  using T = Command::Type;
  switch (c.type) {
    case T::SetView: setView(c.value); break;
    case T::Scan: scanNow("web"); break;
    case T::SetAutoScan: setAuto(c.value != 0); break;
    case T::SetBacklight:
      state.backlight = uint8_t(c.value);
      display.setBacklight(state.backlight);
      log_i("backlight %u", state.backlight);
      break;
    case T::SetLed: setLed(c.r, c.g, c.b); break;
    case T::ShowMessage:
      state.message = c.text;
      setView(kMessageView);
      log_i("message from the web (%u characters)", unsigned(state.message.length()));
      break;
    case T::SaveWifi:
      connection.save(c.text, c.secret);
      setView(kStatusView);
      break;
    case T::ForgetWifi: connection.forget(); break;
    case T::Reboot:
      log_i("reboot requested");
      state.rebootAt = millis() + 800;  // let the HTTP answer go out first
      break;
  }
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
    case Button::Event::Click:
      setView(state.view + 1);
      pushEvent(b.name(), "click");
      break;
    case Button::Event::DoubleClick:
      scanNow(b.name());
      pushEvent(b.name(), "double-click");
      break;
    case Button::Event::LongPress:
      setAuto(!state.autoScan);
      pushEvent(b.name(), "long-press");
      break;
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
  if (lower == "relay" || lower.startsWith("relay ")) {
    if (!remote::builtIn()) log_w("this build has no remote link: flash wifi-lab-w2-remote");
    else if (lower == "relay") printRelay();
    else if (lower == "relay off") remote::setUrl("");
    else remote::setUrl(line.substring(6));  // as typed: host names can be mixed case
    return;
  }
  if (lower == "setup") return portal.start();
  if (lower == "token") {
    Serial.printf("Admin token: %s  (put it in esp-labs/apps/dashboard/.env as ESP_TOKEN)\n", token.value().c_str());
    return;
  }
  if (lower == "token new") {
    token.regenerate();
    Serial.printf("New admin token: %s  (the old one stops working now)\n", token.value().c_str());
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

// Open the set-up network when there's nothing to join (or joining keeps failing),
// close it a while after we're connected.
void managePortal() {
  using State = Connection::State;
  static uint32_t failingSince = 0;
  const uint32_t now = millis();
  const State s = connection.state();
  if (s == State::NoCredentials) portal.start();
  if (s == State::Failed) {
    if (!failingSince) failingSince = now;
    if (now - failingSince > kPortalAfterFailMs) portal.start();
  } else if (s != State::Connecting) {
    failingSince = 0;
  }
  if (s == State::Connected && portal.activeFor() > kPortalCloseAfterJoinMs) portal.stop();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  if (!display.begin()) {
    log_e("No memory for the frame buffer. Stopping.");
    return;
  }
  setLed(0, 0, 0);
  external.begin();
  boot.begin();
  commands.begin();
  token.begin();
  scanner.begin();
  portal.begin();
  connection.begin();
  web.begin();
  remote::begin(rules, token);
  log_i("%s: MAC %s. Radio: 2.4 GHz only.", kFirmware, WiFi.macAddress().c_str());
  Serial.printf("Admin token: %s  (for the dashboard's .env; serial command 'token' shows it again)\n", token.value().c_str());
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
  if (state.rebootAt && now >= state.rebootAt) ESP.restart();

  pollSerial();
  handleButton(external);
  handleButton(boot);
  for (Command c; commands.pop(c);) applyCommand(c);
  connection.loop();
  portal.loop();
  managePortal();
  web.loop();
  if (connection.takeJustConnected()) setView(kStatusView);

  if (scanner.poll()) {
    if (scanner.failed()) log_w("scan failed, will retry");
    else log_i("scan %lu done: %u networks in %lu ms", static_cast<unsigned long>(scanner.scans()),
               unsigned(scanner.networks().size()), static_cast<unsigned long>(scanner.lastScanMs()));
    printResults();
    const String networks = networksJson();
    web.publishNetworks(networks);
    remote::publish("networks", networks);
  }
  // No automatic scans while joining (the radio must stay on the router's channel) or while the
  // set-up network is open (a scan would briefly cut off the phone that's setting us up).
  const bool joining = connection.state() == Connection::State::Connecting;
  // millis() here, not `now`: a scan that finished during poll() above has a finish time later
  // than `now`, and `now - finishedAt` would wrap around to a huge number and scan again at once.
  if (state.autoScan && !joining && !portal.active() && !scanner.scanning() &&
      millis() - scanner.finishedAt() >= autoScanInterval())
    scanNow("auto");

  static uint32_t lastPush = 0, lastTemp = 0;
  if (now - lastTemp >= 1000) {
    lastTemp = now;
    state.tempC = temperatureRead();
  }
  if (now - lastPush >= kStatusPushMs) {
    lastPush = now;
    const String status = statusJson();
    web.publishStatus(status);
    remote::publish("status", status);
  }

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
    case kStatusView:
      if (portal.active() && connection.state() != Connection::State::Connected) views::setupScreen(fb, portal);
      else views::status(fb, connection, now / 1000.0f);
      break;
    default: views::message(fb, state.message, pal::amber); break;
  }
  views::header(fb, scanner, state.view, kViews, kViewNames[state.view], connection.state());
  // Remote link: a ring around the Wi-Fi dot, teal when the dashboard server is connected.
  if (remote::state() == remote::State::Online) fb.drawCircle(312, 20, 7, pal::teal);
  else if (remote::state() == remote::State::Connecting) fb.drawCircle(312, 20, 7, pal::amber);
  views::footer(fb, "click: view  double: scan  hold: auto", state.autoScan, scanner, autoScanInterval() / 1000);
  views::otaOverlay(fb, web.otaPercent());
  display.push();
}
