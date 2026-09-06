# PLAY package — Options → Fades, Options → Slice Tail

*Branch `pkg/play`, round 2 of 2026-09-06. Owns `Voice` / `TriggerStep` in
`core/bb_engine.h`; shell hooks in `src/audiothurgist.cpp` inside
`/* PLAY */` fences. Lit review recs 3 and 9; keyframe lesson L3.*

## What

Two Options fields that were already in the v2 schema now do something.
Both default to the old behaviour, and in that setting the engine's output
is **byte-identical** to the pre-PLAY engine (host test with golden
checksums).

### Options → Fades

| sel | label | behaviour |
|---|---|---|
| 0 | **Fixed** | as before: 1 ms attack ramp, 5 ms choke release, 5 ms edge guard, linear |
| 1 | **Adaptive (keyframe density)** | per-slice lengths from the slice's keyframe density; ramps bent to ≈ equal-power on cross-slice joins |

Every slice gets a **fade length = 10 keyframes** worth of samples
(`kFadeKeyframes / density`, Nielsen's "leash" measured in keyframes rather
than samples), clamped to **1–10 ms** (48–480 samples: his 1 ms floor;
Driedger & Müller's ~10 ms OLA frame as the ceiling for percussive joins).
Dense material (hats, bright transients) gets short fades, sparse material
(kicks, sustained tails) gets long ones. The length drives three things:

- the **choke release** when a new attack replaces a ringing voice
  (`Voice::chokeLen`, was a fixed 240),
- the **edge guard** ramp (`guardInv`, was 1/240),
- the **attack ramp**: a third of the release, floored at 1 ms
  (`atkInv`; a kick can take 2–3 ms without losing its click, a hat cannot).

Ramp **shape** follows the join: when the voice(s) being choked were not
heading into the incoming slice (their position is outside the 160-sample
continuity window of its start) the join is uncorrelated and both ramps
are bent `x·(2−x)` — a parabola pair whose power sum stays within +0.5 dB
of constant (the sin/cos pair costs trig per sample; this costs one
multiply-add). When the choked voice *was* about to play that slice — a
near-verbatim re-splice that missed continuity by a hair — the ramps stay
linear, which is the right crossfade for time-aligned correlated material.
Continuity joins themselves are not crossfades at all (the voice is
extended) and are untouched.

Densities that are 0 (no analysis) fall back to the fixed lengths, so an
unloaded engine or a test without the lens behaves exactly like Fixed.

### Options → Slice Tail

| sel | label | behaviour |
|---|---|---|
| 0 | **Gate (honest)** | as before: a slice ends at its natural end; the rest of the slot is silence |
| 1 | **Loop forward** | the last part of the slice loops from a zero crossing until the next hit |
| 2 | **Ping-pong** | the same region plays back and forth until the next hit |

Applies only when **the grid slot outlasts the slice**. With varispeed the
rate follows tempo, so a slot always consumes exactly one slice at any
tempo; what makes a slot outlast its slice is **pitch up** (the encoder) —
at +7 semitones a 16th runs out two thirds of the way through its slot and
today's engine gates the remaining third. (Tempo below native on its own
does *not* open gaps in this engine; the field wording "tempo below native"
describes a non-varispeed sampler.) The condition is
`slotSamples × rate − sliceLen > 160` — more than the continuity window to
spare — so a slice that is only fractionally short is still handed to the
continuity branch instead.

The loop region is computed once per slice on first use (cached; reset by
`SetMaterial`): from a **positive-going zero crossing at or after the
slice's midpoint** to **one just before its natural end**, both refined to
sub-sample positions by the same linear interpolation the reader uses, so
the forward join is 0 → 0 with no phase error. The region always ends
before the boundary, so it never plays the next slice's attack — the
pre-echo `sliceExt = 1.0` exists to avoid. Ping-pong reflects at the same
two points (a reflection is value-continuous by construction). Both wraps
also go through the existing `clickComp` bridge: the output continues from
the previous sample and any residual offset decays over ~1 ms, exactly as a
voice steal does. Slices without usable crossings (DC, silence) loop the
plain midpoint..end region under the same bridge; slices shorter than 64
samples of region do not loop.

A tail **always yields**: any voice with a tail is choked by the next
attack (pattern step, ratchet, or Voice call), fading out forward from
where it is, never past the slice's own end. Tail voices are excluded from
the continuity branch, and the 400-sample lookahead grace is not granted to
them, so at native pitch — where the tail never engages — verbatim runs are
still one unbroken voice (tested). Voice-role calls play honestly (no
tail): a looping call under mute would otherwise sustain until the next
call. Reversed steps are not looped.

## Why

Keyframe lesson L3: no fixed crossfade suits both transients and sustains,
and we already compute the per-slice density lens at load, so scaling the
fades by it is the lowest-risk audible win available (rec 3). The tail
policies are Ableton Beats mode's *Transient Loop Mode* (Loop Off / Loop
Forward from a zero crossing / Back-and-Forth) and ReCycle's *Stretch* in
one option beside our honest gate (rec 9). Both are options because Ernest
asked that every behaviour change be one, and because the honest gate is a
sound the instrument has had all along.

## How (the plumbing)

