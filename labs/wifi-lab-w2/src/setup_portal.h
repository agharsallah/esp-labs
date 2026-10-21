// The set-up network: when no Wi-Fi is saved (or joining keeps failing), the ESP32 also
// becomes an access point called "bench-setup". Join it from a phone, and every web address
// you open lands on the set-up page (a "captive portal"): a tiny DNS server answers every
// name with the ESP32's own address, 192.168.4.1.
//
// The access point's password is derived from the chip's MAC address and shown on the
// screen, so only someone who can see the board can use it.
#pragma once

#include <DNSServer.h>
#include <WiFi.h>

class SetupPortal {
 public:
  static constexpr const char *kSsid = "bench-setup";

  void begin() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char pass[16];
    snprintf(pass, sizeof(pass), "bench-%02x%02x%02x", mac[3], mac[4], mac[5]);
    password_ = pass;
  }

  void start() {
    if (active_) return;
    WiFi.mode(WIFI_AP_STA);  // access point and station at the same time (one radio, shared channel)
    WiFi.softAP(kSsid, password_.c_str());
    dns_.start(53, "*", WiFi.softAPIP());
    active_ = true;
    startedAt_ = millis();
    log_i("Set-up network '%s' is up (password on the screen). Open http://%s", kSsid,
          WiFi.softAPIP().toString().c_str());
  }

  void stop() {
    if (!active_) return;
    dns_.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    active_ = false;
    log_i("Set-up network closed");
  }

  void loop() {
    if (active_) dns_.processNextRequest();
  }

  bool active() const { return active_; }
  uint32_t activeFor() const { return active_ ? millis() - startedAt_ : 0; }
  const String &password() const { return password_; }
  String address() const { return WiFi.softAPIP().toString(); }

 private:
  DNSServer dns_;
  String password_;
  bool active_ = false;
  uint32_t startedAt_ = 0;
};
