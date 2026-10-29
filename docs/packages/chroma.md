# CHROMA package — one sample, played chromatically (2026-10-26, branch `beta-chromatic`)

*Combust, 2026-10-25: "We need to be able to be a normal mono / stereo sampler
as much as a break slicer and rearranger." The KEYP work gave each key its own
gain, tune and Plays; what a mono sampler does that this still could not was
the standard thing — J4's V/oct playing **one** sample at 24 pitches. CHROMA
adds it as one explicit edit, so nothing changes until you ask.*

Owns `EditOp::SpreadKeys`, `Engine::SpreadPlan` / `EdSpreadKeys` /
`kSpreadKeepPads` in `core/bb_engine.h` + `core/bb_edit.h` (and two lines of
`core/bb_undo.h`); the `/* CHROMA */` lines of `src/audiothurgist.cpp`
(ACTION 38, `HandleSpread`); keyp_check §6; the
CHROMA block of `web/hostlink.js`, the Kit's Spread button and its prompt in
`web/index.html`, `Editor.spreadKeys` and the history label in `web/edit.js`,
the fake's spread in `web/demo.js`, selftest §7f / §9h. **Nothing here has run
on hardware**; the checklist is `docs/hardware-pass-beta.md` §10f.

## What it does

**Spread [root] chromatically (C1 … B2)** — the last button of the Kit's
settings row, acting on the selected key, the *root* k:

- every key j ≠ k plays **k's entry** (the same sample, the same span);
- j's tune = **(j − k) × 100 cents + k's own cents**;
- j takes **k's gain and Plays** (and, in Ratchet, its rate; since ENV, its
  envelope and choke group too — a grouped root makes a monophonic spread);
- **k itself is untouched** — it stays the reference.

