// Wi-Fi lab W3: internet & AI.  uv run pio run -d labs/wifi-lab-w3 -e wifi-lab-w3 -t upload
// The board becomes an internet client: it fetches the weather over HTTPS as JSON (Open-Meteo, no
// key needed), draws it, and asks an AI model to turn the moment into a haiku or to answer your
// questions with the weather in mind. The answer picks the LED colour too. The AI goes through a
// LiteLLM gateway (esp-labs/apps/ai-gateway), so any provider and model works.
//
// Wiring: the screen from lesson 03. The push button on GPIO 5 is optional: it does exactly what
// BOOT does. Wi-Fi: the network saved by W1 or W2 (same NVS namespace); join one there first.
//
// Views: now · forecast · week · oracle · status
// Button or BOOT: click = next view · double-click = refresh weather (oracle view: new haiku)
//                 long press = ask the oracle from any view
// Serial: city <name> | weather | oracle | ask <question> | ai | ai url <url> | ai key <key> |
//         ai key forget | ai model <name> | led off | view <1-5> | status | ping [ip] | tz <TZ> |
//         reconnect | forget | help
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "button.h"
#include "color.h"
#include "connection.h"
#include "display.h"
#include "pinger.h"
#include "pins.h"
#include "settings.h"
#include "views.h"
#include "w3_views.h"
#include "worker.h"

