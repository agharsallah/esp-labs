// R7's reader task: the only code that touches the RC522 (core 0). It
//   - polls for cards and checks each tap against the card list (registry.h + access_rules.h)
//   - drives two simulated locks (front door, admin room): the one the reader is mounted at
//     goes OPEN for 5 s when granted, DENIED for 2 s otherwise
//   - runs the app's card jobs, one at a time: enroll, replace, label, wipe, inspect, save,
//     restore, copy, open, chip reset
//   - waits for a card to be laid down when a job needs one (enroll, replace, copy's target)
// Results and events become JSON messages in an outbox; loop() sends them over the WebSocket.
//
// Guard rails, as in R4/R5: only the factory key, only the data blocks of sectors 1 and 2
// (4, 5, 6, 8, 9, 10), never block 0 or a sector trailer, and only where the access bits let
// key A write. "Wipe" means: those data blocks back to zeros. "Copy" and "restore" write saved
// data into the same blocks. Keys, trailers and the UID are never touched.
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
#include "snapshots.h"

namespace doorjson {

// ArduinoJson 7 takes a `const char[N]` for a string literal: it keeps only the pointer and
// sends all N-1 bytes. For a name field in a struct that is about to go away, that means the
// JSON later shows whatever reused the memory (the bug where a card's name became the tail of
// another card's JSON). Passing the field through here makes it a `const char *`, which is copied.
inline const char *copied(const char *s) { return s; }

inline void clockNow(door::Clock &c) {
  const time_t now = time(nullptr);
  c.synced = now > 1'700'000'000;
  c.now = uint32_t(now);
  struct tm lt;
  localtime_r(&now, &lt);
  c.hour = uint8_t(lt.tm_hour);
}

// One card as the app sees it, including what a tap would do right now at each lock.
inline void card(JsonObject o, const door::Card &c, const door::Clock &clk, bool lockdown) {
  char uid[21];
  door::uidToHex(c.uid, c.uidLen, uid, sizeof(uid));
  o["uid"] = uid;
  o["name"] = copied(c.name);
  o["role"] = door::roleName(c.role);
  o["blocked"] = c.blocked;
  o["validUntil"] = c.validUntil;
  o["fromHour"] = c.fromHour;
  o["toHour"] = c.toHour;
  o["usesLeft"] = c.usesLeft;
  o["added"] = c.added;
  o["lastSeen"] = c.lastSeen;
  o["taps"] = c.taps;
  JsonObject now = o["now"].to<JsonObject>();  // per lock: {"front": "granted", "admin": "admins only"}
  for (door::Lock l : {door::Lock::Front, door::Lock::AdminRoom})
    now[door::lockId(l)] = door::decisionName(door::decide(&c, clk, lockdown, l));
}

// Saved card data: only the blocks that were readable.
inline void snapshot(JsonObject o, const snap::Snapshot &s) {
  char uid[21];
  door::uidToHex(s.uid, s.uidLen, uid, sizeof(uid));
  o["id"] = s.id;
  o["name"] = copied(s.name);
  o["uid"] = uid;  // "" when imported from a file
  o["at"] = s.at;
  JsonArray blocks = o["blocks"].to<JsonArray>();
  for (int i = 0; i < snap::kBlocks; i++) {
    if (!(s.have & (1 << i))) continue;
    char hex[33];
    for (int k = 0; k < 16; k++) snprintf(hex + k * 2, 3, "%02X", s.data[i][k]);
    JsonObject b = blocks.add<JsonObject>();
    b["block"] = snap::kBlock[i];
    b["hex"] = hex;
  }
}

}  // namespace doorjson

struct DoorEvent {
  uint32_t seq = 0;
  uint32_t at = 0;  // unix seconds (0 before the clock syncs)
  char uid[21] = "";
  char name[24] = "";
  char what[24] = "";      // "tap", "enrolled", "label written", "card data saved", "data copied here", ...
  char decision[16] = "";  // for taps: granted, blocked, ...
  char lock[8] = "";       // taps and remote opens: "front" or "admin"
};

