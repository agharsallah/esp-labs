// The ESP32's web side: a small REST API, a WebSocket for live updates, the built-in phone page
// and firmware updates over HTTP. Runs on ESPAsyncWebServer, which answers requests in its own
// network task, so pages stay fast even while the screen is busy drawing.
//
// Endpoints (all JSON unless noted)
//   GET    /                 built-in phone page (HTML)
//   GET    /api              this list
//   GET    /api/status       device, Wi-Fi, screen and LED state
//   GET    /api/networks     last scan results
//   POST   /api/scan         start a scan                          -> 202
//   PUT    /api/view         {"view": "radar"}                     screen view
//   PUT    /api/settings     {"autoScan": true, "backlight": 180}  either or both
//   PUT    /api/led          {"r": 0, "g": 120, "b": 255}          on-board RGB LED
//   PUT    /api/message      {"text": "hello"}                     show text on the screen
//   PUT    /api/wifi         {"ssid": "...", "password": "..."}    open during set-up, admin after
//   DELETE /api/wifi         forget the saved network               admin
//   POST   /api/reboot       restart                                admin
//   POST   /api/ota          multipart firmware.bin                 admin
//   WS     /ws               pushes {"type": "status" | "networks" | "event" | "ota", "data": {...}}
//
// Admin = header "Authorization: Bearer <token>" (see api_token.h). Everything else is open on
// your local network: this is a bench toy, and nothing open can harm it.
#pragma once

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

#include <mutex>

#include "api_rules.h"
#include "api_token.h"
#include "commands.h"
#include "web_page.h"

class WebApi {
 public:
  WebApi(const ApiRules &rules, CommandQueue &queue, const ApiToken &token) : rules_(rules), queue_(queue), token_(token) {}

