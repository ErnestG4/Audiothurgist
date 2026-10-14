# Audiothurgist — web frontend

**v0.13.0-beta: playing it live.** A transport on every tab (▶ / ■, BPM,
the clock, a CPU meter, Dice / Flow / Reslice — Undo / Clear in Play), the
grammars as big mode chips on the Play tab, 24 on-screen pads, a Jacks card
with the live J4 → key, the Kit's markers → keys / clear markers / autoslice
count, a light theme beside the dark one, and a layout that works on a
phone. Details under *What is on the page*; the protocol it needs under
*The v0.13 keys ops*.

`index.html` is the instrument's face in a browser: the live bar, per-slice
waveforms you can select and punch into steps, an output scope, a **Controls
panel that always says what B1/B2/B3 and the six knobs do right now**, a
**ten-channel Scope tab** (out L/R, in L/R and the six jacks J3–J8 as
DC-coupled CV traces, zoom, trigger, ≤ 30 frames/s), the module's Options,
and its manual. It speaks **HostLink over Web Serial** to
the module's front USB-C — the same protocol Hermetic's `/program` uses —
plus the firmware's `StateExt` (commands 0x60–0x66, v0.7.0+; the v0.8.0
state tail for locks, native BPM and bars; the CLOCK package's jitter,
multiplier-zone and clock-flags tail; the sample-mode tail — record state,
buffer bookkeeping, key table — and the `GET_WAVE` / `GET_TABLE` /
`GET_HITS` reads behind the **Kit** tab when the firmware sends them; the
SCOPE package's ten-channel `0x62` body behind the **Scope** tab). No
dependencies, four static files (plus the vendored Android polyfill), phone-friendly.

| file | what |
|---|---|
| `index.html` | the page (UI, polling, rendering) |
| `hostlink.js` | HostLink framing, the priority link, StateExt parsers, descriptor helpers — shared with the self-test |
| `edit.js` | the Kit tab's slice editor without its DOM: snap magnets, handles and hit-testing, clamps, the gesture recogniser, the key-drop rule, the history labels, and the `Editor` that turns gestures into ACTION ops — shared with the self-test |
| `demo.js` | a fake module behind a fake serial port, for the Demo button and the self-test |
| `selftest.mjs` | node: golden-vector check + synthetic frames + an end-to-end run over the fake module + the page itself under a DOM stub + the descriptor-parity and contrast checks |
| `sw.js`, `manifest.webmanifest` | the installable app (network-first service worker) |
| `publish-pages.sh` | rebuilds the `pages` branch from `web/`, stamping the commit into the footer and the script URLs (`hostlink.js?v=<commit>`) |

## Running it

The page needs a **secure context** (`https://`, or `http://localhost`;
`file://` will not do) and a Chromium browser. On the desktop it speaks
**Web Serial** (Chrome, Edge, Opera). On **Chrome for Android** — which has
WebUSB but no Web Serial — it loads Google's
[web-serial-polyfill](vendor/web-serial-polyfill/) and talks to the
module's CDC interface over WebUSB; a USB OTG cable from the phone or
tablet to the module's front USB-C is all it takes, and the module pill
says `· WebUSB`. iOS and iPadOS browsers have neither API. Either:

- **Locally:** `cd web && python3 -m http.server 8080` → http://localhost:8080
- **Hosted:** `./web/publish-pages.sh` rebuilds the `pages` branch from
  `web/`; `git push -f origin pages` publishes it on Codeberg Pages →
  https://combust.codeberg.page/Audiothurgist/ (https, so the phone works
  too). Chrome's menu offers **Add to Home screen** / **Install app** —
  the manifest and the small network-first service worker (`sw.js`) make
  it a proper installable app with the ring icon.
