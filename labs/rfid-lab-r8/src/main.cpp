// RFID lab R8 Card friends.  uv run pio run -d labs/rfid-lab-r8 -e rfid-lab-r8 -t upload
// A playful lab for kids: every card is the home of a friend (a fox, a robot, a dragon, ...).
// Tap a card and its friend pops out on the screen, and in 3D in the app (esp-labs/apps/friends-app),
// each with its own colours, dance, sound and sparkles. A new card is added by itself on its
// first tap; in the app you drag a friend onto it. Card 1 and card 2 show different friends.
//
// Built from R7's parts: the same driver and reader health checks, the same web libraries, the
// same admin token, the same Wi-Fi (saved by W1/W2). Only the UID is read; no card is written.
//
// Wiring: the screen from lesson 03 and the RC522 as in R1. Push button on GPIO 5 optional.
// Views: stage · friends   (button: click = next view)
// On-board LED: flashes in the friend's colour on a tap, breathes softly while waiting.
// Serial: help · status · token · token new · cards · give <number> <friend> · name <number> <name> ·
//         forget <number> · cast · reset
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>

#include "api_token.h"
#include "button.h"
#include "characters.h"
#include "color.h"
#include "connection.h"
#include "display.h"
#include "friend_views.h"
#include "friends.h"
#include "friends_api.h"
#include "pins.h"
#include "tap_reader.h"

namespace {

const char *const kViewNames[] = {"stage", "cards"};
constexpr int kViews = 2;

Display display;
Connection connection;
ApiToken token;
friends::List cardList;
TapReader reader(cardList);
FriendsApi api(cardList, reader, token);
Button external(PIN_BUTTON, "button");
Button boot(PIN_BOOT_BTN, "BOOT");

struct {
  int view = 0;
  Stage stage;
  std::vector<friends::Card> cards;
  uint32_t cardsVersion = 0, lastStatus = 0;
  bool apiStarted = false;
} state;

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (supply dipped)";
    case ESP_RST_EXT: return "reset button";
    case ESP_RST_USB: return "USB";
    default: return "other";
  }
}

void printHelp() {
  Serial.println("\nRFID lab R8 Card friends  (rfid-lab-r8)");
  Serial.println("  status                Wi-Fi, address, reader chip, card count");
  Serial.println("  token                 the admin token for esp-labs/apps/friends-app/.env (token new: replace it)");
  Serial.println("  cards                 every card and who lives on it");
  Serial.println("  cast                  the friends this firmware knows");
  Serial.println("  give <number> <id>    move a friend onto a card, e.g. give 1 fox (give 1 none: empty it)");
  Serial.println("  name <number> <name>  the friend's name on that card (name 1 -: back to its own)");
  Serial.println("  forget <number>       forget a card (it comes back empty on its next tap)");
  Serial.println("  reset                 hard reset of the reader chip");
  Serial.println("Button or BOOT: click = next view (stage, cards)");
}

void printStatus() {
  const bool joined = connection.state() == Connection::State::Connected;
  Serial.printf("Wi-Fi: %s %s | http://%s.local (%s) | app clients %u\n", joined ? "joined" : "not joined",
                connection.ssid().c_str(), Connection::kHostname, joined ? WiFi.localIP().toString().c_str() : "-",
                unsigned(api.clients()));
  const Stage &s = state.stage;
  Serial.printf("Reader chip 0x%02X (%s), %s | settings restored %u x | lost %u x | last problem: %s\n", s.chipVersion,
                rc522::versionName(s.chipVersion), s.irqMode ? "IRQ wire" : "polling", s.restores, s.lost,
                s.problem[0] ? s.problem : "none");
  Serial.printf("%d of %d cards | up %lu s | last start: %s | free heap %u B\n", cardList.count(), friends::List::kMax,
                millis() / 1000, resetReason(), unsigned(ESP.getFreeHeap()));
}

