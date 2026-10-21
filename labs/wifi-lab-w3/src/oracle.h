// The Bench Oracle: the board asks an AI model about this moment at the bench.
//
// The board talks to a LiteLLM gateway (esp-labs/apps/ai-gateway, on your Mac or a server) in the
// OpenAI-style chat completions format. LiteLLM forwards to whichever provider and model the name
// maps to (Claude, GPT, Gemini, a local Ollama model...), so switching models never needs a reflash,
// and the provider keys stay on the gateway, not on the chip.
//
// The board sends what it knows (place, time, weather, its own temperature and signal) and asks for
// a haiku, or an answer to your question. To get an answer the screen can draw, the request offers
// one function, show_on_bench, and forces the model to call it: the reply is then always JSON in
// that function's shape (title, lines, LED colour, scene), never free text to pick apart.
//
// The gateway's key lives in NVS on the chip (serial: ai key ...), never in code, git or the log.
#pragma once

#include <ArduinoJson.h>

#include "https_client.h"
#include "weather.h"

struct OracleAnswer {
  String title;       // one or two words
  String lines[4];
  int lineCount = 0;
  uint8_t r = 0, g = 0, b = 0;  // LED colour
  Scene scene = Scene::Clouds;
  String model;
  uint32_t inputTokens = 0, outputTokens = 0;
  uint32_t ms = 0;  // how long the whole request took
};

namespace oracle {

constexpr const char *kDefaultModel = "oracle";  // a model_name in ai-gateway/config.yaml

// The screen fonts only have plain ASCII. Fold the usual typography to it, drop the rest.
inline String toAscii(const char *utf8) {
  String out;
  const auto *s = reinterpret_cast<const uint8_t *>(utf8);
  while (*s) {
    uint32_t cp = *s;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) cp &= 0x3F >> extra;
    s++;
    for (; extra > 0 && (*s & 0xC0) == 0x80; extra--, s++) cp = (cp << 6) | (*s & 0x3F);
    if (cp >= 0x20 && cp < 0x7F) out += char(cp);
    else if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0x201C || cp == 0x201D) out += '"';
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    else if (cp == 0x2026) out += "...";
    else if (cp == 0xB0) out += " deg";
    else if (cp == 0xDF) out += "ss";
    else if (cp >= 0xE0 && cp <= 0xE5) out += 'a';
    else if (cp >= 0xE8 && cp <= 0xEB) out += 'e';
    else if (cp >= 0xEC && cp <= 0xEF) out += 'i';
    else if (cp >= 0xF2 && cp <= 0xF6) out += 'o';
    else if (cp >= 0xF9 && cp <= 0xFC) out += 'u';
  }
  return out;
}

inline void addTool(JsonDocument &req) {
  JsonObject tool = req["tools"].add<JsonObject>();
  tool["type"] = "function";
  JsonObject fn = tool["function"].to<JsonObject>();
  fn["name"] = "show_on_bench";
  fn["description"] = "Show text on the bench's small 320x170 screen and set its RGB LED.";
  JsonObject schema = fn["parameters"].to<JsonObject>();
  schema["type"] = "object";
  JsonObject props = schema["properties"].to<JsonObject>();
  props["title"]["type"] = "string";
  props["title"]["description"] = "one or two words, at most 14 characters";
  props["lines"]["type"] = "array";
  props["lines"]["items"]["type"] = "string";
  props["lines"]["description"] = "1 to 4 lines, at most 28 characters each, plain ASCII only";
  props["color"]["type"] = "string";
  props["color"]["description"] = "LED colour matching the mood, as #rrggbb";
  props["scene"]["type"] = "string";
  JsonArray scenes = props["scene"]["enum"].to<JsonArray>();
  for (int i = 0; i <= static_cast<int>(Scene::Night); i++) scenes.add(weather::sceneName(static_cast<Scene>(i)));
  JsonArray required = schema["required"].to<JsonArray>();
  for (const char *k : {"title", "lines", "color", "scene"}) required.add(k);
  req["tool_choice"]["type"] = "function";  // must answer by calling it
  req["tool_choice"]["function"]["name"] = "show_on_bench";
}

// context: a small JSON text describing the moment (see main.cpp). question: empty = haiku.
// baseUrl: the gateway, e.g. http://192.168.178.31:4000 (serial: ai url ...).
inline HttpsResult ask(const String &baseUrl, const String &apiKey, const String &model, const String &context,
                       const String &question, OracleAnswer &out) {
  JsonDocument req;
  req["model"] = model;
  req["max_tokens"] = 400;
  JsonArray messages = req["messages"].to<JsonArray>();
  JsonObject system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] =
      "You are the Bench Oracle, living inside an ESP32 on a maker's workbench with a tiny screen and an RGB LED. "
      "Be warm, witty and concrete; use the context (place, time, weather, the chip's own state) when it fits. "
      "Always answer by calling show_on_bench. Lines must be short enough for the screen.";
  addTool(req);
  JsonObject msg = messages.add<JsonObject>();
  msg["role"] = "user";
  const String task = question.isEmpty()
                         ? String("Write a haiku (3 lines) about this exact moment at the bench, weaving in the "
                                  "weather. Title: the mood in one or two words.")
                         : "Question from the maker: " + question +
                               "\nAnswer in at most 4 short lines. Title: a one or two word headline.";
  msg["content"] = "Context: " + context + "\n" + task;
  String body;
  serializeJson(req, body);

  String base = baseUrl;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  HttpsResult r = https::post(base + "/v1/chat/completions", body,
                              {{"Authorization", "Bearer " + apiKey}, {"Content-Type", "application/json"}}, 30'000);
  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, r.body);
  if (!r.ok()) {
    // LiteLLM explains errors as {"error":{"message":...,"type":...,"code":...}}
    if (!parseError && doc["error"]["message"].is<const char *>())
      r.error = "HTTP " + String(r.status) + ": " + toAscii(doc["error"]["message"].as<const char *>());
    return r;
  }
  if (parseError) {
    r.error = String("bad JSON: ") + parseError.c_str();
    return r;
  }
  // The function's arguments arrive as a JSON *string* inside the JSON answer: parse it a second time.
  const char *arguments = doc["choices"][0]["message"]["tool_calls"][0]["function"]["arguments"] | "";
  JsonDocument args;
  if (!*arguments || deserializeJson(args, arguments)) {
    r.error = "no show_on_bench call in the reply (does this model support tools?)";
    return r;
  }
  JsonObjectConst input = args.as<JsonObjectConst>();
  OracleAnswer a;
  a.title = toAscii(input["title"] | "");
  for (JsonVariantConst line : input["lines"].as<JsonArrayConst>())
    if (a.lineCount < 4) a.lines[a.lineCount++] = toAscii(line | "");
  unsigned rgb = 0x40C0FF;
  sscanf(input["color"] | "#40c0ff", "#%6x", &rgb);
  a.r = rgb >> 16, a.g = (rgb >> 8) & 0xFF, a.b = rgb & 0xFF;
  if (!weather::sceneFromName(input["scene"] | "", a.scene)) a.scene = Scene::Clouds;
  a.model = doc["model"] | model.c_str();
  a.inputTokens = doc["usage"]["prompt_tokens"] | 0;
  a.outputTokens = doc["usage"]["completion_tokens"] | 0;
  a.ms = r.ms;
  out = a;
  return r;
}

}  // namespace oracle