- **A phone against a page served from the PC** (before Pages, or for
  development): a plain `http://<pc-ip>:8080` is not a secure context and
  the phone hides its USB APIs. Two ways round it: `adb reverse tcp:8080
  tcp:8080` makes the phone's own `localhost:8080` reach the PC's server
  (localhost is always secure), or in Chrome on the phone set
  `chrome://flags/#unsafely-treat-insecure-origin-as-secure` to
  `http://<pc-ip>:8080`.

Close the `/program` tab first — one page holds the serial port at a time.
Press **Connect**, pick the Alchemy Lab (0483:5740), and the bar starts moving.
If the page cannot reach a USB API it says exactly why (not secure, iOS,
no WebUSB) under the header instead of failing silently.

**Play Store, later:** with the page on https and the manifest + service
worker in place, [Bubblewrap](https://github.com/GoogleChromeLabs/bubblewrap)
wraps it as a Trusted Web Activity (`bubblewrap init --manifest
https://combust.codeberg.page/Audiothurgist/manifest.webmanifest`) — it is
still Chrome underneath, so the WebUSB path is what runs. One catch: the
full-screen (no URL bar) verification needs
`/.well-known/assetlinks.json` at the **origin root**, which on Codeberg
Pages means a repo named `pages` (served at `combust.codeberg.page/`), not
the `pages` branch of this repo (served under `/Audiothurgist/`). Without
it the TWA works but keeps Chrome's bar.

**No module to hand?** Press **Demo** (or open `index.html?demo`) — the page
drives itself from `demo.js`, a fake module that answers real HostLink
frames. `?demo=brain,ext,grids,locks` starts it on the Brain page, locked to
a clock at ×2, in Grids, with two motion locks; `?demo=ext,pending` leaves a
×4 switch waiting for its beat; `?demo=camp` starts in Camp; **`?demo=kit`**
opens the Kit tab — the slice editor — on a swung 32-slice break with three keys already
recorded (arm a key and the fake take runs armed → taking → pending →
committed by itself; the looper count drifts up; every edit op, the audition
voice and the tap mark answer as the firmware would); `?demo=old` sends a
pre-v0.8.0 state body, `?demo=short` the 167-byte CLOCK body (no flags)
and `?demo=v099` the 176-byte body (no sample-mode tail), `?demo=nosets`
the 219-byte body (no sets tail), `?demo=noedit` a firmware without
slice editing (no `"edit"` capability, the 255-byte body, the 13-byte
table) and `?demo=noundo` one without the undo journal (the 262-byte body)
so you can see the fallbacks; **`?demo=play`** starts in Play (an empty
loop: what the pads play is written into it), **`?demo=pads`** in Pads with
six autoslice markers already on the keys, **`?demo=old12`** plays a 0.12
firmware (no `"keys"`: no pads, ▶ / ■ is the mute toggle, the Chop pad
unguarded); **`?demo=scope`** opens the Scope tab (`cv` / `gates`
press that preset, `noin` leaves J1/J2 silent, `body512` announces the
protocol's default `max_body` so a frame takes two requests). `#kit` / `#scope` / `#opts` / `#man` open a tab
directly.

## What is on the page

- **The status line** (one line, ends in an ellipsis; tap it for all of
  it): module + firmware, current page, clock state, the **link**
  round-trip (last / worst in the last second · requests per second · KB/s
  in · queue depth), the jack map, and the firmware's own diag (worst poll
  gap and audio callback, playhead jumps, regenerations). Under it one
  message line: what just happened, gone after 8 s unless it is a fault.
  **Connect** says *Connecting…* and cannot be pressed twice; pulling the
  cable (the read loop ending) disconnects the page and clears every
  readout; a drawing error lands on the message line instead of freezing
  the canvases.
- **The transport** (every tab; sticky under the tabs, at the bottom of a
  phone): **▶ / ■** is the mute — the pattern stops, the clock runs on, the
  keys still play (B1 held 600 ms on the panel); `SET_MUTE` (35) on a v0.13
  module, the toggle (2) before. BPM (steadied: a short average, shown when
  it moves 0.1, a 2 BPM step taken at once), playing / stopped, the mode and
  the clock. The **CPU** meter: the audio callback's mean (bar) and peak
  (tick) over the last second against the 24-sample block, orange past
  70 % or on an overrun (the v0.13 tail; older firmware: the diag's worst
  callback as a tick). **Dice / Flow / Reslice** follow the grammar: in
  Play they are the looper's **Undo** (12) and **Clear** (13, asked first);
  Pads has no dice and no flow; Camp's Dice rings the next change. Flow is
  held — on while the button is down.
- **Mode** (top of the Play tab): one chip per grammar label in the
  descriptor — Classic · Grids · Loop (= Play) · Pads big, Collins · Euclid
  · Camp under *more* — written through the Options blob path; the line
  under them says what the mode means: the jack map it brings (with Jacks
  = Follow grammar, Play and Pads take the Voice map) and what B2 / B3 do.
- **Pads** (`"keys"`): the 24 keys, two octaves from C1, each saying what it
  plays (slice / marker / recording); `PLAY_KEY` (32) down on press, up on
  release, one pointer per pad, so several fingers play at once. A pad
  lights whenever its key is called from anywhere (tail `keyFlash` /
  `keySeq`). First on the tab in Play and Pads; *keys follow markers* shows
  when Pads has put the markers on the keys.
- **Jacks**: the applied map and why (the option, the grammar), J4 live as
  a key with its volts (0 V = key 0 = C1, a semitone per key, below 0 V =
  key 0) on a 24-key strip, J5's style in the Voice map, what J6–J8 do.
- **The slice view** in the pattern grammars is the grid's slices with
  their envelopes (click one, then a step on the strip, to punch it — ACTION
  3); in **Play / Pads** it is the kit instead, one column per key, lit as
  the key fires — no grid ghosts — and a click plays that key. `callFlash`
  is a slice: the steps that play it light, not step `callFlash`.
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
  = page, B1 held + pot = motion lock, hold + nudge again clears, B1 held
  alone = start / stop; B2 tap dice — in Play the looper's undo — / hold
  flow; B3 tap reslice — in Play clear the loop — / hold load; B2+B3 2 s =
  Settings), with Switch page (ACTION 8), Clear locks (7) and Load selected
  (6) here — the transport has the rest. Below it the six knobs laid out as on the panel
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
  keyframe density (from `GET_TABLE`); a key on a **marker** — a span of the
  break placed by hand — says `marker 36`. Each cell has **●** (arm a take into
  that key, ACTION 9 — the next gate on the panel's record path starts it)
  and **✕** (clear the recording, ACTION 10); the armed / taking / pending
  key pulses from the state's record byte, with the **input meter** beside
  it. A **buffer** bar
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
- **The editor** (EDIT-UI, firmware with `"edit": 1` — without it the Kit
  tab is the view-only one above, `?demo=noedit`): the waveform below the
  keys is the **Sample Drum surface**. Its top band is the material from
  `GET_WAVE` windows with every edge of the table drawn over it — grid
  boundaries white, recordings yellow, markers grey until touched and
  **orange** once you placed or moved them (the table's flags byte), blue
  onset ticks, a green playhead from `srcPos`. Under it a 44 px row of
  **lollipop handles**, one per grid boundary and marker start, and a square
  at the selected marker's end; under that a **strip** with the whole buffer
  and the view's window (drag = pan, drag up / down or pinch or wheel = zoom,
  double-tap = fit the selection). Tap inside a marker to **split** it there
  (snapped to an onset within 12 ms; a grid slice only selects — the grid
  never changes count); drag a handle to **move** a grid boundary (linked,
  64 samples from its neighbours, clamped before sending) or a marker's
  start or end — it **plays from the new point as you drag** (≤ 10 auditions
  a second) and the whole slice once you let go; a second finger or Shift
  turns the magnets off, and *0-snap grid / markers* add a zero-crossing
  magnet (±2 ms) from the cached windows; double-tap a marker's handle to
  **merge** it with the one ending where it starts; long-press a handle or a
  slice for **delete** (a tombstone — the audio stays), **copy as a marker**,
  **promote** (the nearest grid boundary snaps to the marker) or an
  audition; hold a marker's end handle still to **loop its tail**. The row
  under the handles shows the selection's start and end in samples · ms ·
  bar:beat:16th, typed values commit. **Keys:** tap a cell to hear it and
  select it (cell and handle pulse together); drag a cell onto another to
  **swap** (a *swap / replace / cancel* prompt when the target is taken —
  **red**, swap by default, when a recording would lose its only key);
  long-press a cell, then drop it on or tap another, to **copy** the entry
  there (many keys may play one entry); drag a slice up from the waveform
  onto a cell to assign it. The **Edit** panel has the big **Chop** pad
  (tap while the break plays: the module timestamps from its playhead,
  backs off Options → *Tap Allowance*, snaps to an onset), the **onset
  histogram** (bars = each onset's strength, drag the line, *Make markers*
  = one `FROM_ONSETS`), **nudge all** ± N ms for the grid or the markers,
  **keys follow the grid**, and the **history** — the page's labels for
  what it did, keyed by the table's generation, with Undo / Redo driving
  the module's journal (ACTION 30 / 31, 64 steps; the undone entries are
  struck through, the depths come from the tail). Every op
  carries the page's `tableGen`; a stale one is refetched and retried once,
  a take or a set op in flight is named, and a refusal reverts the handle.
  Details and the gesture table: `docs/packages/slice-edit-ui.md`.
