# B1 Listen

Track: **Bluetooth LE** · part of the [esp-labs](../../README.md) monorepo.

## `ble-lab-b1`

B1 Listen: a scanner. Radar, list, a byte-by-byte packet inspector, air statistics.

```sh
uv run pio run -d labs/ble-lab-b1 -e ble-lab-b1                 # compile
uv run pio run -d labs/ble-lab-b1 -e ble-lab-b1 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`ble-kit`](../../kit/lib/ble-kit/)
