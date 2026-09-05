# Morning checklist — every piece landed overnight (2026-09-05)

Flash `audiothurgist.bin` (= stage 7 + review fixes, **v0.7.1-reviewed**; the
`/program` page and the web app both show the version) via the web flasher.
If a later stage misbehaves, flash back one `audiothurgist-stageN-*.bin` to
bisect. Known-but-deferred (from the review, all low): history scrub replays
with the *current* grammar/Brain settings, not the ones heard; presets saved
with a card won't load on a cardless boot (schema includes the file list).
Tick as you go; note anything surprising next to the box — that's the
iteration list for the day.

## 0. Boot & regression (do these first, no patches)
- [ ] Power-cycle with **nothing patched** → plays immediately (regression check for the v0.3.3/0.3.4 boot freeze).
- [ ] P2 sets tempo; native detent still snaps.
- [ ] Hold B3 → loads the highlighted file; opens at its native tempo; P2 re-engages on move.
- [ ] `/program` connects; shows **Perform** + **Brain** pages, **Options** panel (six selectors), Sample dropdown, SD browser, presets, the manual.

## 1. Clock (stage 2 — DSP expert)
- [ ] Clock into J3, set PPQN in Options → ~3 pulses of silence, then B1 white-flashes per pulse and sits **green** when locked.
- [ ] P1 full right (verbatim), listen **two minutes**: no periodic re-splice; internal vs external at the same BPM sound identical.
- [ ] P2 while following: nothing until moved >3 %, then ÷4 / ÷2 / ×1 / ×2 / ×4 (wide ×1 centre); park on a zone edge — no flicker.
- [ ] Unpatch → after ~2 s B1 goes dim, tempo **holds**, P2 takes over on move. Repatch → re-locks.
- [ ] **Lost-before-lock:** patch J3 with the master *stopped* (or stop it after one pulse), wait ~3 s → playback resumes at the previous tempo. (Review caught this leaving the clock stopped until reboot — fixed in v0.7.1.)
- [ ] Known residual: loading a file **while clocked** lurches the PLL. Confirm, and don't do it in a set.

