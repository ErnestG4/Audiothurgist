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

## Protocol — `0x68 PADS`

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
op 4 RECLAIM  ← u8 mode (0 FREE: what no undo step needs · 1 ALL: everything held, the undo steps that need it forgotten)
              → status (the main loop does it; the tail's pads seq says when — "Held memory comes back", below)
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

### State bytes 293..323 (the end of the 324-byte body)

```
293 u8 padState (0 idle · 1 receiving · 2 committing · 3 clearing: New empty kit · 4 reclaiming)   294 u8 progress
295 u8 result (0 ok · 1 no room · 2 busy · 3 bad args · 4 the table is full · 5 timed out · 6 aborted
              · 7 the planes moved under it)
296 u8 seq (bumps per finished op)   297 u8 key (the drop's; 255 none)
298 u8 flags (b0 the kit holds audio the card does not — a power-off loses it · b1 an empty kit: no break
              · b2 the last drop made the kit stereo, Sampling → Stereo)
299 u32 free   (frames past freePos)
303 u32 total  (the pads' share: bufCap − bufLen)
307 u32 held   (frames in [bufLen, freePos) no live entry covers: replaced, deleted, undone recordings, trimmed edges)
311 u32 pads   (bit k = key k holds a recording)
315 u32 undoHeld   (of held, what some undo step could still bring back: FREE leaves it)
319 u32 reclaimed  (the last reclaim's frames)
323 u8  undoForgot (the undo steps the last reclaim dropped)                              — ends at 324
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
  rule). The old region is **held** until a reclaim gives it back — the
  Reclaim button, or a drop that needs the room (below, "Held memory comes
  back"); a set save never writes it. The tail's `held` is that memory and the
  page shows "pads: 12.30 s of 28.00 s free · 2.10 s held" beside "Reclaim
  2.10 s".
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
  then the page through real `dragover` / `drop` /
  `change` / `click` events (a Play pad, three files onto one pad, the last
  pad, a Kit cell, both pickers, a stereo file, a file longer than the room,
  no room, a busy module, a hidden tab, an undecodable file, the unsaved
  note's Save set with and without a name, New empty kit's "no" and "yes", a
  drop onto the empty kit).

## Sizes (release build, the same SDK)

SRAM 456 896 → 461 376 B (92.96 → 93.87 %; the PADS shell is built `-Os` like
the card code), DTCM 111 488 → 111 544 B, SDRAM 42 837 064 → 52 053 064 B
(63.83 → 77.57 %).

## Known / deferred

- ~~No reclamation of held regions~~ — reclaimed now: the Reclaim button, a
  drop that needs the room, and a set that holds only what plays, with the
  autosave following through a card map kept beside the set (below, "Held
  memory comes back").
- A take into a key (J5 style 7) does not reclaim on its own (arming is
  gate-latency work); Reclaim first when the room line shows held memory.
- ~~No per-drop normalise option~~ — the page has one now (*normalise
  drops*, beta-sampler, below); the module still never normalises a drop.
- A 192 kHz break loads 8.5 s (above).

## Held memory comes back — reclaim (2026-10-29, branch `beta-reclaim`)

*Combust: "This is software in development accountable to nobody … focus on
creating a great final product." The gap was the first "Known / deferred" line
below: a replaced, deleted or undone pad kept its memory until new material
arrived. It now comes back without the player having to think about it.*
Owns `core/bb_reclaim.h` (new), `tests/reclaim_check.cpp` (new), the
`/* RECLAIM */` lines of `src/audiothurgist.cpp`, the gathered write in
`core/bb_wavwrite.h` / `bb_set.h`, `SetFromEngine` through a map in
`core/bb_sets.h`, and the reclaim parts of the page, `hostlink.js`, `demo.js`
and the selftest (§7h, §9k). **Nothing here has run on hardware**; the bench
steps are `docs/hardware-pass-beta.md` §10h.

**What is held.** Memory in `[bufLen, freePos)` no live entry covers: a
replaced pad's old take, a deleted pad, a drop or take that was undone, the
edges a trim cut off. The tail's `held` is all of it; `undoHeld` (new) is the
part some undo step could still bring back.

**When it comes back.**

| | what | Undo |
|---|---|---|
| **Reclaim** (the button in Play's pads row and the Kit's Buffer: "Reclaim 2.10 s") | everything held (mode ALL) | the steps that need a reclaimed sound are forgotten — the page asks first, only when there are any |
| **A drop that does not fit** | first what no undo step needs (FREE) if that is enough, else everything (ALL); then the drop lands whole | FREE loses nothing; ALL forgets as above — no question, the line says so ("· reclaimed 0.50 s first (Undo forgot 2 steps)") |
| **Save set** | nothing moves in memory: the set **holds only what plays** (the break and the live spans, packed) | untouched |
| a load, a loop take, New empty kit | as ever: the planes start over | as ever |

Why a drop reclaims by itself rather than asking: the alternative is a drop
cut short or refused while seconds sit unused, and the only cost — undo steps
for sounds you already replaced — is said in the line and in the history. Why
the save does not compact the memory: it does not need to (the card map,
below, lets the file be packed while the planes stay put), so a save never
moves anything that sounds and never costs an undo step. Takes into keys
(J5 style 7) do not reclaim on their own: arming a take is gate-latency work,
and a take records into whatever is free; press Reclaim first if the room line
says something is held.

**How — the planes.** `BuildReclaimPlan` lists the kept runs over
`[0, freePos)`: the break, every live entry's span and (FREE) every span the
live undo journal holds, merged where they touch; each run moves down to
`dst`. `Map(p)` is the move as one monotone function (the identity over the
break, a run's offset inside it, the next kept position inside a gap, past
freePos by the total), so every position anywhere — a table entry, a voice, a
journal record, a card-map segment — moves the same way. The copy is never in
place: the main loop copies the runs into the **other** buffer (the load
buffer, `gLoad` / `gLoadR`, free whenever no load, loop take or drop is in
flight — all refused meanwhile), so the planes the voices read stay whole
throughout; then the audio thread swaps the buffers at a block's start
(`ReclaimSwap`, as a load does) and moves every position with them in the
same block: the entries past the grid and their cached loop points, freePos,
each sounding voice (`pos`, its start, ends, bound and loop points). A voice
playing a live pad goes on sounding sample for sample — a held pad, a Loop
key, a ratchet all run straight through it. A voice still reading something
that goes (a replaced pad's old take still ringing, an audition across a gap)
is faded first (`ReclaimFade`, the choke geometry, each block until it is
silent — the swap waits up to 100 ms) and cut at the swap if anything is left
(its last sample carried as the steal's bridge). The swap refuses if the table
moved since the plan (a J5 style-8 Clear landing mid-copy); the main loop then
plans and copies once more. Cost: the kept runs copied once, both planes —
the break plus the pads, at most 2 × 8.6 MB, a few hundred ms of main-loop
time at SDRAM speed; `KeepClock` runs the whole control loop at 1 kHz through
it (`gPadTicking`), and everything long yields every 16 Ki samples. The two
buffers trade places, so `gPlay` / `gLoad` simply swap as at a load.

**How — undo.** Every journal record holds spans (`UndoEntry` starts and
lengths; a NUDGE_ALL record holds starts only, read beside the length its slot
will have when the record is restored — the walk follows the slot lengths
back from the cursor through the undo half and forward through the redo
half). FREE keeps every span any record holds, so the whole history survives
and is only remapped through `Map`. ALL keeps only what is live, so the
records that need a reclaimed span go: in the undo half the newest such step
and everything older, in the redo half the first such step and everything
after (cut at step heads, so a joined step goes whole); what is left is
remapped and undoes and redoes exactly as before. This runs on the main loop
after the swap — nothing edits in between (every edit, Undo included, is
refused while a reclaim runs) — and the journal is bound to the new
generation. A journal the module had already gone past (a take's commit, a
Clear) counts for nothing and is cleared. `undoHeld` in the tail is worked out
from a FREE plan whenever the table moves (`ServiceRoom`, main loop, idle).

**How — the set and the card map.** `CardMap` says where each live span lies
in the card's copy of the kit: segments live ↔ card. A loaded set or break is
the identity; **Save set** packs (`PackForCard`: the ALL plan's runs, without
touching memory), writes the JSON through the map (`SetFromEngine(…,
CardMap)`) and the WAV gathered from the map's segments (`WriteSpec`
`segFrom` / `segLen`), so the file holds exactly what plays and its positions
are the file's; a reclaim moves the live side of the map (`Follow`). The
autosave writes the JSON through the map too — the WAV on the card never
changes, and whatever the memory did since, the JSON describes that WAV (an
entry the map cannot place — a pad dropped since — is left out, as a take past
the WAV always was). The unsaved note reads the map: a pad is saved when the
card's copy holds it, wherever it sits in memory now. **The set format** is
unchanged in shape (schema 1, the same keys): only what the positions mean
changed — the WAV is the break plus the live spans packed, `len` its length,
every `start` a position in it — so a set loads with nothing held. An older
set with held memory in its WAV loads as before; the held part then reclaims
like any other.

**Protocol (api 2).** `0x68 op 4 RECLAIM ← u8 mode (0 FREE · 1 ALL) →
status` (BAD_STATE as a BEGIN: a set op, a load, a take, a card job, a drop,
a set's recordings still to commit); `padState` 4 = reclaiming; the tail grows
to 324 bytes: `315 u32 undoHeld · 319 u32 reclaimed · 323 u8 undoForgot` (the
last reclaim's frames and dropped steps). The descriptor's `"api"` is 2 and
`HL.API` with it; a page and a firmware of different api refuse each other as
before.

**The page.** The room line reads "pads: 12.30 s of 28.00 s free · 2.10 s
held" (its title says how much Undo could still bring back); **Reclaim 2.10 s**
sits beside it and in the Kit's Buffer ("held 2.100 s (1.200 s of it Undo can
still bring back)"). The click asks only when Undo would lose steps; the
answer is one line ("reclaimed 2.10 s — 14.40 s free for pads · Undo forgot 3
steps") and one history line that is no undo step, the forgotten steps greyed
"forgotten" (`History.info`; Undo and Redo never mark them). A drop that does
not fit says "making room — reclaiming 2.10 s held…" on its pad and what it
did in its line. Colours: the meter's held segment and the buttons keep the
existing tokens (`--ink3`, `.mini`) — no new hue.

**Tests.** `tests/reclaim_check.cpp` (**99**, ASan/UBSan): the plan (runs,
`Map`, `Inside`, the cap's conservative merge); a compaction keeps **every
live key sounding the same bits** against a twin engine that never reclaimed
— one-shot, held and reversed calls, a Loop key and a Ratchet key, mono and
stereo, every entry's samples where it now starts on **both planes**, cached
loop points moved; **undo across it** — FREE with the whole history undone
and redone bit for bit (and with orphans reclaimed under a history of two
NUDGE_ALLs, a split and a replacement), ALL forgetting exactly the steps that
need a reclaimed sound (a trim's edges included) and the rest undoing and
redoing bit for bit (a bulk kSnap step and a grid nudge in the redo half), an
undone drop's redo forgotten by ALL and kept by FREE, a stale journal cleared; **voices across the swap** — a held pad, a ratcheting
key, a held Loop key and a marker sounding through it, identical block for
block; a replaced pad still ringing faded by ReclaimFade within a few blocks,
a voice the fade did not finish cut, nothing reading past the new freePos; a
refused swap touching nothing; **sets** — a packed save (the held span left
out, every entry where the map says), loaded back bit for bit; save → load
after a compaction; the card map through a reclaim: the autosave's JSON over
the old WAV loads back the same bits for every pad the card holds, and the
unsaved note exact before and after; the gathered WAV writer sample for
sample. Checked to bite: thirteen mutations (voices not moved, the journal
or the nudge starts not remapped, a nudge's span one sample long, FREE keeping
no undo spans, no truncation in either half, loop points not moved, no fade,
no cut, the map not followed, R not swapped, the gather ignored) fail 1–13
checks each — and the first run found the redo half's head walk reading one
record past the ring's count. `pads_check` now saves as
the firmware does (a replaced pad left out of the set). `web/selftest.mjs`
§7h / §9k (733 → **765**): the tail's bytes, `padReclaimFor`, then over the
fake: FREE leaving what Undo needs, ALL taking all of it and forgetting those
steps, every key the same samples on both planes, a stale history counting for
nothing, a packed save loading back the same on a fresh module, still saved
after a reclaim and unsaved after a drop, the refusals both ways; then the page
through real drops and clicks — the room line, both buttons, the question and
its no / yes, the history line, a drop reclaiming ALL on its own and landing
whole, one reclaiming FREE. Checked to bite: no auto-reclaim fails 4, the
fake's table not moved 14, the page never asking 3, a save not packed 2.

**Sizes.** SRAM 469 184 → 472 608 B (+3 424, 95.46 → 96.15 %: the plan, the
journal walk, the swap and the voice pass, the map, `PadReclaim` /
`ServiceRoom`; all of `bb_reclaim.h` `BB_COLD`, one `SetFromEngine` body, one
journal walk and one voice pass shared by both modes), DTCM +8 B, SDRAM
+6 240 B (two plans and two maps, NOLOAD PODs; `sdramcheck` clean). Merged onto beta 9e82e0e (the review fixes, beta-env): SRAM 473 184 →
476 544 B (+3 360, 96.27 → 96.95 %), DTCM +8 B, SDRAM +6 240 B.

## Start In — the module powers up as a sampler (2026-10-25, branch `beta-sampler`; an Option since `beta-clean`)

*Combust, verbatim: "I need to test importing and playing individual samples,
and it may be nice to actually start in "sampler" mode and let the user flip
into Breakfix/drum mode and press play on their own."* Nothing here has run on
hardware; the bench steps are `docs/hardware-pass-beta.md` §10d.

**What it does.** Start In = **Sampler** (the default): at power-up, after the
boot preset and the card backups have been applied, the live grammar goes to
**Pads** and the transport is **stopped** before the first audio block. The
24 keys (J4, the page's pads, the Kit) play; nothing else does. The main loop
applies the grammar as any grammar change (patternMode, the Voice map under
Jacks = Follow grammar, the keys onto the markers if the material has any).
Pick a drum grammar (the page's chips, or the sampler line's Classic / Grids)
and the transport stays stopped; ▶ / B1 held starts it — the playhead ran on
underneath, so it comes in on the beat. A start made in Pads first brings back
the last drum grammar — the boot preset's, or the one Pads was entered from
(`TransportStart` / `gLastDrum`) — so on the panel alone B1 held after a
sampler power-up plays the preset's beat. Start In = **As saved**: the boot
preset's grammar, running. The preset itself is never rewritten: a preset load
later brings its grammar back, and a preset **saved** while the grammar is Pads
only because Start In put it there (no grammar chosen since the power-up) keeps
the beat it came with (`gLastDrum`, `PresetKeepsBeat`: the saved slot's grammar
byte patched, one more flash write) — Pads chosen by the player saves as Pads.

**Where it lives.** Options byte 24 (`start.in`, "Start In": *Sampler (Pads,
stopped)* · *As saved (the preset's grammar, running)*), in the preset-managed
`Options` block — 25 bytes under `'OPT9'`. It shows in the page's Options
(under *Power-up*) and in `/program` like every other field, and a preset save
keeps it. Boot reads it once, after `presets.BootLoad()` and the backup
restore; changing it does nothing until the next power-up. A set load keeps
the device's value (as it keeps Autosave): a set is material, not how the
module starts.

It was a file on the card (`Audiothurgist/startup.json`, ACTION 40, GET_STATE
byte 3 b5) for one beta, only so that adding it would not move the preset
schema; the page and the firmware now ship together and the presets are
allowed to move once. **Flashing this build drops the flash slots once** (the
SDK's slot hash is the XOR of every managed component's hash, and 'OPT9' moves
it); `RestorePresetBackups` brings each slot with a `presets/<slot>.json` back —
options, knobs, the sample and the name, **not the motion locks and not the
brightness** (`docs/packages/sample-sets.md`). A backup written before this
build has 24 option bytes; the 25th (Start In) takes its default, Sampler.

**The panel at a fresh boot** (the existing language, nothing new): B1
breathes cool white (stopped), the P1 bar ring dimmed to a quarter with only
the playhead step going round (Pads' pattern is all rests), no gates out;
Edit's amber and a take's red still win.

**The page, sampler-first** (polish, no new hues): in Pads with the transport
stopped (`samplerish`) the Play tab puts the pads card first and shows a line
over the pads — what plays, where a drop goes, **Classic** / **Grids**
buttons ("for a beat: … then ▶") that only write the grammar, and, when Start
In is Sampler, that the module powers up like this. The transport reads
*sampler*, the ring SAMPLER, ▶'s title says Pads has no pattern. Once a drum
grammar waits, stopped, a line under the modes says ▶ starts it and ▶ is
ringed in the accent. An empty kit's pads are dashed with a centred **+**
(the sub-text stays "empty"). `?demo=sampler` (and the Demo button) power the
fake up with Start In = Sampler; without it the fake's boot preset says As
saved (Classic, running), which the selftest drives.

**Normalise drops** (page only): a checkbox beside *Choose samples…*, **off by
default**, remembered per browser (`localStorage audiothurgist.padNorm`).
When on, `HL.padNormalize` scales the decoded planes so the loudest sample of
the frames that go sits at −1 dBFS (0.891), one gain for both channels (a
stereo image keeps its balance); silence is left alone; the line says
"normalised +11.0 dB". It applies to every drop path (a Play pad, a Kit cell,
both pickers). The module is unchanged: a drop is still placed as it comes.

**Tests.** `tests/persist_check.cpp` pins the 25 option bytes (a set and a
preset backup carry them). `web/selftest.mjs` §9f: Start In is Options byte 24
(Sampler the default), written through the live blob without touching the
running module, a power-up with each value (Sampler: Pads, stopped, the Voice
map, the preset's beat kept for ▶), ▶ in Pads bringing Classic back and Grids
→ Pads → ▶ bringing Grids back, ACTION 40 gone; `padNormalize`; then the page
over `?demo=sampler` — it reads as a sampler, Start In on the Options tab (both
ways, through the live blob), **a mono WAV and a stereo WAV** (real RIFF
bytes, through a PCM16 stand-in for the browser's decoder) dropped on two pads
and arriving sample for sample, both **played from the page's pads**, **the
kit turned stereo**, normalise (remembered), **Save set**, a **power-cycle**
on the same card with the page reconnected (a sampler again, the drops gone
from memory), **Load set** — the same three pads sample for sample, stereo,
playing — and the flip to Classic, stopped until ▶. Not covered by anything
automated: the boot itself on the module, and ▶ landing on the beat (the fake
has no audio clock).

**Sizes.** As an Option it costs less than the card file did: the reader /
writer, ACTION 40 and its service are gone (with the rest of `beta-clean`,
SRAM 469 664 → 469 216 B).
