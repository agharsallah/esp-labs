// The microphone, whichever one is wired: sampled 16 000 times a second, the same pipeline after.
//
// Two sources, one class:
//   KY-037   analog. Its AO pin is a small wave on top of a steady voltage (the "bias"). The ESP32's
//            own ADC measures it, 12 bits (0-4095). No amplifier: a good loudness sensor, hissy for
//            voices. Its DO pin is the comparator's yes/no, watched by an interrupt.
//   INMP441  digital. The microphone has its own amplifier and a 24-bit ADC inside, and sends
//            numbers over I2S (a clock, a left/right line, a data line). No bias, no ADC noise, and
//            its sensitivity is in the datasheet (-26 dBFS at 94 dB SPL), so S2 can show real dB SPL.
//
// Why not analogRead() in loop()? It takes one sample when asked, and loop() also draws the screen
// (~40 ms a frame), so we would get a few samples a frame, unevenly spaced: useless for sound.
// Both sources run on their own clock instead: the ADC in "continuous" mode, the I2S peripheral as
// the microphone's clock master, each with DMA (direct memory access) moving results into RAM with
// no CPU involved. A sampler task on core 0 (the screen loop runs on core 1) picks them up and:
//
//   1. removes the DC part: a very slow average follows it, and every sample becomes "raw - average",
//      a wave around 0 (the KY-037's bias, or the INMP441's small offset).
//   2. scales it to 16 bits and stores it in a 4-second ring in PSRAM. Each sample has a running
//      number (`written()`): the scope, FFT and recorder copy stretches out of the ring by number,
//      whenever they like, without stopping the sampler.
//   3. every 64 samples (4 ms) measures the chunk's RMS and peak and queues it as a Frame. The lab
//      shell turns frames into the level meter and the clap, knock and beat detectors.
//
// Pins (kit/include/pins.h): KY-037 AO must be an ADC1 pin (ADC2 is busy while Wi-Fi is on). The
// INMP441 uses the I2S0 peripheral; the speaker (speaker.h) uses I2S1, so both can run at once.
#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_continuous.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>

#include "pins.h"

namespace snd {

constexpr int kChunk = 64;  // samples per frame

enum class Source : uint8_t { Ky037, Inmp441 };
inline const char *sourceName(Source s) { return s == Source::Inmp441 ? "INMP441 (I2S, digital)" : "KY-037 (ADC, analog)"; }
inline const char *sourceShort(Source s) { return s == Source::Inmp441 ? "INMP441" : "KY-037"; }

struct Frame {
  uint32_t end;   // the sample number just after this frame
  float rms;      // int16 units
  int16_t peak;   // the biggest |sample|
};

// Attenuation (KY-037 only): how much the ADC divides the input before measuring. More = a wider
// voltage range, fewer millivolts per step. The S3's usable ranges, roughly: 0 dB to ~0.95 V,
// 2.5 dB ~1.25 V, 6 dB ~1.75 V, 12 dB ~3.1 V. The bias must sit inside the range, with room for the wave.
inline const char *attenName(adc_atten_t a) {
  switch (a) {
    case ADC_ATTEN_DB_0: return "0 dB (0-0.95 V)";
    case ADC_ATTEN_DB_2_5: return "2.5 dB (0-1.25 V)";
    case ADC_ATTEN_DB_6: return "6 dB (0-1.75 V)";
    default: return "12 dB (0-3.1 V)";
  }
}
inline int attenTopMv(adc_atten_t a) { return a == ADC_ATTEN_DB_0 ? 950 : a == ADC_ATTEN_DB_2_5 ? 1250 : a == ADC_ATTEN_DB_6 ? 1750 : 3100; }

class Mic {
 public:
  static constexpr uint32_t kRing = 1 << 16;  // samples: 4.1 s at 16 kHz, 128 KB of PSRAM
  static constexpr uint32_t kMask = kRing - 1;
  // INMP441: its 24-bit sample >> 6 = our int16. That's 12 dB of gain over plain 16 bits: a quiet
  // room still uses a few bits, and it clips only around 114 dB SPL (a clap right at the mic).
  static constexpr int kI2sShift = 8 + 6;
  // dB SPL = our RMS dBFS + this (INMP441 only): -26 dBFS at 94 dB SPL, +12 dB for the shift above,
  // +3 dB because a full-scale sine has an RMS 3 dB under its peak. About +-2 dB, no A-weighting.
  static constexpr float kInmp441Spl = 94.0f + 26.0f - 12.0f + 3.0f;

