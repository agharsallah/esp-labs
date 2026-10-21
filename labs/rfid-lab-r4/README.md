# R4 Inside the card

Track: **RFID & NFC** · part of the [esp-labs](../../README.md) monorepo.

## `rfid-lab-r4`

R4 Inside the card: + the memory view, dump, read.

```sh
uv run pio run -d labs/rfid-lab-r4 -e rfid-lab-r4                 # compile
uv run pio run -d labs/rfid-lab-r4 -e rfid-lab-r4 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`rfid-lab-kit`](../../kit/lib/rfid-lab-kit/)
