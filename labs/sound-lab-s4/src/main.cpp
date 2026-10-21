// Sound lab S4 Recorder.  uv run pio run -d labs/sound-lab-s4 -e sound-lab-s4 -t upload
// A voice-activated recorder. Armed, it waits for sound louder than the trigger, then records,
// starting half a second *before* the trigger (the pre-roll: the sampler's ring still has it),
// until 1.2 s of quiet. Clips go to PSRAM (12 clips, 6 MB, gone on reset). Listen to them on your
// phone at http://bench.local (the Wi-Fi W1 saved, plus the admin token), or pull a WAV file over
// USB with tools/sound_pull.py.
//
// And with the MAX98357A + speaker wired, `play <n>` plays a clip on the board itself, at any speed
// (0.5 = slower and deeper, 2 = the chipmunk).
//
// Which microphone matters here more than anywhere. The KY-037 has no amplifier, so a voice fills
// a small part of the ADC's range: clips are made louder ("normalised") on the way out and sound
// thin and hissy: a loudness sensor first, a microphone second. The INMP441 has its own amplifier
// and a 24-bit converter inside: its clips are actually pleasant to listen to. `mic auto` (the
// default) picks the INMP441 whenever it answers.
//
// Echo guard: while the speaker plays (and 200 ms after), the shell gives the lab no frames, so the
// voice trigger can't record the board's own playback. `play` refuses while recording, and `rec`
// stops a playback first.
//
// Wiring: a microphone (KY-037: + -> 3V3, G -> GND, AO -> GPIO 6, DO -> GPIO 21; or INMP441: VDD ->
// 3V3, GND, L/R -> GND, SCK -> 40, WS -> 41, SD -> 42), the screen from lesson 03, optionally the
// MAX98357A (VIN -> 5V, GND, BCLK -> 39, LRC -> 38, DIN -> 47). Push button on GPIO 5 optional.
//
// Views: recorder · clips · web   (click = next, double-click = the action in the footer,
//        long press = rec / stop; on the clips view: double-click = play, long press = next clip)
// Serial: help lists the commands. Shared code: kit/lib/sound-kit, kit/lib/wifi-kit (Wi-Fi, token).
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

// Playback on the board's speaker. The clip is leased from the store while it plays, so `del`,
// `clear` or a full store can take it off the list but can't free its samples under the speaker.
// The speaker task reads the samples on its own core, up to a block (16 ms) after it was told to
// stop, so a stopped clip is "retiring" for 100 ms before its lease is given back.
struct Playback {
  rec::Clip *clip = nullptr;
  uint32_t number = 0;
  float speed = 1;
  uint32_t sinceMs = 0;
} playback;
rec::Clip *retiring = nullptr;
uint32_t retireAt = 0;

void endPlayback() {
  rec::ClipStore::release(retiring);  // an older one still retiring: long done by now
  retiring = playback.clip;
  retireAt = millis();
  playback = Playback{};
}

// ---- Views ------------------------------------------------------------------------------------

void drawRecorder(SoundLab &, FrameBuffer &fb, float t) { s4v::recorder(fb, recorder, recorder.config(), recorder.store(), t); }
void drawClips(SoundLab &lab, FrameBuffer &fb, float) {
  s4v::clips(fb, recorder.store(), picked, playback.number, playback.clip ? lab.speaker().progress() : 0);
}
void drawWeb(SoundLab &, FrameBuffer &fb, float) { s4v::web(fb, connection, api.started(), api.clients()); }

