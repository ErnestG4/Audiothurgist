# Audiothurgist

**A sampling, slicing and rearranging toolkit for the Hermetic Modular
Alchemy Lab.** Load a break from the card or sample the rack into it, and one
knob travels from *the record, verbatim* to chops, rolls and bursts. Patterns hold until you ask
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
- **Voice** — the rack plays *it*: with the Voice jack map, J4 is the slice
  note, J5 the style note (reverse, half, double, ratchet, staccato,
  punch/choke — and Record), J6 the gate; J7 sends a trigger per hit and J8
  its velocity. Two play grammars: **Play** loops what you play in
  (unquantized by default), **Pads** is the MPC — nothing written, the beat
  is yours.
- **Brain** — it plays the rack: kick, snare and hat gates on J6/J7/J8 as
  the pattern runs; a clock on J3 locks the loop to your master.

The roles run at once; nothing needs a mode. **Sampling**: hold a key with
the Record style and it records from J1/J2 (mono or stereo) into that key;
arm Record and B3 or a J6 edge takes a whole loop into the break, with J6
edges dropping slice markers while it runs. One buffer, one slice table: a
break and a kit are the same thing, played by different rules (slice or held).

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
**B1 held + B3 tap**: the **Edit page** (B1 amber; B1 tap leaves and commits;
the Perform/Brain page underneath is untouched) — see the table below.

| | Edit page (B1 held + B3 tap) |
|---|---|
| P1 | **Select** a slice: grid, then markers; the bar ring shows its bar with its segment amber, pulsing as it plays; a Voice-map note selects its key's entry |
| P2 | **Start nudge** ±50 ms — inert until swept through centre (a dim fan shows where it would land), then live in 1 ms steps; every move re-plays the first 250 ms |
| P3 | **End nudge** ±50 ms, the same; every move re-plays the last 250 ms |
| P4 | Pitch, as ever |
| P5 | The kit map, with the selection's key cells pulsing |
| B1 | tap: leave (commit) |
| B2 | tap: undo the last edit · hold: revert the slice to the computed grid (a marker: delete it) |
| B3 | tap while the break plays: a marker at the playhead, onset-snapped (Options → Tap Allowance) · hold: reslice |

Jacks, **Brain map**: J1/J2 audio in · **J3 clock in** · **J4 v/oct · J5
gate** · **J6/J7/J8 kick/snare/hat gates out** · J9/J10 audio out. **Voice
map** (Options → Jacks, or automatically with Play/Pads): J4 slice · J5 style
· **J6 gate in** · J7 hit trigger out · J8 velocity out; the clock stays.

## Options (in the web app or Hermetic's `/program`)

| Option | Choices (default in bold) |
|---|---|
| Classifier | **Hockman 3-band** · Legacy 120/900 (hi/lo) · Two-band · Density (keyframes) · Adaptive (onset, clustered) |
| Clock In | **24** · 4 · 2 · 1 · 48 · 96 · 16 · 8 PPQN |
| Voice | **Choke** · Punch (writes the bar) |
| Gate Length | **6 ms** · 20 ms · 50 ms · Hold to next hit |
| Gate Level | **Fixed 5 V** · Velocity (accents to the ceiling) |
| Grammar | **Classic** · Collins · Grids · Euclid · Camp · Play (looped) · Pads (free) |
| Gate Fires | Every step · **Onsets only (once per hit)** |
| Bars | **Auto (backbeat)** · 1 · 2 · 4 · 8 — fixes a break playing at half or double speed |
| Rests | **Continue the break** · Gate (silence) |
| Gate Lanes | **Dominant class** · Multi-label |
| Fades | Fixed · **Adaptive (keyframe density)** |
| Slice Tail | Gate · Loop forward · Ping-pong · **Zoom** (pitch reads on, never through an attack) |
| Swaps | Class only · **Class + salience tier** |
| Flow Déjà Vu | Off · **Some** · Lots |
| Swing | Straight · **Auto (the material's swing)** |
| Onsets | Spectral flux · **Log-filtered flux** |
| Jacks | **Follow grammar** · Brain · Voice |
| Key Plays | **One-shot** · Held (until the gate falls) |
| Sampling | **Mono (J1+J2)** · Stereo |
| Record Source | **Inputs** · Output (resample the module) |
| Record | **Off** · Arm (B3 tap or J6 starts a loop take) |
| Quantize | **Off (as played)** · 16ths · 8ths — Play's looper |
| Tap Allowance | 0 · 20 · **40** · 60 · 80 ms — a tap-while-playing mark lands this far back (slice editing) |
| Autosave | Off · **On (2 s after an edit)** — the slice table and key map to the set's JSON, or a `sets/<file>.slices.json` sidecar beside a loose WAV |

## The web app

`web/` is a single-page WebSerial instrument view: the bar ring and loop
strip, per-slice waveform envelopes, an oscilloscope, every button and
knob function for the current page and grammar, locks, options, presets,
the manual, a diagnostics line, a **Kit** tab: the 24 keys, each
recording's waveform with onsets and boundaries, record/clear per key, the
looper's events, and set save/load — and a **Scope** tab: out L/R and
in L/R at full rate plus the six jacks J3–J8 as DC-coupled CV traces
(LFOs, sequences, gates — ours read back too), ten colours, time zoom per
tier, gain, a trigger on any channel, peak decimation, afterglow, at up to
30 frames/s (`docs/packages/scope.md`). Serve it from a secure context:

```sh
cd web && python3 -m http.server 8080     # then http://localhost:8080 in Chrome/Edge
```

or publish it: `./web/publish-pages.sh && git push -f origin pages` →
https://combust.codeberg.page/Audiothurgist/. **Phones and tablets:** on
Chrome for Android the page uses Google's Web Serial polyfill over WebUSB
(vendored, Apache-2.0), so an OTG cable to the module's front USB-C is
enough; Chrome's *Install app* puts it on the home screen. iOS has no
USB API in the browser. Details, and how to test a phone against a page
served from the PC, in `web/README.md`.

Close `/program` first (one page holds the port). Press **Demo** to try it
without the module. `node web/selftest.mjs` checks the protocol against
the SDK's golden vectors.

Platform gotchas live in `docs/sdk-quirks.md`; `make sdramcheck` reads the
ELF for static initialisers touching SDRAM; the module's HELLO carries the
git hash it was built from, so a flashed binary can identify itself.

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
an engine suite, a clock suite and a scope suite under ASan/UBSan, plus
corpus tools.

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
