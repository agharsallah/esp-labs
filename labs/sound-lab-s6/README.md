# S6 Sound games

Track: **Sound** · part of the [esp-labs](../../README.md) monorepo.

## `sound-lab-s6`

S6 Sound games: a knock lock (your secret rhythm), Clap Bird, and a beat-following light party.

```sh
uv run pio run -d labs/sound-lab-s6 -e sound-lab-s6                 # compile
uv run pio run -d labs/sound-lab-s6 -e sound-lab-s6 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`sound-kit`](../../kit/lib/sound-kit/)

## Host tests (no board)

Run them all from the repo root with `tools/host-tests.sh`.

- `labs/sound-lab-s6/test/host/sound_games_test.cpp`
