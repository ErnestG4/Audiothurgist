# SDK and platform quirks — things that cost time, written down so they cost it once

Each entry: symptom · cause · evidence · what to do · upstream ask. Line
numbers are alchemy-sdk v0.11.0 (`7b1cf0c`) plus our local `817d632` unless
stated. Kyklophoria keeps the same register (`docs/sdk-quirks.md` there);
entries marked *(K)* were found there first and checked here.

## `System::GetUs()` wraps every 21.5 s (17.9 s boosted)

**Symptom.** Periodic hiccups; "×2 then falls back after a few bars"; a
dropped moment every ~20 s. **Cause.** `GetUs()` is TIM2's 32-bit counter
divided by 200; spans across the wrap go negative. The SDK's `PollEdges`
and `examples/clock_sync` use it for spans. **Evidence.** The three field
symptoms in `docs/for-luke-getus-wrap.md`, all gone with the fix.
**What to do.** Never use `GetUs()`/`GetMs()` for spans. Control thread:
`NowUs()` (64-bit tick accumulator); audio thread: raw ticks `TickNow()`.
**Upstream.** An `alchemy::MonotonicUs()` fed from `ControlLoop`'s poll;
Luke confirmed the bug 2026-09-06.

## Boost needs a local patch

`AlchemyLabV2::Init` calls `seed.Init()` with the default `boost = false`.
Our `817d632` adds `Init(sr, block, boost)`. **What to do.** Keep the branch
`local/v0.11-boost` until Hermetic's point release lands, then rebase and
drop it. Luke approved boost 2026-09-06; the release has not appeared as of
2026-09-15.

## Static initialisers into `.sdram_bss` *(K, with a correction)*

**Kyklophoria's finding.** A default member initialiser or constructor on an
object placed in SDRAM makes `.init_array` store into `0xC000_0000..` before
`hw.Init()` has configured the FMC → hard fault on every boot.
**What we measured here.** `tools/sdramcheck.sh` reads the ELF's static
initialisers and lists every SDRAM object they touch. Audiothurgist has had
`daisysp::ReverbSc gVerb` (user-provided constructor) in `.sdram_bss` since
v0.1, the initialiser demonstrably stores into it, and every verified stage
booted. Under the Daisy bootloader (`APP_TYPE = BOOT_SRAM`, both projects)
the FMC is already up when the app's initialisers run, so the store lands.
Kyklophoria's fault therefore had another ingredient we have not identified;
a no-bootloader build would fault exactly as described.
**What to do.** Every object of our own in SDRAM is guarded by
`BB_SDRAM_POD(x)` (`std::is_trivially_default_constructible`); library types
and address-taken arrays are listed with reasons in `tools/sdram-allow.txt`;
`tools/sdramcheck.sh` fails on anything else. Run it after adding an SDRAM
object.

## SDMMC's IDMA cannot reach DTCM

**Symptom.** File reads return zeros or garbage names; directory ops work.
**Cause.** Anything FatFs touches — `FIL`, `DIR`, `FILINFO`, staging buffers —
must be in AXI SRAM (`ALCHEMY_SDMMC_BSS`, `alignas(32)` for cache
maintenance), never on the stack (DTCM under `BOOT_SRAM`). Found in the
BeatBlaster era, independently of the SDK's later note. Card work never
under the audio callback; hold `SdCard::BusyGuard`.

## Over-large replies are dropped silently *(K)*

`Send` returns without a word when the frame exceeds the body limit or the
TX ring is momentarily full (`host_link.cpp:474-479`); the host burns its
whole timeout and cannot tell it from a dead module. **What to do.** Page
list replies count-first and stop *before* the body would overflow
(`GET_TABLE` chunks at 39 entries; the sets list and `GET_HITS` should be
audited against 1024). **Upstream.** Answer `TOO_LARGE` / `BUSY`.

## `max_body`: 1024, not 2048

The SDK sizes its CDC rings and frame buffers from `ALCHEMY_HOSTLINK_MAX_BODY`,
all in DTCM: 1024 costs ~6 KB, 2048 ~18 KB and leaves the stack ~17 KB.
Measured in the SCOPE package; 1024 is the ceiling for this module.

## Two avoidable milliseconds per round trip *(K)*

`Pump()` runs first in `Poll()` (`host_link.cpp:72`), so a reply queued
during poll N goes out at N+1; `kChunk` is 256 B with one chunk per poll
(`cdc_transport.h:95`), so a full 1 KB reply takes four polls to leave. RTT
floor ~2 ms small, ~5 ms full body; throughput ceiling ~256 KB/s. This
bounds the Scope tab's frame rate. **Upstream.** Pump after the handler; a
larger chunk per poll.

## `Crc32` is bitwise *(K)*

~40 instructions per byte; a 7.5 KB live blob is ~300k instructions ≈ 0.6 ms
inside a 1 ms poll, which is where "a preset save costs one 10–50 ms poll
gap" partly comes from. A 64-byte nibble table is ~5× faster, identical
output. **Upstream** candidate; local copy possible for our own CRCs.

## No partial write to live state *(K)*

One option change is `BLOB_BEGIN` + N × `BLOB_DATA` + `BLOB_COMMIT` over the
whole managed blob (~12 KB here with motion locks), stop-and-wait; anything
shorter is `SCHEMA_MISMATCH`. That is why an option edit holds the page for
up to a second. The StateExt (0x60–0x6F) is the sanctioned escape for
everything performance-rated. **Upstream.** `SET_COMPONENT idx, bytes`.

## `Pager::SetStored` dereferences `phys` unchecked *(K)*

`pager.cpp:521` → `InitCatch(s, phys[pot])`; `nullptr` compiles and reads
address `0x8` in uninitialised ECC ITCM → double-bit ECC fault. Always pass
a real `float phys[kNumPots]` from `hw.pots[i].Value()`. The panel Edit page
does (`PanelLeave`).

## `Host::Pages()` must be called for knob metadata to be published *(K)*

`loop.Use(page)` makes a page work but does not put it in the descriptor.
Audiothurgist hand-rolls `DescribeManual(host)` and never calls
`host.Pages(...)`; the web page takes knob names from the pager block it
finds. Cap `kMaxPageRefs = 8`; ~7.6 KB SRAM for six pages of six knobs —
SRAM is at 83 % here, so this is a measured decision, not a free one.

## The manual is capped at eight sections

`kMaxSections = 8`; a ninth makes `/program` reject the whole descriptor
("Descriptor error — manual: too many sections"). Fold, don't add.

## Selector knobs are zero-based *(K)*

`.Selector(8).Value()` returns 0..7.

## Web Serial: the parser holds half a frame across an untidy close

A link that did not close leaves the module's COBS parser mid-frame; the
next HELLO is appended, delimited into one malformed frame and dropped
correctly, so the first connect times out and the second works. `Link.start`
now sends a lone zero first and `hello` retries with another zero between
attempts (`web/hostlink.js`). The frame accumulator is capped at 2048 bytes
so a burst of undelimited input resyncs instead of growing the heap.
