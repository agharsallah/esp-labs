# Diagnostic

Track: **Display & input** · part of the [esp-labs](../../README.md) monorepo.

## `pin-check`

Diagnostic: are the screen wires shorted or broken? Short test + an LED probe on each data pin.

```sh
uv run pio run -d labs/pin-check -e pin-check                 # compile
uv run pio run -d labs/pin-check -e pin-check -t upload -t monitor   # flash + watch
```
