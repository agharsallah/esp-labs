# S5 Spectrum

Track: **Sound** · part of the [esp-labs](../../README.md) monorepo.

## `sound-lab-s5`

S5 Spectrum: our own FFT. Bands, a waterfall, a whistle tuner, and steering by pitch.

```sh
uv run pio run -d labs/sound-lab-s5 -e sound-lab-s5                 # compile
uv run pio run -d labs/sound-lab-s5 -e sound-lab-s5 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`sound-kit`](../../kit/lib/sound-kit/)

## Host tests (no board)

Run them all from the repo root with `tools/host-tests.sh`.

- `labs/sound-lab-s5/test/host/sound_spectrum_test.cpp`
