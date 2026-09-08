# PERSIST package — sets and preset backups on the card (2026-09-07, branch `pkg2/persist`)

> **Shipped.** The FatFs half — the shell's to-do list in §5 — landed as the
> SETS package: [sample-sets.md](sample-sets.md). Schema additions made
> there (still `"audiothurgist":1`, all optional on read): `material` (the
> break's length; `len` is the whole WAV, recordings included), `swing`, a
> slice `key` of **−1 for grid entries** (recorded ones carry 0..23), a
> looper event's `slice`; `kSetMaxSlices` is 96 (the engine's whole table)
> and `kSetJsonMax` 32 KiB.
>
> **EDIT-PERSIST** ([slice-edit-persist.md](slice-edit-persist.md)) added,
> still under schema 1: per entry `"flags"` (b0 orange — user-placed or
> user-moved; tombstones are never written and every table index in the
> file is re-indexed to the compacted order), a top-level `"grid"` (the
> grid's entry count; absent = the leading `key −1` entries inside the
> material, an old file's only kind) and `"keys":[24]` (the entry each
> Voice key plays as an index into the file's `slices[]`, −1 = the
> default). `key` on an entry is provenance — the key that recorded it, −1
> for a grid slice or a marker nobody recorded — not the map. Options are
> 24 bytes (`OPT8`: 22 Tap Allowance, 23 Autosave; `bb::kSetOptionsToday`,
> static_asserted against `Options::kBytes` in the shell). A worst-case set
> is 29.5 KB. The same document minus the material, options and looper is
> the loose-file **sidecar** `sets/<wavname>.slices.json` (there).

