# Overnight report — 2026-09-05

Mandate: "mostly complete the current roadmap, check actual examples (don't
guess), branches OK, leave open what needs me." Everything below **builds and
is committed on `main`**; nothing below was heard on hardware tonight — every
stage is a hypothesis until your ears say otherwise. Flash in order if you
want to bisect; the last stage is also `audiothurgist.bin`.

| Stage | Binary (Downloads) | What it adds |
|---|---|---|
| 1 | `audiothurgist-stage1-m2fix-musical.bin` | Gate Length / Gate Level (velocity) dropdowns |
| 2 | `audiothurgist-stage2-dsp-musical.bin` | the stutter fix + boot-freeze fix + P2 hysteresis |
| 3 | `audiothurgist-stage3-voice.bin` | **M3 Voice** — J4 v/oct + J5 gate play slices |
| 4 | `audiothurgist-stage4-voice-jungle.bin` | **Collins grammar** (Grammar dropdown) |
| 5 | `audiothurgist-stage5-toolkit.bin` | **M6** — Options component, ParamLock, Settings, presets, manual |
| 6 | `audiothurgist-stage6-languages.bin` | **M4** — Grids + Euclid, the Brain page |
| 7 | `audiothurgist-stage7-web.bin` (= `audiothurgist.bin`) | web-frontend firmware extension |

All flash via the web flasher (HostLink is up on the current build).

## Findings you should know before touching anything

1. **Stutter root cause (DSP expert, confirmed in code):** the varispeed
   rate was quantised to 0.4 BPM steps while the PLL's rate is continuous —
   ~20 samples/step of drift, blowing the 160-sample continuity window every
   ~8 steps. Fixed: the engine's `barSeconds` is now derived every audio
   block from a snapshot of the NCO's `TicksPerUs`, so slice rate == phase
   advance exactly. The PLL "phase snap" theory was killed with evidence.
2. **Boot-freeze bug in v0.3.3/0.3.4 (found, fixed):** `ClockFollower::
   Enable()` at boot stops the NCO until three pulses lock — with **no clock
   patched, playback froze after one step**. You probably never saw it because
   you were testing with a clock. Now armed only when a clock actually appears.
   **Test the unpatched case first tomorrow.**
3. **Residual (not fixed):** loading a sample while clocked stalls the control
   loop; the PLL surges to catch up and the loop lands phase-shifted. Rule
   for now: don't load while clocked. Clean fix is a yieldable loader.
4. **Amen hypothesis, measured (musical expert):** the loop does **not** pitch
   down spectrally — both broadband and kick-band centroids *rise* across the
   Amen (the crash pulls them up). The "gestalt pitch-down" is a feel effect.
   Beat-lag hi/lo alternation is real (0.55) but Apache scores higher (0.70), so
   that metric doesn't single the Amen out. The Phase-0 clustering outlier
   (99.8th pct) remains its one measured signature.
5. **Grids fidelity:** the drum map is ported verbatim; a naive test read 253
   at an exact node instead of 255 — that is Grids' own `U8Mix`
   (`a*(255-b)+b*b >> 8`), verified against avril's `op.h`. Faithful.
6. **Classic grammar is byte-identical** under the new Grammar switch
   (1,575 seed/knob cases hashed, `tests/grammar_check.cpp`).

## Control map now

| | Perform page | Brain page (B1 tap) |
|---|---|---|
| P1 | Chop | Map X (Grids) |
| P2 | Tempo / ÷4…×4 while following | Map Y |
| P3 | History | Kick density |
| P4 | Pitch | Snare density |
| P5 | Sample select | Hat density |
| P6 | Volume | Rotation (Euclid) |

B1 tap = page · **B1 hold + turn a pot = record a motion loop** (ParamLock,
saved with presets) · B2 tap dice / hold flow · B3 tap reslice / hold load ·
**B2+B3 held 2 s = Settings** (brightness, preset save/load gesture).

Jacks: J3 clock in · **J4 v/oct · J5 gate** (Voice) · J6/J7/J8 kick/snare/hat
gates out · J1/J2 audio in · J9/J10 out.

`/program` → **Options** (one panel): Classifier · Clock In PPQN · Voice
(Choke/Punch) · Gate Length · Gate Level · Grammar (Classic/Collins/Grids/
Euclid). Plus Sample dropdown, the SD browser, presets (renameable), and the
interactive manual.

## Morning A/B checklist (in this order)

1. **Unpatched, power-cycle:** plays; P2 sets tempo; B3-hold loads a break
   that opens at native tempo. (Regression check for the boot freeze.)
2. **Clock into J3, PPQN set:** ~3 pulses of silence, B1 green. P1 full right,
   listen two minutes: the ~8-step re-splice should be gone; internal and
   external at the same tempo should sound identical.
3. **P2 while following:** engages after a >3 % move; ÷4/÷2/×1/×2/×4; park
   near a zone edge — no flicker. Unpatch: tempo holds, P2 takes over on move.
4. **Gates:** three lanes on J6/J7/J8. Try Gate Length *Hold* and Gate Level
   *Velocity* (ghost notes ~1.5–2.5 V, backbeats near 5 V).
5. **Classifier lenses on the Amen** with the gates patched: Hockman vs
   Legacy hi/lo vs Two-band — which does your rack prefer?
6. **Voice (KeyStep Pro → J4/J5):** the key that outputs 0 V mutes; two
   octaves up, sixteen keys call slices (blue flash on the ring). Try
   Options → Voice → Punch: mute, play a pattern in, unmute.
7. **Grammar → Collins:** ~4–2 o'clock the loop gets restless toward its
   end; ~10–9 rolls only in the last quarter; on the Amen, every 4th loop's
   last bar fills and the downbeat snaps back.
8. **Grammar → Grids / Euclid**, B1 to the Brain page: Map X/Y + densities
   through the break's own hits; Chop knob = slice faithfulness.
9. **Web frontend:** `cd web && python3 -m http.server 8080`, Chrome →
   http://localhost:8080, close `/program`, Connect. Bar, waveform, scope,
   click-slice-then-click-step to punch.

## Open — needs you

- **M5 live-slice** (trigger-jack slicing): needs a control-gesture decision
  now that B3 is load and J5 is the Voice gate. Proposal: J5 doubles as the
  slicer *only* in a LIVE-SLICE mode entered from Settings, or via a jack map.
- **SPLIT keymap** needs two more inputs → **jack maps as presets** (spec
  §3.3) is the next architectural piece; the HYBRID/BRAIN/VOICE maps are
  designed, not built.
- **P2 multiplier zone changes phase-jump** — align to the next beat (musical
  expert's #1; do it with a scope on J6).
- **Per-role presets** — "Cypher" (Hockman, fixed 5 V, 6 ms) and "Brain"
  (Legacy hi/lo, velocity, 20 ms) — save them from the browser once you've
  chosen defaults.
- **Web app hosting** — Codeberg Pages setup is yours (a `pages` branch);
  localhost works meanwhile.
- **Every A/B verdict above.** Tuning constants in Collins (faithfulness
  slope 0.55, roll probabilities, fill multipliers) are first guesses for ears.
- DTCM is at 69 % (ParamLock arena + Settings); SRAM 60 %. Fine, but the
  next big thing should watch DTCM.

Branches `dsp`, `jungle`, `musical`, `voice` are merged; the sibling clones in
`~/Daisy/Audiothurgist-*` can be deleted. `git push` when you're happy.
