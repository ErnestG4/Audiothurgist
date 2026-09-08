# SETS package — sets and preset backups on the card, the FatFs half (2026-09-07, branch `pkg2/sets`)

*The last piece of sample-mode persistence: [sample-persist.md](sample-persist.md)
built the formats (`core/bb_wavwrite.h`, `bb_json.h`, `bb_set.h`) and left
a nine-item to-do list for the shell; this package does the list. Plan:
[sample-mode-plan.md](../sample-mode-plan.md) §4 — Ernest: sets =
`sets/<name>.wav` + `sets/<name>.json`, preset backups `presets/<slot>.json`,
"JSON is cheap, a 20 s WAV is cheap". Owns `core/bb_sets.h`, the
`/* SETS */` fences in `src/audiothurgist.cpp`, `Looper::Adopt`, the schema
additions in `bb_set.h`, `tests/sets_check.cpp`, the Kit tab's set card.*

## What it does

| | Where | What |
|---|---|---|
| **Save a set** | Kit tab → Set → name + **Save set** (ACTION 14) | `sets/<name>.wav`: PCM16, the whole used plane (the break, then the recordings, = `freePos`), both planes when the material is stereo, a `cue ` chunk at every table start. `sets/<name>.json`: the table (grid entries with `key −1`, recorded ones with their key; class, salience, hit), the break's length (`material`) and swing, bars/bpm, the 22 Options bytes, the looper's events with `slice`. The WAV goes first; if it fails nothing is left; if only the JSON fails the WAV stays (a plain break is a valid load). |
| **Load a set** | P5 + B3 hold on a "name [set]" entry · the Sample dropdown · ACTION 6 on that entry · Kit tab **Load set** (ACTION 15, by name) | WAV through the existing loader (no 17 s cap, no conditioning — a set was saved conditioned, its positions are the buffer's) into the other buffer, both planes when the file is stereo **and** Options → Sampling = Stereo. Then the JSON: options through `Options::Deserialize` (the blob-write path, so the dirty flags fire — minus Bars / Swing / Onsets / Sampling / Classifier, the set's table *is* that re-slice; Record forced Off), the grid rebuilt by `SetMaterial` from the set's count, swing and hit onsets with the file's classes and salience, the recorded entries through `CommitSlice` after the swap, the looper adopted by the audio thread at the swap. A WAV in `sets/` with no JSON beside it is a plain load. |
| **Preset backups** | every preset save — the Settings gesture, a host `PRESET_SAVE`, an import | `presets/<slot>.json` (`bb::PresetBackup`): the slot's own bytes (not the live state) — options, the pager's 12 pot values, the sample entry, the name. A slot erased from the browser loses its backup too. |
| **Restore at boot** | after `presets.BootLoad()`, before audio | every slot the flash no longer honours (empty, or its schema hash moved with a firmware's managed set — "presets re-init on OPT bumps") whose JSON exists is applied to the live state and **saved back** into flash, so the slot is valid again under the new schema; slot 0 is re-loaded afterwards so what plays is the boot preset. Locks and Settings (brightness) are not in the backup: a restored slot carries the live ones. |

### On the card

`sets/` and `presets/` are **created automatically** at the first save
(`f_mkdir`, `FR_EXIST` is fine); nothing to prepare beyond a FAT32 card.
The browser's FS block sees both directories as ordinary directories.
Names are 1..31 of `[A-Za-z0-9_-]` (the web field enforces it; the
firmware refuses anything else with BAD_ARGS). FatFs here has LFN on, so
names past 8.3 are fine; a foreign WAV dropped into `sets/` by hand with a
name outside that rule is listed as `sets/<file>` verbatim and loads as a
plain file.

### The list

`ScanSD` lists the root's loose WAVs, sorted, then `sets/*.wav`, sorted,
as the entries `sets/<name>.wav`; the dropdown and P5 show a set as
**"name [set]"** (`bb::SetLabel`). `ServiceLoad` routes any `sets/` entry
through `LoadSet`. A set saved this session is **appended** to the list
(indices a host or a preset already holds stay valid; the next boot sorts
it in); the browser's dropdown is rendered at boot, so it shows the new
set at the next connect, while the Kit tab's name list learns it at once.
A preset's `sample` field stores the entry (`sets/amen.wav`), so a preset
that pointed at a set finds it again.

### LEDs and refusals

While a set saves, P5's ring fills with the progress (B3 in the set
colour); while one loads, a dot sweeps the ring. A refused or failed op
blinks B1/B2/B3 red for about a second and a half. Refused: no card
(mount fails), card full (FatFs's cached free count, when it has one, or a
short write), a take armed or running (save), a load already waiting, a
bad name; a load also reports "not found" (no WAV) and "bad file"
(unreadable, a JSON the reader rejects, a rate other than 48 kHz, a table
`PlanSet` refuses, a WAV cut short of what its JSON describes).

