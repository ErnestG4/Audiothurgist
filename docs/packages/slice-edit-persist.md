# EDIT-PERSIST package — an edited table on the card (2026-09-08, branch `pkg4/edit-persist`)

*Package 5 of SLICE-EDIT ([slice-edit-design.md](../slice-edit-design.md),
Part A "Persistence", B1 principle 3, B7, B8). Owns the EDIT-PERSIST
additions to `core/bb_set.h` (the format), `core/bb_sets.h` (`SetFromEngine`,
`PlanSet`, `CommitSetRecordings`), `Engine::CommitEntry` and the flags
argument of `SetGridBoundaries` in `core/bb_engine.h` / `bb_edit.h`, the
`/* EDIT-PERSIST */` fences in `src/audiothurgist.cpp` (Options byte 23,
the loader's two new steps, the sidecar, the autosave), and the EDIT-PERSIST
sections of `tests/sets_check.cpp`, `tests/persist_check.cpp` and one of
`tests/engine_check.cpp`. Built on [slice-edit-core.md](slice-edit-core.md),
[sample-sets.md](sample-sets.md) and [sample-persist.md](sample-persist.md);
both of those are revised where this changes them.*

## What it is

Before this package a set could not hold an edited table: `SetFromEngine`
wrote tombstones and keyless / in-break markers, and `PlanSet` refused all
three ("a recording inside the break", "everything after the first keyed
entry must be keyed", `len < kRecMin`) — a set saved after any edit, and a
pre-existing case, a set saved after `ClearKey`, did not load. Now:

- **The file carries the edit.** Per entry `"flags"` (b0 orange), the
  grid's `"grid"` count, and a top-level `"keys":[24]` key map. Tombstones
  are **not written**; every index that refers to the table — the keys, a
  keyless looper event's `slice` — is re-indexed to the compacted order.
  Still schema 1: an older firmware skips the three keys and loads the
  material and recordings minus the edits.
- **Load applies it.** `PlanSet` accepts markers anywhere in `[0, len)`,
  keyed or keyless, many keys per entry; it still validates a grid that
  tiles `[0, material)` with ≥ 64 samples per slice. After `SetMaterial`
  the file's grid starts go through `SetGridBoundaries` on the audio
  thread (an old file carries exactly the computed grid: a no-op); after
  the markers are committed, the key map goes through `SetKeyTable`.
  Orange comes back on both.
- **Autosave** (Options byte 23, **On** by default — Ernest's call on B9.3):
  2 s after the table last moved, the JSON half of a save is written — into
  the set's JSON when the material came from a set (the WAV is never
  rewritten), else into `sets/<wavname>.slices.json` beside a loose file,
  which the next plain load of that file reads back. "Save set" stays
  explicit and is still what a take needs.

## The format (`sets/<name>.json`, still `"audiothurgist":1`)

```json
{"audiothurgist":1, …, "grid":32,
 "slices":[{"start":0,"len":3000,"key":-1,"cls":0,"sal":0,"hit":1,"flags":0,"name":""}, …,
           {"start":21000,"len":6000,"key":-1,"cls":1,"sal":0.9,"hit":1,"flags":1,"name":""},
           {"start":96000,"len":6000,"key":20,"cls":2,"sal":0.4,"hit":0,"flags":0,"name":""}],
 "keys":[0,1,2,4,5,33,33,34,2,12,13,14,16,17,18,20,21,22,24,25,32,28,29,30],
 "options":[…24…], "looper":[{"phase":0.1,"key":-1,"style":0,"vel":1,"slice":33}, …]}
```

| Key | Type | Meaning |
|---|---|---|
| `grid` | int | the grid's entry count: `slices[0, grid)` are the grid, the rest markers. Absent (an old file, or 0 / out of range): inferred as the leading `key −1` entries that start inside the material — right for every file the old firmware wrote, since it had no keyless in-break markers |
| `slices[].flags` | int | b0 **orange** = user-placed / user-moved (`Engine::kFlagOrange`); other bits are masked on read. A tombstone is never written |
| `slices[].key` | int | unchanged meaning, wider use: the key that **recorded** the entry (provenance, `tableKey`), −1 for a grid slice **or a marker nobody recorded** — an in-break span, a recording after `ClearKey`. Not the key map |
| `keys` | int[24] | the entry each Voice key plays, as an index into **this file's** `slices[]`, −1 = the default (the key's own recording, else its span slice). Extras dropped, a short list defaults the rest, values outside −1..127 → −1 |
| `looper[].slice` | int | as before, but re-indexed to the compacted order; a keyless event whose entry is gone is not written |
| `options` | int[24] | `OPT8`: 22 Tap Allowance (EDIT-PROTO), **23 Autosave**. A set's byte 23 is ignored on load (Autosave is the device's, like Record); a preset's is honoured |

