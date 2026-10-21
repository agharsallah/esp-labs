# R5 Write & count

Track: **RFID & NFC** · part of the [esp-labs](../../README.md) monorepo.

## `rfid-lab-r5`

R5 Write & count: + the credits view, write, credits.

```sh
uv run pio run -d labs/rfid-lab-r5 -e rfid-lab-r5                 # compile
uv run pio run -d labs/rfid-lab-r5 -e rfid-lab-r5 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`rfid-lab-kit`](../../kit/lib/rfid-lab-kit/)
