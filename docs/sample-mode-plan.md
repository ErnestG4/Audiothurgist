# Sample mode — plan (2026-09-07)

Ernest: "the key pressed can choose the sample being recorded into", and the
unquantized looper "would be a part of sample mode as well". This is the
missing third of the vision — *sampling*, slicing, rearranging — and the
first change to the engine's founding assumption (one material buffer,
sliced on a grid). Everything below is designed to keep that assumption
intact for the grammars and add a second world beside it.

## 1. One buffer, one table, two playback rules (Ernest, 2026-09-07)

There are not two worlds. There is **one material buffer** and **one slice
table**; a "sample set" is the same thing as a sliced beat — a WAV plus a
table — and what differs is only the *playback rule*:

- **Slice**: a key plays its slice, one shot (today).
- **Held**: a key plays from its slice's start and keeps reading the buffer
  until the gate releases (or the buffer ends) — the sampler rule.

So: loading a plain WAV gives the grid table (a beat); recording into a key
appends a region to the buffer and adds a table entry for that key; saving
a set writes `<name>.wav` + `<name>.json` (the table: start, len, key,
class, name, plus bars/tempo and the options snapshot); loading a WAV that
has a JSON beside it takes the table. The grammars address the table by
index and class exactly as now, so a recorded kit can be chopped like a
break and a break can be played like a kit. The user fits as many samples
as the buffer holds.

**Stereo**: Options → Sampling = Mono / **Stereo**. Stereo doubles the
buffer (L/R planes); voices read both planes with one envelope and
position; analysis (onsets, classes, density, bars) runs on the mono sum.
Buffer: 30 s per plane at 48 kHz float = 5.8 MB/plane; double-buffered
stereo = 23 MB, decode staging 11.5 MB; SDRAM has room (19.8 of 64 MB
today).

## 2. Engine changes (core/, pure, host-tested)

1. `Slice` gains nothing: the table stays `{start, len}` into the material
   buffer — one buffer, so the founding assumption holds. The table gains
   `key` (which note plays it), `name`, and the buffer gains a **free
   pointer** (where the next recording lands; a recording that does not
   fit fails loudly, LED red).
2. **Held rule**: `CallSlice(key, …, held)` — a held call's voice has
   `natEnd = bufLen` (read on to the end) and is choked by the gate's
   release (`ReleaseKey(key)`), with the normal fade. One-shot is today.
3. **Recording** (`Rec` in the engine): `Arm(slot)`, then the audio thread
   appends input samples at the free pointer while the gate is held (mono
   sum, L only, or both planes per the Sampling option); `Finish()` hands
   the region to the control thread for trim / normalise / onset / class /
   density with `bb::Yield()`, then commits the table entry. Stereo keeps
   the planes in step.
4. **Unquantized looper** ("Loop", a Play sub-mode; Quantize = **Off** /
   16ths / 8ths, default Off): an event list per loop (≤ 256: bar phase as
   a double, key, style, velocity), overdubbed by playing; `Process` fires
   each event at its exact sample. B3 clears; the newest overdub undoes.

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

## 4. Persistence (Ernest: "JSON is cheap, a 20 s WAV is cheap")

A set = `sets/<name>.wav` (the buffer, mono or stereo) + `sets/<name>.json`
(the table + bars/tempo + the options snapshot + the looper's events).
Preset save also writes `presets/<slot>.json` as a human-readable backup
of every setting, and the kit in use. Load takes the JSON when it sits
beside the WAV, else the grid. Needs a small WAV writer (a few dozen
lines) and a tiny JSON emitter/parser (no allocation, fixed schema).

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

## 7. Decisions (2026-09-07)

1. Record arm into a key — **under discussion**. Proposal: J5 style note 7
   = *Record* arms; while it stands, every key held records into that key
   (release = stop, trim, live) so a whole kit can be sampled key by key;
   any other style note disarms. Style 8 = *Clear*. Buttons red while
   armed, brighter while recording.
2. Sampling: **Mono / Stereo option** (people love their stereo samplers).
3. **One buffer, one table** — samples fit as the buffer allows; sets save
   and load like beats.
4. Loop recording start/stop — **under discussion**. Proposal: arm with
   Options → Record (or style 9); **start on B3 tap or the first J6 edge**;
   when clocked the length is the Bars count and it stops itself, and J6
   edges during the take drop slice markers; unclocked, B3 or J6 stops it.
5. Looper quantize default **Off**.
6. **Backups on the card**: sets (WAV + JSON) and settings/presets (JSON).

Aside: Luke has approved the 480 MHz boost; the BSP calls `seed.Init()`
without the flag, so it needs an `Init(sr, block, boost)` overload upstream
(asked). Our time base takes any tick rate. SDK v0.11 (clock-follower fix
for fast clocks, pot filtering, ParamLock v2) is a separate upgrade task.
