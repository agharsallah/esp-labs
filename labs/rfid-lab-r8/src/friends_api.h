// R8's web side, for the kids' app in esp-labs/apps/friends-app: a small JSON API and a WebSocket on
// port 80, the same libraries and the same admin token as R7 (kit/lib/wifi-kit/api_token.h).
//
// Endpoints (JSON). Reading is open on your Wi-Fi; every change needs the admin token, which the
// app's dev server adds from esp-labs/apps/friends-app/.env (the browser never holds it).
//   GET    /api                  this list
//   GET    /api/status           reader chip, Wi-Fi, board, the card on the reader, the last show
//   GET    /api/cards            the cards and who lives on each
//   GET    /api/cast             the friend ids this firmware can draw
//   PUT    /api/card?uid=HEX     {"character": "fox" | "", "name": "Max"}: move a friend onto the card
//   DELETE /api/card?uid=HEX     forget the card (it comes back, empty, on its next tap)
//   POST   /api/show?uid=HEX     show this card's friend on the screen and in every app
//   POST   /api/chip-reset       hard reset of the reader chip
//   WS     /ws                   pushes {"type": "status" | "cards" | "tap", "data": ...}
#pragma once

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <mutex>

#include "api_token.h"
#include "characters.h"
#include "friends.h"
#include "tap_reader.h"

class FriendsApi {
 public:
  FriendsApi(friends::List &list, TapReader &reader, const ApiToken &token) : list_(list), reader_(reader), token_(token) {}

  void begin() {
    ws_.onEvent([this](AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *, uint8_t *, size_t) {
      if (type != WS_EVT_CONNECT) return;
      std::lock_guard<std::mutex> lock(mutex_);  // the latest picture straight away
      if (!status_.isEmpty()) client->text(wrap("status", status_));
      client->text(wrap("cards", cardsJson()));
    });
    server_.addHandler(&ws_);

    auto get = [this](const char *path, ArRequestHandlerFunction fn) { server_.on(AsyncURIMatcher::exact(path), HTTP_GET, fn); };
    auto post = [this](const char *path, ArRequestHandlerFunction fn) { server_.on(AsyncURIMatcher::exact(path), HTTP_POST, fn); };

    server_.on(AsyncURIMatcher::exact("/"), HTTP_GET, [](AsyncWebServerRequest *req) {
      req->send(200, "text/plain", "rfid-lab-r8, card friends. Use the app in esp-labs/apps/friends-app, or the JSON API at /api\n");
    });
    get("/api", [](AsyncWebServerRequest *req) { req->send(200, "application/json", kIndex); });
    get("/api/status", [this](AsyncWebServerRequest *req) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (status_.isEmpty()) return error(req, 503, "starting up");
      req->send(200, "application/json", status_);
    });
    get("/api/cards", [this](AsyncWebServerRequest *req) { req->send(200, "application/json", cardsJson()); });
    get("/api/cast", [](AsyncWebServerRequest *req) {
      JsonDocument d;
      JsonArray a = d.to<JsonArray>();
      for (const cast::Character &c : cast::kCast) {
        JsonObject o = a.add<JsonObject>();
        o["id"] = c.id, o["name"] = c.name;
      }
      send(req, 200, d);
    });

