# Audiothurgist

**A sampling, slicing and rearranging toolkit for the Hermetic Modular
Alchemy Lab.** Load a break from the card or sample the rack into it, and one
knob travels from *the record, verbatim* to chops, rolls and bursts. Patterns hold until you ask
for new ones. Every sound is a slice of real material played from its
attack, on the grid: no grains, no time-stretch. If you don't like it,
re-slice it.

Audiothurgist is the Eurorack form of BreakFix
(Daisy Pod), rebuilt for the [Alchemy Lab v2](https://hermeticmodular.com)
(Daisy Seed 2 DFM, six pots with LED rings, three buttons, ten jacks, SD,
USB). **Status: beta (firmware 0.13.0-beta).** It works and it is being
played; the manual is in the firmware and on the page's Manual tab.

**Quick start:** flash `audiothurgist.bin` (below), put breaks on a FAT32
card, and open the page — [ernestg4.github.io/Audiothurgist](https://ernestg4.github.io/Audiothurgist/)
in Chrome or Edge — then Connect. **It powers up as a sampler**: Pads, the
pattern stopped — play the 24 pads or the J4 keys, drop your own samples on
the pads. For a beat, pick a mode (Classic · Grids · Loop) and press **▶**
(or hold **B1** on the panel); ▶/■ starts and stops it from then on.
Options → Power-up → *Start In: As saved* (then save the preset) powers up
in the preset's mode, running, instead. No module? Press **Demo** (or add `?demo=sampler` to the
page's address).

## Three roles, one engine

- **Chopper** — the knobs and buttons: slice, rearrange, dice, flow,
  history, tempo, pitch.
- **Voice** — the rack plays *it*: **J4 is the key in every map — 0 V =
  key 0 (C1)**, a semitone per key to key 23, negatives = key 0, each key
  playing what the Kit's key table says. With the Voice jack map J5 is the
  style note (reverse, half, double, ratchet, staccato, punch/choke — and
  Record; under ~70 mV is plain), J6 the gate; J7 sends a trigger per hit
  and J8 its velocity; with the Brain map J5 is the gate. Two play grammars: **Play** loops what you play in
  (unquantized by default), **Pads** is the MPC — nothing written, the beat
  is yours.
- **Brain** — it plays the rack: kick, snare and hat gates on J6/J7/J8 as
  the pattern runs; a clock on J3 locks the loop to your master.

The roles run at once; nothing needs a mode. **Sampling**: hold a key with
the Record style and it records from J1/J2 (mono or stereo) into that key;
arm Record and B3 or a J6 edge takes a whole loop into the break, with J6
edges dropping slice markers while it runs. One buffer, one slice table: a
break and a kit are the same thing, played by different rules (slice or held).

## Use it as a sampler

The 24 keys (J4, the page's pads, the Kit) are a sampler's pads as much as a
break's slices — and the module powers up as one (**Start In = Sampler**:
Pads, the pattern stopped; the page opens on the pads). Three ways to put a
sound on a pad:

1. **A break, autosliced** — load or upload a break, then **Autoslice** (Kit →
   Edit, or P5 on the panel's Edit page); in **Pads** the keys follow the
   markers. BreakFix's way: the slices follow the tempo.
2. **Record from CV** — Voice map, J5 style 7 (Record), hold the key on J4:
   the input goes into that key.
3. **Drop a file on a pad** — drag any audio file the browser can open onto a
   pad (Play) or a key cell (Kit), or *Choose samples…* on a phone. Mono or
   stereo, as the file is (no trim; a stereo one makes the kit stereo); tick
   **normalise drops** to bring each file's peak to −1 dBFS first (off by
   default, remembered by the browser). Several files fill that pad and the
   next ones.

**Each key has its own settings** (Kit tab → select a key): **gain** (−24 …
+6 dB), **tune** (±24 semitones, with cents) and **Plays** — *Follow* Options →
Key Plays, *One-shot*, *Held* (while the gate or the pad is down), *Loop* (held,
the key's own sample looping) or *Ratchet* (held, the sample again from its
start every whole, ½, ¼ or ⅛ of itself, or every 8th / 16th / 32nd at the
tempo). They ride every call through the key — CV, the pads, Play's loop —
never the pattern's steps; they are saved with the set, and an untouched key
plays exactly as before. Details: `docs/packages/keyp.md`.

**Play one sample chromatically** — the mono sampler's way: put a sound on a
key, select it in the Kit and press **Spread [key] chromatically (C1 … B2)**.
Every key then plays that sample, each a semitone from the next (the key you
spread from stays where it is; a key 7 above it plays a fifth up), with its
gain and Plays — so J4's V/oct plays a melody from one sample (0 V = C1, as
always). If other keys hold pads of their own the page asks first: spread
around them (the default — they keep their pads), over them too, or cancel. One
**Undo** puts every key back. A recording or a dropped pad stays in tune at any
tempo; a slice of the break spread this way still follows the tempo. Details:
`docs/packages/chroma.md`.

Recordings and dropped samples play at their own pitch (P4 transposes them);
the break's slices follow the tempo. The sample memory is 45 s per channel: a
break takes at most 17 s and the pads get the rest — or press **New empty
kit** (Play → Pads) for no break at all and 44.75 s of pads, in Pads mode.
Replacing, deleting or undoing a pad leaves its memory **held** (the room line
says how much); **Reclaim** beside it gives it back — the pads move down in
memory, everything plays exactly as before (sounding pads included), and if
Undo could still have brought one of those sounds back it asks first, since
those undo steps go. A drop that does not fit reclaims on its own, then lands
whole. A dropped sample lives in the module's memory; the page shows
**Unsaved kit** until you **Save set** (the break, every pad, the table and
the options, in `Audiothurgist/sets/` — only what plays: held memory is never
written), and a power-off before that loses it. To play a
beat with your kit, pick Classic or Grids (the line over the pads has both)
and press ▶ — it comes in on the beat. Details: `docs/packages/pads.md`.

## Flashing

1. Rack and power the module, panel USB-C to the computer.
2. Open Hermetic's web flasher (`/program`) in Chrome or Edge and flash
   `audiothurgist.bin` (a release build is attached to each tag; see
   *Building* to make your own).
3. FAT32 microSD with your breaks in `Audiothurgist/breaks/` — or upload
   them from the page (see *The card* below). 1 to 4 bar breaks are ideal.

If a build ever leaves the module USB-silent, tap **BOOT** during the
power-up breathing pulse to reach the Daisy bootloader, then flash.

A build that changes the Options block (this one added Start In) moves the
preset schema: the flash slots are dropped once at the first power-up, and
each slot with a backup on the card (`Audiothurgist/presets/<slot>.json`)
comes back from it — options, knobs, sample and name; its motion locks and
the brightness do not.

## The card

Everything the module reads or writes lives in one folder, `Audiothurgist/`:

| | |
|---|---|
| `Audiothurgist/breaks/*.wav` | the breaks P5 and the page's Sample list show, sorted |
| `Audiothurgist/sets/` | sets (`<name>.wav` + `<name>.json`) and the loose files' edits (`<wav>.slices.json`) |
| `Audiothurgist/presets/` | preset backups (`<slot>.json`) |

The folders are made the first time something is written. **What loads:** WAV,
PCM 16- or 24-bit, mono or stereo, 8 kHz or more, at least ¼ s; the first 17 s
are used (the rest of the 45 s buffer is kept for takes into keys and samples
dropped onto pads); any rate is converted to 48 kHz on load (a 192 kHz file:
its first 8.5 s). **Uploads** from the page (Kit → Card, or *Add
breaks…* beside the Sample list) take anything the browser can decode — WAV of
any kind including float, AIFF, MP3, FLAC, OGG — and send it as a 48 kHz PCM16
WAV, stereo kept, cut to 17 s; the module writes it into `breaks/` while the
music plays.

**Cards from older firmware** kept WAVs in the root and `sets/`, `presets/`
beside them. This firmware reads only `Audiothurgist/` and moves nothing for
you — files in the root may belong to another module. The page says so and
offers **Copy them in**: the root's WAVs, `sets/` and `presets/` are *copied*
into the folder (anything already there is skipped; nothing is moved or
deleted). Do it before relying on preset backups. Details:
`docs/packages/card.md`.

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
| Key Plays | **One-shot** · Held (until the gate falls) — a key's own Plays (Kit) overrides it |
| Sampling | **Mono (J1+J2)** · Stereo |
| Record Source | **Inputs** · Output (resample the module) |
| Record | **Off** · Arm (B3 tap or J6 starts a loop take) |
| Quantize | **Off (as played)** · 16ths · 8ths — Play's looper |
| Tap Allowance | 0 · 20 · **40** · 60 · 80 ms — a tap-while-playing mark lands this far back (slice editing) |
| Autosave | Off · **On (2 s after an edit)** — the slice table and key map to the set's JSON, or a `sets/<file>.slices.json` sidecar beside a loose WAV |
| Start In | **Sampler (Pads, stopped)** · As saved (the preset's mode, running) — read from the boot preset at power-up; a set load leaves it alone |

## The web app

`web/` is a single-page WebSerial instrument view: the bar ring and loop
strip, per-slice waveform envelopes, an oscilloscope, every button and
knob function for the current page and grammar, locks, options, presets,
the manual, a diagnostics line, a **Kit** tab: the 24 keys, each
recording's waveform with onsets and boundaries, record/clear per key, the
looper's events, set save/load, and the **Card** panel (upload breaks,
copy an old card in) — and a **Scope** tab: out L/R and
in L/R at full rate plus the six jacks J3–J8 as DC-coupled CV traces
(LFOs, sequences, gates — ours read back too), ten colours, time zoom per
tier, gain, a trigger on any channel, peak decimation, afterglow, at up to
30 frames/s (`docs/packages/scope.md`). Serve it from a secure context:

```sh
cd web && python3 -m http.server 8080     # then http://localhost:8080 in Chrome/Edge
```

or publish it: `./web/publish-pages.sh && git push --force-with-lease github pages` →
https://ernestg4.github.io/Audiothurgist/. **Phones and tablets:** on
Chrome for Android the page uses Google's Web Serial polyfill over WebUSB
(vendored, Apache-2.0), so an OTG cable to the module's front USB-C is
enough; Chrome's *Install app* puts it on the home screen. iOS has no
USB API in the browser. Details, and how to test a phone against a page
served from the PC, in `web/README.md`.

Close `/program` first (one page holds the port). Press **Demo** to try it
without the module. `node web/selftest.mjs` checks the protocol against
the SDK's golden vectors. **The page and the firmware go together**: flash
the `.bin` built from the same commit as the page; a page that finds another
build on the port says so in one line and does not connect.

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
