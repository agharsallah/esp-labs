"""Pull one recorded clip off the board (sound lab S4) over USB and save it as a WAV file.

Close the serial monitor first: only one program can hold the port.

    uv run python tools/sound_pull.py /dev/cu.usbserial-A5069RR4 3              # -> s4-clip-3-....wav
    uv run python tools/sound_pull.py /dev/cu.usbserial-A5069RR4 3 hello.wav    # your own name

It types `send <n>` to the board, which answers with

    WAV-BEGIN <n> <bytes> <name>
    <the WAV file as base64, 76 characters a line>
    WAV-END <crc32 in hex>

and checks both the byte count and the CRC-32 before writing anything. The port is opened with
DTR and RTS released so the board is not reset (some USB-serial chips still pulse them once;
then the clips, which live in RAM, are gone: see tools/read_serial.py). At 115200 baud a clip
takes about four times its own length to arrive.
"""

import argparse
import base64
import binascii
import re
import sys
import time
import zlib
from pathlib import Path

import serial  # pyserial, installed with PlatformIO

BASE64_LINE = re.compile(r"^[A-Za-z0-9+/]+={0,2}$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port")
    parser.add_argument("n", type=int, help="the clip's number (the board's `clips` command lists them)")
    parser.add_argument("out", nargs="?", help="file to write (default: the name the board gives)")
    parser.add_argument("--timeout", type=float, default=10, help="seconds to wait for the board to start (default 10)")
    args = parser.parse_args()

    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 0.5
    port.dtr = False  # set before open(), so opening the port does not reset the chip
    port.rts = False
    try:
        port.open()
    except serial.SerialException as err:
        print(f"Could not open {args.port}: {err}  (is the serial monitor still open?)", file=sys.stderr)
        return 1

    with port:
        port.reset_input_buffer()
        port.write(f"send {args.n}\n".encode())
        header = wait_for_begin(port, args.timeout)
        if header is None:
            return 1
        number, size, name = header
        print(f"Receiving clip {number}: {size} bytes as {name} ...", file=sys.stderr)
        result = receive(port, size)
    if result is None:
        return 1
    data, crc = result

    if len(data) != size:
        print(f"Got {len(data)} bytes, expected {size}: not saved.", file=sys.stderr)
        return 1
    if zlib.crc32(data) != crc:
        print(f"CRC-32 mismatch ({zlib.crc32(data):08x} != {crc:08x}): a damaged line, not saved. Try again.", file=sys.stderr)
        return 1
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        print("That is not a WAV file: not saved.", file=sys.stderr)
        return 1

    out = Path(args.out or name)
    out.write_bytes(data)
    rate = int.from_bytes(data[24:28], "little")
    samples = (len(data) - 44) // 2
    print(f"Saved {out} ({samples / rate:.1f} s at {rate} Hz, {len(data)} bytes, CRC ok)")
    return 0


def wait_for_begin(port: serial.Serial, timeout: float):
    """Skip log lines until WAV-BEGIN. Returns (number, bytes, name) or None."""
    end = time.time() + timeout
    while time.time() < end:
        line = port.readline().decode(errors="replace").strip()
        if line.startswith("WAV-BEGIN "):
            parts = line.split(" ", 3)
            if len(parts) == 4 and parts[1].isdigit() and parts[2].isdigit():
                return int(parts[1]), int(parts[2]), parts[3]
        elif "send <n>" in line or "no finished clip" in line or "recording: stop first" in line:
            print(f"The board said: {line}", file=sys.stderr)
            return None
    print(f"No answer within {timeout:g} s. Is sound-lab-s4 on the board, and the clip number right?", file=sys.stderr)
    return None


def receive(port: serial.Serial, size: int):
    """Collect base64 lines until WAV-END. Returns (bytes, crc) or None."""
    chunks: list[bytes] = []
    received = 0
    quiet_since = time.time()
    while True:
        raw = port.readline()
        if not raw:
            if time.time() - quiet_since > 5:
                print(f"The board went quiet after {received} of {size} bytes.", file=sys.stderr)
                return None
            continue
        quiet_since = time.time()
        line = raw.decode(errors="replace").strip()
        if line.startswith("WAV-END "):
            try:
                return b"".join(chunks), int(line.split()[1], 16)
            except (IndexError, ValueError):
                print(f"Unreadable end line: {line!r}", file=sys.stderr)
                return None
        if not BASE64_LINE.match(line):
            print(f"  (skipped a log line: {line[:60]})", file=sys.stderr)  # another task's log slipped in
            continue
        try:
            chunk = base64.b64decode(line, validate=True)
        except binascii.Error:
            print(f"  (skipped a damaged line at byte {received})", file=sys.stderr)  # the CRC check will catch it
            continue
        chunks.append(chunk)
        received += len(chunk)
        if received % (57 * 200) < 57:
            print(f"  {received * 100 // max(size, 1)} %", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