struct DoorState {
  enum class Mode : uint8_t { Locked, Open, Denied };
  struct LockState {
    Mode mode = Mode::Locked;
    uint32_t modeUntil = 0;  // millis()
    char lastName[24] = "", lastUid[21] = "", lastWhy[16] = "";
  };
  // Waiting for a card to be laid down: a new one (enroll), a replacement, or copy's target.
  enum class Wait : uint8_t { None, Enroll, Replace, CopyTarget };
  uint32_t version = 0;
  uint8_t chipVersion = 0;
  LockState locks[door::kLocks];  // indexed by door::Lock
  bool present = false;
  char presentUid[21] = "";
  Wait wait = Wait::None;
  uint32_t waitUntil = 0;  // millis()
  char waitName[24] = "";  // enroll, replace: the card's name; copy: where the data comes from
  char waitUid[21] = "";   // replace: the card being replaced; copy: the source card
  const char *busy = nullptr;
  // How the reader is doing, for the app's reader line.
  bool irqMode = false;     // waiting on the IRQ wire (false: polling the chip)
  uint16_t restores = 0;    // the chip reset itself (3.3 V dip) and its settings were put back
  uint16_t lost = 0;        // the chip stopped answering on SPI
  char problem[72] = "";    // the last thing that went wrong, in words
  uint32_t problemAt = 0;   // millis()
};

class Door {
 public:
  enum class Job : uint8_t { Enroll, Replace, Cancel, Label, Wipe, Inspect, Save, Restore, Copy, Open, ChipReset };
  struct Request {
    Job job = Job::Open;
    uint32_t id = 0;
    uint8_t uid[10] = {};
    uint8_t uidLen = 0;
    char name[24] = "";
    door::Role role = door::Role::Resident;
    door::Lock lock = door::Lock::Front;  // open: which lock
    uint32_t snapshot = 0;                // restore: which saved data
  };

  static constexpr uint32_t kOpenMs = 5000, kDeniedMs = 2000, kWaitMs = 20000;

