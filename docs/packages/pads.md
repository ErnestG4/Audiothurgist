# PADS package — a sample dropped onto a pad, a kit with no break, 45 s of memory (2026-10-25, branch `beta-pads`)

*Combust's goal, verbatim: "We need to enable sample drop ins for an arbitrary
pad set as well. So paths to samples are: drum loop / sample drop in and auto
slice to pads (breakfix), record into a pad from CV, or drag and drop onto
pads. We need to be able to be a normal mono / stereo sampler as much as a
break slicer and rearranger." His calls: grow the sample memory to about 45 s
per channel (the 17 s break cap stays, pads get the rest); a dropped sample
lives in the module's memory like a take into a key — nothing is written to
the card until a set is saved, and the page says so plainly.*

Owns `core/bb_pads.h` (new), `EditOp::Record` / `Engine::AdoptR` /
`Engine::OwnRate` in `core/bb_engine.h` + `core/bb_edit.h` + `core/bb_undo.h`,
the same-rate path of `core/wav_loader.h`, the `/* PADS */` lines of
`src/audiothurgist.cpp`, `tests/pads_check.cpp` (new), the PADS block of
`web/hostlink.js`, the pads controls of `web/index.html`, the fake's pads in
`web/demo.js`, and selftest §7d / §9e. **Nothing here has run on hardware**;
the checklist is `docs/hardware-pass-beta.md` §10c.

## The three paths to a sound on a pad

| | how | what the pad plays |
|---|---|---|
| **A break, autosliced** (BreakFix) | load or upload a break, AUTOSLICE (Kit → Edit, or P5 on the Edit page); in **Pads** the keys follow the markers | a span of the break, at the tempo's varispeed |
| **Record from CV** | Voice map, J5 style 7, hold the key on J4 | the take, trimmed and normalised, at its own pitch |
| **Drop a file** (new) | drag an audio file onto a pad (Play) or a key cell (Kit), or *Choose samples…* / *Load into …* on a phone | the file as it is — mono or stereo, no trim, no normalise — at its own pitch |

**New empty kit** clears the break, so all the memory is for pads, and puts
the module in **Pads**: a plain 24-pad sampler.

## Memory

| | before (548a842) | now |
|---|---|---|
| planes `gBufA` / `gBufB` / `gBufAR` / `gBufBR` | 4 × 25 s × 48 kHz floats = 4 × 4.80 MB | 4 × **45 s** = 4 × 8.64 MB |
| decode staging `gStage` / `gStageR` | 2 × 25 s × 96 kHz = 2 × 9.60 MB | 2 × **17 s × 96 kHz** = 2 × 6.53 MB |
| card upload `gUpBuf` | 3.27 MB | 3.27 MB (unchanged) |
| **SDRAM** | 42 837 064 B, **63.8 %** | 52 053 064 B, **77.6 %** |
| the break (`kLoadSec`) | ≤ 17 s | ≤ 17 s |
| room for keys and pads (`kRecSec`) | 8 s | **28 s** (44.75 s in an empty kit) |

**Why 45 s, and how it fits.** The staging only ever holds a *resampled*
load's native-rate decode: `bbwav::Load` now decodes a file that is already
at 48 kHz straight into the output planes (the resample is the identity
there), so a set — always 48 kHz — may fill the whole 45 s plane through no
staging at all, and the staging is sized for the longest resampled load the
firmware makes: a 17 s break at 96 kHz. That freed 6.1 MB, which pays for most
of the planes' growth. 45 s is Combust's figure; at 77.6 % it leaves 14.4 MB
of headroom (the 85 % ceiling would have allowed ~51 s), kept for a later
need — a bigger drop buffer or a longer break, say. `tools/sdramcheck.sh`
passes; every SDRAM object is still a POD.

