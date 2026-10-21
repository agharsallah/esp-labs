// S4's web side: the phone page at http://bench.local/, a JSON API and a WebSocket on port 80
// (the same libraries as W2, R7 and R8).
//
// Privacy first. A microphone you can reach over Wi-Fi is a bug in a box, so:
//   - every /api route (except the index) and the WebSocket need the admin token: the same one R7
//     and R8 use, kept in NVS, printed only by the serial command `token`. The page asks for it
//     once and keeps it in the tab's sessionStorage (gone when the tab closes);
//   - the board shows every recording, whoever started it: the REC dot, the red LED, the big REC;
//   - clips live in RAM only and vanish on reset.
//
// Endpoints:
//   GET    /                  the page (no audio in it: it fetches clips with the token)
//   GET    /api               this list
//   GET    /api/status        state, level, settings, store use
//   GET    /api/clips         the clips (facts, not sound)
//   GET    /api/clip?n=N      clip N as a WAV file, normalised (&raw=1: the numbers as recorded)
//   DELETE /api/clip?n=N      delete clip N
//   POST   /api/arm?on=1|0    voice trigger on or off
//   POST   /api/rec           start recording now
//   POST   /api/stop          stop recording, and stop the board's playback
//   POST   /api/play?n=N      play clip N on the board's speaker (&speed=0.5-2.0, default 1)
//   WS     /ws?token=...      pushes {"type": "status" | "clips", "data": ...}, ~10 a second
//
// Web requests run in the network task; loop() owns the recorder. So the API never changes the
// recorder itself: it queues a WebCommand and loop() applies it (W2's commands.h pattern). Reading
// a clip is the exception that's safe: the clip is leased, so a delete can't free it mid-download.
#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <memory>
#include <mutex>

#include "api_token.h"
#include "clips.h"
#include "recorder.h"
#include "web_page.h"

struct WebCommand {
  enum class Type : uint8_t { Arm, Rec, Stop, Delete, Play };
  Type type;
  uint32_t value = 0;  // Arm: 1/0 · Delete, Play: the clip number
  float speed = 1.0f;  // Play: 0.5-2.0
};

class RecorderApi {
 public:
  RecorderApi(rec::ClipStore &store, const ApiToken &token) : store_(store), token_(token) {}

