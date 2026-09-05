# Keyframe lessons — Matthew Nielsen's DAFx26 paper and capicola

*Digested 2026-09-05. Paper: "Keyframe Time Stretching via Extrema Sampling",
DAFx26 (Cambridge MA, Sept 2026), CC-BY-4.0, in `docs/`. Code: capicola,
AGPL-3.0, cloned as a sibling at `~/Daisy/capicola` (an Alchemy Lab V2
module — the hardware embodiment of the paper). Matthew's README states an
AI policy: the engine is his human-authored IP; AI was used for docs and SDK
glue. We honour that by using his code as a **credited library**, never by
re-deriving it.*

## 1. The paper in one breath

Reduce audio to its **local extrema** (timestamped keyframes) with a
B-spline derivative (zero at Nyquist, so noise doesn't spawn extrema), a
−60 dB difference deadband, and sub-sample refinement of each peak. Because
every keyframe has zero derivative, reconstruction between two keyframes is
**smoothstep with no tangents, no division** — two multiplies. Time stretching
is OLA where the crossfade is triggered and sized by **keyframe count** (the
"leash" K), so it is automatically short at transients and long in sustains.
Cost ≈ 34 cycles/sample offline, 61 live, vs ≈ 850 for a phase vocoder on a
Cortex-M7. Sample-by-sample, no frame latency. Best log-attack-time score of
the four methods at 1.8×; MUSHRA "fair" overall, tracking sparsification
fidelity (voice/glockenspiel lose spectral contrast; **dense, percussive,
layered material is its best case** — i.e. breaks).

## 2. What capicola adds beyond the paper

- **Transient-driven re-anchor.** A TKEO detector runs on the input; each
  kept transient *punches* the read head to the delayed tap with one
  crossfade, so stretched tails ring out underneath while the output stays
  locked to the incoming rhythm. Two grains trade places over one gain ramp,
  so a punch can never click.
- **Self-calibrating detector** (`Detector.h`): TKEO from a B-spline read
  (value, d1, d2 → |d1² − v·d2|) → SVF lowpass → peak pick on the envelope's
  d1 zero crossing, refined sub-sample. Threshold is a **ratio over a 100 ms
  one-pole average of the envelope stream**, so silence pulls the reference
  down and an isolated hit after a gap always passes. 150 ms hold-off. Every
  peak is stored; the threshold only flags which are *kept*.
- **Gap-gated guard keyframes** (`Analyzer::GuardKeyframe`): a coverage
  keyframe only when no extremum has been logged for fade/2 — the paper's
  per-block boundary frame injected block-rate error.
- **Keyframe waveshaper**: drive applied to keyframe *values* before
  interpolation — the nonlinearity never sees the sample rate, so it cannot
  alias.
- **One knob, three consequences**: fade = wet latency = splice length =
  retrigger floor.
- **Routing + Depth pages**: every performance knob has a mod source
  (in-follower / out-follower / CV IN, 3-zone selector) and a bipolar depth.
  Default source is the *output* follower → self-modulating out of the box.
- Engineering: SDRAM classes trivially constructible with `Init()` after
  `hw.Init()` (same law we follow); 64-bit monotonic counters (32-bit wrapped
  at 12 h 25 min); per-class host tests plus a fuzz test; `CpuLoadMeter` in
  the engine.

## 3. Lessons banked for Audiothurgist (ranked)

### L1 — The Detector is our M5 live-slicer, and a better onset table
Our `bb_onset.h` is offline spectral flux (512-pt FFT, median threshold,
35 ms gap, ±hop refinement). Matthew's runs **live at ~50 cycles/sample**,
lands **sub-sample**, needs **no gain staging** (ratio threshold), and
stores salience (peak value) for free. Concrete plan:
- Vendor `Detector.h`, `DeluxeLine.h`, `SparseLine.h`, `filter.h` with
  attribution (or a submodule — Matthew's call, see §5).
- RECORD/LIVE-SLICE (spec §5.2): run `Detector::Analyze` per input sample in
  the audio callback; a kept event = a marker (same path as the trigger jack
  and B3). Hold-off via `SetMinEventGap` — 150 ms is his default; we want
  ~35–40 ms for 16ths at 170 BPM.
- Load path: run it over the buffer once → onset SliceTable + salience,
  replacing/augmenting spectral flux for the "onset" table of §6.1.
- His `LIVE_EFFECT` punch semantics (idle grain hard-seats at the tap, fades
  in over the fade) is exactly "the patch that samples is the patch that
  performs".

### L2 — Extrema density is a free fourth classifier lens — **built, v0.7.2**
Keyframe spacing = instantaneous bandwidth. Hats are dense, kicks sparse,
snares in between. Offline at load: run the `Analyzer` once, count
keyframes per grid step ÷ step length → a per-step **density**. Exposed as
Options → Classifier → **Density (keyframes)** (`core/bb_classify.h`).

**Measured on the canon** (`tests/classify_check.cpp`, Amen / Apache /
Funky Drummer / Hot Pants) before shipping — three things the data forced:
1. A *global* deadband fails both ways: small → the ride carpet makes every
   step "hat"; large → quiet hat-only steps fall under it and read "kick".
   The deadband must be relative to **each step's own peak** (floored at 5 %
   of the loop's). Then it reads the bandwidth of whatever dominates the
   step regardless of level.
2. The lens is a **brightness ranking**, not a band detector: a kick under
   an equally loud mid layer reads as that layer (Apache's bongos flatten
   it). So the split is by rank — sparsest 30 % kick, densest 45 % hat —
   which guarantees gate spread on any break.
3. Deadband 0.2 / 0.35 / 0.5 of the step peak agree with Hockman 58 / 58 /
   55 % on average; 0.2 wins on the Amen (60 %). The disagreements are the
   point: it is a different lens. Kicks land where Hockman's do on the Amen
   and Funky Drummer; the hat/snare boundary is where it differs.

Still open from this lesson: LED brightness per slice from density, and
transient detection by jumps in keyframe spacing (Matthew's further-work
item — relevant to M5).

### L3 — Measure crossfades in keyframes, not samples
Our continuity window (160 samples) and choke fades are fixed. The paper's
central point is that no fixed crossfade suits both transients and
sustains. Cheap version without adopting the sparse reader: use the L2
per-step density to set each slice's choke/crossfade length — dense → short
(floor 48 samples = 1 ms, his floor), sparse → long, capped at a few
hundred ms (his Limitation 3: silence-then-transient makes very long
splices that audibly repeat). This is the lowest-risk audible win here.

### L4 — Stretch as an *Option*, not a replacement (M7 candidate)
The spec bans stretch (§1) because **repitch is the genre**: the Amen at
170 BPM is ~+4 semitones bright, and that brightness is jungle. Keep that
default. But a keyframe reader per voice would give what users will ask
for: **tempo without pitch** (fit a break to the clock without chipmunk)
and **pitch without tempo**. Facts for the decision:
- Analysis is offline at load (cheap). Storage: 12 B/keyframe × ~0.5 N ≈
  6 B/sample — **1.5× our float buffer, not compression** for us. Fine:
  SDRAM is 30 % used.
- Playback: one `Granule` per voice (we have 4) over one shared
  `SparseLine`; `Spawn(sliceStart)`, `stretch` = tempo ratio, `pitch` =
  pitch ratio, `leash` = a macro knob. `Granule` assumes a live writer
  (fences from `writePos`); for static material the fences are the buffer
  end, and we stop voices at slice end ourselves.
- Pushed hard (2×+) the method repeats segments ahead of transients — that
  is the Akai-S1000 "timestretch" texture of Goldie/Photek, potentially a
  feature. At modest ratios (0.8–1.25) it is clean and transient-true.
- Defer until M5 lands and the ears verdict on the present engine is in.

### L5 — Ecosystem UX idioms worth adopting
- **Routing page + Depth page** answers our open "jack maps as presets"
  question more simply: CV IN → any knob via a 3-zone selector, with a
  bipolar depth. That unblocks a Chop-CV input and the SPLIT keymap without
  a preset schema for jacks.
- Pot catch with a **muted pip at the stored value** (we have re-latch but no
  pip — the Brain-density trap on the checklist is exactly this).
- Button LEDs never off; activity is a **dark flash**.
- TRIG IN **always accepted, even at mute** — our J5 gate should be too.
- One fade knob tying latency, splice length and retrigger floor together.

### L6 — Engineering hygiene to copy
- `CpuLoadMeter` in the engine → CPU avg/max in `GET_STATE` and the web app.
- Audit our monotonic counters for 32-bit wrap (his bit him at 12 h 25 min).
- Per-class host tests + fuzz. Our `tests/` only has `grammar_check`; the
  engine (`bb_engine.h`) deserves a host harness before M5/M7.

## 4. What *not* to take

- The memory-compression claim: for float buffers keyframes cost more, not
  less (see L4).
- The whole engine as our playback path. Audiothurgist's identity is
  sampling, slicing, rearranging with period-correct repitch; capicola
  already exists in the ecosystem as the stretcher. Complement, don't
  duplicate.
- Spectral-contrast haze is a non-issue for breaks but real for vocal or
  glockenspiel-like material; if a Keyframe option ships, say so in the
  manual.

## 5. Licensing and courtesy

Both projects are AGPL-3.0, so vendoring is legal with attribution. Matthew
is a collaborator on the same platform and his README is explicit about
authorship, so the right move is: tell him what we'd like to use (Detector,
DeluxeLine, SparseLine, filter, possibly Granule later), credit him in the
manual and a NOTICE, and ask whether he prefers a submodule (pins his
versions) or a vendored copy. **Ernest's call and message to send.**

## 6. Proposed roadmap insertions (decisions for Ernest)

1. **M5 live-slice = Detector-driven** (L1), with the trigger jack and B3
   as forced markers. Keeps spec §5.2 intact; the detector is the "envelope
   follower on the material itself" case made native.
2. **Options → Classifier → Density** (L2). Small, testable, novel.
3. **Per-slice adaptive fade from density** (L3). Small, audible.
4. **Routing/Depth pages** (L5) in place of jack-maps-as-presets.
5. **M7 — Options → Stretch: Repitch / Keyframe** (L4), after M5.

## 7. Questions for Matthew

- Hold-off: 150 ms is right for capicola's splices; for 16ths at 170 BPM
  we need ~40 ms. Any detector tuning he'd change at that density?
- Has he tried keyframe *density* as a timbral/class feature (L2)? The
  paper hints at it under transient detection.
- Typical keyframes-per-second he sees on breaks (for our SDRAM budget).
- Whether `Granule` over a static (non-live) buffer is a use he'd support,
  or whether he'd rather we take only the reader primitives.
