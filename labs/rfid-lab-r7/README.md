# R7 Card manager

Track: **RFID & NFC** · part of the [esp-labs](../../README.md) monorepo.

## `rfid-lab-r7`

R7 Card manager: a bench door (simulated: screen + LED) managed from the React app in apps/door-app over Wi-Fi. The board keeps the card list in NVS and decides every tap. JSON API + WebSocket on port 80 (the same web libraries as W2); admin token for every change.

```sh
uv run pio run -d labs/rfid-lab-r7 -e rfid-lab-r7                 # compile
uv run pio run -d labs/rfid-lab-r7 -e rfid-lab-r7 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`door-rules`](../../kit/lib/door-rules/)
- [`rfid-kit`](../../kit/lib/rfid-kit/)
- [`wifi-kit`](../../kit/lib/wifi-kit/)

## Companion app

- [`apps/door-app`](../../apps/door-app/README.md)

## Host tests (no board)

Run them all from the repo root with `tools/host-tests.sh`.

- `labs/rfid-lab-r7/test/host/door_rules_test.cpp`