The WAV's `cue ` chunk keeps every live start (the compacted table's), as
before. A worst-case set (96 entries, 256 events, 32 options) emits
29 497 B, under `kSetJsonMax` (32 KiB).

### The sidecar — `sets/<wavname>.slices.json`

The set JSON **without the material**: the same document with `len` =
`material` = the break's (conditioned) length, the grid and the in-break
markers only, `keys` (a key on a recording → −1), no `options`, no
`looper`, `name` = the WAV's filename. Recordings past the material are
not in it — they live in the buffer, and a loose file has no WAV of its
own to hold them; a take needs a set. `<wavname>` is the entry's basename
with its extension (`AMEN_170.WAV` → `sets/AMEN_170.WAV.slices.json`; a
JSON-less `sets/plain.wav` → `sets/plain.wav.slices.json`, never a
collision with a set's own `sets/plain.json`).

On a plain load the sidecar is read before the load pipeline runs and
staged only when it fits: its `material` must equal the conditioned
length (the conditioning is deterministic for the same file, options and
firmware — a different cut means a different buffer, and the sidecar is
ignored), and its grid starts apply only when its `grid` count equals the
slice count the analysis chose (Bars changed since → the markers and their
keys still come back, the grid keys default, the boundaries stay computed).
Its `swing` replaces the analysis's so the untouched boundaries land where
the file has them; a later Swing change re-swings the white ones through
the overlay and leaves the orange ones.

## Save — `SetFromEngine(e, lp, s, stereo, sr, maxLen = 0)`

Walks the table once with an old → file index map: grid entries always;
a marker when it is not a tombstone, has `len > 0` and lies inside
`[0, maxLen]` (`maxLen` 0 = `freePos`). `flags` = the orange bit; `grid`
= `nSlices`; `keys[k]` = `map[KeySlice(k)]`; a looper event's `slice`
through the map, a keyless event dropped when its entry is not in the
file (a keyed one keeps −1 there — it resolves through the key at fire
time). `maxLen` is the autosave's bound: a set's JSON may only describe
what its WAV holds, so the set-JSON autosave passes the WAV's length (a
take since the save is left for Save set) and the sidecar passes `bufLen`.

The save runs as before (`SaveSet`, control thread, WAV then JSON) and
records the WAV's length and plane count for the autosave.

## Load

`PlanSet(s, p, maxGrid, recMin, keys, gridMin = kGridMin)` now fills, on
top of the old plan, `starts[nGrid]` and `gridFlags[nGrid]` (the file's
grid), `hasKeys` and `keys[24]` (file indices), and lists every entry after
the grid in `rec[]` — recordings and in-break spans, keyed or keyless, in
file order. Refused, plan zeroed: no grid, more than `maxGrid`, a grid
that does not tile — `starts[0] ≠ 0`, a step under `gridMin`, less than
`gridMin` before the material's end — a marker outside `[0, len)` or under
`recMin`, a key ≥ 24 or a keyed entry whose key another entry already
claims (`CommitEntry` would replace it), a key-map index the file does not
hold.

The shell's sequence, in `LoadSet` → the apply → `ServiceSets`:

| Step | Thread | What |
|---|---|---|
| `PlanSet`, options, the WAV into the other buffer, the looper staged | control | as before, plus `gSetPendGrid`, `gSetPendCommit` (always now), the WAV's length / planes remembered |
| `SetMaterial(…, plan.onsets, plan.cls, plan.nGrid, …)` | audio (the apply) | as before |
| **`SetGridBoundaries(plan.starts, plan.nGrid, plan.gridFlags)`** | audio, the line after | the file's grid: boundaries that differ become orange and enter the overlay; an orange bit on a boundary that sits where the grid computes it is restored too (`flags`, the new third argument); the computed grid is a no-op (no gen bump). Runs before any trigger can read `slices[]` — the thread rule of slice-edit-core.md kept to the letter |
| `CommitSetRecordings(e, s, plan, dens, live)` | control, the next pass | every marker through **`CommitEntry`** (`CommitSlice` generalised: a key < 0 makes a keyless entry with no owner and no key re-pointed; `flags` = the orange bit; same publication order — region, entry, flags, count, key). `live[24]` = the file's key map as live indices: grid i → i when the engine's count is the file's (else −1), marker j → the index `CommitEntry` gave it |
| **`SetKeyTable(live)`** when the file carried `keys` | control, same pass | 24 one-byte writes the audio thread reads whole (`KeySlice` clamps); an old file skips it and keeps `SetMaterial`'s default table, byte-identical to before |
| the autosave baseline | control, same pass | the post-hook `tableGen` is "on the card" |

Old files: every step is a no-op or the old path — `SetGridBoundaries`
with the computed grid changes nothing, `CommitEntry` with a key behaves
as `CommitSlice`, no `keys` → no `SetKeyTable`. `sets_check` proves the
old-format file and the new one load into identical engines with
identical audio.

