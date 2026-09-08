# CLASSIFY package — the Adaptive lens, and a ground truth to measure lenses by

Branch `pkg3/classify`. Owns the mode-4 lens in `core/bb_classify.h` (and
its fitted anchors, `core/bb_classify_anchors.h`), the onset-table argument
of `ClassifySteps`, the ground-truth harness (`tests/classify_bench.cpp`,
`tests/classify_bench.sh`, `tests/neural/adt_corpus.py`, `idmt_truth.py`,
`compare_adt_truth.py`, `adapt_fit.py`), the CLASSIFY section of
`tests/engine_check.cpp`, the Adaptive line of `tests/classify_check.cpp`,
and the shell hooks marked `/* CLASSIFY */` in `src/audiothurgist.cpp`.

Ernest's brief: "our slice algos are great at finding the hits, and very
mediocre at classifying them." The four lenses judge a step by the energy of
the WHOLE step, so a snare's ring, a kick's tail and the hat carpet all vote
in the next step's decision, the band edges are absolute, and — the measured
fact behind the feeling — Hockman calls **a third of the snares hats** (on
the corpus sample 1,032 of 2,483 snare steps read as hat). What ships:

| Option | Values | What changes |
|---|---|---|
| **Classifier** (byte 0) | **Hockman 3-band** · Legacy 120/900 · Two-band · Density · **Adaptive (onset, clustered)** — new | the ring's labels, swaps, Dominant-class gates. Default unchanged. Salience, the −50 dB silence rule and the GATES band mask are the common rule under every lens. |

## 1. The lens

Each step is judged **where its drum is**, not over the step:

1. **Window placement.** The engine's own hit rule (an onset in the first
   quarter of the step, or within 10 ms before the next step's start, opens
   that step; the earliest wins) puts the window on the hit. A step the
   detector left bare gets a look of its own: the biggest short-term energy
   rise inside its first three quarters (128-sample blocks, ≥ +4.8 dB over
   the block before — the quiet hat the detector missed) or, failing that,
   the step start. `ClassifySteps` gained a trailing `onsets, nOnsets`; the
   shell passes `gPendOnsets` (the material's table at every call site);
   modes 0–3 ignore it.
2. **Bleed subtraction.** Ten milliseconds before the hit are measured too
   and taken off every band, so what the previous hit left ringing does not
   vote (floor: 5 % of the window's own energy).
3. **Two windows**, because a snare's attack is broadband noise like a
   hat's and its body arrives later: *early* = the first 40 ms, *full* =
   from the hit to the next hit, at most 250 ms.
4. **Eighteen cheap features**: six log-spaced band shares in each window
   (< 100, 100–250, 250–600, 600–1.5 k, 1.5–4 k, > 4 kHz; differences of
   cascaded one-pole pairs, 12 dB/oct each side) and their band centroids,
   zero crossings of the > 100 Hz signal and the crest factor of the early
   window (Gouyon, Pachet & Delerue 2000; Herrera et al. 2002), the decay
   (late energy over early) and the tail brightness (late centroid − early).
   A step whose next hit comes too soon has no decay measure and sits at
   the corpus mean for those two.
5. **Anchors, then a shrunk k-means.** The features are scaled by corpus
   statistics and weighted; in that space three *anchors* — the training
   sample's mean kick, snare and hat — seed a k-means over the steps that
   open with a hit. Each of three rounds moves a centroid **halfway** from
   its anchor toward its members' mean and never further, so the clusters
   follow this break's drums (a tuned 80s kick with a click, a brushed
   snare, a ride instead of a hat) but cannot wander into another class's
   territory. Clusters are named by the anchor nearest their centroid,
   ties by seed. **Two-class guard:** a cluster with under 5 % of the hits
   (two at least) dies and its members go to the nearest live centroid — a
   kick/snare loop grows no hat class (host test). Decay-only steps are then
   labelled by nearest live centroid. Under six hit steps every sounding
   step votes; under six sounding steps the Hockman labels stand.

Pure C++17, static scratch (~10 KB), no allocation, `Yield()` every four
steps. Host, `-O2`: 6 ms for the 4-bar Amen (64 steps), 11 ms for a 20 s
file, against 0.9 / 2.0 ms for Hockman and 3.6 / 7.8 ms for Density — so
about 1.7× the Density lens; on the M7 expect tens of milliseconds, well
under one onset detection, and it yields to the clock throughout.

### What was tried first, and measured worse

The brief's design — standardise over the loop's sounding steps, k-means
from tercile seeds on a brightness axis, name by centroid order — was built
first. Measured on hit steps against truth it was *below Hockman on every
set* (IDMT 60 % vs 82 %; canon 65 % vs 73 %), and so was a hierarchical
variant (kick vs rest on the low band, then snare vs hat on brightness: 78 %
/ 72 %). Two causes, both visible in the feature dumps: a loop of 20–60
noisy hits cannot rediscover what a kick *is* (per-loop standardisation
throws away the absolute knowledge a fixed 325 Hz edge carries), and any
k-means over decay-only steps splits "hit vs decay", not "kick vs snare".
Hence the anchors (the prior) with the shrunk update (the adaptation), and
a population of hit steps only. A supervised linear rule on the same
features (5-fold LDA on the training sample) sits at 77 % there, the lens
at 78 % — the features, not the clustering, are now the ceiling.

## 2. Ground truth

The corpus has no labels. Two truths, one measured against the other:

- **IDMT-SMT-Drums** (Dittmar & Gärtner 2014; zenodo 7544164, CC BY-NC-ND):
  95 drum loops — real kits, drum machines, sampled kits — with every
  kick, snare and hi-hat onset annotated **by hand**. The only real truth.
- **ADTOF** (Zehren, Alunno & Bientinesi 2021/2023; xavriley's PyTorch port,
  MDBDrums++ F 88.5), a CRNN drum transcriber trained on 359 hours of real
  full mixes, run on the RTX 4090 over all 1,230 corpus files (3 minutes)
  and over the IDMT mixes. Against the hand annotations (±30 ms): kick F1
  95 %, **snare 82 % (precision 72 %)**, hat 91 %. It over-reports snares.
  Its toms map to snare, crash/ride to hat (`tests/neural/README.md`).

`tests/classify_bench.cpp` runs each file through the firmware's own load
pipeline (condition → onset table → bar estimate → slice count, as
`ServiceLoad`) and gives every step the class of the truth hits inside it,
kick > snare > hat when they coincide (`--strongest` takes the highest
activation instead: 1–2 points lower for every lens, same ordering); a hit
within 10 ms before a boundary belongs to the next step (the engine's
rule). Silent = no truth hit. It prints per lens a 4 × 4 confusion, P / R /
F1 per class and three accuracies. Measurement sets: IDMT (manual), the
45-file tempo truth set and a fixed random 200-file corpus sample
(`--sample 200 --seed 1`), both against ADTOF. The anchors were fitted on
a **different** random sample (`--seed 2`, 147 files after removing every
file that appears in a measurement set).

### Results — accuracy on steps that carry a truth hit

The number that answers the brief: of the steps where a drum was actually
hit, how many get its name. "Onset found" = the engine's detector put a hit
on the step (the lens judges those at the hit); "no onset" = it did not
(the lens looks for the rise itself, then the step start).

| set (truth) | steps | Hockman | Legacy | Two-band | Density | **Adaptive** |
|---|---|---|---|---|---|---|
| IDMT-SMT-Drums (manual), onset found | 2,124 | 81.7 % | 71.0 % | 63.4 % | 50.2 % | **83.3 %** |
| IDMT, no onset | 1,053 | **85.7 %** | 77.4 % | 75.6 % | 57.6 % | 75.2 % |
| tempo truth set, 45 files (ADTOF), onset found | 1,132 | 72.7 % | 58.8 % | 60.3 % | 58.9 % | **79.5 %** |
| tempo truth set, no onset | 400 | 68.0 % | 55.5 % | 57.5 % | 56.2 % | **74.0 %** |
| 200-file sample (ADTOF), onset found | 3,846 | 66.4 % | 57.4 % | 50.8 % | 48.9 % | **75.8 %** |
| 200-file sample, no onset | 2,367 | 64.2 % | 57.5 % | 48.5 % | 53.0 % | **72.8 %** |

### Results — per class, all sounding steps

Precision counts the decay steps too (the −50 dB silence rule calls ~40 %
of steps sounding that the truth calls silent — a step carrying a snare's
ring is "snare" to every lens and "silent" to the truth), so read recall
and F1 across lenses rather than precision alone.

**IDMT-SMT-Drums, 94 files, 5,760 steps (manual truth)**

| lens | K P / R / F1 | S P / R / F1 | H P / R / F1 | macro-F1 | both-sounding acc. |
|---|---|---|---|---|---|
| Hockman | 54 / 90 / 67 | 43 / 70 / 54 | 41 / 85 / 56 | 58.8 | **83.0 %** |
| Legacy | 52 / 84 / 64 | 35 / 39 / 37 | 35 / 86 / 50 | 50.3 | 73.2 % |
| Two-band | 54 / 90 / 67 | — | 29 / 92 / 44 | 37.0 | 67.4 % |
| Density | 36 / 54 / 43 | 17 / 26 / 20 | 31 / 70 / 43 | 35.4 | 52.7 % |
| **Adaptive** | 42 / **94** / 58 | 43 / **82** / **56** | **51** / 66 / **58** | 57.4 | 80.6 % |

**Tempo truth set, 45 files, 2,240 steps (ADTOF truth)**

| lens | K P / R / F1 | S P / R / F1 | H P / R / F1 | macro-F1 | both-sounding acc. |
|---|---|---|---|---|---|
| Hockman | 58 / 82 / 68 | 51 / 44 / 48 | 41 / 86 / 56 | 57.0 | 71.5 % |
| Legacy | 54 / 75 / 63 | 25 / 11 / 15 | 33 / 84 / 48 | 41.9 | 58.0 % |
| Two-band | 58 / 82 / 68 | — | 32 / 92 / 47 | 38.3 | 59.6 % |
| Density | 54 / 69 / 61 | 28 / 30 / 29 | 36 / 74 / 48 | 45.9 | 58.2 % |
| **Adaptive** | 53 / **91** / 67 | 50 / **80** / **62** | **60** / 61 / **61** | **63.0** | **78.1 %** |

**Random 200-file sample (seed 1), 196 files, 9,376 steps (ADTOF truth)**

| lens | K P / R / F1 | S P / R / F1 | H P / R / F1 | macro-F1 | both-sounding acc. |
|---|---|---|---|---|---|
| Hockman | 52 / 79 / 63 | 58 / 43 / 49 | 30 / 84 / 44 | 51.8 | 65.5 % |
| Legacy | 52 / 77 / 62 | 50 / 25 / 34 | 25 / 82 / 38 | 44.7 | 57.4 % |
| Two-band | 52 / 79 / 63 | — | 22 / 89 / 35 | 32.7 | 49.9 % |
| Density | 45 / 58 / 51 | 35 / 30 / 32 | 25 / 75 / 37 | 40.0 | 50.5 % |
| **Adaptive** | 48 / **87** / 62 | 52 / **78** / **63** | **48** / 50 / **49** | **57.6** | **74.7 %** |

With Options → Onsets = Log-filtered flux on the sample: Hockman 64.8 %,
Adaptive 70.2 % of both-sounding steps (the mode-1 table changes the grid
on a few files; the ordering holds).

### Reading it

- On breaks — the module's material — the Adaptive lens is ahead of
  Hockman by **7–9 points** on hit steps and 6–9 on all sounding steps,
  and the gain is where the brief pointed: **snare recall 43 → 78 %** on
  the sample, 44 → 80 % on the canon breaks, with hat precision up (30 →
  48 %) because snares stop landing in the hat lane. Kick recall rises too
  (79 → 87 %). The price is hat recall (84 → 50 % on the sample): a hat
  riding a kick or snare reads as that drum (the truth's own priority rule
  says so as well), and a quiet hat between hits, on a step the detector
  left bare, is sometimes the previous drum's ring to this lens where
  Hockman's loop-mean normalisation lets the high band win.
- On IDMT — clean studio kits, not breaks — the two are level on hit steps
  (83 vs 82 %) and Hockman is ahead on the no-onset steps (86 vs 75 %),
  where the truth is nearly always a lone quiet hi-hat: the whole-step
  rule reads the carpet, the onset rule reads what rings. Overall IDMT
  both-sounding: 83.0 vs 80.6 %. That is the honest limit of the design:
  it judges the hit, and a step with no hit has no hit to judge.
- Legacy and Two-band have no snare class by construction; Density is the
  weakest lens against every truth — its rank split assigns fixed shares
  (30 / 25 / 45 %) whatever the break has.

### Where the truth itself looks wrong

- ADTOF hears **snares it invents** (precision 72 % on the hand
  annotations; 546 false snares on 1,499) — on the corpus, some of what the
  tables call a lens's "missed snare" is the transcriber's. It also runs
  the hi-hat/ride at "hat" (6,448 crash/ride + 27,544 hi-hat labels over
  the corpus) and toms at "snare" (4,063), by our mapping.
- The step truth is a priority, not a loudness: a ghost kick under a loud
  hat makes the step "kick". `--strongest` (activation) shifts every lens
  1–2 points down and changes no ordering.
- Truth hits sit early: on IDMT, 592 of 4,437 hits lie 10–16 ms before a
  step boundary after the 10 ms lead (the grid comes from the conditioning
  cut, the annotations from the ear). `--lead 15` moves them into the next
  step and changes both-sounding accuracy by under 1 point.
- ~40 % of all steps are "silent" to the truth and "sounding" to every lens
  (the −50 dB rule): unchanged by this package, deliberately — those steps
  still play, swap and gate, and labelling them by the ringing drum is
  what the ring shows today.
- The Amen's first bar reads `K..K..K.SK.S.K.K` in truth (the ride is
  "hat", the intro bar differs from the loop's), Funky Drummer's `.K.KHKSH…`
  — plausible at a glance; nobody listened to 1,230 files. The 45-file
  canon table and the sample agree with the hand-annotated one on the
  ordering of the lenses, which is what the measurement is for.

## 3. Firmware

`src/audiothurgist.cpp`: `kClassDisp` gains "Adaptive (onset, clustered)"
and the descriptor's enum count goes 4 → 5; the `ClassifySteps` wrapper
passes `gPendOnsets, gPendN` (the material's table at every call site:
load, set load, Bars / Swing / Onsets re-apply, boot, and a Classifier
switch on the playing material). `AnalyseRegion` (a key take) uses the
Hockman edges for mode 4 as it does for 0 and 3. Manual: one clause in the
Options section (eight sections, none added). Options schema untouched
(OPT6, 22 bytes). Default stays Hockman.

`arm-none-eabi-size`: text 366,540 → 370,700 B (+4,160), data 4,384 →
4,484, bss 39,231,160 → 39,241,336 (+10,176: the lens's static scratch).

## 4. Tests

`tests/run.sh` ALL GREEN, Classic grammar hash unchanged, 386 engine checks.

- `engine_check.cpp`: the lens loops run all five lenses (classes in 0..3,
  silent step class 3, salience; the GATES mask lens-independent under
  five lenses; classes and salience identical with and without the mask).
  New section **CLASSIFY**: the same loop twice → identical labels and
  salience; pure kick / snare / hat steps of a synthetic loop read K / S /
  H and the quiet hat carpet reads hat (the GATES loop's snare — a 1.8 kHz
  tone plus white noise — reads as a hat to anchors fitted on real breaks,
  so this loop's snare has a 200 Hz body under low-passed noise); salience
  equals Hockman's; without an onset table the self-found rises give much
  the same labels; **a kick/snare loop grows no hat class**; modes 0–3
  are byte-identical with and without an onset table.
- One-off verification (not in the suite): a program printing classes,
  band masks, salience and density for modes 0–3 was built against the
  base commit's header and this one and run on the 45 canon files —
  identical output.
- `classify_check.cpp` prints the Adaptive line and, under it, which steps
  were judged at a detected hit (`h`), a self-found rise (`r`) or the step
  start (`.`).
- `tests/classify_bench.sh` prints the three tables above (≈ 1 min; needs
  the truth CSVs, `tests/neural/README.md`).

## Testing on hardware

1. Load the Amen. Options → Classifier = Adaptive. The ring: kicks on the
   downbeats, the backbeat snares, hats on the off-8ths; the tell is the
   snare on 2 and 4 that Hockman paints as a hat on some bars. Flip back to
   Hockman: the labels change, the pattern does not (a Classifier switch
   re-labels the playing material without regenerating).
2. Brain map, Gate Lanes = Dominant class, an envelope on J7: under
   Adaptive the snare lane fires on the backbeat, not the hat lane. Under
   Multi-label the lanes do not change with the lens (GATES law).
3. A break with no hats (a kick/snare loop): no step turns hat-coloured.
4. Reslice (B3) and a Bars override re-run the lens on the new grid; a
   load takes a few tens of milliseconds longer under Adaptive, with the
   playhead running (Yield).

## Open questions

- **Hats on bare steps.** The lens reads the ringing drum where Hockman
  reads the carpet. A hybrid — the whole-step high-band rule only for
  steps with no hit and no rise — would take the IDMT no-onset column back
  toward 86 % at little cost; untested against the breaks' tables.
- **Onsets = 1 as the table** helps the bar estimator (SLICE) and would
  put more hits on the steps the lens judges; the sample shows the same
  ordering under it. Ernest's default call.
- **Anchors from breaks only.** The fit sample is corpus breaks with
  neural labels; IDMT's kits are near the anchors anyway (83 % on hit
  steps), but a fit on both truths, or a per-loop *offset* of the feature
  means (measured +1 on IDMT, −0.3 on the canon set), is a knob left
  unturned.
- **Cost.** ~1.7× the Density lens per load; the full window (≤ 250 ms
  per step) is the bulk of it. A whole-loop filter run would cost the same
  order; halving the full window to 125 ms was not measured.
- **The silence rule** is shared and untouched; a lens-4-only "no hit, no
  rise → silent" would change what swaps, gates and the ring do on decay
  steps — a behaviour change beyond a label, left for a decision.

## Addendum 2026-09-08: refitting the anchors on the whole corpus

Asked whether the anchors should come from more than the 147-file training
sample, the fit was repeated on every corpus file outside the measurement
sets (971 files, 19,713 hit steps) with `adapt_fit.py`. Hit-step accuracy,
shipped anchors → whole-corpus anchors: IDMT 83.3 → 82.2 %, canon 45
79.5 → 78.5 %, sample 200 75.8 → 75.2 %; no-hit steps moved the same way.
On the training set itself the weighted nearest-anchor rule reaches 76.8 %
and a 5-fold LDA on the same 18 features 76.4 % — the supervised ceiling of
these features under the ADTOF truth. More breaks do not help: the fit is
saturated, and the remaining error is the features and the truth's own
noise (ADTOF snare precision 72 %). The 147-file anchors stay.