    auto *edit = new AsyncCallbackJsonWebHandler("/api/card", [this](AsyncWebServerRequest *req, JsonVariant &body) {
      if (!admin(req)) return;
      if (!body.is<JsonObject>()) return error(req, 400, "send a JSON object");
      editCard(req, body.as<JsonObjectConst>());
    });
    edit->setMethod(HTTP_PUT);
    edit->setMaxContentLength(256);
    server_.addHandler(edit);
    server_.on(AsyncURIMatcher::exact("/api/card"), HTTP_DELETE, [this](AsyncWebServerRequest *req) {
      uint8_t uid[10], len = 0;
      if (!admin(req) || !uidParam(req, uid, len)) return;
      if (!list_.remove(uid, len)) return error(req, 404, "no card with this uid in the list");
      req->send(200, "application/json", "{\"ok\":true}");
    });
    post("/api/show", [this](AsyncWebServerRequest *req) {
      uint8_t uid[10], len = 0;
      if (!admin(req) || !uidParam(req, uid, len)) return;
      if (!reader_.show(uid, len)) return error(req, 404, "no card with this uid in the list");
      req->send(200, "application/json", "{\"ok\":true}");
    });
    post("/api/chip-reset", [this](AsyncWebServerRequest *req) {
      if (!admin(req)) return;
      reader_.requestChipReset();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server_.onNotFound([](AsyncWebServerRequest *req) { error(req, 404, "no such endpoint, see /api"); });
    server_.begin();
    log_i("Web server on port 80: API at /api, live updates at /ws");
  }

  // ---- From loop() ------------------------------------------------------------------------------

  void publishStatus(const String &json) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_ = json;
    }
    if (ws_.count()) ws_.textAll(wrap("status", json));
  }

  void loop() {
    const uint32_t now = millis();
    if (now - lastCleanup_ > 2000) lastCleanup_ = now, ws_.cleanupClients();
    String msg;
    while (reader_.takeMessage(msg))
      if (ws_.count()) ws_.textAll(msg);
    const uint32_t v = list_.version();
    if (v != listVersion_) {
      listVersion_ = v;
      if (ws_.count()) ws_.textAll(wrap("cards", cardsJson()));
    }
  }

  size_t clients() const { return ws_.count(); }

 private:
  void editCard(AsyncWebServerRequest *req, JsonObjectConst b) {
    uint8_t uid[10], len = 0;
    if (!uidParam(req, uid, len)) return;
    // Check everything first, then change the card in one go.
    const char *character = b["character"] | static_cast<const char *>(nullptr);
    if (character && character[0] && !cast::find(character)) return error(req, 400, "character: one of the ids in GET /api/cast, or \"\"");
    const char *name = b["name"] | static_cast<const char *>(nullptr);
    if (name && !validName(name)) return error(req, 400, "name: up to 15 plain characters (A-Z, digits, space, - . ')");
    friends::Card after;
    const bool found = list_.update(uid, len, [&](friends::Card &c) {
      if (character) strlcpy(c.character, character, sizeof(c.character));
      if (name) strlcpy(c.name, name, sizeof(c.name));
    }, &after);
    if (!found) return error(req, 404, "no card with this uid in the list");
    JsonDocument d;
    d["ok"] = true;
    card(d["card"].to<JsonObject>(), after);
    send(req, 200, d);
  }

  static void card(JsonObject o, const friends::Card &c) {
    char uid[21];
    friends::uidToHex(c.uid, c.uidLen, uid, sizeof(uid));
    o["uid"] = uid;
    o["number"] = c.number;
    o["character"] = static_cast<const char *>(c.character);  // copied, see R7's log entry 44
    o["name"] = static_cast<const char *>(c.name);
    o["taps"] = c.taps;
  }

  String cardsJson() {
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (const friends::Card &c : list_.list()) card(a.add<JsonObject>(), c);
    String out;
    serializeJson(d, out);
    return out;
  }

  // Names go to a small screen font: plain ASCII only, "" = use the character's own name.
  static bool validName(const char *s) {
    const size_t n = strlen(s);
    if (n > 15) return false;
    for (size_t i = 0; i < n; i++)
      if (!(isalnum(uint8_t(s[i])) || strchr(" -.'_", s[i]))) return false;
    return true;
  }

  bool admin(AsyncWebServerRequest *req) {
    if (req->hasHeader("Authorization") && token_.allows(req->header("Authorization"))) return true;
    error(req, 401, "admin action: send Authorization: Bearer <token> (serial command: token)");
    return false;
  }

  bool uidParam(AsyncWebServerRequest *req, uint8_t *uid, uint8_t &len) {
    if (req->hasParam("uid") && friends::uidFromHex(req->getParam("uid")->value().c_str(), uid, len)) return true;
    error(req, 400, "uid: 8, 14 or 20 hex digits, e.g. ?uid=9C4F2A1B");
    return false;
  }

  static void send(AsyncWebServerRequest *req, int code, JsonDocument &d) {
    String out;
    serializeJson(d, out);
    req->send(code, "application/json", out);
  }

  static void error(AsyncWebServerRequest *req, int code, const char *message) {
    JsonDocument d;
    d["error"] = message;
    send(req, code, d);
  }

  static String wrap(const char *type, const String &data) { return String("{\"type\":\"") + type + "\",\"data\":" + data + "}"; }

  static constexpr const char *kIndex =
      "{\"endpoints\":[\"GET /api/status\",\"GET /api/cards\",\"GET /api/cast\",\"PUT /api/card?uid=HEX {character,name}\","
      "\"DELETE /api/card?uid=HEX\",\"POST /api/show?uid=HEX\",\"POST /api/chip-reset\",\"WS /ws\"],"
      "\"admin\":\"every change: Authorization: Bearer <token>\"}";

  friends::List &list_;
  TapReader &reader_;
  const ApiToken &token_;
  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  std::mutex mutex_;
  String status_;
  uint32_t lastCleanup_ = 0, listVersion_ = 0;
};
