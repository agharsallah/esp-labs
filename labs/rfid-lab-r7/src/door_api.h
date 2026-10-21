// R7's web side: a JSON API and a WebSocket on port 80, for the React app in esp-labs/apps/door-app.
// ESPAsyncWebServer answers in its own network task, so the screen and the reader never wait.
//
// Endpoints (JSON). Reading is open on your Wi-Fi; every change needs the admin token
// (header "Authorization: Bearer <token>", serial command `token`). The app's dev server adds it
// from esp-labs/apps/door-app/.env, so the token never reaches the browser.
//   GET    /api                  this list
//   GET    /api/status           both locks, where the reader is, reader chip, Wi-Fi, clock,
//                                lockdown, enrollment
//   GET    /api/cards            the card list, each with what a tap would do right now at
//                                each lock: "now": {"front": "granted", "admin": "admins only"}
//   GET    /api/events?since=N   the access log after event N (the last 50 are kept)
//   POST   /api/enroll           {"name": "Anna", "role": "resident|guest|maintenance|admin"}  -> 202 job
//   POST   /api/replace?uid=HEX  a new card takes over this card's name and rules; the old one
//                                is blocked (lost or broken card)                         -> 202 job
//   POST   /api/cancel           stop waiting for a card (enroll, replace, copy)
//   PUT    /api/card?uid=HEX     any of {"name", "role", "blocked", "validUntil", "fromHour",
//                                "toHour", "usesLeft"}                                   -> 200 card
//   DELETE /api/card?uid=HEX     remove it from the list (the card itself is not touched)
//   POST   /api/label?uid=HEX    write its name and role into blocks 4-5 (card on reader) -> 202 job
//   POST   /api/wipe?uid=HEX     data blocks 4-6, 8-10 back to zeros (card on reader)     -> 202 job
//   POST   /api/inspect          read whatever card is on the reader                     -> 202 job
//   GET    /api/snapshots        saved card data (blocks 4-6, 8-10), up to 8
//   POST   /api/save             {"name": "..."} (optional): save the card on the reader  -> 202 job
//   POST   /api/import           {"name", "blocks": [{"block": 4, "hex": "32 hex"}]}      -> 201
//   POST   /api/restore?id=N     write saved data onto the card on the reader             -> 202 job
//   DELETE /api/snapshot?id=N    forget saved data
//   POST   /api/copy             read the card on the reader, then write it onto the next
//                                different card laid down within 20 s                    -> 202 job
//   POST   /api/open?lock=front  open a lock for 5 s from the app (front or admin)
//   PUT    /api/reader-at        {"lock": "front" | "admin"}: which lock the reader is mounted at
//   PUT    /api/lockdown         {"on": true}: only maintenance and admin cards open
//   WS     /ws                   pushes {"type": "status" | "cards" | "snapshots" | "event" | "job", ...}
// A 202 answers {"job": id}; the result arrives later as a "job" message with that id.
#pragma once

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <mutex>

#include "api_token.h"
#include "door.h"
#include "registry.h"
#include "snapshots.h"

class DoorApi {
 public:
  DoorApi(Registry &reg, snap::Store &snaps, Door &door, const ApiToken &token)
      : reg_(reg), snaps_(snaps), door_(door), token_(token) {}