  // Is an INMP441 answering on the I2S pins? With the data line pulled down, a missing microphone
  // reads as zeros; a real one never sends 100 ms of perfect silence. Takes ~0.3 s.
  static bool probeInmp441() {
    i2s_chan_handle_t rx = nullptr;
    if (!startI2s(rx, 16000)) return false;
    static int32_t buf[512];
    bool alive = false;
    const uint32_t until = millis() + 350;
    while (millis() < until) {
      size_t got = 0;
      if (i2s_channel_read(rx, buf, sizeof(buf), &got, 50) != ESP_OK) continue;
      if (millis() + 150 > until)  // the INMP441 needs ~85 ms to wake up: judge only the end
        for (size_t i = 0; i < got / 4; i++) alive |= buf[i] != 0 && buf[i] != -1;
    }
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    return alive;
  }

  bool begin(Source source, uint32_t rate = 16000, adc_atten_t atten = ADC_ATTEN_DB_12) {
    ring_ = static_cast<int16_t *>(heap_caps_calloc(kRing, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    frames_ = xQueueCreate(256, sizeof(Frame));  // ~1 s of frames: loop() can be slow for a while
    if (!ring_ || !frames_) {
      log_e("Mic: no memory for the sample ring");
      return false;
    }
    source_ = source;
    rate_ = wantRate_ = rate;
    atten_ = wantAtten_ = atten;
    if (!startSource()) return false;
    if (source_ == Source::Ky037) {
      pinMode(PIN_MIC_DO, INPUT);  // the module drives DO itself (its own pull-up resistor)
      attachInterruptArg(digitalPinToInterrupt(PIN_MIC_DO), onDoEdge, this, RISING);
    }
    xTaskCreatePinnedToCore(task, "mic", 4096, this, 5, &task_, 0);
    return true;
  }

  // Change the sample rate and/or attenuation (KY-037). The task restarts the source between reads.
  void reconfigure(uint32_t rate, adc_atten_t atten) {
    wantRate_ = rate;
    wantAtten_ = atten;
    reconfigure_ = true;
  }

  Source source() const { return source_; }
  bool digital() const { return source_ == Source::Inmp441; }
  uint32_t rate() const { return rate_; }
  adc_atten_t atten() const { return atten_; }
  // dB SPL for a dBFS level, or NAN when the microphone has no known sensitivity (KY-037).
  float splOf(float dbfs) const { return digital() ? dbfs + kInmp441Spl : NAN; }

  // ---- The sound -------------------------------------------------------------------------------

  // Running sample number: how many samples have ever been stored.
  uint32_t written() const { return written_.load(std::memory_order_acquire); }

  // Copy n samples starting at sample number `from`. False if they're no longer (or not yet) in the ring.
  bool copy(uint32_t from, int16_t *out, uint32_t n) const {
    const uint32_t w = written();
    if (from + n > w || w - from > kRing - 1024) return false;  // 1024 = margin for the writer
    for (uint32_t i = 0; i < n; i++) out[i] = ring_[(from + i) & kMask];
    return true;
  }
  // The newest n samples.
  bool latest(int16_t *out, uint32_t n) const {
    const uint32_t w = written();
    return w >= n && copy(w - n, out, n);
  }

  // The next frame, if any (from loop()).
  bool frame(Frame &f) { return xQueueReceive(frames_, &f, 0) == pdTRUE; }

  // ---- The raw signal (S1) ---------------------------------------------------------------------

  // The slow average of the raw values: KY-037 0-4095 ADC steps, INMP441 24-bit units.
  float biasRaw() const { return dc_; }
  int biasMv() const { return digital() ? 0 : toMv(int(dc_)); }
  int toMv(int raw) const {
    if (digital()) return 0;
    int mv = 0;
    if (cali_ && adc_cali_raw_to_voltage(cali_, raw, &mv) == ESP_OK) return mv;
    return raw * attenTopMv(atten_) / 4095;  // no calibration in eFuse: a straight-line guess
  }
  // Lowest and highest raw value over the last ~100 ms: is the wave hitting the ends (clipping)?
  int rawLow() const { return rawLow_; }
  int rawHigh() const { return rawHigh_; }
  int rawMax() const { return digital() ? (1 << 23) - 1 : 4095; }
  bool clipping() const { return digital() ? (rawHigh_ >= rawMax() - 16 || rawLow_ <= -rawMax() + 16) : (rawLow_ <= 0 || rawHigh_ >= 4095); }
  bool calibrated() const { return cali_ != nullptr; }

  uint32_t overruns() const { return overruns_; }        // the DMA pool overflowed: samples lost
  uint32_t framesDropped() const { return dropped_; }    // loop() was too slow to take frames
  uint32_t measuredRate() const { return measuredRate_; }  // samples counted in the last second

  // ---- The comparator (KY-037's DO) ------------------------------------------------------------

  bool doHigh() const { return source_ == Source::Ky037 && digitalRead(PIN_MIC_DO) == HIGH; }
  uint32_t doRises() const { return doRises_; }
  uint32_t doLastRiseUs() const { return doLastRiseUs_; }

 private:
  static bool startI2s(i2s_chan_handle_t &rx, uint32_t rate) {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_frame_num = 256;
    if (i2s_new_channel(&cc, nullptr, &rx) != ESP_OK) return log_e("Mic: no I2S channel"), false;
    // Philips I2S, 32-bit slots, mono: the INMP441 sends 24 bits, MSB first, in the left slot (L/R
    // pin to GND). The ESP32-S3 drives SCK at rate x 64 (2 slots x 32 bits) = 1.024 MHz at 16 kHz.
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = gpio_num_t(PIN_I2S_MIC_SCK), .ws = gpio_num_t(PIN_I2S_MIC_WS),
                     .dout = I2S_GPIO_UNUSED, .din = gpio_num_t(PIN_I2S_MIC_SD), .invert_flags = {}},
    };
    if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
      i2s_del_channel(rx);
      rx = nullptr;
      return log_e("Mic: I2S set-up failed"), false;
    }
    gpio_pulldown_en(gpio_num_t(PIN_I2S_MIC_SD));  // nothing wired = zeros, not noise
    return true;
  }

