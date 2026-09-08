# EDIT-PROTO + EDIT-AUDITION — slice editing over the wire, the audition voice, the tap mark (2026-09-08, branch `pkg4/edit-proto`)

*Packages 2 and 3 of SLICE-EDIT ([slice-edit-design.md](../slice-edit-design.md)
B7 / B8, revised by B9's resolution: **ACTION ops with an optional trailing
`u8 gen`**, not a new command). Builds on
[slice-edit-core.md](slice-edit-core.md)'s `EditReq` / `ApplyEdit` /
`AnalyseEntry`. Owns the `/* EDIT */` fences in `src/audiothurgist.cpp`
(the mailbox, `HandleEdit`, the tail, GET_TABLE's flags form, Options byte
22), `Engine::CallRegion` / `ReleaseRegion` / `SrcPos` and `Voice::scratch`
in `core/bb_engine.h`, `bb::TapMarkAt` in `core/bb_edit.h`, the EDIT parts
of `web/hostlink.js` / `web/demo.js` / `web/selftest.mjs`, one line of
`web/index.html`, and the EDIT-AUDITION sections of
`tests/engine_check.cpp`. No UI yet (package 4), nothing persists (5), no
undo (6), no panel gesture (7).*

## The protocol, as built

Capability: `"edit":1` in the descriptor's `audiothurgist` object. Every
op is `0x63 ACTION` with the op byte, its fields little-endian, then an
**optional trailing `u8 gen`** — the `tableGen` the page last saw (state
byte 255), 0 = unchecked. A body shorter than the fields is BAD_ARGS; a
longer one is read for its gen. The reply is `u8 status`, then **on
success** `u8 tableGen` (the generation after the op) and, where the table
says so, a third byte.