**Every assumption tied to the old size, traced:** set loads (the WAV — break
plus recordings — up to `kMaxSec` into `kBufMax`, now through the same-rate
path; checked at 45 s in `pads_check`); loop takes (`kLoadMax`, unchanged);
wave fetches (`GET_WAVE` bounded by freePos, u32 positions); GET_TABLE's
fields (u32 start / len); the set JSON (plain integers); the tail's freePos /
bufCap (u32); the page (reads bufCap from the tail; the fake's `BUF_CAP` is
45 s now); the manual and demo.js text ("28 s of the 45 s buffer");
`static_assert`s on `kLoadSec`, the staging and int positions. *One
regression*: a break at **192 kHz** now loads its first 8.5 s (the staging is
17 s at 96 kHz), where it got 12.5 s before; 96 kHz and below still get 17 s,
and everything the page uploads is 48 kHz.

## Protocol — `0x68 PADS`, capability `"pads": 1`

A new command beside `0x67 CARD` (whose machinery it reuses: `bb::UploadRx`,
the `max_body − 24` chunks, the 5 s silence drop, `HL.serial`), because a drop
never touches the card. Every reply starts with a status: 0 OK · 2 BAD_ARGS ·
3 BAD_STATE · 6 TOO_LARGE.

```
op 0 BEGIN    ← u8 key (0..23), u8 ch (1 | 2), u32 frames (48 kHz)
              → status, u32 room (frames free past freePos now), u32 bufCap
op 1 DATA     ← u32 total (frames × ch × 2 bytes), u32 offset, PCM16 LE bytes (interleaved L R)
              → status, u32 have, u8 done (0 more · 1 complete: the commit is queued)
op 2 ABORT    → status (the drop forgotten; nothing published)
op 3 NEW_KIT  → status (the main loop does it; the tail's pads seq says when)
```

- **The bytes** are raw PCM16, no header: the page decodes (a 48 kHz
  `OfflineAudioContext`, as the card upload), keeps two channels at most and
  cuts to the room it read from the tail just before.
- **Refusals at BEGIN** (and NEW_KIT): BAD_ARGS for a key past 23, a channel
  count other than 1 or 2, fewer than 480 frames (10 ms, `kRecMin`);
  TOO_LARGE for more frames than the room — the reply carries the room
  either way; BAD_STATE while a set op runs or waits, a load is requested or
  applying, a take is armed / running / pending (J5 style 7 standing, Record
  armed), a card job runs (receiving, writing, copying, queued), another
  drop commits, or there is no material. A BEGIN while a drop arrives
  replaces it; NEW_KIT is refused while one arrives.
- **The reverse**: while a drop is open (receiving or committing) a card
  upload, a copy, a rescan, ACTION 6 (load), 9 (arm a take), 14 / 15 (set
  save / load) and New empty kit answer BAD_STATE; the panel's B3-hold load,
  a J5 style-7 take and a loop take do not start; a dropdown load or a
  Sampling change wait for it. Edits stay open while the bytes arrive (none
  of them moves freePos) and are refused during the commit.
- **Sequencing** is `bb::UploadRx`'s: in order from zero, the size fixed at
  the begin; a wrong offset, a changed total or bytes past the end is
  BAD_ARGS and drops the drop.

### State tail, appended after byte 292 (the page gates on length ≥ 315)

```
293 u8 padState (0 idle · 1 receiving · 2 committing · 3 clearing: New empty kit)   294 u8 progress 0..255
295 u8 result (0 ok · 1 no room · 2 busy · 3 bad args · 4 the table is full · 5 timed out · 6 aborted
              · 7 the planes moved under it)
296 u8 seq (bumps per finished op)   297 u8 key (the drop's; 255 none)
298 u8 flags (b0 the kit holds audio the card does not — a power-off loses it · b1 an empty kit: no break
              · b2 the last drop made the kit stereo, Sampling → Stereo)
299 u32 free   (frames past freePos)
303 u32 total  (the pads' share: bufCap − bufLen)
307 u32 held   (frames in [bufLen, freePos) no live entry covers: replaced, cleared, undone recordings)
311 u32 pads   (bit k = key k holds a recording)                                         — ends at 315
```

## Where the bytes go, and the thread rules

