// R9's web side: the vault console at http://bench.local/ (vault_page.h), a JSON API and a
// WebSocket on port 80, with the same libraries and the same admin token as W2, R7 and R8.
//
// Endpoints (JSON). Reading is open on your Wi-Fi; every change needs the admin token
// (Authorization: Bearer <token>; the console asks for it once and keeps it in that browser).
// The master secret is never part of any answer: only its fingerprint.
//   GET    /api                  this list
//   GET    /api/status           reader chip, Wi-Fi, policy, the waiting job, the last tap in full
//   GET    /api/cards            the ledger
//   GET    /api/events           the last 40 taps and jobs, oldest first
//   POST   /api/provision        {"callsign": "ALPHA-1", "clearance": "operator"}: the next new card
//   POST   /api/rekey            the next card laid down gets a new epoch (and leaves quarantine)
//   POST   /api/wipe             the next card laid down: vault blocks to zeros, out of the ledger
//   POST   /api/cancel           stop waiting for a card
//   PUT    /api/card?uid=HEX     {"clearance": "secops", "quarantined": false}
//   DELETE /api/card?uid=HEX     forget the card (the card itself is not touched)
//   PUT    /api/policy           {"minClearance": "operator", "strictCloneGuard": true, "autoQuarantine": true}
//   POST   /api/chip-reset       hard reset of the reader chip
//   WS     /ws                   pushes {"type": "status" | "cards" | "tap", "data": ...}
#pragma once

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <mutex>

#include "api_token.h"
#include "vault_page.h"
#include "vault_reader.h"
#include "vault_store.h"

class VaultApi {
 public:
  VaultApi(VaultStore &store, VaultReader &reader, const ApiToken &token) : store_(store), reader_(reader), token_(token) {}

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
    auto json = [this](const char *path, WebRequestMethod method, std::function<void(AsyncWebServerRequest *, JsonObjectConst)> fn) {
      auto *h = new AsyncCallbackJsonWebHandler(path, [this, fn](AsyncWebServerRequest *req, JsonVariant &body) {
        if (!admin(req)) return;
        if (!body.is<JsonObject>()) return error(req, 400, "send a JSON object");
        fn(req, body.as<JsonObjectConst>());
      });
      h->setMethod(method);
      h->setMaxContentLength(256);
      server_.addHandler(h);
    };

