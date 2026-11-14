# Audiothurgist

**A sampling, slicing and rearranging toolkit for the Hermetic Modular
Alchemy Lab.** Play it as a mono or stereo sampler with 24 pads, or load a
break and let one knob carry it from *the record, verbatim* to chops, rolls
and bursts. Every sound is real material played from its attack, on the
grid: no grains, no time-stretch.

Audiothurgist is the Eurorack form of BreakFix (Daisy Pod), rebuilt for the
[Alchemy Lab v2](https://hermeticmodular.com) (Daisy Seed 2 DFM, six pots
with LED rings, three buttons, ten jacks, microSD, USB). Firmware
**0.13.0-beta**. The manual is built into the firmware and shown on the
page's Manual tab.

**The page:** **[ernestg4.github.io/Audiothurgist](https://ernestg4.github.io/Audiothurgist/)**
(Chrome or Edge on a computer, Chrome on Android).

## Quick start

1. Flash the firmware (*Flashing*, below).
2. Put breaks and samples on a FAT32 microSD card in `Audiothurgist/breaks/`,
   or upload them from the page later.
3. Open the page, close Hermetic's `/program` if it's open, and press
   **Connect**.

**It powers up as a sampler**: Pads mode, the pattern stopped. Play the 24
pads on the page or the keys from J4, and drop your own samples onto the
pads. For a beat, pick a mode (**Classic**, **Grids** or **Loop**) and press
**▶**, or hold **B1** on the panel. ▶ or B1 while still in Pads brings back
the boot preset's mode, so the panel alone gets you from the sampler to a
beat. To power up in your preset's mode, running, set Options → Power-up →
*Start In: As saved* and save the preset.

No module at hand? Press **Demo** on the page.

## Play it as a sampler

The 24 keys are a sampler's pads. J4's CV, the page's pads and the Kit tab
all play them, and J4 is the key in every jack map: 0 V is key 0 (C1), a
semitone per key up to key 23.

**Put a sound on a pad** in any of three ways:

1. **Drop a file on a pad.** Drag any audio file the browser can open onto a
   pad (Play tab) or a key cell (Kit tab), or use *Choose samples…* on a
   phone. It keeps its channels: a stereo file makes the kit stereo. Tick
   **normalise drops** to bring each file's peak to −1 dBFS first. Several
   files fill that pad and the ones after it.
2. **Record from CV.** With the Voice jack map, send J5 style 7 (Record) and
   hold the key on J4: the inputs record into that key for as long as the
   gate is up.
3. **Slice a break.** Load or upload a break, then **Autoslice** (Kit → Edit,
   or P5 on the panel's Edit page). In Pads the keys follow the markers, in
   time order. A break's slices follow the tempo; recordings and dropped
   samples keep their own pitch, and P4 transposes them.

**Shape each key** from the Kit tab (select a key), or from the **⋯** in a
pad's corner on the Play tab, which opens the same settings in a sheet while
the pads keep playing:

- **Gain** −24 to +6 dB, **Tune** ±24 semitones with cents.
- **Plays**: *Follow* (Options → Key Plays), *One-shot*, *Held* (while the
  gate or pad is down), *Loop* (held, the sample looping, forward or
  reversed) or *Ratchet* (held, the sample again every whole, ½, ¼ or ⅛ of
  itself, or every 8th, 16th or 32nd at the tempo).
- **Envelope**: Attack up to 2 s; Decay down to a Sustain level (0 dB, lower,
  or off); Release up to 4 s for held keys. Shorten a one-shot with Decay and
  Sustain off.
- **Choke group** 1–4: a key struck in a group cuts the other keys of that
  group. Put an open and a closed hat in group 1.

A key's settings apply to everything played through it (CV, pads, the Loop
mode's replays), never to a pattern's steps, and they're saved with the set.
Each edit, including a whole slider drag, is one Undo step.

**Play one sample melodically:** select a key in the Kit and press
**Spread … chromatically**. Every key plays that sample a semitone apart, so
a V/oct sequence on J4 plays a melody from it. If other keys hold pads of
their own, the page asks whether to spread around them or over them. One
Undo puts every key back.

**Memory:** 45 s per channel. A break takes at most 17 s and the pads get the
rest, or press **New empty kit** (Play tab → Pads) for no break and 44.75 s of
pads. Replacing or deleting a pad leaves its memory *held* until you press
**Reclaim** (beside the memory meter); the pads move down and keep playing
throughout. A drop that doesn't fit reclaims by itself first.

**Keep your kit:** dropped samples live in the module's memory, and the page
shows *Unsaved kit* until you press **Save set**. A set holds the break,
every pad, the key table, each key's settings, the options and the loop, in
`Audiothurgist/sets/`.

## Play it as a break machine

Load a break (P5 to pick, hold B3 to load, or the page's Sample list), pick a
mode and press ▶.

- **Classic**, **Collins**, **Grids**, **Euclid**, **Camp**: pattern
  grammars that rearrange the break's slices. P1 (Chop) runs from the record
  verbatim (full right) through traded beats, chops and rolls to the cluster
  zone (full left). B2 dices a new arrangement, B2 held flows a new bar every
  loop, B3 reslices, and P3 steps back through the last sixteen patterns.
- **Loop**: what you play in (pads or J4) is looped. With Options → Quantize
  *Off* (the default) every hit replays exactly when you played it, held
  notes with their length, overdubbing pass after pass. The Play tab draws
  the loop live: each hit at its place in the bar in its drum colour, held
  notes as bars, the newest pass bright. B2 undoes the newest pass and B3
  clears the loop. *16ths* punches the bar step by step instead.
- **Pads**: nothing is written; the beat is yours.

**Brain:** while a pattern runs, the module sends kick, snare and hat gates on
J6/J7/J8. A clock on J3 locks the loop to your master, and P2 then becomes a
divider/multiplier. **Voice:** with the Voice jack map the rack plays the
module: J4 key, J5 style (reverse, half, double, ratchet, staccato, punch,
record), J6 gate in, J7 a trigger per hit and J8 its velocity out.

**Sample the rack into the break:** Options → Record = Arm, then B3 or a J6
edge takes a whole loop from J1/J2 (or the module's own output), with J6 edges
dropping slice markers while it runs.

## Flashing

1. Rack and power the module and connect its panel USB-C to the computer.
2. Open Hermetic's web flasher (`/program`) in Chrome or Edge and flash the
   `.bin` from [Releases](https://github.com/ErnestG4/Audiothurgist/releases),
   or `build/audiothurgist.bin` from *Building*. The published page matches
   the latest release; if the module runs a different build, the page tells
   you which to flash.
3. Close `/program` (one page holds the port at a time), then open the page.

If a build ever leaves the module silent on USB, tap **BOOT** during the
power-up breathing pulse to reach the Daisy bootloader, then flash.

A build that changes the options drops the preset slots once at its first
power-up. Each slot with a backup on the card
(`Audiothurgist/presets/<slot>.json`) comes back with its options, knobs,
sample and name; re-make its motion locks and save it again.

## The card

Everything lives in one folder, `Audiothurgist/`:

| | |
|---|---|
| `Audiothurgist/breaks/*.wav` | the breaks P5 and the page's Sample list show, sorted |
| `Audiothurgist/sets/` | sets (`<name>.wav` + `<name>.json`) and loose files' edits (`<wav>.slices.json`) |
| `Audiothurgist/presets/` | preset backups (`<slot>.json`) |

The folders are made the first time something is written.

- **What loads:** WAV, PCM 16- or 24-bit, mono or stereo, 8 kHz or more, at
  least ¼ s. The first 17 s are used, converted to 48 kHz.
- **Uploads** from the page (Kit → Card, or *Add breaks…* beside the Sample
  list) take anything the browser can decode (WAV including float, AIFF, MP3,
  FLAC, OGG) and write a 48 kHz WAV into `breaks/` while the music plays.
- **A card from older firmware** (WAVs in the root): the page offers **Copy
  them in**, which copies the root's WAVs, `sets/` and `presets/` into
  `Audiothurgist/`. Nothing is moved or deleted.

## Controls

| | Perform page | Brain page (B1 tap) |
|---|---|---|
| P1 | **Chop** — verbatim (right) → beats trade → chops → rolls → cluster zone (left) | Map X (Grids) · bells (Camp) |
| P2 | **Tempo** 60–200 BPM, varispeed, snaps to the file's own tempo · while clocked: ÷4 ÷2 ×1 ×2 ×4 | Map Y (Grids) |
| P3 | **History** — the last sixteen patterns | Kick density |
| P4 | **Pitch** ±12 semitones, onsets stay on the grid | Snare density |
| P5 | **Sample** — highlight a file on the card | Hat density |
| P6 | **Volume** (reverb rides the cluster zone) | Rotation (Euclid) |

**B1** tap: Perform ↔ Brain · **held 600 ms and let go** (no pot, no other
button; B1 turns white when it will): **start / stop** — the pattern stops,
the clock runs on, keys still play, B1 breathes while stopped · held 300 ms + turn a pot: record that pot's
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
| P5 | **Autoslice** count 0–32: grey markers on onsets drawn at random, weighted by loudness — up adds the next, down drops the last, 0 and back re-draws; picks up at the count there is; a sweep is one undo; the selection's key cells pulse on top |
| B1 | tap: leave (commit) |
| B2 | tap: undo the last edit · hold: revert the slice to the computed grid (a marker: delete it) |
| B3 | tap while something sounds: a marker at the playhead, onset-snapped (Options → Tap Allowance); within 10 ms of a marker it selects that one · hold: reslice |

Jacks, **Brain map**: J1/J2 audio in · **J3 clock in** · **J4 key · J5
gate** · **J6/J7/J8 kick/snare/hat gates out** · J9/J10 audio out. **Voice
map** (Options → Jacks, or automatically with Play/Pads): J4 key · J5 style
· **J6 gate in** · J7 hit trigger out · J8 velocity out; the clock stays.
In **Pads** the keys follow the markers (time order on keys 0, 1, 2…).

## Options (in the web app or Hermetic's `/program`)

| Option | Choices (default in bold) |
|---|---|
| Classifier | **Hockman 3-band** · Legacy 120/900 (hi/lo) · Two-band · Density (keyframes) · Adaptive (onset, clustered) |
| Clock In | **24** · 4 · 2 · 1 · 48 · 96 · 16 · 8 PPQN |
| Voice | **Choke** · Punch (writes the bar) |
| Gate Length | **6 ms** · 20 ms · 50 ms · Hold to next hit |
| Gate Level | **Fixed 5 V** · Velocity (accents to the ceiling) |
| Grammar | **Classic** · Collins · Grids · Euclid · Camp · Play (the page's **Loop**) · Pads (free) |
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
| Key Plays | **One-shot** · Held (until the gate falls) — a key's own Plays (Kit) overrides it |
| Sampling | **Mono (J1+J2)** · Stereo |
| Record Source | **Inputs** · Output (resample the module) |
| Record | **Off** · Arm (B3 tap or J6 starts a loop take) |
| Quantize | **Off (as played)** · 16ths (punch the bar) · 8ths — the Loop mode's looper |
| Tap Allowance | 0 · 20 · **40** · 60 · 80 ms — a tap-while-playing mark lands this far back (slice editing) |
| Autosave | Off · **On (2 s after an edit)** — the slice table and key map to the set's JSON, or a `sets/<file>.slices.json` sidecar beside a loose WAV |
| Start In | **Sampler (Pads, stopped)** · As saved (the preset's mode, running) — read from the boot preset at power-up; a set load leaves it alone |

## The web app

The page is the module's full instrument view over Web Serial:

- **Play:** the transport, the modes, the 24 pads, the bar ring and loop
  strip, the knobs and buttons for the current page and mode.
- **Kit:** the keys and their settings, each sample's waveform with its
  markers and onsets, the slice editor with Undo/Redo, sets, the memory
  meter with Reclaim, and the Card panel.
- **Scope:** out L/R and in L/R at full rate plus the six CV jacks J3–J8 as
  DC-coupled traces, with a trigger, zoom and gain.
- **Options**, **Manual**, presets and locks.

**Phones and tablets:** Chrome on Android reaches the module through an OTG
cable to its front USB-C (Google's Web Serial polyfill over WebUSB is
built in), and *Install app* puts the page on the home screen. iOS browsers
have no USB access.

**The page and the firmware go together:** the page checks the firmware's
protocol version on Connect and names the build to flash if they differ.

To serve it yourself from a checkout (a secure context is required):

```sh
cd web && python3 -m http.server 8080     # then http://localhost:8080 in Chrome or Edge
```

To publish it: `./web/publish-pages.sh && git push --force-with-lease github pages`.
More in `web/README.md`.

## Building

Siblings expected next to this repo:
[`alchemy-sdk`](https://github.com/hermetic-modular/alchemy-sdk) v0.13.0
with its vendored libDaisy, plus two small local patches (the 480 MHz boost in
`AlchemyLabV2::Init`, and the USB statics moved out of `.data`; both in
`docs/sdk-quirks.md`), and `DaisySP/DaisySP-LGPL`. The Makefile uses
`../alchemy-sdk-0.13` when it exists, otherwise `../alchemy-sdk`. Then:

```sh
git submodule update --init          # vendor/capicola (not --recursive)
make                                 # build/audiothurgist.bin
tests/run.sh                         # host test suites (g++, ASan/UBSan)
node web/selftest.mjs                # the page against a fake module
make sdramcheck                      # no static initialiser touches SDRAM
```

The pure engine lives in `core/` (no hardware, no allocation), the Alchemy
Lab shell in `src/audiothurgist.cpp`, the page in `web/`. `tests/` covers
the engine, grammars, clock, bars, classifier, scope, panel, keys, sets and
persistence, the card, pads, per-key settings and Reclaim. The page test
drives the real page against `web/demo.js` with clicks, drags and drops.
Platform gotchas are in `docs/sdk-quirks.md`. The module's HELLO carries the
git hash it was built from.

## Documents

- `docs/audiothurgist-spec.md`: the design.
- `docs/packages/*.md`: each subsystem. For the sampler, start with
  `pads.md`, `keyp.md`, `chroma.md` and `sample-looper.md`.
- `docs/hardware-pass-beta.md`: the bench checklist.
- `docs/colors.md`: the page's colour-blind-safe palette.
- `docs/lit-review-2026-09-05.md`: the literature behind the choices.

## Credits

Ernest Garrison (combust): design and direction. Built with Claude Code.
**Matthew Nielsen**: the keyframe (extrema-sampling) DSP core from
[capicola](https://github.com/heavylight-industries/capicola), used with his
permission (`THIRD_PARTY.md`). **Luke / Hermetic Modular**: the Alchemy Lab
and its SDK. **Émilie Gillet / Mutable Instruments**: the Grids drum map.
**Nick Collins**: the breakbeat-cutting grammars this one leans on.

## License

AGPL-3.0. See `LICENSE` and `THIRD_PARTY.md`.
