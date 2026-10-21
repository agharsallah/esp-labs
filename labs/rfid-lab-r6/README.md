# R6 Edge lab

Track: **RFID & NFC** · part of the [esp-labs](../../README.md) monorepo.

## `rfid-lab-r6`

R6 Edge lab: + the edge view and the seven edge experiments.

```sh
uv run pio run -d labs/rfid-lab-r6 -e rfid-lab-r6                 # compile
uv run pio run -d labs/rfid-lab-r6 -e rfid-lab-r6 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`rfid-lab-kit`](../../kit/lib/rfid-lab-kit/)
