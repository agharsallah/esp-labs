# W1 Scan & join

Track: **Wi-Fi** · part of the [esp-labs](../../README.md) monorepo.

## `wifi-lab-w1`

W1 Scan & join: scanner views, join your network (password in NVS), status view, ping.

```sh
uv run pio run -d labs/wifi-lab-w1 -e wifi-lab-w1                 # compile
uv run pio run -d labs/wifi-lab-w1 -e wifi-lab-w1 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`wifi-kit`](../../kit/lib/wifi-kit/)
