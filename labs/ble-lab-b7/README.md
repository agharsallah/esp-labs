# B7 Music

Track: **Bluetooth LE** · part of the [esp-labs](../../README.md) monorepo.

## `ble-lab-b7`

B7 Music: a BLE-MIDI instrument for GarageBand or the ble-app synth. Button and touch wires play notes.

```sh
uv run pio run -d labs/ble-lab-b7 -e ble-lab-b7                 # compile
uv run pio run -d labs/ble-lab-b7 -e ble-lab-b7 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`ble-kit`](../../kit/lib/ble-kit/)

## Companion app

- [`apps/ble-app`](../../apps/ble-app/README.md)
