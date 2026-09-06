# Literature and prior-art review — 2026-09-05

*Scope: concrete improvements, optimisations and related techniques for
Audiothurgist, organised by subsystem. Every source below was fetched and
read (URL in §4); where a claim rests on an abstract, a secondary page, or
could not be fetched at all, it is marked* **[abstract only]** *or*
**[UNVERIFIED]**. *Nothing here is quoted from memory. Feasibility notes
assume the Daisy Seed 2 DFM: Cortex-M7 at 480 MHz with FPU, 48 kHz,
24-sample blocks (0.5 ms = 240 k cycles per block), CMSIS-DSP only.*

*"We" throughout refers to the code as it stands in `core/` and
`src/audiothurgist.cpp` at v0.7.2-density (190becc), read in full before
this review.*

---

## 1. Executive summary

The literature endorses the architecture more than it challenges it: grid-
anchored rearrangement (Jehan 2005, Collins 2001), odd-cut figures and
every-4th-bar fills (Collins 2001, BBCut `SQPusher1`), repitch rather than
stretch for this genre (Hockman & Davies 2015 work entirely in the repitch
idiom), transient-at-slice-start playback (ReCycle manual), class-matched
substitution (Hockman & Davies 2015's length-weighted timbral matching), and
simple spectral flux as an onset function (Dixon 2006: differences between
detection functions "not significant … which speak for SF"). Two things the
review turned up are *not* literature but code facts that matter more than
any paper:

- **The Grids port is indexed at the wrong resolution.** Mutable's map is one
  4/4 bar at 32nd-note resolution (`kPulsesPerStep = 3 // 24 ppqn; 8 steps
  per quarter note`, `kStepsPerPattern = 32`); `GenerateLang` feeds our 16th
  index straight in, so a 16-step loop plays the first half of the map
  across the whole bar (backbeat lands on beat 3). Fix: Grids step =
  `2·(s % 16)`; the odd (off-16th) map steps become 32nd ratchets, which we
  already have (`stepRatN = 1`). One-line fix, large fidelity payoff.
- **No published work applies random-matrix / Coulomb-gas spacing statistics
  to musical rhythm.** Exhaustive arXiv/web queries found only bus-arrival,
  parked-car and queued-vehicle spacing papers (GUE/GSE). The BreakFix
  Phase-0 result (Amen at the 99.8th percentile of onset clustering over
  1,226 breaks) is, as far as we can find, novel.

### Ranked top-10 recommendations (payoff ÷ effort)

| # | Recommendation | Subsystem | Effort | Payoff | Verdict |
|---|---|---|---|---|---|
| 1 | **Fix Grids resolution** (map step = 2·16th; odd steps → 32nd ratchets; accent when level > 192 → velocity) | Grammars | trivial | high | adopt |
| 2 | **Slice at the material's own swing.** Estimate the 16th swing ratio from onset phases at load (Frane 2017: canon median 1.2, up to 2.1, uncorrelated with tempo) and place odd-16th slice *boundaries* (and, optionally, playback step times) at the measured offset. Straight 16ths decapitate swung hats. | Slicing | low–med | high | adopt |
| 3 | **Per-slice adaptive fade from keyframe density** (Nielsen 2026; Driedger & Müller 2016: ~10 ms OLA frames are fine for purely percussive material). Choke/continuity fades scale inversely with the density lens we already compute; linear crossfade for correlated (verbatim/continuity) joins, equal-power for cross-slice joins. | Playback | low | med–high | adopt |
| 4 | **Multi-label gates.** Fire a class gate when that band's per-step energy exceeds a per-class relative threshold, not only for the arg-max class; a kick+hat step then gates both lanes. Wu et al. 2018 review: band-pass + thresholding is the cheapest real-time drum-transcription pattern. | Classification / Brain | low | med | adopt |
| 5 | **Log-compressed, band-filtered spectral flux + Dixon peak rule** in `bb_onset.h` (Böck, Krebs & Schedl 2012: SF 74.5 → log-filtered SF 80.3 F-measure online). Offline at load; same FFT. Feeds M5's onset SliceTable and per-onset salience. | Onset table | low | med | adopt |
| 6 | **Bar/downbeat: IOI clustering + kick-band downbeat.** Dixon 2001's cluster-and-relate tempo induction (25 ms cluster width, integer-multiple relations) instead of a single folded median; Hockman, Davies & Fujinaga 2012's low-frequency onset function (sum of the lowest 5 of 40 ERB bands) to choose the downbeat rotation. Fixes the known long/non-loop mis-estimate. | Slicing | med | med–high | adapt |
| 7 | **Salience-aware swaps.** In strict class-swap mode also match the salience tier (Hockman & Davies 2015 re-weight candidates by segment length precisely so "an off-beat ghost note" is not pasted "in place of a salient snare"). We already carry `gSal[]`. | Grammars | low | med | adopt |
| 8 | **Clock: regression period estimator + honest clock meter.** Keep the SDK PI loop for phase, but seed `base_per_sec` from a least-squares fit over the last N pulse stamps (Ableton Link's `HostTimeFilter` idiom) rather than an EMA(0.15) of single periods; expose measured cycle-to-cycle jitter on the P2 ring/web (E-RM 2014 measured DAW MIDI-clock jitter up to 38 ms; Pamela's manual: "It can't fix an external clock"). Beat-align multiplier changes (already the user's #1). | Clock | med | med | adapt |
| 9 | **Tail policy for slots that outlast slices** (tempo below native, pitch up): Ableton Beats mode's *Transient Loop Mode* (Loop Off / Loop Forward from a zero crossing / Back-and-Forth) as an Option beside our honest gate. | Playback | low–med | med | adapt |
| 10 | **"Déjà vu" flow coherence.** A probability that each flow-bar reroll re-draws from the history ring instead of fresh seeds (Marbles' DEJA VU semantics: 7–12 o'clock = loop probability, 12–5 = in-loop permutation). Margulis 2013: inserted repetition — immediate *or delayed* — raised enjoyment/interest/artistry ratings. Also the natural home for the "second-time-intentional" motif memory. | History / Grammars | low | med | adopt |

Honourable mentions (see sections): Collins' `CampCutProc` change-ringing
permutations as a deterministic fifth grammar; an LHL syncopation meter on
the flattened pattern (Witek 2014 inverted-U) as a display/calibration aid;
Gouyon–Pachet–Delerue's decay-ZCR as a fourth cheap classifier feature; the
capicola Detector for M5 (already planned); Braids' 9:7 asymmetric Voronoi
quantiser as a cross-check of the Voice note window.

Things the literature says to **leave alone**: don't add synthetic
microtiming to kick/snare (Frühauf, Kopiez & Platz 2013: the exactly
quantised version rated highest, early shifts worst); don't chase a CNN
drum classifier (no microcontroller drum-hit classifier exists in the
literature, and our labels only need self-consistency); don't replace
repitch with stretch as the default (the genre, the spec, and our own
listening all agree — keep Keyframe as an M7 *Option*).

---

## 2. Per-subsystem review

### 2.1 Slicing, onset table, buffer conditioning (`bb_onset.h`, `bb_condition.h`)

**What we do.** Equal 16ths of a power-of-2 bar count chosen so a bar is
1.6–3.2 s; an offline onset detector (512-pt FFT, hop 256, Hann, linear-
magnitude half-wave-rectified spectral flux, threshold = 1.5 × local median
over ±8 frames + 0.1 × mean, 35 ms minimum gap, ±hop waveform refine to the
steepest |x| rise, −16 samples); loop-length snap from the median IOI folded
to a 16th, adopted only if the circular concentration of onset phases
improves by 0.03.

**Sources and what they propose.**

- *Bello et al. 2005* (tutorial): reviews energy, HFC, spectral difference,
  phase deviation, complex-domain and probabilistic reduction functions;
  peak-picking = normalise, low-pass, subtract a moving-median threshold
  with a ~100 ms window, take local maxima above zero; "If the signal is
  very percussive (e.g., drums), then time-domain methods are usually
  adequate"; HFC "performs better for highly percussive sounds and complex
  mixtures (with drums)"; complex-domain "a good choice in general".
- *Dixon 2006* (DAFx): SF with half-wave rectification, L1 norm, linear
  magnitude; peak rule = local max over ±w frames AND ≥ local mean over
  [n−mw, n+w] + δ (w = m = 3 at 10 ms hop). On the Bello set SF reached F
  0.95/0.98/0.97/0.88 (pitched-nonperc/pitched-perc/nonpitched-perc/
  complex); on 106 k piano onsets SF F 0.964, timing error 8.8 ms;
  concludes the algorithm choice "could be based on … simplicity of
  programming, speed of execution … (all of which speak for SF)".
- *Böck, Krebs & Schedl 2012* (ISMIR): log-magnitude `log(λX+1)` and a
  filterbank (quarter-tone) before the flux; online F-measure SF 74.5 →
  SF-log 76.1 → SF-log-filtered 80.3 (vs. RNN OnsetDetector.LL 81.7); log/
  whitening "emphasize higher frequency bands that are important for
  detecting percussive onsets"; online ≈ offline once parameters are
  right (+0.25 % F offline).
- *Böck & Widmer 2013* (SuperFlux, DAFx): a maximum filter over ±1
  quarter-tone bins on the *previous* frame before the difference, to
  suppress vibrato/pitch-glide false positives (−58…61 % FP on violin);
  no gain on percussive sets (Böck set SF-log-filt 0.812 vs SuperFlux
  0.836 offline).
- *Röbel 2003* (DAFx/ICMC): transient = energy centre-of-gravity of the
  frame exceeding K× the COG of a linear ramp; per-band binomial
  statistics; shows COG ≈ normalised time derivative of spectral energy.
- *Kaiser 1990*: Teager–Kaiser operator `x²[n] − x[n+1]x[n−1] ≈ (AΩ)²`,
  three samples, on-the-fly. *Ištvánek et al. 2020* [abstract only]: TKEO
  pre-processing before spectral flux *lowers* onset accuracy but improves
  tempo estimation (18.2 → 10.0 BPM mean error).
- *Nielsen 2026 / capicola `Detector.h`*: TKEO of a B-spline read → SVF
  low-pass → sub-sample peak on the envelope's derivative zero crossing;
  ratio threshold over a 100 ms one-pole average of the envelope stream;
  150 ms hold-off; ~61 cycles/sample live.
- *Su & Wu 2016* (arXiv): minimum-phase asymmetric analysis windows reduce
  the intrinsic latency of STFT-based onset functions [abstract only].
- Products: *ReCycle 2* analyses "where the individual sounds in the loop
  are" with a Sensitivity slider; REX2 stores slices + computed tempo;
  "Stretch is not Time Stretch" (adds an artificial tail per slice).
  *Akai MPC 2.7* Chop To: Manual / Threshold (with Min Time) / Regions
  (equal) / BPM (bars, beats, 1/4…1/32). *Octatrack MKII* only offers
  manual markers and equal grids of 2…64 with optional zero-crossing
  alignment — no transient slicing.

**Relation and recommendations.**

1. **Adopt: log-compressed, band-filtered flux + Dixon's peak rule** (#5).
   Replace `m − prevMag` with `log(λ·m + 1)` differences (λ ≈ 1–20 in
   Böck's search; pick by ear/host test on the four canon breaks), sum
   into ~24–48 log-spaced bands before differencing, and use Dixon's
   two-condition peak rule with δ on the normalised function instead of
   a single 1.5×median scale. Keep the 512/256 FFT; it is offline at load.
   *Feasibility:* unchanged cost (one 512-pt FFT per 5.3 ms of audio,
   offline). Our hand-rolled radix-2 with `cosf/sinf` per stage is fine
   offline; if it ever runs live, switch to `arm_rfft_fast_f32`.
2. **Ignore SuperFlux's max filter** for breaks — designed for vibrato;
   Böck's own numbers show no percussive gain.
3. **Adapt Röbel's COG** as the waveform-level refine: energy centre of
   gravity over the ±hop window is a smoother, less step-size-dependent
   locator than our stride-8 |x| pre/post difference. Cheap, offline.
4. **Live (M5): capicola Detector, hold-off 35–40 ms**, with the trigger
   jack/B3 as forced markers (as `keyframe-lessons.md` L1). Bello's "time-
   domain is adequate for drums" and Kaiser's cost both back this; note
   Ištvánek's finding that TKEO alone is a *worse* onset function than
   spectral flux — so for the offline table, take the **union** of the two
   detectors with a 20 ms merge, and keep spectral-flux salience.
   *Feasibility:* ~61 cycles/sample ≈ 1.5 k cycles per block ≈ 0.6 % CPU.
5. **Adopt an MPC-style "Min Time" as the hold-off setting** (it is the
   one slicing parameter every product exposes) and offer *Threshold /
   Regions / BPM* wording in the web UI — users already know it.
6. Zero-crossing alignment (Octatrack) is moot with our 1 ms attack ramp.

### 2.2 Bar length, tempo, downbeat

**What we do.** Bars = power of 2 with 1.6–3.2 s per bar; loop-length snap
from the median IOI folded down to a 16th, refined twice against all IOIs
within ±15 %, accepted only if onset-phase concentration rises ≥ 0.03. The
known field bug: long / non-loop files mis-estimate the bar.

**Sources.**

- *Dixon 2001* (JNMR; OFAI TR): IOI clustering — all IOIs (including
  compound, 50 ms–2 s) assigned to the nearest cluster within a 25 ms
  cluster width or start a new one; merge drifted clusters; rank clusters
  by count plus contributions from "related" clusters (near-integer
  multiples), the top clusters being tempo hypotheses.
- *Hockman, Davies & Fujinaga 2012* (ISMIR, "One in the Jungle"): downbeat
  in HJDB via SVR on an 8th-note grid; the decisive feature is a **low-
  frequency onset function = sum of the lowest 5 of 40 ERB bands**
  ("drum types found at downbeats are likely to be kick drums"), beat-time
  weighting ω = 1.3; 74.7 % vs 51.3 % for the best prior method; states
  breakbeats "almost invariably begin with kick drums" and producers
  "mostly apply downbeat-preserving transformations".
- *Frane 2017* (Music Perception): 30 canon breaks, 16th-note swing ratio
  median 1.2 (M 1.3, SD 0.25, range 1.0–2.1), *no* correlation with tempo
  (r = −.01); backbeat delay at beat 2 only; e.g. Amen 138 BPM ratio 1.2,
  Funky Drummer 1.1, Apache 1.1, Impeach the President 1.4, Synthetic
  Substitution 1.3 (local max 1.7), Sing a Simple Song 2.1.
- *Boenn 2011* (Bath thesis; 2018 Springer book [abstract/catalogue only]):
  filtered Farey sequences as the quantisation grid, weighting quantised
  IOI ratios against a beat structure. Grounds our Brocot/aligned-cell
  tiling mathematically.
- *Gouyon & Dixon 2005* (CMJ review) [abstract only]: unifying framework
  (tempo induction → beat tracking → quantisation → meter induction).
- *Ableton Live manual*: warp markers "lock a specific point in a sample,
  such as a transient, to a particular place in the timeline".

**Recommendations.**

1. **Adopt swing-aware slicing (#2).** After onset detection, take each
   onset's phase inside its 8th-note cell; the mean phase of the off-beat
   cluster p gives the swing ratio p/(1−p) (Frane's definition is
   duration-based; the two agree for a two-cluster pattern). Place the odd
   16th slice boundary at p rather than 0.5 when the off-beat cluster is
   tight (circular R > ~0.6) and the ratio is in 1.05–2.2. Optionally
   apply the same offset to playback step *times* so the material keeps
   its swing under rearrangement (a per-loop `swing` field consumed where
   `step = (int)(barPhase·nSlices)` is computed). Grid steps remain 16
   per bar — the pattern engine does not change. *Feasibility:* offline;
   a few hundred operations. *Risk:* hats-only swing with straight kick/
   snare is the norm (Frane) — slice boundaries move, kick/snare attacks
   still sit at even 16ths, so nothing is lost.
2. **Adapt Dixon's clustering** for the snap: replace the single folded
   median by cluster-and-relate over IOIs; use the top cluster and its
   integer relatives to estimate the 16th; accept via our existing phase-
   concentration test. Swing no longer shears the estimate (the off-beat
   cluster is simply a second cluster).
3. **Adopt a kick-band downbeat** for non-loop material: one-pole low-band
   (< ~150 Hz) energy derivative, peak-pick, pick the rotation of the 16th
   grid whose beat-1 positions carry the most low-band onset energy
   (Hockman's L function, ERB bands replaced by our one-pole split).
   *Feasibility:* trivial. This is the fix for the carried-over bar bug.
4. Keep the power-of-2 bar prior; every product reviewed (ReCycle, MPC
   "BPM" chop, Ableton) assumes a whole number of bars.

### 2.3 Pattern grammars (`bb_pattern.h`)

**What we do.** Classic: aligned power-of-2 cell tiling with in-place /
repeat / class-matched swap, a five-zone macro knob, a cluster field with
rests and hat ticks, 32nd doubles at burst cores. Collins: per-half-bar
odd-cut figures (3+3+2, 5+3, …), position-in-phrase weighted moves, 1-cell
stutter terminator, WarpCut geometric rolls (accel/decel), every-4th-loop
fill with snap-back, ±1.5 % repitch on repeats. Grids: 25-node 5×5 map,
bilinear `U8Mix`, fire when level > 255 − density. Euclid: Bresenham-form
E(k,16) with rotation.

**Sources (all verified from the author's PDFs / Mutable's code).**

- *Collins 2001, "Algorithmic Composition Methods for Breakbeat Science"*
  (Music Without Walls, De Montfort; revised ARiADA 2003 — **not ICMC**):
  cuts of **odd** unit lengths "up to half of a single bar"; repeats
  1…MaxRepeats (default 2) from the same offset; overshoot leaves a
  terminating segment "like the 2 in 3+3+2"; RepeatChance ≈ 0.2 for a
  one-unit stutter at phrase end; SubDiv 8 "works really well"; "5 unit
  blocks tended to sound more awkward than three unit blocks", "7 unit
  blocks would reveal too much"; in the *beat-generation* section, per-hit
  speed variation "within less than an eighthtone … 0.986 to 1.014";
  AAAB four-bar grouping "where … B is a fill pattern"; position-dependent
  weights appear only as future work ("larger blocks to begin, smaller
  ones to end").
- *Collins 2001, "Further Automatic Breakbeat Cutting Methods"*
  (Generative Art, Milan): the phrase/block/repeat hierarchy; procedures
  **ChooseCutProc**, **StatBalProc** (Ames statistical feedback),
  **StatBalProc2** ("changing weightings … within phrases … large cuts,
  high numbers of repeats and early offsets towards the front of phrases
  and the converse towards the end" — the position-weighting source),
  **CampCutProc** (change-ringing permutations of offsets),
  **RecCutProc** (recursive), **WarpCutProc1** (per block: plain / even
  subdivision / **geometric progression** with a fast→slow or slow→fast
  probability; rolls allowed anywhere, not only phrase ends).
- *Collins 2002, "The BBCut Library"* (ICMC): cut-procedure / cut-synth
  separation; allowable offsets "in line … with … ReCycle"; perceptual
  limit "around semiquavers at 250 bpm".
- *BBCut help files* (`BBCutProc11`, `WarpCutProc1`, `SQPusher1`):
  BBCutProc11 args `stutterchance, stutterspeed, stutterarea` (default
  0.5 = last half bar); WarpCutProc1 `accel` "0.5 to 0.99"; **`SQPusher1`:
  "fills based on musical motifs lifted from Squarepusher tracks. The
  fills occur every fourth bar by default"**, with a `fillscramble`
  probability. An explicit "repitch the repeats" rule was **not found** in
  any Collins text; the ±1.4 % figure is from his beat generator, and the
  SQPusher example repitches cuts by default. Our attribution should say
  "after Collins" for the value, not claim the rule.
- *Collins 2006, BBCut2* (JNMR) [abstract only]: beat tracking + on-the-fly
  event analysis so that "splicing manipulations respect component
  events, and their micro-timing"; scheduling from an external clock.
- *Hockman & Davies 2015* (DAFx-15): resequencing = segment source and
  target at peaks of summed bass + snare detection functions; cosine
  similarity of segment features; top-5 candidates **re-weighted by
  segment-length ratio** to avoid "an off-beat ghost note in place of a
  salient snare"; winner amplitude-scaled to the target. No explicit
  "salience tier" taxonomy — that phrase is ours.
- *Zils & Pachet 2001* (musaicing, DAFx): sequence of samples chosen by
  weighted constraint satisfaction — segment constraints (target
  descriptor per slot) and sequence constraints (**distribution**
  constraints "impose regular rhythmic patterns, by specifying a
  distribution of percussive sounds", a "percussive tempo" constraint,
  continuity, cardinality/All-Different); adaptive local search.
- *Schwarz et al. 2006* (CataRT, **DAFx-06**, not NIME): real-time unit
  selection by Mahalanobis distance in descriptor space, k-NN index;
  Caterpillar's **target cost** vs **concatenation cost** (join-point
  continuity of descriptors).
- *Jehan 2005* (MIT thesis): "Scrambled Music" — rearranging pointers to
  beat segments randomly still lets "the beat be extracted again"; cross-
  synthesis "strictly preserves musical 'edges'".
- *Toussaint 2005* (BRIDGES): E(k,n) via Euclid/Bjorklund; named
  rhythms are often *rotations* of E(k,n); the extended version cites
  Harris & Reingold: Bresenham's line algorithm *is* the Euclidean
  algorithm (upper boundary of a digital line = (33334) = E(5,16)).
- *Mutable Grids* (manual + `pattern_generator.cc/h` +
  `lookup_tables.py`): map "obtained through machine learning techniques"
  from "a large collection of electronic music tracks and drum loops";
  25 nodes × (3 instruments × 32 steps); bilinear `U8Mix`; fire if
  `level > ~density`; **accent if `level > 192`**; Chaos = random
  perturbation added to each part's level; **`kPulsesPerStep = 3 // 24
  ppqn; 8 steps per quarter note`, `kStepsPerPattern = 32`** → one bar of
  32nds; Euclidean mode advances on even steps (16 per bar).
- *Margulis 2013* (Empirical Studies of the Arts): Berio/Carter excerpts
  with segments repeated "immediately or after a delay" were rated more
  enjoyable, interesting and artistic than the originals.
- *Witek et al. 2014* (PLoS ONE): 50 two-bar funk drum breaks at three
  syncopation levels (LHL index extended with instrument weights); pleasure
  and wanting-to-move follow an inverted U — medium syncopation peaks.
- *Marbles manual*: DEJA VU 7–12 o'clock = probability of re-using the
  loop (0 → 1, locked at 12), 12–5 o'clock = probability of jumping
  within the loop; loop lengths incl. 5, 7, 10, 14.

**Recommendations.**

1. **Fix Grids indexing (#1).** `gs = 2·(s % 16)` for the on-16th read;
   read `2·(s%16)+1` too and, when it fires, set `stepRatN = 1` (our 32nd
   double) — the map's off-16th hits become ratchets, which is exactly
   what they are. Add **accent** (level > 192 on the firing class) as a
   velocity boost on the gate and a brighter ring pip. Optionally port
   *Chaos* (per-part random perturbation of the level) as the P1-Brain
   "swap wildness" for Grids. *Feasibility:* trivial.
2. **Adopt salience-aware strict swaps (#7):** in Classic/Collins, when
   `strict`, require `|sal[cand] − sal[pos]| < 0.25` (or same tercile)
   with a fallback after 8 tries. Prevents ghost-for-backbeat swaps; keeps
   the Classic hash only if gated behind a flag (Classic is byte-frozen —
   make it a Collins/Lang behaviour, or a new Classic sub-mode).
3. **Adopt déjà-vu flow (#10):** a `flowRecall` probability; on each flow
   bar, with probability p pick a slot from the history ring (or the
   canonised-mistake pins, when the timeline bank lands) instead of a fresh
   `fillSeed`. Marbles' two-zone knob maps directly onto P5-Brain or a
   Settings value. Margulis's *delayed* repetition condition is the
   evidence that echoing a bar later in the phrase is rewarded.
4. **Adapt CampCutProc** as a fifth, fully deterministic grammar ("Bells"):
   plain-hunt / plain-bob permutations of cell offsets across successive
   bars. It is cheap (a permutation table), reseed-free, and gives the
   "structured, not stochastic" feel the user asked for; also a natural
   partner to the history ring since every bar is a known permutation.
5. **Adapt StatBalProc** (Ames statistical feedback): track the count of
   each move type used so far in the phrase and penalise over-used ones —
   removes the "three swaps in a row" runs without touching the RNG
   stream's seed discipline (apply as a post-filter on candidate moves).
6. **Consider a concatenation cost** (CataRT): when choosing a swap
   target, add a term for level continuity at the join (RMS of the
   candidate's last 5 ms vs the following slice's first 5 ms, precomputed
   per slice). This is the "cost-matrix swaps" item from the earlier codex;
   medium effort, modest payoff, keep behind the flow of #3/#7.
7. **Syncopation meter (display only):** LHL weights on the 16-grid
   (0,−4,−3,−4,−2,−4,−3,−4,−1,…) over the flattened per-step class hits
   give a cheap index; show it on the web front end so knob zones can be
   calibrated toward Witek's medium-syncopation peak. Do not steer the
   grammar by it yet.
8. **Correct the attributions in code comments:** "Algorithmic Composition
   Methods for Breakbeat Science" is Music Without Walls 2001 / ARiADA
   2003, not ICMC; the fill-every-4th-bar rule is BBCut's `SQPusher1`;
   position weighting is `StatBalProc2`; ±1.5 % repitch is "after Collins'
   0.986–1.014 beat-hit variation", not a stated cutter rule.
9. Zils & Pachet's *distribution constraints* are what the Brain page's
   per-class densities already are — no action, but worth citing.

### 2.4 Classification (`bb_classify.h`)

**What we do.** Per-step one-pole band energies with three lenses
(Hockman-style < 325 / 325–4000 / > 4000 Hz; legacy 120/900 hi-lo; two-
band), relative silence at −50 dB, salience = step RMS / loudest; the
Density lens counts capicola keyframes per sample with a per-step-peak
deadband (ε = 0.2, floor 5 % of loop peak) and a rank split (sparsest 30 %
kick, densest 45 % hat).

**Sources.**

- *Hockman & Davies 2015*: bass-drum spectrogram features ≈ 50–325 Hz
  (bins 4–30 at 4096/44.1k), snare ≈ 430–5000 Hz — the numbers behind our
  325/4000 split. (The original 120/900 split put most snare energy in
  "hat"; the field lesson in `MEMORY` stands.)
- *Gillet & Richard 2004* (ICASSP): 6 hand-picked log-energy bands
  [10–70], [70–130], [130–300], [300–800], [800–1500], [1500–5000] Hz,
  "chosen according to a meticulous observation of the frequency content
  of each drum instrument … better performance compared to a … Bark
  scale filterbank"; SVM 83.9 % on 8 simplified classes over 315 loops;
  sequence model (HMM) did not beat per-hit SVM.
- *Gouyon, Pachet & Delerue 2000* (DAFx): **decay-window zero-crossing
  rate** separates bass-drum-like from snare-like hits at 87.5–96 % on
  real excerpts (fs 11 025, DC removed, 30 dB gate), "1.3 times more
  discriminant" than attack ZCR and as good as the strongest FFT partial:
  "such a simple parameter is more appropriate than more complicated and
  computationally time-consuming ones."
- *Herrera, Yeterian & Gouyon 2002* (ICMAI): > 600 isolated hits, ~50
  descriptors; ReliefF top features = decay spectral kurtosis, skewness,
  decay centroid, 300–400 Hz band, decay ZCR, 130–145 Hz band, attack
  ZCR; k-NN 99 / 96 / 90 % at three taxonomy levels.
- *Herrera, Dehamel & Gouyon 2003* (AES 114) [abstract only]: feature set
  reducible 2–3× without loss; 88 % on ≤ 9 drum-kit classes.
- *Wu et al. 2018* (TASLP review): six design patterns; "simple FR methods
  (such as BPF) and EC methods (such as cross-correlation or thresholding)
  can be implemented very efficiently, therefore enabling real-time
  applications"; **fixed-basis NMF** (templates from averaged isolated
  drum spectra, kept fixed) "makes the optimization problem convex …
  appealing and simple", with cross-talk as the known weakness; PFNMF adds
  random harmonic templates to absorb it.
- *Vogl et al. 2017*, *Jacques & Röbel 2018*: CRNN/CNN transcription
  (conv blocks of 32/64 3×3 filters + 256-wide dense; GRU 2×50…3×60). No
  parameter counts published; no microcontroller deployment found anywhere
  (six searches; closest are NIME 2012/2015 sensor/contact-mic k-NN
  classifiers running on a laptop).
- *Kedem 1986* (ZCR ↔ dominant frequency) [citation only]; *Logan 1977*:
  zero crossings determine a bandpass signal (< 1 octave) "within a
  multiplicative constant", but recovery "appears to be very difficult
  and impractical"; *Kumaresan & Wang 2001* [abstract]: real-zero
  conversion carries envelope/phase in "covert zero crossings"; *Rzepka &
  Miśkowicz 2013* **[UNVERIFIED — no page reachable; known only as
  Nielsen's ref. 3]**.

**Recommendations.**

1. **Adopt multi-label gating (#4).** Keep the arg-max label for swaps and
   the ring, but compute per-class *fire flags* per step: kick if
   `l ≥ θ_k·median_l(sounding steps)`, etc., with θ per class (start 1.5),
   and let `gateFired` carry every flagged class, not `1 << stepClass`.
   Wu's review ranks exactly this (band-pass + threshold) as the real-time-
   feasible pattern; Gillet's band edges are a better starting point than
   our three if we ever move to four bands (add 130–300 for toms/low
   snares). *Feasibility:* same one-pole filters, offline; the gate path
   is unchanged.
2. **Adapt decay-ZCR / density as a joint feature:** our Density lens is
   the modern form of Gouyon's decay-ZCR (an extrema rate with a deadband
   instead of a raw ZCR after a gate). A 2-D nearest-centroid on
   (log low/high band ratio, density) with the same rank calibration would
   let Apache's bongo-flattened kicks recover (band ratio) while keeping
   the density lens's brightness ranking. Low effort; test on the four
   canon breaks with `tests/classify_check.cpp`.
3. **Consider fixed-basis NMF offline** (three 256-bin templates from the
   loop's own most-kick-like / snare-like / hat-like steps, then
   multiplicative updates on each step's short-time spectra) to get per-
   step *mixture weights* — the principled version of #1 that can also
   feed velocity per class. ~1–2 M MACs per loop, offline at load; fine.
   Payoff uncertain until #1 is heard.
4. **Ignore CNN/CRNN classifiers.** Feasible offline at load (a Vogl-size
   conv stack over 64 steps is tens of M MACs), but there is no weight set
   for breaks, no embedded precedent, and self-consistent labels are all
   the grammar needs.
5. **Extrema-density theory:** the only fetched primary asserting "extrema
   spacing = instantaneous bandwidth" is Nielsen 2026 (citing Rzepka).
   Logan/Kumaresan back the information content of zero crossings for
   sub-octave bandpass signals, which is a stretch for a full-band drum
   mix — so describe the Density lens as an empirical brightness ranking
   (as `keyframe-lessons.md` L2 already does), not as a bandwidth
   measurement.

### 2.5 Playback: varispeed, continuity, choke, ratchets, splices (`bb_engine.h`)

**What we do.** Period-correct repitch (`rate = pitch·nativeStep/
gridStepSamp`), per-step re-anchoring, continuity branch (extend a voice
whose position is within 160 samples of the next slice start at a rate
within 2 %, with a 400-sample lookahead grace), choke of ringing voices on a
new attack (5 ms ramp, envelope-continuous via `relScale`), 1 ms attack
ramp, `sliceExt = 1.0` (no pre-echo), 32nd doubles and geometric
accel/decel series (floor 1.5 ms), `stepCont` rests that continue the
break in language modes, ±1.5 % repitch jitter on Collins repeats.

**Sources.**

- *Nielsen 2026* (DAFx26): extrema-driven OLA where the leash K in
  keyframes sets both when and how long a splice is; linear crossfade
  `y = y_temp·t + y_play·(1−t)`; max splice "a few hundred milliseconds"
  because silence-then-transient otherwise yields long audible repeats;
  ≈ 34 cycles/sample offline, 61 live vs ≈ 850 PV / 690 FFT-WSOLA / 4100
  time-domain WSOLA on a Cortex-M7; best log-attack-time at 1.8×, worst
  at 0.5×; MUSHRA "fair", tracking sparsification loss (voice,
  glockenspiel); dense percussive material is its best case.
- *Driedger & Müller 2016* (Applied Sciences review): OLA "delivers high
  quality results for purely percussive signals" with "a very small frame
  length N (corresponding to roughly 10 milliseconds)"; WSOLA default
  frame ≈ 50 ms, tolerance 25 ms, and it "introduces noticeable stuttering
  artifacts" at transients; transient-preserving TSM pins Ha = Hs around
  detected transients; HPS approach = PV for harmonic, short-frame OLA for
  percussive. *TSM toolbox 2014*: OLA defaults window 256 / hop 128
  (sin²), WSOLA 1024/512/Δ512, PV 2048/512.
- *Röbel 2003*: at a detected transient, reinitialise phases and boost the
  transient bins; no need to force the stretch factor to 1 there.
- *Verhelst & Roelands 1993* (WSOLA) [citation only].
- *Robjohns, Sound on Sound 2019* (secondary): linear crossfade (−6 dB each
  at midpoint) is seamless for *correlated, time-aligned* material;
  constant-power (−3 dB) for uncorrelated takes.
- *ReCycle 2 manual*: at the original tempo "one slice will end exactly
  where another starts"; slower tempo leaves gaps; *Stretch* adds an
  artificial tail; the Preview lets tempo and pitch change independently
  — but the REX idiom itself is repitch-per-slice on a re-spaced grid,
  i.e. ours.
- *Ableton Live manual, Beats mode*: "granulation … optimized to preserve
  the transients"; Preserve = Transients or a grid division; **Transient
  Loop Mode**: Loop Off (silence to the next transient), Loop Forward
  ("jumps to a zero-crossing near the middle and loops"), Loop Back-and-
  Forth; Transient Envelope 0–100.
- *Collins BBCut `WarpCutProc1`*: geometric series with `accel` 0.5–0.99
  — our ×½ / ×2 series is the accel = 0.5 corner; the help file's range
  suggests gentler ratios are the musical norm.
- *Octatrack*: RTRG (count) and RTIM (interval "relating to the amount of
  steps") — the same two-parameter roll we expose; no documented
  crossfade on slice changes. *MPC*: 32 mute groups ("the last pad played
  will silence all other pads in that mute group") = our choke fabric.

**Recommendations.**

1. **Adopt density-scaled fades (#3).** Per slice: `fade = clamp(k /
   density[s], 48, 480)` samples (1–10 ms; Driedger's ~10 ms OLA frame is
   the natural ceiling for percussive joins, Nielsen's 1 ms his floor).
   Use it for (a) the choke ramp (`Choke()` currently fixes 240), (b) the
   edge guard when `end == natEnd`, (c) the attack ramp on *sparse* slices
   (a kick can take 2–3 ms without losing its click; a hat cannot). Keep
   the linear shape for the continuity/verbatim case (correlated), switch
   to equal-power (sin/cos or the sin² pair from the TSM toolbox) when the
   incoming and outgoing voices are different slices. *Feasibility:* one
   table lookup per voice per block; zero audible risk if the floor is
   kept at 48.
2. **Adapt Ableton's Transient Loop Mode as a Tail option (#9):** when a
   slot outlasts its slice (tempo below native or pitch up), instead of
   silence offer Loop Forward from a zero crossing in the slice's last
   quarter (with a short equal-power loop crossfade) or Back-and-Forth.
   This is ReCycle's Stretch and Ableton's Loop Forward in one option,
   and it never pre-echoes the next attack (the loop region ends before
   `natEnd`). *Feasibility:* a loop-point per slice computed at load; a
   few branches in `Render`. *Risk:* metallic buzz on tonal tails —
   ship Off as the default.
3. **Widen the ratchet ratio** to Collins' 0.5–0.99 range: a `ratchetRatio`
   (e.g. 0.6–0.8) instead of fixed ×½/×2 gives Squarepusher-style rolls
   that don't collapse to the 1.5 ms floor by the third hit. Trivial.
4. **Keep repitch as the default; keep Keyframe stretch as M7 Option** —
   the paper's own numbers (LAT worst at 0.5×, "fair" MUSHRA, ~1.5× the
   float buffer for keyframes at our densities per `keyframe-lessons.md`)
   argue for an option, and Driedger's review says short-frame OLA already
   does well on pure percussion, which suggests a *plain* 10 ms OLA
   tempo-without-pitch mode might be an even cheaper first Option than a
   keyframe reader: ≈ 4 cycles/sample, no representation change, and
   transient-aligned by construction if the OLA grain boundaries snap to
   our onset table (Driedger's "pin Ha = Hs at transients"). Worth a host
   experiment before committing to vendoring `Granule`.
5. Continuity window (160 samples) and rate tolerance (2 %) are ours, not
   the literature's; no source contradicts them. The extrapolated clock
   fix (§2.6) was the actual cause of the stutters — keep it.

### 2.6 Clock: NCO, PLL follower, extrapolation, multiplier

**What we do.** SDK `MusicalClock` (96 PPQN NCO, ticked at 1 kHz),
`ClockFollower` PI-PLL (EMA α 0.15 on the period, Kp 2.0, Ki 0.05,
integrator clamp ±48 ticks, lock after 3 valid pulses, loss at 3× period,
glitch reject at ¼ of the fastest expected pulse); audio thread
extrapolates `tick + tpu·µs` from a double-buffered snapshot capped at 50
ms; `barSeconds` derived per block from the same `tpu`; a half-step
monotonic guard; P2 = ÷4…×4 multiplier with 0.02 hysteresis, engaged on a
3 % move; PPQN dropdown 24/4/2/1; NCO restarted at the pre-clock tempo when
a clock is lost before lock.

**Sources.**

- *E-RM 2014, "The Impact of Clock Jitter on MIDI syncable DAWs"*: a DAW
  master (Ableton) sending 24 PPQN MIDI clock at 120 BPM (ideal tick
  20.8 ms) showed cycle-to-cycle jitter peaks of **−38 ms** (σ 8.4 ms);
  "C2C jitter obviously exceeds the lock-range of the slave PLL, which
  gets unlocked … and cycles start to slip"; "The PLL unlocks on a lost
  MIDI Tick and locks again afterwards, in an unknown phase state";
  replacing the master with a 400 ns-jitter hardware clock cut beat
  offset spread from 10 ms to 210 µs.
- *Ableton Link* (README + `HostTimeFilter.hpp`, `GhostXForm.hpp`): beat
  ↔ time as a `(beat, time, tempo)` bijection; phase alignment via a
  *quantum*; jitter handled by a **linear regression over 512 (sample
  time, host time) pairs**, refit every call; no PPQN.
- *Dannenberg 2017* (ICMC, "Time-Flow Concepts"): "forward synchronous"
  scheduling — time-stamp events in a control stage, delay by a time
  advance L in the rendering stage "to mask the timing jitter", deliver
  late events ASAP rather than add "excessive delay". Our snapshot-and-
  extrapolate is precisely this pattern.
- *Large & Kolen 1994* [citation only; T&F 403]; *Large & Snyder 2009*
  (fetched): adaptive oscillator whose phase is coupled to onsets, with
  systematic anticipation. Perceptual model, event-driven, not tick-
  driven; useful vocabulary (phase coupling vs period adaptation), not a
  design.
- Standards: *DIN Sync* 24 PPQN (E-RM 2014 DIN-sync report: tested Roland
  gear synced to 5 ms-wide pulses up to 300 BPM; raise Run ≥ 9 ms after
  the first tick); *Korg volca*: 15 ms 5 V pulse per **step** (16ths);
  *Arturia KeyStep* sync menu: 1step (Gate) / 1step (Clock, interpolated)
  / 1 Pulse (Korg) / 24 PPQ (Roland) / **48 PPQ (Oberheim)**; *Pamela's
  NEW/PRO Workout*: follows 1–48 PPQN, "Always use as higher PPQN value as
  possible (24 recommended) for best sync accuracy", "If an external clock
  is sloppy then Pam will be too! It can't fix an external clock",
  multipliers /16384 … ×192; whether Pam beat-aligns multiplier changes is
  **not stated**.
- Perception: *Friberg & Sundberg 1995* (JASA; Crossref abstract): JND ≈
  **6 ms** for IOIs < 240 ms, 2.5 % above; *Hirsh 1959*: 15–20 ms to
  judge order; *Wessel & Wright 2002* (CMJ): ≤ 10 ms latency and jitter
  "should not exceed 1 ms" (flam timbre changes above 1 ms); *Lago & Kon
  2004* (ICMC): 20–30 ms latency "probably perfectly acceptable", but
  sub-ms jitter needed to *capture* performer precision.
- *Grids* external clock: 4 / 8 / 24 PPQN resolution setting; RESET jumps
  to step 1 — the reset input is the Eurorack idiom we lack.

**Recommendations.**

1. **Adapt Link's regression estimator (#8).** Keep the SDK PI for phase,
   but replace the EMA period with a least-squares slope over the last
   N = 8–32 pulse stamps (ring of `(pulseIndex, µs)`), seeding
   `base_per_sec` from the slope. A single late/early MIDI tick then moves
   the estimate by ~1/N instead of 15 %, which is exactly the E-RM failure
   mode. Widen the glitch window per E-RM's data if a "DAW clock" preset
   is selected. *Feasibility:* 32-point regression at ≤ 200 Hz pulse rate
   is nothing; it lives in `OnPoll`. Needs the SDK's `SetGains`/`period`
   hooks or a subclass — Luke's call.
2. **Adopt an honest clock meter:** we already count poll gaps and jumps;
   add the pulse-period C2C histogram and show σ on the P2 ring / web
   (Pamela's "can't fix an external clock" as UI truth). Trivial.
3. **Adopt 48 PPQN and rename 2 PPQN "Korg (per step)"** in the dropdown
   (KeyStep / Oberheim / volca naming). Trivial.
4. **Adopt a RESET input semantics** (Grids/Pam): a rising edge on a
   mappable jack re-anchors `barPhase = 0` at the next block — the Eurorack
   way to fix "which bar are we in" without a downbeat detector. Needs the
   Routing page.
5. **Beat-align multiplier changes** (the user's #1): apply `gClkMult`
   only when `lp` crosses a beat boundary; already trivial given
   `s_lastMult`.
6. **Latency budget is fine.** Voice gate → sound: 1 ms poll + ≤ 0.5 ms
   block ≈ 1.5–2 ms, jitter ≈ 1 ms — inside Wessel & Wright. Gate *outs*
   are armed sample-accurately and written on the 1 ms poll; the unknown
   is the MCP4728 I²C transaction time on J3–J6 — **measure it** with a
   scope against J7/J8 (STM DAC) and document the offset (a fixed offset
   is harmless; jitter above ~1 ms is not).
7. **Ignore** adaptive-oscillator beat trackers (Large) and audio beat
   tracking (OBTAIN 2017 etc.) for the clock — we have a clock input; beat
   tracking belongs, if anywhere, to a future "follow the audio in J1"
   feature, and the ISMIR-grade methods are RNN-based.

### 2.7 Gates, velocity, microtiming

**What we do.** Per-class gates 6/20/50 ms or hold; velocity = 1.5 + 3.5 ×
salience V (salience = step RMS / loudest step); gates fire per grid step
before continuity; Collins ±1.5 % repitch jitter on repeats; no timing
jitter.

**Sources.** *Callender, Hawthorne & Engel 2020* (E-GMD): velocity
annotations, and listening tests show predicting velocity "produce[s]
outputs with improved perceptual quality". *Grids*: accent = level > 192.
*Frühauf, Kopiez & Platz 2013* [abstract only]: 93 music students, rock
pattern with kick/snare shifted −25…+25 ms — the **exactly quantised
version received the highest ratings**; early shifts worse than late; kick
shifts worse than snare. *Frane 2017*: swing lives in the 16th hats, not
kick/snare; backbeat 2 slightly late. *Hennig et al. 2011* (PLoS ONE):
human timing has long-range (1/f^β, 0.2 < β < 1.3) correlations; a song
humanised with 1/f noise (σ 15 ms) was preferred over white-noise
humanisation 64 % of the time and judged "more precise" 79 %; commercial
humanisers "generally apply white noise". *Butterfield 2010/2011*
[abstracts]: little evidence for bass–drum "participatory discrepancy"
effects; swing ratio manipulates anacrusis. *Witek 2014*: medium
syncopation peaks pleasure.

**Recommendations.**

1. **Ignore timing humanisation on kick/snare** (Frühauf). If any timing
   variation is ever added — e.g. for hat ticks in the cluster zone — make
   it 1/f (Hennig), never white, and ≤ 10 ms.
2. **Adopt Grids accent → velocity** (with #1 of §2.3), and let salience
   from the *onset table* (flux peak height) replace step RMS when the
   onset table is active: RMS conflates a long hat wash with a loud hit.
3. Gate-per-step-before-continuity is right: a continued verbatim run still
   *sounds* its hits (memory note). No source contradicts it.

### 2.8 History ring

**What we do.** 16 `{cellSeed, fillSeed, knob}` tuples; P3 scrubs; new
rolls append. Known gap: scrub replays with the *current* grammar/Brain
params.

**Sources.** *Marbles DEJA VU* (see §2.3); *Margulis 2013* (repetition,
immediate or delayed, raises ratings); Collins' `SQPusher1` fills every
fourth bar. No product with a scrubbable *generated-pattern* timeline was
found (Steppy / Zularic Repetitor **[UNVERIFIED]**); Octatrack's pattern
banks are manual.

**Recommendations.** Déjà-vu flow (#10); store `patternMode` and the Brain
vector in `PatState` (the open low-priority item) so a scrubbed slot replays
what it sounded like; pinning ("keeper") is the timeline-bank feature the
spec defers — it is where canonised mistakes live.

### 2.9 Voice role (v/oct + gate keymap)

**What we do.** Note = round(12·V), accepted only inside a ±0.4-semitone
centre window, else keep the last; decoded at each gate edge; mute key,
latched modifier keys, 16 slice keys; choke vs punch.

**Sources.** *Braids `quantizer.cc`* (and O&C's copy): keep the previous
codeword while the input stays inside `[previous_boundary_,
next_boundary_]`; boundaries are placed at 9/16 of the way toward each
neighbour (`(9·neighbour + 7·current) >> 4`), i.e. a 1/16-interval
hysteresis band each side. *disting mk4* quantizer: nearest semitone plus a
~10 ms trigger on change, no hysteresis in the quantizer (it is in the
comparator). *MPC mute groups* = choke. *Elektron Octatrack* RTRG/RTIM.

**Recommendation.** Our window rule (accept only within ±0.4 semitone of a
centre, else hold) is stricter than Braids' (accept everywhere, hold only
near boundaries); on a KeyStep this is fine because gate edges arrive with
settled CV. For continuous CV sources (a sequencer with slew, an LFO)
adopt Braids' scheme: always resolve, but with the asymmetric boundary so
a value parked on a boundary never flickers. Trivial; keep the 0.4 window
as the gate-edge sampling rule.

### 2.10 Web front end, engineering hygiene

Nothing in the literature bears on the WebSerial front end. Two engineering
items from the fetched sources: (a) capicola's `CpuLoadMeter` and 64-bit
counters (`keyframe-lessons.md` L6 — the SDK's `ClockFollower` already
notes its 64-bit tick is wrap-safe); (b) Dannenberg's advice to deliver
late events immediately rather than "add excessive delay" is what our
50 ms extrapolation cap does — document it as a design rule.

### 2.11 Spacing statistics (Coulomb gas / RMT) — is there prior art?

No. arXiv API queries for "level repulsion" ∧ {rhythm, music}, "Wigner
surmise" / "Coulomb gas" / "Dyson gas" ∧ {rhythm, music, onset}, and
"random matrix" ∧ music returned nothing musical (only MUSIC-algorithm and
a "random matrix transform" feature paper). Near-misses, all verified:
Krbálek & Šeba 2000 (Cuernavaca bus spacings → GUE), Abul-Magd 2006
(parked-car gaps, Coulomb gas → GUE), Jin et al. 2009 (queued vehicles →
GSE), Kolic et al. 2022 (IOIs via permutation entropy / visibility graphs,
no RMT). **Recommendation:** the Phase-0 dataset result (1,226 breaks,
CV median 0.575 between GOE 0.52 and Poisson 1.0; Amen 99.8th percentile)
is publishable as a late-breaking/demo paper; the firmware itself needs
nothing from it now that the gas is dormant, but a "β meter" on the web
front end (CV of the onset table's spacings) costs nothing and is a
genuinely new descriptor for a break.

---

## 3. Feasibility cheat-sheet (Cortex-M7 @ 480 MHz, 48 kHz, 24-sample blocks)

| Technique | Where it runs | Cost | Verdict |
|---|---|---|---|
| Log-filtered spectral flux, 512/256 | offline at load | one 512-pt FFT per 5.3 ms of audio; our radix-2 with per-stage `cosf/sinf` is fine offline | free |
| capicola Detector (TKEO) | live, per sample | ≈ 61 cycles/sample (Nielsen Table 1) ≈ 0.6 % CPU | free |
| Density-scaled fades | audio thread | one lookup per voice per block | free |
| Multi-label gates / NMF weights | offline at load | one-pole filters; NMF ~1–2 M MACs per loop | free |
| Regression period estimator | 1 kHz control thread | ≤ 32-point LSQ per pulse | free |
| 10 ms OLA tempo-only mode (experiment) | audio thread | ≈ 4 cycles/sample per voice (Nielsen's OLA row) | cheap |
| Keyframe stretch (Granule per voice) | audio thread | ≈ 34 cycles/sample per voice; keyframes ≈ 1.5× float buffer in SDRAM | affordable, deferred |
| Live spectral flux (if ever) | audio thread | CMSIS `arm_rfft_fast_f32` 512-pt: order of 10–20 k cycles per hop (estimate; the one public benchmark table found is ambiguous — measure) plus ≥ 5 ms frame latency | possible, not needed |
| CNN drum classifier | offline at load | tens of M MACs; no weights, no precedent | ignore |
| WSOLA per voice | audio thread | ≈ 690–4100 cycles/sample | ignore |

---

## 4. Bibliography (verified URLs; fetched 2026-09-05)

**Breakbeat cutting, rearrangement, concatenative**

- Collins, N. (2001). "Algorithmic Composition Methods for Breakbeat Science." *Proc. Music Without Walls*, De Montfort University; rev. *ARiADA* 3 (2003). https://composerprogrammer.com/research/acmethodsforbbsci.pdf
- Collins, N. (2001). "Further Automatic Breakbeat Cutting Methods." *Proc. Generative Art 2001*, Milan. https://composerprogrammer.com/research/furtherbbcut.pdf (also https://papers.cumincad.org/data/works/att/ga0121.content.pdf)
- Collins, N. (2002). "The BBCut Library." *Proc. ICMC 2002*, Göteborg. https://composerprogrammer.com/research/bbcutlib.pdf
- BBCut help files: https://raw.githubusercontent.com/nhthn/BBCut/master/HelpSource/Classes/BBCutProc11.schelp , …/WarpCutProc1.schelp , …/SQPusher1.schelp
- Collins, N. (2006). "BBCut2: Integrating beat tracking and on-the-fly event analysis." *JNMR* 35(1):63–70. [abstract only] https://api.openalex.org/works/doi:10.1080/09298210600696600
- Hockman, J. & Davies, M. E. P. (2015). "Computational Strategies for Breakbeat Classification and Resequencing in Hardcore, Jungle and Drum & Bass." *Proc. DAFx-15*, Trondheim. https://www.dafx.de/paper-archive/2015/DAFx-15_submission_47.pdf
- Hockman, J., Davies, M. E. P. & Fujinaga, I. (2012). "One in the Jungle: Downbeat Detection in Hardcore, Jungle, and Drum and Bass." *Proc. ISMIR 2012*, pp. 169–174. https://ismir2012.ismir.net/event/papers/169_ISMIR_2012.pdf
- Hockman, J. (2014). *An ethnographic and technological study of breakbeats in hardcore, jungle and drum & bass.* PhD, McGill. [record page] https://hdl.handle.net/20.500.14905/70331
- Zils, A. & Pachet, F. (2001). "Musical Mosaicing." *Proc. DAFx-01*, Limerick. https://www.francoispachet.fr/wp-content/uploads/2021/01/zils-01a.pdf
- Schwarz, D., Beller, G., Verbrugghe, B. & Britton, S. (2006). "Real-Time Corpus-Based Concatenative Synthesis with CataRT." *Proc. DAFx-06*, Montreal. https://www.dafx.de/paper-archive/2006/papers/p_279.pdf
- Schwarz, D. (2006). "Concatenative sound synthesis: The early years." *JNMR* 35(1):3–22. [abstract only] https://api.openalex.org/works/doi:10.1080/09298210600696857
- Sturm, B. L. (2006). "Adaptive Concatenative Sound Synthesis and Its Application to Micromontage Composition." *CMJ* 30(4):46–66. [citation only — full text unreachable] https://api.openalex.org/works/doi:10.1162/comj.2006.30.4.46
- Jehan, T. (2005). *Creating Music by Listening.* PhD, MIT Media Lab. https://dspace.mit.edu/handle/1721.1/42172
- Davies, M. E. P., Hamel, P., Yoshii, K. & Goto, M. (2013). "AutoMashUpper: An Automatic Multi-Song Mashup System." *Proc. ISMIR 2013*. https://archives.ismir.net/ismir2013/paper/000077.pdf
- Toussaint, G. (2005). "The Euclidean Algorithm Generates Traditional Musical Rhythms." *Proc. BRIDGES 2005*, Banff, pp. 47–56. https://cgm.cs.mcgill.ca/~godfried/publications/banff.pdf ; extended: https://cgm.cs.mcgill.ca/~godfried/publications/banff-extended.pdf
- Mutable Instruments Grids manual: https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/manual/ ; code: https://raw.githubusercontent.com/pichenettes/eurorack/master/grids/pattern_generator.h , …/pattern_generator.cc , …/resources/lookup_tables.py
- Mutable Instruments Marbles manual: https://pichenettes.github.io/mutable-instruments-documentation/modules/marbles/manual/
- Margulis, E. H. (2013). "Aesthetic responses to repetition in unfamiliar music." *Empirical Studies of the Arts* 31(1):45–57. https://collaborate.princeton.edu/en/publications/aesthetic-responses-to-repetition-in-unfamiliar-music/
- Boenn, G. (2011). *Automated analysis and transcription of rhythm data and their use for composition.* PhD, University of Bath. https://researchportal.bath.ac.uk/en/studentTheses/automated-analysis-and-transcription-of-rhythm-data-and-their-use/

**Onset detection, tempo, TSM**

- Bello, J. P. et al. (2005). "A Tutorial on Onset Detection in Music Signals." *IEEE TSAP* 13(5). https://hajim.rochester.edu/ece/sites/zduan/teaching/ece472/reading/Bello_2005.pdf
- Dixon, S. (2006). "Onset Detection Revisited." *Proc. DAFx-06*. https://www.dafx.de/paper-archive/2006/papers/p_133.pdf
- Böck, S., Krebs, F. & Schedl, M. (2012). "Evaluating the Online Capabilities of Onset Detection Methods." *Proc. ISMIR 2012*. https://archives.ismir.net/ismir2012/paper/000049.pdf
- Böck, S. & Widmer, G. (2013). "Maximum Filter Vibrato Suppression for Onset Detection." *Proc. DAFx-13*. https://www.dafx.de/paper-archive/2013/papers/09.dafx2013_submission_12.pdf
- Kaiser, J. F. (1990). "On a simple algorithm to calculate the 'energy' of a signal." *Proc. ICASSP 1990*. https://erickson.academic.wlu.edu/files/courses2020/sigproc_s2020/readings/Kaiser-IEEE19990.pdf
- Klapuri, A. (1999). "Sound onset detection by applying psychoacoustic knowledge." *Proc. ICASSP 1999*. [abstract only] https://api.openalex.org/works?search=Sound%20onset%20detection%20by%20applying%20psychoacoustic%20knowledge
- Ištvánek, M. et al. (2020). "Enhancement of Conventional Beat Tracking System Using Teager–Kaiser Energy Operator." *Applied Sciences* 10(1):379. [abstract/secondary record] https://ouci.dntb.gov.ua/en/works/4VoDkXyl/
- Su, L. & Wu, H.-T. (2016). "Minimum-latency Time-frequency Analysis Using Asymmetric Window Functions." arXiv:1606.09047. [abstract only] https://arxiv.org/abs/1606.09047
- Dixon, S. (2001). "Automatic Extraction of Tempo and Beat From Expressive Performances." *JNMR* 30(1):39–58. https://ofai.at/papers/oefai-tr-2001-19.pdf
- Gouyon, F. & Dixon, S. (2005). "A Review of Automatic Rhythm Description Systems." *CMJ* 29(1):34–54. [abstract only] https://api.openalex.org/works/doi:10.1162/comj.2005.29.1.34
- Mottaghi, A. et al. (2017). "OBTAIN: Real-Time Beat Tracking in Audio Signals." arXiv:1704.02216. [abstract only] https://arxiv.org/abs/1704.02216
- Röbel, A. (2003). "A new approach to transient processing in the phase vocoder." *Proc. DAFx-03*. https://www.dafx.de/paper-archive/2003/pdfs/dafx32.pdf ; ICMC 2003 version: https://nagasm.org/ASL/icmc2003/closed/CR1074.PDF
- Driedger, J. & Müller, M. (2016). "A Review of Time-Scale Modification of Music Signals." *Applied Sciences* 6(2):57. https://audiolabs-erlangen.de/content/05_fau/professor/00_mueller/06_projects/90_siamus/2016_DriedgerMueller_TSMOverview_AppliedSciences_ePrint.pdf
- Driedger, J. & Müller, M. (2014). "TSM Toolbox." *Proc. DAFx-14*. https://www.audiolabs-erlangen.de/content/resources/MIR/TSMtoolbox/2014_DriedgerMueller_TSM-Toolbox_DAFX.pdf
- Verhelst, W. & Roelands, M. (1993). WSOLA, *Proc. ICASSP 1993*. [citation only] https://researchportal.vub.be/en/publications/an-overlap-add-technique-based-on-waveform-similarity-wsola-for-h/
- Nielsen, M. (2026). "Keyframe Time Stretching via Extrema Sampling." *Proc. DAFx26*, Cambridge MA. https://dafx26.mit.edu/assets/papers/DAFx26_paper_21.pdf ; code: https://github.com/heavylight-industries/capicola (AGPL-3.0)
- Robjohns, H. (2019). "Q. Should I use linear or constant-power crossfades?" *Sound on Sound*. [secondary] https://www.soundonsound.com/sound-advice/q-should-use-linear-or-constant-power-crossfades

**Drum classification, extrema sampling**

- Herrera, P., Yeterian, A. & Gouyon, F. (2002). "Automatic classification of drum sounds…" *ICMAI 2002*. http://mtg.upf.edu/files/publications/ICMAI02-pherrera.pdf
- Herrera, P., Dehamel, A. & Gouyon, F. (2003). "Automatic Labeling of Unpitched Percussion Sounds." *AES 114*. [abstract] https://www.aes.org/e-lib/browse.cfm?elib=12599
- Gouyon, F., Pachet, F. & Delerue, O. (2000). "On the use of zero-crossing rate for an application of classification of percussive sounds." *Proc. DAFx-00*. https://www.francoispachet.fr/wp-content/uploads/2021/01/gouyon-00-dafx00.pdf
- Gillet, O. & Richard, G. (2004). "Automatic transcription of drum loops." *Proc. ICASSP 2004*. https://perso.telecom-paristech.fr/grichard/Publications/Icassp04_gillet.pdf
- Gillet, O. & Richard, G. (2006). "ENST-Drums." *Proc. ISMIR 2006*. https://archives.ismir.net/ismir2006/paper/000027.pdf
- Wu, C.-W. et al. (2018). "A Review of Automatic Drum Transcription." *IEEE/ACM TASLP* 26(9). https://www.open-access.bcu.ac.uk/6180/1/Wu-et-al.-2018-A-review-of-automatic-drum-transcription.pdf
- Vogl, R. et al. (2017). "Drum Transcription via Joint Beat and Drum Modeling Using Convolutional Recurrent Neural Networks." *Proc. ISMIR 2017*. https://archives.ismir.net/ismir2017/paper/000123.pdf
- Jacques, C. & Röbel, A. (2018). "Automatic drum transcription with convolutional neural networks." *Proc. DAFx-18*. https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_59.pdf
- Krzyzaniak, M. & Paine, G. (2015). "Realtime Classification of Hand-Drum Strokes." *Proc. NIME 2015*. https://www.nime.org/proceedings/2015/nime2015_147.pdf
- Callender, L., Hawthorne, C. & Engel, J. (2020). "Improving Perceptual Quality of Drum Transcription with the Expanded Groove MIDI Dataset." arXiv:2004.00188. https://arxiv.org/abs/2004.00188
- Logan, B. F. (1977). "Information in the Zero Crossings of Bandpass Signals." *BSTJ* 56(4):487–510. http://languagelog.ldc.upenn.edu/myl/Logan1977.pdf
- Kumaresan, R. & Wang, Y. (2001). "On representing signals using only timing information." *JASA* 110(5). [abstract] https://eutils.ncbi.nlm.nih.gov/entrez/eutils/efetch.fcgi?db=pubmed&id=11757932&rettype=abstract&retmode=text

**Microtiming, groove, perception**

- Frane, A. V. (2017). "Swing Rhythm in Classic Drum Breaks From Hip-Hop's Breakbeat Canon." *Music Perception* 34(3):291–302. https://shamslab.psych.ucla.edu/wp-content/uploads/sites/57/2017/01/Frane_SwingInBreakbeats_2017.pdf
- Frühauf, J., Kopiez, R. & Platz, F. (2013). "Music on the timing grid…" *Musicae Scientiae* 17(2):246–260. [abstract] https://api.openalex.org/works/doi:10.1177/1029864913486793
- Witek, M. A. G. et al. (2014). "Syncopation, Body-Movement and Pleasure in Groove Music." *PLoS ONE* 9(4):e94446. https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0094446
- Longuet-Higgins, H. C. & Lee, C. S. (1984). "The Rhythmic Interpretation of Monophonic Music." *Music Perception* 1(4). [abstract] https://api.openalex.org/works/doi:10.2307/40285271
- Hennig, H. et al. (2011). "The Nature and Perception of Fluctuations in Human Musical Rhythms." *PLoS ONE* 6(10):e26457. https://journals.plos.org/plosone/article/file?id=10.1371/journal.pone.0026457&type=printable
- Hennig, H. (2014). "Synchronization in human musical rhythms and mutually interacting complex systems." *PNAS* 111(36). [abstract] https://www.ebi.ac.uk/europepmc/webservices/rest/search?query=EXT_ID:25114228&resultType=core&format=json
- Danielsen, A. et al. (2019). "Where is the beat in that note?" *JEP:HPP* 45(3). [abstract] https://www.ebi.ac.uk/europepmc/webservices/rest/search?query=EXT_ID:30802130&resultType=core&format=json
- Butterfield, M. (2011). "Why Do Jazz Musicians Swing Their Eighth Notes?" *Music Theory Spectrum* 33(1). [abstract] https://academic.oup.com/mts/article-abstract/33/1/3/1134435
- Butterfield, M. (2010). "Participatory Discrepancies and the Perception of Beats in Jazz." *Music Perception* 27(3). [abstract] https://api.openalex.org/works/doi:10.1525/mp.2010.27.3.157
- Friberg, A. & Sundberg, J. (1995). "Time discrimination in a monotonic, isochronous sequence." *JASA* 98(5):2524–2531. [Crossref abstract] https://api.crossref.org/works/10.1121/1.413218
- Hirsh, I. J. (1959). "Auditory Perception of Temporal Order." *JASA* 31(6). [Crossref abstract] https://api.crossref.org/works/10.1121/1.1907782
- Wessel, D. & Wright, M. (2002). "Problems and Prospects for Intimate Musical Control of Computers." *CMJ* 26(3):11–22. https://opensoundcontrol.stanford.edu/files/p1-wessel-1.pdf
- Lago, N. P. & Kon, F. (2004). "The Quest for Low Latency." *Proc. ICMC 2004*. https://www.ime.usp.br/~kon/papers/icmc04-latency.pdf

**Clocks, sync, hardware**

- Rest, M. (2014). "The Impact of Clock Jitter on MIDI syncable DAWs." E-RM technical report. https://www.e-rm.de/data/E-RM_report_Jitter_02_14_EN.pdf
- Rest, M. (2014). "Implementing DIN Sync / SYNC24 / Roland Sync." E-RM technical report. https://www.e-rm.de/data/E-RM_report_HowToDinSync_10_14_EN.pdf
- Innerclock Systems, "Litmus Timing Tests." [results table only] https://www.innerclocksystems.com/litmus
- Dannenberg, R. B. (2017). "Time-Flow Concepts and Architectures for Music and Media Synchronization." *Proc. ICMC 2017*. https://www.cs.cmu.edu/~rbd/papers/timeflow2017.pdf
- Ableton Link: https://github.com/Ableton/link ; https://raw.githubusercontent.com/Ableton/link/master/include/ableton/link/HostTimeFilter.hpp
- Large, E. W. & Snyder, J. S. (2009). "Pulse and Meter as Neural Resonance." *Ann. NY Acad. Sci.* https://musicdynamicslab.media.uconn.edu/wp-content/uploads/sites/433/2016/03/LargeSnyder2009PubsAHEdits.pdf ; Large & Kolen 1994 [citation only] https://api.crossref.org/works/10.1080/09540099408915723
- ALM Pamela's NEW Workout manual: https://assets.busycircuits.com/docs/alm017-manual.pdf ; PRO Workout: https://assets.busycircuits.com/docs/alm034-manual.pdf
- Arturia KeyStep manual 1.1.2: https://dl.arturia.net/products/keystep/manual/KeyStep_Manual_1_1_2_EN.pdf
- Korg volca keys manual: https://cdn.korg.com/us/support/download/files/93f9f953416733d196f8d817bb3bc668.pdf
- Mutable Braids quantizer: https://raw.githubusercontent.com/pichenettes/eurorack/master/braids/quantizer.cc ; O&C copy: https://raw.githubusercontent.com/mxmxmx/O_C/master/software/o_c_REV/braids_quantizer.cpp
- Expert Sleepers disting mk4 manual 4.28: https://www.expert-sleepers.co.uk/downloads/manuals/disting_user_manual_4.28.pdf
- Propellerhead ReCycle 2.0 Operation Manual: https://cdn.propellerheads.se/download/files/ReCycle2Manual.pdf
- Ableton Live manual, "Audio Clips, Tempo, and Warping": https://www.ableton.com/en/manual/audio-clips-tempo-and-warping/
- Akai MPC Software User Guide 2.7: https://cdn.inmusicbrands.com/akai/ACMPC27/MpcSoftwareManual.pdf
- Elektron Octatrack MKII manual OS 1.40A: https://www.elektron.se/wp-content/uploads/2024/09/Octatrack-MKII-User-Manual_ENG_OS1.40A_210414.pdf

**Spacing statistics (near-misses)**

- Krbálek, M. & Šeba, P. (2000). arXiv:nlin/0001015. https://arxiv.org/abs/nlin/0001015
- Abul-Magd, A. Y. (2006). arXiv:physics/0510136. https://arxiv.org/abs/physics/0510136
- Jin, X. et al. (2009). arXiv:0803.2619. https://arxiv.org/abs/0803.2619
- Kolic, D. et al. (2022). "Rhythm and form in music: a complex systems approach." arXiv:2207.03602. https://arxiv.org/abs/2207.03602

**Could not be verified (do not cite as fact)**

Rzepka & Miśkowicz 2013 (extrema recovery; no page reachable); Bjorklund's SNS technical note (ORNL server timed out); Sayiner et al. 1996 and Kedem 1986 (citations only); Gillet & Richard 2008 TASLP (citation only); Sturm CMJ 2006 / MATConcat content; Large & Kolen 1994 text; any "Hockman salience tiers" (no such concept in his fetched papers); any Collins statement of "repitch the repeats" as a cutter rule; a Hockman paper titled "Analysis of tempo and beat in breakbeat music" (appears not to exist); Akai MPC *Chop Shop* (legacy) documentation; how Mutable's Grids map was actually trained (forum thread unreachable; manual says only "machine learning techniques"); whether Pamela's Workout beat-aligns multiplier changes; any microcontroller drum-hit classifier; the CMSIS-DSP FFT cycle table at github.com/PY1CX/Cortex-M-FFT (columns ambiguous — measure ourselves); Frühauf 2013 and Witek 2014 exact statistics beyond their abstracts/summaries.

---

## 5. What we already do that the literature endorses

- **Grid-anchored rearrangement.** Jehan 2005: shuffling beat-segment
  pointers keeps the beat extractable; Collins 2001/2002: cuts addressed
  by phrase position modulo the unit; Hockman 2012: producers "mostly
  apply downbeat-preserving transformations" and breakbeats "almost
  invariably begin with kick drums" (our slice 0 is always the downbeat
  slice with infinite salience).
- **Odd-cut figures, 3+3+2, stutter terminators, WarpCut rolls, fills
  every fourth bar.** All verified verbatim in Collins 2001 (both papers)
  and BBCut's `SQPusher1` — our Collins grammar is a faithful reading,
  with the one attribution correction in §2.3.8.
- **Power-of-2 cell tiling on a Farey/Brocot grid.** Boenn's thesis and
  book use filtered Farey sequences as the rhythmic grid; Toussaint shows
  our Bresenham-form Euclid is literally the Euclidean algorithm.
- **Repitch, not stretch, as the default.** Hockman & Davies 2015's
  resequencing works in the repitch idiom; ReCycle's REX model is slices
  on a re-spaced grid; Driedger & Müller list WSOLA's transient
  stuttering and PV's smearing as the costs of the alternative.
- **Slice starts at the attack, gate honestly, choke on new attack.**
  ReCycle ("one slice will end exactly where another starts"), MPC mute
  groups, Ableton's "Loop Off" transient mode, and Nielsen's max-splice
  limit all agree that the transient owns its slot and tails must not
  pre-echo the next hit.
- **Spectral flux with a median threshold and waveform refine.** Dixon
  2006 and Böck 2012 both put plain SF within a few F-points of anything
  else on percussive material; Bello 2005 recommends median-based
  adaptive thresholds and ~100 ms windows (ours: ±8 frames × 5.3 ms ≈
  90 ms).
- **Band-energy class labels with Hockman's edges.** 50–325 Hz / 430–
  5000 Hz are his measured bass and snare regions; Gillet's bands and
  Herrera's ReliefF ranking both put low-band ratios and decay ZCR-like
  features (our density lens) at the top of the cheap-feature list;
  Wu's review names band-pass + threshold the real-time pattern.
- **Salience → velocity.** E-GMD shows velocity is perceptually load-
  bearing; Grids' accent bit is the same idea in one comparison.
- **Clock: snapshot + extrapolate, deliver late events immediately.**
  Dannenberg's forward-synchronous scheduling; Link's regression filter
  is the same instinct one step further. Our restart-on-loss-before-lock
  is Pamela's freewheel.
- **1.5–2 ms Voice latency, ~1 ms jitter.** Inside Wessel & Wright's
  10 ms / 1 ms bounds and far inside Lago & Kon's 20–30 ms.
- **No synthetic microtiming.** Frühauf 2013's quantised-is-best result
  supports our decision to jitter pitch (±1.5 %, Collins' 0.986–1.014)
  rather than time.
- **Repetition as a virtue.** Margulis 2013 (inserted repetition raises
  ratings) supports the repeat move, the fill/snap-back hyperperiod, and
  the "second-time-intentional" principle behind the planned motif
  memory.
