# SCOPE package — four channels, zoom, trigger, one request per frame

*Branch `pkg3/scope`, 2026-09-07. Owns the `/* SCOPE */` lines in
`src/audiothurgist.cpp` (the rings, the AudioCb stores, the 0x62 handler,
the descriptor capability), the pure `core/bb_scope.h` with
`tests/scope_check.cpp`, the `max_body` line in the Makefile, and on the
web side `scopeReq / parseScope2 / scopePlan / fetchScopeFrame` in
`hostlink.js`, the fake module's rings in `demo.js`, the Scope tab in
`index.html` and section 6 of `selftest.mjs`.*

## What it is

Ernest: "the scope needs to be zoomed … layer all inputs as different
colored scopes". The old scope was one 1024-sample ring of the mono output
sum, decimated ×4 in the audio callback, read as the last 256 samples
(21 ms) at 5 Hz into a 120 px canvas — no zoom, no trigger, no channels.

Now the firmware keeps **four channels at full rate** — out L, out R, in L
(J1), in R (J2) — in int8 rings of 4096 samples (85 ms) under one write
index, and 0x62 grew a body: channel mask, decimation 1..16, samples per
channel, trigger / peak flags. A **Scope tab** on the page draws the four
traces in their own colours over a time grid, with time zoom (×1 = 5 ms
of every sample, ×16 = 80 ms), display gain, a Schmitt trigger for a
stable trace, peak decimation so a hat survives a long window, afterglow,
freeze, and it polls **as fast as the link allows** (≤ 30 frames/s) while
it shows. The empty body still answers exactly the old reply, so an old
page against new firmware and the new page against old firmware both
work.

## Protocol (0x62, capability `"scope2":1`)

```
← (empty)                → u8 status, u16 256, i8 x[256]           the pre-SCOPE reply:
                           (out L + out R) / 2, one sample in four, the newest 21 ms
← u8 chanMask            b0 out L · b1 out R · b2 in L (J1) · b3 in R (J2)
  u8 decim               1..16 source samples per output sample
  u16 n                  samples per channel, clamped (see below)
  u8 flags               b0 trig · b1 peak · b2 at (+ u32 pos)
  [u32 pos]              with b2: start the window here (a second request of one frame)
→ u8 status              0 ok · 2 BAD_ARGS (chanMask 0 or > 0x0f, decim 0 / > 16, n 0,
                         a body under 5 bytes, `at` without its pos)
  u8 chanMask, u8 decim  echoed
  u16 n                  as clamped: ≤ 512, ≤ (max_body − 10) / channels, n·decim ≤ 3840
  u8 flags               b0 triggered · b1 pos honoured
  u32 pos                ring position of the window start — an absolute sample count
                         (wraps at 2^32), so consecutive frames tell apart and a
                         second request can ask for the same window
  i8 x[n] per channel    in mask order (out L first … in R last)
```

- **Trigger** (`trig`): the window starts on the most recent rising zero
  crossing of the *first selected channel* that still leaves `n·decim`
  samples after it. A crossing is where the signal goes from < 0 to ≥ 0
  after having been ≤ −4 and before reaching ≥ +4 without dipping below 0
  again — a Schmitt trigger walked forward over the usable ring, so ±3 of
  noise around zero never triggers and the crossing reported is the one
  that led to the peak. None found (silence, or nothing loud enough) →
  the newest window, flag clear, and the page says *free-running*.
- **Peak** (`peak`): each output sample is the largest-magnitude sample of
  its `decim` group instead of the first, so a 1 ms click cannot fall
  between picks at ×16. Sign kept.
- **At** (`at` + `pos`): the window starts at `pos` if the ring still holds
  it (`n·decim ≤ now − pos ≤ 3840`), flag b1 set; otherwise the newest
  window with the flag clear, and the page discards the frame as torn.
  This is what lets a frame be split over requests on a module whose
  `max_body` cannot hold every channel: the first request triggers, the
  rest are pinned to its `pos`, and the channels stay sample-aligned.
- Every channel of one reply is read from **one snapshot** of the write
  index, so a single-request frame is aligned by construction.

