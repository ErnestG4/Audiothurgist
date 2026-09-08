# EDIT-UI — the Kit tab becomes the editor (2026-09-08, branch `pkg4/edit-ui`)

*Package 4 of SLICE-EDIT ([slice-edit-design.md](../slice-edit-design.md)
Part B), on the protocol of [slice-edit-proto.md](slice-edit-proto.md) and the
semantics of [slice-edit-core.md](slice-edit-core.md). Web only: a new
`web/edit.js` (the editor with no DOM in it), the Kit tab of
`web/index.html`, two lines of `web/hostlink.js` (ACTION 30 / 31 helpers and
the Tap Allowance table), one option in `web/demo.js`, `web/sw.js`'s shell
list, and `web/selftest.mjs` sections 8 and 9. No firmware. Undo / Redo are
wired to ACTION 30 / 31 and stay disabled while the tail's `undoDepth` is 0
(package 6 makes them live); nothing persists (package 5).*

## What it is

With `"edit": 1` in the descriptor the Kit tab's waveform grows two more
bands and the tab gains an Edit panel. Without it the tab is exactly the
view-only one it was (`?demo=noedit` shows that). Every edit is one
gesture, one ACTION op, and the module's answer — the page never holds a
second copy of the table: it moves a handle locally at 60 fps, sends the op
when the finger lifts, refetches the table (the 14-byte form) on OK and
draws what came back; on a refusal it says why and the handle reverts.

### The waveform's bands (top to bottom)

| band | height | what it is |
|---|---|---|
| **wave** | the rest (≈ 200 px) | the material with every live entry's edge: grid boundaries white, recordings yellow, markers grey until touched and **orange** once the user placed or moved them (`flags` b0); tombstones are not drawn; a thin bar along the top spans each marker; blue onset ticks; a green playhead from `srcPos`; the selected entry's span tinted in its class colour; the dragged edge as a white line (blue when a magnet caught it) |
| **handles** | 44 px | a lollipop below every movable start — grid boundaries 1..n−1 and every live marker — and a **square** at the selected marker's end; the selection's handle is ringed and pulses in step with its key cells |
| **strip** | 46 px | the whole buffer `[0, freePos)` with the view's window drawn over it |

### The gesture map, as built

