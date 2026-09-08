# SCOPE package — ten channels on one clock: audio and the CV jacks, zoom, trigger, one request per frame

*Branch `pkg3/scope`, 2026-09-08. Owns the `/* SCOPE */` lines in
`src/audiothurgist.cpp` (the rings, the AudioCb stores, the ADC pointers and
calibration constants, the 0x62 handler, the descriptor capability), the
pure `core/bb_scope.h` with `tests/scope_check.cpp`, the `max_body` line in
the Makefile, and on the web side `scopeReq / parseScope2 / scopePlan /
fetchScopeFrame` and the jack tables in `hostlink.js`, the fake module's
rings in `demo.js`, the Scope tab in `index.html` and section 6 of
`selftest.mjs`.*

## What it is

Ernest: "the scope needs to be zoomed … layer all inputs as different
colored scopes", and then: make it useful as a Eurorack scope — J1/J2 are
AC-coupled and cannot show an LFO, an envelope or a sequence; J3–J8 can.
The old scope was one 1024-sample ring of the mono output sum, decimated
×4 in the audio callback, read as the last 256 samples (21 ms) at 5 Hz
into a 120 px canvas — no zoom, no trigger, no channels.

Now the firmware keeps **ten channels on one sample clock** in two tiers:

- **audio** — out L, out R, in L (J1), in R (J2) at full rate (48 kHz),
  int8 rings of 64 K samples (1.37 s);
- **cv** — J3..J8 DC-coupled at ±5 V = ±127, one sample per audio block
  (2 kHz), int8 rings of 16 K samples (8.2 s), read straight from ADC1's
  DMA buffer in the audio callback.

0x62 grew a body: channel masks, a decimation per tier, samples per
channel, trigger / peak flags, a trigger level and source. A **Scope tab**
on the page draws the traces in their own colours over a time grid — 1 V
per division once a jack shows — with time zoom per tier (audio ×1 = 11 ms
of every sample … ×16 = 171 ms; cv ×1 = 256 ms … ×32 = 8 s), display gain,
a Schmitt level trigger on any channel, peak decimation so a hat or a 6 ms
gate survives a long window, afterglow, freeze, presets (Audio · CV ·
Gates), each jack labelled by its role under the current jack map with its
present voltage — and it polls **as fast as the link allows** (≤ 30
frames/s) while it shows. The empty body still answers exactly the old
reply, so an old page against new firmware and the new page against old
firmware both work.

## Protocol (0x62, capability `"scope2":2` — 1 was the audio body alone)

```
← (empty)                → u8 status, u16 256, i8 x[256]           the pre-SCOPE reply:
                           (out L + out R) / 2, one sample in four, the newest 21 ms
← u8 chanMask            b0 out L · b1 out R · b2 in L (J1) · b3 in R (J2)
  u8 decim               1..16 audio samples per output sample
  u16 n                  samples per channel, clamped (below)
  u8 flags               b0 trig · b1 peak · b2 at · b3 ext
  [u32 pos]              with b2 or b3 (read only with b2): start the window here
  [u8 maskHi]            with b3: b0..b5 = J3..J8
  [u8 cvDecim]           with b3: 1..64 cv samples (2 kHz) per output sample
  [i8 trigLevel]         with b3: the trigger level in int8 units (0 for audio; 25 ≈ 1 V for a gate)
  [u8 trigSrc]           with b3: 0 = the first selected channel (audio before cv), or 0x80 | 0..3
                         an audio channel, 0x80 | 4..9 = J3..J8 — it need not be selected
→ u8 status              0 ok · 2 BAD_ARGS (no channel at all, mask bits past the channels,
                         decim 0 / > 16, cvDecim 0 / > 64, n 0, a bad trigSrc, a short body)
  u8 chanMask, u8 decim  echoed
  u16 n                  as clamped: ≤ 512, ≤ (max_body − header) / channels, n·decim ≤ 65 280
                         (audio), n·cvDecim ≤ 16 368 (cv)
  u8 flags               b0 triggered · b1 pos honoured · b2 the ext header follows · b3 detached
  u32 pos                the window start on the AUDIO sample clock, wrapping at 1 610 612 736
                         (= 24 · 2^26, a multiple of the block and of both rings); the cv start
                         is pos / 24. Consecutive frames tell apart, a second request can ask
                         for the same window.
  [u8 maskHi, u8 cvDecim, u8 cvDiv (24), u8 trigSrc]   the ext header: trigSrc = the channel
                         the trigger ran on (0..9), 255 none
  i8 x[n] per channel    audio channels in mask order, then J3..J8 in bit order
```

