// Every serial command of the RFID labs, ready to be listed in a lab's LabSpec. A lab includes
// the ones its lesson teaches; R6 has them all. Each one checks its arguments here, on loop(),
// then hands a job to the reader task: the chip is only ever touched by that task.
#pragma once

#include "lab.h"

namespace labcmd {

using J = Reader::Job;

// ---- R1: the chip ------------------------------------------------------------------------------

inline void chip(Lab &lab, const String &, const String &) { lab.submit(J::Chip); }
inline void reset(Lab &lab, const String &, const String &) { lab.submit(J::Reset); }
inline void selftest(Lab &lab, const String &, const String &) {
  lab.showView(View::Chip);
  lab.submit(J::SelfTest);
}
inline void loopback(Lab &lab, const String &, const String &) { lab.submit(J::Loopback); }
inline void regs(Lab &lab, const String &, const String &) { lab.submit(J::Regs); }
inline void spi(Lab &lab, const String &arg, const String &) {
  long n = 0;
  if (!Lab::number(arg, 1, 40, n)) return lab.warn("spi <MHz>, 1-40 (the chip is rated for 10)");
  lab.submit(J::Spi, n);
}
inline void gain(Lab &lab, const String &arg, const String &) {
  long n = 0;
  if (!Lab::number(arg, 0, 7, n)) return lab.warn("gain <0-7>: 0=18 1=23 4=33 5=38 6=43 7=48 dB");
  lab.submit(J::Gain, n);
}
inline void timeout(Lab &lab, const String &arg, const String &) {
  long n = 0;
  if (!Lab::number(arg, 25, 1'000'000, n)) return lab.warn("timeout <us>, 25-1000000 (default 25000)");
  lab.submit(J::Timeout, n);
}
inline void irq(Lab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("irq on | irq off");
  lab.submit(J::Irq, lower == "on");
}

constexpr LabCommand kChip{"chip", "", "read VersionReg again (92h NXP, B2h/88h clones, 00/FF = no answer)", chip};
constexpr LabCommand kReset{"reset", "", "hard reset of the reader chip (RST pin), then VersionReg", reset};
constexpr LabCommand kSelftest{"selftest", "", "the datasheet's digital self-test (clones: two runs compared)", selftest};
constexpr LabCommand kLoopback{"loopback", "", "3,200 bytes through the chip's FIFO and back", loopback};
constexpr LabCommand kRegs{"regs", "", "15 registers with their bits and meaning", regs};
constexpr LabCommand kSpi{"spi", "<MHz>", "SPI clock, 1-40 (default 4, the chip's limit is 10)", spi};
constexpr LabCommand kGain{"gain", "<0-7>", "receiver gain, 18-48 dB (default 4 = 33 dB)", gain};
constexpr LabCommand kTimeout{"timeout", "<us>", "how long the chip waits for a card (default 25000)", timeout};
constexpr LabCommand kIrq{"irq", "on|off", "wait on the IRQ wire, or poll the chip over SPI", irq};

// ---- R2: the field and the sniffer -------------------------------------------------------------

inline void poll(Lab &lab, const String &, const String &lower) {
  if (lower != "on" && lower != "off") return lab.warn("poll on | poll off");
  lab.submit(J::Poll, lower == "on");
}
// `trace` alone toggles (the trace view's double-click), `trace on|off` sets.
inline void trace(Lab &lab, const String &, const String &lower) {
  if (!lower.isEmpty() && lower != "on" && lower != "off") return lab.warn("trace | trace on | trace off");
  const bool on = lower.isEmpty() ? !lab.traceOn() : lower == "on";
  lab.setTraceOn(on);
  lab.submit(J::Trace, on);
}

constexpr LabCommand kPoll{"poll", "on|off", "look for cards ~12 times a second, or pause", poll};
constexpr LabCommand kTrace{"trace", "[on|off]", "the sniffer: print each card's frames (alone: toggle)", trace};

// ---- R3: two cards at once ---------------------------------------------------------------------

inline void inventory(Lab &lab, const String &, const String &) { lab.submit(J::Inventory); }
constexpr LabCommand kInventory{"inventory", "", "find every card in the field, one by one", inventory};

// ---- R4: the card's memory ---------------------------------------------------------------------

inline void dump(Lab &lab, const String &, const String &) {
  lab.showView(View::Memory);
  lab.submit(J::Dump);
}
inline void read(Lab &lab, const String &arg, const String &) {
  long n = 0;
  if (!Lab::number(arg, 0, 63, n)) return lab.warn("read <block>, 0-63");
  lab.submit(J::Read, n);
}

constexpr LabCommand kDump{"dump", "", "all 16 sectors with the factory key (hold the card still)", dump};
constexpr LabCommand kRead{"read", "<block>", "one block, 0-63", read};

// ---- R5: write and count -----------------------------------------------------------------------

inline void write(Lab &lab, const String &arg, const String &) {
  const int sp = arg.indexOf(' ');
  long n = 0;
  if (sp < 0 || !Lab::number(arg.substring(0, sp), 1, 63, n)) return lab.warn("write <block> <text>, e.g. write 4 hello bench");
  lab.submit(J::Write, n, arg.substring(sp + 1).c_str());
}
inline void credits(Lab &lab, const String &arg, const String &lower) {
  lab.showView(View::Credits);
  if (arg.isEmpty()) return lab.submit(J::Credits);
  const int sp = lower.indexOf(' ');
  const String op = sp < 0 ? lower : lower.substring(0, sp);
  long n = 0;
  if (sp < 0 || !Lab::number(lower.substring(sp + 1), 0, 100000, n)) return lab.warn("credits init|add|use <n>, 0-100000");
  if (op == "init") return lab.submit(J::CreditsInit, n);
  if (op == "add") return lab.submit(J::CreditsAdd, n);
  if (op == "use") return lab.submit(J::CreditsUse, n);
  lab.warn("credits init|add|use <n>");
}

constexpr LabCommand kWrite{"write", "<block> <text>", "16 bytes of text into a data block, read back", write};
constexpr LabCommand kCredits{"credits", "[init|add|use <n>]", "a value block on block 8, backup on 9", credits};

// ---- R6: the edge lab --------------------------------------------------------------------------

inline void edge(Lab &lab, const String &, const String &lower) {
  static const J kOrder[] = {J::EdgeSpi, J::EdgeGain, J::EdgeTimeout, J::EdgeTiming, J::EdgeIrq, J::EdgeCrc, J::EdgeStress};
  lab.showView(View::Edge);
  const int sp = lower.indexOf(' ');
  const String what = sp < 0 ? lower : lower.substring(0, sp);
  if (what == "next") return lab.submit(kOrder[lab.takeNextEdge()], 200);
  if (what == "spi") return lab.submit(J::EdgeSpi);
  if (what == "gain") return lab.submit(J::EdgeGain);
  if (what == "timeout") return lab.submit(J::EdgeTimeout);
  if (what == "timing") return lab.submit(J::EdgeTiming);
  if (what == "irq") return lab.submit(J::EdgeIrq);
  if (what == "crc") return lab.submit(J::EdgeCrc);
  if (what == "stress") {
    long n = 200;
    if (sp >= 0 && !Lab::number(lower.substring(sp + 1), 10, 5000, n)) return lab.warn("edge stress [10-5000]");
    return lab.submit(J::EdgeStress, n);
  }
  lab.warn("edge spi | gain | timeout | timing | irq | crc | stress [n] | next");
}

constexpr LabCommand kEdge{"edge", "<test>", "spi gain timeout timing irq crc stress [n], or next", edge};

}  // namespace labcmd
