# SLICE package — onsets, bars, swing (2026-09-06, branch `pkg/slice`)

Three behaviours from the literature review, each behind the Options field
that already existed for it. Defaults are unchanged: everything below is off
until Ernest turns it on, except Bars **Auto**, which was already the default
and now has a better estimator behind it.

| Option | Values | What changes |
|---|---|---|
| **Onsets** (byte 15) | 0 Spectral flux · **1 Log-filtered flux (Böck 2012)** | the onset table (hits, gates, swing/bar analysis) |
| **Bars** (byte 7) | **Auto** · 1 · 2 · 4 · 8 | Auto = IOI-cluster bar estimate with kick-band downbeat; overrides untouched |
| **Swing** (byte 14) | 0 Straight 16ths · **1 Auto (the material's swing)** | odd-16th slice boundaries AND odd-step trigger times move to the measured offset |

Files: `core/bb_onset.h` (mode 1), `core/bb_bars.h` (rewritten), `core/bb_condition.h`
(whole-bar cut), `core/bb_swing.h` (new), `core/bb_engine.h` (swing fields near
`stepHit`, swung boundaries in `SetMaterial`, phase→step map in `Process`; all
inside `/* SLICE */` comments, straight path byte-identical), `src/audiothurgist.cpp`
(hooks inside `/* SLICE */` fences), `tests/engine_check.cpp` (four new
sections appended), `tests/bars_check.cpp` (rewritten: the firmware pipeline),
`tests/bars_truth.txt`, `tests/bars_corpus.sh`, `tests/run.sh` (builds
bars_check).

## 1. Onsets: log-filtered flux + Dixon peak rule

Mode 1 sums the 512-pt magnitudes into ~55 geometric bands (ratio 1.10, at
least one bin per band), compresses `log(1 + 10·|X|)`, and takes the
half-wave-rectified band difference (Böck, Krebs & Schedl 2012). Peaks obey
Dixon 2006's two conditions: a local maximum over ±6 frames (±32 ms) *and*
above the mean over [n−18, n+6] frames by δ = 0.35 × the global mean. Same
FFT, hop, 35 ms minimum gap and salience output as mode 0; offline at load.

Mode 0 is byte-identical to v0.8.0: the test hashes its onset table (positions
and saliences) on the built-in test break and on the tone grid against values
computed from the untouched header.

While validating I found a latent v0.8.0 timing defect: the flux peak at frame
f means the transient lies in that frame's window [f·hop, f·hop+512), but the
waveform refine only searched ±hop around f·hop. An onset in the second half
of the frame finds no energy rise in the search window and is reported at the
window's first sample — up to 10 ms early, and on a 16th grid it hits every
hat whose position falls in the second half of a frame (in the synthetic test,
3 of every 8 hats, all 12 ms early). Mode 0 keeps the bug (byte identity);
mode 1 searches the whole window and keeps the frame position when nothing
rises. Synthetic 100 BPM break with hats at −30 dB under kicks and snares:

| | onsets | recall (±10 ms) | false | median error |
|---|---|---|---|---|
| mode 0 | 32 | 20/32 (12 hats 12 ms early) | 0 | 0.3 ms |
| mode 1 | 32 | 32/32 | 0 | 0.3 ms |

On real breaks mode 1 is more selective on busy material (Funky Drummer 100 →
78 onsets, Apache 104 → 82, Kissing My Love 112 → 91) and finds more on the
Amen (49 → 61). Feeding mode-1 onsets to the bar estimator improved it (table
in §2), which is the strongest evidence I have that the table is better; there
is no onset ground truth in the corpus.

## 2. Bars Auto: IOI clustering + kick-band downbeat

`bb::EstimateBar` (bb_bars.h) replaces the v0.8.0 consecutive-snare histogram:

1. **Dixon 2001 clustering.** Every pair of onsets 50 ms–2.5 s apart votes,
   weighted by both saliences, into a 5 ms histogram smoothed over ±2 bins
   (a 25 ms cluster); near-integer multiples reinforce (f(k) = 6−k for k ≤ 4,
   1 for 5..8). The six top clusters are the hypotheses — but a cluster may
   be a 16th, an 8th, a dotted 8th, a beat or a half bar.
2. **Candidates.** Each top cluster read as k 16ths (k ∈ 1,2,3,4,6,8,12,16,32)
   gives a bar inside 60–220 BPM; candidates within 3 % merge. Each is refined
   against the onsets by a weighted least-squares fit of an **8th-note** grid
   (phase search over 24 sub-phases, three LS rounds), because swing never
   touches 8ths. Fit = on-grid salience ÷ (on-grid + off-grid), with onsets
   in the odd-16th zone (0.35–0.85 of the 8th: straight or swung off-beats)
   neutral. Candidates with fit < 0.60 or fewer than 8 on-grid onsets are
   dropped — this is what kills 4/3 and 3/2 aliases.
3. **Octave and downbeat (Hockman, Davies & Fujinaga 2012 in spirit).** Per
   onset, the energy *rise* across it (40 ms after minus 20 ms before) is
   split into < 120 Hz, 150 Hz–3 kHz and > 4 kHz; the signed kick−snare share,
   centred on the file's mean, is a contrast that survives 70s mixes where a
   snare has plenty of sub-120 Hz and a kick plenty of mids (separately
   normalised kick/snare profiles were nearly identical — the first failure
   mode). The 16-step contrast profile D and salience profile E are rotated
   over the eight *even* rotations (an odd rotation parks every real 8th on
   an odd 16th — the doubled tempo's favourite disguise) and scored
   `2·(D0 + ½D8 − D4 − D12 − ½·snare-on-off-8ths) + ½E0`: kick on 1 (and 3),
   snares on 2 and 4, something on the downbeat. A doubled tempo puts the
   snare on "3" or a kick on "4" with an empty "1"; a halved tempo puts the
   snares on the off-8ths. Bar-to-bar profile alternation is penalised
   (agreement is not rewarded — a halved tempo's bars agree by construction)
   and tempos outside 70–190 lose 0.1.
4. **Confidence gate.** The winner must beat the runner-up by `kConfMin`
   (0.12) or the function returns nothing and the caller keeps the prior.

`bb::ConditionBuffer` now cuts the loop to whole bars of *that* bar when the
estimate is confident: starts at the downbeat when the file has slack before
it, keeps 1/2/4 bars, leaves a file that already is N bars alone (a 16th of
slack, taken back from the trailing-silence trim if needed), and **verifies
the cut** by re-estimating on the trimmed window's own onsets (shifted, no
second detection) — a dissenting second opinion vetoes. Files over 9.5 s get
a second look at their first 8 s. When nothing is confident the v0.8.0 path
runs unchanged (4-bar cap by the duration prior, folded-median IOI snap with
the phase-concentration test). The shell takes the conditioning's verified
bar count first, then a confident estimate whose bar count for the buffer is a
power of two within 6 %, then the prior — and the boot load now takes the
same rule (v0.8.0 sliced the boot sample by the prior alone).

### Truth set

No BPM metadata exists in the corpus (no `acid` chunks; 68 `smpl` chunks carry
loop points only; the Phase-0 CSV has no tempo column). Truth comes from Frane
2017, Table 1 (30 canon breaks, BPM measured on the original recordings); 48
corpus files are rips of those recordings. An independent numpy check
(2048-pt STFT, 40 log bands, comb-autocorrelation, octave-agnostic) confirmed
each file's periodicity family against Frane's tempo within 5.5 % for 45 of
them and gives the per-file rip speed (vinyl/CD rips run a few percent off);
three files whose family contradicts the listed tempo at every octave were
dropped as different passages (Theme From The Planets Parts 1/2 at 104 BPM,
Fool Yourself at 95.5 — a shuffle). `tests/bars_truth.txt` carries both
columns; "correct" below means within 4 % of the rip-adjusted tempo, judged
on the **grid the engine ends up with**, through the whole load pipeline
(condition → detect → estimate → slice count), files capped at 25 s as on the
module. Sped-up = the WAV resampled by the factor (varispeed), truth × factor.

### Results (45 files, onset mode 0 = default)

| speed | prior correct | v0.8.0 backbeat grid (spoke / wrong)* | **new grid correct** | new spoke | wrong-confident |
|---|---|---|---|---|---|
| 1.0× | 39 | 34 (16 / 2) | **39** | 28 | 2 |
| 1.25× | 41 | 38 (21 / 0) | **41** | 31 | 0 |
| 1.5× | 30 | 30 (19 / 0) | **40** | 36 | 0 |
| 1.7× | 8 | 17 (19 / 4) | **34** | 34 | 2 |

\* v0.8.0 = HEAD's `bb_condition.h` + `bb_bars.h` + the v0.8.0 shell rule
(nearest power of two), same files, same pipeline (its prior column reads
36/37/28/5 because its conditioning does not cut whole bars). It spoke on
16–21 files and was wrong on 4 of 19 at 1.7×.

With **Onsets = 1** (log-filtered flux): 1.0× **43** correct (spoke 30, 0
wrong); 1.5× 40 (32 spoke, 1 wrong); 1.7× 32 (30 spoke, 0 wrong). Mode 1 is
the better front end for the estimator; I left the default at 0 as specified.

Where the prior fails at speed the new path wins outright: at 1.5× the prior
is right on 30/45 and the new grid on 40/45; at 1.7× 8 vs 34. Every
sped-up Amen above 220 BPM (138 × 1.7 = 235) is out of range by design and
counted as a miss for both; at 1.7× the remaining misses are 1-bar loops that
become 3 s files with ~26 onsets — not enough evidence, so the prior stays.

The two wrong-confident files at 1.0×: **Apache (Part 2)** — the conditioning
cut 2 bars at 78 BPM, a 3/2 alias of 117 that the bongos' 3-3-2 cycle fits
(the post-cut estimate says 117 but the buffer is then 3 bars); and **Kissing
My Love (Part 1)** at 122 BPM, a 4/3 alias of 92 (both alias cuts passed the
verification because the alias fits the window too). At 1.7× the two are
Apache (Part 2) again (132.8, 4/3 alias… by way of the prior's cap) and the
long Funky Drummer (Ver. 2), where the estimate is right (165.9) but the
fallback buffer is not whole bars, so the prior's grid stands. Nothing in the
table is an octave error at 1.0×.

### Confidence-gate trade-off (`kConfMin`, compile-time `BB_BARS_CONF`)

| gate | 1.0× correct / spoke / wrong | 1.5× | 1.7× |
|---|---|---|---|
| 0.06 | 40 / 37 / 3 | 41 / 41 / 2 | 40 / 41 / 2 |
| **0.12** | 39 / 28 / 2 | 40 / 36 / 0 | 34 / 34 / 2 |
| 0.18 | 38 / 22 / 1 | 37 / 26 / 0 | 24 / 22 / 1 |
| 0.25 | 36 / 13 / 0 | 34 / 19 / 0 | 15 / 11 / 0 |

0.12 is the knee: 0.06 buys 6 more correct at 1.7× for 1–2 more wrong
answers at every speed; 0.18 halves the wrong answers and gives back 10 at
1.7×. A wrong confident answer is worse than the prior, so 0.12 ships; if the
field shows more wrong grids than the table predicts, 0.18 is the next stop.

### Whole corpus (1,230 files, mode 0)

At 1.0× the estimator speaks on 702 files (57 %): 655 give the same grid as
the prior, 34 double the prior's tempo, 13 halve it, none other. At 1.5× on
a 200-file sample it speaks on 93 (46 %): 50 same as the prior, 42 double it
(as they should: the prior halves everything over 150 BPM), 1 halves. Run
`tests/bars_corpus.sh` (≈ 1 min) to reproduce; `tests/bars_check.cpp` prints
per file. Total analysis cost on the M7 is dominated by onset detection: two
detections per load as before (three for files over 9.5 s), the estimator
itself is a few ms.

## 3. Swing: the material's own 16ths

`bb::EstimateSwing` (bb_swing.h) takes the onsets against the grid the engine
will play on (len / nSlices from sample 0): the on-beat cluster (±0.12 of an
8th) must be concentrated with a mean within 0.05 of the grid (else the grid
itself is off and we don't swing), the off-beat cluster (0.30–0.85 of the 8th)
must hold ≥ 4 onsets with circular R ≥ 0.6, and the ratio p/(1−p) must lie in
1.05–2.2 (Frane 2017: canon median 1.2, max 2.1). Result p = the odd 16th's
position inside its 8th, 0.5 = straight.

When Auto is on the shell sets `engine.swing = p` before `SetMaterial`, which
puts each odd slice's **boundary** at p of its 8th (the even slice grows by
the same amount; the average step, hence varispeed, is unchanged), maps hits
to slices by the real boundaries, and `Process` fires the odd **step** of each
8th at p instead of 0.5. Ratchets keep their average-step spacing. With swing
at 0.5 nothing changes: `swung` stays false and every path is the v0.8.0 one
(Classic grammar hash c2081c20c9d7fbbf, run.sh gate).

Host test (synthetic 110 BPM break with swing ratio 1.94, i.e. p = 0.66,
rearranged with a parity-keeping pattern so every step is a fresh trigger):

| | odd slots opening with the wrong slice | hat-lane gates (onsets-only, 2 loops) | odd-step trigger position in its 8th |
|---|---|---|---|
| Straight | 8/8 (the hat slice's head is the kick's tail) | 8 | 0.50 |
| Auto | 0/8 | 22 | 0.66 |

Straight slicing of swung material loses the hats twice over: the odd slice
opens with the previous drum's tail (so any rearrangement plays that tail
where the hat should attack), and with "Gate Fires: Onsets only" the hat's
onset falls past the first quarter of its slice and is not a hit — the hat
lane goes quiet. Auto keeps both, and the groove: odd steps fire where the
material put them. Measured swing ratios on the canon rips: Kissing My Love
1.30–1.69, God Made Me Funky 1.43–1.62, Synthetic Substitution 1.47, Apache
1.34, Amen 1.34–1.36, Funky Drummer 1.11–1.24, Impeach 1.09–1.54 across
versions — same ordering as Frane's table, a little higher (his measure is
duration-based over the swingable events only; ours is the mean phase of the
off-beat cluster).

## Testing on hardware

1. **The field bug.** Load a jungle-tempo break (or any break sped to
   1.5–1.7× in a DAW). Options → Bars = Auto. The web state's `nativeBpm`
   should read the real tempo (a 4-bar Amen at 170 BPM: 170, 64 slices), not
   half of it; with a matched clock the break plays at speed. Bars = 1/2/4/8
   still override. Expect: most 75–150 BPM loops load exactly as before (the
   estimator agrees with the prior on 93 % of the files it speaks on).
2. **Onsets.** Options → Onsets = Log-filtered flux; the material re-slices in
   place. With Gate Fires = Onsets only, listen for hat gates that mode 0
   missed on swung/busy breaks; the hit markers in the web slice view should
   sit on attacks, never a few ms early. A file loaded from a WAV with cue
   points keeps its cues under either setting.
3. **Swing.** Load Kissing My Love or Impeach; Options → Swing = Auto. Chop
   (P1 down) and reslice: hat slices should open on the hat, not on the tail
   of the kick before it, and the pattern should still swing. Switch back to
   Straight: the same pattern straightens and the hat slices carry a kick
   tail at their head. Straight at load = exactly v0.8.0.
4. **Load time.** Files over 9.5 s run one extra onset detection; expect
   loads a third longer on those only.

## Open questions

- **ClassifySteps / ComputeEnvelope** (GATES/WEB territory) still see equal
  steps: under Swing Auto an odd step's class and envelope come from a window
  up to 0.3 step off its real slice. Cheap fix later: hand them the slice
  table.
- **Aliases in conga/bongo material** (Apache): a 3-3-2 cycle fits a 3/2
  grid; the template does not always win. The verification cannot catch it
  because the alias fits the window too. A meter-level check (kick on 1 of
  *every* bar at the alias tempo) might.
- **kConfMin** 0.12 is a corpus choice; the trade-off table shows the knobs.
- **Onsets = 1 as default?** It helps the estimator (43/45 vs 39/45 at 1.0×)
  and fixes the early-hat refine, but changes every slice table; left to
  Ernest.
- The state tail does not carry the bar confidence or the swing value (CLOCK
  appends after `barsOpt`; I did not touch the layout). Worth a byte each.
- Frane's tempos for Theme From The Planets Parts 1/2 and Fool Yourself do not
  match the corpus files' own periodicity; those files are excluded from the
  truth set rather than "fixed".