  Door(Registry &reg, snap::Store &snaps) : reg_(reg), snaps_(snaps) {}

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
    chipLost_ = v == 0x00 || v == 0xFF;
    if (chipLost_) problem("reader chip not answering: check its 3.3V, GND and MISO wires");
    publish([&](DoorState &s) { s.chipVersion = v, s.irqMode = chip_.waitMode() == rc522::Mfrc522::WaitMode::Irq; });
    for (;;) {
      Request r;
      if (xQueueReceive(mailbox_, &r, pdMS_TO_TICKS(80)) == pdTRUE) {
        publish([&](DoorState &s) { s.busy = "working"; });
        perform(r);
        chip_.stopCrypto1();
        checkIrq();
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

  // The last thing that went wrong with the reader, for the app (and the serial log).
  void problem(const char *what) {
    log_w("reader: %s", what);
    publish([&](DoorState &s) {
      strlcpy(s.problem, what, sizeof(s.problem));
      s.problemAt = millis();
    });
  }

  // The IRQ wire stayed quiet although the chip finished: poll the chip instead (as R1-R6 do).
  // Without this, every frame waits out the whole timeout and reading a card crawls.
  void checkIrq() {
    if (!chip_.takeIrqMissed() || chip_.waitMode() != rc522::Mfrc522::WaitMode::Irq) return;
    chip_.setWaitMode(rc522::Mfrc522::WaitMode::Poll);
    publish([](DoorState &s) { s.irqMode = false; });
    char msg[72];
    snprintf(msg, sizeof(msg), "IRQ wire on GPIO %d stays quiet: polling instead", PIN_RFID_IRQ);
    problem(msg);
  }

  // Settings gone after a 3.3 V dip (VersionReg still fine, antenna off): put them back.
  bool restoreIfReset() {
    if (!chip_.settingsLost()) return false;
    chip_.restoreSettings();
    publish([](DoorState &s) { s.restores++; });
    problem("the reader chip reset itself (3.3V dip?): settings restored");
    return true;
  }

  void event(const char *uid, const char *name, const char *what, const char *decision = "", const char *lock = "") {
    DoorEvent e;
    door::Clock clk;
    doorjson::clockNow(clk);
    e.at = clk.synced ? clk.now : 0;
    strlcpy(e.uid, uid, sizeof(e.uid));
    strlcpy(e.name, name, sizeof(e.name));
    strlcpy(e.what, what, sizeof(e.what));
    strlcpy(e.decision, decision, sizeof(e.decision));
    strlcpy(e.lock, lock, sizeof(e.lock));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      e.seq = ++eventSeq_;
      if (events_.size() >= 50) events_.erase(events_.begin());
      events_.push_back(e);
    }
    log_i("%s %s %s %s %s", what, lock, name[0] ? name : uid, decision, e.at ? "" : "(no clock yet)");
    JsonDocument d;
    d["seq"] = e.seq, d["at"] = e.at, d["uid"] = doorjson::copied(e.uid), d["name"] = doorjson::copied(e.name), d["what"] = doorjson::copied(e.what),
        d["decision"] = doorjson::copied(e.decision), d["lock"] = doorjson::copied(e.lock);
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

  void setDoor(door::Lock at, DoorState::Mode mode, uint32_t ms, const char *name, const char *uid, const char *why) {
    publish([&](DoorState &s) {
      DoorState::LockState &l = s.locks[int(at)];
      l.mode = mode;
      l.modeUntil = millis() + ms;
      strlcpy(l.lastName, name, sizeof(l.lastName));
      strlcpy(l.lastUid, uid, sizeof(l.lastUid));
      strlcpy(l.lastWhy, why, sizeof(l.lastWhy));
    });
  }

  // Each lock back to locked after its time; waiting for a card gives up after 20 s.
  void tickDoor() {
    const uint32_t now = millis();
    bool relock = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const DoorState::LockState &l : state_.locks)
        relock |= l.mode != DoorState::Mode::Locked && int32_t(now - l.modeUntil) >= 0;
    }
    if (relock)
      publish([now](DoorState &s) {
        for (DoorState::LockState &l : s.locks)
          if (l.mode != DoorState::Mode::Locked && int32_t(now - l.modeUntil) >= 0) l.mode = DoorState::Mode::Locked;
      });
    if (wait_ != DoorState::Wait::None && int32_t(now - waitUntil_) >= 0) {
      const bool copy = wait_ == DoorState::Wait::CopyTarget;
      stopWait();
      finish(waitJob_, false, copy ? "no second card within 20 s: nothing was written" : "no new card within 20 s");
    }
  }

  void startWait(DoorState::Wait w, uint32_t job, const char *name, const char *uid) {
    wait_ = w, waitJob_ = job, waitUntil_ = millis() + kWaitMs;
    publish([&](DoorState &s) {
      s.wait = w;
      s.waitUntil = waitUntil_;
      strlcpy(s.waitName, name, sizeof(s.waitName));
      strlcpy(s.waitUid, uid, sizeof(s.waitUid));
    });
  }

  void stopWait() {
    wait_ = DoorState::Wait::None;
    publish([](DoorState &s) { s.wait = DoorState::Wait::None, s.waitName[0] = 0, s.waitUid[0] = 0; });
  }

  // ---- Polling and taps ----------------------------------------------------------------------

  void pollOnce() {
    iso14443a::Uid u;
    const bool found = iso14443a::wakeAndSelect(chip_, u) == rc522::Status::Ok;
    if (found) iso14443a::halt(chip_);
    chip_.stopCrypto1();
    checkIrq();
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
    switch (wait_) {
      case DoorState::Wait::Enroll: return enroll(u, hex);
      case DoorState::Wait::Replace: return replace(u, hex);
      case DoorState::Wait::CopyTarget: return copyOnto(u, hex);
      case DoorState::Wait::None: break;
    }

    door::Clock clk;
    doorjson::clockNow(clk);
    door::Card c;
    bool known = false;
    const door::Lock at = reg_.readerAt();
    const door::Decision d = reg_.tap(u.bytes, u.size, clk, at, c, known);
    const char *why = door::decisionName(d);
    if (d == door::Decision::Granted) setDoor(at, DoorState::Mode::Open, kOpenMs, c.name, hex, why);
    else setDoor(at, DoorState::Mode::Denied, kDeniedMs, known ? c.name : "", hex, why);
    event(hex, known ? c.name : "", "tap", why, door::lockId(at));
  }

