# Bench door app (R7)

A small React app that manages the cards of the `rfid-lab-r7` firmware: a bench door (simulated by the screen and the LED) on your ESP32 with the RC522.

```mermaid
flowchart LR
  App["React app<br/>localhost:5174"] -->|"/api, /ws"| Vite["Vite dev server<br/>adds the admin token from .env"]
  Vite -->|"HTTP + WebSocket over Wi-Fi"| Web["board: web server<br/>door_api.h"]
  Web -->|"jobs (mailbox)"| Reader["board: reader task<br/>door.h"]
  Card["your card or tag"] -->|"UID only"| Reader
  Reader -->|"decide"| List[("card list in NVS<br/>registry.h + access_rules.h")]
  Web --> List
```

The board keeps the list and decides every tap; the card only brings its UID. The app never holds the token: Vite's proxy adds it on your Mac.

## Run it

1. Flash the firmware: `cd esp-labs && uv run pio run -d labs/rfid-lab-r7 -e rfid-lab-r7 -t upload` (it uses the Wi-Fi network saved by W1/W2).
2. In the serial monitor type `token` and copy the value.
3. Then:

```sh
cd esp-labs/apps/door-app
cp .env.example .env       # BENCH_URL=http://bench.local (or the board's IP), BENCH_TOKEN=<token>
pnpm install
pnpm dev                   # http://localhost:5174
```

## What you can do

| In the app                                       | What the board does                                                                                                                 |
| ------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------- |
| **Add card** (name, role)                        | waits 20 s for a card it hasn't seen (screen: ADD A CARD, LED blinking blue) and adds it; guests start with 5 uses                  |
| **Block / Unblock**                              | a blocked card is refused before any other rule                                                                                     |
| **Edit** · hours, end date, uses, role           | the rules for the next tap; **presets**: Resident, Office hours, Night shift (across midnight), Weekend guest, One visit, Lost card |
| **Write name label** (card on the reader)        | name into block 4, role into block 5, read back; the door never trusts it                                                           |
| **Wipe card data** (card on the reader, confirm) | data blocks 4-6 and 8-10 back to zeros where the access bits allow; never keys, trailers or block 0                                 |
| **Remove from the list** (confirm)               | forgets the card; the card itself is untouched                                                                                      |
| **Open 5 s** (per lock), **Lockdown**            | remote open of the front door or the admin room; only maintenance and admin cards open during lockdown                              |
| **Move reader here**                             | the bench's one reader is mounted at one lock at a time: the front door (every role) or the admin room (admin cards only)           |
| **Read the card on the reader**                  | UID, type, whether it is in the list, blocks 4-6 and 8-10                                                                           |
| **Edit → Copy rules from…**                      | fills the form with another card's role, hours, end date and uses (not its name or block); Save sends what changed                  |
| **Edit → Replace with a new card** (confirm)     | waits 20 s for a new card; it takes over the name, role and rules, the old card stays in the list blocked as "… old"                |
| **Card data → Save the card on the reader**      | reads blocks 4-6 and 8-10 with the factory key into a named snapshot on the board (up to 8, NVS)                                    |
| **Card data → Copy this card onto another**      | reads the card on the reader, waits 20 s for a different card (screen: COPY: 2ND CARD, LED blinking violet), writes and reads back  |
| **Write onto the card on the reader** (confirm)  | a snapshot back onto the card it came from (restore) or onto another card                                                           |
| **Compare with the reader**                      | the card on the reader next to a snapshot, block by block: same, different, not saved, closed                                       |
| **Download**, **Import a saved file**            | a snapshot as a JSON file on your Mac, and back onto the board (lab blocks only, checked by the app and the board)                  |

A copy moves data, never identity: block 0 (the UID) and the sector trailers (keys, access bits) are never written, so the copy is a different card to the door and needs its own place in the list.

```mermaid
sequenceDiagram
  participant App as React app
  participant Board as board (reader task)
  participant A as card A
  participant B as card B
  App->>Board: POST /api/copy
  Board->>A: AUTH sector 1, 2 (factory key) · READ 4-6, 8-10
  Note over Board: screen COPY: 2ND CARD, LED violet, 20 s
  B->>Board: a different UID
  Board->>B: WRITE each block, READ it back
  Board-->>App: job: "6 of 6 blocks copied"
```

The rule order (`kit/lib/door-rules/src/access_rules.h`, tested in `labs/rfid-lab-r7/test/host/door_rules_test.cpp`): unknown → blocked → admin room needs an admin card → lockdown → no clock yet (fail closed) → expired → outside hours → no uses left → granted.

## Checks

```sh
pnpm typecheck
pnpm test        # rules.test.ts: rule text, dates, presets, what a save sends, copy rules
                 # cardData.test.ts: block text and values, compare, the snapshot file
```