| where | gesture | what the page does | op |
|---|---|---|---|
| wave, inside a **marker** | tap | split at the point, snapped (onset ±12 ms; a zero crossing ±2 ms if *0-snap markers* is on); refused locally when a half would be under 10 ms — then it selects instead | SPLIT (with gen) |
| wave, inside a **grid slice** | tap | select it (the grid never splits) | — |
| wave, on any slice | drag | a ghost follows the finger to the key cells; drop on a cell assigns it (below) | SET_KEY / SWAP_KEYS |
| wave, on any slice | long-press (500 ms) | the menu: *audition · merge with N (only when a marker ends where this one starts) · promote → boundary i (the nearest) · copy as a marker · delete*; a grid slice offers *audition · copy out as a marker* | MERGE / PROMOTE / ADD_ENTRY / TOMBSTONE |
| handle | tap | select, and play the whole entry once | AUDITION(start, len) |
| handle (grid) | drag | the boundary moves, linked, clamped to 64 samples from both neighbours' far ends **before** anything is sent; snapped (onset, then zero crossing if *0-snap grid* — default on); auditioned from the new start every ≤ 100 ms; on release the whole slice plays and the op goes out | AUDITION(start, 250 ms)… then SET_BOUNDARY |
| handle (marker start) | drag | the start moves with the end fixed (≥ 480 samples, ≥ 0), same magnets (*0-snap markers* default off), same auditions | SET_ENTRY |
| handle (marker end, the square) | drag | the end moves with the start fixed (≤ freePos); the preview is the **last** 250 ms up to the new end | AUDITION(end, 250, from-end)… then SET_ENTRY |
| handle (marker end) | press and hold without moving | loops the tail until the finger lifts | AUDITION(end, 250, from-end \| loop) … RELEASE |
| handle (marker) | double-tap | merge with the marker ending at its start; with nothing adjacent the page says so and sends nothing | MERGE (with gen) |
| handle | long-press | the same menu as the slice's | as above |
| anywhere, while a handle is dragged | a second finger down, or Shift | the magnets are off until it lifts | — |
| strip | drag sideways | pan | — |
| strip | drag up / down | zoom around the finger (down = closer) | — |
| strip | pinch | zoom | — |
| strip | double-tap | fit the selected entry | — |
| wave or strip | wheel | zoom around the pointer (up = closer) | — |
| key cell | tap | select the key and its entry (the cell and the handle pulse together), play it once | AUDITION(start, len) |
| key cell | drag onto another cell | **swap** — straight away on an untaken key, else a *swap / replace / cancel* prompt; **red**, and swap as the default, when the target key is the only one playing a recording | SWAP_KEYS / SET_KEY |
| key cell | long-press, then drop on another cell or lift and tap one | **copy** the entry to that key (many keys may play one entry); tap anything else to cancel | SET_KEY |
| **Chop** pad | pointer down | a marker at the module's playhead minus the Tap Allowance (read from the live blob's `tap.allow`, 40 ms without it), snapped to an onset within ±60 ms; taps queue behind a pending reply, they are never refused | TAP_MARK(60, allowMs) |
| onset histogram | drag | the threshold line; the bars above it that survive the 35 ms gap light up and the button says how many | — |
| *Make markers* | click | one op: grey markers inside the break replaced by one per onset above the line | FROM_ONSETS(thresh, 35) |
| *nudge all* − / + | click | every grid boundary, or every marker, by N ms | NUDGE_ALL |
| *keys follow the grid* | click | after a confirm that says how many keys move and how many recorded keys lose their take's key (the take stays in the table), one SET_KEY per key that is off its span | SET_KEY × n |
| start / end fields | type, Enter | samples · ms · s · bar:beat:16th (`2:1:3`, from the native tempo); a grid slice's end is the next boundary | SET_BOUNDARY / SET_ENTRY |
| Undo / Redo | click | disabled while `undoDepth` is 0 | ACTION 30 / 31 |

What is deliberately **not** a gesture: nothing destructive (delete is a
tombstone, the audio stays), no modes — the copy arm after a long-press is
the one transient state, visible as a dashed cell and a status line, and any
tap that is not a target cancels it.

### The laws, and where they are kept

- **Never wait on the module in a pointer handler.** `ED.Editor.pointer()`
  updates local state and returns; the only awaits are in the commit
  pipeline that runs after the finger lifts.
- **One edit in flight.** `Editor.commit` holds `busy` from the send to the
  reply; a gesture op arriving meanwhile is turned away with a status line
  ("one edit at a time — … is still going"); a *panel* op (nudge, onsets,
  keys, undo, the Chop pad) **queues** behind the pending reply instead,
  because a button is not a finger on a position. The refetch after the
  reply is a poll, not part of the edit.
- **Auditions ≤ 10/s.** `ED.RateGate(100 ms)`: the first preview goes out,
  the rest are held and the last held one goes when the window ends — so a
  drag's final position always sounds. The release's whole-slice audition
  and a cell tap's go straight out (the firmware's own window holds them if
  it must).
- **Table refetch only on a `tableGen` change.** `syncKit`'s signature has
  carried it since package 2; a commit forces one refetch. The wave cache is
  cleared only when `[matGen, freePos, bufLen]` changes — an edit moves the
  table, not the audio.
- **Phone first.** 44 px handle row, 22 px slop either side, every cell and
  the pad ≥ 44 px, `touch-action: none` on the canvas and the cells (the
  page scrolls elsewhere), a second finger for Shift, no hover.
