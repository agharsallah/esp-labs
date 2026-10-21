// Bluetooth lab B7 Music.  uv run pio run -d labs/ble-lab-b7 -e ble-lab-b7 -t upload
// The board becomes a Bluetooth MIDI instrument. MIDI carries notes, not sound: GarageBand on a
// Mac/iPhone/iPad, any synth app on Android, or the ble-app Music tab in Chrome makes the sound
// (and can send it on to your JBL speaker). Play with the button, five touch wires, serial
// commands, a drum machine; notes played on the other side light the board's piano and LED.
//
// Wiring: the screen from lesson 03. Optional: the push button on GPIO 5 (does what BOOT does),
// and jumper wires on GPIO 1, 2, 6, 13, 14 as five touch keys (`touch on`). 13 and 14 are the
// RC522's RST/IRQ wires from the RFID track: unplug the reader first, or use the other three.
//
// Views: keys · sequencer · roll · link · log
//   keys, sequencer, roll: the button plays. click = next note (sequencer: next beat),
//   double-click = chord (sequencer: start/stop), hold = all notes off, hold 2 s = next view.
//   link, log: click = next view.
// Serial: help lists the commands. Shared code: kit/lib/ble-kit (midi_ble.h = the packet format).
#include "b7_views.h"
#include "ble_lab.h"
#include "midi_ble.h"
#include "midi_service.h"
#include "music.h"
#include "server_link.h"
#include "touch_pads.h"

