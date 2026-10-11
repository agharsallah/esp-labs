// Weather from Open-Meteo (https://open-meteo.com): free, no account, no key, JSON over HTTPS.
//   geocode("Hamburg")  -> Place   city name to coordinates (Open-Meteo's geocoding API)
//   fetch(place)        -> Weather now + next 24 hours + 7 days, in one request
// Both block (call them from the worker task) and only fill the result when everything parsed.
#pragma once

#include <ArduinoJson.h>

#include "https_client.h"

struct Place {
  String name;  // "Hamburg, Germany"
  float lat = 0, lon = 0;
  bool valid() const { return !name.isEmpty(); }
};

// What the screen and the oracle need; everything else in the answer is dropped while parsing.
struct Weather {
  static constexpr int kHours = 24, kDays = 7;
  float temp = 0, feels = 0, wind = 0;
  int humidity = 0, code = 0;
  bool day = true;
  float hourTemp[kHours] = {};
  int8_t hourRain[kHours] = {};  // precipitation probability, %
  int hours = 0;
  float dayMax[kDays] = {}, dayMin[kDays] = {};
  int dayCode[kDays] = {};
  int days = 0;
};

// The scenes the screen can animate (and the AI may pick from).
enum class Scene : uint8_t { Sun, Clouds, Rain, Snow, Storm, Fog, Night };

namespace weather {

// WMO weather interpretation codes, as Open-Meteo reports them.
inline const char *describe(int code) {
  switch (code) {
    case 0: return "clear sky";
    case 1: return "mainly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: return "drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66: case 67: return "freezing rain";
    case 71: case 73: case 75: case 77: return "snow";
    case 80: case 81: case 82: return "rain showers";
    case 85: case 86: return "snow showers";
    case 95: return "thunderstorm";
    case 96: case 99: return "thunderstorm, hail";
    default: return "unknown";
  }
}

inline Scene sceneFor(int code, bool day) {
  if (code >= 95) return Scene::Storm;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Scene::Snow;
  if (code >= 51) return Scene::Rain;
  if (code == 45 || code == 48) return Scene::Fog;
  if (code >= 2) return Scene::Clouds;
  return day ? Scene::Sun : Scene::Night;
}

inline const char *sceneName(Scene s) {
  static const char *const names[] = {"sun", "clouds", "rain", "snow", "storm", "fog", "night"};
  return names[static_cast<int>(s)];
}

inline bool sceneFromName(const char *name, Scene &out) {
  for (int i = 0; i <= static_cast<int>(Scene::Night); i++) {
    if (strcmp(name, sceneName(static_cast<Scene>(i))) == 0) {
      out = static_cast<Scene>(i);
      return true;
    }
  }
  return false;
}

inline HttpsResult geocode(const String &city, Place &out) {
  HttpsResult r = https::get("https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" +
                             https::urlEncode(city));
  if (!r.ok()) return r;
  JsonDocument doc;
  if (DeserializationError e = deserializeJson(doc, r.body)) {
    r.error = String("bad JSON: ") + e.c_str();
    return r;
  }
  JsonObjectConst hit = doc["results"][0];
  if (hit.isNull()) {
    r.error = "no place called '" + city + "'";
    return r;
  }
  out.name = String(hit["name"] | "?") + ", " + String(hit["country"] | "");
  out.lat = hit["latitude"] | 0.0f;
  out.lon = hit["longitude"] | 0.0f;
  return r;
}

inline HttpsResult fetch(const Place &place, Weather &out) {
  char url[400];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&timezone=auto"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
           "&hourly=temperature_2m,precipitation_probability&forecast_hours=24"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=7",
           place.lat, place.lon);
  HttpsResult r = https::get(url);
  if (!r.ok()) return r;

  // A filter tells ArduinoJson which fields to keep. The time strings (the biggest part of the
  // answer) are dropped while parsing, so they never take up memory.
  JsonDocument filter;
  for (const char *k : {"temperature_2m", "apparent_temperature", "relative_humidity_2m", "weather_code",
                        "wind_speed_10m", "is_day"})
    filter["current"][k] = true;
  filter["hourly"]["temperature_2m"] = true;
  filter["hourly"]["precipitation_probability"] = true;
  filter["daily"]["weather_code"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;

  JsonDocument doc;
  if (DeserializationError e = deserializeJson(doc, r.body, DeserializationOption::Filter(filter))) {
    r.error = String("bad JSON: ") + e.c_str();
    return r;
  }
  JsonObjectConst now = doc["current"];
  if (now.isNull()) {
    r.error = "no current weather in the answer";
    return r;
  }
  Weather w;
  w.temp = now["temperature_2m"] | 0.0f;
  w.feels = now["apparent_temperature"] | 0.0f;
  w.humidity = now["relative_humidity_2m"] | 0;
  w.code = now["weather_code"] | 0;
  w.wind = now["wind_speed_10m"] | 0.0f;
  w.day = (now["is_day"] | 1) == 1;
  JsonArrayConst temps = doc["hourly"]["temperature_2m"], rain = doc["hourly"]["precipitation_probability"];
  for (size_t i = 0; i < temps.size() && w.hours < Weather::kHours; i++, w.hours++) {
    w.hourTemp[i] = temps[i] | 0.0f;
    w.hourRain[i] = int8_t(rain[i] | 0);
  }
  JsonArrayConst maxs = doc["daily"]["temperature_2m_max"], mins = doc["daily"]["temperature_2m_min"],
                 codes = doc["daily"]["weather_code"];
  for (size_t i = 0; i < maxs.size() && w.days < Weather::kDays; i++, w.days++) {
    w.dayMax[i] = maxs[i] | 0.0f;
    w.dayMin[i] = mins[i] | 0.0f;
    w.dayCode[i] = codes[i] | 0;
  }
  out = w;
  return r;
}

}  // namespace weather
