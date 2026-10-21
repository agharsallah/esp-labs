#pragma once
// Ping from the board's side, to tell "the board can't reach anyone" from "nobody can reach the board".
//   ping            the router (gateway)
//   ping <ip>       any address, e.g. your computer
// Uses ESP-IDF's ping, which runs in its own task: results arrive in the serial log a few seconds later.
#include <Arduino.h>
#include <WiFi.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>

class Pinger {
 public:
  // Returns false if the address is invalid or a ping is already running.
  bool start(IPAddress target, uint32_t count = 4) {
    if (busy_) {
      log_w("a ping is already running");
      return false;
    }
    ip_addr_t addr = {};
    if (!ipaddr_aton(target.toString().c_str(), &addr)) {
      log_w("not an IP address: %s", target.toString().c_str());
      return false;
    }
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = addr;
    config.count = count;
    config.timeout_ms = 1000;

    esp_ping_callbacks_t callbacks = {};
    callbacks.cb_args = this;
    callbacks.on_ping_success = onReply;
    callbacks.on_ping_timeout = onTimeout;
    callbacks.on_ping_end = onEnd;

    esp_ping_handle_t session = nullptr;
    if (esp_ping_new_session(&config, &callbacks, &session) != ESP_OK) {
      log_w("could not start a ping");
      return false;
    }
    busy_ = true;
    log_i("ping %s (%lu packets)...", target.toString().c_str(), static_cast<unsigned long>(count));
    esp_ping_start(session);
    return true;
  }

 private:
  static void onReply(esp_ping_handle_t session, void *) {
    uint32_t seq = 0, ms = 0;
    esp_ping_get_profile(session, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(session, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    log_i("  reply %lu: %lu ms", static_cast<unsigned long>(seq), static_cast<unsigned long>(ms));
  }

  static void onTimeout(esp_ping_handle_t session, void *) {
    uint32_t seq = 0;
    esp_ping_get_profile(session, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    log_w("  no reply %lu", static_cast<unsigned long>(seq));
  }

  static void onEnd(esp_ping_handle_t session, void *self) {
    uint32_t sent = 0, received = 0;
    esp_ping_get_profile(session, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(session, ESP_PING_PROF_REPLY, &received, sizeof(received));
    log_i("ping done: %lu sent, %lu answered", static_cast<unsigned long>(sent), static_cast<unsigned long>(received));
    esp_ping_delete_session(session);
    static_cast<Pinger *>(self)->busy_ = false;
  }

  volatile bool busy_ = false;
};