    get("/", [](AsyncWebServerRequest *req) { req->send(200, "text/html; charset=utf-8", kVaultPage); });
    get("/api", [](AsyncWebServerRequest *req) { req->send(200, "application/json", kIndex); });
    get("/api/status", [this](AsyncWebServerRequest *req) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (status_.isEmpty()) return error(req, 503, "starting up");
      req->send(200, "application/json", status_);
    });
    get("/api/cards", [this](AsyncWebServerRequest *req) { req->send(200, "application/json", cardsJson()); });
    get("/api/events", [this](AsyncWebServerRequest *req) {
      JsonDocument d;
      JsonArray a = d.to<JsonArray>();
      for (const VaultEvent &e : reader_.events()) VaultReader::eventJson(a.add<JsonObject>(), e);
      send(req, 200, d);
    });

    json("/api/provision", HTTP_POST, [this](AsyncWebServerRequest *req, JsonObjectConst b) {
      const char *callsign = b["callsign"] | "";
      if (!validCallsign(callsign)) return error(req, 400, "callsign: 1-15 characters, A-Z, digits, - . _");
      vault::Clearance c = vault::Clearance::Operator;
      if (!b["clearance"].isNull() && !vault::clearanceFromName(b["clearance"] | "", c))
        return error(req, 400, "clearance: observer, operator, secops or chief");
      reader_.startJob(Scene::Job::Provision, callsign, c);
      ok(req);
    });
    post("/api/rekey", [this](AsyncWebServerRequest *req) { job(req, Scene::Job::Rekey); });
    post("/api/wipe", [this](AsyncWebServerRequest *req) { job(req, Scene::Job::Wipe); });
    post("/api/cancel", [this](AsyncWebServerRequest *req) { job(req, Scene::Job::None); });
    post("/api/chip-reset", [this](AsyncWebServerRequest *req) {
      if (!admin(req)) return;
      reader_.requestChipReset();
      ok(req);
    });

    json("/api/card", HTTP_PUT, [this](AsyncWebServerRequest *req, JsonObjectConst b) {
      uint8_t uid[10], len = 0;
      if (!uidParam(req, uid, len)) return;
      vault::Clearance c = vault::Clearance::Operator;
      const bool setClearance = !b["clearance"].isNull();
      if (setClearance && !vault::clearanceFromName(b["clearance"] | "", c)) return error(req, 400, "clearance: observer, operator, secops or chief");
      const bool setQuarantine = b["quarantined"].is<bool>();
      const bool quarantined = b["quarantined"] | false;
      const bool found = store_.update(uid, len, [&](vault::LedgerCard &card) {
        if (setClearance) card.clearance = c;
        if (setQuarantine) card.quarantined = quarantined;
      });
      if (!found) return error(req, 404, "no card with this uid in the ledger");
      ok(req);
    });
    server_.on(AsyncURIMatcher::exact("/api/card"), HTTP_DELETE, [this](AsyncWebServerRequest *req) {
      uint8_t uid[10], len = 0;
      if (!admin(req) || !uidParam(req, uid, len)) return;
      if (!store_.remove(uid, len)) return error(req, 404, "no card with this uid in the ledger");
      ok(req);
    });
    json("/api/policy", HTTP_PUT, [this](AsyncWebServerRequest *req, JsonObjectConst b) {
      vault::Policy p = store_.policy();
      if (!b["minClearance"].isNull() && !vault::clearanceFromName(b["minClearance"] | "", p.minClearance))
        return error(req, 400, "minClearance: observer, operator, secops or chief");
      if (b["strictCloneGuard"].is<bool>()) p.strictCloneGuard = b["strictCloneGuard"];
      if (b["autoQuarantine"].is<bool>()) p.autoQuarantine = b["autoQuarantine"];
      store_.setPolicy(p);
      ok(req);
    });

    server_.onNotFound([](AsyncWebServerRequest *req) { error(req, 404, "no such endpoint, see /api"); });
    server_.begin();
    log_i("Web server on port 80: console at /, API at /api, live updates at /ws");
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
    const uint32_t v = store_.version();
    if (v != ledgerVersion_) {
      ledgerVersion_ = v;
      if (ws_.count()) ws_.textAll(wrap("cards", cardsJson()));
    }
  }

  size_t clients() const { return ws_.count(); }

  static void cardJson(JsonObject o, const vault::LedgerCard &c) {
    char uid[21];
    vault::toHex(c.uid, c.uidLen, uid, sizeof(uid));
    o["uid"] = uid;
    o["number"] = c.number;
    o["callsign"] = static_cast<const char *>(c.callsign);  // copied, see R7's log entry 44
    o["clearance"] = vault::clearanceName(c.clearance);
    o["quarantined"] = c.quarantined;
    o["epoch"] = c.epoch;
    o["counter"] = c.counter;
    o["taps"] = c.taps;
    o["tears"] = c.tears;
    o["alerts"] = c.alerts;
  }

 private:
  void job(AsyncWebServerRequest *req, Scene::Job j) {
    if (!admin(req)) return;
    reader_.startJob(j);
    ok(req);
  }

  String cardsJson() {
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (const vault::LedgerCard &c : store_.list()) cardJson(a.add<JsonObject>(), c);
    String out;
    serializeJson(d, out);
    return out;
  }

  // Callsigns go to a small screen font and into block 8: plain ASCII only.
  static bool validCallsign(const char *s) {
    const size_t n = strlen(s);
    if (n == 0 || n > 15) return false;
    for (size_t i = 0; i < n; i++)
      if (!(isalnum(uint8_t(s[i])) || strchr("-._", s[i]))) return false;
    return true;
  }

  bool admin(AsyncWebServerRequest *req) {
    if (req->hasHeader("Authorization") && token_.allows(req->header("Authorization"))) return true;
    error(req, 401, "admin action: send Authorization: Bearer <token> (serial command: token)");
    return false;
  }

  bool uidParam(AsyncWebServerRequest *req, uint8_t *uid, uint8_t &len) {
    if (req->hasParam("uid") && vault::uidFromHex(req->getParam("uid")->value().c_str(), uid, len)) return true;
    error(req, 400, "uid: 8, 14 or 20 hex digits, e.g. ?uid=9C4F2A1B");
    return false;
  }

  static void ok(AsyncWebServerRequest *req) { req->send(200, "application/json", "{\"ok\":true}"); }

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
      "{\"endpoints\":[\"GET /api/status\",\"GET /api/cards\",\"GET /api/events\",\"POST /api/provision {callsign,clearance}\","
      "\"POST /api/rekey\",\"POST /api/wipe\",\"POST /api/cancel\",\"PUT /api/card?uid=HEX {clearance,quarantined}\","
      "\"DELETE /api/card?uid=HEX\",\"PUT /api/policy {minClearance,strictCloneGuard,autoQuarantine}\",\"POST /api/chip-reset\",\"WS /ws\"],"
      "\"admin\":\"every change: Authorization: Bearer <token>\"}";

  VaultStore &store_;
  VaultReader &reader_;
  const ApiToken &token_;
  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  std::mutex mutex_;
  String status_;
  uint32_t lastCleanup_ = 0, ledgerVersion_ = 0;
};