- `Engine::sliceDens[kMaxSlices]` — keyframes per sample per slice. The
  shell computes it with `ComputeDensity()` (the Density lens via
  `bb::ClassifySteps(…, mode 3, …, densOut)`, regardless of the classifier
  actually selected) at load (`ServiceLoad`), at a Bars re-slice, and on the
  two boot paths; `gPendDens[]` sits beside `gPendSal[]` and is copied in the
  `gApply` block. One extra analyzer pass per load on the control thread
  (~35 cycles/sample: tens of ms on a long file).
- `Engine::fadeMode` / `Engine::tailMode` — written per block in `AudioCb`
  from `gFades.sel` / `gTail.sel`.
- `Engine::FadeSamples(s)`, `AttackSamples(s)`, `ApplyFades()`,
  `JoinIsCross()`, `FindLoop()`, `SetTail()`; `Voice::WouldChoke()`,
  `Voice::Choke(bufLen, ep)`; per-voice `atkInv`, `guardInv`, `chokeLen`,
  `epShape`, `tail`, `looping`, `loopA/loopB`.
- Audio-thread cost: the ramps are multiplies by per-voice reciprocals
  (resolved once per trigger); the loop search is ≤ len/2 compares once per
  slice; the wrap is one branch per sample for tail voices only. No
  allocation, no I/O, no new per-sample divisions.

## Tests (`tests/engine_check.cpp`, three new SECTIONs at the end)

1. **Byte-identical**: four renders (Classic chops at native pitch, chops
   at ×1.5, verbatim at ×0.7, Collins rolls at ×1.2) with Fixed + Gate
   match FNV-1a goldens computed from the untouched engine (commit
   `a4cf8ac`) with the same driver. Identical at -O1 sanitized and -O2 on
   x86-64.
2. **Adaptive**: sparse slice 0 (200 Hz) fades longer than dense slice 14
   (900 Hz); lengths stay within 1–10 ms; the *rendered* attack ramp of a
   call from silence matches `AttackSamples`; density 0 keeps the fixed
   length; verbatim still plays as one voice; Collins rolls stay finite.
3. **Tail**: at pitch ×1.5 (a slot 1.5× its slice) Gate leaves the last
   third of every slot silent; Loop forward and Ping-pong keep it sounding
   at the right (repitched) frequency; inside the looped tails no
   sample-to-sample step exceeds the slice's own slope by more than 10 %
   (the seam metric — measured worst 1.07× forward, 1.06× ping-pong, i.e.
   the seams are no steeper than the waveform); never more
   than two voices alive (the fading tail + the new hit); at native pitch
   the tail never engages and continuity still holds a verbatim run as one
   voice; loop points sit on positive-going zero crossings in the slice's
   second half, refined to sub-sample zeros.

A pre-existing observation the tests made visible: the synthetic test
material restarts each slice's sine from 0, and in Gate mode at pitch-up
the 400-sample lookahead grace plays ~8 ms across that boundary before
gating — i.e. today's engine does pre-echo the next slice's attack for the
verbatim-next case when pitched up. Byte-identical, so left alone; the tail
policies never cross the boundary and so avoid it. Worth a listen on real
material at +5 semitones with Fixed/Gate.

## How Ernest tests it on hardware

Load a break with a clear kick and a ride carpet (Amen or Apache), Classic
grammar, chop knob fully clockwise (verbatim).

**Fades, verbatim at native pitch.** Options → Fades: Fixed ↔ Adaptive. You
should hear *nothing* change — verbatim runs are continuity joins and are
not crossfaded. That is the null test.

**Fades, chopped.** Chop knob around 10 o'clock (cells + doubles). A/B
Fixed ↔ Adaptive. Listen at the seams into kicks (longer, smoother joins,
still with the click) and into hats (short, unchanged). Adaptive should
read as slightly less "edge-chattery" on the kick joins and identical on
the hats. If a kick loses its click, the 480-sample cap is too generous for
that break — say so.

**Fades, pitched up.** Encoder +5 to +7 semitones, chopped. Every step now
re-splices (the voice arrives early), so every join is a crossfade; this is
where Adaptive vs Fixed is most audible.

**Tail modes, pitched up.** Verbatim, encoder +7 semitones, Options → Slice
Tail: Gate → Loop forward → Ping-pong. Gate: the last third of every 16th
is a hole (the old sound). Loop forward: the hole fills with the slice's
own tail repeating — ReCycle "Stretch" / Ableton Loop Forward; on a ride
carpet it is nearly seamless, on a long kick tail it stutters at the loop
rate (that *is* the sound; the lit review's "metallic buzz on tonal tails"
warning). Ping-pong: the same, softer, at half the repetition rate. In all
three the next hit must land clean on top — no doubled attacks, no
overhang past a hit.

**Tail modes on a slow tempo.** P2 well below native. With varispeed the
slices stretch to fill the slots, so Loop/Ping-pong should sound identical
to Gate — nothing engages. If you hear a difference at native pitch with
tempo alone, that is a bug: report it. (To hear the tail at a slow tempo,
add pitch-up on the encoder: tempo sets the grid, pitch opens the gap.)

**Gate/Voice interactions.** With a tail mode on, hit the Voice keymap and
the ratchet zones: every call and every roll must choke the loop cleanly.
Hold mute: the loop must stop with the pattern (tails are pattern voices
only; calls do not loop).
