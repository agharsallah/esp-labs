// B5's own characteristics, added to the Bench service from B3 (same UUID base, 0010 and up):
//
//   Canvas   ...0010  write without response   the sketch protocol (canvas_protocol.h)
//   Sensors  ...0011  read, notify             every 200 ms while subscribed, 12 bytes:
//                                              int16 temperature x 100 · uint32 touch (GPIO 1) ·
//                                              uint32 free heap · int8 RSSI (dBm) · uint8 sequence
//   Speed    ...0012  write, notify            write 01 + uint32 bytes: the board notifies that many
//                                              bytes as fast as it can, in packets of MTU - 3 bytes
//                                              starting with a uint16 sequence number. Then one
//                                              summary packet: FF FF · uint32 bytes · uint32 ms ·
//                                              uint16 MTU · uint16 interval (1.25 ms units) · uint8 PHY.
//                                              Write 00 to stop early.
//   Link     ...0013  write                    one byte: 1 fast interval · 2 slow interval ·
//                                              3 2M PHY · 4 1M PHY · 5 longer link-layer packets (251 B)
//
// The NimBLE task only copies bytes into queues here. loop() draws the sketch (tick), and the
// speed test has its own small task that pushes notifications until the stack's buffers are full.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/queue.h>

#include "bench_service.h"
#include "ble_link.h"
#include "server_link.h"

namespace b5 {

enum Chr : uint8_t { Canvas, Sensors, Speed, LinkOps, kCount };

inline const char *chrName(int c) {
  static const char *const names[] = {"Canvas", "Sensors", "Speed", "Link"};
  return names[c];
}

// One write to the Canvas characteristic, waiting for loop().
struct CanvasWrite {
  uint8_t len;
  uint8_t data[244];  // the largest write that fits one 247-byte MTU
};

// One finished speed run, for the speed view.
struct SpeedRun {
  uint32_t bytes = 0, ms = 0, retries = 0;
  uint16_t mtu = 0, interval = 0;
  uint8_t phy = 1;
  float kBps() const { return ms ? bytes / float(ms) : 0; }  // bytes per ms = kB/s
};

// What the views show: copied under the lock.
struct State {
  bool subscribed[kCount] = {};
  uint32_t canvasWrites = 0, canvasBytes = 0, dropped = 0;
  bool running = false;
  uint32_t target = 0, sent = 0, startedAt = 0, retries = 0;
  SpeedRun runs[6];
  int runCount = 0;  // total ever; runs[] keeps the last 6
  uint32_t sensorNotifies = 0;
};

class Extras : public NimBLECharacteristicCallbacks {
 public:
  void begin(NimBLEService *svc) {
    queue_ = xQueueCreate(16, sizeof(CanvasWrite));
    chr_[Canvas] = svc->createCharacteristic(bench::uuid(0x10), NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE, 244);
    describe(chr_[Canvas], "Canvas: sketch commands");
    chr_[Sensors] = svc->createCharacteristic(bench::uuid(0x11), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 12);
    describe(chr_[Sensors], "Sensors: temp, touch, heap, RSSI");
    chr_[Speed] = svc->createCharacteristic(bench::uuid(0x12), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY, 244);
    describe(chr_[Speed], "Speed test");
    chr_[LinkOps] = svc->createCharacteristic(bench::uuid(0x13), NIMBLE_PROPERTY::WRITE, 1);
    describe(chr_[LinkOps], "Link: 1 fast 2 slow 3 2M 4 1M 5 DLE");
    for (auto *c : chr_) c->setCallbacks(this);
    uint8_t zero[12] = {};
    chr_[Sensors]->setValue(zero, sizeof(zero));
  }

  // ---- loop() side ----------------------------------------------------------------------------

  // Next queued Canvas write, if any.
  bool takeCanvas(CanvasWrite &w) { return queue_ && xQueueReceive(queue_, &w, 0) == pdTRUE; }

  // Pending Link operation (0 = none), taken once.
  uint8_t takeLinkOp() {
    portENTER_CRITICAL(&lock_);
    const uint8_t op = linkOp_;
    linkOp_ = 0;
    portEXIT_CRITICAL(&lock_);
    return op;
  }

  // Pending speed start (bytes) or 0; taken once.
  uint32_t takeSpeedStart() {
    portENTER_CRITICAL(&lock_);
    const uint32_t b = speedStart_;
    speedStart_ = 0;
    portEXIT_CRITICAL(&lock_);
    return b;
  }