- **Scope tab** (SCOPE package, firmware with `scope2`): ten channels on
  one sample clock from the firmware's rings — out L, out R, in L (J1), in R
  (J2) at 48 kHz, and the six jacks J3–J8 DC-coupled at ±5 V, one sample per
  audio block (2 kHz): LFOs, envelopes, sequences and gates, in or out (an
  output jack reads back what it drives). One snapshot per frame, so every
  trace starts at the same instant; each tier keeps its own window length,
  drawn over the shared time axis with a ms/div readout and, once a jack
  shows, a 1 V per division grid over ±5 V. Channel buttons (J1/J2 show
  themselves while they carry a signal; each jack is labelled with its role
  under the current jack map — `J6 · kick gate out` — and its present
  voltage), **Audio time** ×1…×16 (11 ms … 171 ms), **CV time** ×1…×32
  (256 ms … 8 s), **Gain** ×1…×8 with a clip pill, **Trigger** on any
  channel (auto = the first shown; a Schmitt level trigger, 0 for audio and
  1 V for a jack, that leaves room for every shown tier — the marker at the
  left edge in the trigger channel's colour), **Peak** decimation (each
  point the largest sample of its group, so hats and 6 ms gates survive a
  long window), **Afterglow**, **Freeze**, and the **Audio · CV · Gates**
  presets (the CV preset turns on the jacks in input mode under the current
  map; Gates shows J6/J7/J8 over out L, triggered on J6). Pills: achieved
  frames/s · requests per frame · KB/s · last round trip, and `n = 252,
  audio ×16 → 84 ms, cv ×2 → 252 ms · triggered on J6`. A cv window longer
  than the audio ring (1.4 s) detaches the audio tier to its newest window
  and says so. Old firmware without `scope2` gets its mono trace here.
