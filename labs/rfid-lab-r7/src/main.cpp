// RFID lab R7 Card manager.  uv run pio run -d labs/rfid-lab-r7 -e rfid-lab-r7 -t upload
// A bench door you manage from a React app (esp-labs/apps/door-app): add cards, name them, block
// them, give them an end date, hours or a number of uses, replace a lost one, write a label onto
// them, wipe their data blocks, save a card's data and write it back or copy it onto another
// card, watch the access log live. Two simulated locks (the screen and the LED): the front door,
// open to every role, and the admin room, admin cards only. The one reader is mounted at one of
// them at a time; the app or `at front|admin` moves it.
//
// The board keeps the card list (NVS) and decides every tap itself; the card only brings its
// UID. The app talks to the board's JSON API and WebSocket over your Wi-Fi (the network saved
// by W1/W2). Every change needs the admin token (serial: token), which the app's dev server adds.
//
// Wiring: the screen from lesson 03 and the RC522 as in R1. Push button on GPIO 5 optional.
// Views: door · cards · log   (button: click = next view)
// On-board LED (the lock the reader is at): green = open · red = denied · blue blinking = waiting for a new card ·
//               violet blinking = copy waiting for its second card · dim amber = lockdown ·
//               off = locked and quiet
// Serial: help · status · token · token new · cards · enroll <name> · open [front|admin] ·
//         at front|admin · lockdown on|off ·
//         saved · save [name] · restore <id> · copy · cancel · reset · tz <TZ>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>

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
#include "snapshots.h"

namespace {

const char *const kViewNames[] = {"door", "cards", "log"};
constexpr int kViews = 3;

Display display;
Connection connection;
ApiToken token;
Registry registry;
snap::Store snapshots;
Door doorTask(registry, snapshots);
DoorApi api(registry, snapshots, doorTask, token);
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

// Why the board last started: a crash or a brownout shows up here (and in the app).
const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (supply dipped)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "reset button";
    case ESP_RST_USB: return "USB";
    default: return "other";
  }
}

void printHelp() {
  Serial.println("\nRFID lab R7 Card manager  (rfid-lab-r7)");
  Serial.println("  status            Wi-Fi, address, reader chip, locks, card count");
  Serial.println("  token             the admin token for esp-labs/apps/door-app/.env (token new: replace it)");
  Serial.println("  cards             the card list");
  Serial.println("  enroll <name>     add a card as a resident: tap it within 20 s");
  Serial.println("  open [front|admin] open a lock for 5 s (the front door when left out)");
  Serial.println("  at front|admin    mount the reader at the front door or the admin room");
  Serial.println("  lockdown on|off   only maintenance and admin cards open while on");
  Serial.println("  saved             saved card data (blocks 4-6, 8-10)");
  Serial.println("  save [name]       save the data of the card on the reader");
  Serial.println("  restore <id>      write saved data onto the card on the reader");
  Serial.println("  copy              copy the card on the reader onto the next card you lay down");
  Serial.println("  cancel            stop waiting for a card (enroll, replace, copy)");
  Serial.println("  reset             hard reset of the reader chip");
  Serial.println("  tz <TZ>           time zone for the hours rules (POSIX string)");
  Serial.println("Button or BOOT: click = next view (door, cards, log)");
}

void printStatus() {
  const bool joined = connection.state() == Connection::State::Connected;
  Serial.printf("Wi-Fi: %s %s | http://%s.local (%s) | app clients %u\n", joined ? "joined" : "not joined",
                connection.ssid().c_str(), Connection::kHostname, joined ? WiFi.localIP().toString().c_str() : "-",
                unsigned(api.clients()));
  const auto lockLine = [](door::Lock l) {
    const DoorState::LockState &ls = state.door.locks[int(l)];
    Serial.printf("%s: %s%s | ", door::lockTitle(l),
                  ls.mode == DoorState::Mode::Open ? "open" : ls.mode == DoorState::Mode::Denied ? "denied" : "locked",
                  registry.readerAt() == l ? " (reader here)" : "");
  };
  lockLine(door::Lock::Front);
  lockLine(door::Lock::AdminRoom);
  Serial.println();
  Serial.printf("Reader chip 0x%02X (%s) | %d cards | lockdown %s | clock %s\n", state.door.chipVersion,
                rc522::versionName(state.door.chipVersion), registry.count(), registry.lockdown() ? "ON" : "off",
                connection.timeSynced() ? "synced" : "not yet (cards with dates or hours are refused until it is)");
  Serial.printf("Reader: %s | settings restored %u x | chip lost %u x | last problem: %s\n",
                state.door.irqMode ? "IRQ wire" : "polling", state.door.restores, state.door.lost,
                state.door.problem[0] ? state.door.problem : "none");
  Serial.printf("Board: up %lu s | last start: %s | free heap %u B\n", millis() / 1000, resetReason(), unsigned(ESP.getFreeHeap()));
}

