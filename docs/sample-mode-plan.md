# Sample mode — plan (2026-09-07)

Ernest: "the key pressed can choose the sample being recorded into", and the
unquantized looper "would be a part of sample mode as well". This is the
missing third of the vision — *sampling*, slicing, rearranging — and the
first change to the engine's founding assumption (one material buffer,
sliced on a grid). Everything below is designed to keep that assumption
intact for the grammars and add a second world beside it.

## 1. The two worlds

- **The break** (today): one buffer, sliced on the grid, played by the
  grammars and by the Voice's slice keys. Unchanged.
- **The kit** (new): up to 24 **slots**, one per key of the Voice map's two
  octaves. A slot is either *empty* (the key plays the break's slice region,
  exactly as now) or holds a **recording** from J1/J2. Recording into a key
  replaces that key's slot; clearing it gives the break back. The kit is the
  Voice's world; the grammars keep playing the break. (Later, optionally:
  Grids' kick/snare/hat could be pointed at kit slots — not in this plan.)

Memory: 24 slots × 4 s × 48 kHz × 4 B = 18.4 MB. SDRAM is at 19.8 of 64 MB.
Fine, with a cap of 4 s per slot (an option can raise it to 8 s later).

## 2. Engine changes (core/, pure, host-tested)

1. `Slice` gains a buffer pointer: `{const float* buf; int start; int len;}`.
   `Voice::Trigger` takes the slice's buffer; `Voice::Render` reads its own.
   The grid slices all point at the material buffer; kit slots at their own.
   Continuity/choke/zoom logic is unchanged (all of it is per voice).
2. `Kit`: `Slot slots[24]` = `{float* buf; int len; uint8_t cls; float sal;
   float dens; bool hit;}` plus `Record(int slot)` state: the audio thread
   appends input samples while armed; `Finish()` hands the slot to the
   control thread for trim / normalise / onset / class / density (the
   existing pure tools, with `bb::Yield()`), then marks it live.
3. `CallSlice(key, …)` resolves the key: slot filled → the slot's buffer
   at rate = pitch × style; empty → today's break slice. Gates (Brain map)
   and the hit trigger (Voice map) behave as now; a slot's class comes from
   its own analysis so a recorded snare drives the snare lane.
4. **Unquantized looper** ("Loop", a Play sub-mode, Quantize option):
   an event list per loop (≤ 256 events: bar phase as a double, key, style,
   velocity), overdubbed by playing; `Process` fires each event at its exact
   sample when the phase crosses it. Quantize = Off (exact) / 16ths (= Play
   today) / 8ths. B3 clears; the newest overdub can be undone (keep a
   generation number per event). Length = the loop (the material's bars, or
   a bar count option when the break is short).

## 3. Recording gestures

- **Into a key (Voice map):** J5 style note **7 = Record**. Hold the key on
  J4 → its slot records from the input for as long as the gate is held
  (auto-stop at the cap); release → trim, analyse, live. Style 8 = **Clear**
  (the key's slot goes back to the break). The B-button LEDs go red while
  recording; the key's ring position lights when its slot holds a
  recording (P5's ring = the kit map, 24 keys over 16 LEDs paired).
- **A loop into the break (the spec's RECORD, M1.6/M5):** B3 hold with
  audio patched and no file selected? No — too easy to hit. Proposed:
  Options → **Record** = Off / Arm; when armed, B3 tap starts and stops a
  loop recording into the material buffer, bar-quantized to the clock
  when following. While recording, rising edges on **J6** (Voice map) drop
  slice markers: the patch that samples is the patch that performs. On
  stop: condition → bars → slice table (markers win over the grid when
  present) → play. Live-slice, at last.
- **Record source:** J1/J2 summed to mono by default; an option for L only
  or *the module's own output* (resampling the chop into a slot — the MPC
  trick).

## 4. Persistence

Slots are saved to the card as `kit/<preset>/<key>.wav` on preset save, and
loaded on preset load (the loader already speaks WAV; writing needs a
small WAV writer — a few dozen lines). Without a card the kit is RAM-only
and survives until power-off. The looper's event list saves with the
preset (≤ 256 × 12 B).

## 5. Web

Slot view: which keys hold recordings, each slot's waveform (the GET_WAVE
command the slicing agent specced), record/clear buttons per key, the
looper's events on the bar strip, input meter. Reuses the envelope path
for slots.

## 6. Phases and who builds them

| Phase | What | Owner | Effort |
|---|---|---|---|
| A | Engine: per-slice buffer, Kit, key resolution, host tests | agent (engine) | half a day |
| B | Record into a key: audio-in capture, style 7/8, trim/analyse, LEDs | agent (record) after A | half a day |
| C | Loop record into the break + J6 live-slice markers | agent (record) | half a day |
| D | Unquantized looper with Quantize option, undo | agent (looper) after A | half a day |
| E | Persistence (kit WAVs, looper events in presets) | me, after B/D | short |
| F | Web: slot view, GET_WAVE, record buttons | agent (web) after B | half a day |

A first, then B, C, D in parallel on branches (the engine API is the
contract), then E and F. Every phase behind the suite; nothing lands on
the audio thread but the sample copy and the event scheduler.

## 7. Decisions needed from Ernest

1. Record arm: **style note 7 on J5** while holding the key (proposed), or a
   button gesture?
2. Record source default: **J1+J2 summed** (proposed), L only, or the
   module's own output?
3. Slot cap: **4 s** (proposed; 24 slots = 18 MB).
4. Loop recording start/stop: **Options → Record = Arm, then B3 tap**
   (proposed), or a dedicated jack?
5. Looper quantize default: **Off** (exact timing — the point of it).
6. Kit persistence on the card: **yes, per preset** (proposed).
