# B2 Be seen

Track: **Bluetooth LE** · part of the [esp-labs](../../README.md) monorepo.

## `ble-lab-b2`

B2 Be seen: a beacon. Live data in the advertisement, iBeacon and Eddystone-URL, TX power, interval.

```sh
uv run pio run -d labs/ble-lab-b2 -e ble-lab-b2                 # compile
uv run pio run -d labs/ble-lab-b2 -e ble-lab-b2 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`ble-kit`](../../kit/lib/ble-kit/)

## Host tests (no board)

Run them all from the repo root with `tools/host-tests.sh`.

- `labs/ble-lab-b2/test/host/ble_beacon_test.cpp`
