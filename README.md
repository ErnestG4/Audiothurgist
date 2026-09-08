# Audiothurgist — web frontend

`index.html` is the instrument's face in a browser: the live bar, per-slice
waveforms you can select and punch into steps, an output scope, a **Controls
panel that always says what B1/B2/B3 and the six knobs do right now**, the
module's Options, and its manual. It speaks **HostLink over Web Serial** to
the module's front USB-C — the same protocol Hermetic's `/program` uses —
plus the firmware's `StateExt` (commands 0x60–0x66, v0.7.0+; the v0.8.0
state tail for locks, native BPM and bars; the CLOCK package's jitter,
multiplier-zone and clock-flags tail; the sample-mode tail — record state,
buffer bookkeeping, key table — and the `GET_WAVE` / `GET_TABLE` /
`GET_HITS` reads behind the **Kit** tab when the firmware sends them). No
dependencies, three static files, phone-friendly.

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
a clock at ×2, in Grids, with two motion locks; `?demo=ext,pending` leaves a
×4 switch waiting for its beat; `?demo=camp` starts in Camp; **`?demo=kit`**
opens the Kit tab on a swung 32-slice break with three keys already
recorded (arm a key and the fake take runs armed → taking → pending →
committed by itself; the looper count drifts up); `?demo=old` sends a
pre-v0.8.0 state body, `?demo=short` the 167-byte CLOCK body (no flags)
and `?demo=v099` the 176-byte body (no sample-mode tail) so you can see
the fallbacks. `#kit` / `#opts` / `#man` open a tab directly.

## What is on the page

- **Header pills:** module + firmware, current page, clock state
  (internal / following / locked), BPM with the multiplier, the **link**
  round-trip (last / worst in the last second · requests per second ·
  queue depth), and the firmware's own diag pill (worst poll gap and audio
  callback, playhead jumps, regenerations).
- **The bar:** ring + strip with the playhead, plus grammar, classifier,
  chop, history, sample, page, **native BPM**, **grid** (bars · steps), the
  **Bars** option (editable here as well as in Options), and the clock line:
  internal / following / locked, the multiplier that is *playing* and — while
  a beat-aligned switch is waiting — the one P2 has *requested* (`×2 → ×4
  pending`), `P2 not engaged` until the knob has moved, `aligned` for ~2 s
  after a lock snaps the loop onto the pulse grid, pulse jitter as peak /
  RMS µs, and `ext n`, the external-clock transition counter (mod 16), when
  the firmware reports them.
- **Controls:** every button gesture as the firmware implements it (B1 tap
  = page, B1 held 300 ms + pot = motion lock, hold + nudge again clears; B2
  tap dice / hold flow; B3 tap reslice / hold load; B2+B3 2 s = Settings),
  with on-page buttons wherever the protocol has an action: Switch page
  (ACTION 8), Clear locks (7), Dice (0), Flow (4), Reslice (1), Load
  selected (6), Mute (2). Below it the six knobs laid out as on the panel
  for the **current page** (flags bit 4), with what each one means under
  the current grammar and clock: P1 is the chop macro in Classic/Collins,
  slice-choice faithfulness in Grids/Euclid and the **number of bells** in
  Camp (full right = rounds, then 4 · 8 · 16); P2 is tempo internally and
  the ÷4 ÷2 ×1 ×2 ×4 multiplier while following (the playing zone is
  highlighted; a requested zone still waiting for its beat is outlined);
  Brain knobs are marked inert in Classic/Collins and unused where a
  grammar ignores them (Euclid's Map X/Y, Grids' Rotation, all six in
  Camp). Under Camp the B2 row reads *ring the next change* rather than
  Dice. Pots carrying an active motion lock (`lockMask`) are badged. Knob
  names come from the descriptor's pager, the meanings from the firmware
  source.
