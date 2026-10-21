// Sound lab S8 Voice FX.  uv run pio run -d labs/sound-lab-s8 -e sound-lab-s8 -t upload
// Microphone in, speaker out. Say something and the parrot says it back: as a chipmunk, a giant, a
// robot, with an echo, or backwards. Then the live effects: an echo you talk into, a robot voice.
//
//   parrot   voice-activated like S4 (snd::Trigger, with 0.3 s of pre-roll from the mic's ring),
//            up to 6 s, then played straight back through the chosen effect. While it talks the
//            parrot doesn't listen: the shell's echo guard holds back frames while the speaker is
//            loud, and the trigger is only re-armed 300 ms after the speaker went quiet. Otherwise
//            it would hear itself and repeat itself forever.
//   echo     live: mic -> delay line -> speaker. Latency = how long the sound waits in buffers: our
//            processing step (~one screen frame) + the speaker's queue + its 16 ms DMA block.
//   robot    live: mic -> ring modulator + bitcrush -> speaker.
//   scope    what goes in and what comes out, and the chain between them.
//
// The live modes read the microphone's ring by sample number in tick(), not through frame(), so the
// echo guard (which would mute them, the speaker being loud) doesn't get in the way and the spec
// keeps hearSpeaker = false: the parrot stays deaf to its own voice.
//
// Feedback: the mic hears the speaker. If the sound comes back louder than it went out (loop gain
// above 1), it goes round and round: the howl. The guard (fx.h HowlDetector) cuts the live effect
// when the level keeps climbing or stays very loud. Keep the speaker away from the microphone,
// pointing elsewhere, and the volume low (`vol 25`).
//
// Wiring: an INMP441 (much clearer than the KY-037 here), the MAX98357A + speaker, the screen.
//   INMP441: VDD -> 3V3, GND, L/R -> GND, SCK -> 40, WS -> 41, SD -> 42
//   MAX98357A: VIN -> 5V, GND, BCLK -> 39, LRC -> 38, DIN -> 47, speaker on its terminal
//
// Views: parrot · echo · robot · scope   (click = next view, double-click = the action in the footer,
//        long press = record by hand / stop)
#include <esp_heap_caps.h>

#include "fx.h"
#include "sound_lab.h"
#include "trigger.h"
#include "wav.h"