namespace {

ble::ServerLink links;
b7::MidiService midiOut;
b7::Scheduler sched;
b7::Screen screen;
constexpr uint8_t kPadPins[5] = {1, 2, 6, 13, 14};
TouchPads<5> pads(kPadPins);
uint8_t padNote[5] = {};
int walk = 0;               // the button's position in the scale
uint32_t nextStepAt = 0;    // the drum machine's next sixteenth
uint32_t tuneEndsAt = 0, tuneStartedAt = 0;
uint32_t lastInAt = 0;      // last note from the synth side (LED)
uint32_t connectedAt = 0;   // to notice a new connection
bool warnedDeaf = false;
uint32_t holdSince = 0;
uint8_t holdPin = 0;

constexpr int kSeqView = 1, kLinkView = 3;

uint32_t beatMs() { return 60000u / uint32_t(screen.bpm); }

void note(uint8_t n, uint8_t vel, uint32_t lengthMs, uint32_t startIn = 0, int channel = -1) {
  const uint8_t ch = uint8_t(channel < 0 ? screen.channel : channel);
  const uint32_t at = millis() + startIn;
  sched.at(at, midi::noteOn(ch, n, vel, 0));
  sched.at(at + lengthMs, midi::noteOff(ch, n, 0));
}

void panic() {
  sched.clear();
  screen.seqOn = false;
  screen.tune = nullptr;
  const uint16_t ts = millis() & 0x1FFF;
  const midi::Msg off[2] = {midi::allNotesOff(uint8_t(screen.channel), ts), midi::allNotesOff(b7::kDrumChannel, ts)};
  midiOut.send(off, 2);
  memset(screen.outBits, 0, sizeof(screen.outBits));
  for (b7::RollNote &r : screen.roll)
    if (!r.incoming && !r.off) r.off = millis();
  BLE_NOTE(Info, "all notes off");
}

void chord() {
  const int count = b7::scaleInfo(screen.scale).count;
  const int root = walk % count;
  for (int k = 0; k < 3; k++) note(b7::degreeNote(screen.scale, screen.octave, root + k * 2), 96, 600);
  BLE_NOTE(Tx, "chord on degree %d", root + 1);
}

void stepNote() {
  const int count = b7::scaleInfo(screen.scale).count;
  walk = (walk + 1) % (count * 2);  // up two octaves, then start again
  note(b7::degreeNote(screen.scale, screen.octave, walk), 100, 300);
}

// Send everything that's due in one go: a run of notes leaves in as few packets as fit.
void flush() {
  midi::Msg due[24];
  const int n = sched.due(millis(), due, 24);
  if (!n) return;
  for (int i = 0; i < n; i++) {
    const midi::Msg &m = due[i];
    if (midi::isNoteOn(m)) screen.setNote(m.d1, false, true);
    else if (midi::isNoteOff(m)) screen.setNote(m.d1, false, false);
  }
  if (!midiOut.send(due, size_t(n)) && !warnedDeaf) {
    warnedDeaf = true;
    BLE_NOTE(Warn, "no synth listening: notes stay on the screen");
  }
}

// ---- Views ------------------------------------------------------------------------------------

void drawKeys(BleLab &, FrameBuffer &fb, float t) { b7v::keys(fb, screen, t); }
void drawSeq(BleLab &, FrameBuffer &fb, float t) { b7v::sequencer(fb, screen, t); }
void drawRoll(BleLab &, FrameBuffer &fb, float t) { b7v::roll(fb, screen, t); }
void drawLink(BleLab &lab, FrameBuffer &fb, float t) { b7v::link(fb, lab.state(), screen, t); }
void drawLog(BleLab &, FrameBuffer &fb, float) { bv::log(fb); }

const BleView kViews[] = {
    {"keys", drawKeys, "chord"},
    {"sequencer", drawSeq, "seq"},
    {"roll", drawRoll, "chord"},
    {"link", drawLink, nullptr},
    {"log", drawLog, nullptr},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdPlay(BleLab &lab, const String &arg, const String &) {
  if (arg.isEmpty()) return lab.warn("play <notes>: play C4 E4 G4 · a chord: C4+E4+G4 · a rest: -");
  uint32_t at = 0;
  int from = 0, count = 0;
  while (from < int(arg.length())) {
    int sp = arg.indexOf(' ', from);
    if (sp < 0) sp = arg.length();
    const String token = arg.substring(from, sp);
    from = sp + 1;
    if (token.isEmpty()) continue;
    if (token != "-") {
      int a = 0;
      while (a < int(token.length())) {
        int plus = token.indexOf('+', a);
        if (plus < 0) plus = token.length();
        const int n = midi::parseNote(token.substring(a, plus).c_str());
        if (n < 0) return lab.warn("'%s' is not a note: C4, F#3, Bb2 (middle C = C4)", token.substring(a, plus).c_str());
        note(uint8_t(n), 100, beatMs() * 9 / 10, at);
        count++;
        a = plus + 1;
      }
    }
    at += beatMs();
  }
  BLE_NOTE(Info, "playing %d notes at %d bpm", count, screen.bpm);
}

void cmdTune(BleLab &lab, const String &, const String &lower) {
  const b7::Tune *tune = b7::findTune(lower);
  if (!tune) return lab.warn("tune twinkle | ode | frere");
  const uint32_t eighth = beatMs() / 2;
  uint32_t at = 0;
  const uint8_t c = uint8_t((screen.octave + 1) * 12);
  for (int i = 0; i < tune->count; i++) {
    const b7::Step &st = tune->steps[i];
    if (st.note >= 0) note(uint8_t(c + st.note), 96, st.eighths * eighth * 9 / 10, at);
    at += st.eighths * eighth;
  }
  screen.tune = tune->name;
  tuneStartedAt = millis();
  tuneEndsAt = millis() + at;
  screen.tuneLen = tune->count;
  BLE_NOTE(Info, "tune %s: %d notes, %.1f s", tune->name, tune->count, at / 1000.0f);
}

void cmdSeq(BleLab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("seq | seq on | seq off");
  screen.seqOn = lower.isEmpty() ? !screen.seqOn : lower == "on";
  if (screen.seqOn && !screen.beat) screen.beat = b7::findBeat("rock");
  if (screen.seqOn) {
    screen.step = 0;
    nextStepAt = millis();
  } else {
    sched.clear(b7::kDrumChannel);
  }
  BLE_NOTE(Info, "drums %s: %s at %d bpm", screen.seqOn ? "on" : "off", screen.beat ? screen.beat->name : "no beat", screen.bpm);
}

void cmdBpm(BleLab &lab, const String &arg, const String &) {
  long n = 0;
  if (!BleLab::number(arg, 40, 220, n)) return lab.warn("bpm <40-220> (now %d)", screen.bpm);
  screen.bpm = int(n);
  BLE_NOTE(Info, "%d bpm: a sixteenth is %lu ms", screen.bpm, static_cast<unsigned long>(15000 / n));
}

void cmdBeat(BleLab &lab, const String &, const String &lower) {
  const b7::Beat *b = b7::findBeat(lower);
  if (!b) return lab.warn("beat rock | funk | house | boom");
  screen.beat = b;
  lab.showView("sequencer");
  BLE_NOTE(Info, "beat %s%s", b->name, screen.seqOn ? "" : " (seq on to start)");
}

void cmdOctave(BleLab &lab, const String &arg, const String &) {
  long n = 0;
  if (!BleLab::number(arg, 1, 7, n)) return lab.warn("octave <1-7> (now %d; 4 = middle C)", screen.octave);
  screen.octave = int(n);
}

void cmdChannel(BleLab &lab, const String &arg, const String &) {
  long n = 0;
  if (!BleLab::number(arg, 1, 16, n)) return lab.warn("channel <1-16> (now %d; 10 = drums)", screen.channel + 1);
  screen.channel = int(n - 1);
}

void cmdScale(BleLab &lab, const String &, const String &lower) {
  if (lower == "pent" || lower == "pentatonic") screen.scale = b7::Scale::Pentatonic;
  else if (lower == "minor") screen.scale = b7::Scale::Minor;
  else if (lower == "major") screen.scale = b7::Scale::Major;
  else if (lower == "blues") screen.scale = b7::Scale::Blues;
  else return lab.warn("scale pent | minor | major | blues   (the button walks it)");
  walk = 0;
  BLE_NOTE(Info, "scale %s", b7::scaleInfo(screen.scale).name);
}

void cmdChord(BleLab &, const String &, const String &) { chord(); }
void cmdStop(BleLab &, const String &, const String &) { panic(); }

void cmdTouch(BleLab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("touch on | touch off   (wires on GPIO 1 2 6 13 14, hands off while it measures)");
  pads.enable(lower == "on");
  screen.touchOn = pads.on();
  if (pads.on()) BLE_NOTE(Good, "touch on: 5 keys, pentatonic from C%d", screen.octave);
  else BLE_NOTE(Info, "touch off");
}

const BleCommand kCommands[] = {
    {"play", "<notes>", "play C4 E4 G4 · chords C4+E4+G4 · rest -", cmdPlay},
    {"tune", "<name>", "twinkle, ode, frere", cmdTune},
    {"seq", "[on|off]", "the drum machine (channel 10)", cmdSeq},
    {"beat", "<name>", "rock, funk, house, boom", cmdBeat},
    {"bpm", "<n>", "tempo, 40-220 (default 100)", cmdBpm},
    {"scale", "<name>", "pent, minor, major, blues: what the button walks", cmdScale},
    {"octave", "<n>", "1-7, 4 = middle C", cmdOctave},
    {"channel", "<n>", "MIDI channel 1-16 (10 = drums)", cmdChannel},
    {"chord", "", "a triad on the current note", cmdChord},
    {"stop", "", "all notes off, drums and tune stop", cmdStop},
    {"touch", "on|off", "five touch wires as keys", cmdTouch},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(BleLab &lab) {
  // Macs and iPhones may ask to pair a MIDI device: allow it, Just Works with Secure Connections.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&links, false);
  midiOut.begin(server);
  server->start();
  midiOut.advertise(lab.spec().deviceName);
}

void tick(BleLab &lab) {
  const uint32_t now = millis();
  const ble::LinkState ls = ble::link().get();
  // A new connection: ask for a short interval, 7.5-15 ms. MIDI wants low latency.
  if (ls.radio == ble::Radio::Connected && ls.connectedAt != connectedAt) {
    connectedAt = ls.connectedAt;
    warnedDeaf = false;
    NimBLEDevice::getServer()->updateConnParams(ls.peer.handle, 6, 12, 0, 200);
    BLE_NOTE(Tx, "asked for a 7.5-15 ms interval (low latency)");
  }

  // The drum machine: one sixteenth per step.
  if (screen.seqOn && screen.beat && int32_t(now - nextStepAt) >= 0) {
    for (int r = 0; r < 4; r++)
      if ((screen.beat->rows[r] >> screen.step) & 1)
        note(b7::kDrumNotes[r], r == 2 ? 70 : 110, 60, 0, b7::kDrumChannel);
    screen.step = (screen.step + 1) % 16;
    nextStepAt += 15000u / uint32_t(screen.bpm);
    if (int32_t(now - nextStepAt) > 200) nextStepAt = now;  // fell behind: don't burst
  }
  if (screen.tune && int32_t(now - tuneEndsAt) > 0) screen.tune = nullptr;

  // Touch keys: down = note on, up = note off. A firmer touch raises the reading more: velocity.
  uint32_t released = 0;
  const uint32_t pressed = pads.poll(released);
  for (int i = 0; i < 5; i++) {
    screen.padDown[i] = pads.on() && pads.down(i);
    screen.padRise[i] = pads.rise(i);
    if ((pressed >> i) & 1) {
      padNote[i] = b7::degreeNote(b7::Scale::Pentatonic, screen.octave, i);
      const uint8_t vel = uint8_t(constrain(50 + pads.rise(i), 50, 127));
      sched.at(now, midi::noteOn(uint8_t(screen.channel), padNote[i], vel, 0));
    }
    if ((released >> i) & 1) sched.at(now, midi::noteOff(uint8_t(screen.channel), padNote[i], 0));
  }

  flush();

  // What the other side plays: the piano strip, the roll, and the LED in the note's colour.
  midi::Msg in[16];
  const int n = midiOut.drain(in, 16);
  for (int i = 0; i < n; i++) {
    const midi::Msg &m = in[i];
    if (midi::isNoteOn(m)) {
      screen.setNote(m.d1, true, true);
      lab.claimLed(true);
      const uint16_t c = hsv(uint8_t((m.d1 % 12) * 21), 255, 255);
      const uint8_t level = uint8_t(m.d2 * 2);
      rgbLedWrite(PIN_RGB, uint8_t(((c >> 11) << 3) * level / 255), uint8_t((((c >> 5) & 63) << 2) * level / 255),
                  uint8_t(((c & 31) << 3) * level / 255));
      lastInAt = now;
      char nm[6];
      midi::noteName(m.d1, nm, sizeof(nm));
      BLE_NOTE(Rx, "note %s vel %u ch %u", nm, m.d2, (m.status & 15) + 1);
    } else if (midi::isNoteOff(m)) {
      screen.setNote(m.d1, true, false);
    } else if ((m.status & 0xF0) == 0xB0 && m.d1 == 123) {
      memset(screen.inBits, 0, sizeof(screen.inBits));
    }
  }
  if (lastInAt && now - lastInAt > 1500) {
    lastInAt = 0;
    lab.claimLed(false);
  }
  screen.stats = midiOut.stats();

  // A hold in a playing view: released before 2 s = all notes off, still held = next view.
  if (holdSince) {
    if (digitalRead(holdPin) == HIGH) {
      holdSince = 0;
      panic();
    } else if (now - holdSince >= 1400) {
      holdSince = 0;
      lab.setView(lab.view() + 1);
    }
  }
}

bool gesture(BleLab &lab, Gesture g, bool external) {
  const int v = lab.view();
  if (v >= kLinkView) return false;  // link and log: the usual click = next view
  switch (g) {
    case Gesture::Click:
      if (v == kSeqView) {
        static const char *const order[] = {"rock", "funk", "house", "boom"};
        int i = 0;
        while (i < 4 && screen.beat && strcmp(screen.beat->name, order[i]) != 0) i++;
        screen.beat = b7::findBeat(order[(i + 1) % 4]);
        BLE_NOTE(Info, "beat %s", screen.beat->name);
      } else {
        stepNote();
      }
      return true;
    case Gesture::LongPress:
      holdSince = millis();
      holdPin = external ? PIN_BUTTON : PIN_BOOT_BTN;
      return true;
    case Gesture::DoubleClick: return false;  // the view's action: chord, or seq on/off
  }
  return false;
}

const BleLabSpec kSpec{
    "ble-lab-b7", "B7", "Music", "Bench B7", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, tick, gesture, nullptr,
};

BleLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
