# W2 Phone control, variation 1

Track: **Wi-Fi** · part of the [esp-labs](../../README.md) monorepo.

## `wifi-lab-w2`

W2 Phone control, variation 1: W1 plus REST API + WebSocket, phone page, set-up network, firmware updates over HTTP. Control from your own Wi-Fi.

```sh
uv run pio run -d labs/wifi-lab-w2 -e wifi-lab-w2                 # compile
uv run pio run -d labs/wifi-lab-w2 -e wifi-lab-w2 -t upload -t monitor   # flash + watch
```

## `wifi-lab-w2-remote`

W2 Phone control, variation 2: everything above, plus the board dials out to the dashboard server (through a cloudflared or ngrok tunnel) so you can control it from anywhere.

```sh
uv run pio run -d labs/wifi-lab-w2 -e wifi-lab-w2-remote                 # compile
uv run pio run -d labs/wifi-lab-w2 -e wifi-lab-w2-remote -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`wifi-kit`](../../kit/lib/wifi-kit/)

## Companion app

- [`apps/dashboard`](../../apps/dashboard/README.md)