void printCards() {
  door::Clock clk;
  doorjson::clockNow(clk);
  const auto list = registry.list();
  Serial.printf("%u card%s\n", unsigned(list.size()), list.size() == 1 ? "" : "s");
  for (const door::Card &c : list) {
    char uid[21];
    door::uidToHex(c.uid, c.uidLen, uid, sizeof(uid));
    Serial.printf("  %-14s %-20s %-11s front: %-13s admin room: %-13s taps %lu\n", uid, c.name, door::roleName(c.role),
                  door::decisionName(door::decide(&c, clk, registry.lockdown(), door::Lock::Front)),
                  door::decisionName(door::decide(&c, clk, registry.lockdown(), door::Lock::AdminRoom)),
                  static_cast<unsigned long>(c.taps));
  }
}

void printSaved() {
  const auto list = snapshots.list();
  Serial.printf("%u of %d places used\n", unsigned(list.size()), snap::Store::kMax);
  for (const snap::Snapshot &s : list) {
    char uid[21];
    door::uidToHex(s.uid, s.uidLen, uid, sizeof(uid));
    Serial.printf("  id %-3lu %-20s from %-14s %d of 6 blocks\n", static_cast<unsigned long>(s.id), s.name,
                  s.uidLen ? uid : "a file", s.count());
  }
}

