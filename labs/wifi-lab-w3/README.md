# W3 Internet & AI

Track: **Wi-Fi** · part of the [esp-labs](../../README.md) monorepo.

## `wifi-lab-w3`

W3 Internet & AI: weather over HTTPS + JSON (Open-Meteo, no key), and the Bench Oracle: Claude (Anthropic API, key stored on the chip) writes a haiku about the moment or answers questions.

```sh
uv run pio run -d labs/wifi-lab-w3 -e wifi-lab-w3                 # compile
uv run pio run -d labs/wifi-lab-w3 -e wifi-lab-w3 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`wifi-kit`](../../kit/lib/wifi-kit/)

## Companion app

- [`apps/ai-gateway`](../../apps/ai-gateway/README.md)