  void begin() {
    ws_.onEvent([this](AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *, uint8_t *, size_t) {
      if (type == WS_EVT_CONNECT) {
        // Send the latest picture straight away, so a new client doesn't wait a second.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.isEmpty()) client->text(wrap("status", status_));
        if (!networks_.isEmpty()) client->text(wrap("networks", networks_));
      }
    });
    server_.addHandler(&ws_);

    // A plain "/api" would also match every "/api/..." URL registered after it, so the two index
    // routes match exactly.
    server_.on(AsyncURIMatcher::exact("/"), HTTP_GET, [](AsyncWebServerRequest *req) {
      req->send(req->beginResponse(200, "text/html", reinterpret_cast<const uint8_t *>(kIndexHtml), strlen(kIndexHtml)));
    });
    server_.on(AsyncURIMatcher::exact("/api"), HTTP_GET, [](AsyncWebServerRequest *req) { req->send(200, "application/json", kApiIndex); });
    server_.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *req) { sendCached(req, status_); });
    server_.on("/api/networks", HTTP_GET, [this](AsyncWebServerRequest *req) { sendCached(req, networks_); });
    // The write endpoints: api_rules.h decides what each one means, here we only do HTTP.
    server_.on("/api/scan", HTTP_POST, [this](AsyncWebServerRequest *req) { route(req, "POST", JsonObjectConst()); });
    for (const char *path : {"/api/view", "/api/settings", "/api/led", "/api/message", "/api/wifi"}) {
      auto *h = new AsyncCallbackJsonWebHandler(path, [this](AsyncWebServerRequest *req, JsonVariant &body) {
        if (!body.is<JsonObject>()) return error(req, 400, "send a JSON object");
        route(req, "PUT", body.as<JsonObjectConst>());
      });
      h->setMethod(HTTP_PUT);
      h->setMaxContentLength(512);
      server_.addHandler(h);
    }
    server_.on("/api/wifi", HTTP_DELETE, [this](AsyncWebServerRequest *req) { route(req, "DELETE", JsonObjectConst()); });
    server_.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest *req) { route(req, "POST", JsonObjectConst()); });
    server_.on(
        "/api/ota", HTTP_POST, [this](AsyncWebServerRequest *req) { finishOta(req); },
        [this](AsyncWebServerRequest *req, const String &, size_t index, uint8_t *data, size_t len, bool final) {
          receiveOta(req, index, data, len, final);
        });

    // In set-up mode every unknown address leads to the set-up page (that's the captive portal).
    server_.onNotFound([](AsyncWebServerRequest *req) {
      if (WiFi.getMode() & WIFI_AP && req->host() != WiFi.softAPIP().toString()) {
        req->redirect("http://" + WiFi.softAPIP().toString() + "/");
        return;
      }
      req->send(404, "application/json", "{\"error\":\"no such endpoint, see /api\"}");
    });
    server_.begin();
    log_i("Web server on port 80: phone page at /, API at /api, live updates at /ws");
  }

  // ---- Called from loop(): cache the latest JSON and push it to every WebSocket client ----

  void publishStatus(const String &json) { publish("status", json, status_); }
  void publishNetworks(const String &json) { publish("networks", json, networks_); }
  void publishEvent(const String &json) { ws_.textAll(wrap("event", json)); }

  void loop() {
    const uint32_t now = millis();
    if (now - lastCleanup_ > 2000) {
      lastCleanup_ = now;
      ws_.cleanupClients();
    }
    if (otaPercent_ >= 0 && otaPercent_ != lastOtaSent_) {
      lastOtaSent_ = otaPercent_;
      ws_.textAll(wrap("ota", "{\"percent\":" + String(otaPercent_) + "}"));
    }
  }

  int otaPercent() const { return otaPercent_; }
  size_t clients() const { return ws_.count(); }

 private:
  bool isAdmin(AsyncWebServerRequest *req) const {
    return req->hasHeader("Authorization") && token_.allows(req->header("Authorization"));
  }

  void route(AsyncWebServerRequest *req, const char *method, JsonObjectConst body) {
    const ApiResult r = rules_.handle(method, req->url().c_str(), body, isAdmin(req));
    if (!r.ok()) return error(req, r.code, r.error);
    req->send(r.code, "application/json", "{\"ok\":true}");
  }

  void enqueue(AsyncWebServerRequest *req, const Command &c, int code = 200) {
    if (!queue_.push(c)) return error(req, 503, "busy, try again");
    req->send(code, "application/json", "{\"ok\":true}");
  }

  static void error(AsyncWebServerRequest *req, int code, const char *message) {
    JsonDocument doc;
    doc["error"] = message;
    String out;
    serializeJson(doc, out);
    req->send(code, "application/json", out);
  }

  void sendCached(AsyncWebServerRequest *req, const String &cached) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cached.isEmpty()) req->send(503, "application/json", "{\"error\":\"starting up\"}");
    else req->send(200, "application/json", cached);
  }

  void publish(const char *type, const String &json, String &cache) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cache = json;
    }
    if (ws_.count() > 0) ws_.textAll(wrap(type, json));
  }

  static String wrap(const char *type, const String &data) {
    return String("{\"type\":\"") + type + "\",\"data\":" + data + "}";
  }

  // ---- Firmware update over HTTP ----------------------------------------------------------
  // The upload arrives in chunks. Update writes them to the *other* app partition, checks the
  // image, and marks it to boot next time. The running firmware is untouched until the reboot,
  // so a failed upload changes nothing.

  void receiveOta(AsyncWebServerRequest *req, size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
      otaAuthorized_ = req->hasHeader("Authorization") && token_.allows(req->header("Authorization"));
      otaError_ = "";
      if (!otaAuthorized_) return;
      otaTotal_ = req->contentLength();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        otaError_ = Update.errorString();
        return;
      }
      otaPercent_ = 0;
      log_i("Firmware update started (%u bytes incoming)", unsigned(otaTotal_));
    }
    if (!otaAuthorized_ || !otaError_.isEmpty()) return;
    if (Update.write(data, len) != len) {
      otaError_ = Update.errorString();
      Update.abort();
      otaPercent_ = -1;
      return;
    }
    if (otaTotal_ > 0) otaPercent_ = int((index + len) * 100 / otaTotal_);
    if (final) {
      if (Update.end(true)) {
        otaPercent_ = 100;
      } else {
        otaError_ = Update.errorString();
        otaPercent_ = -1;
      }
    }
  }

  void finishOta(AsyncWebServerRequest *req) {
    if (!otaAuthorized_) return error(req, 401, "admin action: send Authorization: Bearer <token>");
    if (!otaError_.isEmpty()) {
      log_e("Firmware update failed: %s", otaError_.c_str());
      return error(req, 500, otaError_.c_str());
    }
    log_i("Firmware update written and verified. Rebooting into it.");
    enqueue(req, Command{Command::Type::Reboot}, 200);
  }

  static constexpr const char *kApiIndex =
      R"JSON({"endpoints":["GET /api/status","GET /api/networks","POST /api/scan","PUT /api/view","PUT /api/settings","PUT /api/led","PUT /api/message","PUT /api/wifi","DELETE /api/wifi (admin)","POST /api/reboot (admin)","POST /api/ota (admin)","WS /ws"]})JSON";

  const ApiRules &rules_;
  CommandQueue &queue_;  // only for the reboot after a firmware update
  const ApiToken &token_;
  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  std::mutex mutex_;
  String status_, networks_;
  uint32_t lastCleanup_ = 0;
  volatile int otaPercent_ = -1;
  int lastOtaSent_ = -1;
  bool otaAuthorized_ = false;
  size_t otaTotal_ = 0;
  String otaError_;
};
