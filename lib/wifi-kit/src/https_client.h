// One HTTPS request, the way a browser does it: look up the name, connect, check the server's
// certificate against the Mozilla root certificates built into the firmware, send, read the answer.
// Plain http:// works too (for a server on your own network, like the LiteLLM gateway on your Mac),
// but then nothing is encrypted or checked.
// Blocks until done (0.3 s to several seconds), so call it from a worker task, never from loop().
//
//   HttpsResult r = https::get("https://api.open-meteo.com/v1/forecast?...");
//   if (r.ok()) parse(r.body);
#pragma once

#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include <initializer_list>
#include <utility>

// The CA bundle ESP-IDF links into every firmware: the Mozilla root certificates, about 67 KB.
extern const uint8_t kHttpsCaBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kHttpsCaBundleEnd[] asm("_binary_x509_crt_bundle_end");

struct HttpsResult {
  int status = 0;     // HTTP status, or a negative HTTPClient error (no network, TLS failed...)
  uint32_t ms = 0;    // whole request, including DNS, TCP and the TLS handshake
  size_t bytes = 0;   // size of the answer
  String body;
  String error;       // human-readable, empty on success
  bool ok() const { return status >= 200 && status < 300; }
};

namespace https {

using Header = std::pair<const char *, String>;

inline HttpsResult request(const char *method, const String &url, const String &body = "",
                           std::initializer_list<Header> headers = {}, uint16_t timeoutMs = 15'000) {
  HttpsResult r;
  const uint32_t start = millis();
  const bool secure = !url.startsWith("http://");
  NetworkClientSecure tls;
  NetworkClient plain;
  if (secure) tls.setCACertBundle(kHttpsCaBundleStart, kHttpsCaBundleEnd - kHttpsCaBundleStart);
  HTTPClient http;
  http.setConnectTimeout(8'000);
  http.setTimeout(timeoutMs);
  http.setReuse(false);  // one request per connection keeps memory use predictable
  if (!http.begin(secure ? static_cast<NetworkClient &>(tls) : plain, url)) {
    r.status = -1;
    r.error = "bad URL";
    return r;
  }
  for (const Header &h : headers) http.addHeader(h.first, h.second);
  r.status = http.sendRequest(method, body);
  if (r.status > 0) {
    r.body = http.getString();  // handles chunked answers too
    r.bytes = r.body.length();
    if (!r.ok()) r.error = "HTTP " + String(r.status);
  } else {
    r.error = HTTPClient::errorToString(r.status);  // e.g. "connection refused", "read Timeout"
  }
  http.end();
  r.ms = millis() - start;
  return r;
}

inline HttpsResult get(const String &url, std::initializer_list<Header> headers = {}) {
  return request("GET", url, "", headers);
}

inline HttpsResult post(const String &url, const String &body, std::initializer_list<Header> headers = {},
                        uint16_t timeoutMs = 15'000) {
  return request("POST", url, body, headers, timeoutMs);
}

// Make text safe inside a URL query: "São Paulo" -> "S%C3%A3o%20Paulo".
inline String urlEncode(const String &s) {
  String out;
  for (const uint8_t c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

}  // namespace https