The full text is in the state-extension comment block above `StateExt`
in `src/audiothurgist.cpp`.

## Firmware

- **Rings**: `int8_t gScopeRing[4][4096]` in SDRAM (`ALCHEMY_SDRAM_BSS`, 16
  KB) and `volatile uint32_t gScopeW`. AudioCb snapshots `gScopeW` once per
  block, stores four int8 per sample (clamp, ×127, truncate — the old
  writer's conversion), and publishes `gScopeW + n` after the loop. SDRAM
  was chosen over AXI SRAM (83 %) and DTCM (72 %): the audio thread already
  streams takes into SDRAM every block, and four byte stores per sample
  are noise beside the reverb. "in" is `in[kCodecInChJ1]` /
  `in[kCodecInChJ2]` — the codec buffers the record path reads with Record
  Source = Inputs, before the Sampling mix. With Record Source = Output the
  record path sees out L/R, which the scope shows anyway.
- **Handler** (control thread): one snapshot, `bb::ScopeParse` (clamps),
  `bb::ScopeWindow` (at / trigger / newest), then `bb::ScopeExtract` per
  channel through a 512-byte stack buffer into the FrameWriter. Worst
  case is one pass over 3840 samples for the trigger plus 4 × 512 × 16
  ring reads for a peak frame — well under a millisecond from SDRAM,
  comparable to `GET_WAVE`'s 61 K. The oldest 256 samples (5 ms) of the
  ring are never read: the writer is live while the reply is built.
- **`max_body` 512 → 1024** (`-DALCHEMY_HOSTLINK_MAX_BODY=1024` in the
  Makefile). The reply header is 10 bytes, so a four-channel frame holds
  `(max_body − 10) / 4` samples per channel: **125 at 512, 253 at 1024, 509
  at 2048**. The SDK sizes its CDC rings (three wire frames each way) and
  the frame parser / response buffers from this constant, all in DTCM
  `.bss`: 1024 costs ~6 KB there; 2048 was built and measured at +18 KB
  (DTCM 86 %, leaving the stack ~17 KB) and rejected. Hosts learn the
  value from HELLO, which always fits 512, and never ask for more than
  they can take, so `/program` and the hostlink CLI are unaffected.
- **No time**: nothing here reads `System::GetUs()`; positions are sample
  counts.

## Memory (before → after, `arm-none-eabi-size` and the linker map)

| | before | after |
|---|---|---|
| text | 366 540 | 367 280 (+740) |
| DTCMRAM | 94 576 B · 72.16 % | 99 696 B · 76.06 % (the SDK's buffers at 1024; the old 1 KB ring left) |
| SRAM (AXI) | 408 096 B · 83.03 % | 409 344 B · 83.28 % |
| RAM_D2_DMA | 18 204 B · 55.55 % | unchanged |
| SDRAM | 39 052 648 B · 58.19 % | 39 069 032 B · 58.22 % (+16 KB rings) |

## Web

- **`scopePlan(maxBody, mask, decim)`** picks `n` and the requests of a
  frame: the whole mask in one request when that leaves ≥ 200 samples per
  channel (1024: 253 × 4; 2048: 509 × 4), else the mask split over as few
  requests as hold the largest `n` (512: two requests of two channels ×
  251), the split capped 480 samples short of the ring so the later
  requests can still find the first one's window. `n·decim ≤ 3840` always
  (×16 → 240 samples = 80 ms).
- **`fetchScopeFrame`**: the first request carries the trigger, the rest
  ask for its `pos`; a request the ring could no longer honour throws
  ("scope frame torn") and the loop just takes the next frame.
- **Scope tab** (`#scope`): a 380 px canvas with ten time divisions and a
  ms/div readout, the four traces (out L green, out R yellow, in L blue,
  in R coral — the page's own palette), a clip pill when a sample sits at
  full scale, a trigger marker at the left edge. Channel buttons: out L/R
  on; the inputs are *auto* (dashed) — requested always, drawn while they
  have carried a signal in the last 3 s — and a click forces either on or
  off. Time ×1…×16, Gain ×1…×8, Trigger (default on), Peak, Afterglow (the
  previous two frames dimmer), Freeze (stop polling, keep the last frame).
  Pills: achieved frames/s · requests per frame · KB/s · the last frame's
  round trip; `n = 253, ×4 → 21 ms · triggered`.
- **Cadence**: while the tab shows the next frame is requested the moment
  the last lands, floored at 33 ms (≤ 30 frames/s), and the state poll
  eases from 10 to 4 Hz; the Link's FIFO means a queued state poll goes
  out between two frames, never starved. Off the tab the Play tab's small
  scope keeps its 5 Hz empty-body request (and so keeps proving the old
  path). Tab hidden → nothing polls.
- **Old firmware** (no `scope2` in the descriptor): the tab sends the empty
  body and draws the mono trace with a *mono output (old firmware)* pill.
- **Demo**: `?demo=scope` opens the tab on the fake module, whose rings are
  synthesised from its sample clock (a kick / snare / hat / tone burst per
  step on out L, out R a little different, a 110 Hz saw on in L, a 440 Hz
  tremolo tone on in R); `noin` silences J1/J2, `body512` announces the
  protocol's default `max_body` so a frame takes two requests.

## What was measured

- Host: `tests/scope_check.cpp`, 48 checks — parse and every clamp, the
  trigger on a 100 Hz sine (on the grid, the most recent crossing that
  fits, the first selected channel decides), ±3 noise and silence
  untriggered, a lone pulse, a crossing that has aged out of the ring,
  channel alignment (out R == −out L sample for sample, a ramp on in R at
  the reported pos), plain vs peak decimation, at-pos re-reads (honoured,
  too old, too recent, exactly newest), the old reply within 1 LSB of the
  old writer at the same instants, and the 2^32 wrap.
- Self-test: 161 checks (was 118) — the parsers, the planner at 512 / 1024 /
  2048, and over the fake module: the empty body unchanged, a 1022-byte
  four-channel frame, trigger, moving pos, peak ≥ plain on the same window,
  the two-request plan at 512 with the second pinned, a stale pos not
  honoured, every BAD_ARGS, the clamps, silent inputs, muted outputs, the
  old and new requests keeping their own coalescing keys.
- Frame rate: on the fake port (6 ms latency) the tab holds the 30 frames/s
  floor. On the module a frame is one 1022-byte reply, ~1.05 KB on the
  wire; the SDK's CDC transport sends 256-byte chunks from the 1 ms poll,
  so a frame is ~4–5 ms of transfer plus the ~2–5 ms round trip the link
  pill already shows — **expect 20–30 frames/s, 20–30 KB/s**, and the pill
  will say what it got. Not measured on hardware from this worktree; the
  hike checklist item is below.

## How Ernest tests it

1. Flash, connect, **Scope** tab. Out L/R trace the beat; the trigger
   marker sits at the left edge and the kicks stand still. The fps pill
   should read 20+.
2. Patch anything into J1: the *in L* button lights by itself and the
   blue trace appears (3 s after the signal stops it hides again; click it
   to pin).
3. Time ×1 on a kick: the 55 Hz cycles. ×16: the whole step. Peak on at
   ×16: the hats stay visible.
4. Gain ×4 on a quiet input: the clip pill lights when the trace hits the
   edge.
5. Trigger off: free-running. Freeze, then unfreeze.
6. Kit and Play tabs still behave (state back to 10 Hz — the link pill's
   req/s drops when leaving the Scope tab).
7. Old page (the Pages build from before this package) against this
   firmware: its Play-tab scope still draws.

## Not done / known

- `max_body` stays 1024 rather than 2048 for DTCM's sake; 253 samples per
  channel per frame is the trade. A two-channel frame (only out L/R shown)
  gets 507.
- The trigger channel is the first *selected* one, in mask order; with
  out L hidden and out R shown it triggers on out R, which is the natural
  reading. There is no trigger-level control (zero crossing only).
- The inputs' *auto* state is data-driven (the state tail's `inLevel` only
  moves during a take, so it could not serve).
- The Play tab's small scope was left on the old request on purpose.
- Version string not bumped (shared line; at merge).