| op | body | third reply byte | rule (the engine's, [slice-edit-core.md](slice-edit-core.md)) |
|---|---|---|---|
| 16 SET_BOUNDARY | `u8 i, u32 pos` | — | grid, linked; both neighbours keep ≥ 64; slice i orange |
| 17 SET_ENTRY | `u8 t, u32 start, u32 len` | — | marker, live; `[start, start + len) ⊂ [0, freePos)`, `len ≥ 480` |
| 18 ADD_ENTRY | `u8 key (255 = none), u32 start, u32 len, u8 flags (b0 orange)` | `u8 newIdx` | anywhere in `[0, freePos)`; the first tombstone's slot, else the end; the key assigned at once; analysed on the control thread first |
| 19 SPLIT | `u8 t, u32 pos` | `u8 newIdx` | marker; both halves ≥ 480; keys stay on t; the new half analysed |
| 20 MERGE | `u8 t` | — | t swallows the live marker ending at its start, else the previous live one; the other is a tombstone |
| 21 TOMBSTONE | `u8 t` | — | marker; `len 0`; keys on it → their span slice; the index stays valid |
| 22 SET_KEY | `u8 key, u8 t` | — | any live entry |
| 23 SWAP_KEYS | `u8 a, u8 b` | — | `a ≠ b`; the recorded owners trade too |
| 24 NUDGE_ALL | `i16 ms, u8 which (0 grid · 1 markers)` | — | ms × 48 in the shell; grid clamped and all orange; markers clamped to `[0, freePos)` |
| 25 PROMOTE | `u8 t, u8 i` | — | SET_BOUNDARY(i, marker t's start) |
| 26 FROM_ONSETS | `u8 thresh, u16 minGapMs` | `u8 count` | below |
| 27 AUDITION | `u32 start, u16 lenMs, u8 flags (b0 from-end · b1 loop)` | — (no `tableGen` either) | the scratch voice, below; never gated by the table |
| 28 RELEASE | — | — | ends the audition with the 5 ms fade; drops a held request |
| 29 TAP_MARK | `u8 snapMs, u8 allowMs` | `u8 newIdx` | below; an orange marker |

Status: `EditStatus` → HostLink — `Ok` 0; `BadArgs` / `NotMarker` /
`NotGrid` / `Tomb` / `BadOp` → **2 BAD_ARGS**; `Stale` / `NoMaterial` /
`Full` → **3 BAD_STATE**. BAD_STATE also, before anything is posted, while
**a take is in flight** (the tail's `recState ≠ 0`: a key take, a loop take,
Record = Arm, the engine armed or pending — the replacement take's commit is
the one control-thread write of a live entry left in the engine, so edits
and takes exclude each other), while **a set op** runs or waits, while a
**material swap** is pending (`gLoadReq` / `gApply` / `gReapply`), and while
the mailbox still holds a request. A refusal never bumps `tableGen`.

**FROM_ONSETS** is one op and several applies, with `bb::Yield()` between
them. The onset table (`gPendOnsets`) carries positions only, so an onset's
strength is the **salience of the grid slice it falls in** (`gSal`, ×255).
First every *grey* marker inside the break goes (live, not orange, no
recorded owner, start < `bufLen` — recordings, orange markers and anything
past the break stay); then the onsets with strength ≥ `thresh`, at least
`minGapMs` apart, become grey markers, each spanning to the next accepted
onset (the last to the break's end; a span under 480 is skipped), each
analysed by `AnalyseEntry`. The gen is checked by the first apply only.
`Full` after at least one add is still OK with the count so far.

**State tail** (append-only after `setName`, body now 262 bytes):

| byte | field | meaning |
|---|---|---|
| 255 | `u8 tableGen` | `engine.tableGen` — never 0; bumps on every table change (an edit, a take, a clear, a re-slice, a set load) |
| 256 | `u8 undoDepth` | 0 until EDIT-UNDO |
| 257 | `u8 selSlice` | 255 (none) until EDIT-PANEL |
| 258 | `u32 srcPos` | `engine.SrcPos()`, snapshotted by the audio thread after every `Process`: the material position of the youngest sounding voice that is not an audition, else the start of what the current step plays |

**GET_TABLE** takes a third request byte, `u8 form`: 1 appends `u8 flags`
(b0 orange · b1 tombstone) to every entry — 14 bytes, 39 per chunk (549 of
the 1 024-byte body). Without it, or on a firmware without `"edit"`, the
13-byte form comes back, so an old page keeps working against a new module
and a new page against an old one (it asks for form 1 only when the
capability is there and parses by the size it asked for). This is the one
deviation from the brief's "detect by entry size from the capability": the
request-side opt-in costs a byte and keeps both directions compatible.

**Options byte 22 = Tap Allowance**: `0 · 20 · 40 · 60 · 80 ms`, default
index 2 (40 ms). `kBytes` 22 → 23, schema `OPT6 → OPT7` — every other byte
keeps its meaning; presets saved under OPT6 re-init (and restore from their
JSON backups, byte 22 taking the live default). `Describe` field 22
`tap.allow`; the manual's options line and README's table carry it. The
page sends its own value in TAP_MARK's `allowMs`; the option is for the
panel's lazy chop (package 7).

## The mailbox

`gEditReq` (one `bb::EditReq`), `gEditPend`, `gEditStatus`, `gEditIdx`.
The control thread builds the request, runs `bb::AnalyseEntry` for
ADD_ENTRY, SPLIT (over the new right half), TAP_MARK and every FROM_ONSETS
add (yielding as it goes), writes the slot, `dmb`, raises `gEditPend`, and
waits for the audio thread to clear it — **by the clock** (`TickNow()`, ≤
4 ms = eight blocks), calling `bb::Yield()` (KeepClock) every turn, never a
bare spin. The audio thread, at block start after the call mailbox and
before `Process`, copies the slot, calls `engine.ApplyEdit(req, &idx)`,
stores status and index, `dmb`, clears the flag. A timeout answers
BAD_STATE. `SetBoundary` / `SetEntry` / `Merge` / `Promote` post no meta
(the entry keeps its class / salience / density — the core doc's "first
guess that drifts"; the page may post one when a drag lands, package 4).

`bb::Yield()` inside the frame handler is safe: KeepClock ticks the NCO
and re-publishes the snapshot, and re-enters `loop.Tick()` only during a
set op — which is exactly when edits are refused.

## The audition voice (EDIT-AUDITION)

`Engine::CallRegion(start, len, rateMul, flags)` — AUDIO. One `Voice` with
explicit bounds: `Trigger` on `{start, len}` with `bound = end = start +
len` and `ext 1`, then its **natural end pulled 5 ms (chokeLen) short** so
the release ramp runs inside the region (a grid one-shot with `ext 1` ends
on the click bridge, not a fade); the fixed 1 ms attack; rate = `rateMul`
(the shell passes 1: no tempo varispeed — an audition is about the
boundaries). Clamped into `[0, freePos)`, refused under 64 samples or with
`rateMul ≤ 0`. `kRegionFromEnd` (b0): `start` is the region's **end** —
play `[start − len, start)`. `kRegionLoop` (b1): `tail = 1`, `loopA =
start`, `loopB = end − 2`, so it repeats until `ReleaseRegion` (the click
bridge covers the seam). The voice is tagged `scratch`: the continuity
branch never adopts it, `WouldChoke()` is false so no hit chokes it, it is
never recorded (only `CallSlice` reaches the looper), it raises no gate,
counts no hit and no `trigFlash`. **One audition at a time**: a new call
fades every scratch voice with the normal choke geometry (still tagged, so
nothing adopts it as it fades) and takes a free voice, else the oldest
non-scratch one. `ReleaseRegion()` fades every scratch voice from where it
is. `Voice::Trigger` clears the tag, so a stolen scratch voice is ordinary.

Shell: ACTION 27 → `gAudStart / gAudLen / gAudFlags / gAudReq`, ACTION 28
→ `gAudRel`; the audio thread services the release before the call, after
the edit slot. **Rate limit, 10/s**: a request inside the 100 ms window
since the last post is *held* (replacing any held one) and goes out from
`OnPoll` when the window ends — so a drag's last position always sounds
and nothing queues. BAD_ARGS for `start > freePos`, `lenMs` under 64
samples, unknown flag bits; BAD_STATE only with no material.

## TAP_MARK

`bb::TapMarkAt(srcPos, allow, snap, onsets, n, limit, maxLen = 12000,
minLen = 480)` in `core/bb_edit.h`, pure: `p = max(0, srcPos − allow)`;
the nearest onset within ±`snap` (the earlier wins a tie) replaces it; the
length runs to the next onset after `p`, capped at `maxLen`, never under
`minLen`; a span past `limit` is shortened to it, and only when that leaves
under `minLen` is the start pulled back. The shell calls it with the audio
thread's `gSrcPos` at receipt, `allowMs × 48`, `snapMs × 48`,
`gPendOnsets`, `freePos`, 250 ms; then ADD_ENTRY with `kOrange` and
`AnalyseEntry`'s meta; the reply's third byte is the new index.

## Web

`hostlink.js`: `ACT` 16..29; `EDIT` = `{ orange, tomb, fromEnd, loop, grid,
markers, noKey, maxNudgeMs, maxAuditionMs }`; `parseState` reads 255..257
(`tableGen`, `undoDepth`, `selSlice` −1 for 255) at ≥ 258 and `srcPos` at
≥ 262; `TABLE_ENTRY_EDIT = 14`, `parseTableChunk(b, entrySize)`,
`fetchTable(link, { edit: true })` asks for form 1; `editOp(link, op,
fields, gen)` and one helper per op — `editBoundary`, `editEntry`,
`editAdd`, `editSplit`, `editMerge`, `editTombstone`, `editSetKey`,
`editSwapKeys`, `editNudgeAll(ms, which)`, `editPromote`,
`editFromOnsets`, `audition(start, lenMs, flags)`, `release`,
`tapMark(snapMs, allowMs, gen)` — every one resolving `{ status, ok,
tableGen, newIdx, count }` on any device answer and rejecting only on a
link failure. `index.html`: `tableGen` joins `syncKit`'s refetch signature
(the only page change; the page still fetches the 13-byte form until
package 4 asks for the flags).

`demo.js`: the fake table has `flags` and a `tableGen`; `clearKey` now
does what `Engine::ClearKey` does — the recording **stays**, keyless and
at its index, the key falls back to its span slice (it used to splice and
re-index, which no firmware ever did); `recordKey` replaces a key's own
recording, else takes a tombstone's slot, else appends; every op of
`core/bb_edit.h` is ported (`edSetBoundary` … `edNudge`, `edFromOnsets`,
`tapMarkAt`) with the gen check, BAD_STATE during its fake take or set op,
slot reuse, the 14-byte form on request, the 255..261 tail, `srcPos` from
its clock, the audition with the 100 ms window (`auditionHeld`,
`auditionPosted`). `edit: false` (`?demo=noedit`) plays an older firmware:
no capability, the 13-byte table whatever is asked, the body capped at 255,
ACTION 16.. BAD_ARGS.

## What was measured

`tests/engine_check.cpp` (EDIT-AUDITION, 43 checks, **475 → 518**): the
scratch voice's bounds; it plays the region's tone with the 1 ms attack,
the 5 ms fade closing exactly at `start + len`, silence after, no click,
no trigger flash / hit / gate; a retrigger leaves two tagged voices for 5
ms then one; from-end plays the last 2 400 samples of slice 4; a loop still
plays after three lengths and `ReleaseRegion` ends it within 10 ms without
a click; refusals (63 samples, past `freePos`, rate 0) and the clamps
(`freePos`, `[0, end)`); **the verbatim run re-splices as often with an
audition mid-step as without, and (with) − (without) equals the audition
alone sample for sample** (max |Δ| < 1e-5); the looper records nothing
from `CallRegion` and one event from a written call; `SrcPos()` follows
the call's voice and ignores the audition. `TapMarkAt`: raw, allowance +
snap, the cap, the tie, the `kRecMin` floor, snap 0, the clamp at 0, the
limit (shortened; pulled back; under `kRecMin`).

`web/selftest.mjs` (EDIT, **186 → 256**): the 255 / 257 / 258 / 262 / 300
bodies, the 14- and 13-byte chunks, 39 × 14 in 1 024; over the fake kit:
the capability, the tail, both table forms; SET_BOUNDARY OK (reply gen =
state gen, refetch shows the linked move and the flags), a stale gen
BAD_STATE with nothing done, gen 0 unchecked, four BAD_ARGS with the gen
untouched, a short body; ADD (index, key, flags, class), a keyless grey
one, BAD_ARGS; SPLIT / MERGE with the tombstone left behind; five ops on a
tombstone refused with no bump; the slot reused; SET_ENTRY and its
refusals; TOMBSTONE with two keys re-pointed; SET_KEY onto the grid;
SWAP_KEYS following the recording's owner; NUDGE_ALL both ways with the
clamps and the tombstone skipped; PROMOTE; FROM_ONSETS at 210 (eight
kicks, to the next kick, the grey one replaced, orange and recordings
kept), at 100 (36), with a 200 ms gap (16), a stale gen; the audition's
rate window (held, replaced, posted on the next poll), RELEASE, BAD_ARGS;
TAP_MARK snapped / raw / capped; BAD_STATE during a take (the audition
still plays) and a set op; the table filled to 96 then Full → BAD_STATE
with no bump, a tombstone freeing the slot; and `noedit`. The two
pre-existing "rec clear" checks now assert the index-stable rule.

`tests/run.sh` ALL GREEN, the goldens and the Classic hash as before;
`make`: text 376 088 → 383 708 (+7 620 B — the shell instantiates every
`ApplyEdit` body for the first time, plus `HandleEdit`, `CallRegion`,
`SrcPos`), data 4 496 → 4 552, bss 39 608 344 → 39 608 384.

## Left for packages 4–7

- **4 EDIT-UI**: ask for the 14-byte form (`fetchTable(link, { edit:
  desc.audiothurgist.edit === 1 })`), draw the flags, drive the ops; send
  the gen for split / merge / delete; post `meta` after a landed drag
  (SET_BOUNDARY / SET_ENTRY carry none today); the Chop pad from `srcPos`;
  the onset histogram from `GET_HITS` with the slice-salience strength rule
  above (or a strength table if the detector is asked for one).
- **5 EDIT-PERSIST**: nothing here persists; `SetGridBoundaries` /
  `SetKeyTable` are unused; a set saved after an edit still fails
  `PlanSet` (core doc, item 2).
- **6 EDIT-UNDO**: tail byte 256 is 0; ops 30/31 (UNDO / REDO) are free.
- **7 EDIT-PANEL**: tail byte 257 is 255; the Tap Allowance option is read
  by nobody in the firmware yet (the page sends its own `allowMs`).
- Open: SET_BOUNDARY leaves the neighbours' class / salience alone (core
  doc, item 1); `FROM_ONSETS` strength is a slice-level proxy; the
  audition plays at the file's rate, not the tempo's varispeed.
