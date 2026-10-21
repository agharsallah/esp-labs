# B8 Host

Track: **Bluetooth LE** · part of the [esp-labs](../../README.md) monorepo.

## `ble-lab-b8`

B8 Host: the board is the central. Explore any device's GATT, then host your BLE keyboard and mouse.

```sh
uv run pio run -d labs/ble-lab-b8 -e ble-lab-b8                 # compile
uv run pio run -d labs/ble-lab-b8 -e ble-lab-b8 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`ble-kit`](../../kit/lib/ble-kit/)
