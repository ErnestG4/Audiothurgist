# Audiothurgist — web frontend

`index.html` is the instrument's face in a browser: the live bar, per-slice
waveforms you can select and punch into steps, an output scope, a **Controls
panel that always says what B1/B2/B3 and the six knobs do right now**, the
module's Options, and its manual. It speaks **HostLink over Web Serial** to
the module's front USB-C — the same protocol Hermetic's `/program` uses —
plus the firmware's `StateExt` (commands 0x60–0x63, v0.7.0+; the v0.8.0
state tail for locks, native BPM and bars; the CLOCK package's jitter tail
when the firmware sends it). No dependencies, three static files,
phone-friendly.

| file | what |
|---|---|
| `index.html` | the page (UI, polling, rendering) |
| `hostlink.js` | HostLink framing, the priority link, StateExt parsers, descriptor helpers — shared with the self-test |
| `demo.js` | a fake module behind a fake serial port, for the Demo button and the self-test |
| `selftest.mjs` | node: golden-vector check + synthetic frames + an end-to-end run over the fake module |

## Running it

Web Serial needs a **secure context** and a Chromium browser (Chrome, Edge,
Opera). `file://` will not work. Either:

- **Locally:** `cd web && python3 -m http.server 8080` → http://localhost:8080
- **Hosted:** publish `web/` on Codeberg Pages (a `pages` branch, or a repo
  named `pages`) → https://combust.codeberg.page/Audiothurgist/

Close the `/program` tab first — one page holds the serial port at a time.
Press **Connect**, pick the Alchemy Lab (0483:5740), and the bar starts moving.

**No module to hand?** Press **Demo** (or open `index.html?demo`) — the page
drives itself from `demo.js`, a fake module that answers real HostLink
frames. `?demo=brain,ext,grids,locks` starts it on the Brain page, locked to
a clock at ×2, in Grids, with two motion locks; `?demo=old` sends a
pre-v0.8.0 state body so you can see the fallbacks. `#opts` / `#man` open a
tab directly.

## What is on the page

- **Header pills:** module + firmware, current page, clock state
  (internal / following / locked), BPM with the multiplier, the **link**
  round-trip (last / worst in the last second · requests per second ·
  queue depth), and the firmware's own diag pill (worst poll gap and audio
  callback, playhead jumps, regenerations).
- **The bar:** ring + strip with the playhead, plus grammar, classifier,
  chop, history, sample, page, **native BPM**, **grid** (bars · steps), the
  **Bars** option (editable here as well as in Options), and the clock line
  (multiplier zone and pulse jitter when the firmware reports them).
- **Controls:** every button gesture as the firmware implements it (B1 tap
  = page, B1 held 300 ms + pot = motion lock, hold + nudge again clears; B2
  tap dice / hold flow; B3 tap reslice / hold load; B2+B3 2 s = Settings),
  with on-page buttons wherever the protocol has an action: Switch page
  (ACTION 8), Clear locks (7), Dice (0), Flow (4), Reslice (1), Load
  selected (6), Mute (2). Below it the six knobs laid out as on the panel
  for the **current page** (flags bit 4), with what each one means under
  the current grammar and clock: P1 is the chop macro in Classic/Collins
  and slice-choice faithfulness in Grids/Euclid; P2 is tempo internally and
  the ÷4 ÷2 ×1 ×2 ×4 multiplier while following (the active zone is
  highlighted); Brain knobs are marked inert in Classic/Collins and
  unused where a grammar ignores them. Pots carrying an active motion lock
  (`lockMask`) are badged. Knob names come from the descriptor's pager,
  the meanings from the firmware source.
- **Options tab:** every enum field of the module's Options block, labels
  and names straight from the descriptor, grouped Sound / Brain & gates /
  Clock / Analysis (anything new lands in "Other"). A change reads the live
  blob fresh, flips one byte and writes it back as a live audition
  (`BLOB_BEGIN 0xFF → DATA → COMMIT`), exactly what `/program` does — it is
  not saved until a preset is saved. If the panel changes an option (a
  preset load), the page notices through the state and re-reads the blob.
- **Manual tab:** the descriptor's manual block (tagline, preamble,
  sections), the pager's page and knob help, the jack help and the presets
  help — all firmware-authored (`src/audiothurgist_manual.cpp`). The button
  table is the one thing not in the descriptor: this firmware declares its
  pots through a `Pager`, which carries no button metadata, so the gestures
  are mirrored from `OnPoll`.

## Responsiveness

- **One request in flight, two queues.** Anything the user does (buttons,
  punches, loads, option writes) goes out `urgent` and is pumped before any
  queued poll. Polls are keyed, so a slow link never piles them up.
- **Cadence:** state at 10 Hz; scope at 5 Hz and only while its canvas is
  on screen; envelope only when `matGen` changes; live blob only on demand.
  Polling pauses while the tab is hidden.
- **Rendering:** frames set dirty flags; one `requestAnimationFrame` draws
  only what changed. Canvas backing stores are sized by `ResizeObserver`;
  nothing measures layout in the draw loop; text nodes are touched only
  when their string changes.
- An option write on real hardware moves the whole live blob (the motion
  locks make it ~12 KB) — expect the display to hold for well under a
  second while it goes through.

## Testing without hardware

```
node web/selftest.mjs
```

pins `hostlink.js` to the SDK's golden vectors
(`../alchemy-sdk/tests/host/golden/hostlink_golden.json`: 11/11 — CRC32,
COBS, the HELLO frame both ways — plus every recorded exchange frame and
the golden descriptor), parses synthetic `GET_STATE` bodies of every length
the firmware has shipped (148 / 156 / 164 / 167 bytes), and runs the real
`Link` over the fake module: priority ordering, coalescing, status errors,
envelope fetch, and a `GET_LIVE → BLOB` option round-trip. Pass
`--golden <path>` if the SDK lives elsewhere.

## What talks to what

| Page needs               | HostLink command                  | Notes                                        |
|--------------------------|-----------------------------------|----------------------------------------------|
| identity, max body       | `0x01 HELLO`                      | standard                                     |
| labels, help, manual     | `0x02 GET_DESCRIPTOR`             | the module's own descriptor JSON             |
| option values            | `0x20 GET_LIVE`                   | at connect, before each write, on mismatch   |
| option edits             | `0x12/0x13/0x14 BLOB_*` to `live` | audition; save a preset to keep it           |
| bar / playhead / tails   | `0x60 GET_STATE`                  | 10 Hz; tails parsed by length                |
| waveform                 | `0x61 GET_ENV`                    | refetched when the material changes          |
| scope                    | `0x62 SCOPE`                      | 5 Hz while visible; 256 samples ×4 decimated |
| buttons, punch, scrub    | `0x63 ACTION`                     | 0 dice · 1 reslice · 2 mute · 3 punch · 4 flow · 5 scrub · 6 load · 7 clear locks · 8 page |

Flashing, presets and settings stay in `/program`; this page is deliberately
the *instrument* view.