## Autosave (Options byte 23, `OPT8`, default On)

`ServiceAutosave` in the main loop (`ServiceSets`, off the poll path):

- **Trigger**: `engine.tableGen` ≠ the generation on the card. Every
  table change bumps it — an edit, a take's commit, `ClearKey`, a
  re-slice (Bars, Swing, Onsets, Classifier, Sampling) — so a re-sliced
  set autosaves its new grid too: the set reopens as it was left. A load
  arms a baseline instead: the generation after the load's own two calls
  is taken as clean, so loading never writes.
- **Timer**: the generation is noted with `NowUs()` (the shell's 64-bit
  time base — never `System::GetUs()`); another change restarts the 2 s.
  Off = the timer idles; turn it On and the pending edit saves 2 s later.
- **Gate**: never while a set op, a load, an apply / re-apply, a loop take
  or a key take (armed or pending) is on — it simply waits.
- **Target**: the set's JSON (`gSetCurName`), through `SnapshotLooper` +
  `SetFromEngine(…, gSetWavLen)` + the live options / bars / bpm — the
  JSON half of Save set, written to `<path>.tmp` and `f_rename`d over so an
  interrupted write never leaves half a document beside a good WAV; else
  the sidecar (`gCurFile` a card file); a take has nothing on the card and
  is marked clean. `f_rename` is the SDK's own FS-extension pattern.
- **Reporting**: one attempt per generation. Failure → `SetFinish(8)` —
  **`setResult` 8 = autosave failed**, `setSeq` bumps, the buttons blink
  red as for any set failure; success is silent (no `setState`, no ring —
  a 30 KB write is milliseconds; the sink still yields to the clock).
- **What it never does**: rewrite a WAV, arm a take, run on the audio
  thread, allocate.

Everything pinned by the schema bump, exactly as byte 21 was added:
`Options::kBytes` 22 → **24**, `Serialize` / `Deserialize` / `Put(…, 1)`,
`SchemaHash` `'OPT6'` → **`'OPT8'`** (`0x4F505438`; OPT7 = EDIT-PROTO's
23-byte step), `Describe` field 23 `"autosave"` "Autosave" with labels
"Off" / "On (2 s after an edit)", `gAutosave = {1}`, the manual's options
sentence, README's option table, `bb::kSetOptionsToday = 24` in `bb_set.h`
with a `static_assert(Options::kBytes == bb::kSetOptionsToday)` in the
shell, `persist_check`'s "Options schema pin" section and `sets_check`'s
24-byte option data. Byte 22 is written as 0 and skipped on read here — a
placeholder for EDIT-PROTO's Tap Allowance; the integrator's merge fills
it. Presets saved under OPT6 re-init; their card backups come back
(sample-sets.md) with byte 23 = the live default.

## What was measured

`tests/sets_check.cpp` **70 → 144** (ASan/UBSan, in `tests/run.sh`):

- **PlanSet, relaxed and tightened.** A keyed marker inside the material;
  keyless and keyed markers inside and past the break in any order with
  the explicit `grid` count — and, without it, the same file refused
  (inference reads the keyless in-break marker as a grid entry, out of
  order); a 63-sample grid slice and 63 samples before the end refused, 64
  accepted with the starts carried; a grid not at 0 refused; an explicit
  count past the entries refused; an explicit smaller count makes the rest
  markers; a key onto an entry the file lacks refused, keys carried as file
  indices; the whole table (64 + 24 keyed + 8 keyless) accepted, one more
  refused; the old refusals that still hold (duplicate key, key 24, past
  the file, under `kRecMin`, out of order, no grid).
- **Engine → set → engine with edits.** Boundary 5 moved half a step
  (orange), an in-break marker over slice 7 on key 5 with key 6 copied
  onto it, a keyless marker, a marker on key 7, key 8 on grid slice 2,
  five loop events, then the keyless marker tombstoned (its event dropped).
  The file: 35 entries (the tombstone skipped), `grid` 32, the moved start
  orange, the recording white and owned by 20, the markers orange and
  unowned, entry 35 re-indexed 34, keys `5→33 6→33 7→34 8→2 20→32`, the
  loop's slices re-indexed. Loaded: the grid applied at the swap (orange
  where the file says, white beside), markers committed, keys live, flags
  and provenance back, every live entry identical through the compaction
  map, every key on the same entry, **identical audio** through the
  sequencer, five keys and a keyless call; a same-count re-apply after the
  load keeps the edit, the markers and their keys (the overlay came back
  with the file).
- **The ClearKey regression.** A second recording on key 5, `ClearKey(5)`,
  save: a keyless entry past the material. `PlanSet` accepts it (a08695a's
  headers refuse the same set — reproduced standalone before the fix), and
  so does the old-format text of the same file; loaded, key 5 is on its
  span slice, the orphaned region present but unowned, key 20 on its
  recording, identical audio.
