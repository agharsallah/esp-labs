// RFID lab R6 Edge lab.  uv run pio run -d labs/rfid-lab-r6 -e rfid-lab-r6 -t upload
// Push one setting at a time until something breaks: SPI clock, gain, timeouts, timing,
// IRQ vs polling, CRC, a stress run. Every command of R1-R5 is here too.
//
// Wiring: the screen from lesson 03, plus the RC522 (3.3V only):
//   SDA->15  SCK->16  MOSI->17  MISO->18  IRQ->13 (optional)  GND->GND  RST->14  3.3V->3V3
// The push button on GPIO 5 is optional: it does exactly what BOOT does.
//
// Views: chip · card · trace · memory · credits · edge   (button: click = next, double-click = the action in the footer)
// Serial: help lists this lab's commands. The shared code is in kit/lib/rfid-lab-kit and kit/lib/rfid-kit.
#include "lab_commands.h"

namespace {

const LabView kViews[] = {
    {View::Chip, "selftest"},
    {View::Card, "inventory"},
    {View::Trace, "trace"},
    {View::Memory, "dump"},
    {View::Credits, "credits add 1"},
    {View::Edge, "edge next"},
};

const LabCommand kCommands[] = {labcmd::kChip, labcmd::kReset, labcmd::kSelftest, labcmd::kLoopback, labcmd::kRegs, labcmd::kSpi, labcmd::kGain, labcmd::kTimeout, labcmd::kIrq, labcmd::kPoll, labcmd::kTrace, labcmd::kInventory, labcmd::kDump, labcmd::kRead, labcmd::kWrite, labcmd::kCredits, labcmd::kEdge};

const LabSpec kSpec{"rfid-lab-r6", "R6 Edge lab", kViews, sizeof(kViews) / sizeof(kViews[0]), 5,
                    kCommands, sizeof(kCommands) / sizeof(kCommands[0])};

Lab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
