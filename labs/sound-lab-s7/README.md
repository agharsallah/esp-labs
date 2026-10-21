# S7 Speaker

Track: **Sound** · part of the [esp-labs](../../README.md) monorepo.

## `sound-lab-s7`

S7 Speaker: the board makes sound. Waveforms, envelopes, notes and tunes, a keyboard, a sound-effects box.

```sh
uv run pio run -d labs/sound-lab-s7 -e sound-lab-s7                 # compile
uv run pio run -d labs/sound-lab-s7 -e sound-lab-s7 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`sound-kit`](../../kit/lib/sound-kit/)
