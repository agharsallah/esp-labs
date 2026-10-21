// R8's reader task: the only code that touches the RC522 (core 0, SPI3, as in R1-R7). It
//   - polls for cards about 12 times a second and turns each new card into a "show": who lives
//     on it, its name, whether it was just added
//   - keeps the reader healthy the way R7 learned to: falls back from the IRQ wire to polling,
//     puts the chip's settings back after a 3.3 V dip, hard-resets a chip that stopped answering
// No card data is read or written here: R8 only needs the UID.
// loop() copies the latest Stage for the screen; the web side takes the messages from the outbox.
#pragma once

#include <ArduinoJson.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "friends.h"
#include "iso14443a.h"
#include "mfrc522.h"
#include "pins.h"

// What the screen and the app need to know right now.
struct Stage {
  uint32_t version = 0;
  // Reader health, as in R7.
  uint8_t chipVersion = 0;
  bool irqMode = false;
  uint16_t restores = 0, lost = 0;
  char problem[72] = "";
  uint32_t problemAt = 0;  // millis()
  bool present = false;
  char presentUid[21] = "";
  // The last show: a tap on the reader, or a card the app asked to show.
  uint32_t showSeq = 0;
  uint32_t showAt = 0;  // millis()
  char uid[21] = "";
  char character[12] = "";
  char name[16] = "";
  uint8_t number = 0;
  uint32_t taps = 0;
  bool isNew = false;   // first tap of this card: it was just added
  bool full = false;    // a new card, but the list is full (12)
  bool fromApp = false; // shown from the app, not tapped
};

class TapReader {
 public:
  explicit TapReader(friends::List &list) : list_(list) {}

  void begin() { xTaskCreatePinnedToCore(task, "taps", 8 * 1024, this, 2, nullptr, 0); }

