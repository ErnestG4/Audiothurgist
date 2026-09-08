# RECORD package — sample mode phases B + C (2026-09-07, branch `pkg2/record`)

*Record into a key, and loop-record the break, on top of the SAMPLE engine
contract ([sample-engine.md](sample-engine.md)). Plan: [sample-mode-plan.md](../sample-mode-plan.md)
§3 and §7. Owns `core/bb_record.h`, the `/* RECORD */` fences in
`src/audiothurgist.cpp`, the loader's second plane, and
`tests/record_check.cpp`. Persistence (E) and the web slot view (F) build on
the state this leaves behind.*

## What it does

| | Gesture | Where it lands |
|---|---|---|
| **A key** | Voice map, J5 style note **7 = Record** arms (buttons red). Every key held on J4 records from the record source for as long as its J6 gate stands; release (or the plane filling, or the style leaving 7) ends the take. Style **8 = Clear** puts the key back on its grid slice. Any other style disarms. | past the material in the PLAYING buffer (`Engine::RecArm` at `freePos`); the key plays it |
| **The break** | Options → **Record = Arm**. **B3 tap** or the first **J6** rising edge (Voice map) starts a take into the OTHER buffer. Clocked (a clock on J3): the take is the **Bars** count of bars at the followed tempo × P2's multiplier and stops itself on the bar; J6 edges during it are **slice markers**. Unclocked: B3 or J6 stops it. The take runs the load pipeline and plays. The arm drops after one take. | the load buffer, then the audio thread swaps as for a card load |

Options (bytes 18‥20, `kBytes = 21`, schema `OPT4 → OPT5`; existing bytes
keep their meaning; presets saved under OPT4 re-init):

