// Remote control: the board dials OUT to your dashboard server and keeps a WebSocket open, so
// the dashboard can reach it from anywhere, through a tunnel (cloudflared, ngrok), without
// opening anything on your router.
//
// Two implementations, picked by the PlatformIO environment (like lesson 03b's controls):
//   remote/off.cpp     wifi-lab-w2          no remote link (everything returns "off")
//   remote/relay.cpp   wifi-lab-w2-remote   the WebSocket client, in its own FreeRTOS task
//
// The link speaks the same messages as the board's own /ws, plus requests and replies:
//   server -> board  {"type":"request","id":7,"method":"PUT","path":"/api/led","body":{...}}
//   board -> server  {"type":"reply","id":7,"status":200,"body":{"ok":true}}
//                    {"type":"status"|"networks"|"event","data":{...}}
// The board proves who it is with its admin token (Authorization header on the WebSocket
// upgrade); the server only accepts the board whose token is in its .env.
#pragma once

#include <Arduino.h>

#include "api_rules.h"
#include "api_token.h"

namespace remote {

enum class State : uint8_t { Off, NotConfigured, WaitingForWifi, Connecting, Online };

// false in plain W2: then the serial `relay` commands explain which build to flash.
bool builtIn();

// Reads the saved server address from NVS and starts the link task.
void begin(const ApiRules &rules, const ApiToken &token);

// Save a new server address ("" switches the link off). wss://host[/path] or ws://ip:port[/path];
// the path defaults to /device.
void setUrl(const String &url);
String url();

State state();

inline const char *stateName(State s) {
  switch (s) {
    case State::Off: return "off";
    case State::NotConfigured: return "not-configured";
    case State::WaitingForWifi: return "waiting-for-wifi";
    case State::Connecting: return "connecting";
    case State::Online: return "online";
  }
  return "?";
}

// From loop(): hand over the latest status/networks, or a button event. Thread-safe; the link
// task sends them when it can. Status is sent at most once per call, never queued up.
void publish(const char *type, const String &json);

}  // namespace remote
