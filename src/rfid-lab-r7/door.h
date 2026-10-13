// R7's reader task: the only code that touches the RC522 (core 0). It
//   - polls for cards and checks each tap against the card list (registry.h + access_rules.h)
//   - drives the simulated door: OPEN for 5 s when granted, DENIED for 2 s otherwise
//   - runs the app's card jobs, one at a time: enroll, label, wipe, inspect, open, chip reset
// Results and events become JSON messages in an outbox; loop() sends them over the WebSocket.
//
// Guard rails, as in R4/R5: only the factory key, only the data blocks of sectors 1 and 2
// (4, 5, 6, 8, 9, 10), never block 0 or a sector trailer, and only where the access bits let
// key A write. "Wipe" means: those data blocks back to zeros. Keys and trailers are never touched.
#pragma once

#include <ArduinoJson.h>
#include <freertos/queue.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "access_rules.h"
#include "iso14443a.h"
#include "mfrc522.h"
#include "mifare_classic.h"
#include "pins.h"
#include "registry.h"

namespace doorjson {

inline void clockNow(door::Clock &c) {
  const time_t now = time(nullptr);
  c.synced = now > 1'700'000'000;
  c.now = uint32_t(now);
  struct tm lt;
  localtime_r(&now, &lt);
  c.hour = uint8_t(lt.tm_hour);
}

// One card as the app sees it, including what would happen if it were tapped right now.
inline void card(JsonObject o, const door::Card &c, const door::Clock &clk, bool lockdown) {
  char uid[21];
  door::uidToHex(c.uid, c.uidLen, uid, sizeof(uid));
  o["uid"] = uid;
  o["name"] = c.name;
  o["role"] = door::roleName(c.role);
  o["blocked"] = c.blocked;
  o["validUntil"] = c.validUntil;
  o["fromHour"] = c.fromHour;
  o["toHour"] = c.toHour;
  o["usesLeft"] = c.usesLeft;
  o["added"] = c.added;
  o["lastSeen"] = c.lastSeen;
  o["taps"] = c.taps;
  o["now"] = door::decisionName(door::decide(&c, clk, lockdown));
}

}  // namespace doorjson

struct DoorEvent {
  uint32_t seq = 0;
  uint32_t at = 0;  // unix seconds (0 before the clock syncs)
  char uid[21] = "";
  char name[24] = "";
  char what[24] = "";      // "tap", "enrolled", "label written", "card wiped", "opened from the app", ...
  char decision[16] = "";  // for taps: granted, blocked, ...
};

struct DoorState {
  enum class Mode : uint8_t { Locked, Open, Denied };
  uint32_t version = 0;
  uint8_t chipVersion = 0;
  Mode mode = Mode::Locked;
  uint32_t modeUntil = 0;  // millis()
  char lastName[24] = "", lastUid[21] = "", lastWhy[16] = "";
  bool present = false;
  char presentUid[21] = "";
  bool enrolling = false;
  uint32_t enrollUntil = 0;  // millis()
  char enrollName[24] = "";
  const char *busy = nullptr;
};

class Door {
 public:
  enum class Job : uint8_t { Enroll, CancelEnroll, Label, Wipe, Inspect, Open, ChipReset };
  struct Request {
    Job job = Job::Open;
    uint32_t id = 0;
    uint8_t uid[10] = {};
    uint8_t uidLen = 0;
    char name[24] = "";
    door::Role role = door::Role::Resident;
  };

  static constexpr uint32_t kOpenMs = 5000, kDeniedMs = 2000, kEnrollMs = 20000;

  explicit Door(Registry &reg) : reg_(reg) {}

  void begin() {
    mailbox_ = xQueueCreate(6, sizeof(Request));
    xTaskCreatePinnedToCore(task, "door", 12 * 1024, this, 2, nullptr, 0);
  }

  // From any task. Returns the job id (the app waits for a "job" message with it), 0 when busy.
  uint32_t submit(Request r) {
    r.id = nextId_++;
    return xQueueSend(mailbox_, &r, 0) == pdTRUE ? r.id : 0;
  }

