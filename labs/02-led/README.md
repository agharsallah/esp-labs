# First circuit

Track: **Intro** · part of the [esp-labs](../../README.md) monorepo.

## `02-led`

First circuit: an external LED on the breadboard, driven by serial commands.

```sh
uv run pio run -d labs/02-led -e 02-led                 # compile
uv run pio run -d labs/02-led -e 02-led -t upload -t monitor   # flash + watch
```