  bool startSource() {
    if (source_ == Source::Inmp441) {
      if (!startI2s(rx_, rate_)) return false;
      dcRate_ = 1.0f / 8192.0f;
      gain_ = 1.0f / float(1 << (kI2sShift - 8));
      log_i("Mic: INMP441 on I2S (SCK %d, WS %d, SD %d) at %lu Hz, 24-bit", PIN_I2S_MIC_SCK, PIN_I2S_MIC_WS, PIN_I2S_MIC_SD,
            static_cast<unsigned long>(rate_));
      return true;
    }
    if (adc_continuous_io_to_channel(PIN_MIC_AO, &unit_, &channel_) != ESP_OK || unit_ != ADC_UNIT_1)
      return log_e("Mic: GPIO %d is not an ADC1 pin (use GPIO 1-10)", PIN_MIC_AO), false;
    adc_continuous_handle_cfg_t hc = {};
    hc.max_store_buf_size = 4096;
    hc.conv_frame_size = 256 * SOC_ADC_DIGI_DATA_BYTES_PER_CONV;
    if (adc_continuous_new_handle(&hc, &adc_) != ESP_OK) return log_e("Mic: adc_continuous_new_handle failed"), false;

    adc_digi_pattern_config_t pattern = {};
    pattern.atten = atten_;
    pattern.channel = channel_;
    pattern.unit = unit_;
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
    adc_continuous_config_t cfg = {};
    cfg.pattern_num = 1;
    cfg.adc_pattern = &pattern;
    cfg.sample_freq_hz = rate_;
    cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    if (adc_continuous_config(adc_, &cfg) != ESP_OK) return log_e("Mic: adc_continuous_config failed (rate %lu?)", static_cast<unsigned long>(rate_)), false;

    adc_continuous_evt_cbs_t cbs = {};
    cbs.on_pool_ovf = onOverflow;
    adc_continuous_register_event_callbacks(adc_, &cbs, this);

    // Calibration: each chip's ADC is measured at the factory and the result burnt into eFuse.
    // The "curve fitting" scheme turns raw values into millivolts with it.
    adc_cali_curve_fitting_config_t cc = {};
    cc.unit_id = unit_;
    cc.chan = channel_;
    cc.atten = atten_;
    cc.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cc, &cali_) != ESP_OK) cali_ = nullptr;

    if (adc_continuous_start(adc_) != ESP_OK) return log_e("Mic: adc_continuous_start failed"), false;
    dcRate_ = 1.0f / 4096.0f;  // ~0.25 s to follow the bias at 16 kHz
    gain_ = 16.0f;             // 12-bit steps -> 16-bit
    log_i("Mic: KY-037 AO on GPIO %d (ADC1 channel %d) at %lu Hz, attenuation %s; DO on GPIO %d", PIN_MIC_AO, int(channel_),
          static_cast<unsigned long>(rate_), attenName(atten_), PIN_MIC_DO);
    return true;
  }

  void stopSource() {
    if (rx_) i2s_channel_disable(rx_), i2s_del_channel(rx_), rx_ = nullptr;
    if (adc_) adc_continuous_stop(adc_), adc_continuous_deinit(adc_), adc_ = nullptr;
    if (cali_) adc_cali_delete_scheme_curve_fitting(cali_), cali_ = nullptr;
  }

  static bool IRAM_ATTR onOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *self) {
    auto *mic = static_cast<Mic *>(self);
    mic->overruns_ = mic->overruns_ + 1;
    return false;
  }

  static void IRAM_ATTR onDoEdge(void *arg) {
    auto *self = static_cast<Mic *>(arg);
    self->doRises_ = self->doRises_ + 1;
    self->doLastRiseUs_ = uint32_t(esp_timer_get_time());  // 32 bits: read in one go, wraps safely
  }

  static void task(void *arg) { static_cast<Mic *>(arg)->run(); }

  // One block of raw values from whichever source; 0 when nothing came.
  int readBlock(int32_t *out, int max) {
    if (source_ == Source::Inmp441) {
      size_t got = 0;
      if (!rx_ || i2s_channel_read(rx_, out, max * sizeof(int32_t), &got, 100) != ESP_OK) return 0;
      const int n = int(got / sizeof(int32_t));
      for (int i = 0; i < n; i++) out[i] >>= 8;  // 24 bits, sign kept
      return n;
    }
    uint32_t got = 0;
    if (!adc_ || adc_continuous_read(adc_, adcBuf_, sizeof(adcBuf_), &got, 100) != ESP_OK) return 0;
    int n = 0;
    for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got && n < max; i += SOC_ADC_DIGI_RESULT_BYTES) {
      const auto *d = reinterpret_cast<const adc_digi_output_data_t *>(&adcBuf_[i]);
      if (d->type2.channel == channel_) out[n++] = d->type2.data;
    }
    return n;
  }

  void run() {
    uint32_t windowStart = millis(), counted = 0;
    int low = INT32_MAX, high = INT32_MIN, lowHighCount = 0;
    float sumSq = 0;
    int peak = 0, inChunk = 0;
    bool primed = false;
    for (;;) {
      if (reconfigure_) {
        reconfigure_ = false;
        stopSource();
        rate_ = wantRate_;
        atten_ = wantAtten_;
        primed = false;
        if (!startSource()) log_e("Mic: could not restart");
      }
      const int n = readBlock(raw_, kBlock);
      if (n == 0) {
        if (!adc_ && !rx_) vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      uint32_t w = written_.load(std::memory_order_relaxed);
      for (int i = 0; i < n; i++) {
        const int raw = raw_[i];
        if (!primed) dc_ = raw, primed = true;
        dc_ += (raw - dc_) * dcRate_;
        low = raw < low ? raw : low;
        high = raw > high ? raw : high;
        float v = (raw - dc_) * gain_;
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        const int16_t s = int16_t(v);
        ring_[w & kMask] = s;
        w++;
        sumSq += float(s) * s;
        const int a = s < 0 ? -s : s;
        peak = a > peak ? a : peak;
        if (++inChunk == kChunk) {
          const Frame f{w, sqrtf(sumSq / kChunk), int16_t(peak > 32767 ? 32767 : peak)};
          if (xQueueSend(frames_, &f, 0) != pdTRUE) dropped_ = dropped_ + 1;  // loop() fell 1 s behind
          sumSq = 0, peak = 0, inChunk = 0;
        }
        if (++lowHighCount >= int(rate_ / 10)) {
          rawLow_ = low;
          rawHigh_ = high;
          low = INT32_MAX, high = INT32_MIN, lowHighCount = 0;
        }
        counted++;
      }
      written_.store(w, std::memory_order_release);
      const uint32_t now = millis();
      if (now - windowStart >= 1000) measuredRate_ = counted * 1000 / (now - windowStart), counted = 0, windowStart = now;
    }
  }

  static constexpr int kBlock = 256;
  int16_t *ring_ = nullptr;
  QueueHandle_t frames_ = nullptr;
  TaskHandle_t task_ = nullptr;
  i2s_chan_handle_t rx_ = nullptr;
  adc_continuous_handle_t adc_ = nullptr;
  adc_cali_handle_t cali_ = nullptr;
  adc_unit_t unit_ = ADC_UNIT_1;
  adc_channel_t channel_ = ADC_CHANNEL_0;
  uint8_t adcBuf_[kBlock * SOC_ADC_DIGI_DATA_BYTES_PER_CONV];
  int32_t raw_[kBlock];
  Source source_ = Source::Ky037;
  uint32_t rate_ = 16000, wantRate_ = 16000;
  adc_atten_t atten_ = ADC_ATTEN_DB_12, wantAtten_ = ADC_ATTEN_DB_12;
  float dcRate_ = 1.0f / 4096.0f, gain_ = 16.0f;
  volatile bool reconfigure_ = false;
  std::atomic<uint32_t> written_{0};
  volatile float dc_ = 0;
  volatile int rawLow_ = 0, rawHigh_ = 0;
  volatile uint32_t overruns_ = 0, dropped_ = 0, measuredRate_ = 0;
  volatile uint32_t doRises_ = 0, doLastRiseUs_ = 0;
};

}  // namespace snd
