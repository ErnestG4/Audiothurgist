# EDIT-CORE package — slice editing, the engine side (2026-09-08, branch `pkg4/edit-core`)

*Package 1 of SLICE-EDIT ([slice-edit-design.md](../slice-edit-design.md),
B8). Owns the EDIT additions to `core/bb_engine.h`, the new
`core/bb_edit.h` (the op bodies, included at the foot of the engine
header), `Looper::RemapSlices`, `bb::AnalyseEntry` in `core/bb_sample.h`,
and the EDIT sections of `tests/engine_check.cpp`. Nothing here is a
protocol byte, a gesture or a JSON field — packages 2–7 build on the API
below. The invariants of [sample-engine.md](sample-engine.md) are revised
there (items 1–3, 7, 8).*

## What it is

The two-layer table of the design's B0, made concrete in the engine:

- **The grid** `[0, nSlices)` keeps its count and its trigger times; its
  boundaries **move**, linked (slice i's start is slice i−1's end), each
  slice at least `kGridMin` = 64 samples, so the grid always tiles
  `[0, bufLen)`. A moved boundary is remembered in an **overlay** and
  re-applied when the same material is re-sliced to the same count.
- **Markers** `[nSlices, nTable)` are free spans anywhere in `[0, freePos)`
  — recordings past the material, as before, and now spans of the break
  itself — with overlaps allowed and `len ≥ kRecMin` (480). They split,
  merge and delete. A delete is a **tombstone** (`len 0`, no key,
  `tableFlags` b1): the index stays valid for everything that holds one
  (looper events, punches, the shell, the page); the next ADD / SPLIT /
  take reuses the slot; `SetMaterial` compacts.
- **Keys** point at any live entry, grid or marker; several keys may share
  one (decision 5). `tableKey` stays provenance — the key that recorded
  an entry — for the red-warning test, not the map.
- **Flags** per entry (`tableFlags[t]`, `TableFlags(t)`): b0 orange =
  user-placed or user-moved (every geometry op sets it on the entry it
  touched; a recording and the fresh grid are white), b1 tombstone.
- **`tableGen`** (u8, never 0) bumps on every table change — an edit, a
  take's commit, Clear, `SetMaterial`, the loader's two calls — so a page
  can carry it back and be refused when its view is old.

## The thread rule

**The control thread never writes `slices[]`.** It builds an `EditReq`,
runs `bb::AnalyseEntry` on the span when the op wants metadata, and posts
the record through a mailbox with an ack (the shell's `gCallReq`
discipline). The audio thread calls `engine.ApplyEdit(req, &idx)` at block
start, before `Process`; the status and `idx` go back through the ack.
Every op is O(kMaxTable) — plus O(looper.n ≤ 256) for a delete or a merge,
which re-target or drop the looper's keyless events — with no allocation,
no analysis and no audio search, so it fits the block budget beside a
call.

Control-thread writers of table state audited (item 5 of the brief):

| Writer | Thread today | Verdict |
|---|---|---|
| `SetKeySlice` | control (no shell caller; tests) | safe: one `int8` the audio thread reads whole and clamps; refuses tombstones now; bumps `tableGen` |
| `ClearKey` | control (style 8, browser Clear) | safe: two `int8` writes (`tableKey[t]`, `keySlice[key]`) |
| `CommitSlice` | control (`ServiceRecord`, the set loader) | a NEW entry is published in order (region, entry, flags, count, key) — consistent at every step. A REPLACEMENT (a second take into the same key) rewrites a live entry's two ints: a torn read is one wrong-but-bounded trigger (`ReadEnd` clamps to `bufCap`) — pre-existing, unchanged, noted for package 2 (refuse edits and takes during each other, or route the replacement through a request) |
| `RecArm` / `RecStop` / `RecDiscard` | control | touch no table entry |
| the grid-wide `memcpy`s + `SyncRecordedMeta` | audio (`AudioCb`) | unchanged |
| `SetGridBoundaries`, `SetKeyTable` | **audio, or before audio runs** | write many ints: package 5 must call them where `SetMaterial` runs (the apply block) or post them, never from `ServiceSets` while voices sound |
| `SetFromEngine` (a save) | control, reads | may read a torn entry if an edit lands mid-copy — snapshot between blocks (as the looper is) or refuse edits during a set op (package 5) |

