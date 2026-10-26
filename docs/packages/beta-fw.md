# BETA-FW — the firmware half of v0.13.0-beta (2026-10-14, branch `beta-fw`)

*The firmware for [beta-plan-2026-10-14.md](../beta-plan-2026-10-14.md) §1
(the Kyklophoria port's firmware line: the load meter, one version string)
and §2 (Combust's bench report of 2026-09-22 and his calls of 2026-10-14):
the transport, TAP_MARK that stops stacking, markers → keys, clearing
markers, one CV keymap, autoslice with a count knob, on-screen pads. Owns the
`/* BETA */` lines of `src/audiothurgist.cpp`, `core/bb_keys.h` (new), the
BETA parts of `core/bb_engine.h` / `bb_edit.h` / `bb_undo.h` /
`bb_panel_edit.h` / `bb_clockmath.h`, `tests/keys_check.cpp` (new), the BETA
sections of `engine_check` / `panel_check` / `clock_check`, the manual's
keymap and buttons text, README's keymap lines. Nothing under `web/`: the
page revamp is its own package and reads what is below. Nothing here has
run on hardware; the checklist at the end is Combust's.*

## What changed for the player

| | before (v0.12.3) | now |
|---|---|---|
| **Start / stop** | a J4 note one semitone under 0 V (Voice map) or 0 V (Brain map) toggled mute; the page's mute button | **B1 held ≥ 600 ms and let go**, no pot turned, no other button: ▶/■. The pattern stops, the clock runs on (a restart lands on the beat), keys still play. ACTION 35 sets it. |
| **J4** | Voice map: one key per slice region from 0 V, −1 st = mute. Brain map: 0 V mute, 2..11 latched modifiers, 24..39 sixteen slice keys | **One map: 0 V = key 0 (C1)**, a semitone a key to 23, negatives = key 0, always through the Kit's key table, in both jack maps |
| **J5** | Voice map style, semitone steps from 0 V — an unpatched jack (~60 mV = 0.72 st) read **1 = reverse** | the same steps with a **wide zero**: under 0.85 st (71 mV) is plain. Brain map: J5 is the gate, calls are plain |
| **TAP_MARK / B3 chop in Edit** | took the current step's slice start when nothing sounded; every tap stacked a new marker | **refused** when nothing sounds; a tap **within 10 ms of a live marker selects it** |
| **Markers → keys** | by hand (SET_KEY per key) | ACTION 33; **automatic in Pads** whenever the markers change |
| **Clearing markers** | one TOMBSTONE per marker | ACTION 34, one undo step |
| **Autoslice** | FROM_ONSETS(threshold, gap) only | ACTION 36 and **P5 on the Edit page**: a count, onsets drawn at random weighted by salience; up adds one, down drops one, sweep to 0 and back = a new draw |
| **On-screen pads** | — | ACTION 32 PLAY_KEY (down / up) |
| **CPU** | audioUs (worst µs) and overruns | plus **loadAvg / loadMax** in ‰ of the block budget |

## The transport hold (`bb::panel::Hold`)

B1 is the page button (the Pager, on release), the ParamLock button (held
≥ 300 ms through `HoldButton`, arms on a pot move) and half of Edit's entry
(B1 held + B3 tap). The hold had to fit between them without moving any:

- A B1 press is watched only when it starts outside Edit and Settings.
- Any pot moving more than 3 % from where it was at the press (ParamLock's
  own threshold), or B2 / B3 going down, **spoils** it: a spoiled hold does
  nothing. So B1 + pot still records a lock, B1 + B3 still enters Edit,
  B1 + B3 held still loads.
- From 600 ms on, OnPoll calls `pager.ConsumeButton()` every poll while B1
  is down (the Edit page's own technique), so **a long press never flips the
  page** — clean or spoiled. A tap (< 600 ms) flips it as ever.
- A clean release past 600 ms toggles `engine.muted` — mute keeps its
  meaning: pattern steps silent, the clock running, calls and keys sounding.

**LEDs** (the existing language: white = the clock / a pulse, amber = Edit,
red = record / refusal, the dimmed bar ring = muted): B1 goes **full cool
white** the moment a release would toggle (let go now); while stopped B1
**breathes cool white** (1.6 s, the clock's white flash still shows through)
and the bar ring stays dimmed to a quarter as before. Edit's amber and a
take's red win over both.

## One CV keymap (`core/bb_keys.h`)

`KeyOf(volts, last)`: the nearest note, with a Schmitt band round the last
note only (a reading holds it within 0.5 + 0.1 st), clamped to 0..23. *Fixed
2026-10-23* after Combust's "sometimes playing the last note": the first
quantiser returned the last note for any reading outside ±0.4 st of a note
centre, wherever J4 was — a key that read 0.45 st off, or J4 caught mid-step
on the gate's poll, played the previous key. And the gate is now a
sample-and-hold (`GateSettle`): the key is read on the first poll from the
edge on which J4 moved < 0.15 st — the edge's own poll when J4 was already
still, else a poll or two later, never more than 4 ms; a trigger that falls
during the wait still plays and releases. `StyleOf`: the same
with the zero window — under 0.85 st it is 0 and the memory resets.
`CallFor(style, grammar, voiceSel)`: rate / reverse / ratchet / staccato and
the punch rule (Pads never writes; Play writes unless style 6; elsewhere the
Voice option, flipped by 6). The shell reads J4 every poll for the tail's
live J4 → key, and `CallKey(key, style)` is the one call path: Voice map,
Brain map and PLAY_KEY. It sets keyFlash and bumps keySeq on every call; in
Edit it selects the key's entry. Key Plays = Held: the gate's fall releases
(Brain map: J5's fall, only in Held, so Play's loop does not fill with
release events of one-shots). A key held from the jacks is released when the
jack map changes. Record (7) / Clear (8) and the restrike on a style change
are unchanged. The Jacks option is unchanged (Follow grammar = Play and Pads
on the Voice map).

## The table ops

**One undo step per bulk op — `EditReq::kSnap` / `kQuiet`.** FROM_ONSETS
used to join one journal record per apply (`kJoin`): 40 grey markers out and
40 in was 80 records, more than the 64-record ring, which then dropped the
step's head — a partial undo. Now the first apply of a bulk op carries
`kSnap`: its record is a snapshot of every slot of the marker range
`[nSlices, kMaxTable)` with the key map and `nTable`; every later apply
carries `kQuiet` and leaves no record. `kQuiet` is honoured only when the
journal is live, nothing was undone since, and the newest record restores
what the op writes (any record carries the key map → SetKeyMap; a snapshot
carries the markers → add / re-span / tombstone); otherwise the op is
journaled, joined to the step it follows. `UndoRec` grew from 440 to 2 336 B
(the union holds 96 entries), the journal from 28.6 to 151.9 KB of SDRAM
(NOLOAD, POD); a push still copies only `Bytes()`.

| ACTION | body | reply | rule |
|---|---|---|---|
| 29 TAP_MARK | `u8 snapMs, u8 allowMs [, u8 gen]` | `u8 status, u8 tableGen, u8 idx, u8 flags` | BAD_STATE when nothing sounds (tail 263 b0 clear). `bb::TapMarkAt` as before; then a live marker (or recording) whose start is within 10 ms (480 samples) of the mark's → **selected, not added**: `idx` = that entry, `flags` b0 = 1, the table and gen unmoved |
| 32 PLAY_KEY | `u8 key 0..23, u8 down 1 / up 0` | `u8 status` | the key as a CV note plays it (style 0, Key Plays honoured), any grammar, any jack map; up releases a held key, a no-op for one-shot. Waits ≤ 2 ms for the shared call / release slot. BAD_ARGS key > 23 or down > 1; BAD_STATE no material |
| 33 KEYS_FROM_MARKERS | `[u8 gen]` | `u8 status, u8 tableGen, u8 count` | live markers in time order → keys 0..; a key that owns a recording is skipped and kept; keys past the markers → their span slice. One SetKeyMap = one step; nothing posted (no empty step) when the map already follows. `count` = keys given a marker |
| 34 CLEAR_MARKERS | `[u8 gen]` | `u8 status, u8 tableGen, u8 count` | every live marker, grey and orange, tombstoned; keys on them → their span slice. One step |
| 35 SET_MUTE | `u8 1 stopped / 0 running` | `u8 status` | a start in Pads brings the last drum grammar back; op 2 (the old toggle) is gone |
| 36 AUTOSLICE | `u8 k 0..64, u8 flags (b0 re-roll · b1 join) [, u8 gen]` | `u8 status, u8 tableGen, u8 count` | below. `count` = the grey markers now. BAD_ARGS k > 64 or unknown flag bits |

A **marker** (`Engine::IsMarker`) is a live entry past the grid, inside the
break, owned by no key — a recording is not one however it was re-keyed; a
**grey** marker is a marker that is not orange (FROM_ONSETS and AUTOSLICE
own those; a tap or a drag makes orange ones).

**AUTOSLICE.** Candidates: the onsets inside the break, less those within
10 ms of an orange marker. Weights: `OnsetStrength` — the FROM_ONSETS proxy,
the salience of the grid slice the onset falls in (×255; the detector's
per-onset flux is not kept by the shell). The draw (`bb::AutoSliceOrder`):
Efraimidis–Spirakis, key = u^(1/w) compared as ln(u)/w, w = (s + 1)/256 so
a silent onset still weighs something; u is a hash of **(seed, the onset's
position)**, so for one seed the order is fixed — the first k are a subset
of the first k + 1 — and an onset keeps its key when others leave the
candidate list. b0 re-roll draws a new seed. The first k drawn, back in time
order, each spanning to the next (the last to the break's end; a span under
10 ms is dropped, as FROM_ONSETS does), become the grey markers through
`bb::PlanGrey`: greys at no drawn onset are tombstoned, a grey already at one
keeps its slot and its keys and only re-spans (`SetEntry` + `kKeep`, so it
stays grey; re-analysed), the rest are added (analysed). One detent up is
one add and one re-span. Orange markers, recordings and the grid are never
touched; a full table keeps what fitted (FROM_ONSETS' rule). **b1 join** (a
firmware extra, not in the plan): fold into the previous AUTOSLICE's undo
step when nothing else changed the table since — a slider dragged through
twenty counts undoes as one. The panel always joins.

**Pads: keys follow markers.** In Pads, after every op that changes the
marker set — ADD, SET_ENTRY, SPLIT, MERGE, TOMBSTONE, NUDGE_ALL(markers),
TAP_MARK, FROM_ONSETS, CLEAR_MARKERS, AUTOSLICE, and the panel's chop, revert
and marker nudges — the shell posts the KEYS_FROM_MARKERS map **quiet**, so
it rides in that op's undo step: one undo takes the marker and the key map
back together (measured, below). Entering Pads follows once as a step of its
own. SET_KEY / SWAP_KEYS do not trigger it — a hand-made map stands until
the markers next change. With no markers nothing follows (tail 263 b1 is
"Pads with markers").

## The Edit page's P5: the autoslice count

P5 was "unchanged (Sample)" in Edit. Now: **0..32** over the travel (33
detents of ~8°), with 0.7-detent hysteresis so a pot resting on an edge never
flickers (every change re-slices). 32 is the marker room a 64-step grid leaves
(96 − 64); the page's AUTOSLICE goes to 64. The pot **picks up** at the grey
count it finds (the SDK's catch, at a target: proximity within half a detent
or a crossing), re-armed whenever the table moves under it (a page edit, an
undo, a revert). Counts go out through a 100 ms rate window, the last wins;
coming up from 0 re-rolls; a sweep folds into one undo step. The ring: the
count as a grey-white arc (dim until picked up), the selection's key cells
pulsing on top as before (the kit map's red cells are not drawn in Edit). On
leave P5's stored value is put back with `SetStored`, as P1..P3 — the file
highlight (Perform) or the hat density (Brain) does not jump. OnFrame does
not read P5 while Edit is on.

## The load meter

`DWT->CYCCNT` at the callback's entry and exit (enabled at boot, Kyklophoria's
three lines); per block the cycles go into a one-second window (sum, peak,
count — 32-bit words, saturating); the existing per-second diag roll in
OnPoll turns them into **‰ of the block budget** (`bb::CycleBudget` from the
core clock: 480 MHz × 24 / 48 000 = 240 000) with `bb::LoadPermille`. 1000 ‰
is the whole block; an overrun reads past it. audioUs (worst µs, TIM2) and
overruns (> 450 µs, lifetime) are unchanged.

## State tail and descriptor

Append-only after 262 (`redoDepth`); the body is **276** bytes. An old page
reads its 263 and ignores the rest.

| byte | field | meaning |
|---|---|---|
| 263 | `u8 flags2` | b0 **srcValid**: a voice that is not an audition sounds — TAP_MARK will be taken · b1 **keys follow markers**: Pads with ≥ 1 marker |
| 264 | `u16 loadAvg` | the audio callback, ‰ of the block budget, mean over the last second |
| 266 | `u16 loadMax` | ‰, the worst block of the last second |
| 268 | `i16 j4mV` | J4 now, millivolts (the 1 kHz filtered reading) |
| 270 | `u8 j4Key` | the key J4 would play now (0..23) |
| 271 | `u8 j5Style` | Voice map: the style J5 reads now (0..8); 255 in the Brain map |
| 272 | `u8 keyFlash` | the last key called by anyone (jacks, page), 255 none |
| 273 | `u8 keySeq` | bumps on every key call (a repeat of the same key is still seen) |
| 274 | `u8 autoK` | the AUTOSLICE count last applied; 0 none, and after FROM_ONSETS, CLEAR_MARKERS or new material |
| 275 | `u8 nMarkers` | live markers (`Engine::MarkerCount`) |

This beta's descriptor said `"edit":2` and `"keys":1`; since `beta-clean` it says only
`{"api": n}`, the one number the page checks (the page and the firmware ship
together). HELLO and the descriptor carry `0.13.0-beta` from one constant,
`kFirmwareVersion`.

## What is host-tested

- **engine_check** (655 → **699**): SET_KEY_MAP (−1 = default, a recording's
  key defaults to it, BadArgs past the table and on a tombstone, nothing
  written on refusal, one step undone exactly); IsMarker / IsGrey /
  MarkerCount; MarkerNear (±10 ms, not past it, a recording counts, a
  tombstone does not); KeysFromMarkers (time order, the recording's key kept,
  the rest their span slice); **Pads' fold** — an orange ADD then the quiet
  key map: one record, one Undo restores table and keys byte for byte, one
  Redo both; a quiet op the newest record does not cover is recorded and
  joined; after an undo a quiet op is a step of its own; **a bulk op of 90
  applies** (6 tombstones, 84 adds filling the table, 2 Full) is one record,
  one Undo restores every slot and `nTable`, one Redo all of it, the steps
  before still undo; kKeep keeps grey, without it orange; **the draw** — a
  permutation, deterministic per seed, another seed reorders, the order is the
  keys' descending order (nested for every k), dropping half the candidates
  keeps the rest in order, over 2 000 seeds the loudest onset is in the first
  five 3× more often than the quietest and the quietest still comes up;
  **the plan applied as the shell posts it** — k = 5 replaces a stray grey,
  keeps the orange, the last spans to the break's end, one step; 5 → 6 = one
  add + one re-span and the 6-set contains the 5-set; 6 → 5 = one tombstone +
  one re-span; 0 removes every grey; 0 again posts nothing; four undos restore
  the start exactly; **SrcValid** false muted with no call, true on a call (at
  its position), false once it ends with only an audition sounding. The
  journal's size assertion moves to 2 336 / 151 856 B.
- **panel_check** (116 → **141**): the pickup at a target (the default still
  centre, inert on one side, crossing, proximity within half a detent from
  either side); AutoCount (ends, clamps, monotonic over 400 positions, an
  edge holds, a detent moves it, AutoPos reads back); AutoChange (0 → n
  re-rolls, nothing else does); the hold — a 200 / 590 ms tap neither
  consumed nor toggled, 601 ms and 2 s clean toggle and are consumed, a pot
  past 3 % or B2 / B3 consumed without toggling, 2 % jitter still clean, a
  press from Edit / Settings not watched, a release with no polls never toggles.
- **keys_check** (new, **24**): every key at its semitone, 0 V = key 0,
  negatives → 0, past 23 → 23, the window and its memory either way, a first
  boundary reading; the style's zero (60 mV and 70 mV plain, 75 mV reverse),
  1..8, the hold between notes, the reset under the zero, the tail byte; the
  call plan (each style, Punch, style 6 both ways, Play, Pads never writes
  for any style × option, clamping); millivolts and saturation.
- **clock_check** (65 → **73**): the budget at 480 / 400 MHz, ‰ mean, peak,
  an overrun past 1000, rounding, saturation, a heavy block hidden in the mean
  and shown by the peak.

`tests/run.sh` **ALL GREEN** (grammar hash `c2081c20c9d7fbbf` unchanged;
persist 785, record 53, sets 144, scope 96 as before); `node web/selftest.mjs`
**420 / 420** (unchanged — the page does not read the new tail yet).

## Sizes

`make` against alchemy-sdk v0.11 + the boost patch, `arm-none-eabi-size`,
a52edf2 → this branch: text 399 524 → **408 500** (+8 976), data 4 596 →
4 676, bss 39 637 528 → **39 764 416** (+126 888: the journal's 123 240, the
autoslice scratch and plan, 3.6 KB, in SDRAM). Link map: SRAM (code + data,
BOOT_SRAM) 441 792 → **450 848 B, 91.7 %** (was 89.9 %; 39 KB left), DTCM
111 176 → 111 328 B (84.9 %), SDRAM 58.8 → 59.0 %. `tools/sdramcheck.sh`:
nothing new touches SDRAM before `main` (`gAutoScratch`, `gGreyPlan` are
PODs under `BB_SDRAM_POD`). The manual grew by ~450 characters (the
descriptor's 24 KB buffer also holds the card's file list).

**Audio thread:** one `SrcValid()` scan of eight voices per block, two DWT
reads; ApplyEdit's worst case is a snapshot capture or restore of 95 entries
(field copies, no onset search — a marker's restore skips it), cheaper than
the NUDGE_ALL(markers) that already ran there. No heap; every bulk op's work
(planning, analysis, the posts) is on the control thread through the
existing mailbox.

## Deviations and choices

- **AUTOSLICE weights are slice salience, not per-onset flux.** The shell
  keeps onset positions only (`gPendOnsets`); FROM_ONSETS already uses this
  proxy and the page can reproduce it from GET_HITS + GET_TABLE.
- **A marker excludes recordings** (keyed or keyless, past the break), so
  CLEAR_MARKERS never deletes a take and KEYS_FROM_MARKERS never moves one.
- **Keys past the markers go to their span slice** in KEYS_FROM_MARKERS and
  in Pads (a deterministic map, so "follows" is checkable: tail 263 b1).
- **Entering Pads follows once** (its own undo step) — the plan says "whenever
  markers change"; without it b1 would claim a following map that is not.
- **AUTOSLICE skips onsets within 10 ms of an orange marker**, and re-spans
  greys in place rather than tombstone-and-add (keys on them stay).
- **AUTOSLICE flags b1 (join)** is an addition the page may use.
- **FROM_ONSETS moved to kSnap / kQuiet** (one record per op instead of one
  per apply) — the same result, one undo step, no ring overrun.
- **The Brain map honours Key Plays** (held keys release on J5's fall) — the
  plan's "as a CV note would" for one map.
- **B1 long press never flips the page** (clean or spoiled), as "instead of
  the slow page flip" implies.
- **The panel's chop is refused when nothing sounds** (was: when muted) — keys
  played while stopped can be chopped; B3's chop LED follows srcValid.

## For Combust to decide

- P5's range (0..32) and its 100 ms rate; whether a sweep should be one undo
  step (it is) or one per detent.
- Whether entering Pads should rewrite the keys (it does, undoably).
- Whether the B1 white "let go now" cue is wanted, or only the breathing.

## How Combust tests it (hardware)

1. **Version and load.** Connect the page: HELLO says `0.13.0-beta`. Play a
   break: the page's CPU readout (tail 264 / 266) shows a steady mean and a
   higher peak, both well under 1000 ‰; pile on reverb (P1 full left) — *listen
   for* nothing changing; the numbers rise, overruns stay 0. Write the two
   numbers down: they are the beta's baseline.
2. **Stop / start.** Hold B1: at 0.6 s it turns white; let go — *listen for*
   the pattern stopping dead while the clock LED keeps pulsing; B1 breathes.
   Hold and let go again on a downbeat — *listen for* the break coming back
   in time (not from step 1). Tap B1: the page flips as ever. Hold B1 and turn
   a pot: a lock records, the transport does not change. Hold B1 for 2 s and
   let go clean: still a toggle, never a page flip.
3. **Keys from the jacks (Voice map).** Unpatch J5. Play C1 (0 V) into J4 with
   a gate on J6 — *listen for* key 0's slice, forwards (no reverse from a
   floating J5). Walk up a chromatic octave: one key per semitone. Send −1 V:
   key 0, no mute. Patch J5 at 1/12 V: reverse.
4. **Keys from the jacks (Brain map).** Options → Jacks = Brain. Gate on J5,
   same notes on J4 — *listen for* the same keys, plain; 0 V plays key 0 and
   never mutes; J6/J7/J8 still send the class gates.
5. **Tap marks.** Stop the transport, no keys: TAP_MARK on the page → refused
   (and B3 in Edit blinks red). Start, tap on a snare twice quickly — *listen
   for* nothing; the page shows **one** orange marker, the second tap selected it.
6. **Pads.** Grammar = Pads with a few markers: keys 0, 1, 2… play the markers
   in time order — *listen for* each marker's hit on consecutive keys. Add a
   marker before the first: key 0 now plays it. Undo once: the marker and the
   old key map both come back.
7. **Markers → keys, clear.** In Classic, KEYS_FROM_MARKERS: the keys take the
   markers — *listen for* it on the keyboard; undo restores the old keys.
   CLEAR_MARKERS: every marker gone, keys back on the grid; one undo brings all
   back.
8. **Autoslice on the panel.** Edit page (B1 held + B3 tap). P5's ring shows
   the grey markers there are (dim until you sweep through it). Turn P5 up a
   detent at a time — *listen for* (with Pads, on the keys; elsewhere on the
   page's Kit view) one more chop each detent, the earlier ones unchanged. Down
   removes the last added. Sweep to 0 and back up: a different set. B2 tap once:
   the whole sweep undone. Tapped (orange) markers survive all of it. Leave Edit:
   the Sample highlight did not move.
9. **Pads from the page.** PLAY_KEY down / up with Key Plays = Held — *listen
   for* the slice reading on until up; one-shot ignores up.
10. **Under a clock.** All of the above with a clock on J3: the stop holds the
    phase (restart on the beat), autoslice and the follow post between beats
    without a hiccup (loadMax stays under 1000).