### Threads and time

Everything runs on the control thread from the main loop (`ServiceSets`,
`ServiceLoad`) under a `SdCard::BusyGuard`, nothing on the audio thread but
two whole-struct copies of the looper (`Looper::Adopt`, 4 KB): the save's
snapshot (taken between two blocks, so `Insert`'s memmove is never read
half-way) and the load's adoption right after `SetMaterial`. The write
sink and the loader call `bb::Yield()` per 16 KiB; through a set op
`KeepClock` also runs the **whole control loop** every 40 ms (`loop.Tick()`,
guarded against re-entry), so the panel, the LEDs and HostLink keep
answering during a 5 MB write. A 25 s stereo set (4.8 MB) saves in
roughly two seconds.

DMA law: the writer's `FIL` and its 16 KiB staging in `.axi_bss`
(32-aligned), the 32 KiB JSON text, the `SetInfo` (9 KB), the looper
staging and the plan in SDRAM; the SDRAM objects with default member
initialisers are placement-new'd at boot (NOLOAD sections see no
constructors). DTCM grew by 0.6 KB.

### Hardware gesture

Settings' page 0 has pots 1, 4 and 5 free, but every declarative control
there is a byte of `Settings::Serialize` — adding one moves the Settings
schema hash and re-inits every preset slot on the very flash this package
is protecting (the backups would bring them back, but only once a backup
exists, and this is the first firmware that writes one). No other free
chord: B1 is the pager and the lock recorder, B2/B3 hold and tap are
taken, B2+B3 is Settings. **Save is browser-only for now** (the Kit tab);
load has the panel path (P5 + B3 hold). Revisit once a backup has been on
every card for a release.

## Protocol

```
0x63 ACTION 14  save set   ← u8 14, name bytes (1..31 of [A-Za-z0-9_-])  → u8 status
0x63 ACTION 15  load set   ← u8 15, name bytes                            → u8 status
```
Both answer at once: BAD_ARGS for a bad name, BAD_STATE while an op runs,
a load waits or (save) a take is armed / running. The work happens in the
main loop; the state tail tells:

```
u8 setState u8 setProgress u8 setResult u8 setSeq   (219..222)
char setName[32]                                    (223..254)
```
`setState` 0 idle · 1 saving · 2 loading; `setProgress` 0..255 of a save;
`setResult` of the last op: 0 ok · 1 no card · 2 card full · 3 write
failed · 4 bad name · 5 not found · 6 busy · 7 bad file; `setSeq` bumps
when an op finishes (the page shows each result once); `setName` = the set
the playing material came from (empty: a loose file or a take). The
descriptor advertises `"sets":1`.

## `core/bb_sets.h` (pure, host-tested)

| | |
|---|---|
| `SetNameOk` · `IsSetEntry` · `SetStem` · `SetEntry` · `SetLabel` · `SetPaths` · `SetLockPath` · `PresetPath` · `FindEntry` | the naming: name ↔ entry ↔ label ↔ card paths, case-insensitive lookup |
| `SetFromEngine(engine, looperSnapshot, info, stereo, sr)` | table, keys, classes, salience, hits, `material`, `swing`, the events |
| `PlanSet(info, plan)` | → `nGrid`, `material`, hit onsets, classes, salience, the validated recorded entries; refuses a malformed set (no grid, out-of-order grid, a recording inside the break or past the file or under `kRecMin`, a bad or duplicate key) |
| `CommitSetRecordings(engine, info, plan, densFn)` | the entries through `CommitSlice` after `SetMaterial`; `densFn` = `bb::AnalyseRegion` on the plane (density is not in the file) |
| `LooperFromSet(info, looper)` | `Clear` + `Add` each event + `Seal` — for `Looper::Adopt` |
| `MergeOptions(info, live, out, n)` | the set's bytes, the live ones past its count |

