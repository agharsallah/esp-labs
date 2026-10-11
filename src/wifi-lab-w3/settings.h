// W3's saved settings, in NVS (they survive power-off and reflashing):
//   namespace "w3": the place (name, lat, lon)       serial: city <name>
//   namespace "ai": the LiteLLM gateway: url, key, model   serial: ai url · ai key · ai model
// The key is never printed: only its length and last 4 characters.
#pragma once

#include <Preferences.h>

#include "oracle.h"
#include "weather.h"

namespace settings {

inline Place loadPlace() {
  Preferences p;
  p.begin("w3", true);
  Place place;
  if (p.isKey("name")) {
    place.name = p.getString("name");
    place.lat = p.getFloat("lat");
    place.lon = p.getFloat("lon");
  }
  p.end();
  return place;
}

inline void savePlace(const Place &place) {
  Preferences p;
  p.begin("w3", false);
  p.putString("name", place.name);
  p.putFloat("lat", place.lat);
  p.putFloat("lon", place.lon);
  p.end();
}

inline String loadString(const char *key, const char *fallback = "") {
  Preferences p;
  p.begin("ai", true);
  String v = p.isKey(key) ? p.getString(key) : String(fallback);
  p.end();
  return v;
}

inline void saveString(const char *key, const String &value) {
  Preferences p;
  p.begin("ai", false);
  if (value.isEmpty()) p.remove(key);
  else p.putString(key, value);
  p.end();
}

inline String aiUrl() { return loadString("url"); }
inline String apiKey() { return loadString("key"); }
inline String model() { return loadString("model", oracle::kDefaultModel); }

// "...wxyz (41 characters)", never the whole key.
inline String masked(const String &key) {
  if (key.isEmpty()) return "not set";
  return "..." + key.substring(key.length() > 4 ? key.length() - 4 : 0) + " (" + String(key.length()) + " characters)";
}

}  // namespace settings