  bool update(Stage &mine) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mine.version == stage_.version) return false;
    mine = stage_;
    return true;
  }

  // From the web server: show a card's friend on the screen and in every app, as if tapped
  // (the tap count stays). false when the card isn't in the list.
  bool show(const uint8_t *uid, uint8_t len) {
    friends::Card c;
    if (!list_.get(uid, len, c)) return false;
    announce(c, false, false, true);
    return true;
  }

  void requestChipReset() { resetWanted_ = true; }

  bool takeMessage(String &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.empty()) return false;
    out = std::move(outbox_.front());
    outbox_.erase(outbox_.begin());
    return true;
  }

 private:
  static void task(void *self) { static_cast<TapReader *>(self)->run(); }

  void run() {
    spi_ = new SPIClass(HSPI);  // the S3's SPI3: the screen keeps SPI2 to itself
    const uint8_t v = chip_.begin(*spi_, {PIN_RFID_SS, PIN_RFID_SCK, PIN_RFID_MOSI, PIN_RFID_MISO, PIN_RFID_RST, PIN_RFID_IRQ}, nullptr);
    if (chip_.hasIrqPin()) chip_.setWaitMode(rc522::Mfrc522::WaitMode::Irq);
    log_i("RC522: VersionReg 0x%02X (%s)", v, rc522::versionName(v));
    chipLost_ = !alive(v);
    if (chipLost_) problem("reader chip not answering: check its 3.3V, GND and MISO wires");
    publish([&](Stage &s) { s.chipVersion = v, s.irqMode = chip_.waitMode() == rc522::Mfrc522::WaitMode::Irq; });
    for (;;) {
      if (resetWanted_.exchange(false)) {
        chip_.hardReset();
        const uint8_t again = chip_.read(rc522::VersionReg);
        chipLost_ = !alive(again);
        publish([&](Stage &s) { s.chipVersion = again; });
        log_i("RC522 hard reset from the app: 0x%02X", again);
      }
      if (millis() - lastHealth_ >= 2000) health();
      if (!chipLost_) pollOnce();
      vTaskDelay(pdMS_TO_TICKS(80));
    }
  }

  static bool alive(uint8_t v) { return v != 0x00 && v != 0xFF; }

  template <typename F>
  void publish(F change) {
    std::lock_guard<std::mutex> lock(mutex_);
    change(stage_);
    stage_.version++;
  }

  void send(JsonDocument &data) {
    JsonDocument msg;
    msg["type"] = "tap";
    msg["data"] = data;
    String text;
    serializeJson(msg, text);
    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.size() >= 16) outbox_.erase(outbox_.begin());
    outbox_.push_back(std::move(text));
  }

  void problem(const char *what) {
    log_w("reader: %s", what);
    publish([&](Stage &s) {
      strlcpy(s.problem, what, sizeof(s.problem));
      s.problemAt = millis();
    });
  }

  // ---- Cards -----------------------------------------------------------------------------------

  void pollOnce() {
    iso14443a::Uid u;
    const bool found = iso14443a::wakeAndSelect(chip_, u) == rc522::Status::Ok;
    if (found) iso14443a::halt(chip_);
    checkIrq();
    const bool isNew = found && (!present_ || !u.sameAs(current_));
    if (found) misses_ = 0;
    if (isNew) {
      present_ = true, current_ = u;
      char hex[21];
      friends::uidToHex(u.bytes, u.size, hex, sizeof(hex));
      publish([&](Stage &s) { s.present = true, strlcpy(s.presentUid, hex, sizeof(s.presentUid)); });
      onTap(u, hex);
    } else if (!found && present_ && ++misses_ >= 3) {
      present_ = false;
      publish([](Stage &s) { s.present = false, s.presentUid[0] = 0; });
    }
  }

  void onTap(const iso14443a::Uid &u, const char *hex) {
    friends::Card c;
    const friends::List::TapResult r = list_.tap(u.bytes, u.size, c);
    if (r == friends::List::TapResult::Full) {
      memcpy(c.uid, u.bytes, u.size);
      c.uidLen = u.size;
      log_w("card %s: the list is full (12 cards), forget one in the app", hex);
    }
    announce(c, r == friends::List::TapResult::Added, r == friends::List::TapResult::Full, false);
  }

  // One show: the screen's stage and a "tap" message to every app.
  void announce(const friends::Card &c, bool isNew, bool full, bool fromApp) {
    char hex[21];
    friends::uidToHex(c.uid, c.uidLen, hex, sizeof(hex));
    uint32_t seq = 0;
    publish([&](Stage &s) {
      seq = ++s.showSeq;
      s.showAt = millis();
      strlcpy(s.uid, hex, sizeof(s.uid));
      strlcpy(s.character, c.character, sizeof(s.character));
      strlcpy(s.name, c.name, sizeof(s.name));
      s.number = c.number, s.taps = c.taps, s.isNew = isNew, s.full = full, s.fromApp = fromApp;
    });
    const cast::Character *who = cast::find(c.character);
    log_i("%s card %u (%s): %s%s", fromApp ? "show" : "tap", c.number, hex, who ? (c.name[0] ? c.name : who->name) : "nobody yet",
          isNew ? ", new card" : full ? ", list full" : "");
    // Char arrays go in as const char *: ArduinoJson then copies them (see R7, log entry 44).
    JsonDocument d;
    d["seq"] = seq;
    d["uid"] = static_cast<const char *>(hex);
    d["character"] = static_cast<const char *>(c.character);
    d["name"] = static_cast<const char *>(c.name);
    d["number"] = c.number;
    d["taps"] = c.taps;
    d["isNew"] = isNew;
    d["full"] = full;
    d["fromApp"] = fromApp;
    send(d);
  }

  // ---- Reader health (R7's lessons) -------------------------------------------------------------

  // The IRQ wire stayed quiet although the chip finished: poll the chip instead.
  void checkIrq() {
    if (!chip_.takeIrqMissed() || chip_.waitMode() != rc522::Mfrc522::WaitMode::Irq) return;
    chip_.setWaitMode(rc522::Mfrc522::WaitMode::Poll);
    publish([](Stage &s) { s.irqMode = false; });
    char msg[72];
    snprintf(msg, sizeof(msg), "IRQ wire on GPIO %d stays quiet: polling instead", PIN_RFID_IRQ);
    problem(msg);
  }

  // Every 2 s: settings lost after a 3.3 V dip? Chip silent? Then put it right.
  void health() {
    lastHealth_ = millis();
    const uint8_t v = chip_.read(rc522::VersionReg);
    if (alive(v) && !chipLost_) {
      if (chip_.settingsLost()) {
        chip_.restoreSettings();
        publish([](Stage &s) { s.restores++; });
        problem("the reader chip reset itself (3.3V dip?): settings restored");
      }
      return;
    }
    if (alive(v) == !chipLost_) return;
    if (!alive(v)) {
      chip_.hardReset();
      const uint8_t again = chip_.read(rc522::VersionReg);
      chipLost_ = !alive(again);
      publish([&](Stage &s) { s.chipVersion = again, s.lost++; });
      problem(chipLost_ ? "reader chip stopped answering: check its 3.3V, GND and MISO wires"
                        : "reader chip had hung: a hard reset brought it back");
    } else {
      chipLost_ = false;
      log_i("RC522 answering again: 0x%02X", v);
      publish([&](Stage &s) { s.chipVersion = v; });
    }
  }

  friends::List &list_;
  rc522::Mfrc522 chip_;
  SPIClass *spi_ = nullptr;
  std::mutex mutex_;
  Stage stage_;
  std::vector<String> outbox_;
  std::atomic<bool> resetWanted_{false};
  bool chipLost_ = false, present_ = false;
  iso14443a::Uid current_;
  uint8_t misses_ = 0;
  uint32_t lastHealth_ = 0;
};
