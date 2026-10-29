# KEYP package — each key's own gain, tune and Plays (2026-10-26, branch `beta-keyparams`)

*Combust, 2026-10-25: "We need to be able to be a normal mono / stereo sampler
as much as a break slicer and rearranger." And later: "finishing out the modes
for playback :) maybe actual ratchets at like full sample, half sample. Up to
you." What a sampler has and this lacked was per-pad settings: how a key
played was one global option (Options → Key Plays).*

Owns `KeyParam`, `Engine::keyp` / `keyGain` / `keyRate` / `KeyHeld` /
`LoopHeld` / the ratchet (`RatchetStart`, `RatchetTick`), `EditOp::SetKeyParams`
and the journal's `kKeyP` record in `core/bb_engine.h` + `bb_edit.h` +
`bb_undo.h`; the set JSON's `"keyp"` in `core/bb_set.h` / `bb_sets.h`; the
`/* KEYP */` lines of `src/audiothurgist.cpp` (ACTION 37, 0x69, the
descriptor, the loads, the release rule); `tests/keyp_check.cpp` (new); the
KEYP block of `web/hostlink.js`, the Kit's settings row and the words on the
cells and pads in `web/index.html`, the fake's settings in `web/demo.js`,
selftest §7e / §9f. **Nothing here has run on hardware**; the checklist is
`docs/hardware-pass-beta.md` §10e.

## What a key has

| | range | neutral | what it does |
|---|---|---|---|
| **Gain** | −24 … +6 dB, ½ dB steps | 0 dB | a multiply on the key's voice (one per sample, beside the envelope's) |
| **Tune** | ±24 semitones, in cents | 0 | multiplies the rate the call already computes: a recording's own rate (`OwnRate`) × P4 × the style, or a slice's tempo varispeed × P4 × the style |
| **Plays** | Follow · One-shot · Held · Loop · Ratchet | Follow | Follow = Options → Key Plays; One-shot never holds; Held reads on until the release; Loop holds and loops the key's own entry; Ratchet holds and retriggers it |
| **Ratchet rate** (Ratchet only) | the whole entry · ½ · ¼ · ⅛ of it · an 8th · a 16th · a 32nd at the tempo | whole | how often the entry starts again |

All zero is neutral: unity gain, no tune (both exactly 1.0 — no
transcendental is evaluated for 0), Plays as Options → Key Plays says, and the
ratchet's tick skipped while no ratchet runs. Setting a key and setting it back
leaves nothing behind: `keyp_check` §1 renders a rich session (a stereo break
in Classic with the Zoom tail pitched up, adaptive fades, two pads, one-shot /
held / reversed / ratcheted / staccato calls, then Play's looper recording and
replaying keyed calls) with every key set away and back to neutral through the
edit op, and gets the untouched session's bits. (The package was first built
to hash an untouched session to the pre-KEYP golden; that constraint is gone —
see Loop below.)

## Where the settings apply — the decision

