# Morning checklist 2 — the night of 2026-09-06 ("until noon")

Flash `Desktop\Audiothurgist-flash\audiothurgist.bin` (= **stage 20, v0.9.6-time**: the 21.5 s timer-wrap fix, gates from the audio thread, stall forensics, frozen dials, your defaults; earlier: stage 17, **v0.9.3-defaults** = v0.9.2 with the recommended options on by default; stage 16 = **v0.9.2-swingfix**
= v0.9.1 plus one more fix: no swing estimate on loops sliced at 8ths, where
straight hats read as a 1.67 swing). v0.9.1-reviewed was stage 15 (**reviewed**:
round 2 plus an adversarial review's fixes — two real bugs in the new
loop conditioning, a clock keep-alive so loads no longer freeze the
playhead, DTCM back to 64 %). Stage 14 is the unreviewed round-2 build,
stage 13 = v0.8.0 Round 1 only, for bisecting. The
web app is new too: `cd ~/Daisy/Audiothurgist/web && python3 -m http.server 8080`
→ http://localhost:8080 (close `/program` first). Press **Demo** to see it
without the module. Every package has a longer test script in
`docs/packages/<name>.md`; this page is the short path. Tick and annotate.

## 0. Your report, item by item
- [ ] **Half-speed against the clock.** Load a jungle-tempo break, patch Pamela's at a matched PPQN. It should now run at the right speed with P2 at ×1. If not: Options → **Bars** → pick the true count; it re-slices in place and the native BPM readout (web, "native") should read the real tempo. Then tell me which file and what Auto guessed — the estimator is corpus-validated (see §5) but your card is the real test.
- [ ] **Unclocked tempo reach.** P2 now spans 60–200 BPM; with Bars right, native tempos read true.
- [ ] **Dropped moment / gate slip (v0.9.6):** the same 21.5 s wrap skipped ~48 ms of material once per period, and snare/hat gates were written by the control thread 1–3 ms late; both fixed (snare/hat now written from the audio thread at 1 ms cadence). Listen for a full two minutes unclocked: no skip expected.
- [ ] **2-second gaps unclocked.** Unpatch J3 (or stop Pamela's with the cable in), play five minutes in Classic and Grids. No gaps expected. If one happens, read the web diag pill: `jumps` and the ext-transition count in the clock line.
- [ ] **"×2 then falls back after a few bars."** FOUND (v0.9.6, stage 20): `System::GetUs()` wraps every 21.5 s on this hardware, so every ~21 s the clock leapt 50 ms, the external-clock window dropped, and the follower re-locked with P2 reset to ×1 — 21.5 s is about ten bars at 120 BPM. Fixed with a wrap-safe time base. Verify: engage ×2 and play for two minutes; it must hold. If it still falls back, the older candidates apply, in this order: (1) web app → is P2 badged **locked**? Press Clear locks. (2) Watch the clock line's ext-transition counter while it happens. (3) The zone edges moved: 1 o'clock now sits inside ×1; enter ×2 at ≈0.69, leave at ≈0.61, 100 ms dwell. Multiplier changes now switch on the next beat, in phase — no jump.
- [ ] **Gates once per hit.** Options → Gate Fires → **Onsets only**. A snare's decay steps no longer retrigger the snare lane. (Bonus fixes: a timed gate now retriggers on a real re-hit inside its window; leaving Hold no longer leaves a lane high for 93 minutes.)
- [ ] **Button functions in the web app.** Controls panel: B1/B2/B3 tap · hold · chord, six knob meanings for the current page and grammar, P2's zones highlighted while following, lock badges, on-page buttons for every action, a Manual tab from the firmware's own descriptor.
- [ ] **Web responsiveness.** Clicks now go ahead of polls; state 10 Hz, scope 5 Hz only when visible. The link pill shows round-trip ms and req/s. Say whether it feels right on the phone too.

## 1. Options that change behaviour

Defaults as of **v0.9.3-defaults** (stage 17), per Ernest: the recommendations ship on — Gate Fires: Onsets only · Rests: Continue · Fades: Adaptive · Swaps: Class + salience · Flow Déjà Vu: Some · Swing: Auto · Onsets: Log-filtered. Gate Lanes, Slice Tail and Bars keep the old behaviour. Set an option back to its first entry to hear the old behaviour.
| Option | Default | Try | Listen for |
|---|---|---|---|
| Bars | Auto | 1/2/4/8 on a break Auto gets wrong | tempo and grid snap right |
| Rests | **Continue** | Gate | Grids/Euclid: flowing break vs gated 16ths |
| Gate Fires | **Onsets only** | Onsets only | one gate per drum hit |
| Gate Lanes | Dominant class | Multi-label | kick+hat step gates both J6 and J8; snares often also mark the hat lane (by design) |
| Fades | **Adaptive** | Adaptive | pitched-up/chopped material: fewer clicks on bright slices, softer on sustained |
| Slice Tail | **Zoom (BreakFix)** | Gate / Loop forward / Ping-pong | pitch is a zoom on the material (v0.9.8): pitched up the slot reads on into the next slice and re-anchors (the anticipated arrival), pitched down it is cut at the slot end; Gate is the old honest gap |
| Jacks | **Follow grammar** (Play → Voice map) | Brain / Voice | Voice map (v0.9.9): J4 = slice note, J5 = style note (plain · reverse · half · double · ratchet · staccato · punch/choke flip), J6 = gate in, J7 = hit trigger out, J8 = velocity out; changing the style with the key held re-strikes the slice; one semitone below 0 V on J4 = mute |
| Grammar | — | **Play** (new, 6th) | silent until you play into it: Voice keys punch the bar (the punch keyswitch flips to choke), web punches too; B3 clears it |
| Swaps | **Class + salience** | Class + salience tier | Chop ~10–11 o'clock on the Amen: ghost notes never replace backbeats |
| Flow Déjà Vu | **Some** | Some / Lots | B2 hold: bars come back around (¼ / ¾ of the time) |
| Swing | **Auto** | Auto | a swung break keeps its hats (straight slicing decapitated them) |
| Onsets | **Log-filtered** | Log-filtered | quiet hats found; better Bars Auto (the doc recommends it as the future default) |
| Grammar | — | **Camp** (new, 5th) | change-ringing permutations; B2 rings the next change |
| Gate Level | — | Velocity | Grids accents (map level > 192) hit the 5 V ceiling |

## 2. Clock (docs/packages/clock.md)
- [ ] Patch-in: no silence while the PLL acquires (the clock keeps running); at lock the loop **aligns to the pulse grid** (it never did before — a 20 ms to one-beat offset depending on PPQN).
- [ ] P2 zones while following: ÷4 ÷2 ×1 ×2 ×4 — each switch lands on a beat with the phase continuous.
- [ ] Jitter number in the web clock line: Pamela's should read near the 1 ms poll floor; a DAW MIDI clock will not.

## 3. Playback (docs/packages/play.md)
- [ ] Verbatim at native tempo with Fixed/Gate = a null test against v0.8.0 (byte-identical goldens).
- [ ] Classic seams: the continuity fix from v0.7.4 (lookahead grace) still holds — no 6 ms notch per 16th on a sustained cymbal.

## 4. Grammars (docs/packages/gram.md)
- [ ] Classic unchanged (hash-gated). Collins, Grids (now at the map's real 32nd resolution: backbeat on 2 and 4), Euclid, Camp.

## 5. Slicing (docs/packages/slice.md)
- [ ] Bars Auto on your card's breaks. Corpus result: at 1.0× the new estimator is right on 39/45 canon rips with 2 wrong-confident (conga material), and at 1.7× varispeed 34/45 vs the prior's 8. Your jungle-tempo card is the field test.
- [ ] Swing Auto on a swung break vs Straight.

## Known / deferred
- DTCM is at 64 % after the review moved the bar estimator's scratch and the lock arena to SDRAM.
- Under Swing Auto, class/envelope of odd steps can be up to 0.3 step off (equal-step analysis).
- History scrub replays with the current grammar params; presets saved with a card don't load cardless.
- Least-squares clock period estimator is evaluated (4–5× better than the SDK's EMA on an E-RM-shaped train) but needs an SDK hook — a question for Luke.
- Onsets = Log-filtered is now the default (v0.9.3); Spectral flux stays selectable.
