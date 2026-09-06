# CLOCK package — beat-aligned multiplier, lock alignment, jitter meter

Branch `pkg/clock`. Owns the clock pipeline in `OnPoll`, the phase block in
`AudioCb` (both inside `/* CLOCK */` fences in `src/audiothurgist.cpp`), the
pure math in `core/bb_clockmath.h`, and `tests/clock_check.cpp`.

## What changed, in one paragraph

The P2 multiplier no longer scales the NCO tick directly. A **ClockMap**
(`L = T·mult + off`) turns NCO ticks into loop ticks; a zone change is a
*slope* change scheduled for the next external pulse that is also a loop
beat, with `off` re-solved so the loop position is continuous through the
switch — the break simply starts running faster or slower from that beat.
Zones got wider hysteresis and a dwell time. Every PLL lock now aligns the
loop to the clock's pulse grid (the SDK follower never did). Auto-start is
off (it cost ~1 ms of phase per lock). A per-pulse jitter meter goes into the
state tail. Nothing before byte 164 of `GET_STATE` moved.

## The field report: "×2 engaged, then fell back after a few bars"

I could not make the zone logic itself do this — but three mechanisms in
the code can, and none of them is the pot drifting. In order of likelihood:

1. **A parameter lock on P2.** `VirtualKnob::Norm()` is
   `stored + lock delta + CV delta`. B1 is *both* the clock-status LED
   (white flash per pulse, green when locked — the thing you watch while
   patching Pamela's) *and* the lock-record button: hold it ≥ 300 ms while
   P2 moves > 3 % and the SDK records the gesture and **loops it** (16 s
   max). A loop that goes "turn up to ×2 … return" replays as exactly this
   report, on a period of a few bars. **Check:** the web app's `lockMask`
   bit 1 (page 0, P2) — or just ACTION 7 (clear locks) and see if the
   fall-back stops. The zone request now dwells 100 ms and has 0.08 of
   hysteresis, so a *slow* lock replay still flips it (it is a real knob
   motion as far as the firmware knows); the doc note in `OnPoll` says so.
2. **An `ext` flicker.** `ext` is "a pulse within the last 2 s". Any
   control-loop stall longer than that (a load while clocked — finding 3 in
   the overnight notes — or a long HostLink transaction) drops `ext`; the
   next patch-in transition **reset the zone to ×1 and disengaged P2**, so
   the loop fell to ×1 and stayed there until P2 was moved 3 % again.
   `clkFlags` bits 4–7 now count ext transitions: a number that climbs while
   the cable stays in is this. (The reset itself is kept — spec §6.4 wants
   P2 to engage on movement — but it is continuous now, see below.)
3. **The edge at 1:30.** On a 300° pot 1 o'clock is ≈ 0.60, 2 o'clock ≈
   0.72, and the ×1/×2 edge sits at 0.65 (about 1:30). With ±0.02
   hysteresis "engaged at 1 o'clock" meant the knob was at 0.67+ and only
   0.04 of travel from falling back — a brush. Now: enter ×2 above 0.69,
   leave below 0.61, and either must hold 100 ms.

Nothing else touches the multiplier: the PLL's loss/re-lock path, the
native-tempo re-latch, page switches and pot catch all leave it alone
(checked against the SDK sources).

## Zone map (P2 while following)

```
      ÷4        ÷2          ×1            ×2        ×4
   0 ──── 0.15 ──── 0.35 ─────────── 0.65 ──── 0.85 ──── 1
         8:30       10:30           1:30       3:30      (300° pot, 12:00 = 0.5)
```

- Hysteresis ±0.04: a zone is left only 0.04 past its edge, so enter and
  leave are 0.08 ≈ 24° apart. Dwell 100 ms: the new zone must hold before
  it is requested, so a sweep from ×1 to ×4 never requests ×2 on the way.
- P2 engages on a 3 % move after patch-in (unchanged) and reverts to ×1 on
  every patch-in (unchanged, now continuous).
- The zone is a **request**. `multZone` in the state is the request; the
  `f32 mult` already in the state is what is applied; `clkFlags` b1 says a
  switch is pending. Worst wait: one beat at ×1/×2/×4, two beats leaving ÷2,
  four beats leaving ÷4 (the switch instant must be a loop beat *and* a
  pulse instant — see next section).

## Beat-aligned multiplier with phase continuity (`bb::ClockMap`)

Audio thread, per block: `T = tick + frac + extrap` (unchanged), then
`mult = map.MultAt(T)`, `L = map.Loop(T)`, `lp = fmod(L, loopT)/loopT`,
`barSeconds = loopT / (tpu·1e6·mult)`. The map is part of the published
`ClockSnap`, so the audio thread reads a consistent record and chooses the
old or new slope by comparing its *own* T against `swTick` — block-exact
without any cross-thread flag.

Control thread, per poll: `Commit(T)` adopts a switch the audio thread has
already passed; `Request(zoneMult, T, guard, pulseTick, tpp, align)` is
idempotent and schedules when the target differs. The switch instant is the
first `T0 = pulseTick + n·tpp ≥ T + 3 ms` whose `Loop(T0)` is within half a
pulse of a multiple of `align` (= one beat, 96 loop ticks). Both halves
matter: at ×2 every pulse still lands on an even pulse position of the
loop, at ÷2 every other pulse lands on one, and switching back returns to
the same grid — the loop's relationship to the clock survives any sequence
of zones. `pendOff = Loop_old(T0) − m·T0` makes L continuous; the half-step
guard no longer re-arms on a multiplier change because there is nothing to
forgive. Re-requests within 3 ms of a pending switch are deferred one poll
rather than risk the audio thread having already taken the pending map.

Transitions are continuous too: patch-in `SetMultNow(1, T)`; unpatch folds
the applied multiplier into the NCO rate (`SetTicksPerUs(tpu·m)`) in the
same poll it drops the map to ×1, so the tempo you hear **holds** through
an unpatch (checklist §1) instead of collapsing to ×1.

`tests/clock_check.cpp` runs a 1 ms-poll / 0.5 ms-block simulation through
×1→×2→÷2→×1 and ÷4/×4: loop advance per block never leaves [÷2, ×2] of the
×1 advance (no jump anywhere), the guard counts zero jumps, every switch
instant is a pulse instant and a beat, pulses stay on the grid in every
zone, and the guard still passes legitimate jumps (a 0.2-loop backward
jump, a 76-tick lock alignment) while holding torn reads.

## Arming / acquisition review (v0.8.0 path against the SDK sources)

The second-edge arming and the running-NCO acquisition are sound. Details:

- `ClockFollower::Enable()` does `SetSteered(true); Stop(); SetTicksPerUs(0)`.
  Restoring `TicksPerUs` and calling `Start()` in the same poll works because
  `MusicalClock::ApplyDeferredTransport` applies stop then start inside one
  `Tick`, so `running_` ends true. Cost: `Start()` clears `have_last_us_`
  and the next `Tick` integrates nothing — **one poll (~1 ms) of phase is
  dropped once per patch-in.** Unavoidable without an SDK change; harmless.
- **`DeclareLock` does not set phase at all.** It seeds the rate from the
  two-sample EMA period, then `AnchorPhase()` records the NCO's *current*
  position as the reference. The PI thereafter holds pulses `tpp` ticks
  apart relative to that anchor — pulses are never put *on* the tick grid.
  With the NCO free-running through acquisition the sub-pulse offset is
  uniformly random: ≤ 4 ticks at 24 PPQN (≈ 20 ms at 120 BPM, above the
  6 ms JND), up to a full step at 4 PPQN, up to a full beat at 1 PPQN. (The
  SDK's own stopped-NCO path has the same property; it was only hidden by
  a boot-time anchor at tick 0.) **Fix:** on every `Locked()` rising edge
  (first lock and each re-lock after a loss) `OnPoll` shifts the map so the
  lock pulse sits on a multiple of `tpp·max(mult,1)`, capped at one beat —
  we align to the finest grid the clock actually carries and never guess
  the bar. Nearest direction wins; a backward move ≥ 0.4 step becomes the
  forward move so the previous step never retriggers (backward moves under
  half a step are absorbed by the guard). The pulse's NCO position is
  simply T on the poll that saw it: `CvGate` stamps a rise with the poll's
  `now_us`, and `Tick(now_us)` runs before we read it.
- **`SetAutoStart(true)` was a bug**: `DeclareLock` then calls `Start()` on
  the already-running NCO, which drops one poll of dt at every lock *and
  every re-lock*. Now off; `OnPoll` starts the NCO itself in the
  (theoretical) stopped case.
- **Loss policy** `Freewheel`: on 3× period without a pulse the follower
  forgets its anchor, keeps the rate, fires `EXT_LOST`; the next three
  pulses re-lock and re-anchor (→ we re-align). The 2 s `ext` window then
  un-steers if the clock is really gone. Lost-before-lock: the NCO never
  stopped, so the "restart at the pre-clock tempo" branch is now a safety
  net only. Behaves as the checklist expects.
- Changing the PPQN option while following calls `SetExtPpqn`, which
  re-locks — and so re-aligns. Good.

## Jitter meter

Per pulse: the deviation of this period from the running period (EMA,
α = 0.1 — what the PLL has to swallow on that pulse; a single late pulse
reads as its lateness once, where a strict cycle-to-cycle difference would
read it twice). Rolled once a second into `jitterUs` (peak) and
`jitterRmsUs` (RMS), both µs, 16-bit saturated, reset on every patch-in /
unpatch. **Honesty note:** pulses are stamped on the ~1 ms control poll, so
the meter's floor is about one poll gap — read it next to `pollGapUs`; a
peak that tracks `pollGapUs` is us, not the clock. Pamela's should sit near
the floor. A DAW over MIDI-to-CV will not (E-RM 2014: peaks of 38 ms at 24
PPQN); when it does not, that is the clock, and Pamela's manual has the
last word: it can't fix an external clock, and neither can we — the PLL
loses lock at 3× period.

## State tail (append-only, parse by length)

```
… u8 barsOpt                                   (163, v0.8.0)
u16 jitterUs   peak per-pulse period deviation, last second   164..165
u8  multZone   requested P2 zone 0..4 = ÷4 ÷2 ×1 ×2 ×4         166
u8  clkFlags   b0 P2 engaged · b1 switch pending ·             167
               b2 lock aligned (~2 s) · b4-7 ext transitions mod 16
u16 jitterRmsUs                                                168..169
```

164..166 are exactly the round plan's; 167..169 are appended after them so
a parser that stops at 166 is unaffected. For WEB: `multZone` is the
request, `mult` (byte 20) is what plays; showing "×2 (pending)" when
`clkFlags & 2` is the honest display.

## How Ernest tests it

1. Nothing patched: plays at boot, P2 sets tempo (regression, unchanged).
2. Pamela's → J3, PPQN option to match. ~3 pulses, B1 green. `clkFlags`
   bit 2 lights for ~2 s at the lock (the alignment happened), `jumps` may
   tick once. Listen: the break's 16ths sit on Pam's 16ths. At **1 PPQN**
   especially — before this package the loop could sit anywhere in the
   beat after lock; now step 0 is on a Pam beat.
3. P2 zones: move > 3 % to engage, then park at 2 o'clock. `multZone`
   reads 3 immediately, `clkFlags` b1 goes on, `mult` flips to 2.0 on the
   next beat — and the break does not hiccup or re-splice at the switch;
   it doubles from a beat. Back to 12 o'clock: halves from a beat. Full
   left (÷4), then straight to full right (×4): one switch, not three.
4. Park P2 on the 1:30 edge and rock it ±10°: `multZone` must not move.
5. `jitterUs` / `jitterRmsUs` with Pamela's: near `pollGapUs`. If you have
   a MIDI-to-CV from a DAW, patch that instead and watch it climb — that
   is the meter doing its job.
6. The fall-back: if it recurs, look at `lockMask` (ACTION 7 clears) and
   at `clkFlags` bits 4–7 (ext transitions) before blaming the pot.
7. Unpatch at ×2: the tempo you hear holds; B1 dims after 2 s; P2 takes
   the BPM back on move. Repatch: re-locks, re-aligns.

## Least-squares period estimator (rec 8) — evaluated, not wired

`bb::PeriodRegress<N>` (Link's HostTimeFilter idiom: LS slope of stamp vs
index over the last N pulses) is in the header with a test. On a 20.8 ms
train with σ = 2 ms gaussian jitter and a 15 ms late pulse every 100
(E-RM's shape), period error against the truth:

| estimator | max | RMS |
|---|---|---|
| SDK EMA α = 0.15 | 2897 µs | 386 µs |
| LS over 16 stamps | 563 µs | 136 µs |

A 5× tighter base period for the PI, for ~40 multiply-adds per pulse.
It is **not wired in**: `ClockFollower` exposes only `SetGains`; the base
period (`base_per_sec` in `Update`) is computed from the private EMA and
there is no hook to substitute it. Wiring it means either an SDK hook
(`SetBasePeriodUs(double)` or a virtual `PeriodUs()`) or re-implementing
the PI in the app. The ring must be reset on loss/restart (the index axis
assumes no missing pulses) — the follower's `EXT_LOST` event is the cue.
Luke's call, as the review said.

## Not done / known

- Version string left at `0.8.0-options` (shared line; bump at merge).
- The dwell delays a deliberate zone change by 100 ms on top of the wait
  for the beat; both are by design.
- The "1 ms dropped on Start()" costs are SDK behaviour; noted above.
