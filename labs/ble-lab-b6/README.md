# B6 Keyboard & media

Track: **Bluetooth LE** · part of the [esp-labs](../../README.md) monorepo.

## `ble-lab-b6`

B6 Keyboard & media: a BLE keyboard with media keys. Volume and play/pause for the music on your phone or Mac.

```sh
uv run pio run -d labs/ble-lab-b6 -e ble-lab-b6                 # compile
uv run pio run -d labs/ble-lab-b6 -e ble-lab-b6 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`ble-kit`](../../kit/lib/ble-kit/)

## Host tests (no board)

Run them all from the repo root with `tools/host-tests.sh`.

- `labs/ble-lab-b6/test/host/ble_keymap_test.cpp`
