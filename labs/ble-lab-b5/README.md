# B5 Phone app

Track: **Bluetooth LE** · part of the [esp-labs](../../README.md) monorepo.

## `ble-lab-b5`

B5 Phone app: B3's service plus sketch, live sensors and a speed test for the Web Bluetooth app in apps/ble-app.

```sh
uv run pio run -d labs/ble-lab-b5 -e ble-lab-b5                 # compile
uv run pio run -d labs/ble-lab-b5 -e ble-lab-b5 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`ble-kit`](../../kit/lib/ble-kit/)

## Companion app

- [`apps/ble-app`](../../apps/ble-app/README.md)

## Host tests (no board)

Run them all from the repo root with `tools/host-tests.sh`.

- `labs/ble-lab-b5/test/host/b5_canvas_test.cpp`