void printCards() {
  const auto list = cardList.list();
  Serial.printf("%u card%s\n", unsigned(list.size()), list.size() == 1 ? "" : "s");
  for (const friends::Card &c : list) {
    char uid[21];
    friends::uidToHex(c.uid, c.uidLen, uid, sizeof(uid));
    const cast::Character *who = cast::find(c.character);
    Serial.printf("  %2u  %-14s %-8s %-15s taps %lu\n", c.number, uid, who ? who->id : "-", who ? (c.name[0] ? c.name : who->name) : "nobody yet",
                  static_cast<unsigned long>(c.taps));
  }
}

// The card with this number, as shown by `cards`.
bool byNumber(long number, friends::Card &out) {
  for (const friends::Card &c : cardList.list())
    if (c.number == number) return out = c, true;
  log_w("no card %ld (cards lists them)", number);
  return false;
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
  if (lower == "cast") {
    for (const cast::Character &c : cast::kCast) Serial.printf("  %-8s %s\n", c.id, c.name);
    return;
  }
  if (lower == "reset") return reader.requestChipReset();
  if (lower == "token") {
    Serial.printf("Admin token: %s\nPut it in esp-labs/apps/friends-app/.env as BENCH_TOKEN=... (keep it out of chats and git)\n", token.value().c_str());
    return;
  }
  if (lower == "token new") {
    token.regenerate();
    Serial.printf("New admin token: %s (the old one stops working now; W2 and R7 share it)\n", token.value().c_str());
    return;
  }
  // give <number> <id>, name <number> <name>, forget <number>
  const int space = line.indexOf(' ');
  const String verb = lower.substring(0, space < 0 ? lower.length() : space);
  const String rest = space < 0 ? String() : line.substring(space + 1);
  const int space2 = rest.indexOf(' ');
  const long number = rest.toInt();
  const String arg = space2 < 0 ? String() : rest.substring(space2 + 1);
  friends::Card c;
  if (verb == "forget" && number > 0) {
    if (byNumber(number, c)) cardList.remove(c.uid, c.uidLen), log_i("card %ld forgotten", number);
    return;
  }
  if (verb == "give" && number > 0 && !arg.isEmpty()) {
    String id = arg;
    id.toLowerCase();
    if (id == "none") id = "";
    if (!id.isEmpty() && !cast::find(id.c_str())) {
      log_w("unknown friend '%s' (cast lists them)", id.c_str());
      return;
    }
    if (!byNumber(number, c)) return;
    cardList.update(c.uid, c.uidLen, [&](friends::Card &x) { strlcpy(x.character, id.c_str(), sizeof(x.character)); });
    reader.show(c.uid, c.uidLen);
    return;
  }
  if (verb == "name" && number > 0 && !arg.isEmpty()) {
    if (arg.length() > 15) {
      log_w("names: up to 15 characters");
      return;
    }
    if (!byNumber(number, c)) return;
    const String name = arg == "-" ? String() : arg;
    cardList.update(c.uid, c.uidLen, [&](friends::Card &x) { strlcpy(x.name, name.c_str(), sizeof(x.name)); });
    reader.show(c.uid, c.uidLen);
    return;
  }
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

// The status every app sees, once a second and whenever the stage changes.
void publishStatus() {
  JsonDocument d;
  const bool joined = connection.state() == Connection::State::Connected;
  const Stage &s = state.stage;
  JsonObject chip = d["chip"].to<JsonObject>();
  char v[3];
  snprintf(v, sizeof(v), "%02X", s.chipVersion);
  chip["version"] = v;
  chip["name"] = rc522::versionName(s.chipVersion);
  chip["ok"] = s.chipVersion != 0x00 && s.chipVersion != 0xFF;
  JsonObject wifi = d["wifi"].to<JsonObject>();
  wifi["ssid"] = connection.ssid();
  wifi["ip"] = joined ? WiFi.localIP().toString() : String();
  wifi["rssi"] = joined ? WiFi.RSSI() : 0;
  JsonObject rd = d["reader"].to<JsonObject>();
  rd["wait"] = s.irqMode ? "irq" : "poll";
  rd["restores"] = s.restores;
  rd["lost"] = s.lost;
  rd["problem"] = static_cast<const char *>(s.problem);
  rd["problemAgo"] = s.problem[0] ? (millis() - s.problemAt) / 1000 : 0;
  if (s.present) d["present"] = static_cast<const char *>(s.presentUid);
  else d["present"] = nullptr;
  d["cards"] = cardList.count();
  d["maxCards"] = friends::List::kMax;
  d["view"] = kViewNames[state.view];
  JsonObject board = d["board"].to<JsonObject>();
  board["uptime"] = millis() / 1000;
  board["reset"] = resetReason();
  board["heap"] = ESP.getFreeHeap();
  String out;
  serializeJson(d, out);
  api.publishStatus(out);
}

// The on-board LED: the friend's colour for 1.5 s after a tap, a soft teal breath otherwise.
void showLed() {
  static uint32_t shown = 0xFFFFFFFF;
  const Stage &s = state.stage;
  uint8_t r = 0, g = 0, b = 0;
  const uint32_t since = millis() - s.showAt;
  if (s.showSeq && since < 1500) {
    const cast::Character *who = cast::find(s.character);
    const uint16_t c = who ? who->body : pal::teal;
    const float fade = 1.0f - since / 1500.0f;
    r = uint8_t(((c >> 11) << 3) * fade * 0.6f), g = uint8_t((((c >> 5) & 63) << 2) * fade * 0.6f), b = uint8_t(((c & 31) << 3) * fade * 0.6f);
  } else if (s.chipVersion == 0x00 || s.chipVersion == 0xFF) {
    r = 60;
  } else {
    const float k = 0.5f + 0.5f * sinf(millis() / 700.0f);
    g = uint8_t(4 + k * 14), b = uint8_t(4 + k * 12);
  }
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
  cardList.begin();
  connection.begin();
  reader.begin();
  log_i("rfid-lab-r8: reader task on core 0. Last start: %s. Type token for the app's .env, help for commands.", resetReason());
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
  if (!state.apiStarted && connection.state() == Connection::State::Connected) {
    api.begin();
    state.apiStarted = true;
    log_i("App: http://%s.local or http://%s  (set BENCH_URL in friends-app/.env)", Connection::kHostname, WiFi.localIP().toString().c_str());
  }
  const bool changed = reader.update(state.stage);
  if (changed && state.stage.showSeq && now - state.stage.showAt < 200) state.view = 0;  // a tap brings the stage back
  if (state.apiStarted) {
    api.loop();
    if (changed || now - state.lastStatus >= 1000) {
      state.lastStatus = now;
      publishStatus();
    }
  }
  if (cardList.version() != state.cardsVersion) {
    state.cardsVersion = cardList.version();
    state.cards = cardList.list();
  }
  showLed();

  const float t = now / 1000.0f;
  fb.fillScreen(pal::night);
  if (state.view == 0) fv::stage(fb, state.stage, state.cards, t);
  else fv::list(fb, state.cards, state.stage, t);
  fv::header(fb, kViewNames[state.view], state.view, kViews, state.stage.present);
  char left[48], right[32];
  const bool joined = connection.state() == Connection::State::Connected;
  if (joined) snprintf(left, sizeof(left), "http://%s.local", Connection::kHostname);
  else snprintf(left, sizeof(left), "%s", connection.ssid().isEmpty() ? "no Wi-Fi saved: run W1 once" : "joining Wi-Fi...");
  snprintf(right, sizeof(right), "%d cards  %u app", cardList.count(), unsigned(api.clients()));
  fv::footer(fb, left, right, joined ? pal::teal : pal::amber);
  display.push();
}
