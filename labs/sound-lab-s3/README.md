# S3 Clap control

Track: **Sound** · part of the [esp-labs](../../README.md) monorepo.

## `sound-lab-s3`

S3 Clap control: claps (not voices) in patterns of 1-4 switch the LED, the RGB LED and the screen.

```sh
uv run pio run -d labs/sound-lab-s3 -e sound-lab-s3                 # compile
uv run pio run -d labs/sound-lab-s3 -e sound-lab-s3 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`sound-kit`](../../kit/lib/sound-kit/)
