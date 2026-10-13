// The KY-037 on the ESP32-S3: its AO pin sampled 16 000 times a second, its DO pin watched.
//
// Why not analogRead() in loop()? It takes one sample when asked, and loop() also draws the screen
// (~40 ms a frame), so we would get a few samples a frame, unevenly spaced: useless for sound.
// Instead the ADC runs in "continuous" mode: its own timer starts every conversion and the DMA
// (direct memory access) moves each result into RAM with no CPU involved. A sampler task on core 0
// (the screen loop runs on core 1) picks them up in chunks of 256 and:
//
//   1. removes the DC bias. The microphone's signal sits on top of a steady voltage (the "bias",
//      somewhere between 0 and 3.3 V; S1 shows yours). A very slow average follows it, and every
//      sample becomes "raw - average": a wave around 0.
//   2. scales it to 16-bit (x16: the ADC has 12 bits, 0-4095) and stores it in a 4-second ring in
//      PSRAM. Each sample has a running number (`written()`): the scope, FFT and recorder copy
//      stretches out of the ring by number, whenever they like, without stopping the sampler.
//   3. every 64 samples (4 ms) measures the chunk's RMS and peak and queues it as a Frame. The
//      lab shell turns frames into the level meter and the clap, knock and beat detectors.
//
// The comparator (DO): the LM393 on the module compares the microphone's voltage with the trim
// pot's and pulls DO high or low. An interrupt counts every rising edge; S1 lets you tune the pot.
//
// Only the ADC1 unit works in continuous mode alongside Wi-Fi, hence PIN_MIC_AO on GPIO 1-10.
#pragma once

#include <Arduino.h>
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

struct Frame {
  uint32_t end;   // the sample number just after this frame
  float rms;      // int16 units
  int16_t peak;   // the biggest |sample|
};

