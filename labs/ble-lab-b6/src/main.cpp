// Bluetooth lab B6 Keyboard & media.  uv run pio run -d labs/ble-lab-b6 -e ble-lab-b6 -t upload
// The board becomes a Bluetooth keyboard with media keys. Pair it like any keyboard (phone or Mac
// Bluetooth settings: "Bench B6"), then the button is a remote: play/pause the music on your phone,
// skip tracks, change the volume (that music can be playing on your JBL: the phone talks to the
// speaker, the board talks to the phone), click through slides, or type text from the serial monitor.
//
// Wiring: the screen from lesson 03. Optional: the push button on GPIO 5 (does what BOOT does), and
// two jumper wires on GPIO 1 and GPIO 2 as touch keys for volume down / up (`touch on`).
//
// Views: media · slides · typer · keys · link · log
//   media and slides are modes: click / double-click / hold act there; hold 2 s = next view.
//   elsewhere: click = next view, double-click = the footer's action.
// Serial: help lists the commands. Shared code: kit/lib/ble-kit. HID: hid_keyboard.h, keymap.h.
#include <Preferences.h>

#include "b6_views.h"
#include "ble_lab.h"
#include "hid_keyboard.h"
#include "keymap.h"
#include "server_link.h"
#include "touch_pads.h"