`tableGen` is a byte written from both threads (the control-thread
writers above bump it too); a lost increment is harmless — the page's
refetch signature already carries `nTable`, `freePos` and `keySlice`.

## API

Thread column as in sample-engine.md: **audio** = the callback, at block
start; **control** = the main loop / OnPoll.

### The request

```cpp
struct EntryMeta { uint8_t cls; float sal, dens; bool hit; };   // = Engine::RecMeta
enum class EditOp : uint8_t { None, SetBoundary, SetEntry, AddEntry, Split, Merge,
                              Tombstone, SetKey, SwapKeys, NudgeAll, Promote };
enum class EditStatus : uint8_t { Ok, BadOp, BadArgs, Full, NotMarker, NotGrid, Tomb, Stale, NoMaterial };
struct EditReq { EditOp op; uint8_t gen, flags; int t, i, key, a, b, start, len; EntryMeta meta; };
                 // flags: kOrange (b0, ADD) · kMeta (b7: meta is filled)
```

| op | fields | `meta` (with `kMeta`) | rule |
|---|---|---|---|
| `SetBoundary` | `i`, `start` = pos | → slice i | grid, linked: `0 < i < nSlices`, pos in `[0, bufLen)`, both neighbours keep ≥ 64. Slice i orange. `NotGrid` / `BadArgs` |
| `SetEntry` | `t`, `start`, `len` | → t | marker, live: `[start, start+len) ⊂ [0, freePos)`, `len ≥ kRecMin`; overlaps allowed. `NotMarker` / `Tomb` / `BadArgs` |
| `AddEntry` | `key` (−1 or ≥ 24 = none), `start`, `len`, `flags` b0 orange | → the new entry; without it class 3, sal 0, dens 0, hit from the onset table | anywhere in `[0, freePos)`; the first tombstone's slot, else the end; `key` assigned at once; `tableKey −1` (a take into that key appends, it does not replace). `*newIdx` = the index. `Full` / `BadArgs` |
| `Split` | `t`, `start` = pos | → the new right half (the left keeps t's) | marker: t = `[t.start, pos)`, new = `[pos, t.end)`, both ≥ `kRecMin`; keys stay on t; both orange |
| `Merge` | `t` | → t | marker: t swallows the live marker whose span ends at t's start, else the previous live marker by index (none → `BadArgs`); t = the union; keys, keyless loop events and punches on the other re-point to t; the other is tombstoned; t inherits its `tableKey` if it had none |
| `Tombstone` | `t` | — | marker: `len 0`, `tableKey −1`, keys on it → their span slice (`SpanSlice`), keyless loop events on it dropped, a punched step of it rested; the audio stays in the buffer |
| `SetKey` | `key`, `t` | — | any live entry, grid or marker. `BadArgs` / `Tomb` |
| `SwapKeys` | `a`, `b` | — | `a ≠ b`; `keySlice` trades; the recorded owners (`tableKey`) trade too |
| `NudgeAll` | `a` = samples (≠ 0), `b` = 0 grid / 1 markers | — | grid: every boundary 1..n−1 moves, clamped left to right so each slice keeps 64 against its placed predecessor and leaves 64 per slice after it; all orange. Markers: every live one, clamped into `[0, freePos)`; none → `BadArgs` |
| `Promote` | `t`, `i` | → slice i | `SetBoundary(i, marker t's start)` |

`gen ≠ 0` must equal `tableGen`, else `Stale` (nothing done). A refusal
leaves the table, the flags and `tableGen` untouched. Ms → samples is the
shell's (`ms · sr / 1000`).

### Engine calls

| Call | Thread | Contract |
|---|---|---|
| `EditStatus ApplyEdit(const EditReq&, int* newIdx = nullptr)` | audio | above. On `Ok`: the touched entry's `loopState` (and its neighbour's for a boundary) is 0 — `FindLoop` re-searches at the next tail; `stepHit` is re-derived from the onset table for anything inside the material (`OnsetHit`: an onset in the first quarter of the span, or within 10 ms before it; no table → true), or kept from `recMeta` past it, or taken from `meta`; `recMeta` mirrors it so `SyncRecordedMeta` re-asserts the new values; a grid slice's gas well re-centres; the entry is orange; `tableGen` bumps; grid ops save the overlay |
| `void bb::AnalyseEntry(const Engine&, int start, int len, EntryMeta&, int mode = 0, float refRms = 0)` | control, before the post | `AnalyseRegion` over the L plane, packed as the request's `meta` (set `kMeta` after). Yields as it does. The span must lie in `[0, freePos)` — the audio thread only writes past `freePos` — else the silent default. Needed by: AddEntry (else class 3 / no density → fixed fades), Split (the new half; else it inherits), SetEntry / SetBoundary / Merge / Promote (else the entry keeps its old class / salience / density — first guesses that drift as the span moves; the page may post one when a drag lands) |
| `bool SetGridBoundaries(const int* starts, int n)` | audio, or before audio runs | the set loader: `n == nSlices`, `starts[0] == 0`, ascending by ≥ 64, the last ≥ 64 before `bufLen`. Boundaries that differ from the current grid become orange and enter the overlay (a file carrying the computed grid is a no-op: nothing orange, no gen bump). false = refused, nothing written |
| `bool SetKeyTable(const int8_t keys[24])` | audio, or before audio runs | the set loader: `keys[k]` = the entry, or −1 = the default (the key's own recording, else the span slice). false = an index ≥ `nTable` or a tombstone; nothing written |
| `uint8_t TableFlags(int t) const` · `bool IsTomb(int t) const` | either | the flags byte for `GET_TABLE` (package 2) |
| `int SpanSlice(int key) const` · `int FreeSlot() const` · `bool OnsetHit(start, len) const` | either | the helpers the ops use (`bb::SpanSlice(key, n)` in bb_record.h is the same arithmetic) |
| `uint8_t tableGen` | read anywhere | the generation for the state tail |

### What `SetMaterial` now guarantees

- **The keep rule.** The same planes (`b == buf && bR == bufR`) keep every
  live entry past the grid whose span lies in `[0, freePos)` — recordings
  past the material and markers inside the break — except a recording the
  new material grew over (it began past the old `bufLen` and inside the
  new). Tombstones are dropped and the survivors compacted, in order,
  behind the new grid (`MoveEntry` carries `tableFlags` now). New planes
  start the table over, as before.
- **The looper follows the compaction.** Keyless events (`key < 0`,
  bound to a table index) are re-targeted through the old → new map and
  dropped when their entry went; keyed events resolve through `KeySlice`
  at fire time and are untouched. This also fixes a pre-existing drift: a
  keyless event on a recording used to point at a grid slice after a Bars
  change.
- **Keys follow their entries** (`RestoreKeyTable`): a key on a kept
  marker → its new index; on a grid slice → the same slice when `nSlices`
  holds, else the span default. Without edits this is exactly the old
  `DefaultKeyTable` result (span, then each recording claims its key).
- **The overlay.** After the fresh grid (swing included) and its hits,
  when the planes, `bufLen` and `nSlices` are what they were at the last
  grid edit, every orange boundary goes back where the user put it —
  clamped left to right against its placed predecessor and the room the
  following slices need — and only those; a Swing change swings the
  untouched boundaries and leaves the orange ones. The hits of the slices
  whose span changed are re-derived. Another count, another length or new
  planes drop the overlay (`ovOrange = 0`), as does a grid with no room
  for 64 per slice.
- `nOnsetTab` / `onsetTab[128]` keep the onset table for the refreshes;
  `onsetsGiven` remembers "no table = every slice is a hit".
- With no edits every path is byte-identical: the four PLAY goldens, the
  SAMPLE golden `786e423d477968a2` on all three render paths, and the
  Classic grammar hash `c2081c20c9d7fbbf` hold.

### Tombstones, everywhere they could bite

`FireSlice` already refused `len < 64`; added: `TriggerStep` returns
before `RaiseGates` (a deleted marker punched into a step raises no gate),
`PunchStep` refuses one, `SetKeySlice` refuses one, `SetKeyTable` refuses
one, `KeySlice` never returns one (keys are re-pointed at the delete),
`RecArm` / `CommitSlice` treat a tombstone's slot as free, `ReadEnd` of
one is its start (harmless), `FindLoop` on `len 0` finds nothing. The
looper's keyless events on it are dropped at the delete, and its punched
steps rested, so a reused slot cannot resurrect them.

## What was measured (`tests/engine_check.cpp`, EDIT sections — 89 checks, 386 → 475)

- **A boundary move is live and honest.** `SetBoundary(5, 27000)` on the
  16-step synthetic break: the voice fired before the edit plays slice
  5's old region to its old end; the next call opens with 3000 samples of
  slice 4's tone then its own; in the sequencer step 5 fires at its own
  time with the moved content and step 4's slot is its 3000-sample slice
  then silence (content moves, timing does not). Hits follow: with an
  onset at every old start, slice 5 = `[27000, 36000)` loses its hit
  (the onset at 30000 is past its first quarter) and `[29800, 36000)`
  gets it back; both neighbours' loop points are forgotten.
- **Contiguity and refusals.** Thirteen refusals (i = 0, i = n, a 63-sample
  neighbour either side, pos < 0, pos = bufLen, a stale gen, a zero nudge,
  a marker nudge with none, tombstone / merge / SetEntry on a grid slice,
  an unknown op) each leave the table and `tableGen` alone; a matching gen
  is accepted. Nudges of +1000, −500000 and +700000 clamp every boundary
  to 64 apart and the grid still tiles; every moved boundary is orange
  (`0xFFFE`). The same sequence on two engines gives the same table.
- **Markers.** ADD over slice 7 on key 5 with `AnalyseEntry`'s meta (mid
  band, salience 1, hit, density > 0): entry 17, key 5 → 17, plays slice
  7's tone, no owner (`RecArm(5)` would append). SPLIT at +2400 → 18,
  keys stay on 17, the new half inherits the class; a half under
  `kRecMin` refused. A keyless ADD, SET_KEY onto it, onto a grid slice,
  two keys on one entry. SET_ENTRY anywhere in `[0, freePos)`, refused
  past it or under `kRecMin`. MERGE(18) swallows the adjacent 17 (key 5
  → 18, 17 a tombstone); calling the tombstone: silence, no hit counted,
  no voice; it refuses keys, a second delete, a move and a punch.
  TOMBSTONE(19) with keys 6 and 7 on it → both back to span slices, its
  punched step rested, `nTable` unchanged. ADD reuses slot 17. MERGE(18)
  with nothing adjacent joins the previous marker by index (the union).
  The table fills to `kMaxTable` then `Full`; `RecArm` refuses a new key
  and accepts one with an entry; a delete frees a slot. Marker nudges
  clamp to `[0, freePos)` and skip tombstones. SWAP_KEYS(3, 9) with a
  recording on 3: key 9 plays and owns it, key 3 plays slice 9; `a == b`
  refused.