  // Build and (if subscribed) notify the 12-byte sensors packet.
  void sendSensors(float tempC, uint32_t touch, uint32_t heap, int8_t rssi) {
    uint8_t p[12];
    const int16_t t = int16_t(tempC * 100);
    memcpy(p, &t, 2);
    memcpy(p + 2, &touch, 4);
    memcpy(p + 6, &heap, 4);
    p[10] = uint8_t(rssi);
    p[11] = seq_++;
    chr_[Sensors]->setValue(p, sizeof(p));
    if (!subscribed(Sensors)) return;
    if (chr_[Sensors]->notify()) {
      ble::countTx(sizeof(p));
      portENTER_CRITICAL(&lock_);
      state_.sensorNotifies++;
      portEXIT_CRITICAL(&lock_);
    }
  }

  // Start a speed run in its own task. False when one is running or nobody listens.
  bool startSpeed(uint32_t bytes) {
    const ble::LinkState s = ble::link().get();
    if (s.radio != ble::Radio::Connected) return BLE_NOTE(Warn, "speed: connect first"), false;
    if (!subscribed(Speed)) return BLE_NOTE(Warn, "speed: subscribe to Speed (ba5e0012) first"), false;
    portENTER_CRITICAL(&lock_);
    const bool busy = state_.running;
    if (!busy) {
      state_.running = true;
      state_.target = bytes;
      state_.sent = 0;
      state_.retries = 0;
      state_.startedAt = millis();
      stopRequested_ = false;
    }
    portEXIT_CRITICAL(&lock_);
    if (busy) return BLE_NOTE(Warn, "speed: a run is already going"), false;
    run_ = SpeedRun{};
    run_.mtu = s.peer.mtu;
    run_.interval = s.peer.interval;
    run_.phy = s.peer.txPhy;
    handle_ = s.peer.handle;
    BLE_NOTE(Tx, "speed: %lu bytes in %u-byte packets", static_cast<unsigned long>(bytes), s.peer.mtu - 3);
    // Core 1 at priority 1, like loop(): the NimBLE host on core 0 keeps its own pace.
    if (xTaskCreatePinnedToCore(speedTask, "speed", 4096, this, 1, nullptr, 1) != pdPASS) {
      portENTER_CRITICAL(&lock_);
      state_.running = false;
      portEXIT_CRITICAL(&lock_);
      return BLE_NOTE(Bad, "speed: no memory for the task"), false;
    }
    return true;
  }

  void stopSpeed() {
    portENTER_CRITICAL(&lock_);
    stopRequested_ = true;
    portEXIT_CRITICAL(&lock_);
  }

  State state() {
    portENTER_CRITICAL(&lock_);
    State s = state_;
    portEXIT_CRITICAL(&lock_);
    return s;
  }

  // ---- NimBLE callbacks (NimBLE task) ---------------------------------------------------------

  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    const NimBLEAttValue v = c->getValue();
    ble::countRx(v.size());
    if (c == chr_[Canvas]) {
      CanvasWrite w;
      w.len = uint8_t(std::min<size_t>(v.size(), sizeof(w.data)));
      memcpy(w.data, v.data(), w.len);
      const bool queued = xQueueSend(queue_, &w, 0) == pdTRUE;
      portENTER_CRITICAL(&lock_);
      state_.canvasWrites++;
      state_.canvasBytes += w.len;
      if (!queued) state_.dropped++;
      portEXIT_CRITICAL(&lock_);
      // No activity line per stroke: a sketch sends ~30 writes a second.
    } else if (c == chr_[Speed]) {
      if (v.size() >= 5 && v.data()[0] == 0x01) {
        uint32_t bytes;
        memcpy(&bytes, v.data() + 1, 4);
        bytes = std::max<uint32_t>(1000, std::min<uint32_t>(bytes, 1'000'000));
        portENTER_CRITICAL(&lock_);
        speedStart_ = bytes;
        portEXIT_CRITICAL(&lock_);
        BLE_NOTE(Rx, "write Speed: start %lu bytes", static_cast<unsigned long>(bytes));
      } else if (v.size() >= 1 && v.data()[0] == 0x00) {
        stopSpeed();
        BLE_NOTE(Rx, "write Speed: stop");
      } else {
        BLE_NOTE(Warn, "Speed wants 01 + uint32, or 00");
      }
    } else if (c == chr_[LinkOps]) {
      if (v.size() != 1 || v.data()[0] < 1 || v.data()[0] > 5) return BLE_NOTE(Warn, "Link wants one byte, 1-5");
      portENTER_CRITICAL(&lock_);
      linkOp_ = v.data()[0];
      portEXIT_CRITICAL(&lock_);
      BLE_NOTE(Rx, "write Link: %u", v.data()[0]);
    }
  }