- **The gen.** Split, merge, delete, promote, copy, keys, nudge and onsets
  carry the page's `tableGen`; drags carry it too (SET_BOUNDARY / SET_ENTRY)
  — the design excused them, but a stale drag is exactly the case a refetch
  should catch; the Chop pad sends 0. On BAD_STATE the page refetches state
  and table, then says why: *a take is in flight (armed / taking / pending)*,
  *a set is saving / loading*, or **stale** — the gen moved — in which case
  the op is retried once with the fresh gen if its target still stands (a
  live marker, an adjacent merge partner, a grid index), and the status
  says "(after a refetch)". A refusal reverts the handle and adds nothing
  to the history.

### Colour as state

White grid · yellow recording · grey marker · **orange** placed or moved by
you (the flags byte, not a page guess) · blue onsets · green playhead ·
the selection's class colour as a tint · the selected handle's orange ring
and the selected cells' orange pulse, in step · a drop target dashed green ·
**red** when the drop would orphan a recording · a copy-armed cell dashed
orange · the audition-looping handle says so in the status pill.

### History

The page keeps its own label list keyed by the reply's `tableGen`
(`ED.History`, 64 deep): "boundary 5 −100 ms", "start 36 +4 ms", "split 35
→ 36", "merge 35 into 36", "delete 36", "marker 37 from 6", "F1 → 37",
"G1 ↔ D♯1", "nudge grid +5 ms", "onsets ≥ 82 % → 7", "tap → marker 38",
"promote 36 → boundary 9", "keys follow the grid". A `tableGen` the page
did not cause (a take, the panel, a re-slice, a set load) gets **"changed
on the module"**. The list is a viewer, not a source of truth: the depth
line reads the tail's `undoDepth` / `redoDepth`, Undo enables from the
first and Redo from the second, and (EDIT-UNDO) an undo strikes the newest
live entry through — it stays listed — a redo un-marks it, a new op drops
the struck ones, and a redo depth of 0 from the tail clears the marks.

## What is pure (edit.js) and what is DOM (index.html)

`web/edit.js` — plain script (`window.ED`) and CommonJS, no DOM:

| export | what |
|---|---|
| `K` | the constants: magnets 576 / 96 samples, `gridMin` 64, `recMin` 480, `handlePx` 44, `slopPx` 22, `tapPx` 10, `dblMs` 350, `holdMs` 500, `auditionMs` 100, `previewMs` 250, `tapSnapMs` 60, `minGapMs` 35, the Tap Allowance table |
| `spanSlice`, `isTomb` / `isLive` / `isMarker` / `isGrid` / `isOrange` / `isRecording`, `keysOn` | the table's vocabulary (a recording = a marker with a recorder) |
| `nearestOnset`, `zeroCross`, `snap` | the magnets; `zeroCross` walks outward from the point over `sampleAt(i)` and gives up on the first uncached sample |
| `clampBoundary`, `clampStart`, `clampEnd` | the clamp rules |
| `handleList`, `hitHandle`, `entryAt`, `mergeTarget`, `nearestBoundary` | handles and hit-testing (the selection wins a tie), the entry under a point (the smallest covering marker, else the grid slice), what MERGE would swallow and whether it is adjacent, the boundary for PROMOTE |
| `stepLen`, `fmtBbs`, `fmtMs`, `fmtDelta`, `fmtPos`, `parsePos` | positions: a 16th from `nativeBpm` (else `bufLen / nSlices`), 1-based `bar:beat:16th`, and the parser for samples / ms / s / bbs |
| `histLabel`, `History` | the labels and the viewer |
| `dropPlan` | the key-drop rule: `set` / `swap` / `choose` with `red` and the default |
| `onsetBars`, `onsetAccept` | the histogram's strengths and the FROM_ONSETS preview (the same rule as the firmware and the fake) |
| `RateGate` | the audition window |
| `Gesture` | the recogniser: press · hold · tap · dbltap · dragstart / drag / dragend · second / secondup · pinch · release · cancel, with the caller's tag |
| `Editor` | the controller: layout and zones, `tagAt`, `pointer()`, the gesture → action map, the drag with snap / clamp / audition, `commit` with the stale retry, every op behind the gestures and the panel, `dropOnKey`, `menuItems`, `fieldValues`, `typed`, `noteState` |

