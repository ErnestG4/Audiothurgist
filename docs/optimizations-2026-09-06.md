# Optimizations & field-symptom review — 2026-09-06

Read-only review of Audiothurgist v0.9.5 against the alchemy-sdk (framework,
v2 BSP, vendored libDaisy) and capicola, hunting for the cause of two field
symptoms — an occasional ~200 ms hiccup, and "a single moment dropped or a
single gate slipping a tiny bit" — and for optimizations the platform's own
examples suggest. Items marked *guess* were not verified by measurement.
v0.9.6 applies A1, A2 (loader), A3, B2 and B5 below.

## 0. Two premises that turned out false
- **The CPU runs at 400 MHz, not 480.** `AlchemyLabV2::Init` calls
  `seed.Init()` with no argument; libDaisy's default is `boost = false` →
  400 MHz. Nobody in the SDK or capicola calls `Boost()`. (+20 % CPU for
  free once the BSP exposes an `Init(sr, block, boost)` overload — ask Luke.)
- **`daisy::System::GetUs()` is not a 2^32-µs counter.** It is
  `TIM2->CNT / (GetFreq()/1e6)`: TIM2 free-running 32-bit at 200 MHz, so it
  counts 0 → 21,474,836 and **wraps every 21.47 s**. Every `now − prev` in
  our firmware and in the SDK (`musical_clock.h` even says "32-bit
  wrap-safe") assumed a 2^32 µs modulus. Each call also recomputes the PLL
  frequency in float (~1 µs, *guess*).

## 1. Ranked suspects

### A. The ~200 ms hiccup
**A1. `GetUs()` wrap every 21.5 s** (mechanism verified). At the first poll
after the wrap `now_us` is tiny and every stamp is ~21.4e6:
- `MusicalClock::Tick` computes dt ≈ 4.27e9, clamps to 50 ms → **the NCO
  leaps +50 ms in one poll**. Internal clock: the break skips ~48 ms of
  material once every 21.5 s; the next hit lands early. `jumps` never
  ticks (50 ms < 1.5 steps), so it was invisible.
- The audio thread's extrapolation wraps the same way — same leap, one
  block earlier.
- **External clock (the "~200 ms"):** `ClockFollower::Update` sees
  dt_since ≫ 3 periods → `HandleLoss`; re-lock needs 3 pulses = 62 ms at
  24 PPQN, **375 ms at 4 PPQN**, 1.5 s at 1 PPQN. Our 2 s `ext` window
  also wraps → `ext` drops → the full unpatch → patch-in sequence runs:
  follower Disable/Enable, zone reset, **P2 disengaged, map forced to ×1**,
  jitter reset. At re-lock the loop re-aligns by up to A − r. **This is the
  "×2 engaged, then fell back after a few bars" report**: 21.5 s ≈ 10 bars
  at 120 BPM. The zone logic never could have produced it.
- Confirm (v0.9.5): `pollGapUs` saturating to 65.5 ms with stallPhase = 1
  every ~21 s like clockwork; `clkFlags` ext-transition counter +2 per
  21 s; `jumps` stays 0.
- **Fix (done, v0.9.6):** stop using `GetUs()`. The control thread keeps a
  64-bit tick accumulator (advanced every poll / keep-alive) and hands the
  SDK microseconds that really wrap at 2^32; the audio thread works in
  raw 32-bit ticks, modular. Report upstream: `control_loop.cpp` and
  `examples/clock_sync` share the bug.

**A2. Control-thread stalls > 50 ms** (loads, option changes). `bb::Yield`
keeps the snapshot fresh during analyses, but the WAV read loop never
yielded (16 KB chunks at 1-bit SDMMC: a 4 MB file is seconds) → playhead
frozen for the read, then late. Gates unserviced, J3 edges missed during
any stall. Fix (done): `bb::Yield()` after each `f_read`.

**A3. QSPI preset write** (Save gesture, web BlobCommit). `Presets::Save`
blocks for sector erases ("up to several hundred ms"); `presets.SetYield`
existed and nobody in the SDK or capicola used it. Fix (done):
`SetYield(KeepClock)`.

**A4. I²C on the poll thread.** J6 = MCP4728: `SetVolts` = WriteAll (13 B
≈ 300 µs at 400 kHz) + LDAC via two PCA9557 writes ≈ **450 µs per level
change**; B3 is a ~120 µs I²C read every poll. On a NACK each HAL wait
times out at 5 ms; libDaisy spins with no timeout on `GetState != READY`.
Not a 200 ms candidate unless the panel bus glitches (*guess*). Count
`SetVolts()==false` into the diag tail (todo). Consider UDAC=1 single-
channel writes (~100 µs; SDK change).

**A5. WS2812 — not a stall.** `Show()` is DMA (TIM6 → DMA1 → BSRR); CPU
cost is `EncodePixels` (~100–250 µs, *guess*); the 3.4 ms transfer runs in
the background. Only a 50 ms wait-then-abort if the previous transfer is
unfinished — would show as a persistent stallPhase 6.

**A6. HostLink at 10 Hz — cheap.** Non-blocking pump, ≤ 8 frames per poll;
the 24 KB descriptor is rendered once. Only slot writes (A3) and live blobs
that dirty an option (A2) are heavy. USB IRQs at NVIC priority 0 delay
audio by µs, never ms.

**A7. SD re-mount** only after an IO error (hundreds of ms, 2 s backoff).

**A8. Interrupt priorities:** every peripheral IRQ (SAI DMA, SDMMC1, OTG,
I2C, QUADSPI) at preempt 0; SysTick 14; TIM2–5 15. Nothing preempts audio.

### B. Dropped moment / gate slip
**B1. Same as A1** — the 48 ms skip every 21.5 s is exactly "a single
moment dropped / a hit early".

**B2. Gate write latency.** A gate armed in AudioCb is written on the next
poll: `HAL_Delay(1)` = 1–2 ms plus work, plus the Render path once per
frame, plus the kick's 450 µs I²C ahead of snare/hat in the same loop:
worst ≈ 3–3.5 ms, typical ≈ 1.5 ms. Under JND alone but adds to A1/A2.
**Fix (done):** J7/J8 are STM DAC channels in POLLING mode — `WriteValue`
is a register write — so snare/hat are now written inside AudioCb every
other block (1 ms cadence); J6 stays on the poll. No SDK example writes an
MCP jack from the ISR (correct).

**B3. PLL re-lock re-alignment** fires on every re-lock (A1 every 21.5 s,
any stall > 62 ms at 24 PPQN/120 BPM, short pulses missed by the 1–2 ms J3
sampling, *guess*). After A1 re-locks become rare; then consider slewing
the alignment over a beat instead of `Shift`.

**B4. Audio overrun** candidates: `SetMaterial` on apply, `RerollFill` at
the wrap, lazy `FindLoop` (≤ 100 µs, *guess*). Confirm with `overruns`.

**B5. Snapshot publish ordering** (*guess*, cheap): no fence before the
`gSnapIdx` store; the compiler may sink record stores past it. **Fix
(done):** `dmb` before the index publish.

**B6. 50 ms cap** — correct for stalls ≤ 50 ms; A2's yields are the fix
for longer ones, not a larger cap.

## 2. Optimizations to adopt (payoff / effort)
1. Wrap-safe tick time source (A1) — done.
2. J7/J8 gate writes in AudioCb (B2) — done.
3. `presets.SetYield` + `Yield()` in the WAV read loop (A2/A3) — done.
4. Count `SetVolts()` failures / I²C timeouts into the state tail — S.
5. 480 MHz boost once the BSP exposes it — S, +20 % CPU.
6. Compiler flags: we build `-O2`, no LTO, no fast-math. Try `-O3`, the
   safe subset `-fno-math-errno -fno-trapping-math` (full `-ffast-math`
   changes `fmod`/`floor` semantics the clock math relies on), LTO. Measure
   with `audioUs`. *Guess* 10–30 % on the ISR.
7. Skip the reverb when idle (`gVerb.Process` runs every sample even with
   send = 0). S.
8. `Voice::Render`: `double pos` with per-sample `(int)pos`; an int +
   float-fraction phase would trim it. M, only if `audioUs` gets close.
9. Block size: keep 24 (0.5 ms gate/step quantisation is the point).
10. Poll cadence (SDK): `Delay(1)` busy-waits at SysTick granularity; a
    TIM-based deadline would halve I²C gate latency. M, SDK.
11. ITCM for AudioCb + engine: needs a copied section like `.data`. M,
    *guess* 5–15 %. Not before 6–8.
12. DTCM: `.data` + `.bss` = 83.4 KB (65 %); stack region 47.6 KB. Paint
    the stack at boot and report the high-water mark in the diag tail. S.
13. `pollGapUs` clips at 65.5 ms; report ms or u32 so a 200 ms stall reads
    as 200. S.
14. Cache: SDRAM is write-back cached; SD DMA uses `.axi_bss` with
    clean/invalidate and our buffers are 32-aligned. Nothing to change.

## 3. What the examples / capicola do that we should
- `daisy::CpuLoadMeter` (capicola) — cheap, uses `GetTick()`; adopt for
  avg/max as a fraction of the 500 µs budget, reset per second.
- Edge-gated CV writes — we already do this.
- Capicola found the SDK's CMake ships `-O0`; our Makefile path is `-O2`.
  Do not benchmark against the SDK examples.
- Nobody (SDK, capicola) has a watchdog, brownout config, HardFault handler
  or error LED. Better than an IWDG reset (re-enters the bootloader's 2.5 s
  window): a `HardFault_Handler` that stores PC/LR in backup SRAM and lights
  B1 red, exposed via the state tail. The BSP latches `i2c_ready_ /
  mcp4728_ready_ / cal_loaded_` and everyone ignores them — surface at boot.
- `examples/clock_sync` is the canonical follower wiring and, like us
  before the CLOCK package, never phase-aligns at lock; our alignment and
  the `GetUs` wrap fix are both worth offering upstream.

## Suggested order (after v0.9.6)
Flash, watch `pollGapUs` / `clkFlags` for 60 s: the 21.5 s periodicity
should be gone. Then failure counters (4), then flags/boost with `audioUs`
as the yardstick.