- **Received straight into the L plane past freePos**, not into `gUpBuf`:
  a stereo frame is 4 bytes and a float is 4, a mono sample 2, so the bytes
  always fit the floats' room, a drop can be as long as the room (44.75 s in
  an empty kit, where `gUpBuf` would have stopped at 17 s of stereo) and the
  card upload keeps its buffer. Nothing reads there: every table entry ends
  at or before freePos, a held voice reads to its own entry's end, the
  audition and GET_WAVE stop at freePos, and the one writer past freePos — a
  take into a key — cannot be armed while a drop is open. Each DATA is a
  bounds check and a `memcpy` in the HostLink handler, nothing else.
- **The commit is the main loop's** (`ServicePads`, after any swap):
  `bb::PcmToPlanes` converts in place — stereo forwards (frame i's four bytes
  are float i's), mono backwards (float i covers samples 2i and 2i + 1, which
  a backward walk has already read); a stereo drop onto a mono kit first
  copies L into R over `[0, freePos)` and asks the audio thread to adopt the R
  plane at a block's start (`Engine::AdoptR`: every voice's R bridge state
  takes its L twin's, so nothing sounding steps); then the first second is
  analysed (class, salience, density, hit — a long pad would otherwise hold
  the density lens for 100+ ms without a yield) and `EditOp::Record` goes
  through the edit mailbox: the audio thread publishes the entry and moves
  freePos, in the same block. The commit checks first that the planes and
  freePos are the ones the drop began on (`kPadMoved` otherwise).
- **The control loop never stalls**: every long loop yields (`bb::Yield`
  every 4096 frames), and while the commit runs `KeepClock` runs the whole
  control loop at 1 kHz (`gPadTicking`, set only inside the commit), so the
  jacks' keys, the panel and HostLink carry on; a set op's 25 Hz rule is
  unchanged.

## Choices

- **No trim, no normalise.** A one-shot from a sample pack is already cut and
  levelled, and a kit's relative levels are the point (a soft hat must stay
  softer than the kick). Takes still trim and normalise; loads still
  normalise. The analysis runs on the first second.
- **Recordings play at their own rate.** A key call on an entry that starts
  at or past the material's end — a take or a drop — plays at pitch (P4) ×
  the style's multiplier, never the tempo's varispeed (`Engine::OwnRate`):
  a sampler's pads keep their pitch whatever the clock does. The break's
  slices and the markers inside it follow the tempo as before; a recording
  punched into a step of the bar keeps the step's varispeed. This changes
  path 2: a take into a key no longer repitches when P2 or the clock moves
  off the break's native tempo. `Engine::recOwnRate = false` is the v0.13
  rule.
- **Stereo**: a stereo drop makes the kit stereo — the module copies the L
  plane into R (so everything already there sounds the same on both sides),
  adopts the R plane and sets **Options → Sampling = Stereo** (loads and
  takes stay stereo from then on); the page's line says so. A mono drop onto
  a stereo kit is written to both planes. Nothing is folded behind your back.
- **One undo step per drop** (`EditOp::Record` is journaled like an edit):
  Undo puts the key's previous recording (or its grid slice) back, Redo the
  drop. freePos never moves back, so the region a redo needs is still there.
- **Replacing a pad** reuses the key's recording slot in place (CommitSlice's
  rule). The old region is **not reclaimed** — not by a set save (the set's
  WAV is `[0, freePos)` whole, held regions and all) and not by loading it
  back; only new material starts the planes over (a break loaded, a loop
  take, New empty kit). The room readout is honest about it: the tail's
  `held` is that memory and the page shows "pads: 12.30 s of 28.00 s free ·
  2.10 s held by replaced pads", with the reason in its title and the Kit's
  Buffer section.
- **New empty kit** is a load of a silent ¼ s stub (`kPadStubLen`, 16 grid
  slices of class "other" — no gate ever fires from it — the shortest
  material the loader takes back, so a pads-only set reloads): the table,
  the looper and the undo history start over exactly as at a load, the
  grammar goes to Pads (the jacks follow it to the Voice map), the clock keeps
  its tempo (a stub has none: `gNoBreak` skips the native-tempo reset, and a
  set whose material is exactly the stub's length of digital silence is
  recognised as an empty kit on load). The page asks first, as it does for
  Clear markers, and says when unsaved sounds would be lost.
