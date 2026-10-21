# B4 Pair & trust

Track: **Bluetooth LE** · part of the [esp-labs](../../README.md) monorepo.

## `ble-lab-b4`

B4 Pair & trust: B3's service plus a Vault. Passkey on the screen, numeric comparison on the button, bonds.

```sh
uv run pio run -d labs/ble-lab-b4 -e ble-lab-b4                 # compile
uv run pio run -d labs/ble-lab-b4 -e ble-lab-b4 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`ble-kit`](../../kit/lib/ble-kit/)