  void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &, uint16_t subValue) override {
    for (int i = 0; i < kCount; i++)
      if (chr_[i] == c) {
        portENTER_CRITICAL(&lock_);
        state_.subscribed[i] = subValue != 0;
        portEXIT_CRITICAL(&lock_);
        BLE_NOTE(Rx, "%s %s", subValue ? "subscribe" : "unsubscribe", chrName(i));
      }
  }

  // A new connection starts with nobody subscribed.
  void resetSubscriptions() {
    portENTER_CRITICAL(&lock_);
    for (bool &s : state_.subscribed) s = false;
    portEXIT_CRITICAL(&lock_);
  }

 private:
  static void describe(NimBLECharacteristic *c, const char *text) {
    c->createDescriptor("2901", NIMBLE_PROPERTY::READ, strlen(text))->setValue(text);
  }

  bool subscribed(Chr c) {
    portENTER_CRITICAL(&lock_);
    const bool s = state_.subscribed[c];
    portEXIT_CRITICAL(&lock_);
    return s;
  }

  // The speed test. notify() fails when the stack's buffers are full (the radio hasn't sent the
  // earlier packets yet); we wait one tick and try again. That back-pressure IS the throughput.
  static void speedTask(void *arg) {
    auto *self = static_cast<Extras *>(arg);
    const uint32_t start = millis();
    const size_t size = std::min<size_t>(self->run_.mtu - 3, 244);
    uint8_t buf[244];
    uint16_t seq = 0;
    uint32_t sent = 0, retries = 0;
    for (;;) {
      portENTER_CRITICAL(&self->lock_);
      const bool stop = self->stopRequested_;
      const uint32_t target = self->state_.target;
      portEXIT_CRITICAL(&self->lock_);
      if (stop || sent >= target || ble::link().get().radio != ble::Radio::Connected) break;
      const size_t n = std::min<size_t>(size, std::max<uint32_t>(target - sent, 2));
      buf[0] = uint8_t(seq), buf[1] = uint8_t(seq >> 8);
      memset(buf + 2, uint8_t(seq), n - 2);
      if (self->chr_[Speed]->notify(buf, n, self->handle_)) {
        sent += n;
        seq = seq == 0xFFFE ? 0 : seq + 1;  // FF FF is kept for the summary
        if ((seq & 15) == 0) {
          portENTER_CRITICAL(&self->lock_);
          self->state_.sent = sent;
          self->state_.retries = retries;
          portEXIT_CRITICAL(&self->lock_);
        }
      } else {
        retries++;
        vTaskDelay(1);
      }
    }
    const uint32_t ms = std::max<uint32_t>(millis() - start, 1);
    SpeedRun r = self->run_;
    r.bytes = sent;
    r.ms = ms;
    r.retries = retries;
    // The summary, so the app can compare its own clock with ours.
    uint8_t sum[15] = {0xFF, 0xFF};
    memcpy(sum + 2, &sent, 4);
    memcpy(sum + 6, &ms, 4);
    memcpy(sum + 10, &r.mtu, 2);
    memcpy(sum + 12, &r.interval, 2);
    sum[14] = r.phy;
    for (int i = 0; i < 50 && !self->chr_[Speed]->notify(sum, sizeof(sum), self->handle_); i++) vTaskDelay(2);
    ble::link().edit([&](ble::LinkState &s) { s.txOps += sent / size, s.txBytes += sent; });
    portENTER_CRITICAL(&self->lock_);
    self->state_.running = false;
    self->state_.sent = sent;
    self->state_.runs[self->state_.runCount % 6] = r;
    self->state_.runCount++;
    portEXIT_CRITICAL(&self->lock_);
    BLE_NOTE(Good, "speed: %lu B in %lu ms = %.1f kB/s", static_cast<unsigned long>(sent), static_cast<unsigned long>(ms), r.kBps());
    vTaskDelete(nullptr);
  }

  NimBLECharacteristic *chr_[kCount] = {};
  QueueHandle_t queue_ = nullptr;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  State state_;
  SpeedRun run_;
  uint16_t handle_ = 0xFFFF;
  uint8_t linkOp_ = 0, seq_ = 0;
  uint32_t speedStart_ = 0;
  bool stopRequested_ = false;
};

}  // namespace b5
