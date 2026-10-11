// The worker: does the slow network jobs (an HTTPS request takes 0.3 to 10 s) in its own FreeRTOS
// task, one at a time, so loop() keeps animating the screen meanwhile.
//
// loop() hands over one job with everything it needs (submit). The worker runs it and publishes
// the outcome with a new version number; loop() copies the outcome when the version changes.
// The two only meet inside the mutex, and never share a String while the other is using it.
#pragma once

#include <Preferences.h>

#include <mutex>

#include "oracle.h"
#include "weather.h"

struct RequestStats {
  const char *what = "";  // "weather", "ai", ...
  int status = 0;
  uint32_t ms = 0;
  size_t bytes = 0;
};

class Worker {
 public:
  enum class Job : uint8_t { None, Geocode, Weather, Oracle, Ask };

  struct Request {
    Job job = Job::None;
    String text;     // city name, or the question
    Place place;     // for Weather
    String aiUrl, apiKey, model, context;  // for Oracle and Ask
  };

  struct Outcome {
    uint32_t version = 0;
    const char *busy = nullptr;  // what is running right now, nullptr when idle
    Place place;
    bool placeChanged = false;   // a geocode found a new place (loop saves it and fetches weather)
    Weather weather;
    uint32_t weatherAt = 0;      // millis() when it arrived, 0 = never
    OracleAnswer answer;
    String question;             // empty = haiku
    uint32_t answerAt = 0;
    RequestStats last;
    String error;                // last failure, cleared by the next success
  };

  void begin() { xTaskCreatePinnedToCore(task, "worker", 16 * 1024, this, 1, nullptr, 0); }

  // From loop(). false when a job is already waiting or running: one thing at a time.
  bool submit(const Request &r) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.job != Job::None || out_.busy) return false;
    pending_ = r;
    return true;
  }

  // From loop(): copy the outcome into `mine` if it changed since the last copy.
  bool update(Outcome &mine) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mine.version == out_.version) return false;
    mine = out_;
    out_.placeChanged = false;  // delivered once
    return true;
  }

 private:
  static void task(void *self) { static_cast<Worker *>(self)->run(); }

  void run() {
    for (;;) {
      Request r;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.job != Job::None) {
          r = pending_;
          pending_ = Request{};
          out_.busy = r.job == Job::Geocode ? "finding place" : r.job == Job::Weather ? "weather" : "asking the ai";
          out_.version++;
        }
      }
      if (r.job == Job::None) {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }
      perform(r);
    }
  }

  void perform(const Request &r) {
    HttpsResult res;
    Place place;
    Weather weather;
    OracleAnswer answer;
    const char *what = "";
    switch (r.job) {
      case Job::Geocode: what = "geocoding", res = weather::geocode(r.text, place); break;
      case Job::Weather: what = "weather", res = weather::fetch(r.place, weather); break;
      case Job::Oracle:
      case Job::Ask: what = "ai", res = oracle::ask(r.aiUrl, r.apiKey, r.model, r.context, r.job == Job::Ask ? r.text : "", answer); break;
      case Job::None: break;
    }
    const bool good = res.ok() && res.error.isEmpty();
    if (good) log_i("%s: HTTP %d in %lu ms, %u bytes", what, res.status, static_cast<unsigned long>(res.ms), unsigned(res.bytes));
    else log_w("%s failed after %lu ms: %s", what, static_cast<unsigned long>(res.ms), res.error.c_str());

    std::lock_guard<std::mutex> lock(mutex_);
    out_.busy = nullptr;
    out_.last = {what, res.status, res.ms, res.bytes};
    if (!good) {
      out_.error = String(what) + ": " + res.error;
    } else {
      out_.error = "";
      if (r.job == Job::Geocode) out_.place = place, out_.placeChanged = true;
      if (r.job == Job::Weather) out_.weather = weather, out_.weatherAt = millis();
      if (r.job == Job::Oracle || r.job == Job::Ask) {
        out_.answer = answer;
        out_.question = r.job == Job::Ask ? r.text : "";
        out_.answerAt = millis();
      }
    }
    out_.version++;
  }

  std::mutex mutex_;
  Request pending_;
  Outcome out_;
};