## 2. Gates (stage 1 — musical expert)
- [ ] J6 kick / J7 snare / J8 hat lanes all fire (three, not two).
- [ ] Options → Gate Length: 6 ms · 20 ms · 50 ms · **Hold to next hit** (lane stays high, clean retrigger on re-hit).
- [ ] Options → Gate Level → **Velocity**: ghost notes ≈1.5–2.5 V, backbeats ≈5 V (scope or an envelope's depth).
- [ ] Options → Classifier on the Amen with gates patched: **Hockman 3-band** vs **Legacy 120/900 (hi/lo)** vs **Two-band lo/hi** — ring colours change live, pattern does not. Which does the rack prefer? (Note: the Amen does *not* pitch down spectrally — measured. The hi/lo nesting is real but Apache scores higher.)
- [ ] **Density (keyframes)** — the fourth lens, Matthew's extrema analysis (v0.7.2): no band filters, just how many significant peaks each step has. Expect kicks sparse / hats dense; snares are the uncertain middle. Does it agree with Hockman on the obvious hits, and does it find anything the bands miss? Two constants to tune by ear if it reads "hat" everywhere: the deadband (3 % of peak) and the kick/hat ratios (0.6 / 1.67 × median).

## 3. Voice — M3 (stage 3)
- [ ] KeyStep Pro → J4 (v/oct) + J5 (gate). The key that outputs **0 V toggles mute** (ring dims).
- [ ] Two octaves up: **sixteen keys call sixteen slices** spread across the loop; called step flashes **blue** on the ring; the call replaces what's sounding (no layering).
- [ ] Modifier keyswitches (the ten keys above the gap, latched — tap to toggle): reverse · half-speed · double · choke/punch override · ratchet.
- [ ] Options → Voice → **Punch**: mute, play a pattern in, unmute → your loop plays. (Choke = momentary, pattern untouched.)
- [ ] Note the KeyStep's 0 V key so we can expose a base-note setting if it's awkward.

## 4. Collins grammar (stage 4 — jungle expert)
- [ ] Options → Grammar → **Classic**: identical to before (proved byte-identical; confirm by ear).
- [ ] **Collins**, ~4–2 o'clock: bar 1 stays faithful while later half-bars get restless (5+3 / 3+3+2 figures) — the position gradient is the audible difference.
- [ ] ~1–11 o'clock: 3+3+2 idiom; repeats gather in the back half; ~every 5th loop end drops a 1-cell stutter (…da-da-DA-DA).
- [ ] ~10–9 o'clock: rolls only in the last quarter — even doubles and short accel/decel bursts; repeats drift ±1.5 % so runs aren't cloned.
- [ ] On the Amen (4 bars): loops 1–3 base; loop 4's **last bar fills**, next downbeat **snaps back**. Too frequent/tame? Constants are noted in the overnight report.
- [ ] Punch a hit in (Voice or web) outside the last bar, wait 8+ loops → it **stays** through the fill toggle (review fix: only the fill region re-flattens now).
- [ ] B3 changes *which figures*; B2 changes *what fills them*; P3 replays both.

## 5. Toolkit — M6 (stage 5)
- [ ] **B1 hold + turn any pot** → records a motion loop (~16 s) that replays until re-recorded; survives a preset save/load.
- [ ] **B2 + B3 held 2 s** → Settings screen (brightness; preset save/load gesture). B2 or B3 leaves. Confirm dice/reslice/flow/load did NOT fire on the way in or out (the review caught the exit tap firing — fixed in v0.7.1; this is the regression check).
- [ ] Save a preset from `/program`, rename it, reboot, confirm it reloads (Options, knobs, locks).
- [ ] The manual in `/program`: tagline, sections, per-knob/jack help read correctly.

## 6. Languages — M4 (stage 6)
- [ ] **B1 tap** → Brain page (rings switch to Map X/Y, Kick/Snare/Hat densities, Rotation); tap again → Perform. Bar/history overdraw only on Perform.
- [ ] **Before judging Grids/Euclid:** on the Brain page turn P3/P4/P5 (the three densities) down through zero and back up to ~2 o'clock. Brain knobs start at their *stored* value (0 on a fresh unit = no hits at all) and need pickup — silence here is the pots, not the grammar.
- [ ] Grammar → **Grids**: Map X/Y + densities speak Mutable's map through the break's own kicks/snares/hats; a change regenerates live.
- [ ] Grammar → **Euclid**: densities = hits per bar, Rotation rotates all classes.
- [ ] In both: Chop full right = each class's own nearest hit; lower = same-class swaps (B2 rerolls the swaps).

## 7. Web frontend (stage 7)
- [ ] Close `/program`. `cd ~/Daisy/Audiothurgist/web && python3 -m http.server 8080` → Chrome → http://localhost:8080 → **Connect** → pick the Alchemy Lab.
- [ ] Bar ring + loop strip move with the playhead; BPM/sync pills live; Options labels and the Sample list populate from the module.
- [ ] Waveform shows per-slice envelopes in class colours; the playing slice is outlined.
- [ ] Click a slice (dashed yellow) → click a step in the strip → it **punches** (blue flash on ring + strip).
- [ ] Dice / Reslice / Mute / Flow buttons; History slider scrubs; Sample select loads.
- [ ] Scope shows the output. Try it on the phone (same network, http://<pc-ip>:8080 needs https — localhost only for now).

## Decisions I need from you
- [ ] Default classifier for the "Cypher" (audio) vs "Brain" (gates) roles → save two presets.
- [ ] M5 live-slice control gesture (B3 and J5 are taken).
- [ ] Jack maps as presets (unblocks SPLIT keymap + a chop-CV input).
- [ ] P2 multiplier: align zone changes to the next beat? (recommended; needs a scope session)
- [ ] Codeberg push + Pages for `web/`.