- **Old JSON.** The new file with `grid`, `keys` and every `flags`
  stripped: parses with the defaults, plans to the same grid starts, loads
  into an engine identical to the new file's (nothing orange, no overlay,
  same tables, keys, loop) — identical audio old vs new and source vs old.
- **The sidecar.** Boundary 9 moved, an in-break marker on key 5, key 6 on
  the recording, a keyless loop event on the recording: bounded by
  `bufLen` the file holds the break and its marker, keys on the recording
  default, the recording's event dropped and the marker's re-indexed; a
  plain load with the detector's onsets brings the boundary and the marker
  back, key 5 on the marker, 6 and 20 default, the grid identical to the
  source's. A 16-grid sidecar on a 32-grid material: its starts refused,
  its marker and key back, the grid key defaulted.

`tests/persist_check.cpp` **772 → 785**: the set round trip carries
`flags`, `grid` and `keys` (`CheckSame` compares them); the old-format
text reads with the defaults; clamps (a negative `grid`, `flags` masked,
keys outside −1..127, 28 keys → 24, a count past the entries, a short
list); the OPT8 pin (24 bytes emitted, byte 23 read back, a preset backup
with all 24). `tests/engine_check.cpp` **475 → 478**: the flags argument
(an unchanged boundary the file marks orange is orange and in the overlay,
boundary 0 never, survives a re-apply, nothing new = no gen bump).

`tests/run.sh` ALL GREEN, the Classic hash `c2081c20c9d7fbbf` and every
golden unchanged (`record_check` 53, `clock_check` 65, `scope_check` 96).

Firmware (`make`, `arm-none-eabi-size`): text 376 088 → 381 856
(+5 768 B: the autosave path, the sidecar read / stage, `WriteSetJson`,
`CommitEntry`, the wider `PlanSet` / `SetFromEngine`), data 4 496 → 4 500,
bss 39 608 344 → 39 608 744 (+400 B: the plan's starts, flags and keys in
SDRAM, a few scalars).

## Hand tests (hardware)

1. **An edit survives a set.** Load a set, move a grid boundary and add a
   marker on a key (EDIT-UI, or the console's ACTION ops), wait 2 s: the
   card's `sets/<name>.json` gains `"flags":1` on those entries and the
   key in `"keys"`; the WAV's timestamp does not move. Power-cycle, load
   the set: the boundary is where it was and orange, the key plays the
   marker.
2. **A loose file.** Load a root WAV, move a boundary, wait 2 s:
   `sets/<file>.slices.json` appears. Reload the WAV: the boundary is
   back. Change Bars and reload: the markers come back, the grid is the
   new count with nothing orange.
3. **Off.** Options → Autosave = Off, edit, wait: nothing written. On
   again: written 2 s later.
4. **Failure once.** Pull the card, edit, wait 2 s: the buttons blink red
   once, the state tail shows `setResult` 8; edit again → once more, not
   every 2 s.
5. **Old firmware.** Copy a new set to a card, load it on a build before
   this package: the material and recordings load, the edits do not, no
   "bad file".

## Open questions and what is left

1. **The key table lands one pass after the swap.** Between the audio
   thread's `SetMaterial` + `SetGridBoundaries` and `ServiceSets`'
   `CommitEntry` + `SetKeyTable` the table is the grid alone and the keys
   are the default — as the recordings always were (sample-sets.md, Known).
   Moving the commits into the apply block would close it; `CommitEntry`'s
   `AnalyseRegion` density is the reason it is not there.
2. **Autosave and Save set share `gSet`** (the 9 KB SDRAM `SetInfo`); the
   hook that reads a loaded set runs before the autosave in the same pass,
   so they never overlap — a comment, not a lock.
3. **A re-apply autosaves** (Bars, Swing, …) because `tableGen` is the
   trigger the design names. If Ernest wants only edits to write, EDIT-PROTO's
   mailbox is the place to count them (an "edit generation" beside
   `tableGen`).
4. **Byte 22** is a placeholder here; the integrator merges EDIT-PROTO's
   Tap Allowance into `Serialize` / `Deserialize` / `Describe`. `OPT8` and
   `kBytes = 24` already account for it.
5. **The sidecar ignores `options`**; a loose file's Bars / Swing at save
   time are implied by its `grid` and `swing` only. Good enough for "never
   lose an edit"; a Bars change makes the grid keys default (documented,
   tested).
6. `SetFromEngine` still reads a live table from the control thread;
   EDIT-PROTO refuses edits during a set op, and the autosave does not
   set `setState` — an edit landing during the 30 KB write can tear one
   entry in that JSON. The next generation rewrites it 2 s later.
