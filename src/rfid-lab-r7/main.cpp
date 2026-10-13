// RFID lab R7 Card manager.  uv run pio run -e rfid-lab-r7 -t upload
// A bench door you manage from a React app (test-esp/door-app): add cards, name them, block
// them, give them an end date, hours or a number of uses, write a label onto them, wipe their
// data blocks, watch the access log live. The door is simulated: the screen and the LED.
//
// The board keeps the card list (NVS) and decides every tap itself; the card only brings its
// UID. The app talks to the board's JSON API and WebSocket over your Wi-Fi (the network saved
// by W1/W2). Every change needs the admin token (serial: token), which the app's dev server adds.
//
// Wiring: the screen from lesson 03 and the RC522 as in R1. Push button on GPIO 5 optional.
// Views: door · cards · log   (button: click = next view)
// On-board LED: green = door open · red = denied · blue blinking = waiting for a new card ·
//               dim amber = lockdown · off = locked and quiet
// Serial: help · status · token · token new · cards · enroll <name> · open · lockdown on|off ·
//         reset · tz <TZ>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "api_token.h"
#include "button.h"
#include "color.h"
#include "connection.h"
#include "display.h"
#include "door.h"
#include "door_api.h"
#include "door_views.h"
#include "pins.h"
#include "registry.h"

namespace {

const char *const kViewNames[] = {"door", "cards", "log"};
constexpr int kViews = 3;

Display display;
Connection connection;
ApiToken token;
Registry registry;
Door door(registry);
DoorApi api(registry, door, token);
Button external(PIN_BUTTON, "button");
Button boot(PIN_BOOT_BTN, "BOOT");

struct {
  int view = 0;
  DoorState door;
  std::vector<door::Card> cards;  // copies for the screen, refreshed when the list changes
  std::vector<DoorEvent> events;
  uint32_t cardsVersion = 0, lastStatus = 0, lastEvents = 0;
  bool apiStarted = false;
} state;

void printHelp() {
  Serial.println("\nRFID lab R7 Card manager  (rfid-lab-r7)");
  Serial.println("  status            Wi-Fi, address, reader chip, door, card count");
  Serial.println("  token             the admin token for test-esp/door-app/.env (token new: replace it)");
  Serial.println("  cards             the card list");
  Serial.println("  enroll <name>     add a card as a resident: tap it within 20 s");
  Serial.println("  open              open the door for 5 s");
  Serial.println("  lockdown on|off   only maintenance cards open while on");
  Serial.println("  reset             hard reset of the reader chip");
  Serial.println("  tz <TZ>           time zone for the hours rules (POSIX string)");
  Serial.println("Button or BOOT: click = next view (door, cards, log)");
}

void printStatus() {
  const bool joined = connection.state() == Connection::State::Connected;
  Serial.printf("Wi-Fi: %s %s | http://%s.local (%s) | app clients %u\n", joined ? "joined" : "not joined",
                connection.ssid().c_str(), Connection::kHostname, joined ? WiFi.localIP().toString().c_str() : "-",
                unsigned(api.clients()));
  Serial.printf("Reader chip 0x%02X (%s) | %d cards | lockdown %s | clock %s\n", state.door.chipVersion,
                rc522::versionName(state.door.chipVersion), registry.count(), registry.lockdown() ? "ON" : "off",
                connection.timeSynced() ? "synced" : "not yet (cards with dates or hours are refused until it is)");
}

void printCards() {
  door::Clock clk;
  doorjson::clockNow(clk);
  const auto list = registry.list();
  Serial.printf("%u card%s\n", unsigned(list.size()), list.size() == 1 ? "" : "s");
  for (const door::Card &c : list) {
    char uid[21];
    door::uidToHex(c.uid, c.uidLen, uid, sizeof(uid));
    Serial.printf("  %-14s %-20s %-11s %s%s  taps %lu\n", uid, c.name, door::roleName(c.role), c.blocked ? "BLOCKED " : "",
                  door::decisionName(door::decide(&c, clk, registry.lockdown())), static_cast<unsigned long>(c.taps));
  }
}

void submit(Door::Job job, const char *name = "") {
  Door::Request r;
  r.job = job;
  strlcpy(r.name, name, sizeof(r.name));
  if (!door.submit(r)) log_w("the reader is busy, try again");
}

void handleCommand(const String &raw) {
  String line = raw;
  line.trim();
  String lower = line;
  lower.toLowerCase();
  if (lower.isEmpty()) return;
  if (lower == "help" || lower == "?") return printHelp();
  if (lower == "status") return printStatus();
  if (lower == "cards") return printCards();
  if (lower == "open") return submit(Door::Job::Open);
  if (lower == "reset") return submit(Door::Job::ChipReset);
  if (lower == "token") {
    Serial.printf("Admin token: %s\nPut it in test-esp/door-app/.env as BENCH_TOKEN=... (keep it out of chats and git)\n", token.value().c_str());
    return;
  }
  if (lower == "token new") {
    token.regenerate();
    Serial.printf("New admin token: %s (the old one stops working now; W2 shares it too)\n", token.value().c_str());
    return;
  }
  if (lower == "lockdown on" || lower == "lockdown off") {
    registry.setLockdown(lower == "lockdown on");
    log_i("lockdown %s", registry.lockdown() ? "ON: only maintenance cards open" : "off");
    return;
  }
  if (lower.startsWith("enroll ")) {
    const String name = line.substring(7);
    if (name.isEmpty() || name.length() > 23) {
      log_w("enroll <name>, 1-23 characters");
      return;
    }
    return submit(Door::Job::Enroll, name.c_str());
  }
  if (lower.startsWith("tz ")) return connection.setTimezone(line.substring(3));
  log_w("unknown command '%s' (type help)", line.c_str());
}

void pollSerial() {
  static String buffer;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!buffer.isEmpty()) handleCommand(buffer);
      buffer = "";
    } else if (c == 0x08 || c == 0x7F) {
      if (!buffer.isEmpty()) buffer.remove(buffer.length() - 1);
    } else if (uint8_t(c) >= 0x20 && buffer.length() < 96) {
      buffer += c;
    }
  }
}

