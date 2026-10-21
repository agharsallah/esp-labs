// R9's reader task: the only code that touches the RC522 (core 0, SPI3, as in R1-R8). For each
// new card on the field it
//   1. times WUPA and SELECT, then asks the Gen1a "magic" question (HLTA, then the 7-bit 0x40
//      wake-up): a genuine card stays silent, a rewritable clone answers ACK. Nothing is read or
//      written through that back door; the field goes off at once so the clone leaves that mode.
//   2. reads block 0 (sector 0) and the vault blocks 4, 5, 6, 8, 9 (sectors 1-2) with the
//      factory key, timing AUTH and READ
//   3. runs the job waiting for a card (provision, rekey, wipe), or decides the tap with
//      vault::evaluateTap() and, when it is granted, writes the next counter onto the card:
//      new tag into the free slot, then block 4, then block 9 (see vault_crypto.h on tearing)
// It never writes block 0 or a sector trailer (kit/lib/rfid-kit refuses them) and checks each
// sector's access bits before writing. It keeps the reader healthy the way R7 learned to.
// loop() copies the latest Scene for the screen; the web side takes the messages from the outbox.
#pragma once

#include <ArduinoJson.h>
#include <esp_random.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "iso14443a.h"
#include "mfrc522.h"
#include "mifare_classic.h"
#include "pins.h"
#include "rfid_format.h"
#include "vault_crypto.h"
#include "vault_store.h"

// One line of the incident log.
struct VaultEvent {
  uint32_t seq = 0;
  uint32_t at = 0;  // Unix time, 0 = no clock yet
  char uid[21] = "";
  uint8_t number = 0;
  char callsign[16] = "";
  vault::Verdict verdict = vault::Verdict::UnknownCard;
  uint32_t counter = 0;
  uint8_t risk = 0;
  char detail[72] = "";
};

// What the screen and the app need to know right now.
struct Scene {
  uint32_t version = 0;
  // Reader health, as in R7.
  uint8_t chipVersion = 0;
  bool irqMode = false;
  uint16_t restores = 0, lost = 0;
  char problem[72] = "";
  uint32_t problemAt = 0;  // millis()
  bool present = false;
  char presentUid[21] = "";

  // A job waiting for a card.
  enum class Job : uint8_t { None, Provision, Rekey, Wipe };
  Job job = Job::None;
  char jobCallsign[16] = "";
  vault::Clearance jobClearance = vault::Clearance::Operator;
  uint32_t jobUntil = 0;  // millis()

  // The last tap (or job), with everything the four views show.
  uint32_t seq = 0;
  uint32_t at = 0;  // millis()
  char uid[21] = "";
  uint8_t number = 0;  // 0 = not in the ledger
  char callsign[16] = "";
  vault::Verdict verdict = vault::Verdict::UnknownCard;
  char detail[72] = "Tap a card";
  bool headerOk = false;
  vault::Header header{};  // what block 4 said
  bool wrote = false;      // a granted tap: the next counter was written
  vault::Header next{};    // ... and this is it
  vault::Forensics forensics{};
  vault::Timing timing{}, baseline{};
  bool hasSaved = false;
  uint8_t block0[16] = {}, saved0[16] = {};
  uint8_t block4[16] = {}, slotA[16] = {}, slotB[16] = {};
  bool valueValid = false;
  int32_t value = 0;
};

inline const char *jobName(Scene::Job j) {
  switch (j) {
    case Scene::Job::None: return "none";
    case Scene::Job::Provision: return "provision";
    case Scene::Job::Rekey: return "rekey";
    case Scene::Job::Wipe: return "wipe";
  }
  return "?";
}

class VaultReader {
 public:
  static constexpr uint32_t kJobMs = 20'000;
  static constexpr size_t kEvents = 40;

  explicit VaultReader(VaultStore &store) : store_(store) {}

  void begin() { xTaskCreatePinnedToCore(task, "vault", 12 * 1024, this, 2, nullptr, 0); }