`web/index.html` supplies what needs a document: the canvas drawing
(`drawKitWave`, `drawHist`, `drawEditor`), the pointer listeners that
forward to `ed.pointer()`, the `sampleAt` over the wave cache (finest cached
window covering the sample, at most 8 samples per point, else null), the
refetch (`refetchKit`: a fresh state, then the table), the menu and the
swap / replace / cancel prompt (one popover), the drop target through
`elementFromPoint`, the ghost, the cells' gestures (a second `ED.Gesture`
tagged with the key index), the field row, the panel's buttons, and the
`?demo=noedit` fallback (`setEditorShown`).

## What was measured (`web/selftest.mjs`, **256 → 377**)

**Section 8 — the pure editor, then the Editor over the fake module** (a
page-shaped harness at 600 × 290 with an injected clock and timers): the
magnets (onset tie → earlier, the zero-crossing search and its give-up, snap
off), the clamps, `handleList` / `hitHandle` (slop, the selection's tie),
`entryAt`, `mergeTarget`, `nearestBoundary`, bar:beat:16th both ways and
every `parsePos` form, the labels, `History` (silent first gen, the
module's label, the cap), `dropPlan` in eight cases (unoccupied set, cell
swap, red replace-only, red default swap, not red with a second key, swap
through the key that plays a marker, an occupied grey key, the nothings),
`onsetBars` / `onsetAccept`, `RateGate`, the recogniser (tap, double tap,
a different tag, drag, hold with a still lift and a late move, second
finger and pinch, cancel). Then the Editor: the layout's zones; `tagAt`
over the handle row and the wave; a handle tap selects and auditions once;
a −15 px boundary drag — local before anything is sent, one preview in the
window with the last position held, release → the whole-slice audition,
SET_BOUNDARY, the refetch, orange, "boundary 5 −100 ms", the trailing timer
finds nothing; the onset magnet at +480 and raw with a second finger; a
cancelled drag commits nothing; copy out; the end handle's from-end preview
and SET_ENTRY; hold → loop → RELEASE; a tap in a marker → SPLIT with the
gen, in a grid slice → select only; double-tap → MERGE with the tombstone
left; merge refused with nothing adjacent; the long-press menus (marker and
grid); typed start / end / garbage; PROMOTE; SET_KEY; delete from the menu
with key 5 re-pointed and no handle; a **stale** gen → refetch → retried →
OK and said so; a take in flight → refused, named, the handle reverted, no
history line; the wave → key ghost and SET_KEY; cell 7 → key 3 red default
swap; cancel; replace; cell → empty cell swap; a copy to a third key; keys
follow the grid (six keys, one SET_KEY each); FROM_ONSETS with the
preview's count; nudge; the Chop pad's TAP_MARK(60, 40) with gen 0; Undo
on this firmware → BAD_ARGS, said; one gesture op at a time, a panel op
queued; "changed on the module" from a take; the strip's pan, vertical
zoom, pinch, double-tap fit, the wheel; and `noedit`: no handle row, the
wave pans, a tap selects and never splits.

**Section 9 — the page itself, headless.** `index.html`'s inline script
runs under a DOM stub in `vm` (elements with classList / style / dataset /
listeners / `querySelector` children, canvases with a 2-D context that
counts calls, `elementFromPoint` the test points where it likes, rAF and
timers that record exceptions; `getElementById` throws on an id the HTML
does not have) with `?demo=kit`, so it connects to the fake module through
the real Link and polls. Then: the editor is on (panel, field, 290 px
canvas, the 14-byte form was asked for); the Tap Allowance is read from the
blob; the canvases and cells drew with no runtime error; a **handle drag
from synthetic pointer events** is local until the lift, then
SET_BOUNDARY lands on the fake, the page's table follows, orange, the
history line appears, the wave cache survived; a cell tap selects, pulses
and auditions; cell 7 dragged onto cell 3 shows the ghost, lights the
target red, opens the red prompt with swap as the default, and the swap
button trades the keys; the Chop pad drops an orange marker at the kick;
the field shows the selection, switches units, and a typed end commits
SET_ENTRY; the histogram's pointer sets the threshold and *Make markers*
adds that many; nudge; Undo / Redo disabled with the depth line; keys
follow the grid after the confirm; no runtime errors or link warnings
through all of it; disconnect resets the editor. Then `?demo=noedit,kit`:
no editor, the 180 px canvas, the 13-byte table, a drag pans and nothing
is sent.

Also: `node --check` on the extracted inline script is what section 9's
load amounts to, plus running it.

## Not verifiable here — the browser and hardware checklist for Ernest

No Chromium in this environment, so nothing below has been *seen*:

1. **Touch on a phone** (Chrome for Android over WebUSB): the 44 px handle
   row is comfortable; a drag that starts on a handle never pans; the
   strip's drag / vertical drag / pinch / double-tap; that `touch-action:
   none` on the canvas and the cells does not trap page scrolling
   somewhere annoying (the tab still scrolls from the headings and the
   panel).
2. **Long-press vs the browser's own**: a 500 ms press on the canvas or a
   cell must not raise the context menu or text selection (the cells are
   `user-select: none`; the canvas has `preventDefault` on pointerdown) —
   check both desktop and Android.
3. **Pointer capture and `elementFromPoint`**: a drag from the canvas or a
   cell must find the key cell under the finger at the drop (the drop
   target is looked up at pointerup through `elementFromPoint`, with the
   ghost `pointer-events: none`).
4. **The menu and the prompt** open where the finger is (relative to
   `#wv_wrap`), stay inside the section, and close on an outside tap.
5. **Auditions on the real module**: the drag's previews at ≤ 10/s sound
   like the MPC's slice preview, the whole-slice play on release, the
   from-end preview on an end handle, the tail loop on a held end handle
   and its RELEASE on lift (no stuck loop after a pointercancel).
6. **The zero-crossing magnet**: only available once the view is zoomed to
   ≤ 8 samples per pixel (the cached windows); check the `snapped to zero`
   readout appears then and never at coarser zooms.
7. **Colour / pulse**: the selected handle and its key cells pulse in step;
   the orange flags come back from GET_TABLE after a drag; grey FROM_ONSETS
   markers are visibly different from orange ones.
8. **The stale path on hardware**: edit on the panel (a re-slice, a take)
   while the page holds an old gen, then a split on the page — expect
   "stale … retrying" and a clean result, or "the target is gone".
9. **Performance**: the pulse redraw at ~6 Hz while something is selected,
   plus the state poll, on an older phone — the kit canvas loop is 600 px
   × a few handles; if it stutters, the interval in `index.html` (160 ms)
   is the knob.
10. The Options tab now shows *Tap Allowance* from the descriptor; the
    Chop pad's readout follows it after a change.

## Left for later packages

- **5 EDIT-PERSIST**: nothing here persists; the Autosave option will
  show up in the Options tab by itself.
- **6 EDIT-UNDO**: *done — [slice-edit-undo.md](slice-edit-undo.md)*: the
  buttons drive the module's journal, the list marks the undone entries;
  the labels stay the page's.
- **7 EDIT-PANEL**: `selSlice` (tail byte 257) is parsed and unused; when
  the panel selects, `ed.select(st.selSlice)` is the one line.
- Open: the numeric field sits in a row under the handle row rather than
  floating under the handle itself (stable on a phone; a floating field
  would cover the strip); `SET_BOUNDARY` / `SET_ENTRY` still carry no
  `meta` after a landed drag (the proto doc's option) — the entry keeps its
  class until a re-analysis lands in a later package.