void handleButton(Button &b) {
  if (b.poll() == Button::Event::Click) {
    state.view = (state.view + 1) % kViews;
    log_i("view %d: %s", state.view + 1, kViewNames[state.view]);
  }
}

// The status every app client sees, once a second and whenever the door changes.
void publishStatus() {
  JsonDocument d;
  const bool joined = connection.state() == Connection::State::Connected;
  JsonObject chip = d["chip"].to<JsonObject>();
  char v[3];
  snprintf(v, sizeof(v), "%02X", state.door.chipVersion);
  chip["version"] = v;
  chip["name"] = rc522::versionName(state.door.chipVersion);
  chip["ok"] = state.door.chipVersion != 0x00 && state.door.chipVersion != 0xFF;
  JsonObject wifi = d["wifi"].to<JsonObject>();
  wifi["ssid"] = connection.ssid();
  wifi["ip"] = joined ? WiFi.localIP().toString() : String();
  wifi["rssi"] = joined ? WiFi.RSSI() : 0;
  door::Clock clk;
  doorjson::clockNow(clk);
  JsonObject clock = d["clock"].to<JsonObject>();
  clock["synced"] = clk.synced;
  clock["now"] = clk.now;
  JsonObject dj = d["door"].to<JsonObject>();
  const DoorState &s = state.door;
  dj["mode"] = s.mode == DoorState::Mode::Open ? "open" : s.mode == DoorState::Mode::Denied ? "denied" : "locked";
  dj["msLeft"] = s.mode == DoorState::Mode::Locked ? 0 : max<int32_t>(0, int32_t(s.modeUntil - millis()));
  dj["name"] = s.lastName;
  dj["uid"] = s.lastUid;
  dj["why"] = s.lastWhy;
  if (s.present) d["present"] = s.presentUid;
  else d["present"] = nullptr;
  if (s.enrolling) {
    JsonObject e = d["enrolling"].to<JsonObject>();
    e["name"] = s.enrollName;
    e["msLeft"] = max<int32_t>(0, int32_t(s.enrollUntil - millis()));
  } else {
    d["enrolling"] = nullptr;
  }
  d["lockdown"] = registry.lockdown();
  d["cards"] = registry.count();
  d["busy"] = s.busy != nullptr;
  String out;
  serializeJson(d, out);
  api.publishStatus(out);
}

// The on-board LED, written only when its colour changes.
void showLed() {
  static uint32_t shown = 0xFFFFFFFF;
  const DoorState &s = state.door;
  uint8_t r = 0, g = 0, b = 0;
  if (s.enrolling) b = (millis() / 300) % 2 ? 120 : 20;
  else if (s.mode == DoorState::Mode::Open) g = 160, b = 40;
  else if (s.mode == DoorState::Mode::Denied) r = 150;
  else if (state.door.version && (s.chipVersion == 0x00 || s.chipVersion == 0xFF)) r = 90;
  else if (registry.lockdown()) r = 40, g = 18;
  const uint32_t colour = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
  if (colour == shown) return;
  shown = colour;
  rgbLedWrite(PIN_RGB, r, g, b);
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
  token.begin();
  registry.begin();
  connection.begin();
  door.begin();
  log_i("rfid-lab-r7: reader task on core 0. Type token for the app's .env, help for commands.");
  printHelp();
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
  // The web server starts once we have an address (the API is useless without one).
  if (!state.apiStarted && connection.state() == Connection::State::Connected) {
    api.begin();
    state.apiStarted = true;
    log_i("App: http://%s.local or http://%s  (set BENCH_URL in door-app/.env)", Connection::kHostname, WiFi.localIP().toString().c_str());
  }
  const bool changed = door.update(state.door);
  if (state.apiStarted) {
    api.loop();
    if (changed || now - state.lastStatus >= 1000) {
      state.lastStatus = now;
      publishStatus();
    }
  }
  if (registry.version() != state.cardsVersion) {
    state.cardsVersion = registry.version();
    state.cards = registry.list();
  }
  if (now - state.lastEvents >= 500) {
    state.lastEvents = now;
    state.events = door.events(0);
  }
  showLed();

  const float t = now / 1000.0f;
  fb.fillScreen(pal::night);
  door::Clock clk;
  doorjson::clockNow(clk);
  switch (state.view) {
    case 0: dv::door(fb, state.door, int(state.cards.size()), t); break;
    case 1: dv::cards(fb, state.cards, clk, registry.lockdown()); break;
    default: dv::history(fb, state.events); break;
  }
  dv::header(fb, kViewNames[state.view], state.view, kViews, state.door, registry.lockdown());
  char left[48], right[48];
  const bool joined = connection.state() == Connection::State::Connected;
  if (joined) snprintf(left, sizeof(left), "http://%s.local", Connection::kHostname);
  else snprintf(left, sizeof(left), "%s", connection.ssid().isEmpty() ? "no Wi-Fi saved: run W1 once" : "joining Wi-Fi...");
  snprintf(right, sizeof(right), "%s%u app", clk.synced ? "" : "no clock  ", unsigned(api.clients()));
  dv::footer(fb, left, right, joined ? pal::teal : pal::amber);
  display.push();
}
