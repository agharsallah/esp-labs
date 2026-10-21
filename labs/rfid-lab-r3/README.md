# R3 Who's there

Track: **RFID & NFC** · part of the [esp-labs](../../README.md) monorepo.

## `rfid-lab-r3`

R3 Who's there: + inventory (two cards at once).

```sh
uv run pio run -d labs/rfid-lab-r3 -e rfid-lab-r3                 # compile
uv run pio run -d labs/rfid-lab-r3 -e rfid-lab-r3 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`rfid-lab-kit`](../../kit/lib/rfid-lab-kit/)
