# SLICE-EDIT — the Sample Drum experience: design

*2026-09-08. Part A (engineering) is from a read-only recon of the engine,
shell, sets and web app; Part B (interaction) from
`docs/sampler-recon-2026-09-08.md`. Nothing here is built yet; B9 lists the
decisions that gate the build.*

## What Ernest asked for

Slice in the UI, reposition slice start points, swap slices between keys —
"the Sample Drum experience" — after a recon of the best current samplers'
most loved and hated features, matching the peaks and avoiding the pitfalls.

## Part A — what exists, what is missing

### The data model (`core/bb_engine.h`)

A table entry is `Slice { start, len }` with parallel arrays for class,
salience, density, onset flag, band mask, cached tail-loop points and, for
recorded entries, the owning key. The grid `[0, nSlices)` is rebuilt by
`SetMaterial` as `i · step` plus the swung odd boundaries; recorded entries
`[nSlices, nTable)` each own one key and live past the material in the
buffer. `keySlice[24]` points a key at any table index; `SetKeySlice` exists
and has **no caller** today. `ClearKey` re-points a key but never removes an
entry — the table only shrinks at `SetMaterial`.

The set JSON already writes every entry's `start / len / key / cls / sal /
hit`, but on load the grid starts are used only as onsets and the
boundaries are **recomputed**; keys pointing at grid slices are written as
−1 and lost. So today an edit would not survive a save.

### Thread safety

Both trigger paths copy the entry by value (`FireSlice`, `TriggerStep`) and
`Voice::Trigger` caches start / end / bound — **a sounding voice is
unaffected by a later change to its entry**. What goes stale after a
boundary move: `loopA/loopB` (cached by `FindLoop`), `stepHit[t]`,
`sliceDens[t]`. A two-int `Slice` written from the control thread can be
torn by the audio thread's copy: one wrong-but-bounded trigger.

**Rule for the package:** the control thread never writes `slices[]`. Edits
are posted as a request record (op, t, start, len, key, meta) in the same
mailbox discipline the shell already uses (`gPendReslice`, `gCallReq`,
`gPendUndo`…), and the audio thread applies them at block start, resetting
`loopState`, recomputing `stepHit`, updating `recMeta` in the same step.
Analysis of a new region (`AnalyseRegion`) is read-only and runs on the
control thread before the post. `TrimRegion` is not safe on live material.

### Two traps

1. **Re-applies wipe grid edits.** Bars / Swing / Onsets / Sampling /
   Classifier changes and a set load all call `SetMaterial`, which rebuilds
   the grid. Edited boundaries must be an overlay the engine re-applies when
   `nSlices` and the planes are unchanged (the way recordings are kept), or
   the doc must say an option change re-slices. Design choice: **overlay**.
2. **Deleting by compaction** re-indexes `keySlice`, `tableKey`, looper
   events, `stepSrc` punches, pending calls and the shell's last index.
   Design choice: **tombstone** (`len = 0`, key −1; callers already skip
   `len < 64`) and compact only at `SetMaterial`.

### Protocol (ACTION 0x63 ops 16.., capability `"edit":1`, tail byte 255 `tableGen`)

| op | body | rule |
|---|---|---|
| SET_BOUNDARY | `u8 i, u32 pos` | moves the shared boundary between grid slices i−1 and i; both neighbours keep ≥ 64 samples; clamps to `[0, bufLen)` — contiguity is preserved by construction |
| SET_ENTRY | `u8 t, u32 start, u32 len` | recorded / free entries only; inside `[0, freePos)`, `len ≥ kRecMin` |
| ADD_ENTRY | `u8 key, u32 start, u32 len` | a free entry pointing anywhere in `[0, freePos)` — **a slice of the break as a kit key** (needs `SetMaterial`'s keep rule and `PlanSet`'s `start ≥ material` check relaxed) |
| TOMBSTONE | `u8 t` | the delete; keys re-point to their span slice |
| SET_KEY | `u8 key, u8 t` | one owner per recorded entry is the current invariant — decide transfer vs many-to-one |
| SWAP_KEYS | `u8 a, u8 b` | looper events with a key resolve through `KeySlice` at fire time, so the loop follows |
| AUDITION / RELEASE | `u8 t, u8 flags[, u8 key]` / `u8 key` | through `gCallReq` with `punch = false` (a punch would be recorded by the looper); `RELEASE` → `gRelKey` |
| UNDO_EDIT | — | inverse-op journal in SDRAM, ~24 B per level, depth 64 |

No grid split / merge: `nSlices` is what the grammars, the clock map, the
looper divisions and `gridBars` key off. Split/merge on free entries is
ADD + SET / SET + TOMBSTONE.

The page refetches the table only when `[matGen, nTable, freePos, bufLen,
keySlice]` changes — `tableGen` joins that signature. Worth appending too:
`u32 srcPos` (the youngest voice's material position) for tap-while-playing.

### Audition while editing

"Move the start while it loops": either re-trigger the audition on every
landed edit (a 5 ms choke fade, audible) or an **audition voice flag** the
audio thread services every block by copying the entry's current start /
end into that voice's loop points, so the loop follows the drag through the
click bridge with no re-trigger. Design choice: the audition voice.

### Boundary ≠ trigger time

Trigger times come from the bar phase and swing, not from `slices[k].start`.
Moving a boundary moves what the step **plays**, not when. ReCycle-style
"boundary is the trigger time" would touch `ProcessT`, ratchets and the
looper snap — v1 is content-only, and the page says so.

### Persistence

- Grid boundaries: apply the file's grid starts after `SetMaterial` on load
  (old files carry exactly the computed boundaries → no-op, no schema bump).
- Key map: top-level `"keys":[24]` (table index or −1 = default); old
  firmware skips it.
- Material-internal entries: relax `PlanSet` and the keep rule.
- `dens` per entry optional; undo history not persisted (as the looper's).

### Build plan (packages ≤ 1 day, each with tests in `tests/run.sh` / `web/selftest.mjs`)

1. **EDIT-CORE** — `EditReq` + `ApplyEdit()` on the audio thread; ops
   SET_BOUNDARY / SET_ENTRY / ADD / TOMBSTONE / SET_KEY / SWAP_KEYS; refresh
   `stepHit`, `loopState`, `recMeta`; the boundary overlay through
   `SetMaterial`; relaxed keep rule. Tests: a moved boundary plays the new
   region on the next trigger while the sounding voice finishes unchanged;
   contiguity and `len ≥ 64` refusals; tombstones skipped; swap with a
   looper event resolving through the key; edits survive a same-plane
   `SetMaterial`; Classic hash unchanged.
2. **EDIT-PROTO** — ACTION ops, `tableGen` / `srcPos` tail, `"edit":1`,
   `hostlink.js`, `demo.js` with the firmware's index semantics (its
   `clearKey` currently splices — fix). Selftest: every op's OK / BAD_ARGS /
   BAD_STATE, `tableGen` bumps, refetch shows the new bounds.
3. **EDIT-AUDITION** — AUDITION / RELEASE, the following audition voice.
   Test: a following voice tracks a moved end with no discontinuity beyond
   the click bridge.
4. **EDIT-UI** — Part B.
5. **EDIT-PERSIST** — `keys[]`, boundary re-apply, relaxed `PlanSet`,
   round-trip tests, old-JSON tolerance.
6. **EDIT-UNDO** — journal in SDRAM, the op, depth in the tail.

## Part B — the interaction model

Distilled from `docs/sampler-recon-2026-09-08.md` (35 samplers, ~114
sources) and reconciled with Part A. Where the recon and the engine
disagree, the resolution is stated.

### B0. Two layers in one table

The recon's model (split / merge / tap-to-chop with the grid rippling
right) collides with Part A: `nSlices` is the grammars', the clock map's
and the looper's world and cannot change per tap. Resolution — **two
layers, one table, one set of primitives**:

- **The grid** `[0, nSlices)`: the break layer. Sixteen steps per bar, as
  many bars as *Bars* says. Boundaries **move** (linked: slice i's start is
  slice i−1's end, so the bar keeps tiling) but the count is fixed.
  Everything the grammars do keeps working untouched.
- **Markers** `[nSlices, nTable)`: the Sample Drum layer. Free regions over
  *any* part of the buffer — the break included — placed by tap, by onset
  threshold, by drag, or by recording. This is what keys point at. Split
  and merge live here. Up to `kMaxTable − nSlices` of them (32 with four
  bars, 80 with one), for 24 keys.
- A marker can be **promoted** onto the grid ("snap the grid boundary
  here" = SET_BOUNDARY to the marker's start) and a grid slice can be
  **copied out** as a marker (ADD_ENTRY of its span). That is the bridge
  between the two layers and keeps both honest.

### B1. Principles (each traceable to a peak or a pitfall in the recon)

1. **The device owns the table and the history.** Every edit is a small
   idempotent op carrying the page's `tableGen`; a stale gen is refused
   (BAD_STATE), the page refetches and retries. Undo is the firmware's
   inverse-op journal (Part A §7, SDRAM, depth 64), so **panel undo works
   with no page attached** — the recon's page-side snapshot stack becomes
   a *history list* the page renders from the journal's labels, not a
   second source of truth. No `SET_TABLE` bulk replay needed.
2. **Nothing is destructive.** Edits touch the table only; the buffer is
   never rewritten. Delete is a tombstone; the region stays.
3. **Never lose an edit.** *Autosave* (a new Option, default Off because
   it is a card write with a poll gap): after 2 s idle the table and key
   map are written to the current set's JSON, or to `<file>.slices.json`
   beside a loose WAV, and read back on load. "Save set" stays explicit.
4. **Edits are live and audible.** A grid edit plays on the next call of
   that slice; a voice in flight keeps its old bounds (already true).
   Every drag step auditions from the new start (B3).
5. **One surface, two halves.** Kit waveform top half = markers (tap =
   split at the finger, drag the lollipop = move, double-tap a marker =
   merge with the previous, long-press = the slice menu); bottom strip =
   pan / zoom (a drag that starts on a handle never zooms; double-tap fits
   the selected slice). Fat handles below the waveform on a phone.
6. **Colour is state.** Grid white, recordings yellow (already); user-
   placed / user-moved markers **orange** and immune to a re-slice; auto
   markers grey until touched; onset ticks blue; the selected marker and
   its key cell pulse together; a drop that would orphan a recording is
   **red**.
7. **Snap is a modifier, not a mode.** Drag snaps to the nearest onset
   within ±12 ms, then to a zero crossing within ±2 ms when 0-snap is on
   (default on for the grid, off for recordings); a second finger or Shift
   disables both; a numeric field (samples · ms · bar:beat:16th) under the
   handle for typed entry.

### B2. Getting markers (four ways, one primitive set)

1. **Grid**: as today (B3 / Bars). Grid boundary edits survive a regrid
   through Part A's overlay when the plane and count are unchanged.
2. **Onset threshold**: the page draws the onset table as a strength
   histogram; dragging the threshold previews which ticks become markers;
   commit = one `FROM_ONSETS(thresh, minGapMs)` op that adds markers for
   every onset above it (min gap 35 ms, the detector's own). Existing
   orange markers are kept, grey ones replaced.
3. **Tap while playing** (lazy chop): a big Chop pad; each tap sends
   `TAP_MARK(snapMs)`. **The firmware timestamps**: the audio thread's
   material-position snapshot at receipt, minus a reaction allowance
   (40 ms, an Option), snapped to the nearest onset within ±60 ms — else
   raw. A new marker inherits its onset's class.
4. **Tap / drag on the waveform**: tap empty space in a marker = `SPLIT`
   at the (snapped) point; double-tap a handle = `MERGE` with the previous
   marker; drag a handle = `SET_ENTRY`.

### B3. Moving a start or an end, with audition

- The page moves the handle locally at 60 fps and never waits for the
  module; ms and samples shown; fine mode as the finger slows.
- **Audition on the move**: at most ten times a second the page sends
  `AUDITION(start, lenMs ≤ 250, flags)`; the firmware plays a **scratch
  voice** from `start` with the normal attack fade and the 5 ms choke on
  retrigger (the MPC's Slice Preview), tagged so the continuity branch
  ignores it. One such voice; it steals itself. On release the page plays
  the whole slice once and commits `SET_ENTRY` / `SET_BOUNDARY`.
- Ends: the audition plays the *last* 250 ms up to the new end; press-
  and-hold an end handle without moving = loop the tail (v2, the
  following-loop voice of Part A §4).
- **Linked** for the grid (moving slice i's start sets i−1's end); free
  for markers, overlaps allowed.
- **Nudge all ±N ms** for the "every marker is late" case.

### B4. Keys: assign, swap, copy, reset

- Drag a marker onto a key cell, or a cell onto a cell: `SET_KEY` /
  `SWAP_KEYS`. Drop on an occupied key → *swap / replace / cancel*; a drop
  that would orphan a **recording** is red and defaults to swap.
- Tap a cell = one-shot mono audition of that key and select its marker.
- Long-press a cell → copy to another key: **many keys may point at one
  entry** (`keySlice` is an index; the styles give reverse / half /
  double per hit). Resolution of Part A's open point: `tableKey` stays
  "the key that recorded it" for red-warning purposes only; ownership
  does not gate assignment.
- "Keys follow the grid" restores the span map.

### B5. Undo

Firmware journal (Part A §7): `UNDO` pops one inverse op; depth in the
state tail; the page lists the journal's labels ("split 7", "start 3
−4 ms") and offers Undo / Redo (redo = the journal's forward half). The
looper's undo (ACTION 12) stays a separate stack.

### B6. The panel alone (no page) — one hot gesture set, no growth

An **Edit page** (B1 hold + B3 tap; B1 tap leaves): P1 selects a slice
(the bar ring pulses it as it plays — you hear the selection without a
screen), P2 = start nudge ±50 ms, P3 = end nudge ±50 ms, both pickup with
the offset shown on the ring, P4 = pitch as now; **every pot move
retriggers the slice's first 250 ms** so the panel hears the start move.
B2 tap = undo, B2 hold = revert the slice to the grid. B3 tap while the
break plays in Edit = drop a marker at the playhead, onset-snapped (lazy
chop); B3 hold = reslice as today with orange markers kept. A J4/J5 note
in Edit selects that key's marker. Nothing else on the panel: no swap, no
threshold — the Squid lesson.

### B7. Protocol (supersedes Part A's table where they differ)

One new command **`0x67 SLICE`**: `u8 op, u8 gen, args…`; reply `u8
status` (0 OK · 2 BAD_ARGS · 3 BAD_STATE for a stale gen or a running
take) `, u8 tableGen`. Capability `"edit":1`. State tail gains
`u8 tableGen` (255), `u8 undoDepth` (256), `u8 selSlice` (257, the panel's
selection so the page follows the knobs), `u32 srcPos` (258..261, the
youngest voice's material position, for the Chop pad's display).

| op | args | layer |
|---|---|---|
| SET_BOUNDARY | `u8 i, u32 pos` | grid (linked) |
| SET_ENTRY | `u8 t, u32 start, u32 len` | marker |
| ADD_ENTRY | `u8 key(255 = none), u32 start, u32 len, u8 flags(b0 orange)` | marker; `key` assigns at once |
| SPLIT / MERGE | `u8 t, u32 pos` / `u8 t` | marker |
| TOMBSTONE | `u8 t` | marker |
| SET_KEY / SWAP_KEYS | `u8 key, u8 t` / `u8 a, u8 b` | keys |
| FROM_ONSETS | `u8 thresh, u16 minGapMs` | marker (replaces grey) |
| TAP_MARK | `u8 snapMs` | marker (firmware-timestamped) |
| AUDITION / RELEASE | `u32 start, u16 lenMs, u8 flags` / — | scratch voice |
| NUDGE_ALL | `i16 ms, u8 which(grid/markers)` | both |
| PROMOTE | `u8 t, u8 i` | marker → grid boundary i |
| UNDO / REDO | — | journal |

`GET_TABLE` entries gain a `flags` byte (b0 orange · b1 tombstone). Sets'
JSON gains `"keys":[24]` and per-entry `"flags"`. The WAV cue list stays
every start.

### B8. Build plan (revises Part A's)

1. **EDIT-CORE** — as Part A, plus SPLIT / MERGE / FROM_ONSETS on
   markers, the relaxed keep rule, the boundary overlay, `AnalyseRegion`
   per new marker (control thread, chunked with `Yield`).
2. **EDIT-PROTO** — `0x67`, the tail bytes, `"edit":1`, `hostlink.js`,
   `demo.js` (fix its `clearKey`), selftest per op incl. stale-gen refusal.
3. **EDIT-AUDITION** — the scratch voice, `TAP_MARK` timestamping from the
   audio thread's position snapshot, engine tests.
4. **EDIT-UI** — the Kit waveform's two halves, handles, snap magnets,
   the onset histogram, the Chop pad, cell drag / swap / copy, the
   history list, colours; pure helpers (snap, nearest handle) in
   `hostlink.js` for the selftest.
5. **EDIT-PERSIST** — `keys[]`, `flags`, boundary re-apply, the Autosave
   option, loose-WAV sidecar, round-trip tests, old-JSON tolerance.
6. **EDIT-UNDO** — the journal, UNDO / REDO, depth in the tail.
7. **EDIT-PANEL** — the Edit page, pickup rings, panel undo, lazy chop on
   B3. Ships last; the page is complete without it.

### B9. Decisions for Ernest before code

1. The two-layer model (grid fixed-count and linked; markers free) —
   yes / no.
2. Undo in the firmware journal with the page as a viewer (vs the
   recon's page-owned snapshots).
3. Autosave as an Option, default Off (card write, poll gap) — or On.
4. Audition by retrigger on the move (MPC) for v1; the following loop
   voice for v2.
5. Many keys per entry allowed (copy), red only when a recording would be
   orphaned.
6. The panel Edit page gesture: B1 hold + B3 tap — check it against the
   current button map before it is taken.
7. `0x67 SLICE` with gen checking rather than more ACTION ops.

**Decided 2026-09-08 (Ernest):** 1 yes; 2 whatever serves the UX without
costing stability or performance → the firmware journal; 3 Autosave
**default On**; 4 yes; 5 yes, if the UX can be nailed; 6 to be tested
against the button map; 7 held — resolution: **ACTION ops with an optional
trailing `u8 gen` (0 = unchecked)**, sent for split / merge / delete, not
for drags; a dedicated command only if the op space gets crowded.

**Decision 6 checked against the button map (2026-09-08):** B1 is the SDK
Pager's *Cycle* binding (page flip on release) and, through the 300 ms
`HoldButton` proxy, ParamLock's record button; B2 tap = dice / looper undo
in Play, hold = flow; B3 tap = reslice / record gestures, hold = load;
B2+B3 2 s = Settings. **B1 hold + B3 tap is free** and workable: the shell
calls `pager.ConsumeButton()` when B3 falls while B1 is held, which poisons
B1's pending release so no page flip follows (`framework/src/surface/
pager.cpp:88-113`); ParamLock only arms on a pot move, so a still B1 hold
does not start a lock. Inside the Edit page B1 tap leaves, and the Pager's
own page state is untouched (Edit is a shell mode over the current page).
