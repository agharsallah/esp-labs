// What each API request means, independent of how it arrived. The local web server (web_api.h)
// and the remote link (remote/relay.cpp) both call handle(), so a request is checked the same way
// whether it comes from your phone on the Wi-Fi or from the dashboard on the other side of a tunnel.
//
// Like every web-side code, it never changes anything itself: it validates, then queues a Command
// for loop() (see commands.h). Safe to call from any task.
#pragma once

#include <ArduinoJson.h>

#include "commands.h"
#include "connection.h"

struct ApiResult {
  int code;           // HTTP-style status: 200, 202, 400, 401, 404, 503
  const char *error;  // nullptr when it worked
  bool ok() const { return error == nullptr; }
};

class ApiRules {
 public:
  ApiRules(CommandQueue &queue, const Connection &connection, const char *const *viewNames, int viewCount)
      : queue_(queue), connection_(connection), viewNames_(viewNames), viewCount_(viewCount) {}

  // method + path as in the REST API (e.g. "PUT", "/api/led"); body is the parsed JSON object
  // (empty for requests without one). admin = the caller proved it has the token.
  ApiResult handle(const char *method, const char *path, JsonObjectConst body, bool admin) const {
    const String m = method, p = path;
    if (m == "POST" && p == "/api/scan") return push(Command{Command::Type::Scan}, 202);
    if (m == "PUT" && p == "/api/view") return view(body);
    if (m == "PUT" && p == "/api/settings") return settings(body);
    if (m == "PUT" && p == "/api/led") return led(body);
    if (m == "PUT" && p == "/api/message") return message(body);
    if (m == "PUT" && p == "/api/wifi") return saveWifi(body, admin);
    if (m == "DELETE" && p == "/api/wifi") return admin ? push(Command{Command::Type::ForgetWifi}) : kNeedsAdmin;
    if (m == "POST" && p == "/api/reboot") return admin ? push(Command{Command::Type::Reboot}, 202) : kNeedsAdmin;
    return {404, "no such endpoint, see /api"};
  }

  static constexpr ApiResult kNeedsAdmin{401, "admin action: send Authorization: Bearer <token> (serial command: token)"};

 private:
  ApiResult push(const Command &c, int code = 200) const {
    return queue_.push(c) ? ApiResult{code, nullptr} : ApiResult{503, "busy, try again"};
  }

  ApiResult view(JsonObjectConst body) const {
    const int v = viewIndex(body["view"]);
    if (v < 0) return {400, "view must be one of the names in /api/status screen.views"};
    Command c{Command::Type::SetView};
    c.value = v;
    return push(c);
  }

  ApiResult settings(JsonObjectConst body) const {
    const bool hasAuto = body["autoScan"].is<bool>(), hasLight = !body["backlight"].isNull();
    if (!hasAuto && !hasLight) return {400, "send autoScan (true/false) and/or backlight (0-255)"};
    const int level = body["backlight"] | -1;
    if (hasLight && (level < 0 || level > 255)) return {400, "backlight must be 0-255"};
    if (hasAuto) {
      Command c{Command::Type::SetAutoScan};
      c.value = body["autoScan"].as<bool>();
      if (const ApiResult r = push(c); !r.ok()) return r;
    }
    if (hasLight) {
      Command c{Command::Type::SetBacklight};
      c.value = level;
      return push(c);
    }
    return {200, nullptr};
  }

  ApiResult led(JsonObjectConst body) const {
    const int r = body["r"] | -1, g = body["g"] | -1, b = body["b"] | -1;
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return {400, "r, g and b must be 0-255"};
    Command c{Command::Type::SetLed};
    c.r = r, c.g = g, c.b = b;
    return push(c);
  }

  ApiResult message(JsonObjectConst body) const {
    const char *text = body["text"] | "";
    if (strlen(text) > 96) return {400, "text is limited to 96 characters"};
    Command c{Command::Type::ShowMessage};
    strlcpy(c.text, text, sizeof(c.text));
    return push(c);
  }

  ApiResult saveWifi(JsonObjectConst body, bool admin) const {
    // During set-up nobody has the token yet (it's only on serial), so saving a network is open
    // until the board is joined. After that, changing networks is an admin action.
    if (connection_.state() == Connection::State::Connected && !admin) return kNeedsAdmin;
    const char *ssid = body["ssid"] | "";
    const char *pass = body["password"] | "";
    if (strlen(ssid) == 0 || strlen(ssid) > 32) return {400, "ssid must be 1-32 characters"};
    if (strlen(pass) > 63 || (strlen(pass) > 0 && strlen(pass) < 8)) return {400, "password must be empty (open network) or 8-63 characters"};
    Command c{Command::Type::SaveWifi};
    strlcpy(c.text, ssid, sizeof(c.text));
    strlcpy(c.secret, pass, sizeof(c.secret));
    return push(c, 202);
  }

  int viewIndex(JsonVariantConst v) const {
    if (v.is<int>()) {
      const int i = v.as<int>();
      return i >= 0 && i < viewCount_ ? i : -1;
    }
    const char *name = v | "";
    for (int i = 0; i < viewCount_; i++)
      if (strcmp(name, viewNames_[i]) == 0) return i;
    return -1;
  }

  CommandQueue &queue_;
  const Connection &connection_;
  const char *const *viewNames_;
  int viewCount_;
};
