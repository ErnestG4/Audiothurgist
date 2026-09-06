# GATES package — multi-label lanes, accent velocity, the hold/retrigger law

Branch `pkg/gates`. Owns the gate arming block in `AudioCb`, the gate CV
writes in `OnPoll`, the band-mask output of `core/bb_classify.h`, the new
`core/bb_gates.h`, and the GATES rows of `tests/engine_check.cpp`. Shell
hooks in `src/audiothurgist.cpp` are inside `/* GATES */` fences.

J6 / J7 / J8 are the kick / snare / hat gate outputs. J6 is MCP4728 channel
D (I²C through the PCA9557 LDAC pulse); J7 and J8 are the STM32 DAC pair.
The law that keeps the module alive is unchanged: the audio callback only
ARMS per-lane countdowns from `Engine::gateFired`; `OnPoll` (the SDK's 1 ms
control tick) writes `SetVolts` on change only. Nothing here touches I²C
from the audio thread.

## What ships (all behind existing Options fields)

| Option | Values | What this package does with it |
|---|---|---|
| Gate Lanes (`gate.lanes`, 9) | **Dominant class** (default) / **Multi-label** | Dominant = v0.8.0, byte-identical (gates and audio, host-tested). Multi-label raises every lane whose band the classifier marked present on the step. |
| Gate Level (`gate.lvl`, 4) | Fixed 5 V / Velocity | Velocity = 1.5 + 3.5 × salience as before; a grammar **accent** on the step lifts that hit to the 5 V ceiling. Accents come from `Engine::stepAccent` (Grids level > 192 once GRAM lands; all-false until then, which is simply "no accents"). |
| Gate Length (`gate.len`, 3) | 6 / 20 / 50 ms / Hold to next hit | Reviewed and fixed (below). |
| Gate Fires (`gate.mode`, 6) | Every step / Onsets only | Unchanged (v0.8.0); Multi-label respects it exactly as Dominant does. |

Audio never changes with any of these — the host suite checks that the
render is identical with the lanes on either setting.

## The band rule (`bb_classify.h`, `bandsOut`)

`ClassifySteps` gained an optional `uint8_t* bandsOut`. For each step it
returns a 3-bit mask, bit c = class c present (0 kick, 1 snare, 2 hat):

1. Three **second-order** bands at the Hockman edges, computed in their
   own pass so the classification itself stays byte-identical:
   low = LP²(325 Hz), high = HP²(4 kHz), mid = LP²(4 kHz) of HP²(325 Hz).
   The classifier's first-order subtractive split is fine for an arg-max
   but leaks a 100 Hz kick into the mid band at −10 dB (6 dB/oct); two
   poles make it −21 dB. (Note for the next person: `x − LP²(x)` is NOT a
   second-order high-pass — it is `(2s+s²)/(1+s)²`, first order with twice
   the leakage. HP² must cascade `x − LP(x)` twice.)
2. Band c is present when **both**
   - `e_c ≥ gBandTheta × mean_c` — at least half that band's loop mean
     (`gBandTheta = 0.5`, sweepable from a host harness). The loop mean is
     duty × average hit, so a rare class (snares on 2 and 4) is judged
     more leniently than a hat carpet — that is what lets a ghost snare
     mark the snare lane.
   - `e_c ≥ kBandFloor × e_max` — within −20 dB of the step's loudest band
     (`kBandFloor = 0.01`): the class is a real part of this hit, not
     crossover leakage. Measured on the suite's drums: a 60 Hz kick
     reaches the mid band at −27 dB and the high band at −66 dB; a
     band-limited hat reaches the low band at −35 dB; a hat riding the
     kick sits at −16 dB and passes.