So with a sample on G1, C1 plays it 7 semitones down and B2 16 up, and J4's
V/oct (0 V = C1, a semitone a key, `docs/beta-plan-2026-10-14.md` "One CV
keymap") plays a melody from it. A spread is a snapshot, not a mode: change
the root's settings afterwards and only the root changes; spread again to
re-apply (a spread already in place moves nothing and adds no undo step).

**The range.** ±24 semitones covers 23 keys either way, so any root fits — C1
spreads to +23 st, B2 to −23 st. Only the root's own cents can push a far key
past ±2400; it is clamped there (a root at +5 st: keys 19..23 all at +24 st).
keyp_check proves both ends and the clamp.

## Pitch and tempo — why a pad is in tune at any tempo

A call's rate is what it already was, × the key's tune (`docs/packages/keyp.md`):
a **recording or a dropped pad** plays at its own rate × P4 × the style × the
tune (`Engine::OwnRate`) — no tempo in it — so a spread pad is in tune at 90
and at 170 BPM alike. A **slice of the break** plays at the tempo's varispeed
× P4 × the style × the tune, so a spread slice is still chromatic (the ratios
between keys are exact) but the whole spread follows the tempo, as every
slice of the break always has. Allowed, and said in the README and the
manual. keyp_check §6 checks key j's rate = 2^((j − k)/12) × the root's at 90
and 170 BPM (worst relative error ≤ 2e-6), that the pad's rate is the same at
both tempi, and that a spread grid slice keeps its octave at both while
rising by 170/90.

## Keys that hold their own pads — the decision

A spread re-points keys. A key that plays a recording of its own (a pad: a
drop, a take, a loaded set's recording) would lose it: the recording stays in
the table, owned by its key and held in memory, but nothing plays it. That is
the same situation as the key-drop rule's red case ("that key is the only one
playing a recording — replacing orphans it"), and the project's answer there
is a prompt, red, with a non-destructive default. CHROMA follows it with the
same popover (`showChoice`, which now takes a title and its own labels):

> **2 keys play a pad of their own (G1, D2)** — spreading D♯1 over them
> leaves those samples unplayed (still in the kit; Undo brings them back)
> · **spread around them (keep G1, D2)** — the default
> · spread over them too (all 24 keys)
> · cancel

- **Around** sends `keep pads` (flags b0): the firmware leaves every key whose
  entry is a recording other than the root's exactly as it is — map and
  settings. The firmware decides which keys those are (`SpreadPlan`), not the
  page, so the rule cannot race a drop.
- **Over** spreads all 23. The replaced recordings stay where they were
  (`tableKey` still names their key; dropping or recording into that key later
  reuses the slot, as ever); **Undo** brings them back on their keys, and a set
  saved in that state keeps them in the kit (keyp_check's set round trip).
- No prompt when nothing would be lost: keys on the break, on keyless
  markers, empty keys, and keys already playing the root's entry are spread
  without asking (markers and slices are not lost — they stay in the
  waveform).
- An **empty kit's key** (New empty kit, nothing dropped on it) cannot be
  spread: the button is off and its title says to drop a sample first.

Why "around" is the default: it never makes a sound unreachable, and a
sampler player who wants a melodic key *and* a drum pad or two (the usual
case) gets it in one press. *Over* is one click away for "make this a
one-sample instrument". This is a call for the bench (§10f.6, Calls 8).

## Protocol — ACTION 38

```
0x63 ACTION 38 SPREAD_KEYS ← u8 root 0..23, u8 flags (b0 keep pads) [, u8 gen]
                           → u8 status, u8 tableGen, u8 count (keys now on the root's entry, besides it)
```

- A table op through `HandleEdit` (its body `HandleSpread`, out of line): the
  edit mailbox, the same BAD_STATE rules as every edit (a take armed or
  running, a set op, a load, a drop committing, a stale gen); BAD_ARGS for a
  root outside 0..23, an unknown flag bit or a short body.
- The shell **plans first** (`Engine::SpreadPlan`, pure, on the control
  thread with no edit in flight): a spread that changes nothing answers OK
  with the same tableGen and posts nothing — no empty undo step
  (KEYS_FROM_MARKERS' rule). A stale gen is BAD_STATE even then.
- No bytes added to the 0x60 state: the spread moves `tableGen`, and the page
  reads the map (0x60) and the settings (0x69) again, as for KEY_PARAMS.

## The journal

One undo step, one record: `EditOp::SpreadKeys` captures the existing `kKeyP`
record — every key's settings (24 × 6 B) with the whole key map and `nTable`,
as every record carries — so `UndoSwap` puts back all 24 keys' map and
settings in one go and works out gain and rate again only for the keys that
changed. **`UndoRec` does not grow** (2 336 B, the union's `kp` member is
144 B), the journal stays 151.9 KB of SDRAM. Two guards:

- KEY_PARAMS' **join** (a slider drag) folds only into a record whose op is
  SetKeyParams for the same key — never into a spread's record, though both
  are `kKeyP` with the root in `first` (`UndoQuiet` checks `op`). A drag of
  the root's gain right after a spread is its own step.
- A key that a spread **or its undo** re-points stops a running ratchet (it
  was retriggering the old entry); before, `UndoSwap` stopped only keys that
  left the Ratchet mode.

## Interactions

- **Pads mode with markers**: a spread is a hand-made key map, so it follows
  SET_KEY / SWAP_KEYS' rule — it stands until the markers next change, and
  then the keys follow the markers again (`PadsFollow`; keys holding a
  recording keep theirs). The tunes stay with the keys. Calls 10.
- **Settings belong to the key** (KEYP's rule): dropping a new sample on a
  spread key, recording into it or clearing it leaves its tune. Dropping onto
  the **root** replaces the sample every spread key plays (they all point at
  the root's entry) — a quick way to audition a new sound as an instrument.
- **Sets**: the map and the 24 settings save and load as they are (the
  `"keys"` map and `"keyp"`); nothing new in the JSON.
- **New material** resets every key's settings (KEYP) and the map (as ever):
  a spread does not survive loading a loose break.

## The page

The Kit's settings row ends with **Spread G1 chromatically (C1 … B2)** (the
root named; off while a take runs or the link is
busy, off for an empty kit's key). The prompt above; then the cells and the
Play pads say each key's tune in the accent's words the keyp work draws
("−7 st", "+2 dB · +4 st · ratchet ½"), and the history says "G1 spread
chromatically (23 keys)" or "(21 keys, pads kept)". All 24 cells pulse while
the root's entry is selected — they all play it.

## Cost

- **Audio thread**: nothing per sample or per call. The edit itself runs once
  at block start: 24 compares, up to 23 `PutKeyParam` (two `exp2f` each) —
  the same order as an undo of a 24-key settings record.
- **Memory** (release build, the same SDK): SRAM 469 056 → 469 664 B (95.43 →
  95.55 %, +608 B): `SpreadPlan` 300 B, `EdSpreadKeys` 144 B, `HandleSpread`
  340 B (all `BB_COLD`, built for size), `UndoSwap` +72 B, the manual's
  sentence +96 B, minus what GCC shuffled elsewhere. Kept inside `HandleEdit`
  the spread's case stopped GCC inlining `HandleEdit` into the frame handler
  (+1.2 KB), hence the helper. DTCM and SDRAM unchanged; `tools/sdramcheck.sh`
  passes.

## Tests

- `tests/keyp_check.cpp` §6 (98 → **134**, ASan/UBSan, in `tests/run.sh`): the
  map and every tune / gain / Plays, the root untouched, the replaced pad
  still owned in the table, the rates at two tempi (above), a second spread
  planning exactly what is there, Undo / Redo bit for bit (map, settings,
  gain and rate of all 24), a join after a spread its own step, keep pads (a
  pad kept, a grid key with settings spread, a key already on the root's pad
  spread), both ends and the clamp, a spread grid slice following the tempo,
  a running ratchet stopped by the spread and by its undo, the refusals (no
  write, no step, Stale), `UndoRec` unchanged, and a set round trip of a
  spread over a pad. **Checked to bite**: dropping the root's cents, the
  ratchet stop in the op and in the undo, the join guard, the journal capture
  and the keep-pads exception each fail checks.
- `web/selftest.mjs` §7f (the protocol over the fake) and §9h (the page through
  real clicks: the prompt and each answer, the words on the cells and the
  pads, the history line, Undo, the no-prompt path, the empty kit) — 760 →
  **792** then; checked to bite (the cents rule in the fake, the prompt
  skipped, the no-op, keep ignored).
  Looked at in Chromium at 390 and 1100 px, light and dark: no console
  errors, no horizontal scroll.

## Known and deferred

- **The panel**: no gesture. Every Edit-page knob and button is taken, and the
  panel has no "selected key" to be the root (its selection is an entry). A
  gesture would need a key-settings page of its own (KEYP's deferral too).
- **A spread is a snapshot**: the root's later changes do not follow to the
  other keys (spread again). A live "instrument" mode tied to the root would be
  its own design.
- **Only all 24 keys**: no sub-range (C1 … B1, say) — keep pads is the only
  way to hold keys out.
- **The far ends** (±23 st) are as the engine's interpolation makes them;
  §10f.9 asks how they sound.
- A grid key's cell on the last spread key (B2) reads "slices 9–31": the
  cells' span label (an empty key plays its span up to the next grid key)
  predates the spread and does not know the keys above all moved.
