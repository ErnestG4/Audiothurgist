# core/ — the BreakFix engine (vendored)

These headers are the pure, header-only DSP engine shared with
BreakFix (the Daisy Pod original).
"One core, many main()s": `src/audiothurgist.cpp` is the Alchemy Lab main().
No libDaisy, no SDK types, no allocation in the audio path.

- `bb_engine.h` — grid clock, per-step triggers, voices, varispeed, history
- `bb_pattern.h` — Brocot cell tiling + fill moves (the pattern brain)
- `bb_gas.h` — Coulomb-gas β engine (Phase 0 analysis; dormant in triggering)
- `bb_onset.h` — spectral-flux onset detection + attack refine
- `bb_condition.h` — load conditioning: trim, normalize, 4-bar cap, bar snap
- `bb_testbreak.h` — synthesized boot break · `bb_rng.h` — xorshift + gaussian
- `wav_loader.h` — FatFs WAV → mono float loader (shared with BreakFix's Pod shell)
- `bb_wavwrite.h` — WAV writer through a sink (PCM16/24, float32, cues) · `bb_json.h` —
  fixed-schema JSON emitter/parser · `bb_set.h` — sets and preset backups on the card
  (docs/packages/sample-persist.md)

Synced from BreakFix; Audiothurgist is now the living copy.
