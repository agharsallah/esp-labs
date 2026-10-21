// Sound lab S6 Sound games.  uv run pio run -d labs/sound-lab-s6 -e sound-lab-s6 -t upload
// Three ways to play with what the microphone hears:
//   knock   a knock lock: knock your secret rhythm on the table once (`knock set`); after that
//           only that rhythm, fast or slow, opens it (the LED on GPIO 4 + a green RGB LED, 5 s)
//   bird    Clap Bird: every clap is a flap. Fly through the gaps between the pipes
//   party   the board finds the beat in music (a phone speaker near the mic) and throws light at it
// Each view listens only while it is on screen: a clap in Clap Bird never knocks on the lock.
//
// Sounds (MAX98357A + speaker, optional; `sound off` silences the lab): Clap Bird blips on every
// flap, dings per pipe, crashes, and plays a fanfare for a new best; the lock plays your secret back
// after `knock set` (and on `knock play`), chimes when it opens and buzzes on a wrong rhythm; the
// party can tick on every beat (`party click on`).
// The trade-off: while the speaker sounds and 200 ms after, the shell's echo guard gives this lab no
// frames, so the board never hears itself, but a clap in that window is missed too. So the game
// sounds are tiny (a flap blip is 20 ms, a ding 30 ms), and the lock makes no sound per knock
// while you set the secret: it would swallow the next knock of a fast rhythm.
//
// Wiring: a microphone (KY-037 as in S1, or the INMP441) and the screen. Optional: the MAX98357A +
// speaker, lesson 02's LED on GPIO 4 (through its resistor) as the lock's bolt, and the push button
// on GPIO 5 (it does what BOOT does).
//
// Views: knock · bird · party   (click = next, double-click = the action in the footer)
// Serial: help lists the commands. Shared code: kit/lib/sound-kit.
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_random.h>
#include <esp_timer.h>

#include "bird.h"
#include "knock_lock.h"
#include "onsets.h"
#include "rhythm.h"
#include "sound_lab.h"

