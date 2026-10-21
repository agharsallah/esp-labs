# R8 Card friends

Track: **RFID & NFC** · part of the [esp-labs](../../README.md) monorepo.

## `rfid-lab-r8`

R8 Card friends: a kids' lab. Every card is the home of a friend (fox, robot, dragon, ...): tap it and the friend pops out on the screen and in 3D in apps/friends-app, where you drag friends onto cards. New cards add themselves on their first tap. UID only, nothing is written. R7's driver, reader health checks, web libraries and admin token.

```sh
uv run pio run -d labs/rfid-lab-r8 -e rfid-lab-r8                 # compile
uv run pio run -d labs/rfid-lab-r8 -e rfid-lab-r8 -t upload -t monitor   # flash + watch
```

## Kit libraries

- [`bench`](../../kit/lib/bench/)
- [`door-rules`](../../kit/lib/door-rules/)
- [`rfid-kit`](../../kit/lib/rfid-kit/)
- [`wifi-kit`](../../kit/lib/wifi-kit/)

## Companion app

- [`apps/friends-app`](../../apps/friends-app/README.md)