  void enroll(const iso14443a::Uid &u, const char *hex) {
    const uint32_t job = waitJob_;
    stopWait();
    door::Card existing;
    if (reg_.get(u.bytes, u.size, existing)) {
      char msg[80];
      snprintf(msg, sizeof(msg), "this card is already in the list as \"%s\"", existing.name);
      return finish(job, false, msg);
    }
    door::Card c;
    memcpy(c.uid, u.bytes, u.size);
    c.uidLen = u.size;
    strlcpy(c.name, enrollName_, sizeof(c.name));
    c.role = enrollRole_;
    c.usesLeft = c.role == door::Role::Guest ? 5 : -1;  // guests start with 5 uses; the app can change it
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

  // A lost or broken card: the new card takes over the old one's name, role and every rule; the
  // old one stays in the list, blocked and renamed "<name> old", so a tap with it shows in the log.
  void replace(const iso14443a::Uid &u, const char *hex) {
    if (u.size == replaceLen_ && memcmp(u.bytes, replaceUid_, u.size) == 0) {
      log_i("that's the card being replaced: lay the new one");
      return;  // keep waiting
    }
    const uint32_t job = waitJob_;
    stopWait();
    door::Card existing, old;
    if (reg_.get(u.bytes, u.size, existing)) {
      char msg[80];
      snprintf(msg, sizeof(msg), "this card is already in the list as \"%s\"", existing.name);
      return finish(job, false, msg);
    }
    if (!reg_.get(replaceUid_, replaceLen_, old)) return finish(job, false, "the old card is no longer in the list");
    door::Card c = old;
    memset(c.uid, 0, sizeof(c.uid));
    memcpy(c.uid, u.bytes, u.size);
    c.uidLen = u.size;
    c.blocked = false;
    c.taps = 0, c.lastSeen = 0;
    door::Clock clk;
    doorjson::clockNow(clk);
    c.added = clk.synced ? clk.now : 0;
    if (reg_.add(c) != Registry::AddResult::Added) return finish(job, false, "the list is full (20 cards): remove one first");
    char oldName[24], oldHex[21];
    snprintf(oldName, sizeof(oldName), "%.19s old", old.name);
    reg_.update(replaceUid_, replaceLen_, [&](door::Card &o) {
      o.blocked = true;
      strlcpy(o.name, oldName, sizeof(o.name));
    });
    door::uidToHex(old.uid, old.uidLen, oldHex, sizeof(oldHex));
    event(oldHex, oldName, "blocked: replaced");
    event(hex, c.name, "replacement added");
    JsonDocument d;
    doorjson::card(d.to<JsonObject>(), c, clk, reg_.lockdown());
    finish(job, true, "new card added with the same rules; the old one is blocked", &d);
  }

  // Copy, second half: the saved data from the first card goes onto this one.
  void copyOnto(const iso14443a::Uid &seen, const char *hex) {
    if (seen.size == copySrc_.uidLen && memcmp(seen.bytes, copySrc_.uid, seen.size) == 0) {
      log_i("that's the source card: lay the card to copy onto");
      return;  // keep waiting
    }
    const uint32_t job = waitJob_;
    stopWait();
    iso14443a::Uid u;  // polling halted it: wake it again
    if (selectCard(u) != rc522::Status::Ok || !u.sameAs(seen))
      return finish(job, false, "the card moved away: keep it still and copy again");
    JsonDocument d;
    const int n = writeSnapshot(u, copySrc_, d);
    iso14443a::halt(chip_);
    door::Card c;
    const bool listed = reg_.get(u.bytes, u.size, c);
    char msg[112];
    snprintf(msg, sizeof(msg), "%d of %d blocks copied from %s onto %s", n, copySrc_.count(), copySrc_.name, hex);
    event(hex, listed ? c.name : "", "data copied here");
    finish(job, n == copySrc_.count(), msg, &d);
  }

  // Refuse a second wait while one runs: one card at a time.
  bool waitFree(const Request &r) {
    if (wait_ == DoorState::Wait::None) return true;
    finish(r.id, false, "the board is already waiting for a card: cancel that first");
    return false;
  }

  // ---- Jobs from the app ----------------------------------------------------------------------

  void perform(const Request &r) {
    switch (r.job) {
      case Job::Enroll: {
        if (!waitFree(r)) return;
        enrollRole_ = r.role;
        strlcpy(enrollName_, r.name, sizeof(enrollName_));
        present_ = false;  // a card already lying there counts as a new tap for enrollment
        startWait(DoorState::Wait::Enroll, r.id, r.name, "");
        log_i("enroll \"%s\": tap the new card within 20 s", r.name);
        return;
      }
      case Job::Replace: {
        if (!waitFree(r)) return;
        door::Card old;
        if (!reg_.get(r.uid, r.uidLen, old)) return finish(r.id, false, "no card with this uid in the list");
        memcpy(replaceUid_, r.uid, r.uidLen);
        replaceLen_ = r.uidLen;
        char hex[21];
        door::uidToHex(r.uid, r.uidLen, hex, sizeof(hex));
        present_ = false;
        startWait(DoorState::Wait::Replace, r.id, old.name, hex);
        log_i("replace \"%s\": tap the new card within 20 s", old.name);
        return;
      }
      case Job::Cancel: {
        if (wait_ != DoorState::Wait::None) {
          const uint32_t job = waitJob_;
          stopWait();
          finish(job, false, "cancelled");
        }
        return finish(r.id, true, "cancelled");
      }
      case Job::Open: {
        setDoor(r.lock, DoorState::Mode::Open, kOpenMs, "the app", "", "remote open");
        event("", "the app", "opened from the app", "", door::lockId(r.lock));
        char msg[40];
        snprintf(msg, sizeof(msg), "%s open for 5 s", door::lockTitle(r.lock));
        return finish(r.id, true, msg);
      }
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
      case Job::Save: return save(r);
      case Job::Restore: return restore(r);
      case Job::Copy: return copyStart(r);
    }
  }

  // Wake and select the card on the reader for a job. Three tries, with the field switched off
  // for 5 ms in between (the card restarts): a card at the edge of the field often answers on
  // the second try. If nothing answers, check the chip didn't lose its settings and try once more.
  rc522::Status selectCard(iso14443a::Uid &u) {
    rc522::Status s = rc522::Status::NoAnswer;
    for (int i = 0; i < 3; i++) {
      s = iso14443a::wakeAndSelect(chip_, u);
      if (s == rc522::Status::Ok || s == rc522::Status::ChipSilent) break;
      chip_.antenna(false);
      delay(5);
      chip_.antenna(true);
      delay(5);
    }
    if (s != rc522::Status::Ok && restoreIfReset()) s = iso14443a::wakeAndSelect(chip_, u);
    checkIrq();
    return s;
  }

  // Why selectCard() failed, in words. `noCard` is the job's own "lay the card down" sentence.
  static const char *whyNot(rc522::Status s, const char *noCard) {
    if (s == rc522::Status::NoAnswer) return noCard;
    if (s == rc522::Status::ChipSilent) return "the reader chip isn't answering: check its 3.3V, GND and MISO wires";
    return "a card answered, but garbled: lay it flat and still, in the middle of the reader";
  }

  // The card with this UID must be on the reader. Returns false (and finishes the job) if not.
  bool cardOnReader(const Request &r, iso14443a::Uid &u, door::Card &listed) {
    const bool inList = reg_.get(r.uid, r.uidLen, listed);
    const rc522::Status s = selectCard(u);
    char msg[96];
    if (s != rc522::Status::Ok) {
      snprintf(msg, sizeof(msg), "put %s on the reader first", inList ? listed.name : "the card");
      finish(r.id, false, whyNot(s, msg));
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
    const rc522::Status st = selectCard(u);
    if (st != rc522::Status::Ok) return finish(r.id, false, whyNot(st, "no card on the reader"));
    char hex[21];
    door::uidToHex(u.bytes, u.size, hex, sizeof(hex));
    door::Card c;
    JsonDocument d;
    d["uid"] = hex;
    d["type"] = rfid::cardType(u.sak);
    d["listed"] = reg_.get(u.bytes, u.size, c);
    if (c.uidLen) d["name"] = doorjson::copied(c.name);
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

  // ---- Saved card data: save, restore, copy -----------------------------------------------------

  // The lab blocks that open with the factory key, into `s`. Returns how many were read.
  int readSnapshot(iso14443a::Uid &u, snap::Snapshot &s) {
    memcpy(s.uid, u.bytes, u.size);
    s.uidLen = u.size;
    door::Clock clk;
    doorjson::clockNow(clk);
    s.at = clk.synced ? clk.now : 0;
    s.have = 0;
    for (uint8_t first : {uint8_t(4), uint8_t(8)}) {
      rfid::AccessBits ab;
      if (!openSector(u, first, ab)) continue;
      for (int b = 0; b < 3; b++) {
        const int i = snap::slotOf(first + b);
        if (mifare::readBlock(chip_, uint8_t(first + b), s.data[i]) == rc522::Status::Ok) s.have |= uint8_t(1 << i);
      }
    }
    return s.count();
  }

  // The saved blocks onto the card `u`, each read back to check. Returns how many match now.
  int writeSnapshot(iso14443a::Uid &u, const snap::Snapshot &s, JsonDocument &d) {
    JsonArray written = d["written"].to<JsonArray>(), skipped = d["skipped"].to<JsonArray>();
    int n = 0;
    for (uint8_t first : {uint8_t(4), uint8_t(8)}) {
      const int base = snap::slotOf(first);
      if (!(s.have & (7 << base))) continue;  // nothing saved from this sector
      rfid::AccessBits ab;
      const bool open = openSector(u, first, ab);
      for (int b = 0; b < 3; b++) {
        const int i = base + b;
        if (!(s.have & (1 << i))) continue;
        const uint8_t block = uint8_t(first + b);
        uint8_t back[16];
        const bool ok = open && ab.keyAWrites(b) && mifare::writeBlock(chip_, block, s.data[i]) == rc522::Status::Ok &&
                        mifare::readBlock(chip_, block, back) == rc522::Status::Ok && memcmp(back, s.data[i], 16) == 0;
        (ok ? written : skipped).add(block);
        n += ok;
      }
    }
    return n;
  }

  // Whatever card is on the reader, listed or not: its lab blocks become a named snapshot.
  void save(const Request &r) {
    iso14443a::Uid u;
    const rc522::Status st = selectCard(u);
    if (st != rc522::Status::Ok) return finish(r.id, false, whyNot(st, "lay the card to save on the reader first"));
    snap::Snapshot s;
    const int n = readSnapshot(u, s);
    iso14443a::halt(chip_);
    if (!n) return finish(r.id, false, "no lab block opens with the factory key: nothing to save");
    char hex[21];
    door::uidToHex(u.bytes, u.size, hex, sizeof(hex));
    door::Card c;
    const bool listed = reg_.get(u.bytes, u.size, c);
    strlcpy(s.name, r.name[0] ? r.name : listed ? c.name : hex, sizeof(s.name));
    if (!snaps_.add(s)) return finish(r.id, false, "8 snapshots saved already: delete one first");
    event(hex, listed ? c.name : "", "card data saved");
    JsonDocument d;
    doorjson::snapshot(d.to<JsonObject>(), s);
    char msg[80];
    snprintf(msg, sizeof(msg), "%d of 6 blocks saved as \"%s\"", n, s.name);
    finish(r.id, true, msg, &d);
  }

  // A snapshot onto the card on the reader: the card it came from (restore) or another (copy).
  void restore(const Request &r) {
    snap::Snapshot s;
    if (!snaps_.get(r.snapshot, s)) return finish(r.id, false, "no saved data with this id");
    iso14443a::Uid u;
    const rc522::Status st = selectCard(u);
    if (st != rc522::Status::Ok) return finish(r.id, false, whyNot(st, "lay the card to write on the reader first"));
    JsonDocument d;
    const int n = writeSnapshot(u, s, d);
    iso14443a::halt(chip_);
    char hex[21], msg[128];
    door::uidToHex(u.bytes, u.size, hex, sizeof(hex));
    const bool same = s.uidLen == u.size && memcmp(s.uid, u.bytes, u.size) == 0;
    snprintf(msg, sizeof(msg), "%d of %d blocks of \"%s\" written to %s%s", n, s.count(), s.name, hex,
             same ? " (the card it came from)" : "");
    door::Card c;
    const bool listed = reg_.get(u.bytes, u.size, c);
    event(hex, listed ? c.name : "", same ? "data restored" : "data copied here");
    finish(r.id, n == s.count(), msg, &d);
  }

  // Copy, first half: read the source card now, then wait for a different card (copyOnto).
  void copyStart(const Request &r) {
    if (!waitFree(r)) return;
    iso14443a::Uid u;
    const rc522::Status st = selectCard(u);
    if (st != rc522::Status::Ok) return finish(r.id, false, whyNot(st, "lay the card to copy from on the reader first"));
    copySrc_ = snap::Snapshot{};
    const int n = readSnapshot(u, copySrc_);
    iso14443a::halt(chip_);
    if (!n) return finish(r.id, false, "no lab block opens with the factory key: nothing to copy");
    char hex[21];
    door::uidToHex(u.bytes, u.size, hex, sizeof(hex));
    door::Card c;
    strlcpy(copySrc_.name, reg_.get(u.bytes, u.size, c) ? c.name : hex, sizeof(copySrc_.name));
    present_ = true, current_ = u;  // the source still lying there is not the target
    startWait(DoorState::Wait::CopyTarget, r.id, copySrc_.name, hex);
    log_i("copy: %d blocks read from %s; lay the card to copy onto within 20 s", n, hex);
  }

  // ---- Is the chip still there? (as in the R1-R6 labs) -------------------------------------------

  // Every 2 s: does the chip still answer, and does it still have our settings?
  void health() {
    lastHealth_ = millis();
    const uint8_t v = chip_.read(rc522::VersionReg);
    const bool alive = v != 0x00 && v != 0xFF;
    if (alive && !chipLost_) {
      restoreIfReset();
      return;
    }
    if (alive == !chipLost_) return;
    if (!alive) {
      chip_.hardReset();
      const uint8_t again = chip_.read(rc522::VersionReg);
      chipLost_ = again == 0x00 || again == 0xFF;
      log_w("RC522 stopped answering (0x%02X); after a hard reset: 0x%02X%s", v, again, chipLost_ ? " - check 3.3V, GND, MISO" : "");
      publish([&](DoorState &s) { s.chipVersion = again, s.lost++; });
      problem(chipLost_ ? "reader chip stopped answering: check its 3.3V, GND and MISO wires"
                        : "reader chip had hung: a hard reset brought it back");
    } else {
      chipLost_ = false;
      log_i("RC522 answering again: 0x%02X", v);
      publish([&](DoorState &s) { s.chipVersion = v; });
    }
  }

  Registry &reg_;
  snap::Store &snaps_;
  rc522::Mfrc522 chip_;
  SPIClass *spi_ = nullptr;
  QueueHandle_t mailbox_ = nullptr;
  std::atomic<uint32_t> nextId_{1};
  std::mutex mutex_;
  DoorState state_;
  std::vector<DoorEvent> events_;
  std::vector<String> outbox_;
  uint32_t eventSeq_ = 0;
  // Waiting for a card, only touched by the task (the app sees it through state_).
  DoorState::Wait wait_ = DoorState::Wait::None;
  uint32_t waitJob_ = 0, waitUntil_ = 0;
  char enrollName_[24] = "";
  door::Role enrollRole_ = door::Role::Resident;
  uint8_t replaceUid_[10] = {}, replaceLen_ = 0;
  snap::Snapshot copySrc_;
  // Poll state, only touched by the task.
  bool present_ = false, chipLost_ = false;
  int misses_ = 0;
  uint32_t lastHealth_ = 0;
  iso14443a::Uid current_;
};
