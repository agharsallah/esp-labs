// The admin token: a random secret that guards the actions that could hurt the bench
// (firmware update, forgetting the Wi-Fi network, rebooting).
//
// It is created on first boot from the chip's hardware random number generator and kept
// in NVS. It is printed over serial on boot and with the `token` command, never sent by the
// API. The dashboard's Node server keeps a copy in its .env file and adds it to admin requests
// as "Authorization: Bearer <token>". `token new` makes a fresh one.
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>

class ApiToken {
 public:
  void begin() {
    Preferences prefs;
    prefs.begin("api", true);
    token_ = prefs.getString("token", "");
    prefs.end();
    if (token_.length() != 32) regenerate();
  }

  void regenerate() {
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", uint8_t(esp_random()));
    token_ = hex;
    Preferences prefs;
    prefs.begin("api", false);
    prefs.putString("token", token_);
    prefs.end();
  }

  const String &value() const { return token_; }

  // Compares in constant time, so response timing doesn't leak how many characters matched.
  bool allows(const String &authorizationHeader) const {
    const String expected = "Bearer " + token_;
    if (authorizationHeader.length() != expected.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < expected.length(); i++) diff |= uint8_t(authorizationHeader[i] ^ expected[i]);
    return diff == 0;
  }

 private:
  String token_;
};
