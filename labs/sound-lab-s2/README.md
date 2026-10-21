# S2 Loudness

Track: **Sound** · part of the [esp-labs](../../README.md) monorepo.

## `sound-lab-s2`

S2 Loudness: a sound level meter. dB above your room's quiet, fast/slow, a 10-minute diary, alarm.

```sh
uv run pio run -d labs/sound-lab-s2 -e sound-lab-s2                 # compile
uv run pio run -d labs/sound-lab-s2 -e sound-lab-s2 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`sound-kit`](../../kit/lib/sound-kit/)