  void begin() {
    queue_ = xQueueCreate(8, sizeof(WebCommand));
    ws_.onEvent([this](AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *, size_t) {
      if (type != WS_EVT_CONNECT) return;
      // The browser can't add headers to a WebSocket, so the token comes in the URL: /ws?token=...
      auto *req = static_cast<AsyncWebServerRequest *>(arg);
      if (!req || !req->hasParam("token") || !token_.allows("Bearer " + req->getParam("token")->value())) {
        client->close(4001, "token");
        return;
      }
      std::lock_guard<std::mutex> lock(mutex_);  // the latest picture straight away
      if (!status_.isEmpty()) client->text(wrap("status", status_));
      client->text(wrap("clips", clipsJson()));
    });
    server_.addHandler(&ws_);

    server_.on(AsyncURIMatcher::exact("/"), HTTP_GET, [](AsyncWebServerRequest *req) {
      AsyncWebServerResponse *res = req->beginResponse(200, "text/html", reinterpret_cast<const uint8_t *>(kPage), strlen(kPage));
      res->addHeader("Cache-Control", "no-store");
      req->send(res);
    });
    server_.on(AsyncURIMatcher::exact("/api"), HTTP_GET, [](AsyncWebServerRequest *req) { req->send(200, "application/json", kIndex); });
    server_.on(AsyncURIMatcher::exact("/api/status"), HTTP_GET, [this](AsyncWebServerRequest *req) {
      if (!admin(req)) return;
      std::lock_guard<std::mutex> lock(mutex_);
      if (status_.isEmpty()) return error(req, 503, "starting up");
      req->send(200, "application/json", status_);
    });
    server_.on(AsyncURIMatcher::exact("/api/clips"), HTTP_GET, [this](AsyncWebServerRequest *req) {
      if (!admin(req)) return;
      req->send(200, "application/json", clipsJson());
    });
    server_.on(AsyncURIMatcher::exact("/api/clip"), HTTP_GET, [this](AsyncWebServerRequest *req) { download(req); });
    server_.on(AsyncURIMatcher::exact("/api/clip"), HTTP_DELETE, [this](AsyncWebServerRequest *req) {
      uint32_t n = 0;
      if (!admin(req) || !clipParam(req, n)) return;
      queue(req, {WebCommand::Type::Delete, n});
    });
    server_.on(AsyncURIMatcher::exact("/api/arm"), HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (!admin(req)) return;
      if (!req->hasParam("on")) return error(req, 400, "on: 1 or 0, e.g. /api/arm?on=1");
      queue(req, {WebCommand::Type::Arm, req->getParam("on")->value() == "1" ? 1u : 0u});
    });
    server_.on(AsyncURIMatcher::exact("/api/rec"), HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (admin(req)) queue(req, {WebCommand::Type::Rec, 0});
    });
    server_.on(AsyncURIMatcher::exact("/api/stop"), HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (admin(req)) queue(req, {WebCommand::Type::Stop, 0});
    });
    server_.on(AsyncURIMatcher::exact("/api/play"), HTTP_POST, [this](AsyncWebServerRequest *req) {
      uint32_t n = 0;
      if (!admin(req) || !clipParam(req, n)) return;
      float speed = 1.0f;
      if (req->hasParam("speed")) {
        speed = req->getParam("speed")->value().toFloat();
        if (speed < 0.5f || speed > 2.0f) return error(req, 400, "speed: 0.5 to 2.0");
      }
      queue(req, {WebCommand::Type::Play, n, speed});
    });
    server_.onNotFound([](AsyncWebServerRequest *req) { error(req, 404, "no such endpoint, see /api"); });
    server_.begin();
    started_ = true;
    log_i("Web server on port 80: the page at /, the API at /api, live updates at /ws");
  }

  bool started() const { return started_; }
  size_t clients() const { return started_ ? ws_.count() : 0; }

  // ---- From loop() ------------------------------------------------------------------------------

  bool take(WebCommand &c) { return queue_ && xQueueReceive(queue_, &c, 0) == pdTRUE; }

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
    const uint32_t v = store_.version();
    if (v != clipsVersion_) {
      clipsVersion_ = v;
      if (ws_.count()) ws_.textAll(wrap("clips", clipsJson()));
    }
  }

 private:
  // A lease that gives the clip back when the response that holds it is destroyed, however the
  // download ends (finished, cancelled, connection lost).
  struct Lease {
    rec::Clip *clip;
    ~Lease() { rec::ClipStore::release(clip); }
  };

  void download(AsyncWebServerRequest *req) {
    uint32_t n = 0;
    if (!admin(req) || !clipParam(req, n)) return;
    rec::Clip *c = store_.lease(n);
    if (!c) return error(req, 404, "no finished clip with this number (see /api/clips)");
    auto lease = std::make_shared<Lease>(Lease{c});
    const bool raw = req->hasParam("raw") && req->getParam("raw")->value() == "1";
    const float gain = raw ? 1.0f : snd::gainFor(c->peak);
    // The WAV is never built in memory: the network asks for the next few hundred bytes, we make them.
    AsyncWebServerResponse *res = req->beginResponse("audio/wav", c->wavBytes(), [lease, gain](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
      return rec::wavRead(*lease->clip, gain, buf, maxLen, index);
    });
    char name[48], disposition[80];
    rec::clipName(*c, name, sizeof(name));
    snprintf(disposition, sizeof(disposition), "inline; filename=\"%s\"", name);
    res->addHeader("Content-Disposition", disposition);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  }

  String clipsJson() {
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    store_.each([&](const rec::Clip &c) {
      JsonObject o = a.add<JsonObject>();
      char name[48];
      rec::clipName(c, name, sizeof(name));
      o["n"] = c.number;
      o["name"] = static_cast<const char *>(name);  // copied: name is a local buffer
      o["ms"] = c.durationMs();
      o["rate"] = c.rate;
      o["bytes"] = c.wavBytes();
      o["peakDb"] = roundf(snd::toDb(float(c.peak)) * 10) / 10;
      o["gain"] = roundf(snd::gainFor(c.peak) * 10) / 10;
      o["manual"] = c.manual;
      o["open"] = c.open;
      o["wall"] = c.wall;  // Unix seconds, 0 = unknown
    });
    String out;
    serializeJson(d, out);
    return out;
  }

  void queue(AsyncWebServerRequest *req, const WebCommand &c) {
    if (!queue_ || xQueueSend(queue_, &c, 0) != pdTRUE) return error(req, 503, "busy, try again");
    req->send(202, "application/json", "{\"ok\":true}");
  }

  bool admin(AsyncWebServerRequest *req) {
    if (req->hasHeader("Authorization") && token_.allows(req->header("Authorization"))) return true;
    error(req, 401, "send Authorization: Bearer <token> (serial command: token)");
    return false;
  }

  static bool clipParam(AsyncWebServerRequest *req, uint32_t &n) {
    if (req->hasParam("n")) {
      const long v = req->getParam("n")->value().toInt();
      if (v > 0) return n = uint32_t(v), true;
    }
    error(req, 400, "n: the clip number, e.g. ?n=3");
    return false;
  }

  static void error(AsyncWebServerRequest *req, int code, const char *message) {
    JsonDocument d;
    d["error"] = message;
    String out;
    serializeJson(d, out);
    req->send(code, "application/json", out);
  }

  static String wrap(const char *type, const String &data) { return String("{\"type\":\"") + type + "\",\"data\":" + data + "}"; }

  static constexpr const char *kIndex =
      "{\"endpoints\":[\"GET / (the page)\",\"GET /api/status\",\"GET /api/clips\",\"GET /api/clip?n=N[&raw=1]\",\"DELETE /api/clip?n=N\","
      "\"POST /api/arm?on=1|0\",\"POST /api/rec\",\"POST /api/stop\",\"POST /api/play?n=N[&speed=1.0]\",\"WS /ws?token=...\"],"
      "\"auth\":\"everything but this list: Authorization: Bearer <token> (serial command: token)\"}";

  rec::ClipStore &store_;
  const ApiToken &token_;
  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  QueueHandle_t queue_ = nullptr;
  std::mutex mutex_;
  String status_;
  bool started_ = false;
  uint32_t lastCleanup_ = 0, clipsVersion_ = 0xFFFFFFFF;
};
