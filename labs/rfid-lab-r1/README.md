# R1 Wire & talk

Track: **RFID & NFC** · part of the [esp-labs](../../README.md) monorepo.

## `rfid-lab-r1`

R1 Wire & talk: the chip itself. chip, reset, selftest, loopback, regs, spi, gain, timeout, irq.

```sh
uv run pio run -d labs/rfid-lab-r1 -e rfid-lab-r1                 # compile
uv run pio run -d labs/rfid-lab-r1 -e rfid-lab-r1 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`rfid-lab-kit`](../../kit/lib/rfid-lab-kit/)
