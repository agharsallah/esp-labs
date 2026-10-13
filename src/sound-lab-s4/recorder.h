// The recorder: snd::Trigger decides when, this copies the sound into clips.
//
// The sampler never stops and never waits for us: it keeps writing the last 4 s into its ring.
// The recorder only remembers a sample number, `cursor_`, and every frame copies everything from
// there up to the newest sample into the open clip. That is why a recording can start 500 ms in
// the past (the pre-roll is still in the ring), and why a slow screen frame loses nothing. Only if
// loop() fell more than ~4 s behind would the ring have overwritten what we hadn't copied yet:
// then the clip ends early, and says so.
//
// The envelope: for the recorder view, the last 3 s of frames as "dB above the quiet" plus what
// each frame became (nothing, pre-roll, recorded).
#pragma once

#include <esp_heap_caps.h>
#include <time.h>

#include "clips.h"
#include "sound_lab.h"
#include "trigger.h"

namespace rec {

inline void *psramAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
inline void *psramResize(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
inline void psramFree(void *p) { heap_caps_free(p); }

// "s4-clip-3-20261014-213005.wav", or "s4-clip-3.wav" when the clock wasn't set.
inline void clipName(const Clip &c, char *out, size_t n) {
  if (c.wall <= 0) {
    snprintf(out, n, "s4-clip-%lu.wav", static_cast<unsigned long>(c.number));
    return;
  }
  const time_t t = time_t(c.wall);
  struct tm tm;
  localtime_r(&t, &tm);
  char when[20];
  strftime(when, sizeof(when), "%Y%m%d-%H%M%S", &tm);
  snprintf(out, n, "s4-clip-%lu-%s.wav", static_cast<unsigned long>(c.number), when);
}

class Recorder {
 public:
  static constexpr int kEnvelope = 750;  // frames: 3 s at 4 ms
  enum Mark : uint8_t { None, PreRoll, Recorded };

  Recorder() : store_(Allocator{psramAlloc, psramResize, psramFree}), trigger_(16000) {}

  ClipStore &store() { return store_; }
  snd::TriggerConfig &config() { return trigger_.config; }
  snd::Trigger::State state() const { return trigger_.state(); }
  bool armed() const { return trigger_.armed(); }
  bool recording() const { return clip_ != nullptr; }
  bool manual() const { return trigger_.manual(); }
  uint32_t clipNumber() const { return clip_ ? clip_->number : 0; }
  uint32_t recordedMs() const { return clip_ ? clip_->durationMs() : 0; }

  // ---- The envelope, for the screen ------------------------------------------------------------

  int envelopeHead() const { return head_; }
  uint8_t envelopeDb(int i) const { return db_[(head_ + i) % kEnvelope]; }  // 0 = oldest
  Mark envelopeMark(int i) const { return Mark(mark_[(head_ + i) % kEnvelope]); }

  // ---- From the lab's hooks --------------------------------------------------------------------

  void frame(SoundLab &lab, const snd::Frame &f, uint32_t ms) {
    const float above = lab.level().above();
    db_[head_] = uint8_t(std::min(255.0f, lab.level().instAbove()));
    mark_[head_] = clip_ ? Recorded : None;
    head_ = (head_ + 1) % kEnvelope;
    // A frame more than a second old means loop() was stuck (a serial `send`, say): don't let it
    // start a recording of sound that may already be gone from the ring.
    if (lab.mic().written() - f.end > lab.mic().rate()) return;
    trigger_.setRate(lab.mic().rate());
    handle(lab, trigger_.push(above, f.end), ms);
  }

  void tick(SoundLab &lab) {
    pump(lab, lab.mic().written());
    store_.collect();
    lab.setRecording(clip_ != nullptr);  // the REC dot and the red LED: nothing records without them
  }

  // ---- Commands --------------------------------------------------------------------------------

  void arm(SoundLab &lab, bool on) {
    trigger_.arm(on);
    lab.note(sv::Note::Info, on ? "armed: talk and it records" : "voice trigger off");
  }

  void start(SoundLab &lab) {
    if (clip_) return lab.note(sv::Note::Info, "already recording clip %lu", static_cast<unsigned long>(clip_->number));
    trigger_.setRate(lab.mic().rate());
    handle(lab, trigger_.start(lab.mic().written()), lab.soundMs());
  }

  void stop(SoundLab &lab) {
    if (!clip_) return lab.note(sv::Note::Info, "not recording");
    handle(lab, trigger_.stop(lab.mic().written()), lab.soundMs());
  }

 private:
  uint32_t samples(uint32_t ms, uint32_t rate) const { return uint32_t(uint64_t(ms) * rate / 1000); }

  void handle(SoundLab &lab, const snd::Trigger::Result &r, uint32_t ms) {
    using A = snd::Trigger::Action;
    const uint32_t rate = lab.mic().rate();
    if (r.action == A::Start) {
      const uint32_t pre = trigger_.config.prerollMs;
      const time_t now = time(nullptr);
      const int64_t wall = now > 1'700'000'000 ? int64_t(now) - pre / 1000 : 0;
      // Room for the longest clip, plus a few frames: the trigger checks the length once per frame.
      const uint32_t capacity = samples(trigger_.config.maxMs, rate) + 4 * snd::kChunk;
      const ClipStore::Opened o = store_.open(rate, capacity, trigger_.manual(), ms > pre ? ms - pre : 0, wall);
      if (!o.clip) {
        // Back to off: left armed, a loud room would retry (and fail) 250 times a second.
        trigger_.stop(r.from);
        const bool wasArmed = trigger_.armed();
        trigger_.arm(false);
        return lab.note(sv::Note::Bad, "can't record: %s%s", o.why, wasArmed ? " (voice trigger off)" : "");
      }
      if (o.dropped) lab.note(sv::Note::Warn, "store full: dropped the %d oldest voice clip%s", o.dropped, o.dropped > 1 ? "s" : "");
      clip_ = o.clip;
      cursor_ = r.from;
      // Paint the pre-roll into the envelope: those frames are in the clip after all.
      const int back = std::min<int>(kEnvelope, int((r.to - r.from) / snd::kChunk));
      for (int i = 1; i <= back; i++) mark_[(head_ - i + kEnvelope) % kEnvelope] = PreRoll;
      lab.note(sv::Note::Bad, "REC clip %lu (%s)", static_cast<unsigned long>(clip_->number), clip_->manual ? "by hand" : "voice");
      lab.setRecording(true);
    } else if (r.action == A::Save || r.action == A::Drop) {
      if (!clip_) return;
      pump(lab, r.to);
      finish(lab, r.action == A::Save, nullptr);
    }
  }

  void finish(SoundLab &lab, bool keep, const char *why) {
    Clip *c = clip_;
    clip_ = nullptr;
    lab.setRecording(false);
    const uint32_t number = c->number, ms = c->durationMs();
    store_.close(c, keep);
    if (!keep) return lab.note(sv::Note::Info, "a bang, not a voice: clip %lu dropped", static_cast<unsigned long>(number));
    lab.note(why ? sv::Note::Warn : sv::Note::Good, "clip %lu: %.1f s%s%s", static_cast<unsigned long>(number), ms / 1000.0f, why ? ", " : "",
             why ? why : "");
  }

  // Copy from the cursor up to sample number `upTo` into the open clip.
  void pump(SoundLab &lab, uint32_t upTo) {
    static int16_t buf[1024];
    while (clip_ && cursor_ < upTo) {
      const uint32_t n = std::min<uint32_t>(1024, upTo - cursor_);
      if (!lab.mic().copy(cursor_, buf, n)) {
        trigger_.stop(cursor_);
        return finish(lab, true, "ended early: loop fell 4 s behind");
      }
      store_.append(clip_, buf, n);
      cursor_ += n;
      if (store_.full(clip_)) {
        trigger_.stop(cursor_);
        return finish(lab, true, "the longest clip (max)");
      }
    }
  }

  ClipStore store_;
  snd::Trigger trigger_;
  Clip *clip_ = nullptr;
  uint32_t cursor_ = 0;
  uint8_t db_[kEnvelope] = {}, mark_[kEnvelope] = {};
  int head_ = 0;
};

}  // namespace rec
