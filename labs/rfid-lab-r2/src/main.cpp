// RFID lab R2 Wake a card.  uv run pio run -d labs/rfid-lab-r2 -e rfid-lab-r2 -t upload
// Wake a card and read its identity: the card view, the sniffer, the receiver gain,
// card polling. Everything from R1 stays.
//
// Wiring: the screen from lesson 03, plus the RC522 (3.3V only):
//   SDA->15  SCK->16  MOSI->17  MISO->18  IRQ->13 (optional)  GND->GND  RST->14  3.3V->3V3
// The push button on GPIO 5 is optional: it does exactly what BOOT does.
//
// Views: chip · card · trace   (button: click = next, double-click = the action in the footer)
// Serial: help lists this lab's commands. The shared code is in kit/lib/rfid-lab-kit and kit/lib/rfid-kit.
#include "lab_commands.h"

namespace {

const LabView kViews[] = {
    {View::Chip, "selftest"},
    {View::Card, nullptr},
    {View::Trace, "trace"},
};

const LabCommand kCommands[] = {labcmd::kChip, labcmd::kReset, labcmd::kSelftest, labcmd::kLoopback, labcmd::kRegs, labcmd::kSpi, labcmd::kGain, labcmd::kTimeout, labcmd::kIrq, labcmd::kPoll, labcmd::kTrace};

const LabSpec kSpec{"rfid-lab-r2", "R2 Wake a card", kViews, sizeof(kViews) / sizeof(kViews[0]), 1,
                    kCommands, sizeof(kCommands) / sizeof(kCommands[0])};

Lab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
