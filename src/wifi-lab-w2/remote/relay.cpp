// wifi-lab-w2-remote: the board dials out to the dashboard server and stays connected.
//
// Runs in its own FreeRTOS task on core 0 (loop() runs on core 1). Opening a TLS connection takes
// a second or two and blocks while it does; in its own task that never freezes the screen.
// loop() and this task only share the small mailbox below, guarded by a mutex.
//
// Serial: relay (show) · relay <url> (save + connect) · relay off
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include <deque>
#include <mutex>
#include <utility>

#include "../remote.h"

// The Mozilla root certificates that ESP-IDF builds into the firmware, so the board checks the
// tunnel's HTTPS certificate the way a browser does.
extern const uint8_t kCaBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kCaBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace remote {
namespace {

constexpr uint32_t kRetryMs = 5'000;
constexpr size_t kMaxEvents = 8;

struct Target {
  bool secure = true;
  String host;
  uint16_t port = 443;
  String path = "/device";
};

// wss://host[:port][/path] or ws://host[:port][/path]
bool parse(const String &url, Target &t) {
  String rest;
  if (url.startsWith("wss://")) t.secure = true, t.port = 443, rest = url.substring(6);
  else if (url.startsWith("ws://")) t.secure = false, t.port = 80, rest = url.substring(5);
  else return false;
  const int slash = rest.indexOf('/');
  if (slash >= 0) {
    t.path = rest.substring(slash);
    if (t.path == "/") t.path = "/device";
    rest = rest.substring(0, slash);
  }
  const int colon = rest.indexOf(':');
  if (colon >= 0) {
    const long port = rest.substring(colon + 1).toInt();
    if (port < 1 || port > 65535) return false;
    t.port = uint16_t(port);
    rest = rest.substring(0, colon);
  }
  t.host = rest;
  return !t.host.isEmpty();
}

const ApiRules *rules = nullptr;
const ApiToken *token = nullptr;
WebSocketsClient ws;
volatile State current = State::NotConfigured;

// ---- Mailbox shared with loop() (guarded by `mutex`) ----
std::mutex mutex;
String savedUrl;
bool urlChanged = false;
String pendingStatus, pendingNetworks;  // newest only
std::deque<String> pendingEvents;
String lastStatus, lastNetworks;        // sent again after every (re)connect

// ---- Task-only state ----
bool started = false;
bool complainedSinceOnline = false;
String host;

String wrap(const char *type, const String &data) { return String("{\"type\":\"") + type + "\",\"data\":" + data + "}"; }

void answer(const uint8_t *payload, size_t length) {
  JsonDocument in;
  if (deserializeJson(in, payload, length) || strcmp(in["type"] | "", "request") != 0) return;
  const char *method = in["method"] | "";
  const char *path = in["path"] | "";
  // Only the server holding our token can be on the other end (see the server's /device), so
  // everything arriving here counts as admin.
  const ApiResult r = rules->handle(method, path, in["body"].as<JsonObjectConst>(), /*admin=*/true);
  JsonDocument out;
  out["type"] = "reply";
  out["id"] = in["id"];
  out["status"] = r.code;
  if (r.ok()) out["body"]["ok"] = true;
  else out["body"]["error"] = r.error;
  String text;
  serializeJson(out, text);
  ws.sendTXT(text);
  log_i("relay: %s %s -> %d", method, path, r.code);
}

void onEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      current = State::Online;
      complainedSinceOnline = false;
      log_i("relay: online, connected to %s", host.c_str());
      String status, networks;
      {
        std::lock_guard<std::mutex> lock(mutex);
        status = lastStatus, networks = lastNetworks;
      }
      if (!networks.isEmpty()) ws.sendTXT(networks);
      if (!status.isEmpty()) ws.sendTXT(status);
      break;
    }
    case WStype_DISCONNECTED:
      if (current == State::Online) log_w("relay: lost the server, retrying every %lu s", static_cast<unsigned long>(kRetryMs / 1000));
      else if (!complainedSinceOnline)
        log_w("relay: can't reach %s yet. Is the server + tunnel running, the address current, and ESP_TOKEN in its .env = serial `token`?", host.c_str());
      complainedSinceOnline = true;
      if (current != State::WaitingForWifi) current = State::Connecting;
      break;
    case WStype_TEXT: answer(payload, length); break;
    default: break;
  }
}

