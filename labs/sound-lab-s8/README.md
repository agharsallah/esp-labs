# S8 Voice FX

Track: **Sound** · part of the [esp-labs](../../README.md) monorepo.

## `sound-lab-s8`

S8 Voice FX: microphone in, speaker out. A parrot that repeats you as a chipmunk, a giant or a robot, an echo, and a talking-robot mode. Needs the INMP441 + the speaker for good results.

```sh
uv run pio run -d labs/sound-lab-s8 -e sound-lab-s8                 # compile
uv run pio run -d labs/sound-lab-s8 -e sound-lab-s8 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`sound-kit`](../../kit/lib/sound-kit/)

## Host tests (no board)

Run them all from the repo root with `tools/host-tests.sh`.

- `labs/sound-lab-s8/test/host/sound_fx_test.cpp`
