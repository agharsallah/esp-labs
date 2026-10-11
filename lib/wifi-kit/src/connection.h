// Joining a Wi-Fi network, remembering it, and staying connected.
//
// Where the password lives: in NVS ("non-volatile storage"), a small key-value area in the
// chip's flash, via the Preferences library. It survives power-off and re-flashing, and it is
// never in the source code, the build, git or the serial log. `forget` erases it.
// (NVS is not encrypted by default. Real products turn on flash encryption; fine for a bench.)
//
// Wi-Fi events: the driver tells us when we join, get an IP address or drop off. Those
// callbacks run in the Wi-Fi task, so they only set flags; loop() does the real work.
//
// Once we have an IP address:
//   - the clock syncs from internet time servers (NTP)
//   - mDNS answers to "bench.local", so other devices find us without knowing the IP
#pragma once

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include <algorithm>

class Connection {
 public:
  enum class State { NoCredentials, Connecting, Connected, Failed };
  static constexpr const char *kHostname = "bench";
  // Central European time with summer time. Change with the serial command: tz <POSIX TZ string>
  static constexpr const char *kDefaultTz = "CET-1CEST,M3.5.0,M10.5.0/3";
  static constexpr int kHistory = 60;  // seconds of signal history for the chart

  void begin() {
    WiFi.persistent(false);  // don't let the driver keep its own copy of the password
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(kHostname);
    WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) { onEvent(event, info); });

    prefs_.begin("wifi", /*readOnly=*/true);
    ssid_ = prefs_.getString("ssid", "");
    pass_ = prefs_.getString("pass", "");
    tz_ = prefs_.isKey("tz") ? prefs_.getString("tz") : String(kDefaultTz);  // isKey: no error log when unset
    prefs_.end();

    if (ssid_.isEmpty()) {
      log_i("No saved network. Type 'join <number>' after a scan.");
      state_ = State::NoCredentials;
    } else {
      log_i("Saved network found: '%s'", ssid_.c_str());
      connect();
    }
  }

  // Save a network to NVS and join it. pass is empty for open networks.
  void save(const String &ssid, const String &pass) {
    prefs_.begin("wifi", false);
    prefs_.putString("ssid", ssid);
    prefs_.putString("pass", pass);
    prefs_.end();
    ssid_ = ssid;
    pass_ = pass;
    log_i("Saved '%s' (password: %u characters, not shown)", ssid.c_str(), unsigned(pass.length()));
    connect();
  }

  void forget() {
    prefs_.begin("wifi", false);
    prefs_.remove("ssid");
    prefs_.remove("pass");
    prefs_.end();
    WiFi.disconnect();
    ssid_ = pass_ = "";
    state_ = State::NoCredentials;
    log_i("Saved network erased from NVS");
  }

  void setTimezone(const String &tz) {
    prefs_.begin("wifi", false);
    prefs_.putString("tz", tz);
    prefs_.end();
    tz_ = tz;
    if (state_ == State::Connected) configTzTime(tz_.c_str(), "pool.ntp.org", "time.cloudflare.com");
    log_i("Time zone set to '%s'", tz_.c_str());
  }

  void connect() {
    if (ssid_.isEmpty()) return;
    WiFi.disconnect();
    WiFi.begin(ssid_.c_str(), pass_.isEmpty() ? nullptr : pass_.c_str());
    state_ = State::Connecting;
    startedAt_ = millis();
    log_i("Joining '%s'...", ssid_.c_str());
  }

  // Call every loop: handles flags from the Wi-Fi task, time-outs and the signal history.
  void loop() {
    const uint32_t now = millis();
    if (gotIp_) {
      gotIp_ = false;
      state_ = State::Connected;
      connectedAt_ = now;
      log_i("Connected to '%s': IP %s, channel %d, %ld dBm", ssid_.c_str(), WiFi.localIP().toString().c_str(),
            static_cast<int>(WiFi.channel()), static_cast<long>(WiFi.RSSI()));
      // Power save lets the radio doze between router beacons and miss requests (ARP, ping, HTTP).
      // Fine for a sensor that only sends; a web server has to stay awake to be reachable.
      WiFi.setSleep(false);
      configTzTime(tz_.c_str(), "pool.ntp.org", "time.cloudflare.com");
      // mDNS follows reconnects by itself, so start it (and add the service) only once.
      if (!mdnsUp_ && MDNS.begin(kHostname)) {
        MDNS.addService("http", "tcp", 80);  // lets browsers and apps discover the web page
        mdnsUp_ = true;
      }
      if (mdnsUp_) log_i("Reachable as %s.local (try: ping %s.local, or open http://%s.local)", kHostname, kHostname, kHostname);
      else log_w("mDNS did not start");
      justConnected_ = true;
    }
    if (dropped_) {
      dropped_ = false;
      log_w("Disconnected: reason %u, %s", lastReason_, reasonHint(lastReason_));
      if (state_ == State::Connected) state_ = State::Connecting, startedAt_ = now;
    }
    if (state_ == State::Connecting && now - startedAt_ > kJoinTimeoutMs) {
      state_ = State::Failed;
      failedAt_ = now;
      log_w("Could not join '%s' within %lu s (last reason: %s). Retrying in %lu s.", ssid_.c_str(),
            static_cast<unsigned long>(kJoinTimeoutMs / 1000), reasonHint(lastReason_),
            static_cast<unsigned long>(kRetryMs / 1000));
    }
    if (state_ == State::Failed && now - failedAt_ > kRetryMs) connect();
    if (state_ == State::Connected && now - lastSample_ >= 1000) {
      lastSample_ = now;
      rssi_[head_] = int8_t(WiFi.RSSI());
      head_ = (head_ + 1) % kHistory;
      samples_ = std::min(samples_ + 1, kHistory);
    }
  }

  // True once after each successful connection (main.cpp switches to the status view).
  bool takeJustConnected() {
    const bool j = justConnected_;
    justConnected_ = false;
    return j;
  }

  State state() const { return state_; }
  const String &ssid() const { return ssid_; }
  const String &timezone() const { return tz_; }
  uint8_t lastReason() const { return lastReason_; }
  uint32_t connectingFor() const { return millis() - startedAt_; }
  uint32_t connectedFor() const { return millis() - connectedAt_; }
  bool timeSynced() const { return time(nullptr) > 1'700'000'000; }  // any date after Nov 2023
  int samples() const { return samples_; }
  int8_t rssiAt(int i) const { return rssi_[(head_ - samples_ + i + kHistory) % kHistory]; }  // 0 = oldest

  // The common disconnect reasons, in plain words.
  static const char *reasonHint(uint8_t r) {
    switch (r) {
      case 0: return "none yet";
      case 2: case 15: case 202: case 204: return "wrong password?";
      case 201: return "network not found (out of range, 5 GHz only, or name typo)";
      case 203: return "the router refused us (MAC filter, or full)";
      case 8: return "we left on purpose";
      case 200: return "lost the router's beacons (weak signal)";
      default: return WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(r));
    }
  }

 private:
  static constexpr uint32_t kJoinTimeoutMs = 20'000;
  static constexpr uint32_t kRetryMs = 30'000;

  // Runs in the Wi-Fi task: only record what happened.
  void onEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        gotIp_ = true;
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        lastReason_ = info.wifi_sta_disconnected.reason;
        dropped_ = true;
        break;
      default:
        break;
    }
  }

  Preferences prefs_;
  String ssid_, pass_, tz_;
  State state_ = State::NoCredentials;
  volatile bool gotIp_ = false, dropped_ = false;
  volatile uint8_t lastReason_ = 0;
  bool justConnected_ = false, mdnsUp_ = false;
  uint32_t startedAt_ = 0, connectedAt_ = 0, failedAt_ = 0, lastSample_ = 0;
  int8_t rssi_[kHistory] = {};
  int head_ = 0, samples_ = 0;
};