**Every call made through a key, and nothing else.** That is: the CV keys (J4
in both jack maps), the page's pads (PLAY_KEY), the ratchet style's 32nd
double, and **a looper replay of a keyed event** (Play's loop) — which takes
the key's settings **as they are now**, not as they were when it was played,
the same way the replay already resolves the key's entry through the key
table now. Never a grammar's steps (Classic, Collins, Grids… choosing slices),
a keyless call (the page's slice click, a keyless looper event), the Edit
page's audition, or a step written by a 16ths-quantised Play punch.

Why: a sampler player sets "the snare pad −4 dB, the 808 down a fifth" and
expects that pad to sound that way however it is struck — by hand, by CV, or
by the loop that recorded it. The grammars rearrange the *break*: their
steps choose slices, not keys, and the same slice may sit under several keys
(or none), so a key's settings have no claim on a step. A step that would
otherwise run on in a key's gained voice (the continuity branch) starts a
voice of its own instead. A 16ths punch becomes a step of the bar (it already
plays at the step's varispeed, as `OwnRate`'s rule says), so it is plain too —
listed below as a call to flip.

The settings belong to the **key**, not to what it plays: re-pointing,
swapping or copying keys, dropping a new sample on a pad, recording into it
or clearing it all leave them where they are. New material (a loose break,
a loop take, New empty kit) resets every key to neutral; a re-slice of the
same planes keeps them; a set brings its own back.

## Plays

- **Follow** — Options → Key Plays decides (one-shot, or held until the gate
  falls), as before.
- **One-shot / Held** — the key's own answer, whatever the option says.
  `Engine::KeyHeld(key, option)` resolves it inside `CallSlice`, so Play's
  looper records what the call became; the shell asks the same question for
  the Brain map's gate fall and PLAY_KEY's up.
- **Loop** — held, the key's own entry looping (the tail mechanism: loop
  forward over `[start, end − 2 − (rate + 2))`, the wrap bridged by the
  click compensator) until the release, which fades it over the release's
  full length (the choke's, counted in output samples) while it goes on
  looping — a release next to the loop point is never cut short by the entry's
  end.
  **Reversed** (J5's reverse style, or a looper event recorded reversed) it
  loops **backward**: from the entry's top down to `start + (rate + 2)`, then
  back up to the top (`Voice::kTailBack`, the same click bridge — the test's
  worst sample-to-sample step at the seam is 0.022, the tone's own slope); the
  release fades it the same way, still looping backward. Another key's hit or a
  step never chokes it (`WouldChoke` is false for a held loop); the same key
  struck again while it loops (J5's restrike, a looper replay) fades its own
  previous loop under the new one — one loop per key, never two stacked. An entry too
  short for its rate's loop (a grid slice under ~100 samples at 16×) plays as
  plain Held.
- **Ratchet** — held, the entry starting again from its start every period:
  its own length as it plays (length ÷ |rate|, so tune and P4 shorten it) ×
  1, ½, ¼ or ⅛, or a note value at the tempo (8th = two grid steps, 16th =
  one, 32nd = half), never under 5 ms. Each retrigger is a `FireSlice` with
  the key's gain: the previous hit is released with the choke geometry while
  the next one attacks beside it — a crossfade, not a step (the test's worst
  sample-to-sample step is 0.024, the tone's own slope; a hard cut at the
  retrigger gives 0.080 there and fails it). It is worked
  out at the call and at each retrigger, never per sample, and it is
  block-accurate: the countdown carries the remainder, so the period is exact
  on average and each hit lands at most one 24-sample block (0.5 ms) late.
  It stops on the release (the gate's fall, the pad's up, the looper's
  release event), when the key leaves the mode (an edit or its undo), or when the key
  plays something else (any re-point — SET_KEY, a swap, a key map, a spread, a drop into its pad). Each
  hit counts as a call (J7's hit trigger, the gates).
  - **With no gate to go by** — a looper replay of an event that was recorded
    without a hold (e.g. played while the key was One-shot, the key set to
    Ratchet after) — the ratchet **fills the step**: as many hits as fit one
    grid step (a 16th) at the tempo, at least two, then it stops by itself.
  - A held ratchet with no release at all (a release the looper's full list
    could not record) gives up after **eight bars**.
  - The clock-synced rates fell out cheaply (the grid step is at hand at the
    call), so they are in. They follow the tempo at the call; a tempo change
    mid-ratchet takes effect at the next press.

## Protocol — ACTION 37 and `0x69 GET_KEYS`

The state carries no copy of the settings: the page reads 0x69 whenever the
state's `tableGen` (byte 255) moves, and every KEY_PARAMS, undo, redo and load
moves it.

```
0x63 ACTION 37 KEY_PARAMS ← u8 key 0..23, i8 gain (½ dB, −48 … +12), i16 cents (±2400),
                            u8 plays (0 follow · 1 one-shot · 2 held · 3 loop · 4 ratchet),
                            u8 rate (0 the entry · 1 ½ · 2 ¼ · 3 ⅛ · 4 an 8th · 5 a 16th · 6 a 32nd),
                            u8 flags (b0 join) [, u8 gen]
                          → u8 status, u8 tableGen
0x69 GET_KEYS             ← (nothing)
                          → u8 status, u8 n = 24, n × { i8 gain, u8 plays, i16 cents, u8 rate }   (5 bytes a key)
```

- KEY_PARAMS is a table op (`HandleEdit`): the edit mailbox, the same
  BAD_STATE rules as every edit (a take armed or running, a set op, a load,
  a drop committing, a stale gen), BAD_ARGS for any value out of range or an
  unknown flag bit. One undo step.
- **join** (b0): every send of one slider drag after the first. The journal
  folds it into the previous step only when that step is *this key's*
  settings and nothing else has moved the table since (the newest record is a
  `kKeyP` record for this key, the cursor at the end, the generation live);
  otherwise it is a step of its own — never glued onto an AUTOSLICE, a
  marker or another key's drag. The autoslice sweep's pattern, made strict.
- ACTION 38 SPREAD_KEYS (CHROMA, `docs/packages/chroma.md`) puts one key's
  sample on all 24 keys, a semitone a key, written through these same
  settings and journaled in the same `kKeyP` record (a join never folds into
  a spread's record).

## The journal

`EditOp::SetKeyParams` captures a `kKeyP` record — every key's settings (24 ×
6 B) with the key map and `nTable` as every record does — inside the existing
union: `UndoRec` stays 2 336 B, the journal 151.9 KB. Undo / redo swap the
settings back and work out gain and rate again only for the keys that
changed; a ratchet whose key leaves the mode stops.

## The set JSON

```json
"keyp":[{"key":3,"gain":-6.5,"cents":700,"plays":2},{"key":7,"gain":2,"cents":-1200,"plays":4,"rat":1}]
```

Written only for the keys that are not neutral, and only when one is (a kit
without settings has no `"keyp"`; `"rat"` only when not 0).
Read tolerantly: a key outside 0..23 (or none) skipped, values clamped to
their ranges (an unknown plays → Follow, an unknown rate → the entry), dB
rounded to ½ dB, unknown fields skipped, a later entry for the same key wins.
A file without `"keyp"` loads neutral; a reader that does not know a key skips
it whole (the reader's `Skip`, tested by renaming it). Still schema 1. The autosave
writes it (a settings edit moves the generation, which arms it), and so does
the loose file's sidecar; the load applies it after the key table.

## The page

Kit tab: a row under the 24 keys for the **selected key** — Gain (slider, ½ dB),
Tune (semitones slider + cents), Plays (and, in Ratchet, its rate), **▶ hear**
(held while down, so Held / Loop / Ratchet can be heard) and **reset**. The
sliders play live, one request at a time, every send of a drag after its first
with JOIN: one undo step, one history line ("C♯1 settings: −6 dB · ratchet
1/16"). A key cell — and a Play pad — with settings shows them in words, in the
accent's ink (orange = "yours", as a placed marker; no new hue, ≥ 4.5:1 on every
surface; the words are the cue, not the colour). Editing from a Play pad is not offered: a pad's
press is a note (a long press is a held one), so the pads only show the words
and their title points at the Kit.

## Cost

- **Audio thread**: one more multiply per voice per sample (two in stereo) —
  at most 8 voices × 24 samples × 2 = 384 multiplies a block of 240 000
  cycles, under 2 ‰ on the tail's loadAvg / loadMax even with all eight
  voices sounding, ~0 with one. The tune is one multiply per call; Plays is a
  byte compare per call; the ratchet is a mask test per block while none runs
  (skipped), and a countdown per running key plus one `FireSlice` per
  retrigger (the cost of any key call). `exp2f` runs only on an edit, an undo
  of one, or a load — never per call. Not measured on the module (§10e asks
  for the CPU meter with eight voices and four ratchets).
- **Memory** (release build, the same SDK): SRAM 461 376 → 467 072 B (93.87 →
  95.03 %: +5.7 KB of code, about 2 KB of it new code and the rest GCC
  re-deciding what it inlines in the set code — the once-per-load helpers are
  out of line and built for size, `BB_COLD`); DTCM +984 B (the engine's 24
  settings, gains and rates, the voices' gain, the 24 ratchet states);
  SDRAM +128 B (the set info). `tools/sdramcheck.sh` passes.

## Tests

- `tests/keyp_check.cpp` (**138** with CHROMA's §6, ASan/UBSan, in
  `tests/run.sh`): §1 every key set away and back to neutral renders the
  untouched session's bits; §2 gain and tune at the call (a pad's own rate × P4 ×
  tune, a grid key's varispeed × P4 × tune, the style and reverse, keyless
  calls / auditions / neutral keys unity, the ratchet double, the output
  scaled exactly, every key set while the steps render the neutral bits, a
  step never adopting a gained voice, a looper replay taking the settings
  set after it was played); §3 Plays (Follow, One-shot over the option, Held
  under it, Loop looping three lengths inside its entry, unchoked by another
  hit and the steps, fading on the release, at rate 16 still inside, the
  forward seam bridged; reversed: looping backward from the top for three
  lengths, at rate 16 too, its seam bridged, its release; too short → Held;
  the looper recording the resolved hold); §3b Ratchet
  (the periods for every rate and with the tune, evenly spaced, no click, one
  hit sounding, the release, the gain on each hit, leaving the mode and its
  undo stop it, filling the step with no gate, the eight-bar cap, a looper
  replay ratcheting until its release); §4 the edit op (one step, the
  refusals, stale, undo / redo, a drag one step, a join for another key / after
  an undo / behind a marker its own step, the key map untouched, the settings
  staying with the key through a drop / clear / swap, a re-slice keeps, new
  material resets, wholesale with clamps, `UndoRec` unchanged); §5 the JSON
  (absent when neutral, exact round trip incl. a ratchet, an old reader
  skipping it, odd values, ½ dB rounding, an engine → set → engine round trip
  rendering every key's bits the same). **Checked to bite**: the gain
  multiply, Plays' resolution, the tune, the continuity guard, the loop's
  choke guard and margin, the backward wrap, a hard cut at the ratchet's
  retrigger, its tick and its stop on release — each removed fails checks.
- `web/selftest.mjs` §7e / §9g: the protocol over the fake and the page
  through real events (above).

## Known / deferred

- **The panel**: no per-key settings on the module itself (deferred: P1–P6 are
  all spoken for on the Edit page, and a key-settings page is its own
  design).
- Editing from a **Play pad** (deferred; see the page).
- **A 16ths-quantised Play punch** plays as a step, without the key's
  settings. Carrying them would need per-step gain / rate in the sequencer.
- Settings changed while a key sounds apply from its **next** call (a held or
  looping voice keeps what it started with).
- The ratchet is **block-accurate** (≤ 0.5 ms late per hit, exact on average).
- A tempo change mid-ratchet (the clock-synced rates) takes effect at the next
  press.
- The settings are not on the **panel's LEDs** (out of scope).
