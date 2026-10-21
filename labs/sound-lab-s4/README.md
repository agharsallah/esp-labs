# S4 Recorder

Track: **Sound** · part of the [esp-labs](../../README.md) monorepo.

## `sound-lab-s4`

S4 Recorder: a voice-activated recorder into PSRAM, with half a second of "before". Listen and download on your phone at http://bench.local (Wi-Fi saved by W1, admin token), or pull a WAV over USB with tools/sound_pull.py.

```sh
uv run pio run -d labs/sound-lab-s4 -e sound-lab-s4                 # compile
uv run pio run -d labs/sound-lab-s4 -e sound-lab-s4 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`sound-kit`](../../kit/lib/sound-kit/)
- [`wifi-kit`](../../kit/lib/wifi-kit/)

## Host tests (no board)

Run them all from the repo root with `tools/host-tests.sh`.

- `labs/sound-lab-s4/test/host/sound_clips_test.cpp`