- **Options tab:** every enum field of the module's Options block, labels
  and names straight from the descriptor, grouped Pattern / Sound / Looper
  (Quantize) / Jacks & CV / Recording (Sampling, Record Source, Record) /
  Editing (Tap Allowance, Autosave) / Analysis — all 24 bytes have a home;
  a field a newer firmware adds lands in "Other". A change reads the live
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
- **Cadence:** state at 10 Hz; the Play tab's scope at 5 Hz and only while
  its canvas is on screen; **the Scope tab** asks for the next frame the
  moment the last one lands, floored at 33 ms (≤ 30 frames/s), and eases
  the state poll to 4 Hz meanwhile — one 1022-byte request per frame for
  the four audio channels on the firmware's `max_body` of 1024, two
  requests pinned to one ring position when the channels do not fit (all
  ten, the six jacks, or a 512-byte module); envelope only when `matGen`
  changes; the slice table when the
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

- **The Kit's marker ops** (`"keys"`, in the Edit panel): the **autoslice
  count** (0–64; dragging sends each value as it goes, every send after
  the drag's first with JOIN, so a drag is one undo step) and **Re-roll**
  (a new draw at the same count), **Markers → keys** (33: time order onto
  keys 0, 1, 2…, keys holding a recording skipped) and **Clear markers**
  (34, asked first) — one undo step each. The **Chop pad** is disabled with
  the reason while nothing sounds (tail 263 b0 — the module would refuse
  the tap); a tap on a marker that is already there selects it. The
  waveform keeps the grid's boundaries on a lane at the top and the markers
  on a lane at the bottom.
- **Themes:** every colour is a token on `:root` (the canvases read them
  through `getComputedStyle`); a light set follows `prefers-color-scheme`,
  and the ◐ button cycles auto → light → dark (remembered per browser).
  The displays (canvases) stay dark in both, like the module's LEDs behind
  glass. Every text colour is ≥ 4.5:1 on every surface it sits on, measured
  by the selftest.
- **Phone:** one status line instead of wrapping pills, a scrolling tab
  row, every target ≥ 44 px (the Kit's per-key ● / ✕ give way to the big
  pair under the keys), the transport under the thumb, no sideways scroll
  at 320 px.

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
parses the SCOPE request / reply and plans frames at 512 / 1024 / 2048,
runs the real `Link` over the fake module: priority ordering, coalescing,
status errors, envelope fetch, the clock flags on a fake clock, a `GET_LIVE
→ BLOB` option round-trip, the seven-label Grammar (Camp 4, Play 5, Pads 6),
and — on a 64-slice kit with three recordings — the chunked table (39 +
28), the onset table, raw and decimated waveform windows with the clamp at
`freePos`, and ACTION 9–13: arm → taking → pending → committed, a second
arm refused, clear re-pointing the keys, Key Plays landing in the Options
byte, looper undo / clear — and the scope: the empty body unchanged, a
four-channel frame, trigger, peak, at-pos re-reads, the two-request plan,
every BAD_ARGS and clamp of both body forms, the jacks (a cv-only frame,
a mixed frame aligned, the gates preset triggered on J6, an 8 s LFO
detaching the audio, all ten channels pinned) — and every slice-editing op
over the fake module with its gen, tombstones, keys, FROM_ONSETS, the
audition window, TAP_MARK and an older firmware. Then **the editor**: its
pure helpers (magnets, clamps, handles, positions, labels, the drop rule,
the histogram preview, the audition window, the gesture recogniser) and the
`Editor` driven against the fake with synthetic pointer events (a drag
auditions ≤ 10/s and commits SET_BOUNDARY / SET_ENTRY on release; split,
merge, delete, promote, copy, keys, nudge, onsets, the Chop pad; a stale gen
refetched and retried; a take in flight refused and named); and **the page
itself, headless** — `index.html`'s inline script under a DOM stub in `vm`,
connected to the fake through `?demo=kit`, driven through its canvases,
cells, menu and panel, then `?demo=noedit`'s view-only fallback. Then
**v0.13**: the transports (Link over a SerialTransport, bytesIn / kbps, the
unplug as onClose, a WebSocket transport); the KEYS tail and ACTION 32–36
over the fake (the transport, pads, nested autoslice draws, re-roll, JOIN,
in-place re-spans, markers → keys, clear markers, Pads' keys following,
Play's loop, a 0.12 firmware's BAD_ARGS); the **descriptor parity** — it
parses `src/audiothurgist.cpp` (the Options block's ids, names, offsets,
zones and enum labels, its size, hash and live defaults, the jacks, the
pager's knobs, the root capabilities) and fails when `demo.js` drifts (and
proves the check bites); **the revamped page** under the stub, every
gesture performed through its listeners (▶ / ■, Flow held, the mode chips,
three pads at once, the slider drag, re-roll, markers → keys, clear, the
Chop pad twice, Play's Undo / Clear, the theme, a draw error, the unplug,
then `?demo=old12`), with the stub's unknown-id trap and draw logs (what
the last frame drew); the **contrast** of every text token on every
surface in both themes, no colour outside the tokens, no duplicate ids.
549 checks. Pass `--golden <path>` if the SDK lives elsewhere (it is found
above a worktree too) and `--fw-src <file>` to check the parity against
another firmware source (`git show beta-fw:src/audiothurgist.cpp > /tmp/f.cpp`).

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
u8 tableGen u8 undoDepth u8 selSlice                              (255..257, EDIT)
u32 srcPos                                                        (258..261, EDIT)
u8 redoDepth                                                      (262, EDIT-UNDO)
u8 flags2 u16 loadAvg u16 loadMax i16 j4mV u8 j4Key u8 j5Style
u8 keyFlash u8 keySeq u8 autoK u8 nMarkers                        (263..275, KEYS v0.13)
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
`tableGen` (EDIT, capability `"edit": 1`) is the slice table's generation
— never 0, bumped by every edit, take, clear, re-slice and set load; the
page carries it back as an op's trailing `gen` and joins it to the Kit
tab's refetch signature. `undoDepth` / `redoDepth` (EDIT-UNDO) are the
module's journal depths in steps — Undo / Redo enable from them, and a
262-byte body (no `redoDepth`) is a firmware without the journal;
`selSlice` is 255 (none) until EDIT-PANEL. `srcPos` is the material position of the youngest
sounding voice (else the current step's source), snapshotted by the audio
thread once per block — the Chop pad's display, and what a TAP_MARK is
timestamped from.

## The sample-mode commands (0x64–0x66)

```
0x64 GET_WAVE   ← u32 start, u16 n (≤480), u8 plane (0 L · 1 R · 2 L+R), [u8 step 1..128]
                → u8 status, u32 start, u16 n, u8 step, i8 x[n]
0x65 GET_TABLE  ← [u8 first, u8 count (≤39) [, u8 form]]
                → u8 status, u8 nSlices, u8 nTable, then per entry (13 bytes)
                  u32 start, u32 len, i8 key (-1 grid / keyless), u8 cls, u8 sal, u8 hit, u8 dens
                  [, u8 flags (b0 orange · b1 tombstone) — 14 bytes, when form = 1 and "edit" is present]
0x66 GET_HITS   ← [u8 first]  → u8 status, u8 nTotal, u8 first, u32 pos[…] (≤127)
```

## The slice-editing ops (ACTION 16–29, capability `"edit": 1`)

Every body is the op byte, its fields little-endian, then an optional
trailing `u8 gen` — the `tableGen` the page last saw, 0 = unchecked; a
stale gen is BAD_STATE (refetch, retry). The reply is `u8 status`, then on
success `u8 tableGen` and, for ADD / SPLIT / TAP_MARK, `u8 newIdx` (FROM_
ONSETS: the count). BAD_STATE also while a take is in flight, a set op runs
or waits, a material swap is pending, the table is full or nothing is
loaded; BAD_ARGS for anything the engine refuses (an index, a span, a key,
a neighbour under 64 samples, a tombstone). `hostlink.js` has one helper
per op (`editBoundary(link, i, pos, gen)` …) resolving
`{ status, ok, tableGen, newIdx, count }`. Full contract:
`docs/packages/slice-edit-proto.md`.

```
16 SET_BOUNDARY  u8 i, u32 pos                 grid boundary i (linked; both neighbours keep ≥ 64)
17 SET_ENTRY     u8 t, u32 start, u32 len      a marker's span, anywhere in [0, freePos), len ≥ 480
18 ADD_ENTRY     u8 key (255 none), u32 start, u32 len, u8 flags (b0 orange)   → newIdx
19 SPLIT         u8 t, u32 pos                 → newIdx (the right half; keys stay on t)
20 MERGE         u8 t                          t swallows the marker ending at its start (else the previous)
21 TOMBSTONE     u8 t                          delete: len 0, keys → their span slice, the index stays
22 SET_KEY       u8 key, u8 t                  any live entry, grid or marker
23 SWAP_KEYS     u8 a, u8 b
24 NUDGE_ALL     i16 ms, u8 which (0 grid · 1 markers)
25 PROMOTE       u8 t, u8 i                    SET_BOUNDARY(i, marker t's start)
26 FROM_ONSETS   u8 thresh, u16 minGapMs       grey markers inside the break replaced; one per onset ≥ thresh   → count
27 AUDITION      u32 start, u16 lenMs, u8 flags (b0 from-end · b1 loop)   the scratch voice, ≤ 10/s
28 RELEASE       —                             ends the audition (5 ms fade)
29 TAP_MARK      u8 snapMs, u8 allowMs         an orange marker at srcPos − allow, onset-snapped   → newIdx
30 UNDO_EDIT     —                             one journal step back (64 deep; a take / load / new material ends the history)
31 REDO_EDIT     —                             one step forward (a new edit clears the redo half)
```

## The v0.13 keys ops (ACTION 29, 32–36, capability `"keys": 1`, `"edit": 2`)

```
29 TAP_MARK      as above, but BAD_STATE while nothing sounds (tail 263 b0 clear); within 10 ms of a live marker
                 it selects that marker instead: reply u8 status, u8 tableGen, u8 idx, u8 flags (b0 reused)
32 PLAY_KEY      u8 key (0..23), u8 down (1) / up (0)            the key as a CV note would play it (plain); up releases a held key
33 KEYS_FROM_MARKERS [u8 gen]  → count                           markers in time order onto keys 0..; recording keys kept; the
                                                                 keys past the last marker back to their span; one undo step
34 CLEAR_MARKERS [u8 gen]      → count                           every live marker (grey and orange) deleted, never a take; one step
35 SET_MUTE      u8 1 stopped / 0 running                        the transport (2 is still the toggle)
36 AUTOSLICE     u8 k (0..64), u8 flags (b0 re-roll · b1 JOIN) [, u8 gen]  → count
                 grey markers re-spanned / added / removed to the first k of a seeded draw weighted by salience
                 (nested in k); orange markers stay, onsets within 10 ms of one are skipped; JOIN folds the apply
                 into the previous AUTOSLICE's undo step (a slider drag after its first send)
```

`flags2` (263): b0 `srcValid` (a voice sounds — a TAP_MARK will be taken) ·
b1 keys follow markers (Pads with markers; in Pads every marker op carries
markers → keys in its own undo step, and entering Pads applies it once).
`loadAvg` / `loadMax`: the audio callback in ‰ of the 24-sample budget
(240 000 cycles), mean and peak over the last second. `j4mV` is J4 now in
millivolts, `j4Key` the key it would play; `j5Style` the style J5 reads in
the Voice map (255 in the Brain map); `keyFlash` the last key called by any
source (255 none), `keySeq` bumps per call; `autoK` the autoslice count last
applied; `nMarkers` the live markers. `hostlink.js` has `playKey`,
`keysFromMarkers`, `clearMarkers`, `setMute`, `autoslice` and `caps(desc)`.

## The scope (0x62)

```
0x62 SCOPE      ← (empty)  → u8 status, u16 256, i8 x[256]: (out L + out R) / 2, one in four,
                             the newest 21 ms — the pre-SCOPE reply, unchanged
                ← u8 chanMask (b0 out L · b1 out R · b2 in L · b3 in R), u8 decim (1..16),
                  u16 n, u8 flags (b0 trigger · b1 peak · b2 at · b3 ext), [u32 pos (with at or
                  ext)], [ext: u8 maskHi (b0..b5 = J3..J8), u8 cvDecim (1..64), i8 trigLevel,
                  u8 trigSrc (0 first selected · 0x80 | 0..9)]
                → u8 status, u8 chanMask, u8 decim, u16 n (as clamped: ≤ 512, ≤ (max_body − header)
                  / channels, n·decim ≤ 65 280, n·cvDecim ≤ 16 368), u8 flags (b0 triggered · b1 pos
                  honoured · b2 ext header · b3 a tier detached), u32 pos (the window start on the
                  audio sample clock, wrapping at 24·2^26; cv start = pos / 24), [ext: u8 maskHi,
                  u8 cvDecim, u8 cvDiv (24), u8 trigSrc (0..9, 255 none)], then n × i8 per audio
                  channel in mask order, then per jack J3..J8
```

Two ring tiers on one sample clock: four audio rings of 64 K samples at 48
kHz and six jack rings of 16 K at 2 kHz (one per audio block, DC-coupled,
±5 V = ±127, read from ADC1's DMA buffer and converted with the board's
calibration); every channel of a reply comes from one snapshot, so a frame
is one instant. `trigger` starts the window on the most recent rising
crossing of `trigLevel` on the trigger channel that leaves every selected
tier's span after it (Schmitt, ±4 of 127); `peak` makes each point the
largest-magnitude sample of its group; `at` + `pos` starts the window at a
previous reply's `pos` (a second request of the same frame, for when
`max_body` cannot hold every channel); a tier that cannot hold the start
(a cv window older than the audio ring) shows its newest window and the
reply says `detached`. BAD_ARGS for no channel, mask bits past the
channels, decim 0 / > 16, cvDecim 0 / > 64, n 0, a bad trigSrc. The
descriptor's `audiothurgist` object says `"scope2":2` (1 = the audio body
alone). Details: `docs/packages/scope.md`.

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
| scope (Play tab)         | `0x62 SCOPE` (empty body)         | 5 Hz while visible; 256 samples ×4 decimated |
| scope (Scope tab)        | `0x62 SCOPE` (body, `scope2`)     | ≤ 30 frames/s; audio 4 × 253 or out L + 3 jacks × 252 per request at max_body 1024; trigger, peak, at-pos, the jacks' own decimation |
| buttons, punch, scrub    | `0x63 ACTION`                     | 0 dice · 1 reslice · 2 mute · 3 punch · 4 flow · 5 scrub · 6 load · 7 clear locks · 8 page · 9 rec arm(key) · 10 rec clear(key) · 11 key plays(0/1) · 12 looper undo · 13 looper clear · 14 save set · 15 load set |
| slice editing            | `0x63 ACTION` 16–29 (`"edit"`)    | the table ops with a gen, the audition voice, the tap mark — the table above; the Kit tab's editor drives them (`edit.js`) |
| undo / redo              | `0x63 ACTION` 30 / 31             | the module's journal (EDIT-UNDO); Undo enables from the tail's `undoDepth`, Redo from `redoDepth` |
| transport, pads, markers | `0x63 ACTION` 32–36 (`"keys"`)    | SET_MUTE · PLAY_KEY per pointer · markers → keys · clear markers · autoslice (JOIN while dragging) |
| kit waveform             | `0x64 GET_WAVE`                   | on demand, ≤ 3/s, cached windows            |
| slice table              | `0x65 GET_TABLE`                  | when the state's table signature changes    |
| onset ticks              | `0x66 GET_HITS`                   | when `matGen` changes                        |

Flashing, presets and settings stay in `/program`; this page is deliberately
the *instrument* view.