- **Trigger** (`trig`): the window starts on the most recent rising
  crossing of `trigLevel` on the trigger channel that leaves *every
  selected tier's* span after it (an audio trigger looks back far enough
  for the cv window too; a cv trigger for the audio one). A crossing is
  where the signal goes from < level to ≥ level after having been
  ≤ level − 4 and before reaching ≥ level + 4 without dipping below the
  level again — a Schmitt trigger, so ±3 of noise never triggers and the
  crossing reported is the one that led to the peak. It is searched
  **backwards** from the newest sample, linear (a failed candidate skips
  to the sample that failed it), so the usual cost is the distance to the
  last crossing; silence walks the usable ring once (65 K samples ≈ 0.3
  ms). A gate rests at 0 V, so the page triggers a jack at 1 V. None found
  → the newest window, flag clear, *free-running* on the page.
- **Peak** (`peak`): each output sample is the largest-magnitude sample of
  its group instead of the first — a 1 ms click at audio ×16, a 6 ms gate
  at cv ×32. Sign kept.
- **At** (`at` + `pos`): the window starts at `pos` if every selected tier
  still holds it, flag b1 set; otherwise the newest window with the flag
  clear, and the page discards the frame as torn. This is what lets a frame
  be split over requests when `max_body` cannot hold every channel: the
  first request triggers, the rest are pinned to its `pos`.
- **Detached** (reply b3): the two tiers start at the same instant whenever
  both can hold it. A cv window that starts seconds ago is beyond the
  audio ring (1.37 s); the audio tier then shows its newest window and the
  reply says so (the page prints a notice). In the free-running case the
  start is the older of the two tiers' newest starts, so a 4 s cv window
  with audio shown is always detached — by design, not a fault.
- Every channel of one reply is read from **one snapshot** of the clock, so
  a single-request frame is aligned by construction; the cv index is that
  count / 24, so the tiers are coherent too.

The full text is in the state-extension comment block above `StateExt`
in `src/audiothurgist.cpp`.

## Firmware

