# EDIT-PANEL — the module's own Edit page (2026-09-08, branch `pkg4/edit-panel`)

*Package 7 of SLICE-EDIT ([slice-edit-design.md](../slice-edit-design.md)
B6, the button-map check under B9's decision 6). Owns the `/* EDIT-PANEL */`
fences in `src/audiothurgist.cpp` (the state block before `OnFrame`, the
gesture block in `OnPoll`, the palette in `OnRender`, `ServicePanel` after
the EDIT helpers, tail byte 257), the new `core/bb_panel_edit.h` and
`tests/panel_check.cpp`, one line of the Voice keymap, three read-gates on
P1..P3, three lines of `web/index.html`. Built on
[slice-edit-core.md](slice-edit-core.md) (the ops),
[slice-edit-proto.md](slice-edit-proto.md) (the mailbox, `PostEdit`, the
audition voice, `bb::TapMarkAt`, the Tap Allowance option — now read by
the firmware) and the undo journal of package 6 (two marked hook lines until it
merges). Nothing here could be run on hardware; the checklist at the end
is what Ernest does.*

## The gesture map, as built

Edit is a **shell mode over the current Pager page**: the Pager's page
does not move, presets and the descriptor know nothing of it, and the
page underneath keeps playing exactly what it played.

| gesture | outside Edit | in Edit |
|---|---|---|
| **B1 held + B3 tap** | **enter Edit** (B1 goes amber) | — |
| B1 tap | Perform ↔ Brain | **leave and commit** (a held nudge posts, the audition ends, P1..P3 go back) |
| B2 tap | dice (Play: looper undo) | **undo** the last edit (package 6's op; red until it merges) |
| B2 hold 400 ms | flow | **revert** the selection: a grid slice's two boundaries back to the computed grid (only those that moved); a marker is deleted (tombstone; its keys go back to their span slices) |
| B3 tap | reslice / record gestures | **lazy chop**: a marker at the playhead — the audio thread's position at the *press* minus Options → Tap Allowance, snapped to an onset within ±60 ms, to the next onset (≤ 250 ms), orange, analysed, selected. Muted: refused |
| B3 hold 400 ms | load the highlighted file | **reslice** (`engine.Reslice()`: a new cell tree — it never touches the table, so every orange boundary and marker stays) |
| B2 + B3 2 s | Settings | Settings (unchanged; its exit taps are latched away as before) |
| P1 | Chop | **select**: the live entries in table order — grid, then markers, tombstones skipped — spread over the travel; the selection changes once P1 has moved 0.006 |
| J4/J5 note (Voice map) | plays the key | plays the key **and selects its entry** |
| P2 | Tempo | **start nudge** ±50 ms, pickup at centre |
| P3 | History | **end nudge** ±50 ms, pickup at centre |
| P4 | Pitch | Pitch |
| P5 / P6 | Sample / Volume | unchanged |

**Why B1 hold + B3 tap is safe** (the design's decision-6 check, now
code): the shell calls `pager.ConsumeButton()` the moment B3 falls with
B1 down, which poisons B1's pending release in the Pager — no page flip
follows. Inside Edit every poll with B1 down calls it again, so B1's
release is always ours (a claim with nothing held evaporates at the end
of the frame, so it never leaks into a later press). The release of the
B1 that *entered* does not leave; the next B1 tap does. ParamLock sees
B1 only through the 300 ms `HoldButton` proxy and arms on a pot move, so
a still B1 starts no lock — but B1 held ≥ 300 ms *while turning a pot* in
Edit still records a lock, as everywhere (don't stack holds on the lock
button; the SDK's rule). The B3 tap that enters returns from `OnPoll`
before the reslice / load / record blocks, so it is none of those; a B3
*hold* with B1 down still loads (the 400 ms hold-load fires first and
its release is not a tap).

**Entry** needs material and no take in flight (else a red blink). The
first selection is what plays now (`stepSrc[cur]`, else the step), so the
bar ring pulses at once; P1 takes over when it moves. **Leave** happens
from the main loop (the commit posts through the mailbox), so the rings
show Edit for one more turn.

**P1..P3 belong to the panel while Edit is on.** `OnFrame` does not read
`k_chop` / `k_hist` (nor the Brain page's first three knobs) and `OnPoll`
does not read `k_tempo` (the zone request and the internal tempo both
hold), so nothing the page plays moves for an Edit gesture; the Pager's
catch keeps following the pots underneath. On leave the three stored
values of the page Edit was entered on are put back with
`pager.SetStored` — catch re-armed, the frozen-dial ring says so — so a
pot left at the far end after nudging cannot make the chop or the tempo
leap. A page moved under Edit by the web app leaves Edit (commit) the
same way.

## The nudge pots (P2 start, P3 end)

- **Pickup = the SDK's catch, centred.** At every selection (and every
  re-base) the pot is armed against centre: caught at once if within
  0.005 of it, else inert until it crosses centre or rests within 0.005
  of it (`bb::panel::ArmPickup` / `UpdatePickup`, the same rule as
  `pot_catch.h`). The ring shows where the pot *would* land in dim
  colours meanwhile.
- **Offset**: `(pot − 0.5) × 100 ms`, whole milliseconds, a ±0.01 dead
  band at centre (a resting pot never posts), clamped to ±50. Origin =
  the entry's span at arm time, so turning back to centre is exactly "no
  change" and the offset is never cumulative; the *current* span (updated
  as each nudge lands) is what the clamps check against, so a start move
  followed by an end move clamps against the moved start.
- **What posts**: a grid slice's start is `SET_BOUNDARY(t, origStart +
  off)` (slice 0's start is the material's: refused), its end is
  `SET_BOUNDARY(t + 1, origEnd + off)` (the last slice's end: refused);
  both clamped so each neighbour keeps ≥ 64 samples, as `ApplyEdit`
  demands. A marker's are `SET_ENTRY` with the span clamped into
  `[0, freePos)` at ≥ 480. No `meta` (the core doc's "first guess that
  drifts", as the page's drags).
- **Rate**: a new whole-ms value is *offered*; at most one posts per
  50 ms, the last offered value wins and goes out when the window ends
  (`RateGate`, `NowUs()`), so a fast turn never queues and always lands
  on the final position.
- **Audition**: every landed nudge re-plays the slice's first 250 ms from
  the new start (`CallRegion`), an end nudge the *last* 250 ms up to the
  new end (`kRegionFromEnd`) — through the existing audition request with
  ACTION 27's 100 ms window (a request inside it is held and goes out
  from `OnPoll`), rate 1 (no varispeed: an audition is about the edge).
- **Re-base**: when `tableGen` moves for a reason that is not our own
  nudge (a page edit, a take, a load, a re-slice, an undo) the selection
  follows its ordinal (`Reselect`: the same entry if still live, else the
  live entry at its old ordinal, else the last), the origin is re-taken
  and both pickups re-arm.

## Rings and LEDs — the edit palette

| ring | in Edit |
|---|---|
| **P1 (bar)** | the **selection's bar** (a grid selection is always visible; a marker / no selection: the playing bar). The selected slice's own segment **amber**; every step whose source is the selection dim amber; **bright pulse** on whichever of those is the playing step, and on the selection's segment for six frames after a Voice call of it; the playhead elsewhere grey; the rest of the bar its class colours at a quarter |
| **P2 / P3** | a **fan from centre** (`DrawFill` Center): green clockwise = later, orange-red anticlockwise = earlier, the pivot amber — dim versions of the same until the pot has picked up; blank with no selection |
| **P4** | the pitch ring, as ever |
| **P5** | the kit map as ever, and the **key cells of every key pointing at the selection pulse** (a 16-frame triangle, amber; keys 8..23 share LEDs 8..15 in pairs) |
| **P6** | unchanged |
| **B1** | amber (leave); the clock's white pulse still flashes through |
| **B2** | undo-blue when there is something to undo (`PanelUndoDepth()`, 0 until package 6), else near-dark |
| **B3** | the chop orange while the break plays (material, not muted), else near-dark |
| all three | **red for 1.5 s** on any refusal, exactly as a set error (`gSetErrT`); a take's solid red still wins over the palette |

## The laws, kept

- **No `System::GetUs()/GetMs()`**: the rate window and the re-base use
  `NowUs()`; the one `GetNow()` (ms) in `PanelAudition` is the audition
  request's own window, copied verbatim from ACTION 27 so both callers
  share one clock.
- **The audio thread is untouched.** Everything goes through the existing
  mailboxes: `PostEdit` (the ACTION handler's wait — by the clock, ≤ 4 ms,
  `bb::Yield()` every turn) and `PostAudition` / `gAudRel`. `OnPoll` only
  sets flags and reads pots; the waits happen in `ServicePanel` from the
  main loop (phase 7), where `KeepClock` may run — never from inside
  `loop.Tick()`.
- **No behaviour change outside Edit.** Every existing gesture is behind
  `if(!gEdit)` or after a `return` that only Edit takes; the goldens, the
  Classic hash and every suite are as before (`tests/run.sh` ALL GREEN;
  `web/selftest.mjs` 377).
- **Edits are refused, never forced**: `EditBusy()` (a take, a set op, a
  material swap, a request still in the slot) and the engine's own
  refusals all end in the red blink and nothing else.

## Tail byte 257 and the page

`selSlice` = the selection while Edit is on, 255 outside it or with
nothing selected (`PanelSelByte()`). `web/hostlink.js` already parsed it;
`index.html` now calls `ed.select(selSlice)` when it changes and the
editor is up, so the page's waveform and key cell follow the knobs — the
hook EDIT-UI left.

## What is host-tested (`tests/panel_check.cpp`, 116 checks)

`core/bb_panel_edit.h` is pure and takes plain ints and the flags byte:

- **The walker**: live count / ordinal / entry with tombstones skipped,
  P1's bins at the ends and clamped, every ordinal reachable, nothing
  live → −1; key → entry as `Engine::KeySlice` reads it (span default,
  keys past the span, clamps, a marker, a tombstone → the span slice, an
  index past the table → the last, −1 = default, an empty table);
  `Reselect` after a delete, past the table, with nothing live.
- **The pickup**: caught at centre / within tolerance, inert on the same
  side, proximity and crossover pickup, stays caught, re-arms.
- **The offset**: dead band, ±50 at the ends, clamped past them, quarter
  turns, symmetry, monotonic over 200 positions; ms → samples; the ring
  value.
- **The rate window**: first posts, held inside 50 ms, replaced by a
  later offer, drains at 50 ms with the last value, nothing left, posts
  again past the window; across the 2^32 µs wrap.
- **The plan**: grid start / end → the right boundary index and position,
  the audition region (first / last 250 ms, a short slice whole, capped),
  the 64-sample clamps against both neighbours and the slice itself,
  slice 0's start and the last slice's end refused, the other edge of
  each allowed, a slice at both minima can only stay, no legal position
  → refused, centre = no change, no selection → refused; **origin vs
  current** (a landed start does not move the pot's origin, the next
  offset is not cumulative, the end pot clamps against the moved start;
  a marker's the same); markers clamped at 0 / `freePos` / `kRecMin`
  both ways, a stale base clamps in, no room → refused.
- **The computed grid** equals `SetMaterial`'s boundary for boundary on
  a real engine: straight 16, swung 16 (0.62), 32 steps; the odd formula;
  the ends; when swing applies.
- **The revert mask**: nothing / start / end / both, slice 0, the last
  slice, a marker.
- **The bar ring**: the displayed bar, the five LED states (own, plays,
  pulse on either, a Voice call, the playhead, plain, past the grid, a
  punched marker); the key cells on the selection; the pulse triangle.

Not host-testable and left to the checklist: the gestures' interplay
with the Pager and ParamLock, the LED look, the audition's feel.

## Sizes

`make` (`arm-none-eabi-size`, main at 81b40c1 → this branch):
text 389 360 → **396 100** (+6 740 B: the gesture block, `ServicePanel`
with three `PostEdit` paths, the palette — 6 252 of it — and the manual
clause, 488), data 4 556 → 4 596 (+40), bss 39 608 784 → 39 608 880 (+96:
the state block).
`tests/run.sh` ALL GREEN with `panel_check` added (116); the Classic hash
`c2081c20c9d7fbbf` and every golden unchanged; `node web/selftest.mjs`
377 / 377.

## For the integrator

- **Package 6 (`pkg4/edit-undo`) has landed on its branch, not in this
  one's base.** Two marked lines connect it, both under `/* EDIT-PANEL:
  B2 tap → EditOp::Undo (package 6) */`: `PanelUndoDepth()` → `return
  engine.UndoDepth();`, and `case kPanelUndo` → post `EditOp::Undo`
  through `PostEdit` (the exact line is in the comment). After an undo
  the table's gen moves and the next `ServicePanel` re-bases on its own.
- **One expected merge conflict**: the tail line `w.U8(engine.tableGen);
  w.U8(0u); w.U8(255u); w.U32(gSrcPos);` — package 6 replaces the
  `w.U8(0u)` with the depth, this package replaces `w.U8(255u)` with
  `PanelSelByte()`. Keep both.
- `s_b3_loaded` moved up to function scope in `OnPoll` (it gates the
  enter gesture too); the RECORD block's own declaration line went.

## Hardware checklist (Ernest)

1. **Enter / leave.** Load a break. Hold B1, tap B3: B1 amber, the bar
   ring turns amber-on-dark with one bright segment pulsing on the beat
   (the slice playing at entry), P2 / P3 show dim fans (or a dim pivot
   at centre). Release B1: **the page must not flip** and Edit stays.
   Tap B1: Edit leaves, the page is the one you were on, nothing
   audible changed — and the chop / tempo / history did not jump even if
   you turned P1..P3 in Edit (their rings show the frozen blue-grey with
   the pip until you sweep through).
2. **B1 + B3 held long** loads the highlighted file as before (no Edit).
   B2 + B3 for 2 s in Edit still opens Settings.
3. **Select.** Turn P1 slowly: the amber segment walks the bar; past the
   grid the ring shows the playing bar and the P5 cells of any key on the
   marker pulse. In the Voice map, play a key: its entry is selected
   (its segment / cells). Nothing sounds different: selection is silent.
4. **Nudge.** Select a kick. Turn P2 from its resting position: nothing
   happens (the dim fan moves) until it passes centre; then the fan is
   live and every step re-plays the first 250 ms from the new start —
   turn it to +50 ms and the kick's click is gone from the audition and
   from the next bar; back to centre and it is back. P3 likewise plays
   the last 250 ms up to the new end. Turn fast: the sound follows and
   lands on the final position (no queue). The web page's Kit editor
   follows the knobs (the selection moves, the boundary moves).
5. **Refusals.** Select slice 0 and turn P2 through centre: red blink,
   nothing moves (its start is the material's). Same for the last
   slice's P3. Options → Record = Arm, then a nudge: red (a take is
   armed).
6. **Chop.** With the break playing, tap B3 on a snare: an orange marker
   appears in the page's Kit editor at the snare's onset (Tap Allowance
   40 ms; try 0 and 80), selected, keyless. Mute (a note below 0 V):
   B3's LED goes dark and a tap blinks red. Hold B3: a reslice, and every
   orange boundary / marker survives.
7. **Revert.** After nudging a grid slice, hold B2: both boundaries back
   on the grid (the page's editor shows white again). On a marker: it is
   deleted, the selection moves to the next live entry, a key that was on
   it plays its grid slice again.
8. **Undo.** With package 6 merged: B2 blue after an edit, tap = the
   edit comes back, the pots re-arm (dim fans) at the restored geometry.
   Before the merge: red blink.
9. **Under a clock.** Everything above with Pamela's on J3: the nudges
   post between beats without a hiccup; leaving Edit does not touch the
   multiplier (P2's zone holds through Edit).
10. **Autosave** (Options): 2 s after the last nudge the set's JSON
    rewrites (the SETS ring progress on P5); a reload brings the
    boundaries back.