namespace {

// ---- State ------------------------------------------------------------------------------------

snd::Onsets ears;

enum class Screen : uint8_t { Knock, Bird, Party };
Screen screen(const SoundLab &lab) { return Screen(lab.view()); }

// The knock lock.
struct Lock {
  snd::Rhythm secret;
  knock::Collector collector;
  bool setting = false;      // the next pattern becomes the secret
  snd::Rhythm attempt;       // the last finished try
  snd::RhythmMatch match;
  uint32_t attemptAt = 0;    // millis() of the last try
  uint32_t openUntil = 0;    // millis()
  uint32_t openedAt = 0;
  float tolerance = 0.12f;   // each gap may be off by this share of the whole pattern
} lock;

// Clap Bird.
bird::Game game(esp_random());
int best = 0;
uint32_t overAt = 0, lastStep = 0;
float stepAcc = 0;
bird::Game::State lastState = bird::Game::State::Ready;
uint32_t flapAt = 0;

// The beat party.
snd::BeatTracker beats;
struct Ring {
  uint32_t at;  // millis(), 0 = unused
  int16_t x, y;
  uint8_t hue;
};
constexpr int kRings = 8;
Ring rings[kRings];
int ringHead = 0;
uint8_t hue = 0;
constexpr int kBeatLog = 32;
uint32_t beatLog[kBeatLog];  // millis() of the last beats
int beatHead = 0;
uint32_t lastBeat = 0;
bool partyClick = false;  // a tick on every beat (off: the guard would hide ~200 ms after each)

// ---- Sounds -----------------------------------------------------------------------------------

bool soundOn = true;  // NVS games/sound

enum class Sfx : uint8_t { Flap, Point, Crash, Best, Open, Wrong, Tick };

void sfx(SoundLab &lab, Sfx s) {
  if (!soundOn) return;
  snd::Speaker &spk = lab.speaker();
  switch (s) {
    case Sfx::Flap: spk.tone(900, 20, syn::Wave::Square, 0.25f); break;  // 20 ms: the guard stays short
    case Sfx::Point: spk.tone(1568, 30, syn::Wave::Sine, 0.45f); break;
    case Sfx::Crash: spk.tune("C4/16 G3/16 C3/8", 300, syn::Wave::Saw); break;
    case Sfx::Best: lab.chime(Chime::Score); break;
    case Sfx::Open: spk.tune("C5/16 E5/16 G5/16 C6/8", 280, syn::Wave::Triangle); break;
    case Sfx::Wrong: lab.chime(Chime::No); break;
    case Sfx::Tick: spk.tone(2200, 8, syn::Wave::Square, 0.3f); break;
  }
}

// Playing a rhythm back (the secret): an esp_timer every 2 ms fires a click at each knock's time,
// so the timing is exact even though the screen loop only runs every ~40 ms.
struct RhythmPlayer {
  snd::Rhythm r;
  int next = 0;
  int64_t startUs = 0;
  snd::Speaker *speaker = nullptr;
  esp_timer_handle_t timer = nullptr;
} replay;

void onReplayTimer(void *) {
  const int64_t now = esp_timer_get_time() - replay.startUs;
  while (replay.next < replay.r.count && now >= int64_t(replay.r.atMs[replay.next] - replay.r.atMs[0]) * 1000) {
    replay.speaker->tone(replay.next == 0 ? 1320 : 1100, 25, syn::Wave::Square, 0.5f);
    replay.next++;
  }
  if (replay.next >= replay.r.count) esp_timer_stop(replay.timer);
}

void playRhythm(SoundLab &lab, const snd::Rhythm &r) {
  if (!soundOn || r.count < 1) return;
  if (!replay.timer) {
    esp_timer_create_args_t args = {};
    args.callback = onReplayTimer;
    args.name = "knock-replay";
    if (esp_timer_create(&args, &replay.timer) != ESP_OK) return;
  }
  esp_timer_stop(replay.timer);  // fine if it wasn't running
  replay.r = r;
  replay.next = 0;
  replay.speaker = &lab.speaker();
  replay.startUs = esp_timer_get_time() + 300000;  // a short pause first, so it doesn't run into the last knock
  esp_timer_start_periodic(replay.timer, 2000);
}

// ---- Saving -----------------------------------------------------------------------------------

void saveSecret() {
  Preferences p;
  p.begin("knock", false);
  uint32_t rel[snd::Rhythm::kMax];
  for (int i = 0; i < lock.secret.count; i++) rel[i] = lock.secret.atMs[i] - lock.secret.atMs[0];
  if (lock.secret.count) p.putBytes("secret", rel, lock.secret.count * sizeof(uint32_t));
  else p.remove("secret");
  p.end();
}

void loadAll() {
  Preferences p;
  p.begin("knock", true);
  if (p.isKey("secret")) {
    const size_t n = p.getBytesLength("secret") / sizeof(uint32_t);
    if (n >= 2 && n <= size_t(snd::Rhythm::kMax)) {
      p.getBytes("secret", lock.secret.atMs, n * sizeof(uint32_t));
      lock.secret.count = int(n);
    }
  }
  p.end();
  p.begin("bird", true);
  best = int(p.getUInt("best", 0));
  p.end();
  p.begin("games", true);
  soundOn = p.getBool("sound", true);
  p.end();
}

void saveBest() {
  Preferences p;
  p.begin("bird", false);
  p.putUInt("best", uint32_t(best));
  p.end();
}

// ---- Detection (every 4 ms frame) -------------------------------------------------------------

void finishPattern(SoundLab &lab, const snd::Rhythm &r) {
  if (lock.setting) {
    lock.setting = false;
    if (r.count < 2) return lab.note(sv::Note::Warn, "a secret needs 2 knocks or more");
    lock.secret = r;
    saveSecret();
    playRhythm(lab, r);  // hear what it heard
    return lab.note(sv::Note::Good, "secret saved: %d knocks, %.1f s", r.count, r.lengthMs() / 1000.0f);
  }
  lock.attempt = r;
  lock.attemptAt = millis();
  lock.match = snd::matchRhythm(lock.secret, r, lock.tolerance);
  if (lock.match.ok) {
    lock.openUntil = millis() + 5000;
    lock.openedAt = millis();
    digitalWrite(PIN_LED, HIGH);
    sfx(lab, Sfx::Open);
    lab.note(sv::Note::Good, "OPEN (score %d)", lock.match.score);
  } else {
    sfx(lab, Sfx::Wrong);
    lab.note(sv::Note::Bad, "locked: %s", lock.match.why);
  }
}

void frame(SoundLab &lab, const snd::Frame &f, uint32_t ms) {
  const float above = lab.level().instAbove();
  if (screen(lab) == Screen::Party) {
    if (beats.push(f.rms, above, ms)) {
      hue += 37;  // the next colour: 37/256 of the wheel, so neighbours never look alike
      lastBeat = millis();
      beatLog[beatHead] = lastBeat;
      beatHead = (beatHead + 1) % kBeatLog;
      rings[ringHead] = {lastBeat, int16_t(40 + esp_random() % 240), int16_t(50 + esp_random() % 90), hue};
      ringHead = (ringHead + 1) % kRings;
      if (partyClick) sfx(lab, Sfx::Tick);
    }
    return;
  }
  if (lab.meter().learning()) return;
  const snd::Onsets::Event e = ears.push(above, ms);
  if (screen(lab) == Screen::Bird && e.kind == snd::Onsets::Kind::Onset) {
    // A clap at once (not when it has died away): a game can't wait 160 ms.
    if (game.state() == bird::Game::State::Over) {
      if (millis() - overAt < 800) return;  // the crash's own clap must not restart it
      game.reset();
    }
    game.flap();
    flapAt = millis();
    sfx(lab, Sfx::Flap);
  }
  if (screen(lab) == Screen::Knock) {
    if (e.kind == snd::Onsets::Kind::Clap && !lock.collector.add(e.ms)) lab.note(sv::Note::Warn, "12 knocks is the most");
    snd::Rhythm done;
    if (lock.collector.poll(ms, done)) finishPattern(lab, done);
  }
}

// ---- Views ------------------------------------------------------------------------------------

// A rhythm as knocks on a line: x = when, scaled so `scaleMs` (default: the whole pattern) fills w.
void rhythmRow(FrameBuffer &fb, int x, int y, int w, const snd::Rhythm &r, uint16_t c, const snd::Rhythm *secret = nullptr,
               uint32_t scaleMs = 0) {
  fb.drawFastHLine(x, y + 10, w, pal::dim);
  const uint32_t len = scaleMs ? scaleMs : r.lengthMs();
  for (int i = 0; i < r.count; i++) {
    const int kx = x + (len ? int(uint64_t(r.atMs[i] - r.atMs[0]) * w / len) : 0);
    fb.fillRoundRect(kx - 2, y, 5, 21, 2, c);
    // Under each gap of a try: green when it matches the secret's gap, amber close, coral off.
    if (secret && i + 1 < r.count && secret->count == r.count) {
      const float err = fabsf(secret->share(i) - r.share(i));
      const int nx = x + int(uint64_t(r.atMs[i + 1] - r.atMs[0]) * w / len);
      const uint16_t ec = err <= lock.tolerance * 0.5f ? pal::teal : err <= lock.tolerance ? pal::amber : pal::coral;
      fb.fillRect(kx + 4, y + 24, std::max(1, nx - kx - 8), 3, ec);
    }
  }
}

void padlock(FrameBuffer &fb, int cx, int cy, bool open, float lift, float shake, float t) {
  const int x = cx + int(shake);
  const uint16_t c = open ? pal::teal : sv::kSound;
  // The shackle: two half rings. Open: lifted, and its right leg out of the body.
  const int sy = cy - 14 - int(lift * 10);
  for (int r = 10; r <= 13; r++) fb.drawCircle(x, sy, r, c);
  fb.fillRect(x - 14, sy, 29, 15 + int(lift * 10), pal::night);  // hide the lower half of the rings
  fb.fillRect(x - 13, sy, 4, 15 + int(lift * 10), c);
  if (!open) fb.fillRect(x + 10, sy, 4, 15, c);
  // The body with its keyhole.
  fb.fillRoundRect(x - 20, cy, 41, 32, 5, c);
  fb.fillCircle(x, cy + 12, 4, pal::night);
  fb.fillRect(x - 1, cy + 14, 3, 9, pal::night);
  if (open)
    for (int i = 0; i < 6; i++) {
      const float a = i * 1.047f + t * 2;
      fb.fillCircle(x + int(cosf(a) * 30), cy + 16 + int(sinf(a) * 26), 2, pal::teal);
    }
}

void drawKnock(SoundLab &, FrameBuffer &fb, float t) {
  const uint32_t now = millis();
  const bool open = now < lock.openUntil;
  const float lift = open ? std::min(1.0f, (now - lock.openedAt) / 300.0f) : 0;
  const bool failed = !lock.match.ok && now - lock.attemptAt < 500 && lock.attemptAt;
  const float shake = failed ? 5 * sinf((now - lock.attemptAt) * 0.08f) : 0;
  padlock(fb, 40, sv::kTop + 44, open, lift, shake, t);
  sv::bold(fb, 14, sv::kTop + 100, open ? pal::teal : pal::muted, open ? "OPEN" : "LOCKED");

  const int x = 96, w = 210;
  sv::small(fb, x, sv::kTop + 6, pal::muted, "secret");
  if (lock.secret.count >= 2) rhythmRow(fb, x, sv::kTop + 16, w, lock.secret, sv::kSound);
  else sv::small(fb, x + 44, sv::kTop + 22, pal::amber, "none yet: knock set");

  const snd::Rhythm &open_ = lock.collector.open();
  if (open_.count) {  // knocking right now: the knocks so far, on the secret's scale
    sv::small(fb, x, sv::kTop + 52, pal::amber, lock.setting ? "new secret..." : "listening...");
    // On the secret's scale, so you see the knocks land where the secret's did.
    const uint32_t scale = std::max<uint32_t>(lock.secret.lengthMs(), open_.lengthMs());
    rhythmRow(fb, x, sv::kTop + 62, w, open_, pal::amber, nullptr, scale ? scale : 1);
  } else if (lock.attempt.count) {
    char text[40];
    if (lock.match.ok) snprintf(text, sizeof(text), "your knock: score %d", lock.match.score);
    else snprintf(text, sizeof(text), "your knock: %s", lock.match.why);
    sv::small(fb, x, sv::kTop + 52, lock.match.ok ? pal::teal : pal::coral, text);
    rhythmRow(fb, x, sv::kTop + 62, w, lock.attempt, pal::ink, &lock.secret);
  }
  const char *hint = lock.setting ? "knock your secret, then wait 1.5 s"
                     : lock.secret.count < 2 ? "double-click, then knock a rhythm"
                                             : "knock on the table near the mic";
  sv::small(fb, x, sv::kTop + 104, lock.setting ? pal::amber : pal::muted, hint);
  char tol[32];
  snprintf(tol, sizeof(tol), "tolerance %d%%", int(lock.tolerance * 100 + 0.5f));
  sv::small(fb, x, sv::kTop + 116, pal::dim, tol);
}

void drawBird(SoundLab &, FrameBuffer &fb, float t) {
  using namespace bird;
  // Far hills drift by slowly (parallax): the world feels deep even on a flat screen.
  const float drift = game.state() == Game::State::Playing ? t * 12 : 0;
  for (int x = 0; x < 320; x += 2) {
    const int h = 10 + int(8 * sinf((x + drift) * 0.03f) + 5 * sinf((x + drift) * 0.071f));
    fb.drawFastVLine(x, int(kBottom) - h, h, sv::kPanel);
    fb.drawFastVLine(x + 1, int(kBottom) - h, h, sv::kPanel);
  }
  // Pipes, with a lip at each end of the gap.
  for (int i = 0; i < kPipes; i++) {
    const Pipe &p = game.pipe(i);
    const int px = int(p.x), top = int(p.gapY - kGap / 2), bottom = int(p.gapY + kGap / 2);
    fb.fillRect(px, int(kTop), int(kPipeW), top - int(kTop), mix(pal::teal, pal::night, 90));
    fb.fillRect(px, bottom, int(kPipeW), int(kBottom) - bottom, mix(pal::teal, pal::night, 90));
    fb.fillRect(px - 3, top - 8, int(kPipeW) + 6, 8, pal::teal);
    fb.fillRect(px - 3, bottom, int(kPipeW) + 6, 8, pal::teal);
  }
  fb.drawFastHLine(0, int(kBottom), 320, pal::muted);
  // The bird: bobbing while it waits; its wing up just after a flap.
  const float by = game.state() == Game::State::Ready ? game.y() + 4 * sinf(t * 4) : game.y();
  const int bx = int(kBirdX), iy = int(by);
  const bool over = game.state() == Game::State::Over;
  fb.fillCircle(bx, iy, int(kRadius) + 1, over ? pal::coral : pal::amber);
  const bool wingUp = millis() - flapAt < 120;
  fb.fillTriangle(bx - 7, iy + (wingUp ? -1 : 1), bx - 1, iy, bx - 5, iy + (wingUp ? -8 : 7), pal::ink);
  fb.fillCircle(bx + 3, iy - 2, 2, pal::ink);
  fb.drawPixel(bx + 4, iy - 2, pal::night);
  fb.fillTriangle(bx + 6, iy - 1, bx + 11, iy + 1, bx + 6, iy + 3, pal::coral);

  char text[40];
  snprintf(text, sizeof(text), "%d", game.score());
  sv::bold(fb, 160 - sv::textWidth(fb, text) / 2, int(kTop) + 18, pal::ink, text);
  snprintf(text, sizeof(text), "best %d", best);
  sv::small(fb, 312 - sv::textWidth(fb, text), int(kTop) + 6, pal::muted, text);
  if (game.state() == Game::State::Ready) sv::bold(fb, 112, 112, sv::kSound, "clap to fly!");
  if (over) {
    fb.fillRoundRect(80, 70, 160, 44, 6, sv::kPanel);
    snprintf(text, sizeof(text), "score %d", game.score());
    sv::bold(fb, 160 - sv::textWidth(fb, text) / 2, 88, game.score() >= best && best ? pal::amber : pal::ink, text);
    sv::small(fb, 104, 100, pal::muted, millis() - overAt < 800 ? "" : "clap to try again");
  }
}

void drawParty(SoundLab &lab, FrameBuffer &fb, float) {
  const uint32_t now = millis();
  // A flash of the beat's colour across the top, fading in 120 ms.
  const uint32_t since = now - lastBeat;
  if (lastBeat && since < 120) fb.fillRect(0, sv::kTop, 320, 6, mix(hsv(hue, 255, 255), pal::night, uint8_t(since * 2)));
  // Rings: one per beat, growing and fading over 0.7 s.
  for (const Ring &r : rings) {
    if (!r.at || now - r.at > 700) continue;
    const uint32_t age = now - r.at;
    const uint16_t c = mix(hsv(r.hue, 255, 255), pal::night, uint8_t(age * 255 / 700));
    const int rad = 4 + int(age * 0.09f);
    fb.drawCircle(r.x, r.y, rad, c);
    fb.drawCircle(r.x, r.y, rad + 1, c);
    if (age < 150) fb.fillCircle(r.x, r.y, 6 - int(age / 30), c);
  }
  // The tempo, big.
  char text[24];
  const int bpm = beats.bpm();
  snprintf(text, sizeof(text), bpm ? "%d" : "--", bpm);
  sv::big(fb, 12, sv::kTop + 52, since < 150 ? hsv(hue, 200, 255) : pal::ink, text);
  sv::small(fb, 14, sv::kTop + 58, pal::muted, "BPM");
  snprintf(text, sizeof(text), "%lu beats", static_cast<unsigned long>(beats.beats()));
  sv::small(fb, 14, sv::kTop + 72, pal::muted, text);
  // The last 8 s of beats, newest on the right, with the level under them.
  const int x = 8, y = sv::kTop + 100, w = 304;
  fb.drawFastHLine(x, y + 14, w, pal::dim);
  for (uint32_t b : beatLog) {
    if (!b || now - b > 8000) continue;
    const int bx = x + w - int((now - b) * w / 8000);
    fb.fillRect(bx, y, 2, 14, pal::amber);
  }
  sv::small(fb, x, y + 18, pal::muted, "beats, last 8 s");
  snprintf(text, sizeof(text), "sens x%.1f", beats.ratio);
  sv::small(fb, x + w - sv::textWidth(fb, text), y + 18, pal::muted, text);
  (void)lab;
}

const SoundView kViews[] = {
    {"knock", drawKnock, "knock set"},
    {"bird", drawBird, "bird"},
    {"party", drawParty, "party reset"},
};

// ---- Commands ---------------------------------------------------------------------------------

void printRhythm(const char *what, const snd::Rhythm &r) {
  Serial.printf("  %s: %d knocks over %lu ms, gaps:", what, r.count, static_cast<unsigned long>(r.lengthMs()));
  for (int i = 0; i + 1 < r.count; i++) Serial.printf(" %lu (%d%%)", static_cast<unsigned long>(r.atMs[i + 1] - r.atMs[i]), int(r.share(i) * 100 + 0.5f));
  Serial.println();
}

void cmdKnock(SoundLab &lab, const String &, const String &lower) {
  lab.showView("knock");
  if (lower == "set") {
    lock.setting = true;
    lock.collector.clear();
    return lab.note(sv::Note::Info, "knock your new secret, then wait 1.5 s");
  }
  if (lower == "clear") {
    lock.secret.clear();
    saveSecret();
    return lab.note(sv::Note::Info, "secret erased");
  }
  if (lower == "play") {
    if (lock.secret.count < 2) return lab.warn("no secret yet: knock set");
    if (!soundOn) return lab.warn("sound is off: sound on");
    playRhythm(lab, lock.secret);
    return lab.note(sv::Note::Info, "playing the secret: %d knocks", lock.secret.count);
  }
  if (!lower.isEmpty() && lower != "show") return lab.warn("knock set | knock clear | knock show | knock play");
  if (lock.secret.count < 2) Serial.println("  no secret yet: knock set");
  else printRhythm("secret", lock.secret);
  if (lock.attempt.count) {
    printRhythm("last try", lock.attempt);
    Serial.printf("  match: %s, score %d, worst gap off by %d%% (tolerance %d%%)\n", lock.match.ok ? "OPEN" : lock.match.why, lock.match.score,
                  int(lock.match.worst * 100 + 0.5f), int(lock.tolerance * 100 + 0.5f));
  }
}

void cmdTol(SoundLab &lab, const String &arg, const String &) {
  long pct = 0;
  if (!SoundLab::number(arg, 3, 30, pct))
    return lab.warn("tol <percent>, 3-30: how far each gap may be off, as a share of the pattern (now %d)", int(lock.tolerance * 100 + 0.5f));
  lock.tolerance = pct / 100.0f;
  lab.note(sv::Note::Info, "tolerance %ld%%", pct);
}

void cmdBird(SoundLab &lab, const String &, const String &lower) {
  lab.showView("bird");
  if (lower == "reset") {
    best = 0;
    saveBest();
    return lab.note(sv::Note::Info, "best score reset");
  }
  game.reset();
  lab.note(sv::Note::Info, "new round: clap to fly");
}

void cmdParty(SoundLab &lab, const String &arg, const String &lower) {
  lab.showView("party");
  if (lower.startsWith("sens")) {
    String value = arg.substring(4);
    value.trim();
    float r = 0;
    if (!SoundLab::decimal(value, 1.2f, 3.0f, r))
      return lab.warn("party sens <ratio>, 1.2-3.0: how far a beat must jump over the last second (now %.1f)", beats.ratio);
    beats.ratio = r;
    return lab.note(sv::Note::Info, "beat = %.1fx the last second", r);
  }
  if (lower == "click on" || lower == "click off") {
    partyClick = lower == "click on";
    return lab.note(sv::Note::Info, partyClick ? "a tick on every beat (the board can't hear for ~200 ms after it)" : "no beat tick");
  }
  if (lower == "reset") {
    beats.reset();
    for (uint32_t &b : beatLog) b = 0;
    return lab.note(sv::Note::Info, "tempo forgotten: listening again");
  }
  Serial.printf("  %lu beats, %d BPM, sens x%.1f\n", static_cast<unsigned long>(beats.beats()), beats.bpm(), beats.ratio);
}

void cmdSound(SoundLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("sound | sound on | sound off");
  soundOn = lower.isEmpty() ? !soundOn : lower == "on";
  Preferences p;
  p.begin("games", false);
  p.putBool("sound", soundOn);
  p.end();
  lab.note(sv::Note::Info, soundOn ? "game sounds on" : "game sounds off");
}

const SoundCommand kCommands[] = {
    {"knock", "[set|clear|show|play]", "set = knock a new secret; show = secret and last try; play = hear it", cmdKnock},
    {"tol", "<percent>", "knock lock: how far each gap may be off (default 12)", cmdTol},
    {"bird", "[reset]", "Clap Bird: a new round (reset = forget the best score)", cmdBird},
    {"party", "[sens <x>|click on|off|reset]", "beat party: sensitivity 1.2-3.0 (default 1.6), a tick per beat, forget the tempo", cmdParty},
    {"sound", "[on|off]", "the game sounds on the speaker (saved)", cmdSound},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(SoundLab &) {
  pinMode(PIN_LED, OUTPUT);
  // Lesson 02's safety net: the weakest drive (~5 mA), in case the LED has no resistor.
  gpio_set_drive_capability(static_cast<gpio_num_t>(PIN_LED), GPIO_DRIVE_CAP_0);
  digitalWrite(PIN_LED, LOW);
  ears.config.maxClapMs = 200;  // a knock on a wooden table rings a little longer than a clap
  loadAll();
  if (lock.secret.count >= 2) printRhythm("secret", lock.secret);
  Serial.printf("  Clap Bird best: %d\n", best);
}

void stepBird() {
  const uint32_t now = millis();
  stepAcc += std::min(0.1f, (now - lastStep) / 1000.0f);  // after a pause, don't jump ahead
  lastStep = now;
  while (stepAcc >= 1 / 60.0f) game.step(1 / 60.0f), stepAcc -= 1 / 60.0f;
}

void tick(SoundLab &lab) {
  const uint32_t now = millis();
  // Clap Bird runs only while you watch it.
  if (screen(lab) == Screen::Bird) stepBird();
  else lastStep = now;
  if (game.state() == bird::Game::State::Over && lastState != bird::Game::State::Over) {
    overAt = now;
    if (game.score() > best) {
      best = game.score();
      saveBest();
      sfx(lab, Sfx::Best);
      lab.note(sv::Note::Good, "new best: %d", best);
    } else {
      sfx(lab, Sfx::Crash);
      lab.note(sv::Note::Info, "score %d", game.score());
    }
  }
  // A ding for every pipe passed (the score only ever grows by one per step).
  static int lastScore = 0;
  if (game.score() > lastScore && game.state() != bird::Game::State::Over) sfx(lab, Sfx::Point);
  lastScore = game.score();
  lastState = game.state();

  // The lock's bolt closes again after 5 s.
  const bool open = now < lock.openUntil;
  if (!open && lock.openUntil) {
    lock.openUntil = 0;
    digitalWrite(PIN_LED, LOW);
  }

  // The RGB LED: green while open, the beat's colour in the party, else the shell's level colour.
  static bool claimed = false;
  static uint32_t shown = 0xFFFFFFFF;
  const bool party = screen(lab) == Screen::Party;
  const bool want = open || party;
  if (want != claimed) lab.claimLed(claimed = want), shown = 0xFFFFFFFF;
  if (!want) return;
  uint8_t r = 0, g = 0, b = 0;
  if (open) g = 110, b = 20;
  else if (lastBeat && now - lastBeat < 300) {
    // Bright on the beat, fading in 4 steps (each step one WS2812 write, not one per frame).
    const uint8_t v = uint8_t(160 - (now - lastBeat) / 75 * 40);
    const uint16_t c = hsv(hue, 255, v);
    r = uint8_t((c >> 11) << 3), g = uint8_t(((c >> 5) & 63) << 2), b = uint8_t((c & 31) << 3);
  }
  const uint32_t colour = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
  if (colour == shown) return;
  shown = colour;
  rgbLedWrite(PIN_RGB, r, g, b);
}

const SoundLabSpec kSpec{
    "sound-lab-s6", "S6", "Sound games", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, frame, tick, nullptr, "knock show",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
