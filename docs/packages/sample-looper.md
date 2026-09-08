# SAMPLE package, phase D — the unquantized looper

*Branch `pkg2/looper`, 2026-09-07. Owns `core/bb_looper.h`, the `/* LOOPER */`
fences in `core/bb_engine.h` and `src/audiothurgist.cpp`, the LOOPER sections
of `tests/engine_check.cpp`, and Options byte 21. Plan: `docs/sample-mode-plan.md`
§2.4. Builds on phase A's contract (`docs/packages/sample-engine.md`: CallSlice
with `held`/`key`, ReleaseKey, thread ownership).*

## What it is

Ernest: "when I think of play mode I think of someone slapping pads on an Akai
and it's completely up to them to keep beat." **Pads** (grammar 6) is that with
no looper. **Play** (grammar 5) was the 16th-quantized looper: a call punches
the current step. This phase puts the free looper between them, and makes it
Play's default:

- **Quantize = Off** (default): every call in Play is recorded at the phase it
  was played and replays there, sample-exact, every pass. Held notes (Key
  Plays = Held) record their release too, so they replay with their length.
  Overdub is the only mode — recording never stops while you are in Play.
- **Quantize = 8ths**: the same looper, the phase snapped to the nearest
  pair of steps at record time.
- **Quantize = 16ths**: Play exactly as before — the call punches its step;
  the looper neither records nor fires. Byte-identical (the PLAY goldens and
  the Play test hold with `looperQuant = 1`).

The looper is **active when `patternMode == 5 && looperQuant != 1`**. Its
events survive a grammar change (silent until Play again) and a re-slice of
the same material (phases are bar-relative; they rescale with the bar); a
load (new planes) empties it, as does B3.

## Engine (`core/bb_looper.h`, `bb::Looper`, owned as `Engine::looper`)

An event list, ≤ 256, **sorted by phase on insert** (a memmove of ≤ 255
16-byte entries: O(events) on the audio thread, no allocation):

```
Event { double phase;  // 0..1 of the loop
        float  vel;    // reserved (1.0): the Voice map has no velocity in
        int8   key;    // Voice key 0..23, or -1 for a keyless call (Brain map, web)
        int8   slice;  // the table entry called — the replay target when key < 0
        uint8  style;  // bits: 1 rev · 2 half · 4 double · 8 ratchet · 16 staccato
                       //       32 held (a release follows) · 64 release · 128 skip (transient)
        uint8  gen; }  // generation: the undo unit
```

`style & 0x7F` is the wire byte of `bb_set.h`'s `looper[].style`; `phase`,
`key`, `style`, `vel` match that schema field for field. `slice` is the
one addition: a keyed event resolves its slice **through the key table at
replay time** (re-record a key, re-point it, re-slice the bar — the loop
follows, as a sampler's sequence does), a keyless one plays the entry it was
called with. Phase E's loader should `Add` events from the JSON and, for
keyless ones, carry the slice (a `slice` field, or map it onto a key).

| Call | Thread | Contract |
|---|---|---|
| `bool Record(phase, key, slice, style, vel)` | audio | Append a live call. Quantized per `quant` (0 exact, 1 nearest of `divisions`, 2 nearest of `divisions / 2`; 1.0 wraps to 0 — a late hit lands on the next downbeat). Marked **skip** when it lies ahead of the playhead (within the next half loop): the pass the player is hearing does not replay it — it first sounds on the following pass, exactly as a punched step does. false = full (the call still sounded). A `kHeld` event arms its key for the release. |
| `bool RecordRelease(phase, key)` | audio | The key came up: a `kRelease` event, only if a held note-on for that key is recorded and unreleased (`heldMask`). false otherwise, or when full — the replayed note then holds to the material's end. |
| `bool Add(phase, key, slice, style, vel)` / `void Seal()` | control, audio stopped | The loader's entry: no quantize, no skip, into the current generation; `Seal` closes it so the first live overdub is its own undo unit. |
| `bool UndoLast()` | either | Remove the newest generation (the highest `gen` present); the next overdub takes its number. false = empty. Clears `heldMask`. |
| `void Clear()` | either | Empty; generation 0. |
| `void Advance(phase, span, nSamp, Window&)` | audio, every block | The block's window over the loop (below) and the wrap detector: a wrap with something recorded since the last one closes the generation (`gen++`; the byte wraps by renumbering from the oldest generation present, merging the two oldest only when all 256 exist). |
| `int Offset(Window, phase, shift, nSamp)` | audio | The event's sample inside the block: nearest sample, clamped. |
| `static double Snap(phase, div)` | either | The quantizer. |

**The window.** A block at `phase` with extent `span` covers
`[from, phase + span − ½ sample)`. `from` is where the previous window ended
(the *frontier*), reconciled with the shell's phase: within ±4 blocks of
jitter or up to half a loop ahead it stays the frontier (nothing fires twice,
nothing is skipped — a late event fires at offset 0); a wrap that fell
between two windows (frontier near 1, phase near 0) is stitched; anything
else is a jump and re-anchors at `phase`. The half-sample bound means an
event nearer to the next block's first sample rolls into that block instead
of being clamped a sample early. Up to three index ranges come back, in time
order: just-before-the-wrap (phase − 1), the plain span, past-the-wrap
(phase + 1). Sorted events make each bound a binary search.

**Firing (`Engine::ProcessT`).** After the step scheduler and ratchets,
`looper.Advance` runs every block whatever the grammar (the generation
clock). While active, the block renders in **segments** split at each
event's offset: `RenderSpan(seg, off)` for every voice sounding at the
block's start or fired within it, then `LooperFire(e)`, and so on to `n`.
With no events the single segment `[0, n)` is the old voice loop op for op —
the four PLAY goldens, the SAMPLE golden (`786e423d477968a2`) and the Classic
hash are unchanged. A voice that ends inside a segment keeps rendering its
click-decay to the block's end, as before (the `on[]` mask is taken at the
block's start), so the pre-LOOPER block-alignment of that decay is kept.

**Sample-exact:** yes — the voice starts at the event's sample (the test
renders two events one sample apart and gets the same samples shifted by
one; a 7-sample and a 24-sample block give identical onsets across the
wrap). Not block-exact. The *record* side is block-exact by nature: the
shell drains a call at the previous block's phase (the gate itself is
polled at 1 ms).