  void begin() {
    ws_.onEvent([this](AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *, uint8_t *, size_t) {
      if (type != WS_EVT_CONNECT) return;
      std::lock_guard<std::mutex> lock(mutex_);  // the latest picture straight away
      if (!status_.isEmpty()) client->text(wrap("status", status_));
      client->text(wrap("cards", cardsJson()));
      client->text(wrap("snapshots", snapshotsJson()));
    });
    server_.addHandler(&ws_);

    auto get = [this](const char *path, ArRequestHandlerFunction fn) { server_.on(AsyncURIMatcher::exact(path), HTTP_GET, fn); };
    auto post = [this](const char *path, ArRequestHandlerFunction fn) { server_.on(AsyncURIMatcher::exact(path), HTTP_POST, fn); };

    server_.on(AsyncURIMatcher::exact("/"), HTTP_GET, [](AsyncWebServerRequest *req) {
      req->send(200, "text/plain", "rfid-lab-r7, the bench door. Use the React app in esp-labs/apps/door-app, or the JSON API at /api\n");
    });
    get("/api", [](AsyncWebServerRequest *req) { req->send(200, "application/json", kIndex); });
    get("/api/status", [this](AsyncWebServerRequest *req) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (status_.isEmpty()) return error(req, 503, "starting up");
      req->send(200, "application/json", status_);
    });
    get("/api/cards", [this](AsyncWebServerRequest *req) { req->send(200, "application/json", cardsJson()); });
    get("/api/events", [this](AsyncWebServerRequest *req) {
      const uint32_t since = req->hasParam("since") ? uint32_t(req->getParam("since")->value().toInt()) : 0;
      JsonDocument d;
      JsonArray a = d.to<JsonArray>();
      for (const DoorEvent &e : door_.events(since)) {
        JsonObject o = a.add<JsonObject>();
        o["seq"] = e.seq, o["at"] = e.at, o["uid"] = doorjson::copied(e.uid), o["name"] = doorjson::copied(e.name), o["what"] = doorjson::copied(e.what),
        o["decision"] = doorjson::copied(e.decision), o["lock"] = doorjson::copied(e.lock);
      }
      send(req, 200, d);
    });

    json("/api/enroll", HTTP_POST, [this](AsyncWebServerRequest *req, JsonObjectConst body) {
      Door::Request r;
      r.job = Door::Job::Enroll;
      const char *name = body["name"] | "";
      if (!validName(name)) return error(req, 400, "name: 1-23 plain characters (A-Z, digits, space, - . ')");
      strlcpy(r.name, name, sizeof(r.name));
      if (!door::roleFromName(body["role"] | "resident", r.role)) return error(req, 400, "role: resident, guest, maintenance or admin");
      queued(req, door_.submit(r));
    });
    post("/api/cancel", [this](AsyncWebServerRequest *req) { simpleJob(req, Door::Job::Cancel); });
    json("/api/card", HTTP_PUT, [this](AsyncWebServerRequest *req, JsonObjectConst body) { editCard(req, body); });
    server_.on(AsyncURIMatcher::exact("/api/card"), HTTP_DELETE, [this](AsyncWebServerRequest *req) {
      uint8_t uid[10], len = 0;
      if (!admin(req) || !uidParam(req, uid, len)) return;
      if (!reg_.remove(uid, len)) return error(req, 404, "no card with this uid in the list");
      req->send(200, "application/json", "{\"ok\":true}");
    });
    for (const char *path : {"/api/label", "/api/wipe", "/api/replace"}) {
      post(path, [this, path](AsyncWebServerRequest *req) {
        Door::Request r;
        if (!admin(req) || !uidParam(req, r.uid, r.uidLen)) return;
        r.job = strcmp(path, "/api/label") == 0 ? Door::Job::Label : strcmp(path, "/api/wipe") == 0 ? Door::Job::Wipe : Door::Job::Replace;
        queued(req, door_.submit(r));
      });
    }
    post("/api/inspect", [this](AsyncWebServerRequest *req) { simpleJob(req, Door::Job::Inspect); });