// Attenuation: how much the ADC divides the input before measuring. More = a wider voltage range,
// fewer millivolts per step. The S3's usable ranges, roughly: 0 dB to ~0.95 V, 2.5 dB ~1.25 V,
// 6 dB ~1.75 V, 12 dB ~3.1 V. The bias must sit inside the range, with room for the wave.
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

  bool begin(uint32_t rate = 16000, adc_atten_t atten = ADC_ATTEN_DB_12) {
    ring_ = static_cast<int16_t *>(heap_caps_calloc(kRing, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    frames_ = xQueueCreate(256, sizeof(Frame));  // ~1 s of frames: loop() can be slow for a while
    if (!ring_ || !frames_) {
      log_e("Mic: no memory for the sample ring");
      return false;
    }
    rate_ = rate;
    atten_ = atten;
    if (adc_continuous_io_to_channel(PIN_MIC_AO, &unit_, &channel_) != ESP_OK || unit_ != ADC_UNIT_1) {
      log_e("Mic: GPIO %d is not an ADC1 pin (use GPIO 1-10)", PIN_MIC_AO);
      return false;
    }
    if (!startAdc()) return false;

    pinMode(PIN_MIC_DO, INPUT);  // the module drives DO itself (its own pull-up resistor)
    attachInterruptArg(digitalPinToInterrupt(PIN_MIC_DO), onDoEdge, this, RISING);

    xTaskCreatePinnedToCore(task, "mic", 4096, this, 5, &task_, 0);
    log_i("Mic: AO on GPIO %d (ADC1 channel %d) at %lu Hz, attenuation %s; DO on GPIO %d", PIN_MIC_AO, int(channel_),
          static_cast<unsigned long>(rate_), attenName(atten_), PIN_MIC_DO);
    return true;
  }

  // Change the sample rate and/or attenuation. The task restarts the ADC between two reads.
  void reconfigure(uint32_t rate, adc_atten_t atten) {
    wantRate_ = rate;
    wantAtten_ = atten;
    reconfigure_ = true;
  }

  uint32_t rate() const { return rate_; }
  adc_atten_t atten() const { return atten_; }

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

  float biasRaw() const { return dc_; }        // the slow average, 0-4095
  int biasMv() const { return toMv(int(dc_)); }
  int toMv(int raw) const {
    int mv = 0;
    if (cali_ && adc_cali_raw_to_voltage(cali_, raw, &mv) == ESP_OK) return mv;
    return raw * attenTopMv(atten_) / 4095;  // no calibration in eFuse: a straight-line guess
  }
  // Lowest and highest raw value over the last ~100 ms: is the wave hitting 0 or 4095 (clipping)?
  int rawLow() const { return rawLow_; }
  int rawHigh() const { return rawHigh_; }
  bool calibrated() const { return cali_ != nullptr; }

  uint32_t overruns() const { return overruns_; }        // the DMA pool overflowed: samples lost
  uint32_t framesDropped() const { return dropped_; }    // loop() was too slow to take frames
  uint32_t measuredRate() const { return measuredRate_; }  // samples counted in the last second

  // ---- The comparator (DO) ---------------------------------------------------------------------

  bool doHigh() const { return digitalRead(PIN_MIC_DO) == HIGH; }
  uint32_t doRises() const { return doRises_; }
  uint32_t doLastRiseUs() const { return doLastRiseUs_; }

 private:
  bool startAdc() {
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
    return true;
  }

  void stopAdc() {
    adc_continuous_stop(adc_);
    adc_continuous_deinit(adc_);
    adc_ = nullptr;
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

  void run() {
    static uint8_t buf[256 * SOC_ADC_DIGI_DATA_BYTES_PER_CONV];
    uint32_t windowStart = millis(), counted = 0;
    int low = 4095, high = 0, lowHighCount = 0;
    float sumSq = 0;
    int peak = 0, inChunk = 0;
    bool primed = false;
    for (;;) {
      if (reconfigure_) {
        reconfigure_ = false;
        stopAdc();
        rate_ = wantRate_;
        atten_ = wantAtten_;
        primed = false;
        if (!startAdc()) log_e("Mic: could not restart the ADC");
        else log_i("Mic: %lu Hz, attenuation %s", static_cast<unsigned long>(rate_), attenName(atten_));
      }
      uint32_t got = 0;
      if (!adc_ || adc_continuous_read(adc_, buf, sizeof(buf), &got, 100) != ESP_OK) {
        if (!adc_) vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      uint32_t w = written_.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got; i += SOC_ADC_DIGI_RESULT_BYTES) {
        const auto *d = reinterpret_cast<const adc_digi_output_data_t *>(&buf[i]);
        if (d->type2.channel != channel_) continue;  // not ours (never, with one channel; cheap to check)
        const int raw = d->type2.data;
        if (!primed) dc_ = raw, primed = true;
        dc_ += (raw - dc_) * (1.0f / 4096.0f);  // ~0.25 s to follow the bias at 16 kHz
        low = raw < low ? raw : low;
        high = raw > high ? raw : high;
        float v = (raw - dc_) * 16.0f;
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
          low = 4095, high = 0, lowHighCount = 0;
        }
        counted++;
      }
      written_.store(w, std::memory_order_release);
      const uint32_t now = millis();
      if (now - windowStart >= 1000) measuredRate_ = counted * 1000 / (now - windowStart), counted = 0, windowStart = now;
    }
  }

  int16_t *ring_ = nullptr;
  QueueHandle_t frames_ = nullptr;
  TaskHandle_t task_ = nullptr;
  adc_continuous_handle_t adc_ = nullptr;
  adc_cali_handle_t cali_ = nullptr;
  adc_unit_t unit_ = ADC_UNIT_1;
  adc_channel_t channel_ = ADC_CHANNEL_0;
  uint32_t rate_ = 16000, wantRate_ = 16000;
  adc_atten_t atten_ = ADC_ATTEN_DB_12, wantAtten_ = ADC_ATTEN_DB_12;
  volatile bool reconfigure_ = false;
  std::atomic<uint32_t> written_{0};
  volatile float dc_ = 0;
  volatile int rawLow_ = 0, rawHigh_ = 0;
  volatile uint32_t overruns_ = 0, dropped_ = 0, measuredRate_ = 0;
  volatile uint32_t doRises_ = 0, doLastRiseUs_ = 0;
};

}  // namespace snd
