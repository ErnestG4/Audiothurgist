# GRAM — salience-aware swaps, déjà-vu flow, Grids accents, Camp

Branch `pkg/gram`. Owns `core/bb_pattern.h` and `RerollFill`/history in
`core/bb_engine.h`. Shell hooks are fenced `/* GRAM */` in
`src/audiothurgist.cpp`. Everything is behind an Option or a new grammar:
with **Swaps = Class only**, **Flow Déjà Vu = Off** and a grammar other than
Camp, every pattern is byte-identical to v0.8.0 (all four grammar hashes in
`tests/run.sh` are unchanged; Classic's frozen `c2081c20c9d7fbbf` gate
passes).

## 1. Options → Swaps (byte 12)

| sel | label | behaviour |
|---|---|---|
| 0 | Class only | today: a swap target must share the drum class (when the knob's `classStrict` says so) |
| 1 | Class + salience tier | additionally, a swap target must share the **salience tier** of the step it replaces |

**Why.** Hockman & Davies 2015 re-weight resequencing candidates by segment
length precisely so that "an off-beat ghost note" is not pasted "in place of
a salient snare". Class matching alone lets a ghost hat replace the loud
hat on the "and", or a quiet snare drag replace the backbeat.

**How.** The shell already computes per-slice salience (`gSal`, from the
classifier). The engine now carries `Engine::sliceSal[]` (copied in the
`gApply` block *before* `SetMaterial`, so even the boot pattern sees it, and
again on reclassify). `Engine::ComputeSalTiers()` ranks the loop's salience
into three tiers by **tercile** — the thresholds are the values at ranks
n/3 and 2n/3, so ties collapse upward: a loop with uniform salience (or no
classifier data) is a single tier and swaps exactly as before. Tiers land in
`Pattern::salTier[]`; `Pattern::salSwaps` switches the rule on.

Where it bites, in every grammar:

- **Classic** — the class-respecting swap loop (8 tries, then in place) also
  requires `TierOk(cand, pos)`. Repeats, in-place cells, rests and hat
  ticks are not swaps and are untouched.
- **Collins** — the same in `genRange`, base and fill layers (the fill's
  looser `strictE` still gets the tier rule).
- **Grids / Euclid** — the same-class swap pool is re-drawn up to 8 times
  for a tier match; the faithful (in-place / nearest) pick stands otherwise.

The rule is a pure *rejection* on top of the existing RNG stream: with the
option off no extra draws happen, so the shipped grammars are unchanged. On
the test material about half of Classic's class-only swaps crossed a tier;
with the option on, none do.

Changing the option regenerates with the **same seeds** (audible now, no
new roll). Scrubbing history replays with the current option, as it does
with the current grammar.

## 2. Options → Flow Déjà Vu (byte 13)

| sel | label | behaviour |
|---|---|---|
| 0 | Off | every flow bar draws fresh seeds (today) |
| 1 | Some | ¼ of flow bars **replay** a bar from the history ring |
| 2 | Lots | ¾ of flow bars replay |

**Why.** Marbles' DEJA VU (probability of re-using the loop / jumping
within it); Margulis 2013 — inserted repetition, immediate *or delayed*,
raised enjoyment, interest and artistry ratings. Flow today never repeats
itself; with déjà vu a held B2 produces phrases with returns in them.

