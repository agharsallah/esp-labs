// Sound lab S4 Recorder.  uv run pio run -e sound-lab-s4 -t upload
// A voice-activated recorder. Armed, it waits for sound louder than the trigger, then records,
// starting half a second *before* the trigger (the pre-roll: the sampler's ring still has it),
// until 1.2 s of quiet. Clips go to PSRAM (12 clips, 6 MB, gone on reset). Listen to them on your
// phone at http://bench.local (the Wi-Fi W1 saved, plus the admin token), or pull a WAV file over
// USB with tools/sound_pull.py.
//
// The KY-037 has no amplifier, so a voice fills a small part of the ADC's range: clips are made
// louder ("normalised") on the way out, and they will sound thin and hissy. That's the lesson's
// honest part: this module is a loudness sensor first, a microphone second.
//
// Wiring: as S1 (+ -> 3V3 · G -> GND · AO -> GPIO 6 · DO -> GPIO 21) and the screen from lesson 03.
// The push button on GPIO 5 is optional (it does what BOOT does).
//
// Views: recorder · clips · web   (click = next, double-click = the action in the footer,
//        long press = rec / stop)
// Serial: help lists the commands. Shared code: lib/sound-kit, lib/wifi-kit (Wi-Fi, token).
#include "api_token.h"
#include "connection.h"
#include "recorder.h"
#include "recorder_api.h"
#include "s4_views.h"
#include "sound_lab.h"

