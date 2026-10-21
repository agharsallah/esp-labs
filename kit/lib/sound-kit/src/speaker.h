// The speaker: a MAX98357A I2S amplifier and a small 4 ohm speaker. The board sends numbers, the
// amplifier turns them into sound. No DAC, no analog wire: I2S carries 16-bit samples, the MAX98357A
// switches the speaker on and off very fast (class D) in proportion to each number.
//
// A task on core 0 renders 256 samples at a time (16 ms at 16 kHz) and hands them to the I2S DMA.
// i2s_channel_write() waits while the DMA is full, so the task runs exactly as fast as the speaker
// plays. What it mixes:
//   - a tone or a tune (synth.h Voice + Tune), queued from any task with tone() / tune()
//   - recorded samples (synth.h Player): play(), at any speed (pitch). The caller keeps the samples
//     alive until playing() turns false.
//   - a live stream (stream()): S8's voice effects push processed microphone samples here.
//
// Echo guard: the microphone hears the speaker. recentlyLoud() says whether anything audible left
// the speaker in the last few hundred ms, so the lab shell can stop the clap/knock detectors from
// hearing the board's own beeps.
//
// Nothing wired? Then the numbers go nowhere and nothing happens: every lab works without it.
#pragma once

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <algorithm>
#include <atomic>

#include "pins.h"
#include "synth.h"

namespace snd {

class Speaker {
 public:
  static constexpr uint32_t kRate = 16000;
  static constexpr int kBlock = 256;

  bool begin() {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.dma_frame_num = kBlock;
    cc.auto_clear_after_cb = true;  // if we ever fall behind, the DMA plays silence, not the last block again
    if (i2s_new_channel(&cc, &tx_, nullptr) != ESP_OK) return log_e("Speaker: no I2S channel"), false;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = gpio_num_t(PIN_I2S_SPK_BCLK), .ws = gpio_num_t(PIN_I2S_SPK_LRC),
                     .dout = gpio_num_t(PIN_I2S_SPK_DIN), .din = I2S_GPIO_UNUSED, .invert_flags = {}},
    };
    // Stereo, the same sample in left and right: the amplifier's (L+R)/2 mix (SD unconnected) plays it either way.
    if (i2s_channel_init_std_mode(tx_, &sc) != ESP_OK || i2s_channel_enable(tx_) != ESP_OK) return log_e("Speaker: I2S set-up failed"), false;
    cmds_ = xQueueCreate(24, sizeof(Cmd));
    streamBuf_ = static_cast<int16_t *>(heap_caps_calloc(kStream, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!cmds_ || !streamBuf_) return log_e("Speaker: no memory"), false;
    xTaskCreatePinnedToCore(task, "speaker", 4096, this, 4, nullptr, 0);
    log_i("Speaker: MAX98357A on I2S (BCLK %d, LRC %d, DIN %d) at %lu Hz", PIN_I2S_SPK_BCLK, PIN_I2S_SPK_LRC, PIN_I2S_SPK_DIN,
          static_cast<unsigned long>(kRate));
    return true;
  }

  // 0-100. Loudness grows with the square: 50 sounds about half as loud as 100.
  void setVolume(int v) { volume_ = v < 0 ? 0 : v > 100 ? 100 : v; }
  int volume() const { return volume_; }

  // ---- What to play (from any task) ------------------------------------------------------------

  void tone(float hz, uint32_t ms, syn::Wave wave = syn::Wave::Sine, float level = 0.8f) {
    Cmd c{Cmd::Tone};
    c.hz = hz, c.ms = float(ms), c.wave = wave, c.level = level;
    send(c);
  }
  // A melody in Tune's text format. The text must stay alive while it plays (string literals do).
  void tune(const char *text, int bpm = 140, syn::Wave wave = syn::Wave::Triangle) {
    Cmd c{Cmd::Tune};
    c.text = text, c.bpm = bpm, c.wave = wave;
    send(c);
  }
  // Recorded samples at `step` x speed. They must stay alive until playing() is false.
  void play(const int16_t *samples, uint32_t count, float step = 1.0f, float gain = 1.0f) {
    Cmd c{Cmd::Play};
    c.samples = samples, c.count = count, c.step = step, c.level = gain;
    playing_ = true;  // at once, so the caller's next playing() check already says yes
    send(c);
  }
  void stop() { send(Cmd{Cmd::Stop}); }

  bool busy() const { return busy_; }
  bool playing() const { return playing_; }
  float progress() const { return progress_; }
  // Was anything audible sent in the last `ms`? (the echo guard)
  bool recentlyLoud(uint32_t ms = 250) const { return millis() - lastLoudMs_ < ms; }
  // The loudest output sample of the last block, 0-1 (for a meter).
  float outputLevel() const { return outLevel_; }