| Byte | Option | Values | Meaning |
|---|---|---|---|
| 18 | **Sampling** | Mono / Stereo | Mono: the record input is (J1 + J2) / 2 into one plane; loads fold to mono (today's path, byte-identical). Stereo: J1 → L, J2 → R; a stereo WAV keeps its planes; playback renders both planes (`Process(L, R, n)`). Applies now when no key holds a recording (the material is re-applied with / without its R plane — a mono break gets R = L), else at the next load or loop take. |
| 19 | **Record Source** | Inputs / Output | J1/J2, or the module's own post-mix output (what just left J9/J10: reverb, volume, clip) — the resampling trick. Read live by the audio thread. |
| 20 | **Record** | Off / Arm | Arm the loop take. Firmware sets it back to Off after one take; setting Off mid-take stops it. |

LEDs: B1/B2/B3 dim red while armed (either kind), full red during a take.
P5's ring (Perform page) shows the **kit map** — every key whose slot holds
a recording, 24 keys over 16 LEDs: keys 0..7 own LEDs 0..7, keys 8..23 pair
up on LEDs 8..15 (`bb::KeyLed`); the key being recorded is brighter. While
a take runs, P6's ring meters the record input (peak, 0.85 decay per frame).
GET_STATE gains a tail at 176..180: `u8 recFlags` (b0 key armed · b1 key
take · b2 loop armed · b3 loop take · b4 stereo material) and `u32 kitMask`
(bit k = key k holds a recording).

**B3 while Record is armed**: its tap (reslice) and hold (load) are
suppressed; tap = start (or, unclocked, stop) the take. The B2+B3 Settings
chord is unaffected.

## Buffers and room

Each material buffer now has an R plane in SDRAM: `gBufAR` / `gBufBR`
(`kBufMax` = 25 s each) beside `gBufA` / `gBufB`, plus `gStageR` for the
stereo decode. SDRAM 19.8 → 39.0 MB of 64; DTCM 67.4 → 68.5 % (the two
24-sample scratch blocks, the take recorder, a few flags; the marker list
lives in SDRAM). `SetMaterial` always receives `cap = kBufMax`, so the
engine can record past the material.

**Headroom rule**: a load, and a loop take, fill at most **`kLoadSec` = 17 s**
(`kLoadMax`), leaving **`kRecSec` = 8 s** of every plane for takes into keys —
`RecArm` cannot fail for want of room after a load. Files longer than 17 s are
cut at 17 s before conditioning (they were cut to 4 bars anyway). A clocked
loop take longer than 17 s (8 bars under ~113 BPM) is cut at 17 s and
analysed like an unclocked one. When the plane is full a key take stops
itself (`recFull`) and commits what it got.

## Threads

| Thread | Does |
|---|---|
| **audio** (`AudioCb`) | `engine.Process(L, R, n)`; the stereo-aware mix (reverb fed per plane, the scope keeps the mono sum (L+R)/2); when a take is armed or running: `bb::RecordInput` picks the source and Sampling, then `engine.RecFeed(L, R, n)` (a key) and `gTake.Feed(L, R, n)` (the loop) — both return at once when idle — and the block peak for the meter. No I²C, SD, printf or allocation. |
| **control** (`OnPoll`) | The gestures: style 7/8 in the Voice keymap, `RecArm`, the loop take's start / markers / stop, `SetKeySlice` for Clear. Cheap calls only. |
| **control** (main loop, `ServiceRecord`) | Finishing a key take: `RecStop → bb::TrimRegion → bb::AnalyseRegion(classifier mode) → CommitSlice` (or `RecDiscard`). Finishing a loop take: `MarkersToCues → PrepareMaterial` (the load pipeline). Every long loop yields to the clock (`bb::Yield`), as `ServiceLoad` does. |

One honest deviation from "RecFeed and Process are the only sample-touching
calls in the audio thread": the loop take's copy, `bb::TakeRecorder::Feed`,
is a third. The engine's `RecFeed` writes past the material in the engine's
own planes; a loop take goes into the *other* buffer, which no engine owns.
`Feed` has RecFeed's exact shape — O(n), one bounds clamp, no per-sample
branch, control-thread Start/Stop — and is host-tested.

## The load pipeline, factored

`ServiceLoad`'s tail became `PrepareMaterial(len, cues, nCues, stereo,
knownBars)`: condition → onset table → bars → swing → classes → density →
bands → envelope → `gApply`. A card load calls it with the WAV's cues and
`knownBars = 0`; a loop take with its J6 markers as cues and, when clocked,
`knownBars` = the take's bar count. Two things differ for a clocked take:
no trim and no bar cut (`bb::NormalisePlanes`: a silent first 16th is still
the first 16th, the take IS that many bars), and `gCondBars = knownBars` so
Bars = Auto slices it at bars × 16. The table is the markers (plus 0) when
there are ≥ 2 of them — the same code path a WAV's cue chunk takes
(`bb::CuesToOnsets`, the re-base by the head cut, the sort, the 20 ms rule,
factored out of ServiceLoad unchanged) — else the onset detector's grid.

Stereo: `ConditionBuffer` works on L and now reports the head cut on the
prior path too (`trimmedStart` used to be filled only by the bar cut — cues
on the prior path were off by the head trim; fixed) and the gain it applied
(new `ConditionInfo::gain`); `bb::ConditionPlane` gives R the same cut and
gain, bit for bit (tested on both paths).

## Marker timing

A marker is `gTake.pos` at the poll that saw the J6 edge: within one poll
(~1 ms) plus one block (0.5 ms) of the audio position. Edges closer than
10 ms to the previous kept marker are dropped (a bounce). Markers are
take-relative; the first J6 edge *starts* the take, so the downbeat is the
implicit cue at 0.

Take length when clocked: `bb::TakeSamples(bars, gClock.Bpm() × gClkMult,
kSr)` = bars × 16 × (15 / bpm) × sr. The multiplier is folded in so the
take's native tempo equals the tempo the module plays at — a take at ×1 is
then verbatim against the clock. Record loop takes with P2 at ×1.

## `core/bb_record.h`

| Helper | Thread | Contract |
|---|---|---|
| `RecordInput(sampling, a, b, mix, n) → {L, R}` | audio | Mono: (a+b)/2 into `mix`, R = null. Stereo: {a, b}. |
| `BlockPeak(L, R, n)` | audio | max |x| over one or two planes |
| `KeyLed(key)` | any | P5 ring offset for a key (above) |
| `SpanSlice(key, nSlices)` | any | the engine's default key mapping — Clear's target |
| `TakeSamples(bars, bpm, sr)` | control | the clocked take's length; 0 = no auto-stop |
| `MarkersToCues(m, n, len, minGap, cues, max)` | control | take-relative J6 edges → a cue list |
| `CuesToOnsets(cues, n, trimmedStart, len, sr, onsets, max)` | control | the WAV cue path (rewrites `cues` in place) |
| `ConditionPlane(R, ci, len)` | control | R follows ConditionBuffer's cut and gain |
| `NormalisePlanes(L, R, len)` | control | one gain, peak → 0.95, no trim; returns the gain |
| `TakeRecorder` | Start/Stop control, Feed audio | the loop take's copier (above); `pos`, `running`, `peak` are what the control thread reads |

`bbwav::Load(…, err, outR = nullptr, stageR = nullptr, gotR = nullptr)`:
give both planes to keep a stereo file's L and R apart (`*gotR = true`); a
mono file, or no planes, takes the old fold, byte-identical (tested). Also
fixed while here: the cue-chunk walk now skips the bytes of every record
read, not only the kept ones (the persist doc's "loader gotcha" — a file
with more than `maxCues` markers no longer desynchronises the chunk walk).

## Tests (`tests/record_check.cpp`, in `tests/run.sh`, ASan/UBSan — 53 checks)

Record input per Sampling; block peak; the kit map (every LED one or two
keys, clamps); `SpanSlice == Engine::KeySlice` on 16 and 64 slices and Clear
after a take; take length at 60 / 120 / 170 BPM and a 2-bar take reading
120 native; markers → cues (bounce, range, cap) → onsets (re-base, drop,
sort, the 20 ms rule, cap, the in-place rewrite); the R plane bit-identical
to L through ConditionBuffer on the prior path and on the synth break
(bar-cut path), `trimmedStart` on the prior path, `gain`; NormalisePlanes;
TakeRecorder (exact stop mid-block, nothing past it, the R copy, cap, Stop,
peak, target clamp); the pipeline on a synthetic 2-bar take with a silent
first 16th and three jittered markers — clocked (nothing moves, the markers
are the hits at steps 4/8/12, 32 slices, 120 BPM) and unclocked (the head
cut is the silent 16th, markers re-base, R mirrors L); the loader with two
planes (within one LSB, the fold == (L+R)/2, mono file → no R and identical
samples, a 5-cue file read with `maxCues` 2). Classic hash unchanged;
`engine_check` goldens hold (`Process(L, R)` on mono material is
byte-identical to `Process(out)`).

## Hand tests

`docs/morning-checklist-2.md` §6. The gestures themselves — the style
notes, the B3/J6 start, the auto-stop on the bar, the LEDs, the meter — are
hardware.

## Known / deferred

- Sampling = Stereo with a key already recorded applies at the next load or
  loop take (the engine keeps recordings only on the same planes). A mono
  break under Stereo renders both planes (R = L): every voice reads twice.
  Watch `audioUs` in the web diag pill with Stereo on; the budget is 450 µs.
- A take into a key on mono material under Sampling = Stereo records mono
  (`RecFeed` ignores R on mono material) — the option is best set before
  the load.
- J6 is an output in the Brain map, so the J6 gestures (a key take's gate,
  the loop take's start / markers / stop) need the Voice map (Jacks →
  Voice, or Grammar → Play under Follow grammar). B3 starts and stops a
  loop take on either map.
- A stereo *file* longer than 12.5 s at 96 kHz native: the R staging is
  sized like the L one (`kStageMax`), so nothing changes; the 17 s cap
  applies at the output rate.
- `gCurFile` is −1 after a loop take (GET_STATE 255): the material is not
  a card file. Phase E's set save is where a take gets a name.