- **The unsaved note** is the tail's flag: the material or a live entry
  reaches past what the card holds (a set's WAV length, a loose break's
  length, nothing for a take; the empty kit's stub counts as on the card).
  A set save clears it; the next drop or take raises it again.

## The page

Play → Pads gains a row: the room meter, *into [key] · Choose samples…* (the
phone path), **New empty kit**; the warn-coloured **Unsaved kit** note with
**Save set…** (saves at once when the Kit's Set name is filled, else goes
there and asks for a name). Each pad is a drop target: dragging a file over it
outlines it, a drop shows "↑ 40 %", "placing…", then "✓ 0.50 s" (or
"✕ not loaded") along with a fill on the pad's foot, and the line under the
pads says what the file became ("kick.wav → E1: mono, 0.50 s — the first
1.00 s of 3.0 s, all the room left"). Several files fill the pad and the ones
after it; files past B2 are said left over. The Kit's key cells take drops
too, and **⤓ Load into …** picks a file for the selected key. A recording's pad
shows its length; an empty kit's empty pads say "empty".

Every card and pad job goes through one queue (`cardRun`), so a drop and a
card upload never interleave. The waits ask for the state themselves with their
own request key (a background poll already in flight would hand back a room
from before the change) and give up only when the module's state and progress
stand still — the 548a842 lesson.

## Tests

- `tests/pads_check.cpp` (**132**, ASan/UBSan, in `tests/run.sh`): the
  receiver as the shell drives it (BEGIN's check, in order into the plane past
  freePos, nothing below touched); PcmToPlanes bit-exact at 1 … 9001 frames,
  mono / stereo / mono into a stereo kit / stereo folded; EditOp::Record (a new
  pad, a replaced pad, undo / redo, freePos never back, the refusals, Full);
  keys-from-markers and a re-apply keep a pad; the room (union of live
  spans, a split counted once, markers excluded); OwnRate (1.0 at 150 BPM,
  the grid ×1.25, P4 still transposes, recOwnRate = false); AdoptR under a
  sounding voice (L as the mono twin, R = L); KitUnsaved and the stub; the
  loader's same-rate path (3 s through a 1000-sample staging, bit-exact,
  44.1 kHz still capped by the staging); set round trips of a **pads-only
  kit** (a stereo pad turns it stereo, a replaced pad held — the held region
  survives the round trip: not reclaimed) and of a **17 s break plus pads
  filling the 45 s plane to the sample**, saved to WAV + JSON, loaded back
  through the firmware's loader with a 4096-sample staging: both planes
  bit-identical, every key rendering the same bits. **Checked to bite**: the
  mono conversion run forwards fails 8 checks; the loader's same-rate path
  removed fails 8 (the 45 s set no longer loads).
- `web/selftest.mjs` §7d / §9e (615 → **682**): the tail, the plan, the
  encoder (= encodeWav's data chunk), a drop over the fake byte for byte,
  every request ≤ max_body, the polls interleaving, the commit, one undo step,
  a replaced pad held, undo / redo, a stereo drop turning the kit stereo and
  GET_WAVE's R plane, every refusal both ways, ABORT, the 5 s timeout, out
  of sequence, two drops through the queue, paths 1 and 2 beside the pads, a
  **pads-only kit end to end** (New empty kit, a mono and a stereo pad,
  PLAY_KEY, Save set, a power-cycle — a fresh fake on the same card — Load
  set: the same pads, stereo, nothing unsaved; a drop after: unsaved again),
  a pre-pads firmware; then the page through real `dragover` / `drop` /
  `change` / `click` events (a Play pad, three files onto one pad, the last
  pad, a Kit cell, both pickers, a stereo file, a file longer than the room,
  no room, a busy module, a hidden tab, an undecodable file, the unsaved
  note's Save set with and without a name, New empty kit's "no" and "yes", a
  drop onto the empty kit) and `?demo=nopads`.
- Parity: the descriptor's `"pads": 1` and the 45 s planes are checked
  against the firmware source as every other capability.

## Sizes (release build, the same SDK)

SRAM 456 896 → 461 376 B (92.96 → 93.87 %; the PADS shell is built `-Os` like
the card code), DTCM 111 488 → 111 544 B, SDRAM 42 837 064 → 52 053 064 B
(63.83 → 77.57 %).

## Known / deferred

- No reclamation of held regions (above). A compacting set save is possible
  — write only the live spans and remap the JSON — but the autosave writes
  JSON-only against the WAV on the card, so the live positions would have to
  follow the compaction (a reload, or a remap kept beside the set); left for
  Combust.
- ~~No per-drop normalise option~~ — the page has one now (*normalise
  drops*, beta-sampler, below); the module still never normalises a drop.
- A 192 kHz break loads 8.5 s (above).

## Start in — the module powers up as a sampler (2026-10-25, branch `beta-sampler`)

*Combust, verbatim: "I need to test importing and playing individual samples,
and it may be nice to actually start in "sampler" mode and let the user flip
into Breakfix/drum mode and press play on their own."* Nothing here has run on
hardware; the bench steps are `docs/hardware-pass-beta.md` §10d.

**What it does.** Start in = **Sampler** (the default): at power-up, after the
boot preset and the card backups have been applied, the live grammar goes to
**Pads** and the transport is **stopped** before the first audio block. The
24 keys (J4, the page's pads, the Kit) play; nothing else does. The main loop
applies the grammar as any grammar change (patternMode, the Voice map under
Jacks = Follow grammar, the keys onto the markers if the material has any).
Pick a drum grammar (the page's chips, or the sampler line's Classic / Grids)
and the transport stays stopped; ▶ / B1 held starts it — the playhead ran on
underneath, so it comes in on the beat (BETA-FW's mute rule, unchanged).
Start in = **As saved**: the boot preset's grammar, running — v0.13 as it was.
The boot preset itself is never rewritten: a preset load later, or As saved,
brings its grammar back.

**Where it lives, and why not in Options.** The Options block is 24 bytes
under `'OPT8'`, managed by the SDK's presets, whose slot hash is the XOR of
every managed component's hash. A 25th byte moves that hash and the SDK then
drops **every** slot as empty. `RestorePresetBackups` brings a slot back only
if its `presets/<slot>.json` is on the card, and a backup carries options,
knobs, the sample and the name — **not the motion locks and not the
brightness** (`docs/packages/sample-sets.md`). So an Options byte would have
cost Combust's beta4 presets their locks at best, and any slot without a
backup outright. A new managed component or a Settings control moves the same
hash. So Start in is a **device setting on the card**:
`Audiothurgist/startup.json`, `{"audiothurgist":1,"startIn":"sampler"}` (or
`"saved"`). It is a power-up behaviour of the module rather than of a preset,
which suits it anyway: saving or loading a preset never changes it.

- `core/bb_startup.h`: the text both ways (`StartupJson` / `StartupRead`:
  no file, a broken or foreign file, an unknown word, no card → **Sampler**,
  the safe reading) and the FatFs half (`ReadStartup` at boot;
  `WriteStartup` — tmp + rename, so a power-off mid-write keeps the old
  setting; the caller lends the FIL and an AXI buffer: `gSetFil`,
  `gSetStage`, under `BusyGuard`).
- Boot: `ReadStartup` right after `RestorePresetBackups` / `PrimePresetSeq`,
  before `StartAudio`.
- `ACTION 40 START_IN (u8 0 Sampler / 1 As saved)` → OK, BAD_ARGS (> 1),
  NO_CARD (21) without a card. The handler only records it; the main loop
  (`ServiceStartIn`, beside the preset backups, never during a set op — they
  share `gSetFil`) writes the file; a failed write puts the reported value
  back to the card's. 37..39 are left free for the per-key branch.
- `GET_STATE` byte 3 (`flags`) **b5** = Start in is As saved (clear =
  Sampler). No bytes added to the 0x60 tail. The descriptor's root says
  `"startIn":1`.
- Presets, sets, the Options block, the schema, the tail: unchanged. A
  beta4 card needs nothing; the file appears the first time Start in is set.

**The panel at a fresh boot** (the existing language, nothing new): B1
breathes cool white (stopped), the P1 bar ring dimmed to a quarter with only
the playhead step going round (Pads' pattern is all rests), no gates out;
Edit's amber and a take's red still win. There is no panel control for Start
in or for the grammar: on the panel the way to a beat after a sampler boot is
a preset load (Settings) then B1 held — see the call below.

**The page, sampler-first** (polish, no new hues): in Pads with the transport
stopped (`samplerish`) the Play tab puts the pads card first and shows a line
over the pads — what plays, where a drop goes, **Classic** / **Grids**
buttons ("for a beat: … then ▶") that only write the grammar, and, when Start
in is Sampler, that the module powers up like this. The transport reads
*sampler*, the ring SAMPLER, ▶'s title says Pads has no pattern. Once a drum
grammar waits, stopped, a line under the modes says ▶ starts it and ▶ is
ringed in the accent. An empty kit's pads are dashed with a centred **+**
(the sub-text stays "empty"). Options gains **Start-up (kept on the card)**:
the Start in select, read back from b5, disabled with no card in.
`?demo=sampler` (and the Demo button) power the fake up this way; a bare
`?demo` keeps the old boot (Classic, running) the earlier checks drive.

**Normalise drops** (page only): a checkbox beside *Choose samples…*, **off by
default**, remembered per browser (`localStorage audiothurgist.padNorm`).
When on, `HL.padNormalize` scales the decoded planes so the loudest sample of
the frames that go sits at −1 dBFS (0.891), one gain for both channels (a
stereo image keeps its balance); silence is left alone; the line says
"normalised +11.0 dB". It applies to every drop path (a Play pad, a Kit cell,
both pickers). The module is unchanged: a drop is still placed as it comes.

**Tests.** `tests/card_check.cpp` 94 → **114** (the text both ways, every
truncation and foreign file read as Sampler, the FatFs half: no file, no
folder, written, read back, rewritten over a stale tmp, a file too big, no
card). `web/selftest.mjs` §9f, 682 → **725**: ACTION 40 and b5 over the fake,
the card file, two power-cycles, a card without the file, BAD_ARGS, NO_CARD,
an older firmware; `padNormalize`; then the page over `?demo=sampler` — it
reads as a sampler, Start in on the Options tab, **a mono WAV and a stereo
WAV** (real RIFF bytes, through a PCM16 stand-in for the browser's decoder)
dropped on two pads and arriving sample for sample, both **played from the
page's pads**, **the kit turned stereo**, normalise (remembered), **Save
set**, a **power-cycle** on the same card with the page reconnected (a
sampler again, the drops gone from memory), **Load set** — the same three
pads sample for sample, stereo, playing — and the flip to Classic, stopped
until ▶. Checked to bite (the normalise and the sampler detection removed:
three checks fail). Not covered by anything automated: the boot itself on
the module, and ▶ landing on the beat (the fake has no audio clock).

**Sizes** (release build, the same SDK): SRAM 461 376 → 463 232 B (93.87 →
94.24 %: the reader / writer, the action, the service and ~600 B of manual
text), DTCM 111 544 → 111 552 B, SDRAM unchanged (52 053 064 B, 77.57 %).

**Calls left to Combust.**
- A panel-only start: with Start in = Sampler and no computer, the grammar
  can only come from a preset load, and B1 held in Pads starts nothing
  audible. Proposal (not built): the first B1 hold / ▶ after a sampler
  power-up brings the boot preset's grammar back and starts it.
- The Demo button now powers the fake up as a sampler; a bare `?demo` URL
  keeps the old running demo.
- Start in sits on the card, so it is not in `/program` and not in a
  preset; with no card the module always starts as a sampler.
