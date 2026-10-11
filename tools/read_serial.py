"""Print the board's serial output for a few seconds, then exit.

Unlike `pio device monitor`, this needs no interactive terminal, so Claude can run it
and paste the result into HARDWARE_LOG.md.

    uv run python tools/read_serial.py /dev/cu.usbserial-A5069RR4 10            # just listen
    uv run python tools/read_serial.py /dev/cu.usbserial-A5069RR4 10 --reset    # restart, then listen

Without --reset we leave DTR and RTS released, which normally leaves the EN (reset) pin
alone. Some USB-serial chips still pulse those lines while the port opens (seen with this
bench's FTDI FT232R on macOS), which restarts the board once. Harmless, but expect it.
"""

import argparse
import sys
import time

import serial  # pyserial, installed with PlatformIO


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port")
    parser.add_argument("seconds", type=float)
    parser.add_argument("--reset", action="store_true", help="pulse RTS to restart the board first")
    args = parser.parse_args()

    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 0.2
    port.dtr = False  # set before open(), so opening the port does not reset the chip
    port.rts = False
    try:
        port.open()
    except serial.SerialException as err:
        print(f"Could not open {args.port}: {err}", file=sys.stderr)
        return 1

    if args.reset:
        port.rts = True  # RTS pulls EN low = reset
        time.sleep(0.1)
        port.rts = False

    end = time.time() + args.seconds
    received = 0
    while time.time() < end:
        data = port.read(512)
        if data:
            received += len(data)
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()
    port.close()
    if received == 0:
        print(f"(no output in {args.seconds:g} s)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
