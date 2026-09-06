# Round plan — night of 2026-09-06 ("until noon")

Ernest's mandate: fix the field findings, adopt the literature review's
recommendations **as options wherever behaviour changes**, show button
functions in the web app, use agents, and "put a wonderful toolkit in
people's hands."

## Round 1 — landed on main as v0.8.0 (`ec32fc5`)
Bars estimate + Bars override (the half-speed-against-the-clock bug), Gate
Fires: Onsets only, Options v2 schema (16 bytes, `OPT2`), 48/96/16/8 PPQN,
tempo knob 60–200, second-edge clock arming with the NCO kept running, the
300 ms lock-record guard, ACTION 7 clear locks / 8 page, state tail
(lockMask, native BPM, grid bars, bars option), tests.

## Round 2 — six packages, one agent each, own worktree + branch

Worktrees are siblings (`~/Daisy/Audiothurgist-<pkg>`, branch `pkg/<pkg>`)
so `../alchemy-sdk` and `../DaisySP` resolve. Each package: implement
behind its Options field(s), add host tests to `tests/engine_check.cpp`
(or a new `tests/<pkg>_check.cpp` wired into `tests/run.sh`), keep
`tests/run.sh` green, keep `make` green, commit on the branch, write
`docs/packages/<pkg>.md` (what/why/how to test on hardware). Do not edit
files owned by another package; shell hooks in `src/audiothurgist.cpp`
are the one shared file — keep them minimal and inside clearly marked
`/* <PKG> */` regions so merges are mechanical.

| Package | Owns | Options field(s) | Deliverable |
|---|---|---|---|
| **SLICE** | `core/bb_onset.h`, `core/bb_condition.h`, `core/bb_bars.h`, `tests/bars_check.cpp` | `onsets` (15), `swing` (14), Bars **Auto** behaviour | Log-filtered flux + Dixon peak rule; IOI-cluster bar/downbeat estimator validated on the 1,230-break corpus; swing-aware slice boundaries and step timing |
| **CLOCK** | OnPoll clock section + the audio phase block in `src` | (none new; uses P2 zones) | Beat-aligned multiplier changes with phase continuity; pulse-jitter meter into the state tail (`u16 jitterUs, u8 multZone` after `barsOpt`); host test of the phase math (`core/bb_clockmath.h`) |
| **PLAY** | `Voice`/`TriggerStep` in `core/bb_engine.h` | `fades` (10), `tail` (11) | Density-scaled fades from the keyframe density lens (`sliceDens[]` into the engine); tail policy Gate / Loop forward from a zero crossing / Ping-pong |
| **GRAM** | `core/bb_pattern.h`, `RerollFill`/history in the engine | `swaps` (12), `flow.dejavu` (13) | Salience-tier-aware swaps; déjà-vu flow rerolls from the history ring; Grids accent → `Pattern::accent` (level > 192); optional `CampCut` grammar if time |
| **GATES** | gate arming in `AudioCb`, gate writes in `OnPoll`, `core/bb_classify.h` band-mask output | `gate.lanes` (9) | Multi-label lanes (every band over a relative threshold); accent → velocity; Hold mode retrigger review |
| **WEB** | `web/index.html`, `web/README.md` | — | Responsiveness (clicks before polls, cadence, rAF); Controls panel (button + knob functions per page and grammar); locks display + Clear; page switch; native BPM / bars; Options grouped; manual tab; parse the v0.8.0 tail (+ CLOCK's jitter tail when present) |

Merge order (main): WEB → CLOCK → GATES → PLAY → GRAM → SLICE, running
`tests/run.sh` and `make` after each; then a stage binary and the
checklist for Ernest.

## State-tail layout (append-only, parse by length)
```
… u8 stepClass[64]                       (ends at 148)
u16 pollGapUs u16 audioUs u16 jumps u16 regens   (148..155, v0.7.3)
u16 lockMask  f32 nativeBpm u8 gridBars u8 barsOpt (156..163, v0.8.0)
u16 jitterUs  u8 multZone                          (164..166, CLOCK)
```
ACTION ops: 0 dice · 1 reslice · 2 mute · 3 punch(step,slice) · 4 flow(on)
· 5 scrub(depth) · 6 load(idx) · 7 clear locks · 8 page(0/1).
