// S4's three screens (320 x 170, under the shared header):
//   recorder  the state as a big word (OFF, ARMED, REC 3.2 s), the last 3 s of loudness with the
//             trigger line, the pre-roll (amber) and what went into a clip (red)
//   clips     the list with each clip's shape, and the picked clip's facts; a playhead runs
//             across the clip the speaker is playing
//   web       where the page is, the Wi-Fi, how to get the token, who is watching
#pragma once

#include <WiFi.h>

#include "connection.h"
#include "recorder.h"
#include "sound_views.h"

namespace s4v {

using sv::kTop;

inline void recorder(FrameBuffer &fb, const rec::Recorder &r, const snd::TriggerConfig &cfg, const rec::ClipStore &store, float t) {
  // The state, big: nobody should wonder whether this thing is listening.
  char word[24];
  uint16_t c = pal::muted;
  if (r.recording()) {
    snprintf(word, sizeof(word), "REC %.1f s", r.recordedMs() / 1000.0f);
    c = sv::kRec;
    if (fmodf(t, 1.0f) < 0.6f) fb.fillCircle(16, kTop + 26, 8, sv::kRec);
  } else if (r.armed()) {
    snprintf(word, sizeof(word), "ARMED");
    c = pal::teal;
    fb.drawCircle(16, kTop + 26, 8, pal::teal);
  } else {
    snprintf(word, sizeof(word), "OFF");
    fb.drawCircle(16, kTop + 26, 8, pal::dim);
  }
  sv::big(fb, 32, kTop + 42, c, word);

  char line[40];
  snprintf(line, sizeof(line), "%d clips", store.count());
  sv::small(fb, 236, kTop + 12, pal::ink, line);
  snprintf(line, sizeof(line), "%.1f / %.0f MB", store.used() / 1048576.0f, store.budget / 1048576.0f);
  sv::small(fb, 236, kTop + 24, pal::muted, line);
  sv::small(fb, 236, kTop + 36, pal::muted, "lost on reset");

  // The last 3 s, newest on the right: each column the loudest of ~2.5 frames, mirrored.
  const int x = 10, y = kTop + 54, w = 300, h = 66, mid = y + h / 2;
  fb.fillRect(x, y, w, h, sv::kPanel);
  constexpr float kRange = 50;  // dB above the quiet across the half-height
  for (int col = 0; col < w; col++) {
    const int a = col * rec::Recorder::kEnvelope / w, b = (col + 1) * rec::Recorder::kEnvelope / w;
    int db = 0;
    rec::Recorder::Mark mark = rec::Recorder::None;
    for (int i = a; i < b; i++) {
      db = std::max<int>(db, r.envelopeDb(i));
      mark = std::max(mark, r.envelopeMark(i));
    }
    const int half = std::min(h / 2 - 1, int(db / kRange * (h / 2)));
    const uint16_t colour = mark == rec::Recorder::Recorded ? sv::kRec : mark == rec::Recorder::PreRoll ? pal::amber : sv::kSound;
    if (mark != rec::Recorder::None) fb.drawFastVLine(x + col, y, h, mix(colour, sv::kPanel, 200));
    if (half > 0) fb.drawFastVLine(x + col, mid - half, half * 2 + 1, colour);
  }
  // The trigger: louder than this line starts a recording (when armed).
  const int ty = std::min(h / 2 - 1, int(cfg.startDb / kRange * (h / 2)));
  for (int i = x; i < x + w; i += 4) fb.drawPixel(i, mid - ty, pal::ink), fb.drawPixel(i, mid + ty, pal::ink);
  sv::small(fb, x + 2, y + 2, pal::muted, "3 s ago");
  sv::small(fb, x + w - 20, y + 2, pal::muted, "now");
  // (The screen's built-in font has no "·": plain ASCII only on the display.)
  char settings[64];
  snprintf(settings, sizeof(settings), "start %.0f dB, hold %.1f s, pre-roll %.1f s", cfg.startDb, cfg.holdMs / 1000.0f, cfg.prerollMs / 1000.0f);
  sv::small(fb, x, y + h + 4, pal::muted, settings);
}

inline void clips(FrameBuffer &fb, const rec::ClipStore &store, int &picked, uint32_t playing = 0, float progress = 0) {
  const int n = store.count();
  if (n == 0) {
    sv::bold(fb, 12, kTop + 34, pal::ink, "No clips yet.");
    sv::small(fb, 12, kTop + 52, pal::muted, "arm (voice trigger) or rec (by hand), then talk.");
    sv::small(fb, 12, kTop + 66, pal::muted, "Clips live in PSRAM: a reset forgets them.");
    return;
  }
  picked = std::min(std::max(picked, 0), n - 1);
  // Four rows, scrolled so the picked one is visible. Newest at the bottom.
  const int first = std::max(0, std::min(picked - 1, n - 4));
  for (int row = 0; row < 4 && first + row < n; row++) {
    rec::Clip c;
    if (!store.facts(first + row, c)) break;
    const int y = kTop + 4 + row * 30;
    const bool on = first + row == picked;
    if (on) fb.fillRoundRect(4, y, 178, 27, 4, sv::kPanel);
    char num[8];
    snprintf(num, sizeof(num), "%lu", static_cast<unsigned long>(c.number));
    sv::bold(fb, 10, y + 19, on ? sv::kSound : pal::ink, num);
    // The clip's shape from its 48-slice overview, mirrored.
    for (int b = 0; b < 48; b++) {
      const int half = c.open ? 1 : std::max(1, c.overview[b] * 11 / 255);
      fb.drawFastVLine(42 + b * 2, y + 13 - half, half * 2, c.open ? sv::kRec : on ? sv::kSound : pal::muted);
    }
    char dur[12];
    snprintf(dur, sizeof(dur), "%.1fs", c.durationMs() / 1000.0f);
    sv::small(fb, 142, y + 6, pal::ink, dur);
    const bool sounding = playing && c.number == playing;
    sv::small(fb, 142, y + 16, sounding ? pal::teal : pal::muted, c.open ? "REC" : sounding ? "PLAY" : c.manual ? "hand" : "voice");
    if (sounding) {
      // The playhead: what the speaker has played so far, over the clip's shape.
      const int px = 42 + int(std::min(1.0f, progress) * 96);
      fb.drawFastVLine(px, y + 1, 25, pal::ink);
      fb.fillTriangle(px - 3, y + 1, px + 3, y + 1, px, y + 5, pal::ink);
    }
  }
  // The picked clip's facts.
  rec::Clip c;
  if (!store.facts(picked, c)) return;
  char a[24], b[24];
  snprintf(a, sizeof(a), "%.1f dBFS", snd::toDb(float(c.peak)));
  snprintf(b, sizeof(b), "x%.1f", snd::gainFor(c.peak));
  sv::stat(fb, 194, kTop + 4, "loudest", a);
  sv::stat(fb, 194, kTop + 42, "sent louder by", b, sv::kSound);
  if (c.wall > 0) {
    const time_t w = time_t(c.wall);
    struct tm tm;
    localtime_r(&w, &tm);
    strftime(a, sizeof(a), "%H:%M:%S", &tm);
  } else {
    snprintf(a, sizeof(a), "%lus in", static_cast<unsigned long>(c.startMs / 1000));
  }
  sv::stat(fb, 194, kTop + 80, "started", a);
  snprintf(b, sizeof(b), "long press: next clip");
  sv::small(fb, 194, kTop + 118, pal::muted, b);
}

inline void web(FrameBuffer &fb, const Connection &conn, bool apiUp, size_t clients) {
  const bool joined = conn.state() == Connection::State::Connected;
  if (conn.ssid().isEmpty()) {
    sv::bold(fb, 12, kTop + 26, pal::amber, "No Wi-Fi saved");
    sv::small(fb, 12, kTop + 44, pal::muted, "Run W1 once to join your network,");
    sv::small(fb, 12, kTop + 56, pal::muted, "or pull clips over USB:");
    sv::small(fb, 12, kTop + 72, pal::ink, "tools/sound_pull.py <port> <n>");
    return;
  }
  char a[40];
  if (joined) snprintf(a, sizeof(a), "http://%s.local", Connection::kHostname);
  else snprintf(a, sizeof(a), "joining %s...", conn.ssid().c_str());
  sv::bold(fb, 12, kTop + 22, joined ? sv::kSound : pal::amber, a);
  if (joined) {
    char ip[24], rssi[16], who[16];
    snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
    snprintf(rssi, sizeof(rssi), "%ld dBm", static_cast<long>(WiFi.RSSI()));
    snprintf(who, sizeof(who), "%u", unsigned(clients));
    sv::stat(fb, 12, kTop + 34, "or", ip);
    sv::stat(fb, 150, kTop + 34, "signal", rssi);
    sv::stat(fb, 240, kTop + 34, "pages open", who, clients ? pal::teal : pal::ink);
  }
  sv::small(fb, 12, kTop + 78, pal::muted, apiUp ? "The page asks for the admin token:" : "The page starts once Wi-Fi is up.");
  sv::small(fb, 12, kTop + 90, pal::ink, "type token in the serial monitor.");
  sv::small(fb, 12, kTop + 106, pal::muted, "Recording always shows here: REC + red LED.");
}

}  // namespace s4v
