# Meet the board

Track: **Intro** · part of the [esp-labs](../../README.md) monorepo.

## `01-hello`

Meet the board: print its identity card and cycle the on-board RGB LED.

```sh
uv run pio run -d labs/01-hello -e 01-hello                 # compile
uv run pio run -d labs/01-hello -e 01-hello -t upload -t monitor   # flash + watch
```