  bool update(Scene &mine) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mine.version == scene_.version) return false;
    mine = scene_;
    return true;
  }

  // From serial or the web server: the next card laid on the reader gets this job (20 s).
  void startJob(Scene::Job job, const char *callsign = "", vault::Clearance clearance = vault::Clearance::Operator) {
    publish([&](Scene &s) {
      s.job = job;
      strlcpy(s.jobCallsign, callsign, sizeof(s.jobCallsign));
      s.jobClearance = clearance;
      s.jobUntil = millis() + kJobMs;
    });
    present_ = false;  // a card already lying there counts as a new tap for the job
    if (job != Scene::Job::None) log_i("waiting 20 s for a card to %s", jobName(job));
  }

  void requestChipReset() { resetWanted_ = true; }

  std::vector<VaultEvent> events() {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

  bool takeMessage(String &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.empty()) return false;
    out = std::move(outbox_.front());
    outbox_.erase(outbox_.begin());
    return true;
  }

 private:
  static void task(void *self) { static_cast<VaultReader *>(self)->run(); }

  void run() {
    spi_ = new SPIClass(HSPI);  // the S3's SPI3: the screen keeps SPI2 to itself
    const uint8_t v = chip_.begin(*spi_, {PIN_RFID_SS, PIN_RFID_SCK, PIN_RFID_MOSI, PIN_RFID_MISO, PIN_RFID_RST, PIN_RFID_IRQ}, nullptr);
    if (chip_.hasIrqPin()) chip_.setWaitMode(rc522::Mfrc522::WaitMode::Irq);
    log_i("RC522: VersionReg 0x%02X (%s)", v, rc522::versionName(v));
    chipLost_ = !alive(v);
    if (chipLost_) problem("reader chip not answering: check its 3.3V, GND and MISO wires");
    publish([&](Scene &s) { s.chipVersion = v, s.irqMode = chip_.waitMode() == rc522::Mfrc522::WaitMode::Irq; });
    for (;;) {
      if (resetWanted_.exchange(false)) {
        chip_.hardReset();
        const uint8_t again = chip_.read(rc522::VersionReg);
        chipLost_ = !alive(again);
        publish([&](Scene &s) { s.chipVersion = again; });
        log_i("RC522 hard reset: 0x%02X", again);
      }
      expireJob();
      if (millis() - lastHealth_ >= 2000) health();
      if (!chipLost_) pollOnce();
      vTaskDelay(pdMS_TO_TICKS(80));
    }
  }

  static bool alive(uint8_t v) { return v != 0x00 && v != 0xFF; }

  template <typename F>
  void publish(F change) {
    std::lock_guard<std::mutex> lock(mutex_);
    change(scene_);
    scene_.version++;
  }

  void problem(const char *what) {
    log_w("reader: %s", what);
    publish([&](Scene &s) {
      strlcpy(s.problem, what, sizeof(s.problem));
      s.problemAt = millis();
    });
  }

  void expireJob() {
    bool expired = false;
    publish([&](Scene &s) {
      if (s.job != Scene::Job::None && int32_t(s.jobUntil - millis()) <= 0) s.job = Scene::Job::None, expired = true;
    });
    if (expired) log_i("no card within 20 s: job cancelled");
  }

  Scene::Job takeJob(char *callsign, vault::Clearance &clearance) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Scene::Job j = scene_.job;
    strlcpy(callsign, scene_.jobCallsign, 16);
    clearance = scene_.jobClearance;
    scene_.job = Scene::Job::None;
    scene_.version++;
    return j;
  }

  // ---- One card --------------------------------------------------------------------------------

  // Everything read from the card in one visit.
  struct Read {
    iso14443a::Uid uid;
    char hex[21] = "";
    bool gen1a = false;
    vault::Timing timing{};
    uint8_t b0[16] = {}, b4[16] = {}, b5[16] = {}, b6[16] = {}, b8[16] = {}, b9[16] = {};
    rfid::AccessBits s1, s2;  // sectors 1 and 2: may key A write their data blocks?
    bool valueValid = false;
    int32_t value = 0;
  };

  void pollOnce() {
    iso14443a::Uid u;
    chip_.stopCrypto1();
    vault::Timing timing{};
    rc522::Status s = iso14443a::request(chip_, iso14443a::WUPA, u.atqa);
    timing.wupaUs = clampUs(chip_.stats().lastUs);
    if (s == rc522::Status::Ok || s == rc522::Status::Collision) s = iso14443a::select(chip_, u);
    timing.selectUs = clampUs(chip_.stats().lastUs);
    checkIrq();
    const bool found = s == rc522::Status::Ok;
    if (!found) {
      if (present_ && ++misses_ >= 3) {
        present_ = false;
        publish([](Scene &sc) { sc.present = false, sc.presentUid[0] = 0; });
      }
      return;
    }
    misses_ = 0;
    if (present_ && u.sameAs(current_)) {  // still the same card from the last tap
      iso14443a::halt(chip_);
      return;
    }
    present_ = true, current_ = u;
    Read r;
    r.uid = u, r.timing = timing;
    vault::toHex(u.bytes, u.size, r.hex, sizeof(r.hex));
    publish([&](Scene &sc) { sc.present = true, strlcpy(sc.presentUid, r.hex, sizeof(sc.presentUid)); });

    if (!readCard(r)) return;
    char callsign[16];
    vault::Clearance clearance;
    switch (takeJob(callsign, clearance)) {
      case Scene::Job::Provision: provision(r, callsign, clearance); break;
      case Scene::Job::Rekey: rekey(r); break;
      case Scene::Job::Wipe: wipe(r); break;
      case Scene::Job::None: verify(r); break;
    }
    iso14443a::halt(chip_);
    chip_.stopCrypto1();
  }

  static uint16_t clampUs(uint32_t us) { return uint16_t(us > 65535 ? 65535 : us); }

  // Genuine MIFARE Classic cards ignore a 7-bit 0x40 after HLTA; "Gen1a" clone cards answer with
  // a 4-bit ACK and open a back door. We only listen for the ACK, then switch the field off so the
  // card resets and forgets it. A second card nearby could answer too; hold one card at a time.
  bool probeGen1a() {
    iso14443a::halt(chip_);
    const uint32_t keep = chip_.timeoutUs();
    chip_.setTimeoutUs(2000);
    uint8_t cmd = 0x40, back[1] = {}, len = sizeof(back), bits = 7;
    const rc522::Status s = chip_.transceive(&cmd, 1, back, &len, &bits);
    chip_.setTimeoutUs(keep);
    const bool ack = s == rc522::Status::Ok && len == 1 && bits == 4 && (back[0] & 0x0F) == 0x0A;
    if (ack) {
      chip_.antenna(false);
      vTaskDelay(pdMS_TO_TICKS(6));
      chip_.antenna(true);
      vTaskDelay(pdMS_TO_TICKS(6));
    }
    return ack;
  }

  bool readCard(Read &r) {
    r.gen1a = probeGen1a();
    iso14443a::Uid again;
    if (iso14443a::wakeAndSelect(chip_, again) != rc522::Status::Ok || !again.sameAs(r.uid)) return lifted(r, "card left during the clone check");
    r.uid = again;
    if (mifare::authA(chip_, r.uid, 0, mifare::kFactoryKey) != rc522::Status::Ok) return lifted(r, "sector 0 doesn't open with the factory key");
    r.timing.authUs = clampUs(chip_.stats().lastUs);
    if (mifare::readBlock(chip_, 0, r.b0) != rc522::Status::Ok) return lifted(r, "card left while reading block 0");
    r.timing.readUs = clampUs(chip_.stats().lastUs);
    uint8_t t[16];
    if (mifare::authA(chip_, r.uid, 4, mifare::kFactoryKey) != rc522::Status::Ok) return lifted(r, "sector 1 doesn't open with the factory key");
    if (mifare::readBlock(chip_, 4, r.b4) != rc522::Status::Ok || mifare::readBlock(chip_, 5, r.b5) != rc522::Status::Ok ||
        mifare::readBlock(chip_, 6, r.b6) != rc522::Status::Ok || mifare::readBlock(chip_, 7, t) != rc522::Status::Ok ||
        !rfid::AccessBits::decode(t, r.s1))
      return lifted(r, "card left while reading sector 1");
    if (mifare::authA(chip_, r.uid, 8, mifare::kFactoryKey) != rc522::Status::Ok) return lifted(r, "sector 2 doesn't open with the factory key");
    if (mifare::readBlock(chip_, 8, r.b8) != rc522::Status::Ok || mifare::readBlock(chip_, 9, r.b9) != rc522::Status::Ok ||
        mifare::readBlock(chip_, 11, t) != rc522::Status::Ok || !rfid::AccessBits::decode(t, r.s2))
      return lifted(r, "card left while reading sector 2");
    uint8_t addr = 0;
    r.valueValid = rfid::ValueBlock::decode(r.b9, r.value, addr);
    return true;
  }

  bool lifted(Read &r, const char *why) {
    iso14443a::halt(chip_);
    chip_.stopCrypto1();
    vault::Outcome out{};  // reported once: lay the card down again to retry (a waiting job stays)
    out.verdict = vault::Verdict::IoError;
    strlcpy(out.detail, why, sizeof(out.detail));
    report(r, out, nullptr, nullptr);
    return false;
  }

  bool writable(const Read &r) const {
    return r.s1.keyAWrites(0) && r.s1.keyAWrites(1) && r.s1.keyAWrites(2) && r.s2.keyAWrites(0) && r.s2.keyAWrites(1);
  }

  // Writes the vault blocks in the order that survives a cut: tag, block 4, then block 9 (and 8
  // with a fresh label). Returns false at the first block the card didn't take.
  bool writeVault(const Read &r, const vault::Header &h, const vault::Prepared &p, bool withLabel, bool clearOtherSlot) {
    const uint8_t zeros[16] = {};
    uint8_t value[16];
    rfid::ValueBlock::encode(int32_t(h.counter), vault::kValueBlock, value);
    iso14443a::Uid u = r.uid;
    const auto ok = [](rc522::Status s) { return s == rc522::Status::Ok; };
    if (!ok(mifare::authA(chip_, u, 4, mifare::kFactoryKey))) return false;
    if (!ok(mifare::writeBlock(chip_, h.slotBlock(), p.tag))) return false;
    if (clearOtherSlot && !ok(mifare::writeBlock(chip_, h.slot ? vault::kSlotA : vault::kSlotB, zeros))) return false;
    if (!ok(mifare::writeBlock(chip_, vault::kHeaderBlock, p.header))) return false;
    if (!ok(mifare::authA(chip_, u, 8, mifare::kFactoryKey))) return false;
    if (withLabel && !ok(mifare::writeBlock(chip_, vault::kLabelBlock, p.label))) return false;
    return ok(mifare::writeBlock(chip_, vault::kValueBlock, value));
  }

  void verify(Read &r) {
    uint8_t master[32];
    store_.master(master);
    const vault::Policy policy = store_.policy();
    vault::LedgerCard card;
    const bool known = store_.get(r.uid.bytes, r.uid.size, card);
    vault::TapInput in;
    in.uid = r.uid.bytes, in.uidLen = r.uid.size, in.sak = r.uid.sak;
    in.block0 = r.b0, in.block4 = r.b4, in.block5 = r.b5, in.block6 = r.b6, in.block8 = r.b8;
    in.valueValid = r.valueValid, in.value = r.value, in.gen1a = r.gen1a, in.timing = r.timing;
    vault::Outcome out = vault::evaluateTap(master, policy, in, known ? &card : nullptr);

    vault::Header next{};
    bool wrote = false;
    if (vault::granted(out.verdict)) {
      // The next state: one more on the counter, a fresh nonce, the other slot, the ledger's clearance.
      next = out.header;
      next.counter = out.header.counter + 1;
      next.nonce = esp_random();
      next.slot = out.header.slot ^ 1;
      next.clearance = card.clearance;
      const vault::Prepared p = vault::prepare(master, r.uid.bytes, r.uid.size, r.b0, next, out.callsign);
      if (!writable(r)) {
        out.verdict = vault::Verdict::IoError;
        strlcpy(out.detail, "access bits no longer allow key A to write sectors 1-2", sizeof(out.detail));
      } else if (!writeVault(r, next, p, false, false)) {
        out.verdict = vault::Verdict::IoError;
        strlcpy(out.detail, "lifted during the update: the next tap picks it up", sizeof(out.detail));
      } else {
        wrote = true;
        const bool tear = out.verdict == vault::Verdict::RecoveredTear;
        store_.update(r.uid.bytes, r.uid.size, [&](vault::LedgerCard &c) {
          c.counter = next.counter, c.nonce = next.nonce, c.taps++;
          if (tear) c.tears++;
        });
        memcpy(next.slot ? r.b6 : r.b5, p.tag, 16);
        memcpy(r.b4, p.header, 16);
        r.value = int32_t(next.counter), r.valueValid = true;
      }
    } else if (known && vault::isAlert(out.verdict)) {
      const bool quarantine = policy.autoQuarantine && (out.verdict == vault::Verdict::CloneGen1a || out.verdict == vault::Verdict::CloneBlock0 ||
                                                        out.verdict == vault::Verdict::CloneRollback);
      store_.update(r.uid.bytes, r.uid.size, [&](vault::LedgerCard &c) {
        c.alerts++;
        if (quarantine) c.quarantined = true;
      });
      if (quarantine) {
        const size_t n = strlen(out.detail);
        if (n + 14 < sizeof(out.detail)) strlcat(out.detail, " (quarantined)", sizeof(out.detail));
      }
    }
    memset(master, 0, sizeof(master));
    report(r, out, known ? &card : nullptr, wrote ? &next : nullptr);
  }

  void provision(Read &r, const char *callsign, vault::Clearance clearance) {
    vault::Outcome out{};
    vault::LedgerCard card;
    const vault::Policy policy = store_.policy();
    out.forensics = vault::inspect(r.uid.bytes, r.uid.size, r.uid.sak, r.b0, nullptr, r.gen1a, r.timing, nullptr);
    auto refuse = [&](vault::Verdict v, const char *why) {
      out.verdict = v;
      snprintf(out.detail, sizeof(out.detail), "not provisioned: %s", why);
      report(r, out, nullptr, nullptr);
    };
    if (r.gen1a && policy.strictCloneGuard) return refuse(vault::Verdict::CloneGen1a, "a Gen1a magic card");
    if (store_.get(r.uid.bytes, r.uid.size, card)) return refuse(vault::Verdict::IoError, "already in the vault (rekey or wipe it)");
    if (store_.full()) return refuse(vault::Verdict::IoError, "the ledger is full (16)");
    if (!writable(r)) return refuse(vault::Verdict::IoError, "sectors 1-2 don't let key A write");

    card = vault::LedgerCard{};
    memcpy(card.uid, r.uid.bytes, r.uid.size);
    card.uidLen = r.uid.size, card.sak = r.uid.sak;
    strlcpy(card.callsign, callsign, sizeof(card.callsign));
    card.clearance = clearance;
    card.epoch = 1, card.counter = 1, card.nonce = esp_random();
    memcpy(card.block0, r.b0, 16);
    card.baseline = r.timing;
    write(r, card, out, "provisioned");
  }

  void rekey(Read &r) {
    vault::Outcome out{};
    vault::LedgerCard card;
    out.forensics = vault::inspect(r.uid.bytes, r.uid.size, r.uid.sak, r.b0, nullptr, r.gen1a, r.timing, nullptr);
    auto refuse = [&](vault::Verdict v, const char *why) {
      out.verdict = v;
      snprintf(out.detail, sizeof(out.detail), "not rekeyed: %s", why);
      report(r, out, nullptr, nullptr);
    };
    if (!store_.get(r.uid.bytes, r.uid.size, card)) return refuse(vault::Verdict::UnknownCard, "not in the vault (provision it)");
    const vault::Policy policy = store_.policy();
    if (r.gen1a && policy.strictCloneGuard) return refuse(vault::Verdict::CloneGen1a, "a Gen1a magic card");
    if (memcmp(r.b0, card.block0, 16) != 0 && policy.strictCloneGuard) return refuse(vault::Verdict::CloneBlock0, "block 0 isn't the provisioned chip's");
    if (!writable(r)) return refuse(vault::Verdict::IoError, "sectors 1-2 don't let key A write");
    card.epoch++, card.counter++, card.nonce = esp_random(), card.quarantined = false;
    write(r, card, out, "rekeyed");
  }

  // Provision and rekey: a fresh header in slot A, the label, the value block, then the ledger.
  void write(Read &r, vault::LedgerCard &card, vault::Outcome &out, const char *what) {
    uint8_t master[32];
    store_.master(master);
    vault::Header h{};
    h.counter = card.counter, h.epoch = card.epoch, h.clearance = card.clearance, h.slot = 0, h.nonce = card.nonce;
    const vault::Prepared p = vault::prepare(master, r.uid.bytes, r.uid.size, r.b0, h, card.callsign);
    memset(master, 0, sizeof(master));
    const bool ok = writeVault(r, h, p, true, true) && store_.put(card);
    out.header = h, out.headerOk = true;
    strlcpy(out.callsign, card.callsign, sizeof(out.callsign));
    if (ok) {
      out.verdict = vault::Verdict::Verified;
      snprintf(out.detail, sizeof(out.detail), "%s as %s, %s, epoch %u", what, card.callsign, vault::clearanceName(card.clearance), card.epoch);
      memcpy(r.b4, p.header, 16), memcpy(r.b5, p.tag, 16), memset(r.b6, 0, 16);
      r.value = int32_t(h.counter), r.valueValid = true;
      store_.get(r.uid.bytes, r.uid.size, card);  // for its number
    } else {
      out.verdict = vault::Verdict::IoError;
      snprintf(out.detail, sizeof(out.detail), "not %s: card lifted while writing, try again", what);
    }
    report(r, out, ok ? &card : nullptr, ok ? &h : nullptr);
  }

  void wipe(Read &r) {
    vault::Outcome out{};
    const uint8_t zeros[16] = {};
    bool ok = writable(r);
    iso14443a::Uid u = r.uid;
    if (ok) {
      ok = mifare::authA(chip_, u, 4, mifare::kFactoryKey) == rc522::Status::Ok;
      for (uint8_t b : {vault::kHeaderBlock, vault::kSlotA, vault::kSlotB}) ok = ok && mifare::writeBlock(chip_, b, zeros) == rc522::Status::Ok;
      ok = ok && mifare::authA(chip_, u, 8, mifare::kFactoryKey) == rc522::Status::Ok;
      for (uint8_t b : {vault::kLabelBlock, vault::kValueBlock}) ok = ok && mifare::writeBlock(chip_, b, zeros) == rc522::Status::Ok;
    }
    const bool forgot = ok && store_.remove(r.uid.bytes, r.uid.size);
    out.verdict = ok ? vault::Verdict::Unprovisioned : vault::Verdict::IoError;
    snprintf(out.detail, sizeof(out.detail), "%s", !ok ? "not wiped: lifted, or the access bits don't allow it"
                                                   : forgot ? "blocks 4-6, 8-9 zeroed and removed from the ledger"
                                                            : "blocks 4-6, 8-9 zeroed");
    if (ok) memset(r.b4, 0, 16), memset(r.b5, 0, 16), memset(r.b6, 0, 16), r.valueValid = false;
    report(r, out, nullptr, nullptr);
  }

  // ---- Telling the screen, the log and the app --------------------------------------------------

  void report(const Read &r, const vault::Outcome &out, const vault::LedgerCard *card, const vault::Header *wrote) {
    VaultEvent ev;
    publish([&](Scene &s) {
      ev.seq = ++s.seq;
      s.at = millis();
      strlcpy(s.uid, r.hex, sizeof(s.uid));
      s.number = card ? card->number : 0;
      strlcpy(s.callsign, card ? card->callsign : out.callsign, sizeof(s.callsign));
      s.verdict = out.verdict;
      strlcpy(s.detail, out.detail, sizeof(s.detail));
      s.headerOk = out.headerOk, s.header = out.header;
      s.wrote = wrote != nullptr;
      if (wrote) s.next = *wrote;
      s.forensics = out.forensics;
      s.timing = r.timing;
      s.baseline = card ? card->baseline : vault::Timing{};
      s.hasSaved = card != nullptr;
      memcpy(s.block0, r.b0, 16);
      if (card) memcpy(s.saved0, card->block0, 16);
      memcpy(s.block4, r.b4, 16), memcpy(s.slotA, r.b5, 16), memcpy(s.slotB, r.b6, 16);
      s.valueValid = r.valueValid, s.value = r.value;
      ev.number = s.number;
      strlcpy(ev.callsign, s.callsign, sizeof(ev.callsign));
    });
    ev.at = time(nullptr) > 1'700'000'000 ? uint32_t(time(nullptr)) : 0;
    strlcpy(ev.uid, r.hex, sizeof(ev.uid));
    ev.verdict = out.verdict;
    ev.counter = wrote ? wrote->counter : out.header.counter;
    ev.risk = out.forensics.risk;
    strlcpy(ev.detail, out.detail, sizeof(ev.detail));
    if (vault::isAlert(out.verdict)) log_w("[%s] %s %s: %s", vault::verdictName(out.verdict), r.hex, ev.callsign, out.detail);
    else log_i("[%s] %s %s: %s", vault::verdictName(out.verdict), r.hex, ev.callsign, out.detail);

    JsonDocument d;
    d["type"] = "tap";
    eventJson(d["data"].to<JsonObject>(), ev);
    String text;
    serializeJson(d, text);
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() >= kEvents) events_.erase(events_.begin());
    events_.push_back(ev);
    if (outbox_.size() >= 16) outbox_.erase(outbox_.begin());
    outbox_.push_back(std::move(text));
  }

 public:
  // Char arrays go in as const char *: ArduinoJson then copies them (see R7, log entry 44).
  static void eventJson(JsonObject o, const VaultEvent &e) {
    o["seq"] = e.seq;
    o["at"] = e.at;
    o["uid"] = static_cast<const char *>(e.uid);
    o["number"] = e.number;
    o["callsign"] = static_cast<const char *>(e.callsign);
    o["verdict"] = vault::verdictId(e.verdict);
    o["label"] = vault::verdictName(e.verdict);
    o["granted"] = vault::granted(e.verdict);
    o["alert"] = vault::isAlert(e.verdict);
    o["counter"] = e.counter;
    o["risk"] = e.risk;
    o["detail"] = static_cast<const char *>(e.detail);
  }

 private:
  // ---- Reader health (R7's lessons) -------------------------------------------------------------

  void checkIrq() {
    if (!chip_.takeIrqMissed() || chip_.waitMode() != rc522::Mfrc522::WaitMode::Irq) return;
    chip_.setWaitMode(rc522::Mfrc522::WaitMode::Poll);
    publish([](Scene &s) { s.irqMode = false; });
    char msg[72];
    snprintf(msg, sizeof(msg), "IRQ wire on GPIO %d stays quiet: polling instead", PIN_RFID_IRQ);
    problem(msg);
  }

  void health() {
    lastHealth_ = millis();
    const uint8_t v = chip_.read(rc522::VersionReg);
    if (alive(v) && !chipLost_) {
      if (chip_.settingsLost()) {
        chip_.restoreSettings();
        publish([](Scene &s) { s.restores++; });
        problem("the reader chip reset itself (3.3V dip?): settings restored");
      }
      return;
    }
    if (alive(v) == !chipLost_) return;
    if (!alive(v)) {
      chip_.hardReset();
      const uint8_t again = chip_.read(rc522::VersionReg);
      chipLost_ = !alive(again);
      publish([&](Scene &s) { s.chipVersion = again, s.lost++; });
      problem(chipLost_ ? "reader chip stopped answering: check its 3.3V, GND and MISO wires" : "reader chip had hung: a hard reset brought it back");
    } else {
      chipLost_ = false;
      log_i("RC522 answering again: 0x%02X", v);
      publish([&](Scene &s) { s.chipVersion = v; });
    }
  }

  VaultStore &store_;
  rc522::Mfrc522 chip_;
  SPIClass *spi_ = nullptr;
  std::mutex mutex_;
  Scene scene_;
  std::vector<VaultEvent> events_;
  std::vector<String> outbox_;
  std::atomic<bool> resetWanted_{false};
  bool chipLost_ = false, present_ = false;
  iso14443a::Uid current_;
  uint8_t misses_ = 0;
  uint32_t lastHealth_ = 0;
};
