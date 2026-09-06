#pragma once
// Cooperative keep-alive for long control-thread analyses. The pure core
// calls bb::Yield() every millisecond or so inside its heavy loops (onset
// detection, classification, bar estimation, conditioning). The shell
// installs a hook that ticks the musical clock and republishes the audio
// thread's clock snapshot, so a load or re-slice no longer freezes the
// playhead for the length of the analysis (review 2026-09-06, M6): the
// audio thread extrapolates from the snapshot but only 50 ms past it.
// Null on the host (tests) — a no-op.
namespace bb {
inline void (*gYield)() = nullptr;
inline void Yield() { if(gYield) gYield(); }
} // namespace bb