namespace {

constexpr uint32_t kRate = 16000;  // the speaker's rate; the parrot adapts if the mic's differs
constexpr uint32_t kMaxRec = kRate * 6;
constexpr uint32_t kTail = kRate * 3 / 2;  // room for the echo effect's tail
constexpr uint32_t kMaxDelay = kRate / 2;  // 500 ms
constexpr float kLiveGain = 4;             // voices are quiet: +12 dB before the speaker's volume

// ---- State ------------------------------------------------------------------------------------

enum class Effect : uint8_t { Normal, Chipmunk, Giant, Robot, Echo, Reverse };
const char *const kEffectNames[] = {"normal", "chipmunk", "giant", "robot", "echo", "reverse"};
const char *const kEffectWhat[] = {"just as you said it", "1.5x faster = higher", "0.7x slower = deeper", "ring modulator + bitcrush",
                                   "220 ms echo, 45 % feedback", "backwards"};
constexpr int kEffects = 6;

enum class Parrot : uint8_t { Listening, Recording, Talking, Resting, Off };
enum class Live : uint8_t { None, Echo, Robot };

struct State {
  Effect effect = Effect::Chipmunk;
  Parrot parrot = Parrot::Listening;
  Live live = Live::None;
  bool parrotOn = true;
  // The recording.
  int16_t *rec = nullptr, *out = nullptr;
  uint32_t recLen = 0, outLen = 0, copied = 0, recFrom = 0;
  uint32_t restUntil = 0, talks = 0;
  // Live.
  uint32_t liveFrom = 0, lastTick = 0;
  float tickMs = 40;
  uint16_t delayMs = 250;
  uint8_t feedbackPct = 40;
  float carrierHz = 50;
  float inDb = -96, outDb = -96;
  uint32_t howlUntil = 0;
  // The scope's last blocks.
  int16_t scopeIn[320] = {}, scopeOut[320] = {};
} st;

snd::Trigger trig(kRate);
fx::DelayLine delayLine;
fx::RingMod ringMod;
fx::Bitcrush crush;
fx::HowlDetector howl;
int16_t *delayBuf = nullptr;

// ---- The parrot -------------------------------------------------------------------------------

void listen(SoundLab &lab) {
  st.parrot = st.parrotOn && st.live == Live::None ? Parrot::Listening : Parrot::Off;
  trig.arm(st.parrot == Parrot::Listening);
  lab.setRecording(false);
}

// Copy what the mic has heard so far into the recording.
void copyUpTo(SoundLab &lab, uint32_t to) {
  while (st.copied < to && st.recLen < kMaxRec) {
    const uint32_t n = std::min<uint32_t>({to - st.copied, kMaxRec - st.recLen, 1024});
    if (!lab.mic().copy(st.copied, st.rec + st.recLen, n)) {
      st.copied = to;  // fell more than 4 s behind: keep what we have
      return;
    }
    st.copied += n, st.recLen += n;
  }
}

// Turn the recording into the effect's sound and play it.
void talk(SoundLab &lab) {
  if (st.recLen < kRate / 10) return listen(lab);
  const float gain = std::min(snd::gainFor(fx::peakOf(st.rec, st.recLen)), 24.0f);
  // A mic running at another rate plays back at the speaker's 16 kHz faster or slower: correct for it.
  const float rateFix = float(lab.mic().rate()) / kRate;
  float step = rateFix;
  const int16_t *src = st.rec;
  uint32_t n = st.recLen;
  switch (st.effect) {
    case Effect::Chipmunk: step *= 1.5f; break;
    case Effect::Giant: step *= 0.7f; break;
    case Effect::Robot:
      ringMod.set(st.carrierHz, kRate);
      crush.set(6, 2);
      for (uint32_t i = 0; i < n; i++) st.out[i] = crush.process(ringMod.process(st.rec[i]));
      src = st.out;
      break;
    case Effect::Echo: {
      fx::DelayLine d;
      d.begin(delayBuf, kMaxDelay);
      d.set(kRate * 220 / 1000, 0.45f);
      const uint32_t total = std::min(n + kTail, kMaxRec + kTail);
      for (uint32_t i = 0; i < total; i++) {
        const int16_t x = i < n ? st.rec[i] : 0;
        st.out[i] = fx::clamp16(x + 0.7f * d.process(x));
      }
      src = st.out, n = total;
      delayLine.clear();  // the live echo shares the buffer: start it clean
      break;
    }
    case Effect::Reverse:
      std::copy(st.rec, st.rec + n, st.out);
      fx::reverse(st.out, n);
      src = st.out;
      break;
    case Effect::Normal: break;
  }
  st.outLen = n;
  trig.arm(false);  // no listening while talking
  lab.speaker().play(src, n, step, gain);
  st.parrot = Parrot::Talking;
  st.talks++;
  lab.note(sv::Note::Good, "%s, %.1f s, x%.0f louder", kEffectNames[int(st.effect)], st.recLen / float(lab.mic().rate()), gain);
}

void handle(SoundLab &lab, const snd::Trigger::Result &r) {
  switch (r.action) {
    case snd::Trigger::Action::Start:
      st.parrot = Parrot::Recording;
      st.recLen = 0;
      st.copied = st.recFrom = r.from;
      lab.setRecording(true);  // the REC dot and red LED: nothing records without them
      break;
    case snd::Trigger::Action::Save:
      copyUpTo(lab, r.to);
      lab.setRecording(false);
      talk(lab);
      break;
    case snd::Trigger::Action::Drop:
      lab.note(sv::Note::Info, "too short: a bang, not a word");
      listen(lab);
      break;
    case snd::Trigger::Action::None: break;
  }
}

// ---- Live effects -----------------------------------------------------------------------------

void setLive(SoundLab &lab, Live mode) {
  if (mode != Live::None && lab.mic().rate() != kRate) return lab.warn("live effects need the mic at 16000 Hz (rate 16000)");
  st.live = mode;
  lab.speaker().stop();
  if (mode == Live::None) {
    st.restUntil = millis() + 300;
    st.parrot = Parrot::Resting;
    return lab.note(sv::Note::Info, "live off");
  }
  trig.arm(false);
  lab.setRecording(false);
  st.parrot = Parrot::Off;
  delayLine.clear();
  delayLine.set(kRate * st.delayMs / 1000, st.feedbackPct / 100.0f);
  ringMod.set(st.carrierHz, kRate);
  crush.set(8, 1);
  howl.reset();
  st.liveFrom = lab.mic().written();
  lab.note(sv::Note::Warn, "live %s: speaker away from the mic, volume low", mode == Live::Echo ? "echo" : "robot");
}

float blockDb(const int16_t *s, uint32_t n) {
  float sum = 0;
  for (uint32_t i = 0; i < n; i++) sum += float(s[i]) * s[i];
  return snd::toDb(sqrtf(sum / std::max<uint32_t>(n, 1)));
}

void runLive(SoundLab &lab) {
  snd::Mic &m = lab.mic();
  const uint32_t w = m.written();
  // More than 1/4 s behind (a slow frame)? Skip ahead: low latency matters more than every sample.
  if (w - st.liveFrom > kRate / 4) st.liveFrom = w - 256;
  static int16_t in[512], out[512];
  while (st.liveFrom < w) {
    const uint32_t n = std::min<uint32_t>(512, w - st.liveFrom);
    if (!m.copy(st.liveFrom, in, n)) {
      st.liveFrom = w;
      break;
    }
    st.liveFrom += n;
    for (uint32_t i = 0; i < n; i++) {
      const int16_t x = fx::clamp16(in[i] * kLiveGain);
      out[i] = st.live == Live::Echo ? delayLine.process(x) : crush.process(ringMod.process(x));
    }
    lab.speaker().stream(out, n);
    st.inDb = blockDb(in, n), st.outDb = blockDb(out, n);
    std::copy(in + n - std::min<uint32_t>(n, 320), in + n, st.scopeIn);
    std::copy(out + n - std::min<uint32_t>(n, 320), out + n, st.scopeOut);
    if (howl.push(st.inDb, n * 1000 / kRate)) {
      st.howlUntil = millis() + 5000;
      lab.note(sv::Note::Bad, "Howl! Move the speaker away or turn it down");
      lab.chime(Chime::No);
      setLive(lab, Live::None);
      return;
    }
  }
}

// ---- Drawing ----------------------------------------------------------------------------------

constexpr uint16_t kGreen = rgb(60, 200, 110), kGreenDark = rgb(30, 130, 70), kRed = rgb(235, 70, 60), kBeak = rgb(255, 196, 60);

// The parrot: body, wing, head, eye, beak, crest, on a perch. `open` 0-1 opens the beak.
void drawParrot(FrameBuffer &fb, int cx, int cy, float t, float open, Parrot mood) {
  const int bob = mood == Parrot::Listening ? int(2 * sinf(t * 2.5f)) : mood == Parrot::Talking ? int(2 * sinf(t * 14)) : 0;
  fb.fillRoundRect(cx - 50, cy + 52, 100, 6, 3, rgb(120, 80, 50));  // perch
  fb.fillTriangle(cx - 6, cy + 40, cx + 6, cy + 40, cx - 2, cy + 70, kRed);  // tail
  fb.fillCircle(cx, cy + 22 + bob, 24, kGreen);                                // body
  fb.fillTriangle(cx - 22, cy + 10 + bob, cx - 4, cy + 18 + bob, cx - 18, cy + 46 + bob, kGreenDark);  // wing
  fb.fillCircle(cx + 8, cy - 8 + bob, 18, kGreen);                           // head
  for (int i = 0; i < 3; i++) fb.fillTriangle(cx + 2 + i * 6, cy - 24 + bob, cx + 8 + i * 6, cy - 24 + bob, cx + i * 7, cy - 38 + bob - i * 2, kRed);  // crest
  // The eye: wide while recording, blinking now and then while listening.
  const bool blink = mood == Parrot::Listening && fmodf(t, 3.7f) < 0.12f;
  const int eyeR = mood == Parrot::Recording ? 7 : 5;
  if (blink) fb.drawFastHLine(cx + 9, cy - 12 + bob, 10, pal::night);
  else {
    fb.fillCircle(cx + 14, cy - 12 + bob, eyeR, pal::ink);
    fb.fillCircle(cx + 15 + (mood == Parrot::Listening ? int(2 * sinf(t)) : 0), cy - 12 + bob, eyeR / 2, pal::night);
  }
  // The beak: upper half fixed, lower half drops with `open`.
  const int bx = cx + 24, by = cy - 4 + bob, drop = int(open * 12);
  fb.fillTriangle(bx, by - 8, bx, by, bx + 16, by + 2, kBeak);
  fb.fillTriangle(bx, by + 1 + drop / 2, bx, by + 6 + drop, bx + 11, by + 3 + drop, mix(kBeak, pal::night, 60));
  fb.fillCircle(cx - 6, cy + 54, 3, kBeak), fb.fillCircle(cx + 6, cy + 54, 3, kBeak);  // feet
}

void drawParrotView(SoundLab &lab, FrameBuffer &fb, float t) {
  const Parrot mood = st.parrot;
  const float open = mood == Parrot::Talking ? std::min(1.0f, lab.speaker().outputLevel() * 2.5f) : 0;
  drawParrot(fb, 70, sv::kTop + 40, t, open, mood);
  // Sound arriving while it listens: arcs from the right.
  if (mood == Parrot::Listening)
    for (int i = 0; i < 3; i++) {
      const float k = fmodf(t * 0.8f + i / 3.0f, 1.0f);
      fb.drawCircleHelper(150, sv::kTop + 30, 8 + int((1 - k) * 30), 0x8 | 0x1, mix(sv::kSound, pal::night, uint8_t(k * 220)));
    }
  // The speech bubble.
  const int bx = 150, by = sv::kTop + 6, bw = 162, bh = 58;
  fb.fillRoundRect(bx, by, bw, bh, 10, sv::kPanel);
  fb.fillTriangle(bx, by + 30, bx, by + 44, bx - 14, by + 42, sv::kPanel);
  const char *line1 = mood == Parrot::Listening ? "Say something!"
                      : mood == Parrot::Recording ? "I'm listening..."
                      : mood == Parrot::Talking   ? "Squawk!"
                      : mood == Parrot::Off       ? (st.live == Live::None ? "Resting (parrot on)" : "Busy: live effect on")
                                                  : "...";
  sv::bold(fb, bx + 8, by + 22, mood == Parrot::Recording ? sv::kRec : pal::ink, line1);
  char text[48];
  snprintf(text, sizeof(text), "voice: %s", kEffectNames[int(st.effect)]);
  sv::small(fb, bx + 8, by + 34, sv::kSound, text);
  sv::small(fb, bx + 8, by + 45, pal::muted, kEffectWhat[int(st.effect)]);
  if (mood == Parrot::Recording) {
    const float secs = (lab.mic().written() - st.recFrom) / float(lab.mic().rate());
    snprintf(text, sizeof(text), "REC %.1f s", secs);
    sv::bold(fb, 150, sv::kTop + 88, sv::kRec, text);
  } else if (mood == Parrot::Talking) {
    fb.drawRect(150, sv::kTop + 80, 160, 8, pal::dim);
    fb.fillRect(151, sv::kTop + 81, int(lab.speaker().progress() * 158), 6, sv::kSound);
  }
  // The level against the trigger line.
  const float above = lab.level().above();
  sv::meter(fb, 150, sv::kTop + 100, 160, 10, lab.level(), 50, trig.config.startDb);
  snprintf(text, sizeof(text), "%.0f dB, starts at %.0f", above, trig.config.startDb);
  sv::small(fb, 150, sv::kTop + 114, pal::muted, text);
}

void drawEcho(SoundLab &lab, FrameBuffer &fb, float t) {
  const bool on = st.live == Live::Echo;
  char text[64];
  sv::bold(fb, 10, sv::kTop + 20, on ? pal::teal : pal::muted, on ? "Echo ON" : "Echo off");
  snprintf(text, sizeof(text), "delay %u ms - feedback %u %%", st.delayMs, st.feedbackPct);
  sv::small(fb, 110, sv::kTop + 10, pal::ink, text);
  // The taps: what one clap turns into. Each repeat feedback x quieter, delay apart.
  const int x0 = 14, y0 = sv::kTop + 34, h = 54, w = 292;
  fb.drawFastHLine(x0, y0 + h, w, pal::dim);
  fb.fillRect(x0, y0, 3, h, pal::ink);  // the clap itself (in the room)
  float a = 1;
  for (int k = 1; k < 12; k++) {
    const int x = x0 + k * st.delayMs * w / 2000;
    if (x > x0 + w) break;
    const int hh = int(h * a);
    if (hh > 0) fb.fillRect(x, y0 + h - hh, 3, hh, sv::kSound);
    a *= st.feedbackPct / 100.0f;
  }
  sv::small(fb, x0, y0 + h + 3, pal::muted, "one clap -> its echoes over 2 s");
  // Meters and latency.
  const float latency = st.tickMs + lab.speaker().streamQueued() * 1000.0f / kRate + 16;
  snprintf(text, sizeof(text), "in %.0f dBFS  out %.0f dBFS  latency ~%.0f ms", st.inDb, st.outDb, latency);
  sv::small(fb, 10, sv::kTop + 104, on ? pal::ink : pal::muted, text);
  if (millis() < st.howlUntil && fmodf(t, 0.5f) < 0.35f) sv::bold(fb, 10, sv::kTop + 126, pal::coral, "Howl! Move the speaker away");
  else sv::small(fb, 10, sv::kTop + 120, pal::muted, "speaker away from the mic, vol low");
}

void drawRobot(SoundLab &lab, FrameBuffer &fb, float t) {
  const bool on = st.live == Live::Robot;
  // A robot head; its mouth lights with what comes out of the speaker.
  const int cx = 80, cy = sv::kTop + 62;
  fb.fillRoundRect(cx - 44, cy - 44, 88, 84, 10, rgb(110, 130, 150));
  fb.drawFastVLine(cx, cy - 58, 14, pal::muted);
  fb.fillCircle(cx, cy - 60, 4, on && fmodf(t, 1.0f) < 0.5f ? pal::coral : pal::dim);
  fb.fillCircle(cx - 18, cy - 14, 9, pal::night), fb.fillCircle(cx + 18, cy - 14, 9, pal::night);
  fb.fillCircle(cx - 18, cy - 14, 4, on ? pal::teal : pal::dim), fb.fillCircle(cx + 18, cy - 14, 4, on ? pal::teal : pal::dim);
  const float lvl = on ? lab.speaker().outputLevel() : 0;
  for (int i = 0; i < 7; i++) {
    const int hh = 4 + int(lvl * 18 * (0.6f + 0.4f * sinf(i * 1.7f + t * 20)));
    fb.fillRect(cx - 30 + i * 9, cy + 18 - hh / 2, 6, std::max(2, hh), on ? sv::kSound : pal::dim);
  }
  char text[48];
  sv::bold(fb, 150, sv::kTop + 20, on ? pal::teal : pal::muted, on ? "Robot ON" : "Robot off");
  snprintf(text, sizeof(text), "carrier %.0f Hz", st.carrierHz);
  sv::small(fb, 150, sv::kTop + 36, pal::ink, text);
  sv::small(fb, 150, sv::kTop + 52, pal::muted, "voice x a slow sine:");
  sv::small(fb, 150, sv::kTop + 63, pal::muted, "every f becomes f-c, f+c");
  sv::small(fb, 150, sv::kTop + 74, pal::muted, "+ 8-bit crunch");
  if (millis() < st.howlUntil) sv::bold(fb, 150, sv::kTop + 100, pal::coral, "Howl! Stopped");
  else sv::small(fb, 150, sv::kTop + 100, pal::muted, "speaker away from the mic");
}

void drawScope(SoundLab &lab, FrameBuffer &fb, float) {
  // Input on top, output below, the chain on the left.
  static int16_t in[320], out[320];
  if (st.live == Live::None) {
    lab.mic().latest(in, 320);
    // Talking: the part of the effect's sound the speaker is on now.
    if (st.parrot == Parrot::Talking && st.outLen) {
      const int16_t *src = (st.effect == Effect::Robot || st.effect == Effect::Echo || st.effect == Effect::Reverse) ? st.out : st.rec;
      const uint32_t n = (st.effect == Effect::Robot || st.effect == Effect::Echo || st.effect == Effect::Reverse) ? st.outLen : st.recLen;
      const uint32_t at = std::min<uint32_t>(uint32_t(lab.speaker().progress() * n), n > 320 ? n - 320 : 0);
      std::copy(src + at, src + std::min(n, at + 320), out);
    } else std::fill_n(out, 320, int16_t(0));
  } else {
    std::copy(st.scopeIn, st.scopeIn + 320, in);
    std::copy(st.scopeOut, st.scopeOut + 320, out);
  }
  const bool active = st.live != Live::None || st.parrot == Parrot::Talking || st.parrot == Parrot::Recording;
  const char *mid = st.live == Live::Echo ? "delay" : st.live == Live::Robot ? "ring mod" : kEffectNames[int(st.effect)];
  const char *boxes[3] = {"mic", mid, "speaker"};
  for (int i = 0; i < 3; i++) {
    const int y = sv::kTop + 8 + i * 42;
    fb.fillRoundRect(6, y, 76, 26, 6, active ? mix(sv::kPanel, sv::kSound, 70) : sv::kPanel);
    sv::small(fb, 12, y + 10, active ? pal::ink : pal::muted, boxes[i]);
    if (i < 2) fb.fillTriangle(40, y + 30, 48, y + 30, 44, y + 38, active ? sv::kSound : pal::dim);
  }
  sv::small(fb, 92, sv::kTop + 4, pal::muted, "in");
  sv::wave(fb, 92, sv::kTop + 14, 222, 50, in, 320, sv::autoScale(in, 320, 512), pal::teal);
  sv::small(fb, 92, sv::kTop + 68, pal::muted, "out");
  sv::wave(fb, 92, sv::kTop + 78, 222, 50, out, 320, sv::autoScale(out, 320, 512), sv::kSound);
}

const SoundView kViews[] = {
    {"parrot", drawParrotView, "fx next"},
    {"echo", drawEcho, "echo"},
    {"robot", drawRobot, "robot"},
    {"scope", drawScope, "again"},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdFx(SoundLab &lab, const String &, const String &lower) {
  if (lower == "next") st.effect = Effect((int(st.effect) + 1) % kEffects);
  else {
    int found = -1;
    for (int i = 0; i < kEffects; i++)
      if (lower == kEffectNames[i]) found = i;
    if (found < 0) return lab.warn("fx normal | chipmunk | giant | robot | echo | reverse | next");
    st.effect = Effect(found);
  }
  lab.note(sv::Note::Info, "voice: %s (%s)", kEffectNames[int(st.effect)], kEffectWhat[int(st.effect)]);
}

void cmdAgain(SoundLab &lab, const String &, const String &) {
  if (st.recLen == 0) return lab.warn("nothing recorded yet: say something first");
  if (st.parrot == Parrot::Recording || st.live != Live::None) return lab.warn("busy");
  talk(lab);
}

void cmdEcho(SoundLab &lab, const String &, const String &lower) {
  const bool on = lower.isEmpty() ? st.live != Live::Echo : lower == "on";
  setLive(lab, on ? Live::Echo : Live::None);
  lab.showView("echo");
}

void cmdRobot(SoundLab &lab, const String &, const String &lower) {
  const bool on = lower.isEmpty() ? st.live != Live::Robot : lower == "on";
  setLive(lab, on ? Live::Robot : Live::None);
  lab.showView("robot");
}

void cmdDelay(SoundLab &lab, const String &arg, const String &) {
  long ms = 0;
  if (!SoundLab::number(arg, 20, 500, ms)) return lab.warn("delay <20-500 ms> (now %u)", st.delayMs);
  st.delayMs = uint16_t(ms);
  delayLine.set(kRate * st.delayMs / 1000, st.feedbackPct / 100.0f);
}

void cmdFeedback(SoundLab &lab, const String &arg, const String &) {
  long pct = 0;
  if (!SoundLab::number(arg, 0, 70, pct)) return lab.warn("feedback <0-70 %%> (now %u): how much of each echo echoes again", st.feedbackPct);
  st.feedbackPct = uint8_t(pct);
  delayLine.set(kRate * st.delayMs / 1000, st.feedbackPct / 100.0f);
}

void cmdCarrier(SoundLab &lab, const String &arg, const String &) {
  float hz = 0;
  if (!SoundLab::decimal(arg, 10, 300, hz)) return lab.warn("carrier <10-300 Hz> (now %.0f): 30-80 = robot, 150+ = bell-like", st.carrierHz);
  st.carrierHz = hz;
  ringMod.set(hz, kRate);
}

void cmdTrigger(SoundLab &lab, const String &arg, const String &) {
  float db = 0;
  if (!SoundLab::decimal(arg, 4, 40, db)) return lab.warn("trigger <dB above the quiet> 4-40 (now %.0f)", trig.config.startDb);
  trig.config.startDb = db;
}

void cmdParrot(SoundLab &lab, const String &, const String &lower) {
  st.parrotOn = lower.isEmpty() ? !st.parrotOn : lower == "on";
  if (st.parrot != Parrot::Talking) listen(lab);
  lab.note(sv::Note::Info, st.parrotOn ? "the parrot listens" : "the parrot rests");
}

// Long press: start or stop a recording by hand.
void cmdRec(SoundLab &lab, const String &, const String &) {
  if (st.live != Live::None || st.parrot == Parrot::Talking) return lab.warn("busy");
  if (st.parrot == Parrot::Recording) return handle(lab, trig.stop(lab.mic().written()));
  handle(lab, trig.start(lab.mic().written()));
}

const SoundCommand kCommands[] = {
    {"fx", "<name|next>", "the parrot's voice: normal chipmunk giant robot echo reverse", cmdFx},
    {"again", "", "say the last recording again, with the current voice", cmdAgain},
    {"rec", "", "record by hand / stop (long press)", cmdRec},
    {"parrot", "[on|off]", "let the parrot listen, or rest", cmdParrot},
    {"echo", "[on|off]", "live echo through the speaker (careful: feedback)", cmdEcho},
    {"delay", "<ms>", "echo delay, 20-500 (default 250)", cmdDelay},
    {"feedback", "<percent>", "echo feedback, 0-70 (default 40)", cmdFeedback},
    {"robot", "[on|off]", "live robot voice through the speaker", cmdRobot},
    {"carrier", "<Hz>", "the robot's carrier, 10-300 (default 50)", cmdCarrier},
    {"trigger", "<dB>", "how loud starts a recording (default 12)", cmdTrigger},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(SoundLab &lab) {
  st.rec = static_cast<int16_t *>(heap_caps_malloc(kMaxRec * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  st.out = static_cast<int16_t *>(heap_caps_malloc((kMaxRec + kTail) * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  delayBuf = static_cast<int16_t *>(heap_caps_calloc(kMaxDelay, sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (!st.rec || !st.out || !delayBuf) {
    log_e("S8: no PSRAM for the buffers");
    st.parrotOn = false;
  }
  delayLine.begin(delayBuf, kMaxDelay);
  trig.setRate(lab.mic().rate());
  trig.config.startDb = 12;
  trig.config.holdMs = 700;
  trig.config.prerollMs = 300;
  trig.config.maxMs = 6000;
  trig.config.minMs = 300;
  listen(lab);
  if (!lab.mic().digital()) lab.note(sv::Note::Warn, "KY-037: it works, but an INMP441 sounds much clearer");
}

void frame(SoundLab &lab, const snd::Frame &f, uint32_t) {
  if (st.parrot != Parrot::Listening && st.parrot != Parrot::Recording) return;
  if (lab.meter().learning()) return;
  handle(lab, trig.push(lab.level().above(), f.end));
}

void tick(SoundLab &lab) {
  const uint32_t now = millis();
  if (st.lastTick) st.tickMs += (float(now - st.lastTick) - st.tickMs) * 0.1f;
  st.lastTick = now;
  if (st.live != Live::None) return runLive(lab);
  switch (st.parrot) {
    case Parrot::Recording:
      copyUpTo(lab, lab.mic().written());
      if (st.recLen >= kMaxRec) handle(lab, trig.stop(lab.mic().written()));
      break;
    case Parrot::Talking:
      if (!lab.speaker().busy()) st.parrot = Parrot::Resting, st.restUntil = now + 300;
      break;
    case Parrot::Resting:
      // 300 ms after the speaker went quiet: the room has stopped ringing.
      if (now >= st.restUntil && !lab.speaker().recentlyLoud(300)) listen(lab);
      break;
    default: break;
  }
}

const SoundLabSpec kSpec{
    "sound-lab-s8", "S8", "Voice FX", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, frame, tick, nullptr, "rec",
};

SoundLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