Phase E of [the sample-mode plan](../sample-mode-plan.md) §4, the pure half.
Ernest: "JSON is cheap, a 20 s WAV is cheap." A **set** is
`sets/<name>.wav` (the material buffer, mono or stereo) + `sets/<name>.json`
(the slice table, bars/tempo, the Options bytes, the looper's events). A
preset save also writes `presets/<slot>.json`, a human-readable backup of
every setting and the sample in use. Load takes the JSON when it sits beside
the WAV, else the grid (today's path).

Everything here is header-only in `core/`, host-tested under ASan/UBSan, no
allocation, no exceptions, no STL, no FatFs: the shell plugs FatFs in
through a two-field sink (§4). Nothing in `src/` changes in this phase.

| File | What |
|---|---|
| `core/bb_wavwrite.h` | `bbwav::Write` — RIFF/WAVE through a `bbwav::Sink`: PCM16 · PCM24 · float32, mono/stereo (planes in → interleaved out), `cue ` chunk, DMA-padded staging |
| `core/bb_json.h` | `bb::JsonOut` (emitter into a caller buffer) · `bb::JsonIn` (pull parser, bounded, skips unknown values) |
| `core/bb_set.h` | `bb::SetInfo` / `bb::PresetInfo` and `bb::SetWriter` · `bb::SetReader` · `bb::PresetBackup` — the two schemas |
| `tests/persist_check.cpp` | 737 checks; `tests/run.sh` runs it under ASan/UBSan |
| `core/wav_loader.h` | one line: the 24-bit sign extension no longer left-shifts a negative `int` (UB; UBSan flagged it the first time the loader ran on the host). Same bits, same result |

## 1. The WAV

Chunk order **fmt → [fact] → [cue] → data**; every size is known before the
first byte goes out, so the sink is written strictly in file order (no
seek-back — `f_write` only). `bbwav::FileSize(spec)` gives the total for a
free-space check.

| Format | fmt tag | bits | `bbwav::Load` reads it | Use |
|---|---|---|---|---|
| `kPcm16` | 1 | 16 | **yes** | the set format (default in `SetWriter::Wav`) |
| `kPcm24` | 1 | 24 | **yes** | if the recording path wants headroom |
| `kFloat32` | 3 (+ `fact`) | 32 | **no** — err 3 | host-side only until the loader grows a float branch (it also folds stereo to mono; Phase A's stereo planes need a two-plane loader — same header, a `planes` argument) |

Samples are clipped to ±1 and rounded; ±full scale reads back as 0.99997 /
−1.0 through the loader's `/32768`. The `cue ` chunk holds one 24-byte cue
point per slice start (`dwName` = 1‥n, `fccChunk` = `data`,
`dwSampleOffset` = the start; the loader reads only the offset). Slice starts
outside `[0, len)` are not written. Cues precede `data`, so a streaming reader
sees them before the audio; the loader walks past `data` too, so
Audacity-style trailing cue chunks still work.

**Loader gotcha kept as-is**: `bbwav::Load` skips `sz − 4 − 24·taken` bytes
after a cue list but has already consumed every entry, so a file with more
cue points than `maxCues` (128 in the shell) desynchronises the chunk walk.
Sets never exceed 64; foreign WAVs with > 128 markers are a pre-existing
hazard, worth a one-line fix (`taken` → `i`) when the loader is next touched.

### DMA and the staging buffer

The writer assembles everything in a caller-lent byte buffer
(≥ `bbwav::kMinStaging` = 64 B; 16 KiB is the loader's size and a good one).
Each fill starts at `staging + bbwav::DmaPad(fileOffset)` — the same formula
as the SDK's `alchemy::SdDmaStagingPad` — and keeps 3 spare bytes, so the
whole-sector part of every `f_write` is word-aligned (SdCard rule 3). The
test proves the byte stream is identical for a 64-byte, a 1000-byte and a
16 KiB staging, and that `DmaPad` aligns for every offset in two sectors.

## 2. The set JSON — `sets/<name>.json`

```json
{"audiothurgist":1,"name":"amen","sr":48000,"stereo":1,"len":96000,"bars":2,"bpm":136.25,
 "slices":[{"start":0,"len":6000,"key":0,"cls":0,"sal":0.8,"hit":1,"name":"kick"}, …],
 "options":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
 "looper":[{"phase":0.25,"key":1,"style":7,"vel":0.9}, …]}
```

| Key | Type | Meaning |
|---|---|---|
| `audiothurgist` | int | schema version, **must be 1** — anything else (or absent) is rejected |
| `name` | string ≤ 31 | display name |
| `sr` | int | sample rate of the WAV (48000) |
| `stereo` | 0/1 (or bool) | two planes in the WAV |
| `len` | int | frames per plane |
| `bars`, `bpm` | int, float | the analysis result at save time |
| `grid` | int | the grid's entry count (EDIT-PERSIST); absent → inferred |
| `slices[]` | | `start`, `len` frames · `key` the pad that recorded it, −1 none · `cls` 0 kick 1 snare 2 hat 3 other · `sal` 0‥1 · `hit` 0/1 · `flags` b0 orange (EDIT-PERSIST) · `name` ≤ 31 |
| `keys[]` | int[24] | the key map, file indices, −1 = default (EDIT-PERSIST) |
| `options[]` | ints 0‥255 | the `Options` block bytes in Serialize order (17 when this was written: class, clock, voice, gate len, gate lvl, grammar, gate mode, bars, rests, gate lanes, fades, tail, swaps, déjà vu, swing, onsets, jacks; **24 today** — 17 key plays, 18 sampling, 19 record source, 20 record, 21 quantize, 22 tap allowance, 23 autosave). Apply through `Options::Deserialize`, which already range-checks each byte |
| `looper[]` | | `phase` bars (unquantized) · `key` · `style` · `vel` |

Capacities (`bb::kSet…`): 64 slices, 256 looper events, 32 option bytes;
the reader consumes and drops anything past them, so a bigger future file
still loads its first 64 slices. Order of keys is free. Unknown keys — at
the top level, inside a slice, inside an event, nested to any depth — are
skipped (forward compatibility: the web app may annotate freely). Numbers
accept sign, fraction and exponent; `hit`/`stereo` accept `true`/`false` or
0/1. Floats are emitted with 6 decimals, trailing zeros trimmed, so a
`phase` survives to ±5e-7.

A worst-case set (64 named slices, 256 events, 32 options) emits 22.3 KB;
`bb::kSetJsonMax` (24 KiB) is the buffer to lend.

## 3. The preset backup — `presets/<slot>.json`

```json
{"audiothurgist":1,"slot":3,"name":"late night","options":[…],
 "pager":[[0,0.1,0.2,0.3,0.4,0.5],[0.6,0.7,0.8,0.9,1,1.1]],"sample":"AMEN_170.WAV"}
```

`pager` is one row of 6 pot values per page (≤ 4 pages; short rows
zero-fill, long rows drop the tail). `sample` is the WAV filename as
`gFiles[]` holds it. Same version rule and tolerance as the set.

This is a *backup* — human-readable, restorable by hand if a flash slot's
schema hash moves (the SDK drops slots whose managed set changed; the JSON
survives). The flash slot stays the live store.

## 4. API in one screen

```cpp
#include "bb_set.h"          // pulls bb_json.h and bb_wavwrite.h

bbwav::Sink sink{ &WriteFn, ctx };            // bool WriteFn(void* ctx, const void* p, size_t n)

// save
bb::SetInfo info = …;                          // fill name, sr, stereo, len, bars, bpm, slices, options, looper
bb::SetWriter::Wav (wavSink,  info, planeL, planeR, staging, sizeof staging /*, bbwav::kPcm16 */);
bb::SetWriter::Json(jsonSink, info, jsonBuf, bb::kSetJsonMax);   // or Json(info, buf, cap, &len) to keep the text
bb::PresetBackup::Write(jsonSink, preset, jsonBuf, bb::kSetJsonMax);

// load
bb::SetInfo in;
if(bb::SetReader::Json(text, nBytes, in)) { /* apply: in.slices[0..in.nSlices), in.options, in.looper */ }
bb::PresetInfo p;
bb::PresetBackup::Read(text, nBytes, p);
```

`bbwav::Write(sink, spec, staging, n)` is the raw writer when a set is not
involved (a recorder dump, a test fixture).

## 5. The shell's to-do list (FatFs side — the next step, not this one)

Everything below is `src/audiothurgist.cpp`, control thread, inside a
`SdCard::BusyGuard`, off the audio thread like `ServiceLoad`.

1. **Placement.** One `alignas(32) ALCHEMY_SDMMC_BSS static FIL` for
   writing (the read side already has its own inside `bbwav::Load`), one
   `alignas(32) ALCHEMY_SDMMC_BSS static uint8_t gWavStage[16384]`, and
   `static char ALCHEMY_SDRAM_BSS gJson[bb::kSetJsonMax]` — the JSON buffer
   is handed to `f_write` whole, so it obeys the same DMA law (SDRAM/AXI, not
   DTCM). `bb::SetInfo` is ~6 KB: `ALCHEMY_SDRAM_BSS` too. `.sdram_bss` /
   `.axi_bss` are NOLOAD — the writer initialises what it uses, but memset
   the `FIL` once at boot as the SDK does for its FATFS.
2. **Directories.** `f_mkdir("0:/sets")` and `f_mkdir("0:/presets")` at
   first save; `FR_EXIST` is success. Both names are 8.3-safe. Set names
   should be too unless `FF_USE_LFN` is on in this build's `ffconf.h` —
   check before letting the web app pick free names.
3. **Sink.** `static bool SdWrite(void* ctx, const void* p, size_t n) {
   UINT bw; return f_write((FIL*)ctx, p, (UINT)n, &bw) == FR_OK && bw == n; }`
   — that is the whole adapter. `bb::Yield()` between fills if the clock
   must stay alive through a 5 MB write (the writer does not call it; add a
   call inside the sink).
4. **Save a set** (from a "Save" action, HostLink or gesture):
   `f_open(FA_WRITE|FA_CREATE_ALWAYS)` the WAV → `SetWriter::Wav` →
   `f_close`; same for the JSON via `SetWriter::Json(sink, …)`. Write the
   WAV first; if it fails, don't leave a JSON pointing at nothing. Fill
   `SetInfo` from the engine: `slices[i] = {engine.slices[i].start, .len,
   key, engine.stepClass[i], engine.sliceSal[i], engine.stepHit[i], name}`,
   `options` from `gOptions.Serialize`, `bars`/`bpm` from the load analysis,
   `looper` from Phase D's event list (schema fixed now, so D and E meet
   without a merge).
5. **Load a set.** In `ServiceLoad`, after `bbwav::Load`, look for
   `<same stem>.json` beside the WAV (`f_stat`); if present, `f_read` it into
   `gJson`, `SetReader::Json` → on success take `in.slices` as the onset
   table. Slice starts are *file* positions: either apply the same
   `ci.trimmedStart` re-base `ServiceLoad` does for cues, or skip
   conditioning for a set (it was saved conditioned). Then push
   `in.options` through `gOptions.Deserialize`, hand `in.looper` to the
   looper. On any failure fall through to today's cue/grid path — the WAV
   alone is still a valid beat.
6. **Preset backup.** In the preset-save path, build a `bb::PresetInfo`
   (slot, `gOptions.Serialize`, `pager.State(page, pot)` for each page,
   `gFiles[gCurFile]`), `PresetBackup::Write` to `0:/presets/<slot>.json`.
   Restore-from-JSON is a manual/HostLink action, not automatic.
7. **Lock + busy.** `sd.Lock("/SETS/NAME.WAV")` while a file is open so the
   browser's FS block refuses to touch it; `BusyGuard` around every f_* run
   so the clock snapshot relaxes its age clamp.
8. **ScanSD.** Today it lists `0:/*.wav` only; add `0:/sets/*.wav` to the
   browser list (prefix the display name, or a second dropdown) and have
   `ServiceLoad` build the path from the entry's directory.
9. **Free space.** `bbwav::FileSize` before opening; `sd.VolumeKib` says
   whether it fits.

## 6. Tests

`tests/persist_check.cpp` (in `tests/run.sh`, ASan + UBSan):

- **WAV**: 7 spec combinations written through a `FILE*` sink → byte-level
  RIFF walk (sizes, order, fmt fields, cue table, clip samples, pad byte) →
  read back with `tests/wav_read.h` **and with the firmware's own
  `bbwav::Load`** — `core/wav_loader.h` compiles on the host behind a
  15-line stdio shim of the five `f_*` calls it makes, so the loader that
  runs on the card is the one that verifies the writer (samples within one
  LSB, cues exact; float32 correctly refused with err 3). Identical streams
  across staging sizes; sink failure, empty spec and undersized staging
  refused; `DmaPad` verified for 2048 offsets.
- **JSON**: emitter shape and float formatting; parser tolerance (unknown
  keys nested four deep, whitespace, `\u` escapes, 0/1 flags, key/value
  truncation); 13 malformed documents; **every prefix** of a real set file
  and of a preset file rejected without over-reading (exact-size heap
  copies under ASan); wrong and missing version rejected, version key in
  any position accepted.
- **Set**: full round trip, a "future" file with extra fields at every
  level, 100 slices → 64 kept, worst-case size under `kSetJsonMax`, WAV +
  JSON through sinks with cues == slice starts through the firmware loader,
  mono set ignores the right plane, out-of-range slice dropped from cues;
  preset backup round trip, truncation, future fields, version 0 rejected.

The three headers also compile for Cortex-M7 with the firmware's flags
(`-fno-exceptions -fno-rtti`): 19 KB of text for the whole package, no
`malloc`/`__cxa`/unwind symbols.