`tests/sets_check.cpp` (70 checks, ASan/UBSan, in `tests/run.sh`): the
naming; a real engine with a swung 32-step grid, two recorded keys and
three looper events → `SetFromEngine` → JSON → `SetReader` → `PlanSet` →
a second engine whose table, keys, hits, classes, salience, density, looper
**and rendered audio** (the sequencer, a held call on a recorded key, a
one-shot on the other) are identical; every `PlanSet` refusal;
`MergeOptions`; `Adopt` (a full list, generation and dirty carried, window
and held mask reset); `LooperFromSet` dropping what cannot fire.
`persist_check` covers the schema additions (772 checks). The web selftest
(`node web/selftest.mjs`, 111 checks) pins the tail parsing, the name
rules and the demo's save / load / refusal flow.

## Hand tests (hardware)

Card in, module booted, browser connected (`web/index.html`), Kit tab.

1. **Save.** Load a break (P5 + B3). In the Voice map (Options → Jacks →
   Voice, J5 style 7) record two keys, say C1 and G1; in Play (Grammar →
   Play, Quantize Off) play a few hits so the looper card counts events.
   Set Options → Fades = Adaptive and Bars = 2 bars (anything not default).
   Type `kit_1` in the Set card, **Save set**: P5's ring fills, the pill
   says "saving kit_1 … %", then "saved kit_1"; "material: set kit_1". On
   the card (browser FS block or a reader): `sets/kit_1.wav` (≈ 2.9 MB for
   a 17 s + takes mono material), `sets/kit_1.json` (open it: `"material"`,
   `"slices"` with `"key":-1` then `"key":0` and `"key":7`, `"options"` 22
   ints, `"looper"` your events).
2. **Power-cycle**, reconnect. The Sample dropdown and P5 list end with
   "kit_1 [set]". Load it (dropdown, or P5 + B3 hold: a dot sweeps P5's
   ring). Check: the key cells show C1 and G1 recorded with the same
   lengths and classes; P5's kit map lights the same LEDs; Options show
   Fades = Adaptive, Bars = 2 bars (the Options tab refreshes from the
   state); the looper card shows the same count and, in Play, the loop
   plays as recorded; the grid readout says the same bars/steps; the Play
   tab's slice boundaries match (swing included).
3. **Load set by name.** Type `kit_1`, **Load set**: same result;
   `nope` → "loaded nope: not found", the buttons blink red.
4. **Refusals.** Pull the card, Save → "no card", red blink. Arm a take
   (● on a key), Save → the button is disabled; ACTION 14 from a console
   → BAD_STATE. A name with a space is refused by the field.
5. **Stereo.** Options → Sampling = Stereo, load a stereo WAV, record a key
   from J1/J2, save `st_1`; reload: the buffer card says "stereo (L · R)",
   the L/R picker on the waveform shows two different planes for the take.
   Set Sampling = Mono and load `st_1` again: mono, folded.
6. **A WAV without a JSON.** Copy any WAV into `sets/` as `plain.wav`:
   listed as "plain [set]", loads as a plain break (grid from the onset
   detector, "material: a loose file").
7. **Preset backup.** Save preset 3 from Settings (B2+B3 2 s, P3/P4 gesture)
   or from `/program`. Within a second `presets/3.json` appears; open it:
   `"slot":3`, the options, two rows of six pot values, `"sample"` = the
   list entry (`sets/kit_1.wav` if the set was playing), `"name"`. Rename
   the preset in the browser and save again: the JSON follows. Delete the
   slot in the browser: the JSON goes.
8. **Restore after a schema bump.** Save presets 0 and 3 with distinctive
   options. Flash a build with a changed Options schema (e.g. bump
   `SchemaHash` to `'OPT7'` locally) — the SDK drops both slots. Boot: the
   card's `presets/0.json` and `3.json` are applied and saved back; the
   browser's preset list shows both with their names; the options of slot
   0 are live; `presets/0.json` unchanged. Flash the real build again: the
   slots are dropped once more and restored once more.
9. **Liveness.** Start a save of a long stereo set and, during the two
   seconds it takes, turn P1 and tap B2: the ring animation and the dice
   still answer; the web diag pill shows no timeouts; audio never glitches
   (the clock snapshot relaxes its age clamp under `BusyGuard`).

## Known / deferred

- The dropdown in the browser is rendered at boot; a set saved this session
  is in the firmware's list (P5, ACTION 6, presets) but appears in the
  dropdown at the next connect. The Kit tab's name list is live.
- A set's recorded entries are committed one main-loop pass after the swap;
  for that pass the table is the grid alone (the page refetches when
  `nTable` moves).
- `bands` (multi-label gate lanes) and `density` are recomputed from the
  audio on load, not stored; classes and salience come from the file.
- A save while Play's looper is being overdubbed snapshots the loop as it
  stands at that block.
- Save has no panel gesture (above).