- **Compaction with the looper.** In Play: a keyless event on marker 18, a
  keyed event on key 3 (a 1 kHz "recording"), a keyless event on marker
  17; TOMBSTONE(18) drops its event from the loop; pass 2 plays the
  recording at 0.5 and marker 17's tone at 0.75. `SetMaterial` to 32
  steps on the same planes: `nTable` 34, no tombstones, key 3 → 32, key 5
  → 33 (still orange), the keyless event → 33 — and the loop renders the
  same audio. The material grown by 6000 samples drops the recording
  under it and keeps the break marker.
- **Swap through the looper.** An event recorded on key 3 plays slice 3;
  after SWAP_KEYS(3, 9) the same event plays slice 9.
- **The overlay.** Two moved boundaries survive a same-count re-apply
  (orange kept, untouched ones white); a Swing re-apply swings boundary 3
  and holds 5 and 9; a count change gives the computed grid with nothing
  orange and no overlay, and back to 16 the plain grid; new planes drop
  it; tight boundaries (100, 200) survive a swing re-apply intact. A
  break marker on key 5 and a key on grid slice 2 survive a same-count
  re-apply; at 32 the marker follows to 32 with its key and the grid key
  falls back to the span; new planes drop both.
- **The loader's calls.** The computed grid is a no-op (no orange, no gen
  bump); two moved starts apply linked and orange and come back through
  a re-apply; refusals for `n ≠ nSlices`, `starts[0] ≠ 0`, a 63-sample
  slice, 63 before the end, not ascending (nothing written).
  `SetKeyTable` with −1 = default, an override, refusals for an index
  ≥ `nTable` and for a tombstone, accepted once the slot is alive again.