- **Rings** (SDRAM, `ALCHEMY_SDRAM_BSS`): `int8_t gScopeRing[4][65536]`
  (256 KB) and `gScopeCv[6][16384]` (96 KB); `volatile uint32_t gScopeW`,
  the audio sample count, published once per block as `ScopeAdd(w, n)`
  (wrap 24·2^26). AudioCb snapshots it, stores the six cv codes at slot
  `(w / 24) & 16383`, then four int8 per audio sample (clamp, ×127,
  truncate — the old writer's conversion). SDRAM was chosen over AXI SRAM
  (83 %) and DTCM (72 %): the audio thread already streams takes into
  SDRAM every block, and ten byte stores per sample plus six ADC reads per
  block are noise beside the reverb. "in" is `in[kCodecInChJ1]` /
  `in[kCodecInChJ2]` — the codec buffers the record path reads with Record
  Source = Inputs, before the Sampling mix.
- **The cv tier** reads `hw.seed.adc.GetPtr(kCvAdcOffset + i)` — the raw
  16-bit DMA slot of J3..J8 — **not** `cv_jacks[i].Volts()`, which goes
  through a one-pole `AnalogControl` updated at the 1 kHz control poll.
  ADC1 runs the twelve channels (six pots, then six CVs; `InitSingle`,
  continuous DMA) at 16 bits with 32× oversampling and 8.5 sample + 8.5
  conversion cycles each, from PLL3R = 16 MHz / 6 × 295 / 32 = 24.58 MHz
  through `ADC_CLOCK_ASYNC_DIV2` = 12.29 MHz: 544 cycles per channel, 6528
  per scan → **a scan every 531 µs, ≈ 1.88 kHz per channel**. One store per
  0.5 ms block is the natural period: every value is stored once, about one
  in sixteen twice. Volts as `CvInput::Volts()`: `(zero − raw) · VDDA /
  65535 / 0.165` (the input stage inverts; the ADC spans ±10 V at the jack,
  the panel ±5 V — beyond ±5 V clips to ±127), with the calibration
  record's `adc_zero_code` per jack and `vdda_at_cal` under the same
  3.0..3.6 V window `AlchemyLabV2::Init` applies, else the design 32768 /
  3.30 (`bb::ScopeCvScale`, `bb::ScopeCvCode`). A jack in output mode
  reads back what its DAC drives (±50 mV), so J6/J7/J8 under the Brain map
  show our own gates.
- **Handler** (control thread): one snapshot, `bb::ScopeParse` (clamps),
  `bb::ScopeWindow` (at / trigger / newest, aligned or detached), then
  `bb::ScopeExtract` per channel through a 512-byte stack buffer into the
  FrameWriter. Worst case is the 65 K backward walk in silence plus ≤ 10 ×
  512 × 64 ring reads for a peak frame — comparable to `GET_WAVE`'s 61 K
  and well under a millisecond from SDRAM. The oldest 256 audio / 16 cv
  samples are never read: the writer is live while the reply is built.
- **`max_body` 512 → 1024** (`-DALCHEMY_HOSTLINK_MAX_BODY=1024` in the
  Makefile). The reply header is 10 bytes (14 with the ext), so a frame
  holds `(max_body − header) / channels` samples per channel: **four audio
  channels 125 at 512, 253 at 1024, 509 at 2048**; out L + J6/J7/J8 252 at
  1024. The SDK sizes its CDC rings (three wire frames each way) and the
  frame parser / response buffers from this constant, all in DTCM `.bss`:
  1024 costs ~6 KB there; 2048 was built and measured at +18 KB (DTCM 86 %,
  leaving the stack ~17 KB) and rejected. Hosts learn the value from
  HELLO, which always fits 512, and never ask for more than they can take,
  so `/program` and the hostlink CLI are unaffected.
- **No time**: nothing here reads `System::GetUs()`; positions are sample
  counts. `static_assert(kScopeCvDiv == kEngineBlockSamples)` ties the cv
  rate to the block.

## Memory (before → after, `arm-none-eabi-size` and the linker map)

| | before | after |
|---|---|---|
| text | 366 540 | 370 440 (+3.9 KB; it runs from SRAM) |
| DTCMRAM | 94 576 B · 72.16 % | 99 744 B · 76.10 % (the SDK's buffers at 1024; the old 1 KB ring left) |
| SRAM (AXI) | 408 096 B · 83.03 % | 412 512 B · 83.93 % |
| RAM_D2_DMA | 18 204 B · 55.55 % | unchanged |
| SDRAM | 39 052 648 B · 58.19 % | 39 413 096 B · 58.73 % (+352 KB rings) |

## Web

- **`scopePlan(maxBody, mask, decim, nWant, minN, maskHi, cvDecim)`** picks
  `n` and the requests of a frame: every selected channel in one request
  when that leaves ≥ 200 samples per channel (1024: 253 × 4 audio; 252 for
  out L + three gates), else the channels split over as few balanced
  requests as hold the largest `n` (all ten: 2 × 5 at 202; the six jacks:
  2 × 3 at 336; a 512-byte module: 2 × 2 at 251), the split capped 480
  audio samples short of the rings so the later requests can still find
  the first one's window. `n·decim ≤ 65 280`, `n·cvDecim ≤ 16 368` always.
- **`fetchScopeFrame(link, plan, decim, flags, opts, ext)`**: the first
  request carries the trigger, the rest ask for its `pos`; a request the
  ring could no longer honour throws ("scope frame torn") and the loop just
  takes the next frame. `ext = { cvDecim, level, trig }` goes with any
  frame that has a jack or an explicit trigger source.
- **Scope tab** (`#scope`): a 400 px canvas with ten time divisions and a
  ms/div readout; with a jack shown, a volts axis at 1 V per division over
  ±5 V (the audio's ±1 sits on the same height). Traces: out L mint, out R
  cyan, in L blue, in R pink; J3 orange, J4 purple, J5 white, J6 / J7 / J8
  the page's kick / snare / hat colours. Each tier is drawn over its own
  window on the shared axis (`audio 85 ms · cv 250 ms`), both starting at
  the frame's instant; a trigger marker in the trigger channel's colour; a
  clip pill; a notice when the audio tier detached. Channel buttons: out
  L/R on; in L/R *auto* (dashed) — requested always, drawn while they have
  carried a signal in the last 3 s; J3..J8 off by default, each labelled
  `J6 · kick gate out` under the current map (state byte 175) with its
  present voltage from the frame's last sample. Controls: Audio time
  ×1…×16, CV time ×1…×32, Gain ×1…×8, Trigger (default on) with a source
  select (auto = the first shown, audio before cv; level 0 for audio, 1 V
  for a jack), Peak, Afterglow, Freeze. Presets: **Audio** (out L/R, ×4),
  **CV** (the jacks in input mode under the current map — J3/J4/J5, and J6
  in the Voice map — 1 s, free-running), **Gates** (J6/J7/J8 over out L:
  ~250 ms of gates, 85 ms of audio, triggered on J6 at 1 V). Pills:
  achieved frames/s · requests per frame · KB/s · the last frame's round
  trip; `n = 252, audio ×16 → 84 ms, cv ×2 → 252 ms · triggered on J6`.
- **Cadence**: while the tab shows the next frame is requested the moment
  the last lands, floored at 33 ms (≤ 30 frames/s), and the state poll
  eases from 10 to 4 Hz; the Link's FIFO means a queued state poll goes
  out between two frames, never starved. Off the tab the Play tab's small
  scope keeps its 5 Hz empty-body request (and so keeps proving the old
  path). Tab hidden → nothing polls.
- **Old firmware** (no `scope2`): the tab sends the empty body and draws
  the mono trace with a *mono output (old firmware)* pill; `scope2: 1`
  (audio only) leaves the jack buttons disabled.
- **Demo**: `?demo=scope` opens the tab on the fake module, whose rings
  are functions of its sample clock (a kick / snare / hat / tone burst per
  step on out L, out R a little different, a 110 Hz saw on in L, a 440 Hz
  tremolo tone on in R; J3 a clock pulse per step while clocked, J4 a
  0.3 Hz ±5 V LFO, J5 a 50 % gate per step, J6/J7/J8 the class gates);
  `cv` / `gates` press that preset, `noin` silences J1/J2, `body512`
  announces the protocol's default `max_body`.

## What was measured

- Host: `tests/scope_check.cpp`, 96 checks — parse and every clamp (both
  body forms, the ext fields, the trigger source), the cv transform (idle,
  ±5 V, 1 V, the clip beyond ±5 V, polarity, a moved zero code), the
  trigger on a 100 Hz sine (on the grid, the most recent crossing that
  fits, the first selected channel decides, ×16 × 512), ±3 noise and
  silence untriggered, a lone pulse, a crossing that aged out, the backward
  search against a forward Schmitt reference on 1040 random windows at two
  levels, channel alignment (out R == −out L sample for sample, a ramp at
  the reported pos), plain vs peak decimation, at-pos re-reads (honoured,
  too old, too recent, exactly newest), the cv tier (a 6 ms gate = 12
  samples at 2 kHz on the step grid, an 8 s LFO window, audio-triggered and
  J6-triggered mixed frames aligned with the kick under its gate, a 4 s cv
  window detaching the audio, at-pos across tiers, J3 pulses and a J5
  staircase, channel order), the old reply within 1 LSB of the old writer
  at the same instants, and the wrap of both clocks.
- Self-test: 186 checks (was 118) — the parsers, the planner at 512 / 1024
  / 2048 with and without jacks, volts and roles, and over the fake
  module: the empty body unchanged, a 1022-byte four-channel frame,
  trigger, moving pos, peak ≥ plain on the same window, the two-request
  plan at 512 with the second pinned, a stale pos not honoured, every
  BAD_ARGS of both forms, the clamps, silent inputs, muted outputs, a
  cv-only frame (3 + 3 × 336), a mixed frame aligned, the gates preset
  triggered on J6 with the kick under it, an 8 s LFO detaching the audio,
  all ten channels in two pinned requests.
- Frame rate: on the fake port (6 ms latency) the tab holds the 30
  frames/s floor for one-request frames (31 fps, 30 KB/s measured in a
  browser-less run of the page). On the module a frame is one 1022-byte
  reply, ~1.05 KB on the wire; the SDK's CDC transport sends 256-byte
  chunks from the 1 ms poll, so a frame is ~4–5 ms of transfer plus the
  ~2–5 ms round trip the link pill already shows — **expect 20–30
  frames/s, 20–30 KB/s** for one-request frames, half that for split
  ones, and the pill will say what it got. Not measured on hardware from
  this worktree; the hike checklist item is below.

## How Ernest tests it

1. Flash, connect, **Scope** tab. Out L/R trace the beat; the trigger
   marker sits at the left edge and the kicks stand still. The fps pill
   should read 20+.
2. Patch anything into J1: the *in L* button lights by itself and the
   blue trace appears (3 s after the signal stops it hides again; click it
   to pin).
3. **Gates** preset: three gates over the kick, J6 red rising at the left
   edge, the kick's first cycles right under it, J7/J8 further along. The
   J6 button reads `kick gate out · +4.9x V` while the gate is up.
4. Patch an LFO into J4, **CV** preset, CV time ×32: the whole LFO over
   8 s, the J4 button's voltage following it. A sequence into J4 at ×4:
   the steps. J3 with a clock: the pulses — Trigger on J3 at 1 V stands
   them still.
5. Time ×1 on a kick: the 55 Hz cycles. ×16: the whole step. Peak on at
   ×16: the hats stay visible.
6. Gain ×4 on a quiet input: the clip pill lights when the trace hits the
   edge.
7. Trigger off: free-running. Freeze, then unfreeze.
8. Kit and Play tabs still behave (state back to 10 Hz — the link pill's
   req/s drops when leaving the Scope tab).
9. Old page (the Pages build from before this package) against this
   firmware: its Play-tab scope still draws.
10. The calibration: with nothing patched every jack in input mode should
    read within ±0.1 V of 0 (the record's zero codes); without a
    calibration record the design fallback is good to ~3–5 %.

## Not done / known

- `max_body` stays 1024 rather than 2048 for DTCM's sake; 253 samples per
  channel per four-channel frame, 202 with all ten, is the trade.
- The cv tier stores at 2 kHz against an ADC that scans at ≈ 1.88 kHz: one
  value in sixteen is stored twice. Honest, and the doc says so; a store
  every 25 samples would drift the other way.
- There is no trigger-level control on the page (0 for audio, 1 V for a
  jack, in the protocol as `trigLevel` if one is wanted later) and no
  falling-edge trigger.
- A cv window longer than the audio ring detaches the audio tier by
  design; the page says so on the canvas.
- The inputs' *auto* state is data-driven (the state tail's `inLevel` only
  moves during a take, so it could not serve).
- The Play tab's small scope was left on the old request on purpose.
- Version string not bumped (shared line; at merge).