const SoundView kViews[] = {
    {"recorder", drawRecorder, "arm"},
    {"clips", drawClips, "play"},
    {"web", drawWeb, "wifi"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdArm(SoundLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("arm | arm on | arm off");
  recorder.arm(lab, lower.isEmpty() ? !recorder.armed() : lower == "on");
}

void stopPlayback(SoundLab &lab) {
  if (!playback.clip) return;
  lab.speaker().stop();
  lab.note(sv::Note::Info, "playback of clip %lu stopped", static_cast<unsigned long>(playback.number));
  endPlayback();
}

void cmdRec(SoundLab &lab, const String &, const String &) {
  if (recorder.recording()) return recorder.stop(lab);  // rec again (or a long press) stops
  stopPlayback(lab);  // never record the board's own speaker
  recorder.start(lab);
}

void cmdStop(SoundLab &lab, const String &, const String &) {
  if (playback.clip && !recorder.recording()) return stopPlayback(lab);
  recorder.stop(lab);
}

// Play clip n (0 = the picked one) at `speed` on the board's speaker.
void startPlayback(SoundLab &lab, uint32_t n, float speed) {
  if (recorder.recording()) return lab.warn("recording: stop first (the microphone would hear the speaker)");
  if (n == 0) {
    rec::Clip c;
    if (!recorder.store().facts(picked, c)) return lab.warn("no clips yet");
    n = c.number;
  }
  rec::Clip *c = recorder.store().lease(n);
  if (!c) return lab.warn("no finished clip %lu", static_cast<unsigned long>(n));
  if (playback.clip) {
    lab.speaker().stop();
    endPlayback();
  }
  // Normalised like the download, but at most x16: a whisper on the KY-037 would otherwise come
  // back as mostly hiss, at full volume.
  const float gain = std::min(16.0f, snd::gainFor(c->peak));
  playback = Playback{c, n, speed, millis()};
  lab.speaker().play(c->data, c->samples, speed * float(c->rate) / float(snd::Speaker::kRate), gain);
  lab.note(sv::Note::Good, "playing clip %lu (%.1f s) at x%.2f", static_cast<unsigned long>(n), c->durationMs() / 1000.0f / speed, speed);
}

void cmdPlay(SoundLab &lab, const String &arg, const String &) {
  const int sp = arg.indexOf(' ');
  long n = 0;
  float speed = 1.0f;
  if (!arg.isEmpty() && !SoundLab::number(sp < 0 ? arg : arg.substring(0, sp), 1, 1000000, n))
    return lab.warn("play [n] [speed 0.5-2.0]: clip n (default: the picked one) on the board's speaker");
  if (sp > 0 && !SoundLab::decimal(arg.substring(sp + 1), 0.5f, 2.0f, speed)) return lab.warn("speed: 0.5 (slow, deep) to 2.0 (fast, chipmunk)");
  startPlayback(lab, uint32_t(n), speed);
}

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
    {"stop", "", "stop recording (or the board's playback)", cmdStop},
    {"play", "[n] [speed]", "play clip n on the board's speaker, speed 0.5-2.0 (default 1)", cmdPlay},
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
  if (!lab.mic().digital())
    Serial.println("S4: recording with the KY-037: expect thin, hissy clips. An INMP441 (mic auto) sounds far better.");
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
  d["mic"] = snd::sourceShort(lab.mic().source());
  d["playing"] = playback.number;  // 0 = the board's speaker is quiet
  d["playProgress"] = playback.clip ? roundf(lab.speaker().progress() * 1000) / 1000 : 0;
  String out;
  serializeJson(d, out);
  api.publishStatus(out);
}

void tick(SoundLab &lab) {
  // Playback ends when the speaker says so; then the clip goes back to the store.
  // (Give the speaker task 150 ms to pick up the play command before trusting "not playing".)
  if (playback.clip && !lab.speaker().playing() && millis() - playback.sinceMs > 150) {
    lab.note(sv::Note::Info, "clip %lu played", static_cast<unsigned long>(playback.number));
    endPlayback();
  }
  if (retiring && millis() - retireAt > 100) {
    rec::ClipStore::release(retiring);
    retiring = nullptr;
  }
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
      case WebCommand::Type::Rec:
        stopPlayback(lab);
        recorder.start(lab);
        break;
      case WebCommand::Type::Stop:
        if (recorder.recording()) recorder.stop(lab);
        stopPlayback(lab);
        break;
      case WebCommand::Type::Play: startPlayback(lab, c.value, c.speed); break;
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

// On the clips view, a long press picks the next clip (double-click plays it). Elsewhere: rec.
bool gesture(SoundLab &lab, Gesture g, bool) {
  if (g != Gesture::LongPress || strcmp(lab.spec().views[lab.view()].name, "clips") != 0) return false;
  Serial.println("> next   (long press)");
  lab.runLine("next");
  return true;
}

const SoundLabSpec kSpec{
    "sound-lab-s4", "S4", "Recorder", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, frame, tick, gesture, "rec",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