void start(const String &url) {
  Target t;
  if (!parse(url, t)) {
    log_w("relay: can't read '%s'. Use wss://your-tunnel-host or ws://ip:port", url.c_str());
    current = State::NotConfigured;
    return;
  }
  host = t.host;
  ws.setExtraHeaders(("Authorization: Bearer " + token->value()).c_str());  // copied by the library
  ws.onEvent(onEvent);
  ws.setReconnectInterval(kRetryMs);
  // Ping every 15 s: tunnels close idle connections, and a dead link is noticed within ~25 s.
  ws.enableHeartbeat(15'000, 5'000, 2);
  if (t.secure) ws.beginSslWithBundle(t.host.c_str(), t.port, t.path.c_str(), kCaBundleStart, kCaBundleEnd - kCaBundleStart, "");
  else ws.begin(t.host.c_str(), t.port, t.path.c_str(), "");
  started = true;
  current = State::Connecting;
  complainedSinceOnline = false;
  log_i("relay: connecting to %s://%s:%u%s", t.secure ? "wss" : "ws", t.host.c_str(), t.port, t.path.c_str());
}

void flush() {
  String status, networks;
  std::deque<String> events;
  {
    std::lock_guard<std::mutex> lock(mutex);
    // Arduino's String has no swap(): move out, then leave an empty String behind.
    status = std::move(pendingStatus);
    pendingStatus = String();
    networks = std::move(pendingNetworks);
    pendingNetworks = String();
    events.swap(pendingEvents);
  }
  if (current != State::Online) return;  // the newest status/networks go out on reconnect instead
  if (!networks.isEmpty()) ws.sendTXT(networks);
  if (!status.isEmpty()) ws.sendTXT(status);
  for (String &e : events) ws.sendTXT(e);
}

void task(void *) {
  for (;;) {
    String url;
    bool changed;
    {
      std::lock_guard<std::mutex> lock(mutex);
      url = savedUrl;
      changed = urlChanged;
      urlChanged = false;
    }
    if (changed && started) {
      ws.disconnect();
      started = false;
    }
    if (url.isEmpty()) current = State::NotConfigured;
    else if (!WiFi.isConnected()) current = State::WaitingForWifi;
    else if (!started) start(url);
    else if (current == State::WaitingForWifi) current = State::Connecting;

    if (started) {
      ws.loop();
      flush();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

}  // namespace

bool builtIn() { return true; }

void begin(const ApiRules &r, const ApiToken &t) {
  rules = &r;
  token = &t;
  Preferences prefs;
  prefs.begin("relay", true);
  savedUrl = prefs.isKey("url") ? prefs.getString("url") : String();
  prefs.end();
  if (savedUrl.isEmpty()) log_i("relay: no server set. Serial: relay wss://<your tunnel host>");
  else log_i("relay: server %s", savedUrl.c_str());
  // 12 KB stack: a TLS handshake needs several KB of it.
  xTaskCreatePinnedToCore(task, "relay", 12 * 1024, nullptr, 1, nullptr, 0);
}

void setUrl(const String &url) {
  Target t;
  if (!url.isEmpty() && !parse(url, t)) {
    log_w("relay: can't read '%s'. Use wss://your-tunnel-host or ws://ip:port", url.c_str());
    return;
  }
  Preferences prefs;
  prefs.begin("relay", false);
  if (url.isEmpty()) prefs.remove("url");
  else prefs.putString("url", url);
  prefs.end();
  {
    std::lock_guard<std::mutex> lock(mutex);
    savedUrl = url;
    urlChanged = true;
  }
  log_i("relay: %s", url.isEmpty() ? "off" : ("server set to " + url).c_str());
}

String url() {
  std::lock_guard<std::mutex> lock(mutex);
  return savedUrl;
}

State state() { return current; }

void publish(const char *type, const String &json) {
  const String text = wrap(type, json);
  std::lock_guard<std::mutex> lock(mutex);
  if (strcmp(type, "status") == 0) pendingStatus = lastStatus = text;
  else if (strcmp(type, "networks") == 0) pendingNetworks = lastNetworks = text;
  else if (pendingEvents.size() < kMaxEvents) pendingEvents.push_back(text);
}

}  // namespace remote
