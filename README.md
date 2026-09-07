# Audiothurgist

**A sampling, slicing and rearranging toolkit for the Hermetic Modular
Alchemy Lab.** Load a break from the card, and one knob travels from *the
record, verbatim* to chops, rolls and bursts. Patterns hold until you ask
for new ones. Every sound is a slice of real material played from its
attack, on the grid: no grains, no time-stretch. If you don't like it,
re-slice it.

Audiothurgist is the Eurorack form of [BreakFix](https://codeberg.org/combust/BreakFix)
(Daisy Pod), rebuilt for the [Alchemy Lab v2](https://hermeticmodular.com)
(Daisy Seed 2 DFM, six pots with LED rings, three buttons, ten jacks, SD,
USB). **Status: alpha.** It works and it is being played, but the manual
is in the firmware and the edges are still being filed.

## Three roles, one engine

- **Chopper** — the knobs and buttons: slice, rearrange, dice, flow,
  history, tempo, pitch.
- **Voice** — the rack plays *it*: a v/oct + gate pair on J4/J5 calls
  slices from a keyboard or sequencer (choke or punch-into-the-bar).
- **Brain** — it plays the rack: kick, snare and hat gates on J6/J7/J8 as
  the pattern runs; a clock on J3 locks the loop to your master.

The roles run at once; nothing needs a mode.

## Flashing

1. Rack and power the module, panel USB-C to the computer.
2. Open Hermetic's web flasher (`/program`) in Chrome or Edge and flash
   `audiothurgist.bin` (a release build is attached to each tag; see
   *Building* to make your own).
3. FAT32 microSD with WAVs in the root — 1 to 4 bar breaks are ideal.

If a build ever leaves the module USB-silent, tap **BOOT** during the
power-up breathing pulse to reach the Daisy bootloader, then flash.

## Controls

| | Perform page | Brain page (B1 tap) |
|---|---|---|
| P1 | **Chop** — verbatim (right) → beats trade → chops → rolls → cluster zone (left) | Map X (Grids) · bells (Camp) |
| P2 | **Tempo** 60–200 BPM, varispeed, snaps to the file's own tempo · while clocked: ÷4 ÷2 ×1 ×2 ×4 | Map Y (Grids) |
| P3 | **History** — the last sixteen patterns | Kick density |
| P4 | **Pitch** ±12 semitones, onsets stay on the grid | Snare density |
| P5 | **Sample** — highlight a file on the card | Hat density |
| P6 | **Volume** (reverb rides the cluster zone) | Rotation (Euclid) |

**B1** tap: Perform ↔ Brain · held 300 ms + turn a pot: record that pot's
motion as a loop (hold + nudge again clears; a green pip marks a locked pot;
a dim blue-grey ring is a pot waiting for you to sweep through its value).
**B2** tap: dice (new arrangement) · hold: flow (a new bar every loop).
**B3** tap: reslice · hold: load the highlighted file.
**B2 + B3** held 2 s: Settings (brightness, preset save/load).

Jacks: J1/J2 audio in (recording: coming) · **J3 clock in** · **J4 v/oct ·
J5 gate** (Voice) · **J6/J7/J8 kick/snare/hat gates out** · J9/J10 audio out.

## Options (in the web app or Hermetic's `/program`)

| Option | Choices (default in bold) |
|---|---|
| Classifier | **Hockman 3-band** · Legacy 120/900 (hi/lo) · Two-band · Density (keyframes) |
| Clock In | **24** · 4 · 2 · 1 · 48 · 96 · 16 · 8 PPQN |
| Voice | **Choke** · Punch (writes the bar) |
| Gate Length | **6 ms** · 20 ms · 50 ms · Hold to next hit |
| Gate Level | **Fixed 5 V** · Velocity (accents to the ceiling) |
| Grammar | **Classic** · Collins · Grids · Euclid · Camp |
| Gate Fires | Every step · **Onsets only (once per hit)** |
| Bars | **Auto (backbeat)** · 1 · 2 · 4 · 8 — fixes a break playing at half or double speed |
| Rests | **Continue the break** · Gate (silence) |
| Gate Lanes | **Dominant class** · Multi-label |
| Fades | Fixed · **Adaptive (keyframe density)** |
| Slice Tail | **Gate** · Loop forward · Ping-pong |
| Swaps | Class only · **Class + salience tier** |
| Flow Déjà Vu | Off · **Some** · Lots |
| Swing | Straight · **Auto (the material's swing)** |
| Onsets | Spectral flux · **Log-filtered flux** |

## The web app

`web/` is a single-page WebSerial instrument view: the bar ring and loop
strip, per-slice waveform envelopes, an oscilloscope, every button and
knob function for the current page and grammar, locks, options, presets,
the manual, and a diagnostics line. Serve it from a secure context:

```sh
cd web && python3 -m http.server 8080     # then http://localhost:8080 in Chrome/Edge
```

Close `/program` first (one page holds the port). Press **Demo** to try it
without the module. `node web/selftest.mjs` checks the protocol against
the SDK's golden vectors.

## Building

Siblings expected next to this repo: [`alchemy-sdk`](https://github.com/hermetic-modular/alchemy-sdk)
(v0.11 with its vendored libDaisy; until Hermetic's point release the 480 MHz
boost needs a one-line local patch to `AlchemyLabV2::Init` — see
`docs/optimizations-2026-09-06.md`) and `DaisySP/DaisySP-LGPL`. Then:

```sh
git submodule update --init          # vendor/capicola (not --recursive)
make                                 # build/audiothurgist.bin
tests/run.sh                         # host test suite (needs g++, ASan)
```

The pure engine lives in `core/` (no hardware, no allocation); the Alchemy
Lab shell is `src/audiothurgist.cpp`; `tests/` holds a grammar-hash gate,
an engine suite and a clock suite under ASan/UBSan, plus corpus tools.

## Documents

`docs/audiothurgist-spec.md` (the design), `docs/lit-review-2026-09-05.md`
(the literature behind the choices), `docs/packages/*.md` (each subsystem),
`docs/optimizations-2026-09-06.md`, and `docs/for-luke-getus-wrap.md`
(a platform timer bug found along the way).

## Credits

Ernest Garrison (combust) — design and direction. Built with Claude Code.
**Matthew Nielsen** — the keyframe (extrema-sampling) DSP core from
[capicola](https://github.com/heavylight-industries/capicola), used with his
permission (`THIRD_PARTY.md`). **Luke / Hermetic Modular** — the Alchemy
Lab and its SDK. **Émilie Gillet / Mutable Instruments** — the Grids drum
map. **Nick Collins** — the breakbeat-cutting grammars this one leans on.

## License

AGPL-3.0. See `LICENSE` and `THIRD_PARTY.md`.
