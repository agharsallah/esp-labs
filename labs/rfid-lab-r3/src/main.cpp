// RFID lab R3 Who's there.  uv run pio run -d labs/rfid-lab-r3 -e rfid-lab-r3 -t upload
// Two cards at once: anticollision, inventory. Lay the card and the tag down together.
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
    {View::Card, "inventory"},
    {View::Trace, "trace"},
};

const LabCommand kCommands[] = {labcmd::kChip, labcmd::kReset, labcmd::kSelftest, labcmd::kLoopback, labcmd::kRegs, labcmd::kSpi, labcmd::kGain, labcmd::kTimeout, labcmd::kIrq, labcmd::kPoll, labcmd::kTrace, labcmd::kInventory};

const LabSpec kSpec{"rfid-lab-r3", "R3 Who's there", kViews, sizeof(kViews) / sizeof(kViews[0]), 1,
                    kCommands, sizeof(kCommands) / sizeof(kCommands[0])};

Lab lab(kSpec);

}  // namespace

void setup() { lab.begin(); }

void loop() { lab.loop(); }