namespace {

constexpr uint32_t kWeatherEveryMs = 15 * 60'000;  // Open-Meteo updates its model every 15 min
constexpr uint32_t kRetryMs = 60'000;              // after a failed fetch
constexpr uint32_t kOracleGapMs = 10'000;          // at most one AI request per 10 s
const char *const kViewNames[] = {"now", "forecast", "week", "oracle", "status"};
constexpr int kViews = sizeof(kViewNames) / sizeof(kViewNames[0]);
constexpr int kOracleView = 3, kStatusView = 4;

Display display;
Connection connection;
Pinger pinger;
Worker worker;
Button external(PIN_BUTTON, "button");
Button boot(PIN_BOOT_BTN, "BOOT");

struct {
  int view = 0;
  Place place;
  bool aiReady = false;  // gateway address and key both saved
  String model;
  Worker::Outcome out;     // the latest copy of the worker's results
  uint32_t lastWeatherTry = 0, lastOracle = 0, seenAnswerAt = 0;
} state;

// ---- What the oracle gets to know about this moment ---------------------------------------

String context() {
  JsonDocument d;
  if (state.place.valid()) d["place"] = state.place.name;
  struct tm lt;
  if (getLocalTime(&lt, 0)) {
    char when[32];
    strftime(when, sizeof(when), "%A %d %B, %H:%M", &lt);
    d["local_time"] = when;
  }
  if (state.out.weatherAt) {
    const Weather &w = state.out.weather;
    JsonObject o = d["weather"].to<JsonObject>();
    o["now"] = weather::describe(w.code);
    o["temp_c"] = serialized(String(w.temp, 1));
    o["feels_like_c"] = serialized(String(w.feels, 1));
    o["wind_kmh"] = int(lroundf(w.wind));
    o["humidity_pct"] = w.humidity;
    o["daylight"] = w.day;
    if (w.hours) o["max_rain_chance_next_24h_pct"] = *std::max_element(w.hourRain, w.hourRain + w.hours);
    if (w.days) o["today_min_c"] = int(lroundf(w.dayMin[0])), o["today_max_c"] = int(lroundf(w.dayMax[0]));
  }
  JsonObject b = d["bench"].to<JsonObject>();
  b["board"] = "ESP32-S3 with a 1.9 inch screen";
  b["chip_temp_c"] = serialized(String(temperatureRead(), 1));
  b["uptime_min"] = millis() / 60'000;
  b["wifi_signal_dbm"] = WiFi.RSSI();
  String out;
  serializeJson(d, out);
  return out;
}

// ---- Actions ---------------------------------------------------------------------------

bool online() { return connection.state() == Connection::State::Connected; }

void setView(int v) {
  state.view = (v % kViews + kViews) % kViews;
  log_i("view %d: %s", state.view + 1, kViewNames[state.view]);
}

void fetchWeather(const char *why) {
  if (!state.place.valid()) {
    log_w("no city yet: type city <name>");
    return;
  }
  if (!online()) {
    log_w("not on Wi-Fi yet");
    return;
  }
  state.lastWeatherTry = millis();
  Worker::Request r;
  r.job = Worker::Job::Weather;
  r.place = state.place;
  if (worker.submit(r)) log_i("weather for %s (%s)", state.place.name.c_str(), why);
  else log_w("busy with another request, try again in a moment");
}

void findCity(const String &name) {
  if (!online()) {
    log_w("not on Wi-Fi yet");
    return;
  }
  Worker::Request r;
  r.job = Worker::Job::Geocode;
  r.text = name;
  if (worker.submit(r)) log_i("looking up '%s'...", name.c_str());
  else log_w("busy with another request, try again in a moment");
}

void askOracle(const String &question) {
  if (!state.aiReady) {
    setView(kOracleView);
    log_w("set the gateway first: ai url <url> and ai key <key>");
    return;
  }
  if (!online()) {
    log_w("not on Wi-Fi yet");
    return;
  }
  if (state.lastOracle && millis() - state.lastOracle < kOracleGapMs) {
    log_w("one question per 10 s, please");
    return;
  }
  Worker::Request r;
  r.job = question.isEmpty() ? Worker::Job::Oracle : Worker::Job::Ask;
  r.text = question;
  r.aiUrl = settings::aiUrl();
  r.apiKey = settings::apiKey();
  r.model = state.model;
  r.context = context();
  if (!worker.submit(r)) {
    log_w("busy with another request, try again in a moment");
    return;
  }
  state.lastOracle = millis();
  setView(kOracleView);
  log_i("asking %s%s%s", state.model.c_str(), question.isEmpty() ? " for a haiku" : ": ", question.c_str());
}

// ---- Input -----------------------------------------------------------------------------

void handleButton(Button &b) {
  switch (b.poll()) {
    case Button::Event::Click: setView(state.view + 1); break;
    case Button::Event::DoubleClick:
      if (state.view == kOracleView) askOracle("");
      else fetchWeather(b.name());
      break;
    case Button::Event::LongPress: askOracle(""); break;
    case Button::Event::None: break;
  }
}

void printAi() {
  const String url = settings::aiUrl();
  Serial.printf("AI gateway: %s | key %s | model %s\n", url.isEmpty() ? "not set" : url.c_str(),
                settings::masked(settings::apiKey()).c_str(), state.model.c_str());
}

void refreshAiReady() { state.aiReady = !settings::aiUrl().isEmpty() && !settings::apiKey().isEmpty(); }

void printStatus() {
  using State = Connection::State;
  switch (connection.state()) {
    case State::NoCredentials: Serial.println("No saved Wi-Fi: flash W1 or W2 once and join there (it's shared)."); break;
    case State::Connecting: Serial.printf("Joining '%s'...\n", connection.ssid().c_str()); break;
    case State::Failed: Serial.printf("Could not join '%s': %s\n", connection.ssid().c_str(), Connection::reasonHint(connection.lastReason())); break;
    case State::Connected:
      Serial.printf("Joined '%s' | IP %s | %ld dBm | time %s\n", connection.ssid().c_str(), WiFi.localIP().toString().c_str(),
                    static_cast<long>(WiFi.RSSI()), connection.timeSynced() ? "synced" : "not yet");
      break;
  }
  Serial.printf("City: %s | weather %s\n", state.place.valid() ? state.place.name.c_str() : "not set",
                w3::ago(state.out.weatherAt).c_str());
  printAi();
  if (!state.out.error.isEmpty()) Serial.printf("Last error: %s\n", state.out.error.c_str());
}

void printHelp() {
  Serial.println();
  Serial.println("Commands: city <name> | weather | oracle | ask <question> | view <1-5> | status | led off");
  Serial.println("          ai (show) | ai url http://<gateway>:4000 | ai key <key> | ai key forget");
  Serial.println("          ai model <name> (a model_name from ai-gateway/config.yaml, default " + String(oracle::kDefaultModel) + ")");
  Serial.println("          ping [ip] | tz <TZ> | reconnect | forget | help");
  Serial.println("Button (GPIO 5, optional) or BOOT: click = next view, double-click = refresh (oracle view: new haiku),");
  Serial.println("          long press = ask the oracle");
}

void handleCommand(const String &raw) {
  String line = raw;
  line.trim();
  String lower = line;
  lower.toLowerCase();
  if (lower.isEmpty()) return;
  if (lower.startsWith("city ")) return findCity(line.substring(5));
  if (lower == "weather") return fetchWeather("serial");
  if (lower == "oracle") return askOracle("");
  if (lower.startsWith("ask ")) return askOracle(line.substring(4, 164));
  if (lower == "ai") return printAi();
  if (lower.startsWith("ai url ")) {
    String url = line.substring(7);
    url.trim();
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
      log_w("the gateway address starts with http:// or https://");
      return;
    }
    settings::saveString("url", url);
    refreshAiReady();
    log_i("AI gateway: %s", url.c_str());
    return;
  }
  if (lower == "ai key forget") {
    settings::saveString("key", "");
    refreshAiReady();
    log_i("AI key erased from the chip");
    return;
  }
  if (lower.startsWith("ai key ")) {
    String key = line.substring(7);
    key.trim();
    settings::saveString("key", key);  // as typed: keys are case-sensitive
    refreshAiReady();
    log_i("AI key saved: %s", settings::masked(key).c_str());
    return;
  }
  if (lower.startsWith("ai model ")) {
    state.model = line.substring(9);
    state.model.trim();
    settings::saveString("model", state.model);
    log_i("model: %s", state.model.c_str());
    return;
  }
  if (lower == "led off") return rgbLedWrite(PIN_RGB, 0, 0, 0);
  if (lower == "status") return printStatus();
  if (lower == "forget") return connection.forget();
  if (lower == "reconnect") return connection.connect();
  if (lower.startsWith("tz ")) return connection.setTimezone(line.substring(3));
  if (lower == "ping" || lower.startsWith("ping ")) {
    IPAddress target = WiFi.gatewayIP();
    if (!online()) log_w("ping needs a network");
    else if (lower.length() > 5 && !target.fromString(line.substring(5))) log_w("not an IP address: %s", line.substring(5).c_str());
    else pinger.start(target);
    return;
  }
  if (lower.startsWith("view ")) {
    const long v = lower.substring(5).toInt();
    if (v < 1 || v > kViews) {
      log_w("view needs 1-%d", kViews);
      return;
    }
    return setView(v - 1);
  }
  if (lower == "help" || lower == "?") return printHelp();
  log_w("unknown command '%s' (type help)", line.c_str());
}