namespace {

rec::Recorder recorder;
ApiToken token;
Connection connection;
RecorderApi api(recorder.store(), token);
int picked = 0;          // the clips view's picked row
uint32_t lastStatus = 0;

// ---- Views ------------------------------------------------------------------------------------

void drawRecorder(SoundLab &, FrameBuffer &fb, float t) { s4v::recorder(fb, recorder, recorder.config(), recorder.store(), t); }
void drawClips(SoundLab &, FrameBuffer &fb, float) { s4v::clips(fb, recorder.store(), picked); }
void drawWeb(SoundLab &, FrameBuffer &fb, float) { s4v::web(fb, connection, api.started(), api.clients()); }

const SoundView kViews[] = {
    {"recorder", drawRecorder, "arm"},
    {"clips", drawClips, "next"},
    {"web", drawWeb, "wifi"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdArm(SoundLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("arm | arm on | arm off");
  recorder.arm(lab, lower.isEmpty() ? !recorder.armed() : lower == "on");
}

void cmdRec(SoundLab &lab, const String &, const String &) {
  if (recorder.recording()) return recorder.stop(lab);  // rec again (or a long press) stops
  recorder.start(lab);
}

void cmdStop(SoundLab &lab, const String &, const String &) { recorder.stop(lab); }

void cmdTrigger(SoundLab &lab, const String &arg, const String &) {
  float db = 0;
  if (!SoundLab::decimal(arg, 3, 60, db)) return lab.warn("trigger <dB above the quiet>, 3-60 (now %.0f)", recorder.config().startDb);
  recorder.config().startDb = db;
  lab.note(sv::Note::Info, "voice starts above %.0f dB", db);
}

void cmdHold(SoundLab &lab, const String &arg, const String &) {
  long ms = 0;
  if (!SoundLab::number(arg, 200, 10000, ms)) return lab.warn("hold <ms>, 200-10000: quiet this long ends a clip (now %lu)", static_cast<unsigned long>(recorder.config().holdMs));
  recorder.config().holdMs = uint32_t(ms);
  lab.note(sv::Note::Info, "a clip ends after %.1f s of quiet", ms / 1000.0f);
}

void cmdMax(SoundLab &lab, const String &arg, const String &) {
  long s = 0;
  if (!SoundLab::number(arg, 1, 120, s)) return lab.warn("max <s>, 1-120: the longest clip (now %lu)", static_cast<unsigned long>(recorder.config().maxMs / 1000));
  recorder.config().maxMs = uint32_t(s) * 1000;
  lab.note(sv::Note::Info, "clips stop at %ld s (from the next one)", s);
}

void cmdClips(SoundLab &lab, const String &, const String &) {
  rec::ClipStore &store = recorder.store();
  Serial.printf("%d clips, %.2f of %.0f MB of PSRAM (gone on reset):\n", store.count(), store.used() / 1048576.0, store.budget / 1048576.0);
  Serial.println("    #  length  kind   loudest     sent x  started");
  store.each([](const rec::Clip &c) {
    char when[24] = "-";
    if (c.wall > 0) {
      const time_t w = time_t(c.wall);
      struct tm tm;
      localtime_r(&w, &tm);
      strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    }
    Serial.printf("  %3lu  %5.1fs  %-5s  %5.1f dBFS  x%-5.1f %s%s\n", static_cast<unsigned long>(c.number), c.durationMs() / 1000.0f,
                  c.manual ? "hand" : "voice", snd::toDb(float(c.peak)), snd::gainFor(c.peak), when, c.open ? "  (recording)" : "");
  });
  lab.showView("clips");
}

void cmdNext(SoundLab &lab, const String &, const String &) {
  const int n = recorder.store().count();
  picked = n ? (picked + 1) % n : 0;
  lab.showView("clips");
}

void cmdDel(SoundLab &lab, const String &arg, const String &) {
  long n = 0;
  if (!SoundLab::number(arg, 1, 1000000, n)) return lab.warn("del <n>: the clip's number (clips lists them)");
  if (recorder.recording() && uint32_t(n) == recorder.clipNumber()) return lab.warn("clip %ld is recording: stop first", n);
  if (!recorder.store().remove(uint32_t(n))) return lab.warn("no clip %ld", n);
  lab.note(sv::Note::Info, "clip %ld deleted", n);
}

void cmdClear(SoundLab &lab, const String &, const String &) {
  const int n = recorder.store().clear();
  picked = 0;
  lab.note(sv::Note::Info, "%d clip%s deleted", n, n == 1 ? "" : "s");
}

// The serial transfer: a header line, the WAV file as base64 lines, a CRC-32 line. Nothing else may
// be printed in between, so this runs to the end before loop() goes on (~20 s for 5 s of sound
// at 115200 baud: 32 KB of WAV a second of sound, 4/3 more as base64, 11.5 KB/s down the wire).
void cmdSend(SoundLab &lab, const String &arg, const String &) {
  long n = 0;
  if (!SoundLab::number(arg, 1, 1000000, n)) return lab.warn("send <n>: for tools/sound_pull.py (close the serial monitor first)");
  if (recorder.recording()) return lab.warn("recording: stop first, the transfer pauses everything else");
  rec::Clip *c = recorder.store().lease(uint32_t(n));
  if (!c) return lab.warn("no finished clip %ld", n);
  const float gain = snd::gainFor(c->peak);
  char name[48];
  rec::clipName(*c, name, sizeof(name));
  const size_t total = c->wavBytes();
  Serial.printf("WAV-BEGIN %lu %u %s\n", static_cast<unsigned long>(c->number), unsigned(total), name);
  uint8_t bytes[57];  // 57 bytes = one 76-character base64 line
  char line[80];
  uint32_t crc = 0;
  for (size_t at = 0; at < total;) {
    const size_t got = rec::wavRead(*c, gain, bytes, sizeof(bytes), at);
    crc = rec::crc32(crc, bytes, got);
    rec::base64(bytes, got, line);
    Serial.println(line);
    at += got;
  }
  Serial.printf("WAV-END %08lx\n", static_cast<unsigned long>(crc));
  rec::ClipStore::release(c);
  lab.note(sv::Note::Good, "sent clip %ld (%u bytes)", n, unsigned(total));
}

void cmdToken(SoundLab &lab, const String &, const String &lower) {
  if (lower == "new") {
    token.regenerate();
    lab.note(sv::Note::Warn, "new admin token: open pages must unlock again");
  } else if (!lower.isEmpty()) {
    return lab.warn("token | token new");
  }
  Serial.printf("Admin token: %s\nPaste it into the page at http://%s.local (it is shared with R7/R8; keep it out of chats and git)\n",
                token.value().c_str(), Connection::kHostname);
}

void cmdWifi(SoundLab &lab, const String &, const String &) {
  if (connection.ssid().isEmpty()) return lab.warn("no Wi-Fi saved: flash wifi-lab-w1 once and join, or use tools/sound_pull.py over USB");
  if (connection.state() != Connection::State::Connected)
    return lab.note(sv::Note::Warn, "joining '%s'... (last reason: %s)", connection.ssid().c_str(), Connection::reasonHint(connection.lastReason()));
  Serial.printf("Wi-Fi '%s' · IP %s · %ld dBm · page http://%s.local · %u page(s) open\n", connection.ssid().c_str(),
                WiFi.localIP().toString().c_str(), static_cast<long>(WiFi.RSSI()), Connection::kHostname, unsigned(api.clients()));
  lab.showView("web");
}

const SoundCommand kCommands[] = {
    {"arm", "[on|off]", "voice-activated recording (alone: toggle)", cmdArm},
    {"rec", "", "record now, by hand (again, or long press: stop)", cmdRec},
    {"stop", "", "stop recording", cmdStop},
    {"trigger", "<dB>", "how loud starts a clip, dB above the quiet (default 15)", cmdTrigger},
    {"hold", "<ms>", "quiet this long ends a clip (default 1200)", cmdHold},
    {"max", "<s>", "the longest clip (default 20)", cmdMax},
    {"clips", "", "list the clips (PSRAM only: gone on reset)", cmdClips},
    {"next", "", "pick the next clip on the clips view", cmdNext},
    {"del", "<n>", "delete clip n", cmdDel},
    {"clear", "", "delete every finished clip", cmdClear},
    {"send", "<n>", "clip n as a WAV over serial, for tools/sound_pull.py", cmdSend},
    {"token", "[new]", "the admin token the phone page asks for", cmdToken},
    {"wifi", "", "Wi-Fi and the page's address", cmdWifi},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(SoundLab &lab) {
  token.begin();
  connection.begin();
  Serial.println("S4: clips are kept in PSRAM only, a reset forgets them. Recording always shows: REC on screen, red LED.");
  (void)lab;
}

void frame(SoundLab &lab, const snd::Frame &f, uint32_t ms) { recorder.frame(lab, f, ms); }

void publishStatus(SoundLab &lab) {
  const snd::LevelReading &r = lab.level();
  const rec::ClipStore &store = recorder.store();
  JsonDocument d;
  d["state"] = recorder.recording() ? "recording" : recorder.armed() ? "armed" : "off";
  d["armed"] = recorder.armed();
  d["manual"] = recorder.recording() && recorder.manual();
  d["clip"] = recorder.clipNumber();
  d["recMs"] = recorder.recordedMs();
  d["above"] = roundf(r.above() * 10) / 10;
  d["floorDb"] = roundf(r.floorDb * 10) / 10;
  d["trigger"] = recorder.config().startDb;
  d["holdMs"] = recorder.config().holdMs;
  d["maxS"] = recorder.config().maxMs / 1000;
  d["prerollMs"] = recorder.config().prerollMs;
  d["clips"] = store.count();
  d["usedBytes"] = store.used();
  d["budgetBytes"] = store.budget;
  d["rate"] = lab.mic().rate();
  String out;
  serializeJson(d, out);
  api.publishStatus(out);
}

void tick(SoundLab &lab) {
  recorder.tick(lab);
  connection.loop();
  if (!api.started() && connection.state() == Connection::State::Connected) {
    api.begin();
    lab.note(sv::Note::Good, "page: http://%s.local", Connection::kHostname);
  }
  if (!api.started()) return;
  WebCommand c;
  while (api.take(c)) {
    switch (c.type) {
      case WebCommand::Type::Arm: recorder.arm(lab, c.value != 0); break;
      case WebCommand::Type::Rec: recorder.start(lab); break;
      case WebCommand::Type::Stop: recorder.stop(lab); break;
      case WebCommand::Type::Delete:
        if (recorder.recording() && c.value == recorder.clipNumber()) lab.note(sv::Note::Warn, "web: clip %lu is recording", static_cast<unsigned long>(c.value));
        else if (recorder.store().remove(c.value)) lab.note(sv::Note::Info, "web: clip %lu deleted", static_cast<unsigned long>(c.value));
        break;
    }
  }
  api.loop();
  if (millis() - lastStatus >= 100) {
    lastStatus = millis();
    publishStatus(lab);
  }
}

const SoundLabSpec kSpec{
    "sound-lab-s4", "S4", "Recorder", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, frame, tick, nullptr, "rec",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