- **Kit tab** (sample mode): the 24 Voice-map keys as cells, two octaves
  from C. An **empty key** shows the span of the break it plays (`slices
  2–4`, from the state's key table); a **recorded key** shows its take's
  length, class colour, whether it opens on a hit, and its salience /
  keyframe density (from `GET_TABLE`). Each cell has **●** (arm a take into
  that key, ACTION 9 — the next gate on the panel's record path starts it)
  and **✕** (clear the recording, ACTION 10); the armed / taking / pending
  key pulses from the state's record byte, with the **input meter** beside
  it. Below, the selected key's **waveform** from `GET_WAVE` windows — drag
  to pan, wheel or − / + to zoom, *fit* / *all*, an L / R / L+R plane
  picker on stereo material — with the slice boundaries from the table
  (white for the grid, yellow for recordings; **swung boundaries are the
  table's, so they land where the slices really open**) and blue onset
  ticks from `GET_HITS`. The **Key plays** toggle is Options → Key Plays
  (ACTION 11: one-shot or held until the gate falls). A **buffer** bar
  shows the break, the recordings and the free room out of `bufCap`, and
  the **looper** card shows the event count from state byte 176 with
  **Undo** (12) and **Clear** (13). The **set** card saves the playing
  material — the break with its recordings, the table, keys, options and
  the loop — as `sets/<name>.wav` + `.json` (ACTION 14, the name as the
  body) and loads one back (15); the buttons follow the state tail's
  `setState`, the progress bar the save's `setProgress`, and each result
  shows once per `setSeq`. The **Play** tab's strip and slice view
  use the same table boundaries instead of assuming `i · step`, and carry
  the onset ticks.
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
  on screen; envelope only when `matGen` changes; the slice table when the
  state's (matGen, nTable, freePos, bufLen, key table) signature changes —
  a take, a clear, a re-slice; the onset table when `matGen` changes;
  live blob only on demand. **Waveform windows** (`GET_WAVE`, ≤ 480 bytes
  each) only while the Kit tab shows, one in flight, **at most 3 per
  second**, nearest the view's centre first, cached per (plane, start,
  decimation) until the buffer changes — a 4 s break at 600 px is five
  windows. Polling pauses while the tab is hidden.
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
the firmware has shipped (148 / 156 / 164 / 167 / 170 / 176 / 177 / 194 /
218 / 219 bytes) plus `GET_WAVE`, `GET_TABLE` and `GET_HITS` bodies, and
runs the real `Link` over the fake module: priority ordering, coalescing,
status errors, envelope fetch, the clock flags on a fake clock, a `GET_LIVE
→ BLOB` option round-trip, the five-label Grammar with Camp at index 4,
and — on a 64-slice kit with three recordings — the chunked table (39 +
28), the onset table, raw and decimated waveform windows with the clamp at
`freePos`, and ACTION 9–13: arm → taking → pending → committed, a second
arm refused, clear re-pointing the keys, Key Plays landing in the Options
byte, looper undo / clear. Pass `--golden <path>` if the SDK lives
elsewhere.

## The GET_STATE body

Fixed 148 bytes, then append-only tails that the page parses by length —
an older firmware simply sends a shorter body and the page falls back.

```
u8 status u8 nSlices u8 curStep u8 flags u8 matGen u8 grammar u8 cls u8 file
u8 chop u8 histN u8 histDepth u8 callFlash f32 bpm f32 mult
i8 stepSrc[64] u8 stepClass[64]                                  (ends at 148)
u16 pollGapUs u16 audioUs u16 jumps u16 regens                    (148..155, v0.7.3)
u16 lockMask f32 nativeBpm u8 gridBars u8 barsOpt                 (156..163, v0.8.0)
u16 jitterUs u8 multZone                                          (164..166, CLOCK)
u8 clkFlags u16 jitterRmsUs                                       (167..169, CLOCK)
u8 stallPhase u16 gateLateUs u16 overruns                         (170..174, v0.9.5)
u8 jackMap                                                        (175, v0.9.9)
u8 looperEvents                                                   (176, LOOPER)
u8 recState u8 recKey u32 freePos u32 bufCap u8 stereo u8 inLevel (177..188, SAMPLE)
u8 nTable u32 bufLen                                              (189..193, SAMPLE)
i8 keySlice[24]                                                   (194..217, SAMPLE)
u8 keyPlays                                                       (218, SAMPLE)
u8 setState u8 setProgress u8 setResult u8 setSeq                 (219..222, SETS)
char setName[32]                                                  (223..254, SETS)
```

`recState`: 0 idle · 1 armed (waiting for the gate) · 2 taking · 3 pending
(stopped, being trimmed and analysed); `recKey` is the key of the take in
flight (255 none). `freePos` is the end of used material — the break
`[0, bufLen)` then the recordings — out of `bufCap` samples per plane;
`inLevel` is the peak of the last input block. `nTable` counts grid +
recorded entries; `keySlice[k]` is the table entry key `k` plays (past
`nSlices` = a recording). `keyPlays` mirrors Options → Key Plays so the
Kit toggle follows the panel. `setState` is the set op in flight (0 idle ·
1 saving · 2 loading) with `setProgress` 0..255 for a save; `setResult`
is the last op's outcome (0 ok · 1 no card · 2 card full · 3 write failed
· 4 bad name · 5 not found · 6 busy · 7 bad file) and `setSeq` bumps when
one finishes, so the page reports each result once; `setName` names the
set the playing material came from (empty for a loose file or a take).

## The sample-mode commands (0x64–0x66)

```
0x64 GET_WAVE   ← u32 start, u16 n (≤480), u8 plane (0 L · 1 R · 2 L+R), [u8 step 1..128]
                → u8 status, u32 start, u16 n, u8 step, i8 x[n]
0x65 GET_TABLE  ← [u8 first, u8 count (≤39)]
                → u8 status, u8 nSlices, u8 nTable, then per entry (13 bytes)
                  u32 start, u32 len, i8 key (-1 grid), u8 cls, u8 sal, u8 hit, u8 dens
0x66 GET_HITS   ← [u8 first]  → u8 status, u8 nTotal, u8 first, u32 pos[…] (≤127)
```

`GET_WAVE` reads the material buffer directly on the control thread,
bounded by `freePos` (a take in flight is not exposed until it is
committed); with `step > 1` each output byte is the largest-magnitude
sample of `step` source samples, so one window spans `n · step` samples
(at most 61,440 — about a millisecond of SDRAM reading). `GET_TABLE` is the
engine's slice table: the grid `[0, nSlices)` with the swung boundaries
the SLICE package computed, then one entry per recorded key; `sal` and
`dens` are 0..255 (density = keyframes per sample × 5, saturating at the
adaptive fade's dense end); the entry count comes from the body length,
so a table over 39 entries is read in chunks. `GET_HITS` is the shell's
onset table for the current material (`gPendOnsets`, up to 128 positions,
in material samples), chunked the same way.

`flags`: b0 muted · b1 following an external clock · b2 locked · b3 flow
held · b4 Brain page. `mult` is the multiplier being *applied*; `multZone`
is the zone P2 is *requesting* (0..4 = ÷4 ÷2 ×1 ×2 ×4), which differs only
while a switch waits for its beat. `jitterUs` / `jitterRmsUs` are the peak
and RMS per-pulse period deviation over the last second. `clkFlags`: b0 P2
has engaged (moved since the clock arrived) · b1 multiplier switch pending
· b2 a lock alignment happened in the last ~2 s · b4–7 external-clock
transitions mod 16. `grammar` indexes the descriptor's Grammar labels —
Classic, Collins, Grids, Euclid, **Camp** (4: change ringing; Chop = the
number of bells, B2 rings the next change, Brain knobs unused).

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
| buttons, punch, scrub    | `0x63 ACTION`                     | 0 dice · 1 reslice · 2 mute · 3 punch · 4 flow · 5 scrub · 6 load · 7 clear locks · 8 page · 9 rec arm(key) · 10 rec clear(key) · 11 key plays(0/1) · 12 looper undo · 13 looper clear |
| kit waveform             | `0x64 GET_WAVE`                   | on demand, ≤ 3/s, cached windows            |
| slice table              | `0x65 GET_TABLE`                  | when the state's table signature changes    |
| onset ticks              | `0x66 GET_HITS`                   | when `matGen` changes                        |

Flashing, presets and settings stay in `/program`; this page is deliberately
the *instrument* view.
