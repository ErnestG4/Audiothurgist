# Audiothurgist — Specification v0.1

**Target:** Hermetic Modular Alchemy Lab v2 (12HP, Daisy Seed 2 DFM)
**Lineage:** the final form of BreakFix. Same core, new body, three new senses.
**Date filed:** 2026-09-04 · **Status:** in refinement — v1 freeze awaits
hands-on time with the arrived hardware and its shipped firmware

> *Sample exploration toolkit. Stochastically slice and rearrange, call
> individual slices and modulate slicing behavior on the fly via CV. Record
> samples from input or load them from elsewhere. Sliced live and on time.
> If you don't like it, re-slice it.*

## 0. What this is

A sampling, slicing, and rearranging toolkit. **Not a granular engine** — no
grains, no time-stretch, no spectral anything. Repitch is period-correct
forever. Every sound the module makes is a slice of real material, played
from its attack, on the grid. That boundary is the identity; scope arguments
end here.

BreakFix proved the interaction model: patterns hold, dice advance, history
rescues, one knob means one thing. Audiothurgist keeps that soul and gains
what only Eurorack can give: the rack talks to it, it talks back, and the
panel finally *shows* what it's thinking.

## 1. Architecture rule (non-negotiable, inherited)

The core stays pure: header-only C++, no SDK types, no allocation or printf
in the audio path. `shells/alchemy/` is a **new main()** written against the
Alchemy SDK Framework (VirtualKnob/Pager/ParamLock/Presets/MusicalClock/
LedRender/CvMatrix). The desktop shell and test harnesses keep working; the
SDK's host-native preset joins them. What the Pod build taught us is law
(see DEVNOTES.md, especially the DMA-reachability and boundary-knife-edge
rules — the SDK's own SdCard header confirms the former independently).

## 2. Three roles, one engine — NOT modes

The central design decision: the roles are **concurrent capabilities of one
running engine**, not switched modes.

| Role | What it does | Interface |
|---|---|---|
| **Chopper** | BreakFix: slice, rearrange, reroll, flow, history, tempo | pots + buttons |
| **Voice** | The rack plays slices: v/oct selects, trigger fires, CV shapes | jacks in |
| **Brain** | The pattern drives the rack: per-class gates, clock, index CV | jacks out |

The pattern engine always runs; Brain outputs always reflect it. Voice calls
inject into the same voice pool and the same choke fabric as pattern steps
(policy in §6.3). Chopper controls are always live. Nothing needs a mode to
exist — **modes are for workflows, not for roles** (§5).

Rationale: mode-switching between roles would make patching fragile (a CV
patch that dies when you change modes is a broken promise). A patch made in
any role must keep working while you perform in another.

## 3. Control surface

### 3.1 Pots — two pages (SDK Pager on B1)

**Page PERFORM** (boot default — this *is* BreakFix, plus two dimensions):

| Pot | Function | Notes |
|---|---|---|
| P1 | **Chop** — the journey: verbatim → edits → chops → rolls → cluster | the knob; protected zones carry over |
| P2 | **Tempo** — internal BPM 70–180, whole-BPM, native detent | shows sync state when ext-clocked (§6.4) |
| P3 | **History** — the timeline dial: 16 slots, CCW = older, full CW = now | ring fills as rolls accumulate; §3.5 |
| P4 | **Pitch** — slice repitch ±12 semi, center detent | per-step anchoring makes this playable now |
| P5 | **Space** — rest/fill density: gaps ↔ ticks ↔ crowded | the cluster-zone space controls, promoted |
| P6 | **Mix** — output level + reverb send tail | reverb no longer chained to P1's zone |

**Page BRAIN** (the pattern-language console):

| Pot | Function |
|---|---|
| P1 | **Language** — morph Brocot ↔ Grids ↔ Euclid (§6.2) |
| P2 | **Map X** (Grids) / rotation (Euclid) / cell bias (Brocot) |
| P3 | **Map Y** / — / swap wildness |
| P4 | **Kick density** |
| P5 | **Snare density** |
| P6 | **Hat density** |

Per-language reinterpretation of P2/P3 follows the SDK's per-page
VirtualKnob pattern. Pot-catch everywhere; pickup-on-nudge carried over.

### 3.2 Buttons

| Button | Tap | Hold |
|---|---|---|
| B1 | page toggle (PERFORM ↔ BRAIN) | + pot turn = **ParamLock** automation lane (SDK gesture, free) |
| B2 | **refill** (dice) | **flow** (new fill per bar) |
| B3 | **reslice** / in LIVE-SLICE: drop marker (alternative to the trigger jack, §5.2) | enter/exit LIVE-SLICE (§5.2) |

History lives on P3 (§3.5) — the protected dice-rescue behavior gets its
own dial. Slice-table resolution demotes to Settings (grid 16ths default,
8…64 selectable); it earned no performance pot.

### 3.3 Jacks — field-programmable, preset-defined

J1/J2 audio in (record/resample), J9/J10 stereo audio out. J3–J8 are the six
programmable jacks; **the map is part of the preset**, edited via HostLink
or settings. Ship three named maps:

**HYBRID (default)** — a taste of every role:

| Jack | Dir | Signal |
|---|---|---|
| J3 | in | clock (24 PPQN; divider configurable) |
| J4 | in | v/oct slice select |
| J5 | in | trigger — fire selected slice |
| J6 | in | chop CV (sums into P1) |
| J7 | out | kick gate |
| J8 | out | snare gate |

**BRAIN** — sequencer for the rack: J3 clock in; J4 reroll trigger in;
J5 kick, J6 snare, J7 hat gates out; J8 slice-index CV out (tracks the
playhead's current source position — another module can follow the chop).

**VOICE** — the keyboard instrument (§6.3): J3 v/oct + J4 gate (slices;
in MONO this pair carries the whole keymap), J5 v/oct + J6 gate (SPLIT
modifiers; in MONO these become speed CV + decay CV instead), J7 speed CV
(SPLIT) / reverse gate (MONO), J8 end-of-slice trigger out.

### 3.4 Rings — the panel finally speaks

The display language (all class colors consistent everywhere:
kick / snare / hat / other = four fixed hues):

| Ring | Shows |
|---|---|
| P1 | **THE BAR** — 16 steps in class colors, playhead pip sweeping, rests dim, burst cores bright, voice-called slices flash white |
| P2 | beat pip + clock-source state (internal / following / lost) |
| P3 | **the timeline** — 16 history slots filling as rolls accumulate; selection pip, head marked distinctly (in LIVE-SLICE this ring shows markers landing instead) |
| P4 | pitch, bipolar around center detent |
| P5 | space/fill level |
| P6 | output meter (live), reverb tail shimmer |

BRAIN page recolors: P1 language selector, P2/P3 map position, P4–P6
density arcs per class in that class's color. Buttons carry state color
(recording red, flow pulse, live-slice amber).

### 3.5 The history dial (P3)

Sixteen pattern slots, sixteen LEDs — the ring *is* the timeline. Every
generated pattern (tap, reslice, each bar of flow) appends at the head; the
ring fills until all 16 slots glow. Turning P3 scrubs: CCW into the past,
full CW is now; landing on a slot regenerates it deterministically from its
stored seeds. New rolls always append and play regardless of the dial's
position — the dial goes stale and re-arms by pickup-on-nudge (the CatchPot
mechanics already solve the interaction). Selection is a 16-zone selector
over the full ring; empty slots dark, filled dim, selected bright, head in
its own color.

**Deferred (v1.x): timeline curation** — remove a slot, pin a keeper,
insert. Not an early feature, but it is where the history ring grows into a
pattern *bank*, and the natural home for the no-wrong-notes motif memory:
canonized mistakes get pinned here.

## 4. Sound sources

1. **SD load** — WAVs from card, conditioned exactly as BreakFix does
   (trim, normalize, 4-bar cap, guarded bar-snap). Browse via P3-ring +
   buttons or HostLink; the blind-browse era is over.
2. **Record** — J1/J2, plus self-resampling (input + own output), the
   no-cables B→A loop carried over. Record is clocked: armed record starts
   and ends on bar boundaries when the clock runs (records "on time").
3. **Boot break** — the synthesized demo remains; the module always wakes
   up making sound.

## 5. Workflow modes (few, shallow, all return to PERFORM)

### 5.1 RECORD
As BreakFix, plus bar-quantized punch-in/out when clocked. Auto-analyze,
auto-return.

### 5.2 LIVE-SLICE — the on-the-fly mode (new)
Hold B3 to enter. Material keeps playing (or recording — slicing WHILE
sampling is the point). **The primary slicing method is the trigger jack**:
every rising edge on the trigger input drops a slice marker at the playhead
(optionally quantized: off / 16th / 8th — setting). B3 taps do the same —
a button is just another trigger, kept as the no-patch alternative.

This makes slicing *patchable*, which is the whole point:

- Clock a 16-count into the trigger jack → 16 slices, on time.
- Split a sequencer's trigger between Audiothurgist and a VCO's envelope
  while the sequencer's v/oct plays a scale → record → **one slice per
  note, automatically**. Exit, and the *same* sequencer now plays those
  slices back through the Voice (v/oct + trigger) — the patch that samples
  is the patch that performs. Auto-multisampling as a patching idiom.
- Any trigger source works: euclidean streams, clock dividers, an envelope
  follower on the material itself.

The trigger jack is the same one the Voice uses: **in LIVE-SLICE it cuts;
in PERFORM it plays.** One jack, one meaning per workflow, perfect patch
symmetry. The P3 ring shows markers land in real time; markers immediately
rebuild the slice table (v/oct re-spreads on the next call, the pattern
engine re-flattens on the next bar). Hold B3 again to exit;
long-hold-with-B2 clears all markers (back to grid slicing). Markers are
cheap and disposable — *if you don't like it, re-slice it* is a literal
gesture here.

Slice-table sources, unified (§6.1): **grid** (equal), **onset** (detected),
**manual** (live markers), and each is just a table — the pattern engine and
the Voice address slices by index and class regardless of origin.

### 5.3 LOAD
SD browse. Richer than the Pod: ring shows position, HostLink shows names.

## 6. Engine evolution (core work, all pure)

### 6.1 Slice tables decouple from the pattern grid
Today slices ≡ grid steps. v1 introduces `SliceTable` (start/len/class/
salience per slice, any count ≤ 64, any boundaries) with the pattern grid
remaining 16ths of the bar. Pattern steps address slices via the existing
class-matched selection; the Voice addresses them by index. Grid slicing
stays the default table; onset and manual tables are alternates.

### 6.2 Pattern languages
`PatternSource` produces the per-step {slice, class, velocity} stream that
flattens into triggers — three dialects, one fabric:

- **Brocot** — the BreakFix generator, as shipped (protected behaviors intact).
- **Grids** — port of the Mutable Grids drum map (GPL3 data into this AGPL3
  work: compatible, attributed). Map X/Y + three densities drive per-class
  step patterns; *applied to slices*: each class's steps fire class-matched
  slices. Every drum pattern Grids knows, spoken through any break.
- **Euclid** — E(k,16) per class with rotation (Toussaint, from the Codex).

P1-BRAIN morphs between languages by crossfading per-class step
probabilities — no hard switch, no dead zones.

### 6.3 The Voice and the Keymap

The Voice is a keyboard instrument carried on v/oct + gate. Incoming
v/oct quantizes to a semitone note number (note-CENTER windows with
hysteresis — the boundary rule applies to pitch too); note *ranges* are
functions. Reference controller: KeyStep Pro, 37 keys, optimized first.

**CV is monophonic — one pair reports one note.** So the keymap ships in
two configurations:

- **MONO (one pair, J3+J4)**: modifiers are **latched** — tap a keyswitch
  to toggle it, then play; every gate edge samples v/oct once and decodes
  it (function range = apply, slice range = fire). Set-then-play grammar,
  fully functional on two jacks.
- **SPLIT (two pairs, J3+J4 slices / J5+J6 modifiers)**: the KeyStep Pro's
  split sends each hand to its own CV pair; modifier semantics become
  genuinely **held-while-gate-high** — reverse only under your finger,
  ratchet as a gesture. The expressive two-hand version.

Default map (C2-base):

| Range (default, C2-base) | Function |
|---|---|
| **C2** | **Mute** — toggle the pattern loop on/off |
| C#2 | (gap — the buffer key) |
| **D2…B2** | **Left hand: modifier keyswitches** (latched in MONO, held in SPLIT): reverse · half-speed · double/ratchet · choke-vs-punch override · octave-down · **capture** (see below) |
| C3…B3 | reserved (future: language/fill gestures, second slice octave) |
| **C4…D#5** | **Right hand: sixteen slice keys**, chromatic, one slice per note |

The map is data (settings/HostLink-editable); the default fits 37 keys with
the mute key at the very bottom and a gap above it, per the reference
performance layout: left hand shapes, right hand calls.

**Interaction semantics — replace, don't layer.** The player is not
soloing over the pattern; they are *changing* it. Two levels, toggled by
keyswitch or setting:

- **CHOKE (momentary)**: a played slice replaces whatever is sounding now;
  the pattern resumes at the next step. Transient interjection.
- **PUNCH (persistent)**: played hits are recorded into the looping bar at
  their grid-quantized positions, *overwriting those steps* — playing IS
  editing. Each punched bar appends to the history timeline (P3 can undo a
  bad punch — the dial and the keyboard compose).

**Mute + punch = compose from silence**: mute the loop, play your pattern
in, unmute — your loop plays. The continuum the pitch promises falls out:
fully automatic rearranger at one end, hand-played slice patterns at the
other, every mixture in between; the internal recording is the *pattern
and timing* (grid-quantized v1), while the audio output records anywhere
downstream.

Calls use the same 8-voice pool; **policy: a call chokes sounding voices
(the replace principle), never silences the pattern's future steps** except
via punch. CV-shapable per-call speed/decay/reverse remain available on
jack maps that spend inputs on them.

### 6.4 Clock
`barPhase` is fed by the SDK's `MusicalClock` (96 PPQN, monotonic).
Unpatched: internal BPM from P2. Patched: `ClockFollower` PI-PLL follows
24 PPQN; P2 becomes a clock *divider/multiplier* select around the followed
tempo; the native-tempo detent logic (load-bearing for continuity) applies
to the *effective* step rate. Varispeed repitch semantics carry over intact.

### 6.5 Brain outputs
Step events enqueue per-class gates (configurable 1–10 ms) written to the
calibrated CV outs; velocity (from slice salience — the lit review's cheap
win, finally shipped) optionally scales gate level on the analog-capable
outs. Slice-index CV out: current source position as 0–5 V.

## 7. Ecosystem

- **HostLink from M6**: knobs, pages, presets, jack maps, and file
  management in the browser over panel USB-C. *This is the display.* A
  dedicated controller (the P4+C6 module) is a separate future project that
  must speak HostLink — one protocol, many faces, never a second UI.
- **Presets**: 16 slots — pattern seeds, knob state, jack map, slice table
  meta. The dice's history ring persists per session, not per preset.
- No MIDI in v1 (no DIN on the panel; HostLink CDC could carry it later).

## 8. Build order (each milestone flashes and gets heard before the next)

- **M0 — Skeleton**: `shells/alchemy/` CMake target against the SDK
  (sibling clone), boots, passes audio, one ring lit. Prove the toolchain.
- **M1 — Transplant**: engine + SD load + record + P1/P2/B2 + **the bar
  ring**. BreakFix parity on new hardware, now visible. The moment it grins.
- **M2 — Sync & Brain**: MusicalClock + ClockFollower, class gates out,
  velocity from salience.
- **M3 — Voice**: v/oct + trigger slice calls, CV mods, chop-CV input.
- **M4 — Languages**: Grids port + Euclid + the BRAIN page.
- **M5 — Live-slice**: SliceTable, manual markers, the on-the-fly workflow.
- **M6 — Toolkit**: presets, ParamLock, HostLink, settings, jack-map editor.

v2 horizon (explicitly out of v1): multitrack/layering (N engine instances
+ mixer), harmonic-layer separation (bass/lead/harmony sliced and layered),
pitch-tracked v/oct out, the companion display device, mistake-canonization
motif memory.

## 9. Open questions (tear here)

1. ~~History scrub gesture~~ — **RESOLVED**: P3 is the history dial (§3.5).
2. ~~Voice/pattern interaction~~ — **RESOLVED**: replace-don't-layer;
   CHOKE (momentary) vs PUNCH (persistent, history-appending) toggled by
   keyswitch/setting (§6.3).
3. ~~P3 Slices on PERFORM~~ — **RESOLVED**: demoted to Settings; P3 belongs
   to history.
4. **Default jack map** — HYBRID as shipped, or is BRAIN the truer first
   impression for a Eurorack audience?
5. **Gate count vs. jack budget** — is hat's gate worth losing an input in
   HYBRID, or does hat ride the index-CV out as accent?
6. **Live-slice quantize default** — off is now the lead candidate: a
   clocked trigger stream is inherently on time, so the quantizer serves
   sloppy sources (button taps, envelope followers), not the main path.

## 10. Protected behaviors (inherited law)

The 1 o'clock double-time illusion. The ~10 o'clock dice-and-grin zone.
The native-tempo detent. Patterns hold until asked. Every control means one
thing. No wrong notes — only notes that haven't been repeated yet.


## Addendum 2026-09-05 — keyframe lessons

Matthew Nielsen's DAFx26 paper and capicola were digested; see
`docs/keyframe-lessons.md`. The no-stretch rule in §1 stands as the default
(repitch is the genre) but is now under review as an *Option* (M7 candidate),
and §5.2 live-slice is proposed to be driven by his TKEO detector. Routing/
Depth mod pages are proposed in place of jack-maps-as-presets (§3.3).
