# R2 Wake a card

Track: **RFID & NFC** · part of the [esp-labs](../../README.md) monorepo.

## `rfid-lab-r2`

R2 Wake a card: + the trace view, poll, trace.

```sh
uv run pio run -d labs/rfid-lab-r2 -e rfid-lab-r2                 # compile
uv run pio run -d labs/rfid-lab-r2 -e rfid-lab-r2 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`rfid-lab-kit`](../../kit/lib/rfid-lab-kit/)
