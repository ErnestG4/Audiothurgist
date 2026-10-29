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

**ENV (2026-10-29, branch `beta-env`)** added each key's **amp envelope**
(Attack, Decay, Sustain, Release) and **choke group** to the same settings,
and opened them from the Play tab's pads (⋯). Owns `Voice::envSt` / `env` /
`EnvStep` / `Cut` / `src` / `grp`, `Engine::keyEnv` / `EnvOf`, the group cut in
`FireSlice` and the `/* ENV */` lines beside the KEYP ones; keyp_check §7 / §8,
selftest §7g / §9i; the checklist is §10g. See "The amp envelope and choke
groups" below.

## What a key has

| | range | neutral | what it does |
|---|---|---|---|
| **Gain** | −24 … +6 dB, ½ dB steps | 0 dB | a multiply on the key's voice (one per sample, beside the envelope's) |
| **Tune** | ±24 semitones, in cents | 0 | multiplies the rate the call already computes: a recording's own rate (`OwnRate`) × P4 × the style, or a slice's tempo varispeed × P4 × the style |
| **Plays** | Follow · One-shot · Held · Loop · Ratchet | Follow | Follow = Options → Key Plays; One-shot never holds; Held reads on until the release; Loop holds and loops the key's own entry; Ratchet holds and retriggers it |
| **Ratchet rate** (Ratchet only) | the whole entry · ½ · ¼ · ⅛ of it · an 8th · a 16th · a 32nd at the tempo | whole | how often the entry starts again |
| **Attack** (ENV) | 0 … 2 s, 1 ms | 0 | a linear fade-in over that long, on top of the click-safe 1 ms ramp every hit has |
| **Decay** (ENV) | 0 … 4 s, 1 ms | 0 | after the attack, an exponential ease down to the sustain (−60 dB of the way in that time); nothing at full sustain |
| **Sustain** (ENV) | 0 dB … −59 dB, off | 0 dB | the level the decay settles at; *off* = the decay falls to silence and the voice ends there |
| **Release** (ENV) | 0 … 4 s, 1 ms | 0 | how a held key fades once it comes up (60 dB in that time, from full); 0 = the choke geometry (5 ms, or Adaptive's) as before |
| **Choke group** (ENV) | none · 1 … 4 | none | a key struck in a group cuts every other key's sound in it |

All zero is neutral: unity gain, no tune (both exactly 1.0 — no
transcendental is evaluated for 0), Plays as Options → Key Plays says, the
ratchet's tick skipped while no ratchet runs, and (ENV) no envelope — the voice
skips it on one byte compare and its output is the same bits — and no group. Setting a key and setting it back
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
                            ENV: u16 attack ms (≤ 2000), u16 decay ms (≤ 4000),
                                 u8 sustain dB below full (≤ 60; 60 = none), u16 release ms (≤ 4000),
                                 u8 choke group (0 none · 1..4),
                            u8 flags (b0 join) [, u8 gen]                        (15 field bytes)
                          → u8 status, u8 tableGen
0x69 GET_KEYS             ← (nothing)
                          → u8 status, u8 n = 24, n × { i8 gain, u8 plays, i16 cents, u8 rate,
                            u16 attack, u16 decay, u8 sustain, u16 release, u8 group }   (13 bytes a key)
```

ENV broke both formats (the descriptor's `"api"` 1 → 2, `HL.API` with it: the
page refuses the older firmware, as it refuses any other number).

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
14 B since ENV: the envelope and group are part of `KeyParam`) with the key map
and `nTable` as every record does — inside the existing union: `UndoRec` stays
2 336 B, the journal 151.9 KB. The request carries the whole `KeyParam`
(`EditReq::kp`). Undo / redo swap the
settings back and work out gain and rate again only for the keys that
changed; a ratchet whose key leaves the mode stops.

## The set JSON

```json
"keyp":[{"key":3,"gain":-6.5,"cents":700,"plays":2},{"key":7,"gain":2,"cents":-1200,"plays":4,"rat":1}]
```

ENV adds, each only when not 0: `"atk"`, `"dec"`, `"rel"` (ms), `"sus"` (dB,
negative: `-12`; `-60` = none) and `"grp"` (1..4):

```json
{"key":10,"gain":0,"cents":0,"plays":1,"dec":300,"sus":-60,"grp":1}
```

Written only for the keys that are not neutral, and only when one is (a kit
without settings has no `"keyp"`; `"rat"` only when not 0). The envelope's
fields are read clamped (times to their ranges, sustain to −60 … 0, an unknown
group → none); only the per-key block changed.
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
surface; the words are the cue, not the colour). ENV: the row's second line is
the envelope (Attack, Decay, Sustain, Release) and Choke; the words add
"A 20 ms · D 300 ms · S −12 dB · R 1.2 s · choke 2". From a Play pad: see
"The Play tab" below.

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
- **A 16ths-quantised Play punch** plays as a step, without the key's
  settings. Carrying them would need per-step gain / rate in the sequencer.
- Settings changed while a key sounds apply from its **next** call (a held or
  looping voice keeps what it started with).
- The ratchet is **block-accurate** (≤ 0.5 ms late per hit, exact on average).
- A tempo change mid-ratchet (the clock-synced rates) takes effect at the next
  press.
- The settings are not on the **panel's LEDs** (out of scope).

## The amp envelope and choke groups (ENV, 2026-10-29)

*Combust: "We need to be able to be a normal mono / stereo sampler as much as a
break slicer and rearranger … the FOSS full sample slice and playback kit we
wanted." A sampler's pads fade in, ring out when you let go, and hats choke
each other; these keys could not.*

### The envelope

On top of the click-safe fades, never instead of them: the voice's own geometry
(the 1 ms attack ramp, the edge guard, the choke fade) still multiplies the
sound; the envelope is one more factor, folded into the key's gain (one
multiply a sample, shared by both planes).

- **Attack** — linear, 0 → full over its time.
- **Decay** — after the attack, an exponential ease toward the sustain:
  `env = S + (env − S) × d`, `d` the factor that leaves −60 dB of the way after
  the decay time.
- **Sustain** — held while the key sounds. *Off* (−∞) makes the decay fall to
  silence: at −60 dB the voice ends (`kEnvEnd`: its next sample is its natural
  end, the click bridge takes the last −60 dB).
- **Release** — when a held key comes up: `env × r` a sample, 60 dB in the
  release time **from full**. It is a *rate*: from a −12 dB sustain a 500 ms
  release is quiet at 400 ms (listed as a call). At −60 dB the voice ends. The
  voice reads on meanwhile: a Held key's sample goes on under its tail, a
  **Loop key keeps looping** through its release (and, like a held loop, no
  other hit and no step chokes it then — `WouldChoke`).
- A **Decay or Release under 10 ms runs at 10 ms** (a 60 dB fall faster than
  that is a click; below it the release's 0 = the old 5 ms choke fade).
- **Release 0** is today's release: the choke geometry, 5 ms (or Adaptive's).
  So is the **ratchet's retrigger**, always: each hit still crossfades into the
  next in 5 ms (a Release there would pile every hit up in the voices); only the
  last hit, at the key's up, takes the key's Release.

Per sample: when a voice has no envelope, one byte compare (`envSt`) and the
gain as before, bit for bit — every golden and keyp_check §1 hold. With one: an
add (Attack) or a multiply (Decay: a subtract, a multiply, an add; Release: a
multiply), a compare, and the gain × envelope multiply. The coefficients are
worked out once per edit, undo or load (`Engine::EnvOf`, `exp2f`), copied into
the voice at the call — no transcendental per call or per sample. A setting
changed while a key sounds applies from its next call (as gain and tune).

### One-shots: Release does nothing — the decision

A one-shot has no "up": nothing tells it when to start releasing except its
own end, and that end already has its click-safe edge guard (or is the next
slice's attack, for a grid slice). Fading the last *R* ms of the sample would
make Release mean two different things (a tail after the key for held keys, a
fade *inside* the sample for one-shots) and would need the voice to know where
its end falls at its rate. Instead a one-shot is shaped by **Decay to no
sustain** — the drum sampler's decay knob: a hat set to 150 ms decays and stops
there. The row dims Release for a one-shot key (One-shot, or Follow with Key
Plays on One-shot) and its title says why. Listed as a call.

### Choke groups

A key struck in group *g* (1..4) **cuts every other key's voice in group g**,
wherever it is — mid-sample, held, looping, releasing — with the choke geometry
(`Voice::Cut`: the fade every steal uses, the envelope frozen meanwhile), and
stops that key's ratchet. The open / closed hat rule: open on A♯1 and closed on
B1, both in group 1, and B1 cuts A♯1.

- **A key never chokes itself** beyond what any hit already does: its own
  voices (`src == key`) go on as today — a one-shot struck twice rings twice;
  a voice near its boundary is still choked by the old rule.
- **Steps take no part** — they neither cut nor get cut. A grammar's step
  chooses a *slice*, not a key: the same slice can sit under several keys or
  none, so a key's group has no claim on it (KEYP's rule for every setting).
  Their voices carry no group (`src = −1`, `grp = 0`), and the sequencer's
  continuity branch never adopts an enveloped or grouped voice, so a key's
  sound never turns into a step's. Listed as a call (a hat *step* choking a
  grouped open hat on the pads is a different, fair want).
- Keyless calls and auditions carry no group either; a **looper replay** of a
  keyed event is a call through the key, so it chokes as played.
- A spread copies the group too: spread a grouped key and every key chokes the
  last — a **monophonic** chromatic instrument (with the root's attack and
  release, a playable mono synth voice on J4).

### Spread, sets, the journal

Everything is part of `KeyParam`, so Spread copies the envelope and group with
gain and Plays, the journal's `kKeyP` record carries them (one undo step per
edit, a drag joined into one), and the set's per-key block saves them (above).

### The Play tab

A press on a pad is a note (and a long press a held one), so the settings get a
**second target**: **⋯** in each pad's bottom-right corner, a button beside the
pad (not inside it: pressing it plays nothing; 28 px; the pad's small line
gives it room). It opens **the Kit's row itself** in a sheet for that key: the
row element is moved into the sheet and back (`kp_home` ↔ `kp_slot`) and
pointed at the sheet's key (`kpK`) — one component, no duplicated controls; the
Spread button (an all-keys operation with its own prompt on the Kit's
waveform) stays in the Kit. The sheet is not modal: the pads play under it (set
a release, hit the pad), ⋯ on another pad retargets it, the edited pad is
ringed; ✕, **Esc** or another tab put the row back under the Kit's keys and
focus back on the ⋯ that opened it (it opens with focus on Gain). On a wide
screen it sits at the bottom, ≤ 780 px; on a phone it spans the width above the
transport, 62 vh, scrolling. **No long-press**: on a pad a long press is
already a held note, and Held / Loop / Ratchet keys sound for as long as you
hold — a long-press menu would fight them.

The time sliders run on a cubic curve (`HL.envMs` / `envPos`: position 10 →
2 ms of 2 s, 50 → ⅛ of the maximum); a slider left where the key's value puts
it sends that value exactly, so a set's 1 234 ms release survives a gain edit.

### Cost

- **Audio**: per voice per sample, with no envelope one byte compare (and the
  `ended` test's extra compare); with one ≈ 4–6 more operations (the stage
  branch, one add or multiply, a compare, the gain × envelope multiply) — at
  most 8 voices × 24 samples × ~8 cycles ≈ 1.5 k cycles a block of 240 000,
  under 1 % with all eight voices enveloped, ~0 without. The group cut is a
  loop over 8 voices at a grouped key's call. Not measured on the module
  (§10g.11).
- **Memory** (release build): SRAM 469 184 → 470 848 B (+1 664, 95.46 →
  95.79 %): the voice reader +358 B (mono and stereo), `FireSlice` +204, `Cut`
  +268 and `EnvOf` +244 (both `BB_COLD`), the rest the edit, set and wire code
  (`HandleEdit` forced inline — the new fields had pushed GCC to put it out of
  line, +0.3 KB); the manual's sentence +224 B more: **471 072 B, 95.84 %** in
  all (+1 888 B). DTCM +880 B (`keyEnv`, the voices' envelope, the settings'
  new bytes). SDRAM unchanged; `tools/sdramcheck.sh` passes. If squeezed: Decay + Sustain (~200 B) and the dims are the first
  to go; then `Cut` could share `Release`'s fade.

### Tests

- keyp_check **§7** (the envelope: the coefficients; 50 ms into a 100 ms attack
  half way and done at 100 ms ± a block; the decay −60 dB of the way at its
  time; the sustain's level at the output; −30 dB half way through the release
  and silent at its rate's end; no click — the worst step under the tone's own;
  a key without a Release fading as ever; a one-shot's decay to no sustain
  ending it at 300 ms of a 2 s tone and its Release shaping nothing; Loop
  looping through its release untouched by another hit and the steps; Ratchet
  never more than two voices; every key enveloped and grouped leaving the
  steps' bits alone; the refusals, a drag one step, undo working the
  coefficients out again) and **§8** (choke groups: two open hats cut within
  the 240-sample choke time by the closed hat, a fade under the tones' own
  slope; another group and the key itself never; held, looping, releasing and
  ratcheting keys all cut and the ratchet stopped; the steps' voices groupless,
  a grouped Loop key surviving a second of steps; the spread copying envelope
  and group and its keys choking each other within the choke time at their
  rate). §5's round trip now carries envelopes and a group and still renders
  the same bits; odd JSON values clamped. **186** checks; checked to bite
  (seven mutations: no cut, the ratchet releasing with the envelope, a
  releasing loop chokable, no envelope multiply, no envelope release, a key
  cutting itself, no end at −60 dB).
- selftest **§7g** (the protocol over the fake: 13 bytes a key, a full
  envelope read back, the words, the curve, the refusals, the old 7-byte body
  refused, undo, a release drag one step, the fake's sound — a held key ringing
  for its release, a group cut, another group and the key itself spared, a
  one-shot's decay —, a spread, a set) and **§9i** (the page through real
  events: the envelope line, a release drag with JOIN, sustain, decay, group,
  the exact-value rule, the dims; ⋯ not a note, the sheet on its key with the
  row moved, an edit landing on that key and not the Kit's selection, a pad
  played under it, retargeting, Esc / ✕ / another tab / a disconnect). 770
  checks.

### Deferred

- The envelope is not on the panel or its LEDs (as KEYP).
- A one-shot's Release (above), and steps in choke groups (above) — calls.
- No velocity: J4/J6 carry no velocity, so the envelope has no velocity
  amount.
- No per-key filter, pan or pitch envelope.
