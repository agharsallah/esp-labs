// Wi-Fi scanning in the background.
//
// In station mode (STA) the ESP32 behaves like a phone or laptop. To scan, the radio hops
// through the 2.4 GHz channels. On each one it sends a "probe request" (who's there?)
// and listens for answers and for the "beacons" every router broadcasts about 10 times
// a second. Each answer carries the network name (SSID), the router's hardware address
// (BSSID), its channel and its security. We also measure how strong it arrived (RSSI, in dBm).
//
// scanNetworks(async = true) returns immediately, so the screen keeps animating while
// the Wi-Fi driver does the hopping on the other CPU core.
#pragma once

#include <WiFi.h>

#include <algorithm>
#include <vector>

struct ScanResult {
  String ssid;  // empty for hidden networks
  String bssid;
  int32_t rssi;  // dBm: -30 is very strong, -90 barely there
  uint8_t channel;
  wifi_auth_mode_t auth;
};

class Scanner {
 public:
  // Station mode, like a phone. Scanning also works while joined to a network:
  // the radio briefly visits the other channels, then comes back.
  void begin() { WiFi.mode(WIFI_STA); }

  // Start a scan unless one is already running. Hidden networks are included.
  bool start() {
    if (scanning_) return false;
    const int16_t r = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
    if (r == WIFI_SCAN_FAILED) {
      log_e("Could not start a scan");
      return false;
    }
    scanning_ = true;
    startedAt_ = millis();
    return true;
  }

  // Call every loop. Returns true once, when a new set of results is ready.
  bool poll() {
    if (!scanning_) return false;
    const int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return false;

    scanning_ = false;
    lastScanMs_ = millis() - startedAt_;
    networks_.clear();
    if (n < 0) {
      failed_ = true;
      log_e("Scan failed (code %d)", n);
      return true;
    }
    failed_ = false;
    for (int i = 0; i < n; i++) {
      networks_.push_back({WiFi.SSID(i), WiFi.BSSIDstr(i), WiFi.RSSI(i), uint8_t(WiFi.channel(i)), WiFi.encryptionType(i)});
    }
    WiFi.scanDelete();  // free the driver's copy; we keep ours
    std::sort(networks_.begin(), networks_.end(), [](const ScanResult &a, const ScanResult &b) { return a.rssi > b.rssi; });
    scans_++;
    finishedAt_ = millis();
    return true;
  }

  bool scanning() const { return scanning_; }
  bool failed() const { return failed_; }
  const std::vector<ScanResult> &networks() const { return networks_; }
  uint32_t lastScanMs() const { return lastScanMs_; }
  uint32_t scans() const { return scans_; }
  uint32_t finishedAt() const { return finishedAt_; }
  uint32_t elapsed() const { return millis() - startedAt_; }

 private:
  std::vector<ScanResult> networks_;
  bool scanning_ = false;
  bool failed_ = false;
  uint32_t startedAt_ = 0, finishedAt_ = 0, lastScanMs_ = 0, scans_ = 0;
};

// ---- Plain-language helpers ------------------------------------------------------------

inline const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_OWE: return "OWE";
    default: return "other";
  }
}

// 1-4 bars, the same rough scale phones use.
inline uint8_t signalBars(int32_t rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  return 1;
}