**How (`Engine::RerollFill`).** Only a *flow* reroll (`flowRegen` set —
`Process` calls `RerollFill` at the bar wrap) is eligible; a B2 tap is
always fresh (B2 while flowing *stops* flow). The one edge: a web dice
(ACTION 0) sent while flow is held reaches `RerollFill` with `flowRegen`
set and is treated as a flow bar. A replay draws uniformly over the newest
`kDejaWindow = 8` ring entries (the phrase memory — Marbles' "jump within
the loop"), including the head, so "Lots" sometimes repeats the last bar
immediately. Decisions come from `dejaRng`, a separate xorshift stream
seeded from `Init`'s seed: the same seed and option give the same bar
sequence, and with the option off `seedRng` is untouched (flow is
byte-identical).

**A replay does not push to history.** The ring stays the timeline of *new*
ideas, so a P3/web scrub never lands on a duplicate and the 16-deep memory
is not eaten by echoes. `histPos` moves to the replayed slot, so the P3
depth LED and the state tail's `histDepth` show which earlier bar is
sounding; the next fresh roll pushes at the head as before. A replay uses
the slot's stored knob value (as `Scrub` does) — it is that bar, as it was.

Diagnostics: `dejaBars` / `dejaRecalls` count flow bars seen / replayed.

## 3. Grids accent → `Pattern::accent[]`

In `GenerateLang`, Grids mode: a step is accented when the **firing
class's** map level at the on-16th read (`gs = 2·(s % 16)`) is `> 192`.
Verified against Mutable's `grids/pattern_generator.cc`, `EvaluateDrums`:

```
if (level > threshold) { if (level > 192) accent_bits |= instrument_mask; ... }
```

— i.e. the accent test sits inside the density gate, on the same bilinear
level, which is what we do. (The threshold is not stated in
`bb_grids_map.h`'s header; it is confirmed from the Grids source as cited in
the literature review §2.3 and from the firmware itself.) The 32nd (odd)
read that becomes a ratchet carries no accent.

**Euclid:** the bar downbeat (grid step 0 of every bar) is accented when it
fires — the "one". With the Bresenham form, E(k,16) at rotation 0 fires at
the *end* of each group, so a downbeat hit usually comes from rotation or a
full-density lane; that is the user's choice, the rule just marks it.

**Collins fill-bar figure starts: not accented (decision).** The fill layer
is flattened by `Process` (PLAY-owned) without an accent array, and none of
Collins' texts have an accent rule; adding `faccent[]` would need a
`Process` edit, so it is left for a later round rather than half-done.
Classic sets no accents. `FlattenAll` already carries `pat.accent` into
`Engine::stepAccent[]`; GATES reads it as velocity.

## 4. Grammar index 4: Camp (after Collins' `CampCutProc`)

Change ringing. The bar is N "bells" (equal cells) and each bar plays one
*row* of a ringing method — a permutation of its own cells — with successive
bars ringing successive changes until the bells come round. **Nothing is
drawn**: it is fully determined by the two seeds.

- **Cell seed** (B3 reslice): method — plain hunt (2N rows to rounds) or
  plain bob with its "12" lead end ((N−1)·2N rows, the plain course);
  direction (forwards / backwards through the changes); mirror (bells
  numbered from the bar's end).
- **Fill seed** (B2): the *row index*. For Camp, `RerollFill` steps
  `fillSeed` by **one** instead of drawing — so a B2 tap rings the next
  change and hold-to-flow walks the method bar after bar. Déjà vu still
  applies to flow. History scrubs back to the previous row.
- **Chop knob**: the number of bells — full right = rounds (verbatim, one
  bar cell); ≥ 12 o'clock = 4 bells (beat cells); ≥ 9 o'clock = 8 bells;
  below = 16 bells (every 16th hunts).
- In a multi-bar loop, bar b rings row r0 + b, so a 4-bar loop shows four
  consecutive changes and each flow bar shifts the phrase along the method
  by one.

No rests, no reversal, no rolls: the point is the structured feel the
literature review asked for ("structured, not stochastic"). The dropdown
label "Camp" is served from `Describe`, so the web front end shows it
without changes; the Options clamp accepts 0..4. Grids/Euclid-only shell
rules (`mode >= 2`: rests continue the break, Brain-knob regen) also apply
to Camp and are harmless to it (it has no rests and ignores the Brain
vector).

## Tests (`tests/engine_check.cpp`, four GRAM sections; `tests/run.sh`)

- **Swaps** — with `salSwaps` off, `salTier` is ignored in all four
  grammars (patterns identical); on, over 435 patterns per grammar, no swap
  (a cell playing neither in place nor the generator's `prevSrc`) crosses
  a tier, in Collins' base and fill layers and in the language modes'
  same-class swaps; engine tiers are monotone in salience with the expected
  terciles; uniform salience is one tier; `swapsMode = 0` with salience set
  leaves the pattern unchanged.
- **Déjà vu** — 400 flow bars: Some replays 23.5 %, Lots 74.7 %; replays
  never push; every replay reproduces the ring entry it names exactly; the
  sequence is reproducible from `Init`'s seed; taps never replay; rendered
  flow under Lots stays finite.
- **Accents** — Grids: `accent[s]` equals an independent recomputation of
  "firing class's level > 192" over 27 map/density points; some but not all
  hits are accented; Euclid accents exactly the firing downbeats; Classic
  and Collins set none; `stepAccent` follows `pat.accent`.
- **Camp** — every bar is a permutation of its own cells for 16 cell seeds
  × 3 loop lengths × 5 knobs × 9 rows; deterministic; fill seed + 1 equals
  the next bar's row; rounds recur exactly at the cycle (8 for plain hunt,
  24 for plain bob on 4 bells); knob extremes; engine B2 rings the next
  change and scrub restores the previous row; the rung bar plays as mapped.
- `tests/run.sh` now runs `grammar_check` for mode 4 as well (coverage and
  source-range gate). Classic's hash gate is unchanged.

## How Ernest tests it on the module

1. **Swaps on the Amen.** Load the Amen, Options → Swaps = *Class + salience
   tier*, Chop around 10–11 o'clock (the swap-heavy zone, above the cluster
   field). Tap B2 a dozen times. The backbeat snares should stay
   backbeat-loud — no ghost snares or drags in the 2 and 4 — and the loud
   open hats should not be replaced by ghost ticks. Flip Swaps back to
   *Class only* (regenerates with the same seeds) and compare: the same
   arrangement, now with the occasional quiet hit sitting where a loud one
   was. With Gate Level = velocity, the snare-lane CV should stop dipping on
   the backbeat when the option is on.
2. **Flow déjà vu.** Hold B2 with Déjà Vu = *Off*: every bar new, as today.
   Set *Some*: roughly one bar in four is one you heard in the last eight;
   the P3 depth LED jumps back on those bars. Set *Lots*: three bars in
   four are returns, and you will hear immediate repeats; a fresh bar
   arrives about every fourth bar and enters the memory. Release B2, scrub
   P3: only fresh bars are in the ring (no duplicates).
3. **Grids accents.** Grammar = Grids, Gate Level = velocity, GATES' accent
   handling on: the map's downbeat kicks and backbeats put out a hotter
   gate than the fills around them.
4. **Camp.** Grammar = Camp, Chop at 2 o'clock (4 bells): tap B2 and hear
   the four beats of the bar trade places one change at a time; after eight
   taps the bar is back in order (24 with a plain-bob cell seed — tap B3 to
   change method). Hold B2 for flow and it rings through by itself. Turn
   Chop down for 8 and 16 bells.