void submit(Door::Job job, const char *name = "", uint32_t snapshot = 0, door::Lock lock = door::Lock::Front) {
  Door::Request r;
  r.job = job;
  r.lock = lock;
  strlcpy(r.name, name, sizeof(r.name));
  r.snapshot = snapshot;
  if (!doorTask.submit(r)) log_w("the reader is busy, try again");
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
  if (lower.startsWith("open ") || lower.startsWith("at ")) {
    door::Lock l;
    if (!door::lockFromId(lower.substring(lower.indexOf(' ') + 1).c_str(), l)) {
      log_w("which lock? front or admin");
      return;
    }
    if (lower.startsWith("open ")) return submit(Door::Job::Open, "", 0, l);
    registry.setReaderAt(l);
    log_i("the reader is now at the %s", door::lockTitle(l));
    return;
  }
  if (lower == "reset") return submit(Door::Job::ChipReset);
  if (lower == "saved") return printSaved();
  if (lower == "copy") return submit(Door::Job::Copy);
  if (lower == "cancel") return submit(Door::Job::Cancel);
  if (lower == "save" || lower.startsWith("save ")) {
    const String name = line.length() > 5 ? line.substring(5) : String();
    if (name.length() > 23) {
      log_w("save [name], up to 23 characters");
      return;
    }
    return submit(Door::Job::Save, name.c_str());
  }
  if (lower.startsWith("restore ")) {
    const long id = line.substring(8).toInt();
    if (id <= 0) {
      log_w("restore <id>  (saved lists the ids)");
      return;
    }
    return submit(Door::Job::Restore, "", uint32_t(id));
  }
  if (lower == "token") {
    Serial.printf("Admin token: %s\nPut it in esp-labs/apps/door-app/.env as BENCH_TOKEN=... (keep it out of chats and git)\n", token.value().c_str());
    return;
  }
  if (lower == "token new") {
    token.regenerate();
    Serial.printf("New admin token: %s (the old one stops working now; W2 shares it too)\n", token.value().c_str());
    return;
  }
  if (lower == "lockdown on" || lower == "lockdown off") {
    registry.setLockdown(lower == "lockdown on");
    log_i("lockdown %s", registry.lockdown() ? "ON: only maintenance and admin cards open" : "off");
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
  const DoorState &s = state.door;
  JsonArray locks = d["locks"].to<JsonArray>();
  for (door::Lock l : {door::Lock::Front, door::Lock::AdminRoom}) {
    const DoorState::LockState &ls = s.locks[int(l)];
    JsonObject lj = locks.add<JsonObject>();
    lj["id"] = door::lockId(l);
    lj["title"] = door::lockTitle(l);
    lj["mode"] = ls.mode == DoorState::Mode::Open ? "open" : ls.mode == DoorState::Mode::Denied ? "denied" : "locked";
    lj["msLeft"] = ls.mode == DoorState::Mode::Locked ? 0 : max<int32_t>(0, int32_t(ls.modeUntil - millis()));
    lj["name"] = doorjson::copied(ls.lastName);
    lj["uid"] = doorjson::copied(ls.lastUid);
    lj["why"] = doorjson::copied(ls.lastWhy);
  }
  d["readerAt"] = door::lockId(registry.readerAt());
  if (s.present) d["present"] = doorjson::copied(s.presentUid);
  else d["present"] = nullptr;
  if (s.wait != DoorState::Wait::None) {
    JsonObject w = d["waiting"].to<JsonObject>();
    w["for"] = s.wait == DoorState::Wait::Enroll ? "enroll" : s.wait == DoorState::Wait::Replace ? "replace" : "copy";
    w["name"] = doorjson::copied(s.waitName);
    w["uid"] = doorjson::copied(s.waitUid);
    w["msLeft"] = max<int32_t>(0, int32_t(s.waitUntil - millis()));
  } else {
    d["waiting"] = nullptr;
  }
  d["lockdown"] = registry.lockdown();
  d["cards"] = registry.count();
  d["busy"] = s.busy != nullptr;
  JsonObject reader = d["reader"].to<JsonObject>();
  reader["wait"] = s.irqMode ? "irq" : "poll";
  reader["restores"] = s.restores;
  reader["lost"] = s.lost;
  reader["problem"] = doorjson::copied(s.problem);
  reader["problemAgo"] = s.problem[0] ? (millis() - s.problemAt) / 1000 : 0;
  JsonObject board = d["board"].to<JsonObject>();
  board["uptime"] = millis() / 1000;
  board["reset"] = resetReason();
  board["heap"] = ESP.getFreeHeap();
  String out;
  serializeJson(d, out);
  api.publishStatus(out);
}

// The on-board LED, written only when its colour changes.
void showLed() {
  static uint32_t shown = 0xFFFFFFFF;
  const DoorState &s = state.door;
  const DoorState::LockState &here = s.locks[int(registry.readerAt())];
  uint8_t r = 0, g = 0, b = 0;
  const bool blink = (millis() / 300) % 2;
  if (s.wait == DoorState::Wait::CopyTarget) r = blink ? 70 : 12, b = blink ? 120 : 20;
  else if (s.wait != DoorState::Wait::None) b = blink ? 120 : 20;
  else if (here.mode == DoorState::Mode::Open) g = 160, b = 40;
  else if (here.mode == DoorState::Mode::Denied) r = 150;
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
  snapshots.begin();
  connection.begin();
  doorTask.begin();
  log_i("rfid-lab-r7: reader task on core 0. Last start: %s. Type token for the app's .env, help for commands.", resetReason());
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
  const bool changed = doorTask.update(state.door);
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
    state.events = doorTask.events(0);
  }
  showLed();

  const float t = now / 1000.0f;
  fb.fillScreen(pal::night);
  door::Clock clk;
  doorjson::clockNow(clk);
  switch (state.view) {
    case 0: dv::door(fb, state.door, registry.readerAt(), int(state.cards.size()), t); break;
    case 1: dv::cards(fb, state.cards, clk, registry.lockdown(), registry.readerAt()); break;
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