namespace {

ble::ServerLink links;
b6::Keyboard kb;
b6::Screen screen;
Preferences prefs;
constexpr uint8_t kTouchPins[2] = {1, 2};  // volume down, volume up
TouchPads<2> touch(kTouchPins);
uint32_t touchRepeatAt = 0;

// A long press in a mode view waits: released before 2 s = the mode's long action,
// still held at 2 s = next view.
uint32_t holdSince = 0;
uint8_t holdPin = 0;

bool needReady(BleLab &lab) {
  if (b6::Keyboard::ready()) return true;
  lab.warn("not paired yet: open Bluetooth settings on your phone or Mac and pick \"%s\"", lab.spec().deviceName);
  return false;
}

void sendMedia(BleLab &lab, uint16_t usage) {
  if (!needReady(lab)) return;
  kb.mediaKey(usage);
  screen.lastMedia = usage;
  screen.lastMediaAt = millis();
  if (usage == b6::media::kPlayPause) screen.playing = !screen.playing;
  if (usage == b6::media::kMute) screen.muted = !screen.muted;
  if (usage == b6::media::kVolumeUp) screen.volumeSteps++, screen.muted = false;
  if (usage == b6::media::kVolumeDown) screen.volumeSteps--;
  BLE_NOTE(Tx, "media key: %s (usage %03X)", b6::mediaName(usage), usage);
}

void sendKey(BleLab &lab, keymap::Stroke s, const char *what, char ch = 0) {
  if (!needReady(lab)) return;
  if (!kb.tap(s, ch)) return lab.warn("typing queue full, wait a moment");
  BLE_NOTE(Tx, "key %s (mod %02X key %02X)", what, s.mod, s.key);
}

void slide(BleLab &lab, int dir) {
  if (!needReady(lab)) return;
  sendKey(lab, {0, dir > 0 ? keymap::key::kRight : keymap::key::kLeft}, dir > 0 ? "right arrow" : "left arrow");
  screen.slide = std::max(1, screen.slide + dir);
  screen.slideDir = dir;
  screen.slideAt = millis();
  screen.black = false;
}

// Queue text, character by character, with the chosen layout. Returns the keys queued.
int typeText(BleLab &lab, const char *text) {
  if (!needReady(lab)) return 0;
  String skipped;
  int n = 0;
  for (const char *p = text; *p; p++) {
    const keymap::Stroke s = keymap::fromChar(*p, screen.layout);
    if (s.key == 0) {
      if (skipped.indexOf(*p) < 0) skipped += *p;
      continue;
    }
    if (!kb.tap(s, *p)) {
      lab.warn("typing queue full after %d characters", n);
      break;
    }
    n++;
  }
  if (skipped.length()) lab.warn("%s can't type \"%s\": skipped", keymap::layoutName(screen.layout), skipped.c_str());
  // The tail of the text, for the typer view.
  const size_t len = strlen(text);
  strlcpy(screen.typed, len > 46 ? text + len - 46 : text, sizeof(screen.typed));
  screen.total = n;
  BLE_NOTE(Tx, "typing %d keys (%s)", n, screen.layout == keymap::Layout::US ? "US" : "DE");
  return n;
}

// ---- Views ------------------------------------------------------------------------------------

void drawMedia(BleLab &, FrameBuffer &fb, float t) { b6v::media(fb, screen, t); }
void drawSlides(BleLab &, FrameBuffer &fb, float t) { b6v::slides(fb, screen, t); }
void drawTyper(BleLab &, FrameBuffer &fb, float t) { b6v::typer(fb, screen, kb.hostLeds(), t); }
void drawKeys(BleLab &, FrameBuffer &fb, float t) { b6v::keys(fb, kb, screen.layout, t); }
void drawLink(BleLab &lab, FrameBuffer &fb, float t) { bv::link(fb, lab.state(), t, "keyboard"); }
void drawLog(BleLab &, FrameBuffer &fb, float) { bv::log(fb); }

constexpr int kMediaView = 0, kSlidesView = 1;
const BleView kViews[] = {
    {"media", drawMedia, "media next"},
    {"slides", drawSlides, "key left"},
    {"typer", drawTyper, "snippet"},
    {"keys", drawKeys, "key shift"},
    {"link", drawLink, nullptr},
    {"log", drawLog, nullptr},
};

// ---- Commands ---------------------------------------------------------------------------------

void cmdType(BleLab &lab, const String &arg, const String &) {
  if (arg.isEmpty()) return lab.warn("type <text>, e.g. type hello bench");
  typeText(lab, arg.c_str());
  lab.showView("typer");
}

void cmdKey(BleLab &lab, const String &, const String &lower) {
  keymap::Stroke s;
  if (!keymap::fromName(lower.c_str(), s))
    return lab.warn("key <name>: enter tab esc space backspace delete up down left right home end pageup pagedown f1-f12, a letter; "
                    "combos with cmd ctrl alt shift, e.g. key cmd+space");
  sendKey(lab, s, lower.c_str());
}

void cmdLayout(BleLab &lab, const String &, const String &lower) {
  if (lower != "us" && lower != "de") return lab.warn("layout us | layout de   (match the layout set on the computer)");
  screen.layout = lower == "us" ? keymap::Layout::US : keymap::Layout::DE;
  prefs.putUChar("layout", uint8_t(screen.layout));
  BLE_NOTE(Info, "layout %s (saved)", keymap::layoutName(screen.layout));
}

void cmdSnippet(BleLab &lab, const String &arg, const String &) {
  if (arg.isEmpty()) {
    if (!screen.snippet[0]) return lab.warn("no snippet yet: snippet <text> saves one");
    typeText(lab, screen.snippet);
    return;
  }
  strlcpy(screen.snippet, arg.c_str(), sizeof(screen.snippet));
  prefs.putString("snippet", screen.snippet);
  BLE_NOTE(Info, "snippet saved: double-click on the typer view types it");
}

void cmdMedia(BleLab &lab, const String &, const String &lower) {
  if (lower == "play" || lower == "pause") return sendMedia(lab, b6::media::kPlayPause);
  if (lower == "next") return sendMedia(lab, b6::media::kNext);
  if (lower == "prev" || lower == "previous") return sendMedia(lab, b6::media::kPrevious);
  if (lower == "up") return sendMedia(lab, b6::media::kVolumeUp);
  if (lower == "down") return sendMedia(lab, b6::media::kVolumeDown);
  if (lower == "mute") return sendMedia(lab, b6::media::kMute);
  lab.warn("media play | next | prev | up | down | mute");
}

void cmdTouch(BleLab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("touch on | touch off   (jumpers on GPIO 1 = volume down, GPIO 2 = up)");
  touch.enable(lower == "on");
  screen.touchOn = touch.on();
  if (touch.on()) BLE_NOTE(Good, "touch on: baselines %lu / %lu, hands off while measuring", static_cast<unsigned long>(touch.baseline(0)),
                           static_cast<unsigned long>(touch.baseline(1)));
  else BLE_NOTE(Info, "touch off");
}

void cmdForget(BleLab &, const String &, const String &) {
  const ble::LinkState s = ble::link().get();
  NimBLEDevice::deleteAllBonds();
  if (s.radio == ble::Radio::Connected) NimBLEDevice::getServer()->disconnect(s.peer.handle);
  BLE_NOTE(Warn, "bonds deleted here. Also forget \"Bench B6\" on the phone/Mac");
}

const BleCommand kCommands[] = {
    {"type", "<text>", "type text into the paired phone/Mac", cmdType},
    {"key", "<name|combo>", "one key or combo: enter, f5, cmd+space, ctrl+alt+delete", cmdKey},
    {"layout", "us|de", "the computer's keyboard layout (saved)", cmdLayout},
    {"snippet", "[text]", "save a text (alone: type it; typer view 2x too)", cmdSnippet},
    {"media", "<key>", "play next prev up down mute", cmdMedia},
    {"touch", "on|off", "touch wires on GPIO 1/2 = volume down/up", cmdTouch},
    {"forget", "", "delete the bond (then forget the board on the phone too)", cmdForget},
};

// ---- Hooks ------------------------------------------------------------------------------------

void setup(BleLab &lab) {
  prefs.begin("b6", false);
  screen.layout = keymap::Layout(prefs.getUChar("layout", 0) == 1 ? 1 : 0);
  strlcpy(screen.snippet, prefs.getString("snippet", "").c_str(), sizeof(screen.snippet));
  // Bonding yes, man-in-the-middle protection no (no screen on a keyboard... well, we have one,
  // but hosts expect a keyboard to pair like one), LE Secure Connections yes.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&links, false);
  kb.begin(server);
  server->start();
  kb.advertise(lab.spec().deviceName);
  BLE_NOTE(Info, "layout %s. Pair: Bluetooth settings > Bench B6", keymap::layoutName(screen.layout));
}

void tick(BleLab &lab) {
  kb.pump();
  screen.queued = kb.pending();
  screen.ready = b6::Keyboard::ready();
  // A long press waiting for its outcome (mode views).
  if (holdSince) {
    if (digitalRead(holdPin) == HIGH) {
      holdSince = 0;
      if (lab.view() == kMediaView) sendMedia(lab, b6::media::kMute);
      else if (lab.view() == kSlidesView && needReady(lab)) {
        sendKey(lab, keymap::fromChar('b', keymap::Layout::US), "b (black screen)", 'b');
        screen.black = !screen.black;
      }
    } else if (millis() - holdSince >= 1400) {  // + the 600 ms before LongPress fired = 2 s
      holdSince = 0;
      lab.setView(lab.view() + 1);
    }
  }
  // Touch volume: a touch = one step, keep touching = repeat every 300 ms.
  uint32_t released = 0;
  const uint32_t pressed = touch.poll(released);
  for (int i = 0; i < 2; i++) {
    screen.touchRise[i] = touch.rise(i);
    if ((pressed >> i) & 1) {
      sendMedia(lab, i == 0 ? b6::media::kVolumeDown : b6::media::kVolumeUp);
      touchRepeatAt = millis() + 500;
    } else if (touch.down(i) && millis() > touchRepeatAt) {
      sendMedia(lab, i == 0 ? b6::media::kVolumeDown : b6::media::kVolumeUp);
      touchRepeatAt = millis() + 300;
    }
  }
}

// In the two mode views the button is the remote. Double-click falls through to the view action.
bool gesture(BleLab &lab, Gesture g, bool external) {
  const int v = lab.view();
  if (v != kMediaView && v != kSlidesView) return false;
  switch (g) {
    case Gesture::Click:
      if (v == kMediaView) sendMedia(lab, b6::media::kPlayPause);
      else slide(lab, +1);
      return true;
    case Gesture::LongPress:
      holdSince = millis();
      holdPin = external ? PIN_BUTTON : PIN_BOOT_BTN;
      return true;
    case Gesture::DoubleClick:
      if (v == kSlidesView) {
        slide(lab, -1);
        return true;
      }
      return false;  // media: the view action, "media next"
  }
  return false;
}

const BleLabSpec kSpec{
    "ble-lab-b6", "B6", "Keyboard & media", "Bench B6", kViews, sizeof(kViews) / sizeof(kViews[0]), 0,
    kCommands, sizeof(kCommands) / sizeof(kCommands[0]), setup, tick, gesture, nullptr,
};

BleLab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
