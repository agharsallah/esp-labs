# Variation 1

Track: **Display & input** · part of the [esp-labs](../../README.md) monorepo.

## `03b-showcase`

Variation 1: a bigger display demo on the same wiring (3D torus, plasma, fireworks, live dashboard). Controlled with the on-board BOOT button. Nothing new to wire.

```sh
uv run pio run -d labs/03b-showcase -e 03b-showcase                 # compile
uv run pio run -d labs/03b-showcase -e 03b-showcase -t upload -t monitor   # flash + watch
```

## `03b-showcase-button`

Variation 2: the same showcase, driven by a push button on GPIO 5 (plus BOOT).

```sh
uv run pio run -d labs/03b-showcase -e 03b-showcase-button                 # compile
uv run pio run -d labs/03b-showcase -e 03b-showcase-button -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
