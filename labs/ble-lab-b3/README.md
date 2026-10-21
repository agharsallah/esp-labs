# B3 Connect & talk

Track: **Bluetooth LE** · part of the [esp-labs](../../README.md) monorepo.

## `ble-lab-b3`

B3 Connect & talk: a GATT server. LED, message, button, temperature from nRF Connect/LightBlue.

```sh
uv run pio run -d labs/ble-lab-b3 -e ble-lab-b3                 # compile
uv run pio run -d labs/ble-lab-b3 -e ble-lab-b3 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`ble-kit`](../../kit/lib/ble-kit/)