`LooperFire` goes through **`CallSliceRaw` / `ReleaseKeyRaw`** — the live
call path minus the recording — with `punch = false`: the looper is the
memory, no step is written. A ratcheted event schedules its double exactly
as a live ratchet does (single-slot, block-exact, as today). A held event's
replay voice is released by its recorded release; a live release of the
same key releases it too (ReleaseKey is by key).

**Engine glue.** `looperQuant` (0/1/2, the shell writes it per block),
`LooperActive()`, `LooperStyle(rateMul, rev, ratchet, lenMul, held)` (half
< 0.75, double > 1.5, staccato < 0.999 — the style bits are what replay, so a
Brain-map half×double = plain). `CallSlice`: when active and `punch` (the
shell's "write" — style 6 in Play flips it to a choke, as before), record at
`barPhase` then call with `punch = false`. `ReleaseKey`: record the release,
then release. `Reslice()` (B3) clears the loop with the punch bar in Play.
`SetMaterial`: `divisions = nSlices`, frontier reset; new planes clear.

## Shell hooks (`/* LOOPER */` fences in `src/audiothurgist.cpp`)

- **Options byte 21 "Quantize"**: Off (as played) / 16ths (punch the bar) /
  8ths — `gQuantize`, `Put(…, 2)`, `Describe` field 21 (`quantize`).
  `kBytes = 22`, schema `OPT4 → OPT6` (bytes 18..20 are written as 0 here
  and belong to the record phase: Sampling, Record Source, Record — the
  merge takes theirs). Presets saved under OPT4 re-init.
- **AudioCb**: `engine.looperQuant = gQuantize.sel` per block; `gPendUndo`
  drained into `engine.looper.UndoLast()`.
- **B2 tap in Play** = undo the last pass's overdub (`gPendUndo`; dice
  means nothing in Play). Hold = flow, unchanged. Other grammars: dice.
- **B3 tap in Play** = clear (through `engine.Reslice()`, which already
  cleared the punch bar). The web's ACTION 0 (dice) still rerolls in Play;
  mapping it to undo is the web phase's call.
- **State tail byte 176**: `u8 looperEvents` (count, capped 255) after the
  jack map (175); `docs/round-plan-2026-09-06.md`'s table updated. Drawing
  the events on the bar strip is the web phase.
- **Manual**: the Play sentence in *languages* (Quantize, B2 undo, B3
  clear), the keymap's B2/B3 parentheticals, a Quantize entry in *options*.

## Tests (`tests/engine_check.cpp`, LOOPER sections; `tests/run.sh`)

A call in Play writes the loop, not a step; skipped on the pass being played,
fired on the next two; an unwritten (choke) call leaves it alone. Sample-exact:
two renders one sample apart are the same samples shifted by one. A held note
replays with its release, stops there, no held voice outlives it; re-pointing
the key changes what replays. Quantize 16ths: nothing recorded, the step is
punched, a punched call and a web punch render the same second pass; 8ths
snaps 0.3 → 0.25 and 0.98 → the downbeat, sorted, styles kept. Undo removes
only the last generation, a pass with nothing recorded opens none, the freed
generation is reused, an empty loop refuses; 256 sealed generations still
peel one at a time. Clear: Reslice in Play, `Clear()`, Classic punches
instead, Pads writes nothing, the loop waits through another grammar, a
re-slice keeps it, new material drops it. Wrap: events 96 samples before and
48 after the loop end fire every pass at their exact samples, at 24-sample
blocks (wrap on a block boundary) and 7-sample blocks (inside one), with
identical onsets. The 256-event cap refuses `Record`, `Add` and a release,
the call still sounds, the list stays sorted, a full loop renders finite and
fires every event. Determinism: the same gestures render the same bits. The
pre-existing Play test pins `looperQuant = 1` (the punch path); all goldens
and the Classic hash hold; `make` is green.

## Invariants

1. Events are sorted by phase; a note-on precedes a release at the same phase.
2. `0 ≤ phase < 1`; a quantized 1.0 is 0.
3. Generations are monotone in recording order; `UndoLast` removes exactly
   the highest generation present.
4. With `looper.n == 0` or the looper inactive, `ProcessT` is op-for-op the
   pre-LOOPER render.
5. Nothing on the audio thread allocates; per block: one `Advance` (two to
   four binary searches), a scan of the events in the window, one memmove
   per `Record`.