3. The loudest band always counts, so a sounding step marks at least one
   lane and Multi-label is a superset of the Hockman lens. Silent steps
   (the classifier's −50 dB rule) mark nothing.
4. **Lens-independent.** The mask uses the Hockman edges under every
   Classifier (the suite checks all four lenses give the same mask). The
   lens still labels the ring and drives swaps; with Multi-label on, the
   Classifier option no longer changes which lanes fire.

Known cross-talk (Wu et al. 2018's caveat for band-pass + threshold): a
snare's noise is bright, so most snare steps also mark the hat lane, and a
kick with a strong beater click may mark the snare lane. That is the rule
reading the drum rather than the label. If you want one lane per hit, use
Dominant class. Both constants are first guesses for ears; tune them on the
canon breaks with `tests/classify_check.cpp` (add `bandsOut` printing) and
the 1,230-break corpus when it is on disk.

Plumbing: shell `ClassifySteps(...)` wrapper takes a trailing `bands`
pointer; load (`ServiceLoad`) and the Bars re-slice compute into
`gPendBands`, copied to `engine.stepBands` in the `gApply` block next to
`gSal`; boot writes `engine.stepBands` directly (audio not started). A
Classifier switch (reclass) leaves the mask alone — it does not depend on
the lens. `Engine::RaiseGates(idx, accent)` is the one place that decides
WHICH lanes a slice raises (Dominant: `1 << stepClass`; Multi-label:
`stepBands[idx]`, falling back to the dominant class when the mask is 0);
`TriggerStep` and `FireSlice` still decide WHETHER a gate is due
(continuations never, onsets-only needs `stepHit`).

## The lane law (`core/bb_gates.h`)

Pure, host-tested, used verbatim by the shell:

- `GateVolts(velocity, sal, accent)` — 5 V fixed; velocity 1.5–5 V from
  salience; accent → 5 V. The Density lens's per-step density is
  deliberately not folded into velocity: it only exists under that lens,
  and velocity must not change with the Classifier.
- `GateArm(cd, lvl, retrig, fired, lenSamps, volts, n)` — audio side, per
  block per lane.
- `GatePollVolts(cd, lvl, retrig)` — poll side; consumes a retrigger.

### Hold / retrigger review (field wish: "each gate hits once at the open, as drums do")

What was right: with Gate Fires = Onsets only, `TriggerStep` raises a lane
only when the slice opens with a detected onset and is not a continuation,
so a held lane retriggers only on a real hit. The one-poll-low retrigger
(flag set in audio, consumed by the next poll, level rewritten the poll
after) is the robust shape: a timed low window could be missed entirely if
the control thread stalled through it, the flag cannot.

Two things were wrong and are fixed:

1. **Timed lengths did not retrigger.** A re-hit while a 20/50 ms gate was
   still high just extended it (`cd = len`), so a Collins roll or a 32nd
   ratchet inside the gate was invisible to an envelope. Now every length
   mode retriggers on a re-hit while high: one poll low, then up at the new
   level with a fresh countdown.
2. **Leaving Hold left lanes stuck.** Hold parks `cd` at the 2^28-sample
   sentinel; switching to a timed length decremented from there — a lane
   with no further hits (a snare lane on a kick-only pattern) stayed high
   for 93 minutes. `GateArm` now clamps a held countdown to the new length,
   so held lanes close within one gate length of the switch.

Hold semantics otherwise unchanged: the lane stays up through any silence
until the SAME lane fires again (Every step: every step carrying the class;
Onsets only: real hits only). Mute stops pattern gates; held lanes stay up
until the next hit, which is what "hold to next hit" says.

## How Ernest tests it (three envelopes on J6 / J7 / J8)

Patch J6 → kick envelope gate, J7 → snare envelope, J8 → hat envelope (or a
three-channel scope). Load a familiar break (Amen). Options → Grammar =
Classic, Chop knob full (verbatim), Gate Length = 20 ms, Gate Level = Fixed.

1. **Dominant is v0.8.0.** Gate Lanes = Dominant class. The three lanes
   fire exactly as before the merge — one lane per step.
2. **Multi-label.** Flip Gate Lanes. The audio must not change. The hat
   envelope now also fires on the downbeat kicks that carry a hat and on
   most snares; the kick envelope must NOT fire on the hat carpet, and the
   snare envelope must not fire on pure kicks. Flip Classifier between the
   four lenses: with Multi-label on, the lanes do not change (the ring
   colours do).
3. **Hold to next hit.** Gate Length = Hold, Gate Fires = Onsets only,
   envelopes in gate (sustain) mode. Each lane holds until its next real
   hit and dips for ~1 ms at that hit (visible on a scope; envelopes
   retrigger). Set Gate Fires = Every step: the snare lane now retriggers
   on the snare's decay steps too — the rapid-fire the field report meant,
   available on purpose.
4. **Retrigger on rolls.** Gate Length = 50 ms, Grammar = Collins, Chop
   knob into the rolls zone. Each ratchet re-triggers its envelope instead
   of merging into one long gate.
5. **Leaving Hold.** With lanes held, switch Gate Length to 6 ms. Every
   held lane closes within 6 ms even if it never fires again.
6. **Velocity + accent.** Gate Level = Velocity: envelope depth follows
   the hit's loudness. After GRAM lands, Grammar = Grids: accented map
   steps hit the full 5 V, ghost steps stay quiet. (Before GRAM, no step
   is accented and velocity is exactly v0.8.0.)

## Tests (`tests/engine_check.cpp`, GATES sections; `tests/run.sh` ALL GREEN, Classic hash unchanged)

- Band mask on a constructed drum loop: pure kicks → kick lane only;
  kick+hat → kick and hat; pure hats and the quiet carpet → hat only;
  snare(+hat) → snare (and hat), never kick; silent → nothing; superset of
  the Hockman lens; identical under all four lenses; classes and salience
  unchanged by asking for the mask.
- Engine lanes: Dominant ignores `stepBands` (gates and audio identical to
  no mask); Multi-label raises both lanes on the kick+hat step and only the
  kick lane on the pure kick step, audio identical; an all-zero mask falls
  back to the dominant class; onsets-only and continuations gate
  Multi-label exactly as Dominant.
- Accent: `GateVolts` mapping; the accent rides the gate to the shell on
  its own lane; a Voice call gates without an accent.
- Lane law: 6 ms gate arms and closes; re-hit while high retriggers at the
  new level (low poll, then up); Hold survives 24 s of silence and
  retriggers once on a re-hit; Hold → 6 ms closes within 6 ms.
