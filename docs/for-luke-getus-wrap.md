# For Luke: `daisy::System::GetUs()` wraps every 21.5 s (17.9 s boosted)

Found on the Alchemy Lab v2 while chasing three field symptoms in
Audiothurgist that turned out to be one bug: a "dropped moment" once every
~20 s, an external-clock follower that lost lock and reset the P2 multiplier
"after a few bars", and ~200 ms hiccups while clocked.

## The bug

libDaisy `per/tim.cpp`:

```cpp
uint32_t TimerHandle::Impl::GetTick() { return tim_hal_handle_.Instance->CNT; }
uint32_t TimerHandle::Impl::GetUs()   { return GetTick() / (GetFreq() / 1000000); }
```

`System::GetUs()` is TIM2's 32-bit CNT divided by ticks-per-µs. TIM2 runs at
2 × PCLK1: **200 MHz** at the 400 MHz core setting, **240 MHz** with boost.
CNT rolls over every 2³² ticks, so `GetUs()` counts 0 → 21,474,836 and
**wraps every 21.47 s** (0 → 17,895,697, **every 17.9 s**, boosted). The
division only changes the units, not when the counter rolls over. Boost
makes it worse.

Every consumer subtracts stamps assuming a 2³²-µs modulus:

- `framework/src/control/musical_clock.cpp:188` — `uint32_t dt = now_us - last_us_;`
  → at the wrap dt ≈ 4.27e9 → clamped to `kClockMaxTickDtUs` (50 ms) →
  **the NCO leaps 50 ms once per wrap** (the dropped moment).
- `ClockFollower::Update` loss detection (`dt_since > 3 × period`) →
  `HandleLoss` once per wrap → re-lock needs 3 pulses (62 ms at 24 PPQN,
  375 ms at 4 PPQN, 1.5 s at 1 PPQN) → the ~200 ms hiccup while clocked.
- `CvEdge`/`CvGate` stamps, and any app-level "pulse within the last N s"
  window built on `GetUs()` (ours dropped, so the app ran its unpatch →
  patch-in path and reset the multiplier).
- `framework/src/surface/control_loop.cpp:66` — `cv_source_->PollEdges(cv_, daisy::System::GetUs());`
- `examples/clock_sync/clock_sync.cpp:72` — `const uint32_t now_us = daisy::System::GetUs();`

`musical_clock.h` documents `Tick(now_us)` as "32-bit wrap-safe", which is
true of the arithmetic but not of the source it is fed.

## Our fix (Audiothurgist v0.9.6, `src/audiothurgist.cpp`)

Control thread: keep a 64-bit tick accumulator, advanced on every poll (and
on every keep-alive during long analyses — it must run at least once per
counter period), and hand the SDK microseconds that really wrap at 2³².
Audio thread: work in raw 32-bit ticks — modular subtraction is exact for
any interval shorter than one counter period, and our extrapolation is
capped at 50 ms anyway.

```cpp
static uint32_t gTicksPerUs = 200u;     // GetTickFreq() / 1e6, read once at boot
static uint64_t gTicks64    = 0;
static uint32_t gLastTick   = 0;

static uint32_t NowUs()                  /* control thread only */
{
    const uint32_t t = daisy::System::GetTick();
    gTicks64 += (uint32_t)(t - gLastTick);   // modular: exact under one period
    gLastTick = t;
    return (uint32_t)(gTicks64 / gTicksPerUs);   // genuinely wraps at 2^32 µs
}
static inline uint32_t TickNow() { return daisy::System::GetTick(); }   /* any thread */

// boot, after hw.Init():
gTicksPerUs = daisy::System::GetTickFreq() / 1000000u;
gLastTick   = daisy::System::GetTick();

// OnPoll (1 ms): every SDK call that wants a timestamp gets NowUs()
const uint32_t now_us = NowUs();
gClkEdge.Tick(loop.Cv(), loop.NumCv(), now_us);
gFollower.Update(now_us);
gClock.Tick(now_us);
snap.tick32 = gLastTick;                 // the tick NowUs() just read, for the ISR

// audio callback: extrapolate from the published snapshot in ticks
uint32_t dtUs = (TickNow() - snap.tick32) / gTicksPerUs;
if(dtUs > 50000u) dtUs = 50000u;
```

## Suggested SDK-level fix

Give the framework one monotonic microsecond source and use it everywhere
`GetUs()` is used today:

1. `alchemy::MonotonicUs()` (or a method on `ControlLoop`) implemented as
   the accumulator above, advanced from `ControlLoop`'s poll — it already
   runs every ~1 ms, and its `t_ms` could be derived from the same counter
   instead of the synthetic `now + elapsed`.
2. `ControlLoop::PollEdges`, `examples/clock_sync`, and anything that
   stamps pulses use it. `MusicalClock`/`ClockFollower` need no change:
   their arithmetic is already modular on a true 2³²-µs stamp.
3. Document `System::GetUs()`/`GetMs()` as "wraps at 2³² ticks of TIM2
   (21.5 s / 17.9 s), not at 2³² µs" — or fix it in libDaisy with a 64-bit
   extension (the `GetTick()` read plus one subtraction per call).

Related observation from the same review: `AlchemyLabV2::Init` calls
`seed.Init()` with the default `boost = false`, so apps run at 400 MHz.
An `Init(sr, block, boost)` overload would be welcome — and note the wrap
period shrinks to 17.9 s if they do.

— Ernest / Audiothurgist, 2026-09-07