  // ---- Live stream (S8) ------------------------------------------------------------------------

  // Push samples to play as soon as possible. Single producer (loop()), the speaker task consumes.
  // Returns how many fitted; the rest are dropped (the stream is ~0.5 s deep).
  uint32_t stream(const int16_t *s, uint32_t n) {
    const uint32_t w = sw_.load(std::memory_order_relaxed), r = sr_.load(std::memory_order_acquire);
    const uint32_t room = kStream - 1 - (w - r);
    if (n > room) n = room;
    for (uint32_t i = 0; i < n; i++) streamBuf_[(w + i) & (kStream - 1)] = s[i];
    sw_.store(w + n, std::memory_order_release);
    return n;
  }
  uint32_t streamQueued() const { return sw_.load(std::memory_order_acquire) - sr_.load(std::memory_order_acquire); }

 private:
  struct Cmd {
    enum Kind : uint8_t { Tone, Tune, Play, Stop } kind;
    syn::Wave wave = syn::Wave::Sine;
    float hz = 0, ms = 0, level = 1, step = 1;
    const char *text = nullptr;
    int bpm = 140;
    const int16_t *samples = nullptr;
    uint32_t count = 0;
  };
  static constexpr uint32_t kStream = 8192;

  void send(const Cmd &c) {
    if (cmds_ && xQueueSend(cmds_, &c, 0) != pdTRUE) log_w("Speaker: too many sounds queued");
  }

  static void task(void *arg) { static_cast<Speaker *>(arg)->run(); }

  void apply(const Cmd &c) {
    const syn::Envelope env;
    switch (c.kind) {
      case Cmd::Tone:
        tune_.set(nullptr, 0);
        voice_.start(c.hz, c.ms, c.wave, env, kRate, c.level);
        break;
      case Cmd::Tune:
        tune_.set(c.text, c.bpm);
        tuneWave_ = c.wave;
        voice_ = syn::Voice{};
        break;
      case Cmd::Play: player_.set(c.samples, c.count, c.step, c.level); break;
      case Cmd::Stop:
        tune_.set(nullptr, 0);
        voice_ = syn::Voice{};
        player_.stop();
        sr_.store(sw_.load(std::memory_order_acquire), std::memory_order_release);
        break;
    }
  }

  void run() {
    static int16_t out[kBlock * 2];  // left + right
    for (;;) {
      Cmd c;
      while (xQueueReceive(cmds_, &c, 0) == pdTRUE) apply(c);
      // The tune hands the voice its next note when the last one has finished.
      if (!voice_.active() && !tune_.done()) {
        float hz = 0, ms = 0;
        if (tune_.next(hz, ms)) {
          syn::Envelope env;
          env.releaseMs = std::min(90.0f, ms * 0.3f);
          voice_.start(hz, ms - env.releaseMs, tuneWave_, env, kRate, 0.7f);
        }
      }
      const float gain = (volume_ / 100.0f) * (volume_ / 100.0f) * 32000.0f;
      float peak = 0;
      const uint32_t w = sw_.load(std::memory_order_acquire);
      uint32_t r = sr_.load(std::memory_order_relaxed);
      for (int i = 0; i < kBlock; i++) {
        float v = voice_.next() + player_.next();
        if (r != w) v += streamBuf_[r++ & (kStream - 1)] / 32768.0f;
        v = syn::softClip(v);
        peak = std::max(peak, fabsf(v));
        out[2 * i] = out[2 * i + 1] = int16_t(v * gain);
      }
      sr_.store(r, std::memory_order_release);
      outLevel_ = peak;
      if (peak * gain > 300) lastLoudMs_ = millis();
      busy_ = voice_.active() || !tune_.done() || player_.active() || r != sw_.load(std::memory_order_acquire);
      playing_ = player_.active();
      progress_ = player_.progress();
      size_t done = 0;
      i2s_channel_write(tx_, out, sizeof(out), &done, portMAX_DELAY);  // waits for room: this is the clock
    }
  }

  i2s_chan_handle_t tx_ = nullptr;
  QueueHandle_t cmds_ = nullptr;
  syn::Voice voice_;
  syn::Tune tune_;
  syn::Wave tuneWave_ = syn::Wave::Triangle;
  syn::Player player_;
  int16_t *streamBuf_ = nullptr;
  std::atomic<uint32_t> sw_{0}, sr_{0};
  volatile int volume_ = 40;
  volatile bool busy_ = false, playing_ = false;
  volatile float progress_ = 0, outLevel_ = 0;
  volatile uint32_t lastLoudMs_ = 0;
};

}  // namespace snd