    // Saved card data.
    get("/api/snapshots", [this](AsyncWebServerRequest *req) { req->send(200, "application/json", snapshotsJson()); });
    json("/api/save", HTTP_POST, [this](AsyncWebServerRequest *req, JsonObjectConst body) {
      Door::Request r;
      r.job = Door::Job::Save;
      const char *name = body["name"] | "";
      if (name[0] && !validName(name)) return error(req, 400, "name: 1-23 plain characters (A-Z, digits, space, - . ')");
      strlcpy(r.name, name, sizeof(r.name));
      queued(req, door_.submit(r));
    });
    json("/api/import", HTTP_POST, [this](AsyncWebServerRequest *req, JsonObjectConst body) { importSnapshot(req, body); }, 1024);
    post("/api/restore", [this](AsyncWebServerRequest *req) {
      Door::Request r;
      if (!admin(req) || !idParam(req, r.snapshot)) return;
      r.job = Door::Job::Restore;
      queued(req, door_.submit(r));
    });
    server_.on(AsyncURIMatcher::exact("/api/snapshot"), HTTP_DELETE, [this](AsyncWebServerRequest *req) {
      uint32_t id = 0;
      if (!admin(req) || !idParam(req, id)) return;
      if (!snaps_.remove(id)) return error(req, 404, "no saved data with this id");
      req->send(200, "application/json", "{\"ok\":true}");
    });
    post("/api/copy", [this](AsyncWebServerRequest *req) { simpleJob(req, Door::Job::Copy); });

    post("/api/open", [this](AsyncWebServerRequest *req) {
      Door::Request r;
      r.job = Door::Job::Open;
      if (!admin(req) || !lockParam(req, r.lock)) return;
      queued(req, door_.submit(r));
    });
    post("/api/chip-reset", [this](AsyncWebServerRequest *req) { simpleJob(req, Door::Job::ChipReset); });
    json("/api/lockdown", HTTP_PUT, [this](AsyncWebServerRequest *req, JsonObjectConst body) {
      if (!body["on"].is<bool>()) return error(req, 400, "send {\"on\": true} or {\"on\": false}");
      reg_.setLockdown(body["on"].as<bool>());
      req->send(200, "application/json", body["on"].as<bool>() ? "{\"lockdown\":true}" : "{\"lockdown\":false}");
    });
    json("/api/reader-at", HTTP_PUT, [this](AsyncWebServerRequest *req, JsonObjectConst body) {
      door::Lock at;
      if (!door::lockFromId(body["lock"] | "", at)) return error(req, 400, "send {\"lock\": \"front\"} or {\"lock\": \"admin\"}");
      reg_.setReaderAt(at);
      log_i("the reader is now at the %s", door::lockTitle(at));
      req->send(200, "application/json", at == door::Lock::AdminRoom ? "{\"readerAt\":\"admin\"}" : "{\"readerAt\":\"front\"}");
    });