The rest of the suite is unchanged: `tests/run.sh` ALL GREEN, the Classic
hash and every golden as before (`record_check` 53, `sets_check` 70,
`persist_check` 772, `clock_check` 65, `scope_check` 96).

Firmware (`make`, `arm-none-eabi-size`): text 374 632 → 376 088 (+1 456 B),
data 4 496 → 4 496, bss 39 607 448 → 39 608 344 (+896 B: the onset table
copy 512, the overlay 256 + 8, `tableFlags` 96, a few scalars). Nothing
in the shell calls the new API yet, so the text growth is `SetMaterial`'s
new keep / restore / overlay path and the tombstone guards.

## Open questions and what is left for packages 2–7

1. **Grid metadata after a boundary move.** `stepClass` / `sliceSal` /
   `sliceDens` of the two neighbours are kept (a class is per step and the
   move touches only an edge); the page can post `meta` for slice i when a
   drag lands, but not for i−1 in the same op. The shell's reclassify
   (`gReclass`) classifies an equal-step grid, not the edited boundaries,
   as it already ignores swing — a later package may pass the real
   starts to `ClassifySteps`.
2. **Sets.** `SetFromEngine` writes every entry including tombstones
   (`len 0`, key −1) and keyless / in-break markers, which `PlanSet`
   refuses ("a recording inside the break", "everything after the first
   keyed entry must be keyed", `len < kRecMin`) — so a set saved after an
   edit does not load until **EDIT-PERSIST** adds `flags`, `keys[24]`,
   the relaxed `PlanSet`, and skips tombstones on save (re-indexing the
   saved looper's keyless events as it goes). Note the pre-existing case:
   a set saved after `ClearKey` already fails to load for the same reason.
   *Done — [slice-edit-persist.md](slice-edit-persist.md): `SetGridBoundaries`
   gained a `flags` argument, `CommitEntry` joined `CommitSlice`, both
   loader calls run where the table above says.*
3. **Package 2 (EDIT-PROTO)** posts `EditReq` from the ACTION handler
   through a mailbox with an ack, refuses (`BAD_STATE`) while a take is
   armed or pending and during a set op, exports `TableFlags` in
   `GET_TABLE`, `tableGen` in the tail, and converts ms → samples for
   `NudgeAll`. The `Stale` status maps to `BAD_STATE`; `Full` and the rest
   to `BAD_ARGS`.
4. **A replacement take** (`CommitSlice` on a live entry) is still a
   control-thread two-int write — the one torn read left in the engine.
   Package 2 can refuse edits while a take is in flight (it must anyway),
   which leaves only the take's own commit; routing that through a request
   would close it entirely.
5. `RecArm` on a key that points at a break marker appends a new entry and
   re-points the key — the marker stays, keyless. The kit-map LED
   (`RecordedEntry`) does not light for markers. Both are the intended
   "markers are not recordings" reading of B4; say so in the page.
6. `EditReq` carries `int`s for readability; the wire form is package 2's.
   `NudgeAll` for the grid marks every boundary orange (all moved).
7. `MERGE` on a recording and a break marker joins spans that may be far
   apart (the union) — legal by construction, odd in the UI; the page
   should offer merge only for adjacent markers (B1's double-tap).