void pollSerial() {
  static String buffer;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!buffer.isEmpty()) handleCommand(buffer);
      buffer = "";
    } else if (buffer.length() < 240) {  // API keys are ~110 characters
      buffer += c;
    }
  }
}

// ---- Results from the worker ----------------------------------------------------------------

void takeResults() {
  if (!worker.update(state.out)) return;
  if (state.out.placeChanged) {
    state.place = state.out.place;
    settings::savePlace(state.place);
    log_i("city set: %s (%.3f, %.3f)", state.place.name.c_str(), state.place.lat, state.place.lon);
    setView(0);
    fetchWeather("new city");
  }
  if (state.out.answerAt && state.out.answerAt != state.seenAnswerAt) {
    state.seenAnswerAt = state.out.answerAt;
    const OracleAnswer &a = state.out.answer;
    rgbLedWrite(PIN_RGB, a.r / 4, a.g / 4, a.b / 4);  // a quarter: the LED is very bright
    log_i("oracle: \"%s\" (%u lines, %lu > %lu tokens)", a.title.c_str(), unsigned(a.lineCount),
          static_cast<unsigned long>(a.inputTokens), static_cast<unsigned long>(a.outputTokens));
    for (int i = 0; i < a.lineCount; i++) Serial.printf("    %s\n", a.lines[i].c_str());
  }
}

void drawNoWifi(FrameBuffer &fb) {
  fb.setFont(&FreeSans9pt7b);
  fb.setTextColor(pal::ink);
  fb.setCursor(8, 56);
  fb.print("No saved Wi-Fi network");
  const char *lines[] = {"W3 uses the network W1 or W2 saved.", "Flash one of them, join, then come back:",
                         "  uv run pio run -d labs/wifi-lab-w1 -e wifi-lab-w1 -t upload"};
  w3::hint(fb, 72, lines, 3);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  if (!display.begin()) {
    log_e("No memory for the frame buffer. Stopping.");
    return;
  }
  rgbLedWrite(PIN_RGB, 0, 0, 0);
  external.begin();
  boot.begin();
  WiFi.mode(WIFI_STA);
  state.place = settings::loadPlace();
  refreshAiReady();
  state.model = settings::model();
  worker.begin();
  connection.begin();
  log_i("wifi-lab-w3: city %s, AI gateway %s, model %s", state.place.valid() ? state.place.name.c_str() : "not set",
        state.aiReady ? "set" : "not set", state.model.c_str());
  printHelp();
}

void loop() {
  FrameBuffer &fb = display.fb();
  if (fb.pixels() == nullptr) {
    delay(1000);
    return;
  }
  pollSerial();
  handleButton(external);
  handleButton(boot);
  connection.loop();
  if (connection.takeJustConnected()) fetchWeather("joined");
  takeResults();

  // Keep the weather fresh; after a failure wait a minute before trying again.
  const uint32_t now = millis();
  const bool stale = !state.out.weatherAt || now - state.out.weatherAt > kWeatherEveryMs;
  if (online() && state.place.valid() && stale && !state.out.busy && now - state.lastWeatherTry > kRetryMs)
    fetchWeather("refresh");

  const float t = now / 1000.0f;
  fb.fillScreen(pal::night);
  const bool noWifi = connection.state() == Connection::State::NoCredentials;
  switch (state.view) {
    case 0: noWifi ? drawNoWifi(fb) : w3::now(fb, state.out, state.place.valid(), t); break;
    case 1: w3::forecast(fb, state.out); break;
    case 2: w3::week(fb, state.out); break;
    case kOracleView: w3::oracleView(fb, state.out, state.aiReady, state.model, t); break;
    default: noWifi ? drawNoWifi(fb) : views::status(fb, connection, t); break;
  }
  const char *title = state.view < kOracleView ? "Weather" : state.view == kOracleView ? "Bench" : "Wi-Fi";
  w3::header(fb, title, kViewNames[state.view], state.view, kViews, state.place, connection.state());
  w3::footer(fb, state.view == kOracleView ? "click: view  double: new haiku" : "click: view  double: refresh  hold: oracle",
             state.out);
  display.push();
}