  bool update(DoorState &mine) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mine.version == state_.version) return false;
    mine = state_;
    return true;
  }

  std::vector<DoorEvent> events(uint32_t since) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DoorEvent> out;
    for (const DoorEvent &e : events_)
      if (e.seq > since) out.push_back(e);
    return out;
  }

  // The next WebSocket message (JSON text), if any.
  bool takeMessage(String &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.empty()) return false;
    out = std::move(outbox_.front());
    outbox_.erase(outbox_.begin());
    return true;
  }

 private:
  static void task(void *self) { static_cast<Door *>(self)->run(); }

  void run() {
    spi_ = new SPIClass(HSPI);
    const uint8_t v = chip_.begin(*spi_, {PIN_RFID_SS, PIN_RFID_SCK, PIN_RFID_MOSI, PIN_RFID_MISO, PIN_RFID_RST, PIN_RFID_IRQ}, nullptr);
    if (chip_.hasIrqPin()) chip_.setWaitMode(rc522::Mfrc522::WaitMode::Irq);
    log_i("RC522: VersionReg 0x%02X (%s)", v, rc522::versionName(v));
    publish([&](DoorState &s) { s.chipVersion = v; });
    for (;;) {
      Request r;
      if (xQueueReceive(mailbox_, &r, pdMS_TO_TICKS(80)) == pdTRUE) {
        publish([&](DoorState &s) { s.busy = "working"; });
        perform(r);
        chip_.stopCrypto1();
        if (chip_.takeIrqMissed()) chip_.setWaitMode(rc522::Mfrc522::WaitMode::Poll);
        publish([&](DoorState &s) { s.busy = nullptr; });
        continue;
      }
      if (millis() - lastHealth_ >= 2000) health();
      if (!chipLost_) pollOnce();
      tickDoor();
    }
  }

  // ---- State, events, outbox ---------------------------------------------------------------

  template <typename F>
  void publish(F change) {
    std::lock_guard<std::mutex> lock(mutex_);
    change(state_);
    state_.version++;
  }

  void send(const char *type, JsonDocument &data) {
    JsonDocument msg;
    msg["type"] = type;
    msg["data"] = data;
    String text;
    serializeJson(msg, text);
    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.size() >= 24) outbox_.erase(outbox_.begin());
    outbox_.push_back(std::move(text));
  }

  void event(const char *uid, const char *name, const char *what, const char *decision = "") {
    DoorEvent e;
    door::Clock clk;
    doorjson::clockNow(clk);
    e.at = clk.synced ? clk.now : 0;
    strlcpy(e.uid, uid, sizeof(e.uid));
    strlcpy(e.name, name, sizeof(e.name));
    strlcpy(e.what, what, sizeof(e.what));
    strlcpy(e.decision, decision, sizeof(e.decision));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      e.seq = ++eventSeq_;
      if (events_.size() >= 50) events_.erase(events_.begin());
      events_.push_back(e);
    }
    log_i("%s %s %s %s", what, name[0] ? name : uid, decision, e.at ? "" : "(no clock yet)");
    JsonDocument d;
    d["seq"] = e.seq, d["at"] = e.at, d["uid"] = e.uid, d["name"] = e.name, d["what"] = e.what, d["decision"] = e.decision;
    send("event", d);
  }

  void finish(uint32_t id, bool ok, const char *message, JsonDocument *data = nullptr) {
    if (ok) log_i("job %lu: %s", static_cast<unsigned long>(id), message);
    else log_w("job %lu: %s", static_cast<unsigned long>(id), message);
    JsonDocument d;
    d["id"] = id, d["ok"] = ok, d["message"] = message;
    if (data) d["data"] = *data;
    send("job", d);
  }

  void setDoor(DoorState::Mode mode, uint32_t ms, const char *name, const char *uid, const char *why) {
    publish([&](DoorState &s) {
      s.mode = mode;
      s.modeUntil = millis() + ms;
      strlcpy(s.lastName, name, sizeof(s.lastName));
      strlcpy(s.lastUid, uid, sizeof(s.lastUid));
      strlcpy(s.lastWhy, why, sizeof(s.lastWhy));
    });
  }

  // Door back to locked after its time; enrollment gives up after 20 s.
  void tickDoor() {
    const uint32_t now = millis();
    bool relock = false, enrollTimeout = false;
    uint32_t enrollJob = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      relock = state_.mode != DoorState::Mode::Locked && int32_t(now - state_.modeUntil) >= 0;
      enrollTimeout = state_.enrolling && int32_t(now - state_.enrollUntil) >= 0;
      enrollJob = enrollJob_;
    }
    if (relock) publish([](DoorState &s) { s.mode = DoorState::Mode::Locked; });
    if (enrollTimeout) {
      stopEnroll();
      finish(enrollJob, false, "no new card within 20 s");
    }
  }

  // ---- Polling and taps ----------------------------------------------------------------------

  void pollOnce() {
    iso14443a::Uid u;
    const bool found = iso14443a::wakeAndSelect(chip_, u) == rc522::Status::Ok;
    if (found) iso14443a::halt(chip_);
    chip_.stopCrypto1();
    const bool isNew = found && (!present_ || !u.sameAs(current_));
    if (found) misses_ = 0;
    if (isNew) {
      present_ = true, current_ = u;
      char hex[21];
      door::uidToHex(u.bytes, u.size, hex, sizeof(hex));
      publish([&](DoorState &s) { s.present = true, strlcpy(s.presentUid, hex, sizeof(s.presentUid)); });
      onCard(u, hex);
    } else if (!found && present_ && ++misses_ >= 3) {
      present_ = false;
      publish([](DoorState &s) { s.present = false, s.presentUid[0] = 0; });
    }
  }

  void onCard(const iso14443a::Uid &u, const char *hex) {
    bool enrolling;
    uint32_t job;
    char name[24];
    door::Role role;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      enrolling = state_.enrolling;
      job = enrollJob_;
      strlcpy(name, state_.enrollName, sizeof(name));
      role = enrollRole_;
    }
    if (enrolling) return enroll(u, hex, job, name, role);

    door::Clock clk;
    doorjson::clockNow(clk);
    door::Card c;
    bool known = false;
    const door::Decision d = reg_.tap(u.bytes, u.size, clk, c, known);
    const char *why = door::decisionName(d);
    if (d == door::Decision::Granted) setDoor(DoorState::Mode::Open, kOpenMs, c.name, hex, why);
    else setDoor(DoorState::Mode::Denied, kDeniedMs, known ? c.name : "", hex, why);
    event(hex, known ? c.name : "", "tap", why);
  }

  void enroll(const iso14443a::Uid &u, const char *hex, uint32_t job, const char *name, door::Role role) {
    stopEnroll();
    door::Card existing;
    if (reg_.get(u.bytes, u.size, existing)) {
      char msg[80];
      snprintf(msg, sizeof(msg), "this card is already in the list as \"%s\"", existing.name);
      return finish(job, false, msg);
    }
    door::Card c;
    memcpy(c.uid, u.bytes, u.size);
    c.uidLen = u.size;
    strlcpy(c.name, name, sizeof(c.name));
    c.role = role;
    c.usesLeft = role == door::Role::Guest ? 5 : -1;  // guests start with 5 uses; the app can change it
    door::Clock clk;
    doorjson::clockNow(clk);
    c.added = clk.synced ? clk.now : 0;
    const Registry::AddResult r = reg_.add(c);
    if (r == Registry::AddResult::Full) return finish(job, false, "the list is full (20 cards)");
    if (r == Registry::AddResult::Exists) return finish(job, false, "this card is already in the list");
    event(hex, c.name, "enrolled");
    JsonDocument d;
    doorjson::card(d.to<JsonObject>(), c, clk, reg_.lockdown());
    finish(job, true, "card added", &d);
  }

  void stopEnroll() {
    publish([](DoorState &s) { s.enrolling = false, s.enrollName[0] = 0; });
  }

  // ---- Jobs from the app ----------------------------------------------------------------------

  void perform(const Request &r) {
    switch (r.job) {
      case Job::Enroll: {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          enrollJob_ = r.id;
          enrollRole_ = r.role;
        }
        publish([&](DoorState &s) {
          s.enrolling = true;
          s.enrollUntil = millis() + kEnrollMs;
          strlcpy(s.enrollName, r.name, sizeof(s.enrollName));
        });
        present_ = false;  // a card already lying there counts as a new tap for enrollment
        log_i("enroll \"%s\": tap the new card within 20 s", r.name);
        return;
      }
      case Job::CancelEnroll: {
        uint32_t job;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          job = enrollJob_;
        }
        stopEnroll();
        finish(job, false, "enrollment cancelled");
        return finish(r.id, true, "cancelled");
      }
      case Job::Open:
        setDoor(DoorState::Mode::Open, kOpenMs, "the app", "", "remote open");
        event("", "the app", "opened from the app");
        return finish(r.id, true, "door open for 5 s");
      case Job::ChipReset: {
        chip_.hardReset();
        const uint8_t v = chip_.read(rc522::VersionReg);
        chipLost_ = v == 0x00 || v == 0xFF;
        publish([&](DoorState &s) { s.chipVersion = v; });
        char msg[48];
        snprintf(msg, sizeof(msg), "chip reset: VersionReg 0x%02X", v);
        return finish(r.id, !chipLost_, msg);
      }
      case Job::Label: return label(r);
      case Job::Wipe: return wipe(r);
      case Job::Inspect: return inspect(r);
    }
  }

  // The card with this UID must be on the reader. Returns false (and finishes the job) if not.
  bool cardOnReader(const Request &r, iso14443a::Uid &u, door::Card &listed) {
    const bool inList = reg_.get(r.uid, r.uidLen, listed);
    const rc522::Status s = iso14443a::wakeAndSelect(chip_, u);
    char msg[80];
    if (s != rc522::Status::Ok) {
      snprintf(msg, sizeof(msg), "put %s on the reader first", inList ? listed.name : "the card");
      finish(r.id, false, msg);
      return false;
    }
    if (u.size != r.uidLen || memcmp(u.bytes, r.uid, r.uidLen) != 0) {
      finish(r.id, false, "a different card is on the reader");
      return false;
    }
    return true;
  }

  bool openSector(iso14443a::Uid &u, uint8_t firstBlock, rfid::AccessBits &ab) {
    if (mifare::authA(chip_, u, firstBlock, mifare::kFactoryKey) != rc522::Status::Ok) {
      mifare::reselect(chip_, u);
      return false;
    }
    uint8_t t[16];
    return mifare::readBlock(chip_, uint8_t(firstBlock + 3), t) == rc522::Status::Ok && rfid::AccessBits::decode(t, ab);
  }

  // Blocks 4 and 5: the name and the role, as a label anyone can read with the factory key.
  // The door never trusts it: access is decided by the board's list only.
  void label(const Request &r) {
    iso14443a::Uid u;
    door::Card c;
    if (!cardOnReader(r, u, c)) return;
    if (!c.uidLen) return finish(r.id, false, "add this card to the list first");
    rfid::AccessBits ab;
    if (!openSector(u, 4, ab)) return finish(r.id, false, "sector 1 doesn't open with the factory key");
    if (!ab.keyAWrites(0) || !ab.keyAWrites(1)) return finish(r.id, false, "sector 1's access bits don't allow writes");
    uint8_t b4[16] = {}, b5[16] = {}, back[16];
    strncpy(reinterpret_cast<char *>(b4), c.name, 16);
    snprintf(reinterpret_cast<char *>(b5), 16, "R7 %s", door::roleName(c.role));
    bool ok = mifare::writeBlock(chip_, 4, b4) == rc522::Status::Ok && mifare::readBlock(chip_, 4, back) == rc522::Status::Ok &&
              memcmp(back, b4, 16) == 0;
    ok = ok && mifare::writeBlock(chip_, 5, b5) == rc522::Status::Ok && mifare::readBlock(chip_, 5, back) == rc522::Status::Ok &&
         memcmp(back, b5, 16) == 0;
    iso14443a::halt(chip_);
    if (!ok) return finish(r.id, false, "writing failed: keep the card still and try again");
    char hex[21];
    door::uidToHex(u.bytes, u.size, hex, sizeof(hex));
    event(hex, c.name, "label written");
    finish(r.id, true, "label written to blocks 4 and 5");
  }

  // Data blocks of sectors 1 and 2 back to zeros, where the access bits allow it.
  void wipe(const Request &r) {
    iso14443a::Uid u;
    door::Card c;
    if (!cardOnReader(r, u, c)) return;
    JsonDocument d;
    JsonArray cleared = d["cleared"].to<JsonArray>(), skipped = d["skipped"].to<JsonArray>();
    const uint8_t zeros[16] = {};
    for (uint8_t first : {uint8_t(4), uint8_t(8)}) {
      rfid::AccessBits ab;
      if (!openSector(u, first, ab)) {
        for (int b = 0; b < 3; b++) skipped.add(first + b);
        continue;
      }
      for (int b = 0; b < 3; b++) {
        const uint8_t block = uint8_t(first + b);
        uint8_t back[16];
        const bool ok = ab.keyAWrites(b) && mifare::writeBlock(chip_, block, zeros) == rc522::Status::Ok &&
                        mifare::readBlock(chip_, block, back) == rc522::Status::Ok && memcmp(back, zeros, 16) == 0;
        (ok ? cleared : skipped).add(block);
      }
    }
    iso14443a::halt(chip_);
    char hex[21], msg[64];
    door::uidToHex(u.bytes, u.size, hex, sizeof(hex));
    snprintf(msg, sizeof(msg), "%u of 6 data blocks cleared", unsigned(cleared.size()));
    event(hex, c.uidLen ? c.name : "", "card wiped");
    finish(r.id, cleared.size() > 0, msg, &d);
  }

  // Whatever card is on the reader: its UID, type, whether it's in the list, blocks 4-6 and 8-10.
  void inspect(const Request &r) {
    iso14443a::Uid u;
    if (iso14443a::wakeAndSelect(chip_, u) != rc522::Status::Ok) return finish(r.id, false, "no card on the reader");
    char hex[21];
    door::uidToHex(u.bytes, u.size, hex, sizeof(hex));
    door::Card c;
    JsonDocument d;
    d["uid"] = hex;
    d["type"] = rfid::cardType(u.sak);
    d["listed"] = reg_.get(u.bytes, u.size, c);
    if (c.uidLen) d["name"] = c.name;
    JsonArray blocks = d["blocks"].to<JsonArray>();
    for (uint8_t first : {uint8_t(4), uint8_t(8)}) {
      rfid::AccessBits ab;
      const bool open = openSector(u, first, ab);
      for (int b = 0; b < 3; b++) {
        JsonObject o = blocks.add<JsonObject>();
        o["block"] = first + b;
        uint8_t data[16];
        if (!open || mifare::readBlock(chip_, uint8_t(first + b), data) != rc522::Status::Ok) {
          o["closed"] = true;
          continue;
        }
        char h[33], text[17];
        for (int i = 0; i < 16; i++) {
          snprintf(h + i * 2, 3, "%02X", data[i]);
          text[i] = data[i] >= 32 && data[i] < 127 ? char(data[i]) : '.';
        }
        text[16] = 0;
        o["hex"] = h;
        o["text"] = text;
        int32_t value;
        uint8_t addr;
        if (rfid::ValueBlock::decode(data, value, addr)) o["value"] = value;
      }
    }
    iso14443a::halt(chip_);
    finish(r.id, true, "card read", &d);
  }

  // ---- Is the chip still there? (as in the R1-R6 labs) -------------------------------------------

  void health() {
    lastHealth_ = millis();
    const uint8_t v = chip_.read(rc522::VersionReg);
    const bool alive = v != 0x00 && v != 0xFF;
    if (alive == !chipLost_) return;
    if (!alive) {
      chip_.hardReset();
      const uint8_t again = chip_.read(rc522::VersionReg);
      chipLost_ = again == 0x00 || again == 0xFF;
      log_w("RC522 stopped answering (0x%02X); after a hard reset: 0x%02X%s", v, again, chipLost_ ? " - check 3.3V, GND, MISO" : "");
      publish([&](DoorState &s) { s.chipVersion = again; });
    } else {
      chipLost_ = false;
      log_i("RC522 answering again: 0x%02X", v);
      publish([&](DoorState &s) { s.chipVersion = v; });
    }
  }

  Registry &reg_;
  rc522::Mfrc522 chip_;
  SPIClass *spi_ = nullptr;
  QueueHandle_t mailbox_ = nullptr;
  std::atomic<uint32_t> nextId_{1};
  std::mutex mutex_;
  DoorState state_;
  std::vector<DoorEvent> events_;
  std::vector<String> outbox_;
  uint32_t eventSeq_ = 0, enrollJob_ = 0;
  door::Role enrollRole_ = door::Role::Resident;
  // Poll state, only touched by the task.
  bool present_ = false, chipLost_ = false;
  int misses_ = 0;
  uint32_t lastHealth_ = 0;
  iso14443a::Uid current_;
};