    server_.onNotFound([](AsyncWebServerRequest *req) { error(req, 404, "no such endpoint, see /api"); });
    server_.begin();
    log_i("Web server on port 80: API at /api, live updates at /ws");
  }

  // ---- From loop(): the status, card list changes and the reader's messages, to every client ----

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
    while (door_.takeMessage(msg))
      if (ws_.count()) ws_.textAll(msg);
    // The list changed (an edit, a tap, an enrollment), or a minute passed ("now" depends on the time).
    const uint32_t v = reg_.version();
    if (v != cardsVersion_ || now - cardsSent_ > 60'000) {
      cardsVersion_ = v, cardsSent_ = now;
      if (ws_.count()) ws_.textAll(wrap("cards", cardsJson()));
    }
    if (snaps_.version() != snapsVersion_) {
      snapsVersion_ = snaps_.version();
      if (ws_.count()) ws_.textAll(wrap("snapshots", snapshotsJson()));
    }
  }

  size_t clients() const { return ws_.count(); }

 private:
  using JsonFn = std::function<void(AsyncWebServerRequest *, JsonObjectConst)>;

  // A JSON-body route; admin only (every route with a body changes something).
  void json(const char *path, WebRequestMethodComposite method, JsonFn fn, size_t maxBytes = 512) {
    auto *h = new AsyncCallbackJsonWebHandler(path, [this, fn](AsyncWebServerRequest *req, JsonVariant &body) {
      if (!admin(req)) return;
      if (!body.is<JsonObject>()) return error(req, 400, "send a JSON object");
      fn(req, body.as<JsonObjectConst>());
    });
    h->setMethod(method);
    h->setMaxContentLength(maxBytes);
    server_.addHandler(h);
  }

  // ?lock=front|admin, front when left out.
  static bool lockParam(AsyncWebServerRequest *req, door::Lock &out) {
    out = door::Lock::Front;
    if (!req->hasParam("lock")) return true;
    if (door::lockFromId(req->getParam("lock")->value().c_str(), out)) return true;
    error(req, 400, "lock: front or admin");
    return false;
  }

  void simpleJob(AsyncWebServerRequest *req, Door::Job job) {
    if (!admin(req)) return;
    Door::Request r;
    r.job = job;
    queued(req, door_.submit(r));
  }

  void editCard(AsyncWebServerRequest *req, JsonObjectConst b) {
    uint8_t uid[10], len = 0;
    if (!uidParam(req, uid, len)) return;
    // Check everything first, then change the card in one go.
    const char *name = b["name"] | static_cast<const char *>(nullptr);
    if (name && !validName(name)) return error(req, 400, "name: 1-23 plain characters (A-Z, digits, space, - . ')");
    door::Role role{};
    const bool hasRole = !b["role"].isNull();
    if (hasRole && !door::roleFromName(b["role"] | "", role)) return error(req, 400, "role: resident, guest, maintenance or admin");
    if (!b["blocked"].isNull() && !b["blocked"].is<bool>()) return error(req, 400, "blocked: true or false");
    const long until = b["validUntil"] | -1L, from = b["fromHour"] | -1L, to = b["toHour"] | -1L, uses = b["usesLeft"] | -2L;
    if (!b["validUntil"].isNull() && until < 0) return error(req, 400, "validUntil: unix seconds, 0 = no end date");
    if ((!b["fromHour"].isNull() && (from < 0 || from > 23)) || (!b["toHour"].isNull() && (to < 0 || to > 24)))
      return error(req, 400, "fromHour 0-23, toHour 0-24 (equal = any time)");
    if (!b["usesLeft"].isNull() && (uses < -1 || uses > 9999)) return error(req, 400, "usesLeft: -1 (unlimited) to 9999");
    door::Card after;
    const bool found = reg_.update(uid, len, [&](door::Card &c) {
      if (name) strlcpy(c.name, name, sizeof(c.name));
      if (hasRole) c.role = role;
      if (b["blocked"].is<bool>()) c.blocked = b["blocked"].as<bool>();
      if (!b["validUntil"].isNull()) c.validUntil = uint32_t(until);
      if (!b["fromHour"].isNull()) c.fromHour = uint8_t(from);
      if (!b["toHour"].isNull()) c.toHour = uint8_t(to == 24 ? 0 : to);
      if (!b["usesLeft"].isNull()) c.usesLeft = int16_t(uses);
    }, &after);
    if (!found) return error(req, 404, "no card with this uid in the list");
    door::Clock clk;
    doorjson::clockNow(clk);
    JsonDocument d;
    d["ok"] = true;
    doorjson::card(d["card"].to<JsonObject>(), after, clk, reg_.lockdown());
    send(req, 200, d);
  }

  // A snapshot from a file the app saved earlier: lab blocks only, each 16 bytes as 32 hex digits.
  void importSnapshot(AsyncWebServerRequest *req, JsonObjectConst b) {
    const char *name = b["name"] | "";
    if (!validName(name)) return error(req, 400, "name: 1-23 plain characters (A-Z, digits, space, - . ')");
    JsonArrayConst blocks = b["blocks"];
    if (blocks.isNull() || blocks.size() == 0 || blocks.size() > snap::kBlocks)
      return error(req, 400, "blocks: 1-6 entries of {\"block\": 4|5|6|8|9|10, \"hex\": 32 hex digits}");
    snap::Snapshot s;
    strlcpy(s.name, name, sizeof(s.name));
    for (JsonObjectConst one : blocks) {
      const int i = snap::slotOf(one["block"] | -1);
      if (i < 0 || (s.have & (1 << i)) || !hexBlock(one["hex"] | "", s.data[i]))
        return error(req, 400, "each block once: {\"block\": 4|5|6|8|9|10, \"hex\": 32 hex digits}");
      s.have |= uint8_t(1 << i);
    }
    door::Clock clk;
    doorjson::clockNow(clk);
    s.at = clk.synced ? clk.now : 0;
    if (!snaps_.add(s)) return error(req, 409, "8 snapshots saved already: delete one first");
    JsonDocument d;
    doorjson::snapshot(d.to<JsonObject>(), s);
    send(req, 201, d);
  }

  static bool hexBlock(const char *hex, uint8_t out[16]) {
    if (strlen(hex) != 32) return false;
    for (int i = 0; i < 32; i++)
      if (!isxdigit(uint8_t(hex[i]))) return false;
    for (int i = 0; i < 16; i++) {
      const char two[3] = {hex[i * 2], hex[i * 2 + 1], 0};
      out[i] = uint8_t(strtoul(two, nullptr, 16));
    }
    return true;
  }

  String snapshotsJson() {
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (const snap::Snapshot &s : snaps_.list()) doorjson::snapshot(a.add<JsonObject>(), s);
    String out;
    serializeJson(d, out);
    return out;
  }

  String cardsJson() {
    door::Clock clk;
    doorjson::clockNow(clk);
    const bool lockdown = reg_.lockdown();
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (const door::Card &c : reg_.list()) doorjson::card(a.add<JsonObject>(), c, clk, lockdown);
    String out;
    serializeJson(d, out);
    return out;
  }

  // Names go to a 16-character card block and a small screen font: plain ASCII only.
  static bool validName(const char *s) {
    const size_t n = strlen(s);
    if (n == 0 || n > 23) return false;
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
    if (req->hasParam("uid") && door::uidFromHex(req->getParam("uid")->value().c_str(), uid, len)) return true;
    error(req, 400, "uid: 8, 14 or 20 hex digits, e.g. ?uid=9C4F2A1B");
    return false;
  }

  bool idParam(AsyncWebServerRequest *req, uint32_t &id) {
    const long v = req->hasParam("id") ? req->getParam("id")->value().toInt() : 0;
    if (v > 0) {
      id = uint32_t(v);
      return true;
    }
    error(req, 400, "id: the snapshot's number, e.g. ?id=3");
    return false;
  }

  static void queued(AsyncWebServerRequest *req, uint32_t id) {
    if (!id) return error(req, 503, "the reader is busy, try again");
    req->send(202, "application/json", String("{\"job\":") + id + "}");
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
      "{\"endpoints\":[\"GET /api/status\",\"GET /api/cards\",\"GET /api/events?since=N\",\"POST /api/enroll {name,role}\","
      "\"POST /api/replace?uid=HEX\",\"POST /api/cancel\",\"PUT /api/card?uid=HEX {name,role,blocked,validUntil,fromHour,toHour,usesLeft}\","
      "\"DELETE /api/card?uid=HEX\",\"POST /api/label?uid=HEX\",\"POST /api/wipe?uid=HEX\",\"POST /api/inspect\","
      "\"GET /api/snapshots\",\"POST /api/save {name}\",\"POST /api/import {name,blocks}\",\"POST /api/restore?id=N\","
      "\"DELETE /api/snapshot?id=N\",\"POST /api/copy\","
      "\"POST /api/open?lock=front|admin\",\"PUT /api/reader-at {lock}\",\"POST /api/chip-reset\",\"PUT /api/lockdown {on}\",\"WS /ws\"],"
      "\"admin\":\"every change: Authorization: Bearer <token>\"}";

  Registry &reg_;
  snap::Store &snaps_;
  Door &door_;
  const ApiToken &token_;
  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  std::mutex mutex_;
  String status_;
  uint32_t lastCleanup_ = 0, cardsVersion_ = 0, cardsSent_ = 0, snapsVersion_ = 0;
};
