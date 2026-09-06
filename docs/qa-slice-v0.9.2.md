# QA checklist — SLICE package (Onsets · Bars Auto · Swing), v0.9.2

Flash the v0.9.2 `audiothurgist.bin` (built from `main` 7be57f9: `audiothurgist-v0.9.2-7be57f9.bin`, sha256 e17ed9a8…; the
web app shows the version). Everything below is on the card's own breaks plus
two files you make once in a DAW: **Amen at 1.5×** (≈207 BPM, exported as a
WAV) and **Funky Drummer at 1.6×** (≈160 BPM). Tick as you go; write what
you heard next to the box — a wrong answer's *number* (the web app's native
BPM) is the most useful note.

Defaults are unchanged: Onsets = Spectral flux, Bars = Auto, Swing = Straight.
Only Bars Auto behaves differently from v0.8.0 out of the box.

## 0. Regression (nothing new switched on)
- [ ] Power-cycle, nothing patched → plays immediately.
- [ ] Amen (4 bars, ~138): web app **native BPM ≈ 138, grid 4 bars**, plays at its own speed. (v0.8.0 read the same; this confirms the new estimator agrees where the old one was right.)
- [ ] Five 75–150 BPM loops from the card, loaded in a row: each reports a sane native BPM and the wrap lands on the downbeat. Anything that now reads half or double of what v0.8.0 read → note the file name (the corpus run says 655 of 702 agree; the rest are worth hearing).
- [ ] Bars = **1 / 2 / 4 / 8** overrides still force the grid; back to Auto re-estimates in place (no click, playhead keeps running).
- [ ] Options → Swing = Straight and Onsets = Spectral flux: pattern, gates and slicing identical to v0.8.0 (Classic grammar is hash-locked; this is the ear check).

## 1. Bars Auto — the field bug
- [ ] **Amen at 1.5×** (≈207 BPM): native BPM ≈ 207, **64 slices**, plays at speed against P2 at the same tempo. v0.8.0 reported ≈104 and ran at half speed against a matched clock — that is the bug this fixes.
- [ ] **Funky Drummer at 1.6×** (≈160): native ≈ 160, 32 or 64 slices, 16ths are 16ths (hats tick at the right rate under a Classic chop).
- [ ] A **song excerpt** from the card (10–25 s, not loop-cut): after load the loop is a whole number of bars and starts on a downbeat (P1 full right = verbatim, listen for the wrap — no stumble). Corpus figure: the cut lands on a downbeat in ~80 % of files; the usual miss is a **half-bar** rotation (beat 3 taken for beat 1). Note any file whose wrap lands mid-bar.
- [ ] A **half-time rock/soul groove** (snare on 3, 130–160 BPM): expect the estimator may read it at half tempo (65–80). Confirm Bars = 1 or 2 override sets it right in one click. (Known, documented; a Bars override is the intended escape.)
- [ ] Load **while clocked** (external clock at the break's tempo): the new grid takes over without the PLL lurching more than in v0.8.x (a known residual, not new — just confirm it is no worse).
- [ ] Load time: a 20 s file should take about a third longer than v0.8.0 (one extra analysis pass on files over 9.5 s); nothing else should feel slower.

## 2. Onsets — log-filtered flux
- [ ] Options → Onsets = **Log-filtered flux (Böck 2012)** on the Amen: re-slices in place; Gate Fires = Onsets only, hat lane patched to a scope or a pinger — **hat hits fire that Spectral flux missed** on the swung 16ths (mode 0 places some hats up to 10 ms early and then misses the hit window).
- [ ] Same on Kissing My Love or Funky Drummer: fewer, cleaner onsets on the busy sections (the corpus shows 100 → 78 on Funky Drummer); nothing obviously missing on kicks/snares.
- [ ] Web slice view: the hit markers sit **on** attacks, never a few ms ahead of them.
- [ ] A WAV with **cue points** loads the same under either Onsets setting (cues win; the setting does not replace them).
- [ ] Toggle Onsets back and forth twice during playback: each toggle re-applies cleanly, no dropout longer than a re-slice.

## 3. Swing Auto
- [ ] Options → Swing = **Auto** on Kissing My Love (ratio ≈1.3–1.7 measured) or Impeach: P1 to a chop zone, Reslice a few times — hat slices open **on the hat**, not on the tail of the kick before it, and the chop still swings. Switch to Straight: the same pattern straightens and hat slices carry a kick-tail bump at their head.
- [ ] Funky Drummer (ratio ≈1.1): Auto and Straight sound nearly the same — the estimator should report little or no swing (below 1.05 it stays straight).
- [ ] A **straight** electronic loop: Swing Auto changes nothing (the on-beat cluster must confirm the grid; a straight file stays at 0.5).
- [ ] An **8-bar** loop (or Bars = 8): Swing Auto is intentionally **off** on 8th-note grids — confirm nothing shifts (v0.9.2 fix).
- [ ] Gates under Swing Auto with Onsets only: the hat lane fires on odd 16ths that were silent under Straight.
- [ ] Voice calls under Swing Auto: the sixteen keys still address the right slices; an odd-16th slice called by key starts on its hat.

## 4. Things to bring back
- Any file where Bars Auto is confidently **wrong** (not "prior") — name + native BPM shown. The gate is tunable (`kConfMin` 0.12; 0.18 halves wrong answers at the cost of speaking on fewer files).
- Whether **Onsets = Log-filtered** should become the default (it helps the bar estimator: 43/45 vs 39/45 on the known set).
- Whether the classifier/envelope should follow the swung boundaries (today they read equal steps: an odd step's class can be ~0.3 step off under Swing Auto).
