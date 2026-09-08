# EDIT-UNDO — the inverse-op journal (2026-09-08, branch `pkg4/edit-undo`)

*Package 6 of SLICE-EDIT ([slice-edit-design.md](../slice-edit-design.md)
Part A §7, B5, B8). Owns `core/bb_undo.h`, the `EDIT-UNDO` lines of
`core/bb_engine.h` / `core/bb_edit.h` (`EditOp::Undo` / `Redo`,
`EditReq::kJoin`, the hooks in `ApplyEdit`, the clear in `SetMaterial`),
the shell's `gUndoJournal`, ACTION 30 / 31 and tail bytes 256 / 262 in
`src/audiothurgist.cpp`, the EDIT-UNDO parts of `web/hostlink.js` /
`web/demo.js` / `web/edit.js` / `web/index.html` / `web/selftest.mjs`, and
the EDIT-UNDO section of `tests/engine_check.cpp`. Builds on
[slice-edit-core.md](slice-edit-core.md) (`EditReq`, `ApplyEdit`) and
[slice-edit-proto.md](slice-edit-proto.md) (the mailbox, the tail).*

## What it is

**The firmware owns the history** (design B1.1): every successful
`ApplyEdit` leaves one record in a ring of 64, and `UNDO` / `REDO` are two
more ops through the same mailbox — so the panel's undo (package 7) works
with no page attached, and the page is a viewer that draws labels next to
the depths the tail reports. Nothing persists (as the looper's undo).

A record is **what restoring one op needs**, not a table snapshot:

| forward op | record |
|---|---|
| SET_BOUNDARY / PROMOTE | entries i−1 and i |
| SET_ENTRY / TOMBSTONE | entry t |
| ADD_ENTRY (TAP_MARK too) | the slot it will take (`FreeSlot()`: a tombstone's state, or a slot past `nTable`) |
| SPLIT | entry t and the slot the right half takes |
| MERGE | entry t and the marker it swallows (`MergeTarget`, factored out of `EdMerge`) |
| SET_KEY | the key map alone |
| SWAP_KEYS | the two entries the keys point at (their recorded owners trade) |
| NUDGE_ALL | every start of the nudged range — the grid `[0, nSlices)` or the markers `[nSlices, nTable)` — with its orange and hit bits |
| FROM_ONSETS | the shell's applies, joined into one step (below) |

An "entry" in a record (`UndoEntry`, 24 B) is `start, len, tableKey,
tableFlags, stepClass, sliceSal, sliceDens, stepBands, stepHit` plus its
index; every record also carries the whole `keySlice[24]` and `nTable`.
The hit bit travels because it is not always re-derivable: an entry's hit
may have come from `AnalyseEntry`'s meta, not the onset table, and
`RefreshEntry` would overwrite it. NUDGE_ALL stores per entry because
`EdNudge` clamps each boundary against its neighbours and each marker into
`[0, freePos)` — the op is uniform, the result is not, so the delta alone
would not invert it.

**Records are symmetric.** Restoring one first captures the same indices'
current state into the same slot, so an undone record *is* its redo, and
one cursor walks the ring both ways: `cur` counts the records before it
(`[0, cur)` undo, `[cur, count)` redo); undo = `cur−−` + swap, redo = swap
+ `cur++`, a new forward op sets `count = cur` (the redo half is gone) and
pushes; a full ring drops the oldest. No separate redo stack, and no
memory for one.

**Steps.** A record is a *head* unless the request carried
`EditReq::kJoin` (b6), in which case it joins the previous record's step.
`Undo` pops records back to and including a head; `Redo` applies from the
cursor to the next head. The shell posts FROM_ONSETS' tombstones and adds
with `kJoin` after the first successful apply, so the whole op undoes as
one step — and the depths count steps, not records (a headless run at the
oldest end, whose head the ring overwrote, counts as one).

**Bound to the generation.** Every journaled apply leaves `journal.gen =
tableGen`. Anything that bumps `tableGen` past the journal — a take's
commit (`CommitSlice` / `CommitEntry`), `ClearKey`, the set loader's
`SetGridBoundaries` / `SetKeyTable`, `SetMaterial` — makes it stale: the
tail's depths read 0, the next `UNDO` / `REDO` empties it and answers
BAD_ARGS, and the next forward op restarts the history at that op. This
one rule covers every non-journaled writer without touching any of them
(`tableGen` is a byte, so 255 un-journaled bumps with no edit between
would alias; not a real case). `SetMaterial` also clears explicitly.

**What is *not* restored** (documented, not hidden): a TOMBSTONE or MERGE
drops the looper's keyless events on the entry and rests its punched steps
(`stepSrc` / `playSrc`); an undo brings the entry back but not those — the
looper's own undo (ACTION 12) is the place for its events, and a punch is
a performance gesture, not table state. A MERGE's re-pointed events stay
on the surviving marker.

## API

| Call | Thread | Contract |
|---|---|---|
| `Engine::undo` (`UndoJournal*`) · `SetUndoJournal(j)` | before audio runs | attach the shell's ring (clears it); `nullptr` = no journal: no record, Undo / Redo → `BadArgs`, depths 0, the op itself unchanged |
| `ApplyEdit(EditReq{op = Undo / Redo})` | audio | one step; `BadArgs` = nothing to undo / redo (or a stale journal, emptied); `Stale` for a wrong non-zero gen, before anything moves; on `Ok` the entries, key map and `nTable` are back, `loopState` of every touched entry 0, `stepHit` / `recMeta` as they were, the overlay re-saved when a grid entry moved, `tableGen` bumped |
| `int UndoDepth() const` · `int RedoDepth() const` | either | steps back / forward, 0 when stale or detached (the tail) |
| `EditReq::kJoin` | — | the op joins the previous step |
| `UndoJournal::kDepth` = 64 · `Clear()` · `count` / `cur` / `base` / `gen` | — | the ring; `Steps(from, to)` counts heads |

`UndoRec` is 440 B (`static_assert`): an 8-byte header (op, kind, head, n,
nTable, first), the 24-byte key map, then a union of two `UndoEntry` (48)
or the nudge form (orange + hit masks 24, `int32 start[kMaxTable]` 384).
The push copies only `Bytes()` (a two-entry record is 80 B; a 32-marker
nudge 184). `UndoJournal` = 65 × 440 + 16 = **28 616 B**: the ring, the
capture in flight (`scratch`, so a refused op leaves the ring alone) and
four counters. It is a POD: NOLOAD in SDRAM, cleared by `SetUndoJournal`.

## Protocol (adds to [slice-edit-proto.md](slice-edit-proto.md))

| op | body | reply | rule |
|---|---|---|---|
| 30 UNDO_EDIT | — (an optional trailing `u8 gen` as the others) | `u8 status, u8 tableGen` | one step back; BAD_ARGS when there is none (or the history was ended by a take / clear / load); BAD_STATE during a take, a set op, a swap, a stale gen |
| 31 REDO_EDIT | — | `u8 status, u8 tableGen` | one step forward; BAD_ARGS when there is none (a new edit clears the redo half) |

**State tail**: byte 256 `u8 undoDepth` is live (steps back, saturating at
255 — the ring makes 64 the real ceiling); **byte 262 `u8 redoDepth`** is
appended (steps forward), the body ends at **263**. Append-only: a page
that reads 262 bytes still works; a page seeing 263 knows the journal is
there (a 262-byte body is EDIT-PROTO without package 6 — `undoDepth` 0
means "no journal" there, "nothing to undo" here).

An undo bumps `tableGen`, so EDIT-PERSIST's autosave fires for it like any
edit, and a page's `syncKit` refetches.

## Web

`hostlink.js`: `parseState` reads `redoDepth` at ≥ 263; `editUndo` /
`editRedo` were already there. `demo.js`: the journal with the firmware's
semantics but whole-table snapshots (`tableSnap` / `tableRestore` — the
fake has no memory budget): 64 deep, a new op clears the redo half, bound
to the generation it last wrote (`journalGen`; `recordKey`, `clearKey`,
new material and a set load end it), ops 30 / 31, the 263-byte tail, BAD_STATE
during its take. `edit.js` `History`: `undo(gen)` strikes the newest live
entry (it stays listed), `redo(gen)` un-marks the oldest undone, `push`
drops the undone ones, `note(gen, undoDepth, redoDepth)` drops the marks
when the tail says redo 0 — the marks are a picture of the module's
depths, never the other way round. `index.html`: Undo enables from
`undoDepth`, Redo from `redoDepth`, the depth line reads "undo n · redo m"
/ "nothing to undo" / "no journal on this firmware" (no byte 262), undone
entries struck through; `?demo=noundo` plays a 262-byte firmware.

## What was measured

`tests/engine_check.cpp` (EDIT-UNDO, **521 → 655**, +134): on the 16-step
synthetic break with a recording on key 3 — an empty journal refuses both
ops; then, cumulatively, SET_BOUNDARY with meta, ADD with meta, SET_ENTRY,
SPLIT with meta, SET_KEY onto a marker and onto the grid, SWAP_KEYS with the
recording's owner trading, MERGE of the adjacent pair, three grid NUDGEs
(+1000, −500000 clamping every boundary to 64 apart, +700000), a marker
NUDGE, PROMOTE, a grey ADD reusing the tombstone's slot, TOMBSTONE of a
marker carrying a key and of the recording, SET_ENTRY to the buffer's head:
each undo equals the full snapshot before (slices, flags, `keySlice`,
`tableKey`, `recMeta`, class / salience / density / bands / hit, `ovOrange`,
`nTable`; gen differs) with the depths stepping, each redo equals the
snapshot after; undo-all (17 steps) is the origin, redo-all the end; a stale
gen on UNDO is `Stale` with nothing moved; a new op after two undos clears
the redo half. A tombstone plus three `kJoin` adds is one step (four
records) both ways. 100 seeded random ops over all eleven kinds (≥ 40 land),
undo-all restores the origin exactly and redo-all the end. 70 boundary
moves: depth 64, 64 undos then BAD_ARGS, the table where op 6 left it, 64
redos back. Audio: a voice fired after SET_BOUNDARY(5, 27000) and undone
1200 samples in still plays slice 4's tone then slice 5's (its bounds were
copied at trigger), and the next call plays slice 5 from its restored
start. `SetMaterial` to 32 steps clears (Undo refused); the same planes and
count keep the edit through the overlay but clear the history (the choice
here); a `CommitSlice` past the journal reads depth 0, Undo refused, the
next op restarts at 1; `ClearKey` ends it; with the journal detached the op
lands, depth 0, Undo BAD_ARGS. Sizes asserted.

`web/selftest.mjs` (**377 → 430**, +53): the 262 / 263 / 300 bodies; ACT 30
/ 31; over the fake kit every op's undo restores the raw 14-byte table and
key map byte for byte and redo re-applies (SET_BOUNDARY, ADD, SET_ENTRY,
SPLIT, SET_KEY ×2, SWAP_KEYS, MERGE, PROMOTE, both NUDGEs, FROM_ONSETS as
one step, TAP_MARK, TOMBSTONE ×2) with the depths; a stale gen BAD_STATE;
two undos then a new op → redo 0 and REDO BAD_ARGS; undo-all equals a fresh
kit, redo-all the end; 70 moves saturate at 64, 64 undos, the 65th refused;
BAD_STATE during a take; a take ends the history and the next op restarts
it; `noedit` refuses 30 / 31; the `History` viewer's marks; the editor's
undo / redo of a tap mark (the label struck then cleared); the page's
buttons and depth line through a nudge → Undo → Redo.

`tests/run.sh` ALL GREEN (Classic hash `c2081c20c9d7fbbf`, every golden as
before — with no edits nothing runs but a pointer test). Firmware (`make`,
`arm-none-eabi-size`): text 389 360 → **392 696 (+3 336 B)**, data 4 556 →
4 556, bss 39 608 784 → **39 637 432 (+28 648 B)** = the 28 616-byte journal
in `.sdram_bss`, the engine's pointer, alignment. DTCM: the engine grew by
one pointer.

## The choice at SetMaterial, and what is left

- **Cleared, not remapped.** The compaction's old → new map could re-index
  the records' entry indices and key maps, but a record also names the
  slot an ADD took (which the compaction may have dropped as a tombstone),
  `nTable` before and after, and the marker range of a NUDGE — all of which
  the compaction re-derives. The overlay already keeps the *edits* through
  a same-plane re-apply; keeping the *history* too would mean remapping
  four kinds of index and rejecting records whose entries went. The
  generation rule makes the clear free and honest: Bars / Swing / Onsets /
  a set load / new material end the history, the page says so by the
  depths dropping to 0.
- Labels stay the page's (design B5's "the journal's labels"): the record
  keeps the forward `op` byte for a future GET, but the panel (package 7)
  needs no label and the page has better ones than the firmware could
  send.
- The looper's keyless events and punches dropped by a delete are not
  brought back by its undo (above).
- A firmware without this package answers 30 / 31 with BAD_ARGS and a
  262-byte body; the page reads that as "no journal" and keeps its list.
