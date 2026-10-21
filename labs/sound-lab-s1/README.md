# S1 Hear

Track: **Sound** · part of the [esp-labs](../../README.md) monorepo.

## `sound-lab-s1`

S1 Hear: the raw signal. Oscilloscope, the DC bias and the ADC's range, the comparator (DO + pot).

```sh
uv run pio run -d labs/sound-lab-s1 -e sound-lab-s1                 # compile
uv run pio run -d labs/sound-lab-s1 -e sound-lab-s1 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`sound-kit`](../../kit/lib/sound-kit/)
