/* demo.js — a fake Audiothurgist on a fake serial port.
 *
 * Answers HostLink frames exactly as the firmware would (same framing, same
 * StateExt layouts including the v0.8.0, CLOCK, SAMPLE and SETS tails — 255 bytes
 * with the record state, key table and set status, or shorter on request — a descriptor in
 * the SDK's shape with the Options block, pager help and the manual), so
 * the page can be exercised without hardware — open index.html?demo — and
 * so selftest.mjs can drive the real Link end to end. The port exposes
 * WHATWG `readable`/`writable` streams like a Web Serial SerialPort.
 *
 * Nothing here is wired into the page when a real port is used.
 */
(function (root) {
'use strict';
const HL = typeof module === 'object' && module.exports ? require('./hostlink.js') : root.HL;
const { CMD, crc32, buildFrame, FrameParser, u16, u32 } = HL;

/* The Options block as src/audiothurgist.cpp's Options::Describe writes it — id, name, the enum labels (each
 * field's `zones` is the label count), byte = index, and the live default from the struct initialisers
 * (gGateMode = {1}, gTail = {3} …). selftest.mjs parses the C++ and fails when this drifts from it. */
const OPTION_FIELDS = [
  ['class.mode', 'Classifier', ['Hockman 3-band', 'Legacy 120/900 (hi/lo)', 'Two-band lo/hi', 'Density (keyframes)', 'Adaptive (onset, clustered)'], 0],
  ['clock.ppqn', 'Clock In', ['24 PPQN (MIDI/DIN)', '4 PPQN (16ths)', '2 PPQN (8ths)', '1 PPQN (quarters)', '48 PPQN', '96 PPQN', '16 PPQN', '8 PPQN'], 0],
  ['voice.mode', 'Voice', ['Choke (momentary)', 'Punch (writes the bar)'], 0],
  ['gate.len', 'Gate Length', ['6 ms trig', '20 ms', '50 ms', 'Hold to next hit'], 0],
  ['gate.lvl', 'Gate Level', ['Fixed 5 V', 'Velocity (1.5–5 V)'], 0],
  ['grammar.mode', 'Grammar', ['Classic', 'Collins', 'Grids', 'Euclid', 'Camp', 'Play (what you play in, looped)', 'Pads (free play, nothing written)'], 0],
  ['gate.mode', 'Gate Fires', ['Every step', 'Onsets only (once per hit)'], 1],
  ['bars', 'Bars', ['Auto (backbeat)', '1 bar', '2 bars', '4 bars', '8 bars (8th grid)'], 0],
  ['rests', 'Rests', ['Continue the break', 'Gate (silence)'], 0],
  ['gate.lanes', 'Gate Lanes', ['Dominant class', 'Multi-label (every band over threshold)'], 0],
  ['fades', 'Fades', ['Fixed', 'Adaptive (keyframe density)'], 1],
  ['tail', 'Slice Tail', ['Gate (honest)', 'Loop forward', 'Ping-pong', 'Zoom (read on, BreakFix)'], 3],
  ['swaps', 'Swaps', ['Class only', 'Class + salience tier'], 1],
  ['flow.dejavu', 'Flow Déjà Vu', ['Off', 'Some (¼ of flow bars from history)', 'Lots (¾)'], 1],
  ['swing', 'Swing', ['Straight 16ths', 'Auto (the material\'s swing)'], 1],
  ['onsets', 'Onsets', ['Spectral flux', 'Log-filtered flux (Böck 2012)'], 1],
  ['jacks', 'Jacks', ['Follow grammar', 'Brain (class gates out)', 'Voice (2× v/oct + gate)'], 0],
  ['key.plays', 'Key Plays', ['One-shot (the slice)', 'Held (read on until the gate falls)'], 0],
  ['sampling', 'Sampling', ['Mono (J1+J2 summed)', 'Stereo (J1 → L, J2 → R)'], 0],
  ['rec.src', 'Record Source', ['Inputs (J1/J2)', 'Output (the module\'s own mix)'], 0],
  ['record', 'Record', ['Off', 'Arm (B3 tap or J6 starts a loop take)'], 0],
  ['quantize', 'Quantize', ['Off (as played)', '16ths (punch the bar)', '8ths'], 0],
  ['tap.allow', 'Tap Allowance', ['0 ms', '20 ms', '40 ms', '60 ms', '80 ms'], 2],
  ['autosave', 'Autosave', ['Off', 'On (2 s after an edit)'], 1],
];
/* Option bytes the fake reads by name (the page reads them by the descriptor's offsets, never these). */
const O = Object.fromEntries(OPTION_FIELDS.map((f, i) => [f[0], i]));
const OPTIONS_HASH = 0x4F505438;   /* 'OPT8' — Options::SchemaHash() */
/* The grammars by index (Options byte 5): Play loops what you play in, Pads plays it and writes nothing. */
const GRAM = { classic: 0, collins: 1, grids: 2, euclid: 3, camp: 4, play: 5, pads: 6 };
/* The card's list as the firmware labels it: the breaks of Audiothurgist/breaks, then the sets of Audiothurgist/sets
 * as "<name> [set]". */
const FILES = ['amen_170.wav', 'apache_120.wav', 'funky_drummer_100.wav', 'think_110.wav', 'amen_kit [set]'];
/* CARD: the firmware's upload ceiling (kUpMax: 17 s × 48 kHz × stereo PCM16 + 4096) and how long the fake's write and
 * copy take (the firmware's are the card's speed; these only make the states visible). */
const UP_MAX = 17 * 48000 * 4 + 4096, CARD_WRITE_MS = 500, CARD_COPY_MS = 900;
/* A tiny valid WAV for the fake card's files (the bytes are never played — the fake's material is synthetic). */
const tinyWav = () => HL.encodeWav([new Float32Array(480)], 48000);
const SET_NAME_RE = /^[A-Za-z0-9_-]{1,31}$/;
/* The pager as the VirtualKnob declarations and DescribeManual give it: ident, name, help (v0.13.0-beta's text). */
const KNOBS = [
  [["chop", "Chop", "**Chop character** — the journey from verbatim (full right) to rolls, bursts and reverb (full left)."],
   ["tempo", "Tempo", "**Tempo**, 60–200 BPM, varispeed like vinyl. Snaps to the break's own tempo within ±2 BPM. While following an external clock it becomes a ÷4…×4 multiplier."],
   ["history", "History", "**History** — the last sixteen patterns. Counter-clockwise is older; full clockwise is now. New rolls always append."],
   ["pitch", "Pitch", "**Pitch**, ±12 semitones. Onsets stay on the grid; the kit retunes."],
   ["sample", "Sample", "**Sample** — highlight a file on the card (sets show as “name [set]”); hold **B3** to load it. The browser dropdown loads directly. While a set saves, this ring fills with the progress; while one loads, a dot sweeps it. On the Edit page (B1 held + B3 tap) it is the **autoslice** count."],
   ["volume", "Volume", "**Volume**, with the reverb tail riding the cluster zone."]],
  [["lang.x", "Map X", "**Map X** — Grids drum-map coordinate."],
   ["lang.y", "Map Y", "**Map Y** — Grids drum-map coordinate."],
   ["lang.kick", "Kick", "**Kick density** — Grids threshold / Euclid hits per bar."],
   ["lang.snare", "Snare", "**Snare density** — Grids threshold / Euclid hits per bar."],
   ["lang.hat", "Hat", "**Hat density** — Grids threshold / Euclid hits per bar."],
   ["lang.rot", "Rotation", "**Rotation** — Euclid: rotates every class's hits together."]],
];
/* The jacks as declared (Jack jk1 ("audio_l", "Audio In L", …)) with their help (v0.13.0-beta: one keymap). */
const JACKS = [
  ["audio_l", "Audio In L", "audioIn", "**Audio in, left.** The record input: summed with J2 (Sampling: Mono) or the L plane (Stereo); a key take (J5 style 7) or a loop take (Options → Record) hears it unless Record Source = Output."],
  ["audio_r", "Audio In R", "audioIn", "**Audio in, right.** Summed with J1, or the R plane (Sampling: Stereo)."],
  ["clock", "Clock In", "trig", "**Clock in.** Set PPQN in Options. B1 = pulse/lock indicator."],
  ["voct", "V/Oct", "voct", "**Key** (v/oct) — 0 V = key 0 (C1), a semitone per key to key 23, below 0 V = key 0; the key plays what the Kit's key table says. Gated by J6 (Voice map) or J5 (Brain map)."],
  ["gate", "Gate", "gate", "**Gate** (Brain map) — each rising edge plays J4's key, plain. **Style v/oct** (Voice map): 0 plain (anything under ~70 mV) · 1 reverse · 2 half · 3 double · 4 ratchet · 5 staccato · 6 punch/choke flip · **7 Record** (keys held on J4 record from the input) · **8 Clear** (the key back to its slice)."],
  ["kick", "Kick Gate", "gate", "**Kick gate out** (Brain map) — every kick-class step, or once per hit (Gate Fires); Gate Lanes can add it to mixed hits. **Gate in** (Voice map): plays the J4 key; with Record armed its first edge starts the loop take, later edges drop slice markers (clocked) or stop it (unclocked)."],
  ["snare", "Snare Gate", "gate", "**Snare gate out** (Brain map) — every snare-class step, or once per hit. **Hit trigger out** (Voice map): 10 ms per call."],
  ["hat", "Hat Gate", "gate", "**Hat gate out** (Brain map) — every hat-class step, or once per hit. **Velocity out** (Voice map): 1.5–5 V per call."],
  ["out_l", "Audio Out L", "audioOut", "Audio out, left."],
  ["out_r", "Audio Out R", "audioOut", "Audio out, right."],
];

/* Blob layout, in Manage() order: pager 48 · locks 64 · sample 1 · options 24 · settings 2. */
const OFF = { pager: 0, locks: 48, sample: 112, options: 113, settings: 137 };
const N_OPTIONS = OPTION_FIELDS.length;   /* 24, as Options::kBytes */
const LIVE_SIZE = 139;
const SCHEMA = 0x0a0d1338;
const FW = '0.13.0-beta-demo', FW_OLD = '0.12.3-demo';
/* The fake material: 48 kHz, one plane (and an R plane once the kit is stereo), 45 s of capacity; the break fills
 * nSlices × 6000 samples (32 slices = 4 s = two bars at 120 BPM). */
const SR = 48000, BUF_CAP = SR * 45, SLICE_LEN = 6000, KEYS = 24;   /* PADS: the firmware's planes, 45 s (kMaxSec) */
/* PADS: an empty kit's silent stub (bb::kPadStubLen, 16 slices of 750) and how long the fake's commit and New empty
 * kit take (the firmware's are its main loop's; these only make the states visible). */
const PAD_STUB = 12000, PAD_COMMIT_MS = 120, PAD_KIT_MS = 150;
const REC_MIN = 480;
/* EDIT: the engine's table geometry (core/bb_engine.h): the shortest grid slice an edit may leave, the
 * table's capacity, the flag bits, 48 samples per ms, the tap mark's 250 ms cap. */
const GRID_MIN = 64, MAX_TABLE = 96, F_ORANGE = 1, F_TOMB = 2, MS = 48, TAP_MAX = 250 * MS;
/* KEYS (v0.13): a tap this close to a live marker's start selects that marker instead of stacking another (10 ms);
 * the autoslice count's ceiling. */
const TAP_REUSE = 10 * MS, AUTO_MAX = 64;
const EDIT_NEED = [5, 9, 10, 5, 1, 1, 2, 2, 3, 2, 3, 7, 0, 2, 0, 0, 2, 0, 0, 1, 2, 7, 2];   /* field bytes of ACTION 16..38 */
/* KEYP: one key's settings (core/bb_engine.h KeyParam) — gain in ½ dB −48..+12, cents ±2400, plays 0 follow Key Plays ·
 * 1 one-shot · 2 held · 3 loop (held, looping the entry) · 4 ratchet (held, retriggering at `rat` 0..6); all zero = neutral */
const KEYP = { gainMin: -48, gainMax: 12, centsMax: 2400, playsMax: 4, ratMax: 6 };
const keypNeutral = () => ({ gain: 0, plays: 0, cents: 0, rat: 0 });
const UNDO_DEPTH = 64;   /* EDIT-UNDO: the journal's ring */
const EDIT_STATUS = { Ok: 0, BadOp: 2, BadArgs: 2, Full: 3, NotMarker: 2, NotGrid: 2, Tomb: 2, Stale: 3, NoMaterial: 3, Silent: 3 };
/* SCOPE: the firmware's ring geometry (core/bb_scope.h) and this module's max_body
 * (1024 in the firmware's Makefile; 512 is the protocol default an older build has). */
const MAX_BODY = 1024;
const SC = HL.SCOPE;

/* The manual: src/audiothurgist_manual.cpp as of v0.13.0-beta, verbatim (the module sends its own). */
const MANUAL = {
  preamble: "**Audiothurgist chops breaks to a grid and rearranges them** with one knob that travels from *the record, verbatim* to rolls, bursts, and reverb rubble. Patterns hold until you ask for new ones. Every sound is a slice of real material played from its attack, on the grid — no grains, no time-stretch. If you don't like it, re-slice it.",
  sections: [
    { id: "roles", title: "Three roles, one engine", body: "**Chopper** — the knobs and buttons: slice, rearrange, reroll, flow, history, tempo. **Voice** — the rack plays *it*: a v/oct + gate pair calls the Kit's keys from a keyboard or sequencer. **Brain** — it plays the rack: kick, snare and hat gates leave on J6/J7/J8 as the pattern runs, and a clock on J3 locks the loop to your master. The roles run at once; nothing needs a mode. **It powers up as a sampler**: Pads, the pattern stopped (B1 breathes) — the 24 keys (J4, the browser's pads) play, nothing else does. For a beat, pick a grammar (Classic, Grids…) and hold **B1** 600 ms (or ▶ in the browser); it starts on the beat. *Start in* (the web app's Options → Start-up, kept in `Audiothurgist/startup.json`) = As saved powers up in the boot preset's grammar, running, as before." },
    { id: "journey", title: "The knob journey (P1)", body: "Full right: verbatim. ~4 o'clock: whole beats trade places. ~1 o'clock: one-beat chops — listen for the double-time illusion. ~11: half-beat motifs. ~9: sixteenth-note dice and reversals. Past 8: the cluster zone — bursts of 32nd rolls, hat-ticks and real silence, reverb blooming toward full left. B3 relocates the burst centres. Chops respect the kit: kicks trade with kicks, snares with snares." },
    { id: "keymap", title: "Playing it from a keyboard (one keymap)", body: "**J4 is the key in every map: 0 V = key 0 (C1)**, a semitone per key to 23, below 0 V = key 0. A key plays what the Kit's key table says (in **Pads** the keys follow the markers: time order on keys 0, 1, 2…). **Voice map**: J6 gate, **J5 = the style** (0 V plain, then reverse · half · double · ratchet · staccato · punch/choke flip · 7 Record · 8 Clear, a semitone each; under ~70 mV is plain). The style is read with the hit; changing it while the key is held re-strikes it. J7 sends a trigger per call, J8 its velocity. **Brain map**: J5 gate, plain calls, class gates on J6/J7/J8. Options → Jacks picks the map; *Follow grammar* gives Play and Pads the Voice map. In Play calls write the loop (punch flips to momentary), in Pads nothing is written, elsewhere the Voice option decides." },
    { id: "buttons", title: "The three buttons", body: "**B1** tap: Perform ↔ Brain page. **B1 held 600 ms, let go** (B1 goes white; no pot, no other button): start / stop — the clock runs on, keys still play; B1 breathes while stopped. **B1 held 300 ms + turn a pot**: record that pot's motion as a loop that replays until you hold and nudge it again (a green pip at 6 o'clock marks a locked pot; *Clear locks* in the web app removes them all). **B2** tap: dice (a new arrangement of the same slices; in Play: undo the last overdub); hold: *flow*, a new bar every loop until released. **B3** tap: reslice (new cell tree; in Play: clear the loop); hold: load the highlighted file. **B2 + B3 held 2 s**: Settings (brightness, preset save/load); B2 or B3 leaves. B1 also shows the clock: white per pulse, green when locked. **B1 held + B3 tap: the Edit page** (B1 amber; B1 tap leaves): P1 selects a slice — its segment pulses on the bar ring as it plays and its keys pulse on P5, a Voice-map note selects too — P2 / P3 nudge its start / end ±50 ms once swept through centre, each move re-playing the new edge; B3 tap drops a marker at the playhead (onset-snapped, Options → Tap Allowance; near a marker it selects it), B3 hold reslices; **P5 = autoslice**: 0–32 grey markers on onsets drawn by loudness — up adds one, down drops one, 0 and back re-draws; B2 tap undoes, B2 hold puts the slice back on the grid (a marker: deletes it). Refusals blink red." },
    { id: "brain", title: "Clock in, gates out", body: "Patch a clock into J3 and set its PPQN in Options; B1 flashes white per pulse and sits green when locked (the loop aligns to the pulse grid at lock). While following, P2 becomes a clock divider/multiplier (÷4 · ÷2 · ×1 · ×2 · ×4, wide ×1 centre) that engages once you move it and switches on the next beat, in phase. Unpatch and the tempo holds until P2 moves. Gates on J6/J7/J8 follow the classifier's labels: *Gate Fires* chooses every step or once per hit (as drums do), *Gate Lanes* can mark every class present in a hit, Gate Length and Gate Level (velocity from loudness, accents to the ceiling) shape them. If a break plays at half or double speed against your clock, set **Bars** in Options — the bar guess is the culprit." },
    { id: "languages", title: "Pattern languages (Grammar, in Options)", body: "**Classic** is BreakFix's Brocot cell grammar. **Collins** adds the breakbeat-science upgrades: faithful early in the phrase, restless toward its end; 3+3+2 odd cuts; a fill on every fourth loop that snaps back on the downbeat; stutter terminators; accelerating rolls. **Grids** speaks Mutable Instruments' drum map through your break's own kicks, snares and hats — Map X/Y and three densities on the **Brain** page (B1). **Euclid** spaces each class's hits evenly with Rotation. In both, the Chop knob sets how faithfully slices are chosen: full right plays each class's own nearest hit, lower swaps within the class; a rest lets the break run on underneath (or gates it: *Rests*). **Camp** rings the bar's cells through change-ringing permutations — deterministic, B2 rings the next change. **Play** has no pattern of its own: the bar is what you play into it from the keyboard or the web page, looped; B3 clears it. With **Quantize** Off (the default) it is a free looper — every hit replays at the exact moment you played it, held notes with their length, overdubbing pass after pass; 8ths snaps hits to the nearest 8th, 16ths writes the bar step by step as before. **B2** tap undoes the last pass's overdub, **B3** tap clears. **Pads** is the instrument with no looper: hits sound as you play them, nothing is written, keeping the beat is yours." },
    { id: "options", title: "Options, one line each", body: "**Classifier** — which lens labels kick/snare/hat (Hockman 3-band, the legacy hi/lo split, two-band, keyframe density, or Adaptive: each step judged at its hit, clustered per break). **Clock In** — J3 PPQN. **Voice** — Choke or Punch. **Gate Length / Level / Fires / Lanes** — see the Brain section. **Grammar** — the pattern language. **Bars** — override the bar guess (Auto reads the backbeat). **Rests** — continue or gate. **Fades** — fixed, or adaptive to each slice's brightness. **Slice Tail** — what a slot does at pitch: zoom (read on into the next slice, the record's way), gate, loop forward, or ping-pong. **Swaps** — class only, or class + loudness tier (a ghost never replaces a backbeat). **Flow Déjà Vu** — some flow bars replay from history. **Swing** — slice and step at the material's own swing. **Onsets** — the onset detector behind the bar guess and hit gates. **Jacks** — Brain (class gates out) or Voice (two v/oct + gate, hit trigger and velocity out); Follow grammar switches with Play. **Quantize** — Play's looper: Off (as played), 16ths (punch the bar), 8ths. **Autosave** — Off, or On: 2 s after an edit the slice table and key map are written to the set's JSON, or to sets/<file>.slices.json beside a loose WAV, and read back on load. **Key Plays** — one-shot, or held until the gate falls (a key's own Plays, in the Kit, overrides it). **Sampling** — mono, or stereo planes. **Record Source** — the inputs, or the module's own output. **Record** — arm a loop take into the break (see Sampling). **Tap Allowance** — how far back a tap-while-playing mark lands (your reaction time): 0–80 ms. **Start in** (on the card, not in a preset; the web app sets it) — Sampler (Pads, stopped; the default, and with no card) or As saved (the boot preset's grammar, running); read at power-up." },
    { id: "sampling", title: "Sampling (record into a key, loop-record the break)", body: "One buffer, one slice table: a loaded break and a recorded kit are the same thing. **Into a key** (Voice map): play J5 style note **7 = Record** — the buttons go red — and every key you hold on J4 records from the input for as long as its gate stands (release: trim, normalise, analyse, live; the key plays it, its position lights on P5's ring). Style **8 = Clear** puts a key back on its grid slice; any other style disarms. Records are appended after the break; a load clears them. **The break itself**: Options → **Record** = Arm, then **B3 tap** or the first **J6** edge starts a take into the other buffer. With a clock on J3 the take is the **Bars** count of bars at the followed tempo and stops itself on the bar; J6 edges during it drop **slice markers** (two or more replace the grid). Unclocked, B3 or J6 stops it. The take runs the load pipeline and plays; the arm drops after one take, and B3's reslice / load are held off while armed. **Record Source**: J1/J2, or the module's own output — play the keys into a take and chop the chop. **Sampling**: Mono sums J1+J2; Stereo keeps J1 → L, J2 → R (loads, takes and playback in two planes). Loads keep 28 s of the 45 s buffer free for keys and pads; a recording plays at its own pitch (P4 transposes it), the break's slices follow the tempo. **Pads**: drop an audio file on a pad in the browser (Play or Kit) and that key plays it, mono or stereo (a stereo one makes the kit stereo); **New empty kit** clears the break so the whole buffer is for pads. **Each key** has its own gain (−24…+6 dB), tune (±24 semitones) and Plays (follow Key Plays · one-shot · held · loop while held · ratchet while held: the sample again every whole, ½, ¼ or ⅛ of itself, or every 8th / 16th / 32nd at the tempo), set in the browser's Kit tab and saved with the set; they ride every call through the key — CV, the pads, Play's loop — never the pattern's steps. **Spread across keys** (Kit tab) puts one key's sample on all 24, a semitone apart: J4's V/oct plays it as a melody. A dropped sample lives in memory until you save a set. While a take runs the buttons are full red and P6 shows the input level. **Cards**: FAT32 microSD, everything in one folder, `Audiothurgist/`: breaks in `Audiothurgist/breaks` (WAV, PCM 16/24-bit, mono or stereo, the first 17 s used), 1–4 bar breaks are ideal (longer files trim to their first four bars; levels normalise on load). The Kit tab uploads any audio file the browser can open, and copies an old card's root WAVs in. Turn **Sample** (P5) to highlight a file, hold **B3** to load it — or pick it from the Sample dropdown here. New material opens at its own tempo until P2 moves. **Sets** — a break with its recorded keys, the Options and Play's loop — live in `Audiothurgist/sets/` as `<name>.wav` + `<name>.json`, listed as “name [set]” and loaded the same way; the browser's Kit tab saves and loads them by name. Every preset save also writes `Audiothurgist/presets/<slot>.json`; a slot the flash no longer honours after a firmware update comes back from it at boot." },
  ],
  presets: { help: "Preset slots store the complete state of the module. Slot 0 is the boot slot." },
};

/* opts: { edit: true, keys: true } — keys: false plays a 0.12 firmware (edit 1, no ACTION 32.., the 263-byte body).
 * CARD: `files` the list the firmware scanned at boot (its dropdown; none on a card without the folder), `card` the
 * capability. */
function buildDescriptor(edit = true, keys = edit, files = FILES, card = keys, pads = card, keyp = pads, spread = keyp) {
  const pagerFields = [];
  KNOBS.forEach((page, pg) => page.forEach(([id, name, help], pot) => pagerFields.push({ id, name, off: (pg * 6 + pot) * 4, type: 'f32', page: pg, pot, def: 0.5, disp: pot === 3 && pg === 0 ? { kind: 'linear', lo: -12, hi: 12, unit: 'st' } : pot === 1 && pg === 0 ? { kind: 'linear', lo: 60, hi: 200, unit: 'BPM' } : { kind: 'norm' }, help })));
  return {
    dv: 1,
    module: { id: 'audiothurgist', name: 'Audiothurgist', fw: keys ? FW : FW_OLD, git: 'demo', sdk: 'demo', board: 'v2', tagline: 'A sampling, slicing, and rearranging toolkit' },
    schemaHash: SCHEMA, size: LIVE_SIZE,
    components: [
      { id: 'perf', kind: 'pager', hash: 1, size: 48, off: OFF.pager, pages: 2, pots: 6,
        pageNames: ['Perform', 'Brain'], pageColors: ['#eb7414', '#3a9fe0'],
        pageHelp: ['The performance page: everything BreakFix was, plus tempo as a turntable, history as a dial, and file selection.',
                   'The pattern-language console. Live for Grids and Euclid (Grammar, in Options); a change regenerates the pattern.'],
        fields: pagerFields },
      { id: 'locks', kind: 'param_locks', hash: 2, size: 64, off: OFF.locks, name: 'Param Locks', slots: 2, slotSize: 32, bufLen: 4, layout: { activeOff: 0, lengthOff: 1, baseOff: 3, bufOff: 7 } },
      { id: 'sample', kind: 'fields', hash: 0x53414d50, size: 1, off: OFF.sample, name: 'Sample',
        fields: files.length ? [{ id: 'sample.idx', name: 'File', off: 0, type: 'enum', zones: files.length, def: 0, disp: { kind: 'enum', labels: files.slice() } }] : [] },
      { id: 'options', kind: 'fields', hash: OPTIONS_HASH, size: N_OPTIONS, off: OFF.options, name: 'Options',
        fields: OPTION_FIELDS.map(([id, name, labels], i) => ({ id, name, off: i, type: 'enum', zones: labels.length, def: 0, disp: { kind: 'enum', labels } })) },
      { id: 'settings', kind: 'settings', hash: 4, size: 2, off: OFF.settings, pages: 1, pageNames: ['Setup'],
        fields: [{ id: 'brightness', name: 'Brightness', off: 0, type: 'f32', page: 0, pot: 0, def: 0.7, disp: { kind: 'brightness', lo: 0.1, hi: 1 } }] },
    ],
    jacks: JACKS.map(([id, name, sig, help]) => ({ id, name, sig, help })),
    /* StateExt::DescriptorRootJson: v0.13 adds "keys":1 and bumps "edit" to 2 (TAP_MARK refuses when nothing sounds
     * and reuses a marker within 10 ms) */
    audiothurgist: Object.assign({ ext: 1, state: 96, env: 97, scope: 98, action: 99, wave: 100, table: 101, hits: 102, sets: 1, scope2: 2 },
      edit ? { edit: keys ? 2 : 1 } : {}, keys ? { keys: 1 } : {}, card ? { card: 1 } : {}, pads ? { pads: 1, startIn: 1 } : {}, keyp ? { keyp: spread ? 2 : 1 } : {}),   /* START: "startIn" came with the pads work's successor; CHROMA: "keyp": 2 = ACTION 38 */
    manual: MANUAL,
  };
}

class FakeDevice {
  constructor(opts = {}) {
    /* EDIT: `edit: false` plays an older firmware — no "edit" capability, the 13-byte table whatever is asked,
     * the body capped at 255 (no tableGen / srcPos tail), ACTION 16.. unknown (BAD_ARGS). */
    /* KEYS: `keys: false` (or a body shorter than 276) plays a 0.12 firmware — "edit": 1, no "keys", ACTION 32.. unknown,
     * TAP_MARK taken whatever sounds. */
    this.opts = Object.assign({ tail: 315, bpm: 120, nSlices: 32, latencyMs: 8, swing: false, kit: false, maxBody: MAX_BODY, inputs: true, edit: true }, opts);
    if (!this.opts.edit && this.opts.tail > 255) this.opts.tail = 255;
    if (this.opts.keys == null) this.opts.keys = this.opts.edit && this.opts.tail >= 276;
    if (!this.opts.keys && this.opts.tail > 263) this.opts.tail = 263;
    /* CARD: `card: false` (or a body shorter than 293) plays a 0.13 firmware before the card work — no "card", no 0x67,
     * the 276-byte body, the list from the card root. `oldcard` lays the card out the old way (WAVs in the root, sets/
     * and presets/ beside them, no Audiothurgist folder — so this firmware lists nothing); `nocard` has no card in. */
    if (this.opts.card == null) this.opts.card = this.opts.keys && this.opts.tail >= 293;
    if (!this.opts.card && this.opts.tail > 276) this.opts.tail = 276;
    /* PADS: `pads: false` (or a body shorter than 315) plays a firmware before the pads work — no "pads", no 0x68, the
     * 293-byte body. */
    if (this.opts.pads == null) this.opts.pads = this.opts.card && this.opts.tail >= 315;
    if (!this.opts.pads && this.opts.tail > 293) this.opts.tail = 293;
    /* KEYP: `keyp: false` plays a firmware before the per-key settings — no "keyp", ACTION 37 unknown (BAD_ARGS), 0x69
     * UNSUPPORTED. No tail bytes: the page reads 0x69 whenever tableGen moves. */
    if (this.opts.keyp == null) this.opts.keyp = this.opts.pads;
    /* CHROMA: `spread: false` plays a firmware with the key settings but before SPREAD_KEYS — "keyp": 1, ACTION 38 unknown */
    if (this.opts.spread == null) this.opts.spread = !!this.opts.keyp;
    this.keyp = Array.from({ length: KEYS }, keypNeutral);
    this.cardInit();
    this.live = new Uint8Array(LIVE_SIZE); this.live.fill(0x80, OFF.pager, OFF.pager + 48);
    OPTION_FIELDS.forEach((f, i) => { this.live[OFF.options + i] = f[3]; });   /* the firmware's live defaults */
    this.desc = new TextEncoder().encode(JSON.stringify(buildDescriptor(this.opts.edit, this.opts.keys, this.files, this.opts.card, this.opts.pads, this.opts.keyp, this.opts.spread)));
    /* KEYS: the pad / CV voice (the key, when it started, its span, until when it sounds — Infinity while a held key is
     * down), the key-call flash and sequence, the play loop's events (Play: each write, for B2's undo), the autoslice
     * seed and count, the selection the panel publishes (tail 257), and the last position something sounded at
     * (srcPos holds still while nothing does — the Chop pad's guard). */
    this.voice = null; this.keyFlash = 255; this.keySeq = 0; this.loopEvents = []; this.autoSeed = 1; this.autoK = 0; this.selSlice = 255; this.lastSrcPos = 0;
    /* EDIT: the table generation (never 0), the audition voice's last request, its rate window and the request held
     * for it (ten a second; a later one replaces), and how many went out. */
    this.tableGen = 1; this.audition = null; this.auditionAt = -1e9; this.auditionHeld = null; this.auditionPosted = 0;
    /* EDIT-UNDO: the journal — the firmware keeps inverse records, the fake keeps whole table snapshots (same semantics on
     * the wire: 64 deep, a new op clears the redo half, bound to the generation it last wrote so a take / clear / load
     * / new material ends it). */
    this.journal = []; this.redoStack = []; this.journalGen = 0;
    this.muted = false; this.flowing = false; this.brain = false; this.ext = false; this.locked = false;
    this.lockMask = 0; this.matGen = 1; this.file = 0; this.histN = 3; this.histDepth = 0; this.callFlash = 255; this.callT = 0;
    this.mult = 1; this.multZone = 2; this.t0 = Date.now(); this.log = [];
    /* CLOCK flags (170-byte body): P2 engaged, a multiplier switch waiting for its beat, a lock
     * alignment in the last ~2 s; the ext-transition counter is kept here from `ext` flips. */
    this.p2Engaged = false; this.pending = false; this.aligned = false; this.extTrans = 0; this._lastExt = false;
    this.src = new Int8Array(64); this.stepClass = new Uint8Array(64);
    this.env = new Uint8Array(64 * 16); this.scopePhase = 0;
    /* SAMPLE: the material buffer, the slice table (grid then recorded), the
     * key table, the onset table, and a take's state machine driven by time:
     * armed → (700 ms) taking → (1.2 s, the write head advancing) → pending
     * → (300 ms) committed. */
    this.buf = new Float32Array(BUF_CAP); this.bufLen = 0; this.freePos = 0;
    /* PADS: the R plane (allocated when the kit turns stereo), whether it is, an empty kit (no break), the frames the
     * card holds of this kit (the unsaved note), the last drop made it stereo; the drop in flight and the op state. */
    this.bufR = null; this.stereo = false; this.noBreak = false; this.onCard = 0; this.padStereo = false;
    this.padUp = { open: false, key: -1, ch: 1, frames: 0, total: 0, have: 0, start: 0, last: 0, bytes: null };
    this.padOp = { state: 0, progress: 0, result: 0, seq: 0, key: 255, t: 0, job: null };
    this.table = []; this.keySlice = new Int8Array(KEYS); this.hits = [];
    this.rec = { state: 0, key: -1, t: 0, pos: 0 }; this.inLevel = 0; this.looperEvents = 0;
    /* SETS: the card's sets (name → a snapshot of everything a set carries) and the op in flight,
     * driven by time on each state poll: pending → saving / loading for 600 ms → done (seq bumps). */
    this.sets = new Map(); this.set = { state: 0, progress: 0, result: 0, seq: 0, name: '', t: 0, pending: null };
    this.newMaterial(); this.onCard = this.bufLen;
    if (this.opts.kit) { this.recordKey(3, 0.35, 0); this.recordKey(7, 0.9, 1); this.recordKey(14, 1.6, 2); this.looperEvents = 3; this.sets.set('amen_kit', this.snapshot()); }
    this.blob = null;
    /* START (beta-sampler): Start in, the module's power-up setting, kept on its card (Audiothurgist/startup.json).
     * `startIn` 0 Sampler / 1 As saved; `bootCard` (another fake's card — a power-cycle) is read as the firmware reads
     * it: no file, no card (`nocard` too) = Sampler. Without either the fake boots As saved — the boot every check written before this
     * branch expects (Classic, running). Sampler: Pads, the transport stopped, as the firmware's boot. */
    if (this.opts.bootCard) this.cardFs = this.opts.bootCard;
    this.startIn = !this.opts.pads ? 1 : !this.cardMounted || this.opts.bootCard ? this.startupOnCard() : this.opts.startIn != null ? this.opts.startIn : 1;   /* no card: Sampler, as the firmware */
    this.lastDrum = this.grammar !== GRAM.pads ? this.grammar : 0;   /* START: ▶ in Pads comes back to this beat */
    if (this.opts.pads && this.startIn === 0) { const g = this.grammar; this.options[O['grammar.mode']] = GRAM.pads; this.setGrammar(GRAM.pads, g); this.muted = true; }
  }
  /* START: as the firmware's TransportStart — a start made in Pads brings back the last drum grammar first */
  transportStart() {
    if (this.grammar === GRAM.pads) { const g = this.grammar; this.options[O['grammar.mode']] = this.lastDrum; this.setGrammar(this.lastDrum, g); }
    this.muted = false;
  }
  /* START: what the card's startup.json says (the firmware's bb::StartupRead: anything but "saved" is Sampler) */
  startupOnCard() {
    if (!this.cardMounted) return 0;
    const k = this.cardKey('Audiothurgist/startup.json'); if (!k) return 0;
    try { return JSON.parse(new TextDecoder().decode(this.cardFs.get(k))).startIn === 'saved' ? 1 : 0; } catch { return 0; }
  }
  /* SETS: what sets/<name>.wav + .json would hold. */
  snapshot() {
    return { nSlices: this.nSlices, bufLen: this.bufLen, freePos: this.freePos, table: this.table.map(e => Object.assign({}, e)),
      keySlice: Int8Array.from(this.keySlice), hits: this.hits.slice(), looperEvents: this.looperEvents, options: Uint8Array.from(this.options),
      buf: this.buf.slice(0, this.freePos), src: Int8Array.from(this.src), stepClass: Uint8Array.from(this.stepClass), env: Uint8Array.from(this.env),
      bufR: this.stereo && this.bufR ? this.bufR.slice(0, this.freePos) : null, stereo: this.stereo, noBreak: this.noBreak,
      keyp: this.keyp.map(p => Object.assign({}, p)) };   /* KEYP: the set JSON's "keyp" */
  }
  restore(sn) {
    this.opts.nSlices = sn.nSlices; this.bufLen = sn.bufLen; this.freePos = sn.freePos; this.table = sn.table.map(e => Object.assign({}, e));
    this.keySlice.set(sn.keySlice); this.hits = sn.hits.slice(); this.looperEvents = sn.looperEvents; this.options.set(sn.options);
    this.buf.fill(0); this.buf.set(sn.buf); this.src.set(sn.src); this.stepClass.set(sn.stepClass); this.env.set(sn.env);
    /* PADS: a stereo set keeps its R plane (Sampling = Stereo); an empty kit's stub is recognised by its silence */
    this.stereo = !!(sn.stereo && sn.bufR) && this.options[O.sampling] === 1; if (this.stereo) { this.bufR = new Float32Array(BUF_CAP); this.bufR.set(sn.bufR); }
    this.noBreak = !!sn.noBreak; this.onCard = sn.freePos; this.padStereo = false;
    this.keyp = sn.keyp ? sn.keyp.map(p => Object.assign({}, p)) : Array.from({ length: KEYS }, keypNeutral);   /* KEYP: an old set: neutral */
    this.rec = { state: 0, key: -1, t: 0, pos: 0 }; this.matGen = (this.matGen + 1) & 0xff; this.bumpGen(); this.journal = []; this.redoStack = [];
  }
  /* EDIT: every table change bumps the generation (an edit, a take's commit, a clear, new material, a set load). */
  bumpGen() { this.tableGen = (this.tableGen % 255) + 1; }
  setTick() {
    const s = this.set, now = Date.now();
    if (!s.state && s.pending) { s.state = s.pending.op === 14 ? 1 : 2; s.progress = 0; s.t = now; }
    if (!s.state) return;
    const el = now - s.t; s.progress = Math.min(255, Math.round(el / 600 * 255));
    if (el < 600) return;
    const { op, name } = s.pending; s.pending = null; s.state = 0; s.progress = 0;
    if (op === 14) {
      if (name === 'nocard') s.result = 1; else if (name === 'full') s.result = 2;
      else { this.sets.set(name, this.snapshot()); this.onCard = this.freePos; s.result = 0; s.name = name; const lab = name + ' [set]'; if (!this.files.includes(lab)) this.files.push(lab); this.cardPut('Audiothurgist/sets/' + name + '.wav', tinyWav()); this.file = this.files.indexOf(lab); this.live[OFF.sample] = this.file; }
    } else {
      const sn = this.sets.get(name);
      if (!sn) s.result = 5; else { this.restore(sn); s.result = 0; s.name = name; const i = this.files.indexOf(name + ' [set]'); this.file = i >= 0 ? i : 255; if (i >= 0) this.live[OFF.sample] = i; }
    }
    s.seq = (s.seq + 1) & 0xff;
  }
  /* ── CARD: the card as files, the list, the receiver, the op in flight ── */
  cardInit() {
    this.cardFs = new Map(); this.cardMounted = !this.opts.nocard; this.cardFree = Infinity;
    const loose = FILES.filter(f => !f.endsWith(' [set]'));
    if (this.opts.oldcard) {
      for (const f of loose) this.cardPut(f, tinyWav());
      this.cardPut('sets/amen_kit.wav', tinyWav()); this.cardPut('sets/amen_kit.json', '{"audiothurgist":1}'); this.cardPut('sets/think_110.wav.slices.json', '{"audiothurgist":1}');
      this.cardPut('presets/0.json', '{"audiothurgist":1,"slot":0}'); this.cardPut('._amen_170.wav', 'AppleDouble'); this.cardPut('OTHER/kyk.wav', tinyWav());
    } else if (this.cardMounted) {
      for (const f of loose) this.cardPut('Audiothurgist/breaks/' + f, tinyWav());
      this.cardPut('Audiothurgist/sets/amen_kit.wav', tinyWav()); this.cardPut('Audiothurgist/sets/amen_kit.json', '{"audiothurgist":1}');
    }
    this.files = this.cardScan();
    this.cardUp = { open: false, total: 0, have: 0, name: '', over: false, buf: null, last: 0 };
    this.cardOp = { state: 0, progress: 0, result: 0, seq: 0, lastIdx: 255, copied: 0, skipped: 0, t: 0, job: null };
    this.survey = this.cardSurvey();
  }
  cardKey(p) { const l = p.toLowerCase(); for (const k of this.cardFs.keys()) if (k.toLowerCase() === l) return k; return null; }   /* FAT: case-insensitive */
  cardHas(p) { return this.cardKey(p) != null; }
  cardPut(p, bytes) { const k = this.cardKey(p); if (k) this.cardFs.delete(k); this.cardFs.set(p, typeof bytes === 'string' ? new TextEncoder().encode(bytes) : bytes); }
  cardDir(dir) { const pre = dir ? dir + '/' : ''; return [...this.cardFs.keys()].filter(k => k.startsWith(pre) && !k.slice(pre.length).includes('/')).map(k => k.slice(pre.length)); }
  /* the firmware's ScanCard: breaks sorted, then sets sorted as "<name> [set]" labels; the root is not read */
  cardScan() {
    if (!this.cardMounted) return [];
    const wav = n => /\.wav$/i.test(n) && n[0] !== '.' && n.length > 4 && n.length <= 120, sort = a => a.sort((x, y) => x.toLowerCase() < y.toLowerCase() ? -1 : x.toLowerCase() > y.toLowerCase() ? 1 : 0);
    return [...sort(this.cardDir('Audiothurgist/breaks').filter(wav)), ...sort(this.cardDir('Audiothurgist/sets').filter(wav)).map(n => /^[A-Za-z0-9_-]{1,31}\.wav$/i.test(n) ? n.slice(0, -4) + ' [set]' : 'sets/' + n)];
  }
  /* the copy plan (core/bb_card.h CopyWants / CopyPaths) */
  cardCandidates() {
    const out = [], ok = (s, n) => n[0] !== '.' && n.length <= 120 && (s === 0 ? /\.wav$/i.test(n) && n.length > 4 : !/\.(tmp|part)$/i.test(n));
    [['', 'Audiothurgist/breaks'], ['sets', 'Audiothurgist/sets'], ['presets', 'Audiothurgist/presets']].forEach(([src, dst], s) => {
      for (const n of this.cardDir(src)) if (ok(s, n)) out.push({ src: (src ? src + '/' : '') + n, dst: dst + '/' + n, name: n, s });
    });
    return out;
  }
  cardSurvey() {
    const c = this.cardCandidates();
    return { rootWavs: this.cardDir('').filter(n => /\.wav$/i.test(n) && n[0] !== '.').length, pending: c.filter(x => !this.cardHas(x.dst)).length,
      breaks: this.cardDir('Audiothurgist/breaks').filter(n => /\.wav$/i.test(n) && n[0] !== '.').length,
      oldSets: [...this.cardFs.keys()].some(k => k.startsWith('sets/')), oldPresets: [...this.cardFs.keys()].some(k => k.startsWith('presets/')) };
  }
  /* MergeCard: what the folders hold and the list does not joins the end — no index moves */
  cardMerge() { for (const lab of this.cardScan()) if (!this.files.some(f => f.toLowerCase() === lab.toLowerCase())) this.files.push(lab); }
  cardFlags() {
    if (!this.cardMounted) return 0; const v = this.survey;
    return 1 | (v.rootWavs ? 2 : 0) | (v.oldSets ? 4 : 0) | (v.oldPresets ? 8 : 0) | (v.breaks ? 16 : 0) | (v.pending ? 32 : 0);
  }
  cardBusy() { return !!(this.set.state || this.set.pending || this.rec.state || this.cardOp.job || this.padBusy()); }   /* PADS: and the reverse */
  /* ── PADS: a sample dropped onto a pad (the firmware's HandlePads / ServicePads), New empty kit ── */
  padBusy() { return !!(this.padOp.state || this.padOp.job); }
  padRefused() { return !!(this.set.state || this.set.pending || this.rec.state || this.cardOp.state || this.cardOp.job || this.padOp.state >= 2 || this.padOp.job || !this.bufLen); }
  padFinish(r) { const o = this.padOp; o.result = r; o.state = 0; o.progress = 0; o.job = null; o.seq = (o.seq + 1) & 0xff; }
  padRoom() { return Math.max(0, BUF_CAP - this.freePos); }
  /* LiveRecordedSamples: the union of the live entries past the grid over [bufLen, freePos) */
  padLive() {
    const iv = []; for (let t = this.nSlices; t < this.nTable; t++) { const e = this.table[t]; if (this.isTomb(t) || e.len <= 0) continue; const a = Math.max(this.bufLen, e.start), b = Math.min(this.freePos, e.start + e.len); if (b > a) iv.push([a, b]); }
    iv.sort((x, y) => x[0] - y[0]); let sum = 0, ca = -1, cb = -1;
    for (const [a, b] of iv) { if (cb < 0 || a > cb) { if (cb > ca) sum += cb - ca; ca = a; cb = b; } else if (b > cb) cb = b; }
    if (cb > ca) sum += cb - ca; return sum;
  }
  padHeld() { return Math.max(0, this.freePos - this.bufLen - this.padLive()); }
  padMask() { let m = 0; for (let k = 0; k < KEYS; k++) if (this.table.some((e, t) => t >= this.nSlices && !this.isTomb(t) && e.key === k)) m |= 1 << k; return m >>> 0; }
  /* KitUnsaved: the material not on the card, or a live entry reaching past what the card holds (an empty kit's stub is nothing) */
  kitUnsaved() {
    if (!this.bufLen) return false;
    if (!this.noBreak && this.bufLen > this.onCard) return true;
    const lim = this.noBreak && this.onCard < this.bufLen ? this.bufLen : this.onCard;
    return this.table.some((e, t) => t >= this.nSlices && !this.isTomb(t) && e.len > 0 && e.start + e.len > lim);
  }
  padTick() {
    const o = this.padOp, u = this.padUp, now = Date.now();
    if (o.state === 1 && u.open && now - u.last > 5000) { u.open = false; u.bytes = null; this.padFinish(5); }
    if (!o.job || now - o.t < (o.job === 'kit' ? PAD_KIT_MS : PAD_COMMIT_MS)) return;
    if (o.job === 'commit') this.padCommit(); else this.padNewKit();
  }
  /* the commit: the bytes → the planes (a stereo drop onto a mono kit grows the R plane, Sampling → Stereo), a crude
   * class from the zero-crossing rate, then the key's recording slot reused or a free one — one undo step */
  padCommit() {
    const u = this.padUp, start = u.start, n = u.frames, ch = u.ch, dv = new DataView(u.bytes.buffer, u.bytes.byteOffset);
    if (start !== this.freePos || start + n > BUF_CAP) { u.open = false; u.bytes = null; return this.padFinish(7); }
    if (ch === 2 && !this.stereo) { this.bufR = this.bufR || new Float32Array(BUF_CAP); this.bufR.set(this.buf.subarray(0, this.freePos)); this.stereo = true; this.options[O.sampling] = 1; this.padStereo = true; }
    let zc = 0, prev = 0;
    for (let i = 0; i < n; i++) {
      const l = dv.getInt16(i * ch * 2, true) / 32768, r = ch === 2 ? dv.getInt16(i * 4 + 2, true) / 32768 : l;
      this.buf[start + i] = l; if (this.stereo) this.bufR[start + i] = r;
      if (i < SR && (l >= 0) !== (prev >= 0)) zc++; prev = l;
    }
    const rate = zc / Math.min(n, SR), cls = rate < 0.01 ? 0 : rate < 0.08 ? 1 : 2;
    const before = this.tableSnap(), e = { start, len: n, key: u.key, cls, sal: 255, hit: true, dens: Math.min(255, Math.round(rate * 1000)), flags: 0 };
    let t = this.table.findIndex((x, i) => i >= this.nSlices && !this.isTomb(i) && x.key === u.key);
    if (t < 0) { t = this.freeSlot(); if (t < 0) { u.open = false; u.bytes = null; return this.padFinish(4); } }
    if (t === this.nTable) this.table.push(e); else this.table[t] = e;
    this.keySlice[u.key] = t; this.freePos = start + n;
    this.journalCheck(); this.bumpGen(); this.journalPush(before);
    u.open = false; u.bytes = null; this.padFinish(0);
  }
  /* New empty kit: the ¼ s silent stub, 16 grid slices of class "other", no onsets; the table, the looper and the undo
   * history start over (as a load); the grammar → Pads; the clock keeps its tempo */
  padNewKit() {
    this.opts.nSlices = 16; this.bufLen = PAD_STUB; this.freePos = PAD_STUB; this.buf.fill(0); if (this.bufR) this.bufR.fill(0);
    this.table = []; this.hits = [];
    for (let s = 0; s < 16; s++) { this.table.push({ start: s * 750, len: 750, key: -1, cls: 3, sal: 0, hit: false, dens: 0, flags: 0 }); this.stepClass[s] = 3; this.src[s] = -1; }
    for (let s = 16; s < 64; s++) { this.stepClass[s] = 0; this.src[s] = 0; }
    this.env.fill(128); for (let k = 0; k < KEYS; k++) this.keySlice[k] = this.spanSlice(k);
    this.keyp = Array.from({ length: KEYS }, keypNeutral);   /* KEYP: a new kit, every key neutral */
    this.rec = { state: 0, key: -1, t: 0, pos: 0 }; this.loopEvents = []; this.looperEvents = 0; this.journal = []; this.redoStack = [];
    this.noBreak = true; this.onCard = 0; this.padStereo = false; this.file = 255; this.set.name = '';
    const g = this.grammar; this.options[O['grammar.mode']] = GRAM.pads; this.setGrammar(GRAM.pads, g);
    this.matGen = (this.matGen + 1) & 0xff; this.bumpGen(); this.padFinish(0);
  }
  /* 0x68 — the firmware's HandlePads */
  padHandle(body, U8, U32) {
    const op = body[0], o = this.padOp, u = this.padUp, room = this.padRoom();
    switch (op) {
      case 0: {
        if (body.length < 7) { U8(2); U32(room); U32(BUF_CAP); return; }
        const key = body[1], ch = body[2], frames = u32(body, 3);
        let st = (key >= KEYS || (ch !== 1 && ch !== 2) || frames < REC_MIN) ? 2 : frames > room ? 6 : 0;
        if (!st && this.padRefused()) st = 3;
        if (st) { U8(st); U32(room); U32(BUF_CAP); return; }
        Object.assign(u, { open: true, key, ch, frames, total: frames * ch * 2, have: 0, start: this.freePos, last: Date.now(), bytes: new Uint8Array(frames * ch * 2) });
        o.state = 1; o.progress = 0; o.key = key; U8(0); U32(room); U32(BUF_CAP); return;
      }
      case 1: {
        if (body.length < 9) return U8(2);
        if (o.state !== 1 || !u.open) return U8(3);
        const total = u32(body, 1), off = u32(body, 5), len = body.length - 9;
        if (total !== u.total || off !== u.have || off + len > u.total) { u.open = false; u.bytes = null; this.padFinish(3); return U8(2); }
        u.bytes.set(body.subarray(9), off); u.have = off + len; u.last = Date.now(); o.progress = Math.floor(u.have * 255 / u.total);
        let done = 0; if (u.have === u.total) { o.state = 2; o.progress = 0; o.job = 'commit'; o.t = Date.now(); done = 1; }
        U8(0); U32(u.have); U8(done); return;
      }
      case 2: if (o.state === 1) { u.open = false; u.bytes = null; this.padFinish(6); } return U8(0);
      case 3: if (this.padRefused() || o.state) return U8(3); o.state = 3; o.key = 255; o.job = 'kit'; o.t = Date.now(); return U8(0);
      default: return U8(2);
    }
  }
  cardFinish(r) { const o = this.cardOp; o.result = r; o.state = 0; o.progress = 0; o.job = null; o.seq = (o.seq + 1) & 0xff; }
  /* the main loop's half, run on each state poll (as the set ops): a quiet transfer drops, the write, the copy */
  cardTick() {
    const o = this.cardOp, u = this.cardUp, now = Date.now();
    if (o.state === 1 && u.open && now - u.last > 5000) { u.open = false; this.cardFinish(9); }
    if (!o.job || this.set.state) return;
    const el = now - o.t, span = o.job === 'write' ? CARD_WRITE_MS : CARD_COPY_MS;
    o.progress = Math.min(255, Math.round(el / span * 255));
    if (el < span) return;
    if (o.job === 'write') {
      const path = 'Audiothurgist/breaks/' + u.name + '.wav', lab = u.name + '.wav';
      this.cardPut(path, u.buf.slice(0, u.total)); u.open = false; u.buf = null;
      let i = this.files.findIndex(f => f.toLowerCase() === lab.toLowerCase()); if (i < 0) { this.files.push(lab); i = this.files.length - 1; }
      o.lastIdx = i < 255 ? i : 255; this.survey = this.cardSurvey(); this.cardFinish(0);
    } else if (o.job === 'copy') {
      let copied = 0, skipped = 0;
      for (const c of this.cardCandidates()) { if (this.cardHas(c.dst)) skipped++; else { this.cardPut(c.dst, this.cardFs.get(c.src)); copied++; } }
      o.copied = copied; o.skipped = skipped; this.cardMerge(); this.survey = this.cardSurvey(); this.cardFinish(copied + skipped ? 0 : 10);
    } else { this.cardMerge(); this.survey = this.cardSurvey(); this.cardFinish(0); }
  }
  /* 0x67 — the firmware's HandleCard */
  cardHandle(body, U8, U32) {
    const op = body[0], o = this.cardOp, u = this.cardUp;
    switch (op) {
      case 0: {
        const n = body.length - 6; if (n < 1 || n > 100) return U8(2);
        const name = new TextDecoder('ascii').decode(body.subarray(6)), total = u32(body, 1);
        if (!HL.breakNameOk(name) || !total) return U8(2);
        if (this.cardBusy() || o.state >= 2) return U8(3);
        if (total > UP_MAX) return U8(6);
        if (!this.cardMounted) return U8(21);
        if (!(body[5] & 1) && this.cardHas('Audiothurgist/breaks/' + name + '.wav')) return U8(20);
        if (this.cardFree < total + 65536) return U8(22);
        Object.assign(u, { open: true, total, have: 0, name, over: !!(body[5] & 1), buf: new Uint8Array(total), last: Date.now() });
        o.state = 1; o.progress = 0; U8(0); U32(UP_MAX); return;
      }
      case 1: {
        if (body.length < 9) return U8(2);
        if (o.state !== 1 || !u.open) return U8(3);
        const total = u32(body, 1), off = u32(body, 5), len = body.length - 9;
        if (total !== u.total || off !== u.have || off + len > u.total) { u.open = false; o.state = 0; o.progress = 0; return U8(2); }
        u.buf.set(body.subarray(9), off); u.have = off + len; u.last = Date.now(); o.progress = Math.floor(u.have * 255 / u.total);
        let done = 0;
        if (u.have === u.total) {
          const b = u.buf, ok = b.length >= 44 && String.fromCharCode(...b.subarray(0, 4)) === 'RIFF' && String.fromCharCode(...b.subarray(8, 12)) === 'WAVE' && [1, 2].includes(u16(b, 22)) && [16, 24].includes(u16(b, 34)) && u32(b, 24) >= 8000;
          if (ok) { o.state = 2; o.job = 'write'; o.t = Date.now(); done = 1; } else { u.open = false; this.cardFinish(7); done = 2; }
        }
        U8(0); U32(u.have); U8(done); return;
      }
      case 2: if (o.state === 1) { u.open = false; o.state = 0; o.progress = 0; } return U8(0);
      case 3: case 4:
        if (this.cardBusy() || o.state) return U8(3);
        if (!this.cardMounted) return U8(21);
        o.job = op === 3 ? 'copy' : 'rescan'; o.t = Date.now(); if (op === 3) { o.state = 3; o.progress = 0; o.copied = o.skipped = 0; }
        return U8(0);
      case 5: {
        const nf = Math.min(255, this.files.length), first = body.length >= 2 ? body[1] : 0; if (first > nf) return U8(2);
        let room = this.opts.maxBody - 4, n = 0; const labs = [];
        for (let i = first; i < nf; i++) { const l = this.files[i].slice(0, 255); if (l.length + 1 > room) break; room -= l.length + 1; labs.push(l); n++; }
        U8(0); U8(nf); U8(first); U8(n); for (const l of labs) { U8(l.length); for (const c of l) U8(c.charCodeAt(0)); } return;
      }
      default: return U8(2);
    }
  }
  get nSlices() { return this.opts.nSlices; }
  get options() { return this.live.subarray(OFF.options, OFF.options + N_OPTIONS); }
  get grammar() { return this.options[O['grammar.mode']]; }
  /* the applied jack map (state byte 175): Options → Jacks, Follow grammar = Voice for Play and Pads */
  get jackMap() { const j = this.options[O.jacks]; return j === 0 ? (this.grammar >= GRAM.play ? 1 : 0) : j - 1; }
  /* KEYS: the voice a pad or CV call started, while it sounds */
  voiceOn(now = Date.now()) { const v = this.voice; return !!(v && now < v.until); }
  /* does the pattern sound at this step? Not while stopped, never in Pads (it has no pattern), a rest is silent */
  patternSounds(step) { return !this.muted && this.grammar !== GRAM.pads && step >= 0 && this.src[step] >= 0; }
  /* tail 263 b0: something sounds now, so a TAP_MARK has a position to take */
  srcValid() { return this.voiceOn() || this.patternSounds(this.curStep()); }
  get nTable() { return this.table.length; }
  /* A synthetic break: one hit per slice (kick / snare / hat / other by class), swung
   * boundaries when asked (odd slices open 15 % late, as the SLICE package does). */
  synthHit(at, len, cls, gain) {
    const b = this.buf; let seed = (at * 2654435761) >>> 0;
    const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296 * 2 - 1; };
    for (let i = 0; i < len && at + i < b.length; i++) {
      const t = i / SR; let v;
      if (cls === 0) v = Math.sin(2 * Math.PI * (55 + 40 * Math.exp(-t * 30)) * t) * Math.exp(-t * 9);
      else if (cls === 1) v = (rnd() * 0.7 + Math.sin(2 * Math.PI * 190 * t) * 0.5) * Math.exp(-t * 14);
      else if (cls === 2) v = rnd() * Math.exp(-t * 45);
      else v = Math.sin(2 * Math.PI * 330 * t) * Math.exp(-t * 6) * 0.6;
      b[at + i] += v * gain;
    }
  }
  newMaterial() {
    const n = this.nSlices;
    for (let s = 0; s < n; s++) { this.stepClass[s] = s % 4 === 0 ? 0 : s % 4 === 2 ? 1 : (s % 8 === 7 ? 3 : 2); this.src[s] = s; }
    for (let s = 0; s < n; s++) for (let p = 0; p < 8; p++) { const a = Math.exp(-p / 2.5) * (this.stepClass[s] === 0 ? 0.9 : 0.6); this.env[s * 16 + p * 2] = Math.round(128 - a * 127); this.env[s * 16 + p * 2 + 1] = Math.round(128 + a * 127); }
    /* Material + grid; recordings past the old material are dropped (new material). */
    this.bufLen = n * SLICE_LEN; this.freePos = this.bufLen; this.buf.fill(0);
    const sw = this.opts.swing ? Math.round(SLICE_LEN * 0.15) : 0;
    this.table = []; this.hits = [];
    for (let s = 0; s < n; s++) {
      const start = s * SLICE_LEN + (s & 1 ? sw : 0), end = s + 1 < n ? (s + 1) * SLICE_LEN + ((s + 1) & 1 ? sw : 0) : this.bufLen;
      const cls = this.stepClass[s], hit = s % 8 !== 5;
      this.table.push({ start, len: end - start, key: -1, cls, sal: cls === 0 ? 240 : cls === 1 ? 200 : 120, hit, dens: cls === 2 ? 220 : cls === 0 ? 60 : 130, flags: 0 });
      if (hit) { this.synthHit(start, Math.min(end - start, 9000), cls, cls === 0 ? 0.95 : 0.7); this.hits.push(start); }
      if (s % 4 === 3) { const g = start + Math.round((end - start) * 0.5); this.synthHit(g, 2400, 2, 0.25); this.hits.push(g); }   /* a ghost hat */
    }
    for (let k = 0; k < KEYS; k++) this.keySlice[k] = this.spanSlice(k);
    this.rec = { state: 0, key: -1, t: 0, pos: 0 };
    this.matGen = (this.matGen + 1) & 0xff; this.bumpGen();
  }
  /* PADS: a loose break from the card — new material, on the card, a break again (Sampling decides the planes) */
  loadBreak() { if (this.noBreak) this.opts.nSlices = 32; this.newMaterial(); this.keyp = Array.from({ length: KEYS }, keypNeutral); /* KEYP: new material, every key neutral */ this.noBreak = false; this.onCard = this.bufLen; this.padStereo = false; this.stereo = this.options[O.sampling] === 1; if (this.stereo) { this.bufR = this.bufR || new Float32Array(BUF_CAP); this.bufR.set(this.buf.subarray(0, this.bufLen)); } this.journal = []; this.redoStack = []; }
  /* The span mapping: key k → grid slice k · nSlices / span (Engine::SpanSlice). */
  spanSlice(key) { const n = this.nSlices, span = Math.min(n, KEYS), kk = key >= span ? span - 1 : key; return Math.floor(kk * n / span); }
  /* EDIT: the first tombstone's slot past the grid, else the end (−1 = full) — Engine::FreeSlot. */
  freeSlot() { for (let t = this.nSlices; t < this.nTable; t++) if (this.table[t].flags & F_TOMB) return t; return this.nTable < MAX_TABLE ? this.nTable : -1; }
  isTomb(t) { return !!(this.table[t].flags & F_TOMB); }
  isMarker(t) { return t >= this.nSlices && t < this.nTable; }
  /* CommitSlice's twin: publish [freePos, freePos + len) as key's entry (a tone burst is synthesised into it).
   * A second take into a key replaces its recording; a new one takes a tombstone's slot, else appends. */
  recordKey(key, seconds, cls = 1) {
    const room = BUF_CAP - this.freePos; if (room < REC_MIN) return -1;
    const len = Math.max(REC_MIN, Math.min(Math.round(seconds * SR), room));
    const start = this.freePos; this.buf.fill(0, start, start + len);
    this.synthHit(start, Math.min(len, 12000), cls, 0.9); if (len > 20000) this.synthHit(start + 16000, Math.min(len - 16000, 12000), 2, 0.5);
    const e = { start, len, key, cls, sal: 255, hit: true, dens: cls === 2 ? 210 : 90, flags: 0 };
    let t = this.table.findIndex((x, i) => i >= this.nSlices && x.key === key);
    if (t < 0) { t = this.freeSlot(); if (t < 0) return -1; if (t === this.nTable) this.table.push(e); else this.table[t] = e; } else this.table[t] = e;
    this.keySlice[key] = t; this.freePos = start + len; this.bumpGen(); return t;
  }
  /* Engine::ClearKey: the recording STAYS, keyless and at its index (nothing shifts — the looper's events, the
   * page's indices and the key table all hold); the key falls back to its span slice. */
  clearKey(key) {
    const t = this.table.findIndex((x, i) => i >= this.nSlices && x.key === key); if (t < 0) return false;
    this.table[t].key = -1;
    this.keySlice[key] = this.spanSlice(key);
    this.bumpGen(); return true;
  }
  /* ── EDIT: the ops, with core/bb_edit.h's rules (docs/packages/slice-edit-core.md). Each returns an EditStatus
   * name; applyEdit maps it to the HostLink status, checks the gen first and bumps it on success. ── */
  refreshEntry(t) {   /* the hit flag from the onset table: an onset in the first quarter, or within 10 ms before */
    const e = this.table[t]; if (e.start >= this.bufLen) return;
    e.hit = this.hits.some(p => (p >= e.start && p - e.start <= e.len / 4) || (p < e.start && e.start - p <= 480));
  }
  edSetBoundary(i, pos) {
    const n = this.nSlices, T = this.table;
    if (i <= 0 || i >= n) return 'NotGrid';
    if (pos < 0 || pos >= this.bufLen) return 'BadArgs';
    const lo = T[i - 1].start + GRID_MIN, hi = T[i].start + T[i].len - GRID_MIN;
    if (pos < lo || pos > hi) return 'BadArgs';
    const end = T[i].start + T[i].len;
    T[i - 1].len = pos - T[i - 1].start; T[i].start = pos; T[i].len = end - pos; T[i].flags |= F_ORANGE;
    this.refreshEntry(i - 1); this.refreshEntry(i); return 'Ok';
  }
  edSetEntry(t, start, len) {
    if (!this.isMarker(t)) return 'NotMarker'; if (this.isTomb(t)) return 'Tomb';
    if (start < 0 || len < REC_MIN || start > this.freePos || len > this.freePos - start) return 'BadArgs';
    const e = this.table[t]; e.start = start; e.len = len; e.flags |= F_ORANGE; this.refreshEntry(t); return 'Ok';
  }
  metaAt(start) {   /* AnalyseEntry's stand-in: the class / salience of the grid slice the span opens in */
    const k = this.table.findIndex((e, i) => i < this.nSlices && start >= e.start && start < e.start + e.len);
    return k >= 0 ? { cls: this.table[k].cls, sal: this.table[k].sal, dens: this.table[k].dens } : { cls: 3, sal: 0, dens: 0 };
  }
  edAdd(key, start, len, flags) {
    if (start < 0 || len < REC_MIN || start > this.freePos || len > this.freePos - start) return 'BadArgs';
    const t = this.freeSlot(); if (t < 0) return 'Full';
    const e = Object.assign({ start, len, key: -1, hit: false, flags: flags & F_ORANGE }, this.metaAt(start));
    if (t === this.nTable) this.table.push(e); else this.table[t] = e;
    this.refreshEntry(t); if (key >= 0 && key < KEYS) this.keySlice[key] = t;
    this.lastIdx = t; return 'Ok';
  }
  edSplit(t, pos) {
    if (!this.isMarker(t)) return 'NotMarker'; if (this.isTomb(t)) return 'Tomb';
    const e = this.table[t], end = e.start + e.len;
    if (pos - e.start < REC_MIN || end - pos < REC_MIN) return 'BadArgs';
    const u = this.freeSlot(); if (u < 0) return 'Full';
    const r = { start: pos, len: end - pos, key: -1, cls: e.cls, sal: e.sal, hit: e.hit, dens: e.dens, flags: F_ORANGE };
    if (u === this.nTable) this.table.push(r); else this.table[u] = r;
    e.len = pos - e.start; e.flags |= F_ORANGE; this.refreshEntry(t); this.refreshEntry(u);
    this.lastIdx = u; return 'Ok';
  }
  edMerge(t) {
    if (!this.isMarker(t)) return 'NotMarker'; if (this.isTomb(t)) return 'Tomb';
    const T = this.table; let u = -1;
    for (let v = this.nSlices; v < this.nTable && u < 0; v++) if (v !== t && !this.isTomb(v) && T[v].start + T[v].len === T[t].start) u = v;
    for (let v = t - 1; v >= this.nSlices && u < 0; v--) if (!this.isTomb(v)) u = v;
    if (u < 0) return 'BadArgs';
    const a = Math.min(T[u].start, T[t].start), e = Math.max(T[u].start + T[u].len, T[t].start + T[t].len);
    T[t].start = a; T[t].len = e - a; T[t].flags |= F_ORANGE; if (T[t].key < 0) T[t].key = T[u].key;
    for (let k = 0; k < KEYS; k++) if (this.keySlice[k] === u) this.keySlice[k] = t;
    T[u].key = -1; T[u].len = 0; T[u].flags = F_TOMB; T[u].hit = false;
    this.refreshEntry(t); return 'Ok';
  }
  edTomb(t) {
    if (!this.isMarker(t)) return 'NotMarker'; if (this.isTomb(t)) return 'Tomb';
    for (let k = 0; k < KEYS; k++) if (this.keySlice[k] === t) this.keySlice[k] = this.spanSlice(k);
    const e = this.table[t]; e.key = -1; e.len = 0; e.flags = F_TOMB; e.hit = false; return 'Ok';
  }
  edSetKey(key, t) {
    if (key < 0 || key >= KEYS || t < 0 || t >= this.nTable) return 'BadArgs'; if (this.isTomb(t)) return 'Tomb';
    this.keySlice[key] = t; return 'Ok';
  }
  edSwapKeys(a, b) {
    if (a < 0 || a >= KEYS || b < 0 || b >= KEYS || a === b) return 'BadArgs';
    const ta = this.keySlice[a], tb = this.keySlice[b]; this.keySlice[a] = tb; this.keySlice[b] = ta;
    if (this.isMarker(ta) && this.table[ta].key === a) this.table[ta].key = b;
    if (this.isMarker(tb) && this.table[tb].key === b) this.table[tb].key = a;
    return 'Ok';
  }
  edNudge(delta, which) {
    if (delta === 0) return 'BadArgs';
    const n = this.nSlices, T = this.table;
    if (which === 0) {
      if (n < 2 || GRID_MIN * n > this.bufLen) return 'BadArgs';
      for (let i = 1; i < n; i++) { const lo = T[i - 1].start + GRID_MIN, hi = this.bufLen - GRID_MIN * (n - i); T[i].start = Math.max(lo, Math.min(hi, T[i].start + delta)); T[i].flags |= F_ORANGE; }
      for (let i = 0; i < n; i++) T[i].len = (i + 1 < n ? T[i + 1].start : this.bufLen) - T[i].start;
      for (let i = 0; i < n; i++) this.refreshEntry(i);
      return 'Ok';
    }
    if (which === 1) {
      let moved = 0;
      for (let t = n; t < this.nTable; t++) { if (this.isTomb(t)) continue; T[t].start = Math.max(0, Math.min(this.freePos - T[t].len, T[t].start + delta)); T[t].flags |= F_ORANGE; this.refreshEntry(t); moved++; }
      return moved ? 'Ok' : 'BadArgs';
    }
    return 'BadArgs';
  }
  /* The strength of an onset: the salience of the grid slice it falls in (the onset table has positions only). */
  onsetStrength(p) { const k = this.table.findIndex((e, i) => i < this.nSlices && p >= e.start && p < e.start + e.len); return k >= 0 ? this.table[k].sal : 0; }
  /* FROM_ONSETS: the grey markers inside the break go (recordings, orange markers and anything past the break stay),
   * then one grey marker per accepted onset spanning to the next accepted one; stops when the table fills. */
  edFromOnsets(thresh, minGap) {
    for (let t = this.nSlices; t < this.nTable; t++) { const e = this.table[t]; if (this.isTomb(t) || (e.flags & F_ORANGE) || e.key >= 0 || e.start >= this.bufLen) continue; this.edTomb(t); }
    const acc = []; let last = -1;
    for (const p of this.hits) { if (p < 0 || p >= this.bufLen || this.onsetStrength(p) < thresh) continue; if (last >= 0 && p - last < minGap) continue; acc.push(p); last = p; }
    let count = 0, st = 'Ok';
    for (let j = 0; j < acc.length; j++) { const end = j + 1 < acc.length ? acc[j + 1] : this.bufLen; if (end - acc[j] < REC_MIN) continue; st = this.edAdd(-1, acc[j], end - acc[j], 0); if (st !== 'Ok') break; count++; }
    this.lastCount = count;
    return st === 'Ok' || (st === 'Full' && count > 0) ? 'Ok' : st;
  }
  /* The audio thread's position snapshot (tail 258): the pad / CV voice while it sounds, else the current step's source
   * at the step's phase while the pattern sounds — and while nothing sounds it HOLDS where it was (a stopped module,
   * a rest, Pads between hits). The fake used to advance it regardless, which hid the stacked-markers bug: every tap
   * on a silent module landed on the same spot. */
  srcPos() {
    const now = Date.now(), v = this.voice;
    if (v && now < v.until) return this.lastSrcPos = Math.min(this.freePos - 1, v.start + Math.min(v.len - 1, Math.floor((now - v.t) / 1000 * SR)));
    const beat = (now - this.t0) / 60000 * this.opts.bpm * this.mult * 4, step = Math.floor(beat) % this.nSlices, frac = beat - Math.floor(beat);
    if (!this.patternSounds(step)) return this.lastSrcPos;
    const e = this.table[this.src[step]]; if (!e) return this.lastSrcPos;
    return this.lastSrcPos = e.start + Math.floor(frac * e.len);
  }
  /* TAP_MARK: core/bb_edit.h's TapMarkAt, ported — the snapshot minus the allowance, snapped to the nearest onset within
   * ±snap (the earlier wins a tie), to the next onset or 250 ms, ≥ REC_MIN, shortened to freePos. */
  tapMarkAt(srcPos, allow, snap) {
    let p = Math.max(0, srcPos - allow), best = -1, bestD = snap + 1;
    this.hits.forEach((o, i) => { const d = Math.abs(o - p); if (d < bestD) { bestD = d; best = i; } });
    if (best >= 0) p = this.hits[best];
    let len = TAP_MAX; for (const o of this.hits) if (o > p && o - p < len) len = o - p;
    if (len < REC_MIN) len = REC_MIN;
    if (p + len > this.freePos) { len = this.freePos - p; if (len < REC_MIN) { len = REC_MIN; p = this.freePos - len; if (p < 0) { p = 0; len = this.freePos; } } }
    return { start: p, len };
  }
  /* ── KEYS (v0.13): the marker ops. A marker is a live keyless entry past the grid (grey or orange); a recording is
   * an entry with a key of its own, and the key holding it is never taken over. ── */
  /* a marker is a span of the break: a recording (past the break, even one whose key was cleared) never is */
  liveMarkers() { const out = []; for (let t = this.nSlices; t < this.nTable; t++) { const e = this.table[t]; if (!this.isTomb(t) && e.key < 0 && e.len > 0 && e.start < this.bufLen) out.push(t); } return out; }
  recordingKey(k) { const t = this.keySlice[k]; return t >= this.nSlices && t < this.nTable && !this.isTomb(t) && this.table[t].key >= 0; }
  /* KEYS_FROM_MARKERS: the markers in time order onto keys 0, 1, 2… skipping the keys that hold a recording; the keys
   * past the last marker go back to their span slices */
  edKeysFromMarkers() {
    const ms = this.liveMarkers().sort((a, b) => this.table[a].start - this.table[b].start || a - b); let k = 0, n = 0;
    for (const t of ms) { while (k < KEYS && this.recordingKey(k)) k++; if (k >= KEYS) break; this.keySlice[k++] = t; n++; }
    for (; k < KEYS; k++) if (!this.recordingKey(k)) this.keySlice[k] = this.spanSlice(k);
    this.lastCount = n; return 'Ok';
  }
  /* CLEAR_MARKERS: every live marker tombstoned; their keys back to their grid slices */
  edClearMarkers() { const ms = this.liveMarkers(); for (const t of ms) this.edTomb(t); this.lastCount = ms.length; return 'Ok'; }
  /* AUTOSLICE(k): the grey markers inside the break replaced by the first k onsets of a seeded weighted draw without
   * replacement — Efraimidis–Spirakis, key u^(1/w) with w = the onset's salience — so for one seed the k-set is nested
   * in the (k+1)-set; each marker spans to the next one drawn (the last to the break's end). Orange markers stay. */
  autoOrder(seed) {
    let s = (seed * 2654435761) >>> 0 || 1; const rnd = () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return (s + 0.5) / 4294967296; };
    const orange = this.liveMarkers().filter(t => this.table[t].flags & F_ORANGE).map(t => this.table[t].start);   /* onsets within 10 ms of an orange marker are skipped */
    return this.hits.filter(p => p > 0 && p < this.bufLen && !orange.some(o => Math.abs(o - p) <= TAP_REUSE)).map(p => ({ p, key: Math.pow(rnd(), 1 / Math.max(1, this.onsetStrength(p))) })).sort((a, b) => b.key - a.key).map(x => x.p);
  }
  edAutoslice(k, reroll) {
    if (k > AUTO_MAX) return 'BadArgs';
    if (reroll) this.autoSeed = (this.autoSeed * 1103515245 + 12345) >>> 0 || 7;
    /* the grey markers already there are re-spanned in place (the keys on them stay), the surplus deleted, the rest added */
    const greys = this.liveMarkers().filter(t => !(this.table[t].flags & F_ORANGE)).sort((a, b) => this.table[a].start - this.table[b].start || a - b);
    const pick = this.autoOrder(this.autoSeed).slice(0, k).sort((a, b) => a - b), spans = [];
    for (let j = 0; j < pick.length; j++) { const end = j + 1 < pick.length ? pick[j + 1] : this.bufLen; if (end - pick[j] >= REC_MIN) spans.push([pick[j], end - pick[j]]); }
    let count = 0, st = 'Ok';
    spans.forEach(([s0, len], j) => { if (st !== 'Ok') return; if (j < greys.length) { const e = this.table[greys[j]]; e.start = s0; e.len = len; this.refreshEntry(greys[j]); Object.assign(e, this.metaAt(s0)); count++; } else { st = this.edAdd(-1, s0, len, 0); if (st === 'Ok') count++; } });
    for (let j = spans.length; j < greys.length; j++) this.edTomb(greys[j]);
    this.autoK = k; this.lastCount = count;
    return st === 'Ok' || (st === 'Full' && count > 0) ? 'Ok' : st;
  }
  /* TAP_MARK, v0.13: refused (BAD_STATE) while nothing sounds; within 10 ms of a live marker's start it selects that
   * marker (the panel's selection, tail 257) instead of stacking another — no edit, no undo step, the reply's flags
   * byte b0 says so. */
  edTap(snapMs, allowMs) {
    if (this.opts.keys && !this.srcValid()) return { st: 'Silent' };
    const m = this.tapMarkAt(this.srcPos(), allowMs * MS, snapMs * MS);
    if (this.opts.keys) { const near = this.liveMarkers().find(t => Math.abs(this.table[t].start - m.start) <= TAP_REUSE); if (near != null) { this.selSlice = near; return { st: 'Ok', reused: near }; } }
    const st = this.edAdd(-1, m.start, m.len, F_ORANGE); return { st };
  }
  editBusy() { return !!(this.rec.state || this.set.state || this.set.pending || this.padOp.state >= 2 || this.padOp.job); }
  /* EDIT-UNDO: a snapshot of what an undo restores (the table and the key map), and the generation rule. */
  tableSnap() { return { table: this.table.map(e => Object.assign({}, e)), keySlice: Int8Array.from(this.keySlice), keyp: this.keyp.map(p => Object.assign({}, p)) }; }   /* KEYP: and the keys' settings */
  tableRestore(sn) { this.table = sn.table.map(e => Object.assign({}, e)); this.keySlice.set(sn.keySlice); if (sn.keyp) this.keyp = sn.keyp.map(p => Object.assign({}, p)); }
  journalCheck() { if (this.tableGen !== this.journalGen) { this.journal = []; this.redoStack = []; } }   /* something went past the journal */
  journalPush(before) { this.journal.push(before); if (this.journal.length > UNDO_DEPTH) this.journal.shift(); this.redoStack = []; this.journalGen = this.tableGen; }
  undoStep(redo) {
    this.journalCheck();
    const from = redo ? this.redoStack : this.journal, to = redo ? this.journal : this.redoStack;
    if (!from.length) return 'BadArgs';
    const now = this.tableSnap(); this.tableRestore(from.pop()); to.push(now);
    this.bumpGen(); this.journalGen = this.tableGen; return 'Ok';
  }
  /* the ops that change the markers — in Pads each one carries keys-from-markers with it (the same undo step) */
  static MARKER_OPS = new Set([17, 18, 19, 20, 21, 24, 26, 29, 34, 36]);
  /* ACTION 16..26, 29..31, 33, 34, 36: parse, gate, apply, answer [status, tableGen, third (, flags)]. */
  applyEdit(op, body) {
    const need = EDIT_NEED[op - 16];
    if (body.length < 1 + need) return [2];
    const gen = body.length > 1 + need ? body[1 + need] : 0;
    const b = body, U16 = at => u16(b, at), I16 = at => { const v = u16(b, at); return v > 0x7fff ? v - 0x10000 : v; }, U32 = at => u32(b, at);
    if (this.editBusy()) return [3];
    if (!this.bufLen) return [3];
    if (gen !== 0 && gen !== this.tableGen) return [3];   /* Stale */
    this.lastIdx = -1; this.lastCount = null; let st, third = null, flags = null, join = false;
    if (op === 30 || op === 31) {   /* EDIT-UNDO: one journal step; the gen bumps inside */
      st = this.undoStep(op === 31);
      return st === 'Ok' ? [0, this.tableGen] : [EDIT_STATUS[st]];
    }
    const before = this.tableSnap();
    switch (op) {
      case 16: st = this.edSetBoundary(b[1], U32(2)); break;
      case 17: st = this.edSetEntry(b[1], U32(2), U32(6)); break;
      case 18: st = this.edAdd(b[1] < KEYS ? b[1] : -1, U32(2), U32(6), b[10]); third = this.lastIdx; break;
      case 19: st = this.edSplit(b[1], U32(2)); third = this.lastIdx; break;
      case 20: st = this.edMerge(b[1]); break;
      case 21: st = this.edTomb(b[1]); break;
      case 22: st = this.edSetKey(b[1], b[2]); break;
      case 23: st = this.edSwapKeys(b[1], b[2]); break;
      case 24: st = this.edNudge(I16(1) * MS, b[3]); break;
      case 25: st = !this.isMarker(b[1]) ? 'NotMarker' : this.isTomb(b[1]) ? 'Tomb' : this.edSetBoundary(b[2], this.table[b[1]].start); break;
      case 26: st = this.edFromOnsets(b[1], U16(2) * MS); third = this.lastCount; break;
      case 29: {
        const r = this.edTap(b[1], b[2]); st = r.st;
        if (st === 'Ok' && r.reused != null) return [0, this.tableGen, r.reused, 1];   /* selected, nothing changed */
        third = this.lastIdx; if (this.opts.keys) flags = 0; break; }
      case 33: st = this.edKeysFromMarkers(); third = this.lastCount; if (!third) return [0, this.tableGen, 0]; break;
      case 34: st = this.edClearMarkers(); third = this.lastCount; if (!third) return [0, this.tableGen, 0]; break;
      case 36: st = b[1] > AUTO_MAX ? 'BadArgs' : this.edAutoslice(b[1], !!(b[2] & 1)); third = this.lastCount; join = !!(b[2] & 2) && this.autoGen === this.tableGen && this.journal.length > 0; break;
      case 37: {   /* KEYP: KEY_PARAMS(key, i8 gain, i16 cents, plays, rat, flags b0 join) — the join folds into THIS key's previous step only */
        const key = b[1], gain = b[2] > 127 ? b[2] - 256 : b[2], cents = I16(3), plays = b[5], rat = b[6], fl = b[7];
        if (key >= KEYS || gain < KEYP.gainMin || gain > KEYP.gainMax || cents < -KEYP.centsMax || cents > KEYP.centsMax || plays > KEYP.playsMax || rat > KEYP.ratMax || (fl & ~1)) { st = 'BadArgs'; break; }
        this.keyp[key] = { gain, plays, cents, rat }; st = 'Ok'; before.kpKey = key;
        const top = this.journal[this.journal.length - 1];
        join = !!(fl & 1) && !!top && top.kpKey === key && !this.redoStack.length && this.journalGen === this.tableGen;
        break; }
      case 38: {   /* CHROMA: SPREAD_KEYS(root, flags b0 keep pads) — Engine::SpreadPlan's rule; a spread already in place moves nothing */
        const root = b[1], fl = b[2];
        if (root >= KEYS || (fl & ~1)) { st = 'BadArgs'; break; }
        const plan = this.spreadPlan(root, !!(fl & 1));
        if (!plan) { st = 'BadArgs'; break; }
        if (!plan.changes) return [0, this.tableGen, plan.n];   /* no empty undo step */
        this.keySlice.set(plan.keys); this.keyp = plan.kp; st = 'Ok'; third = plan.n;
        break; }
      default: st = 'BadOp';
    }
    const status = EDIT_STATUS[st];
    if (status !== 0) { this.tableRestore(before); return [status]; }
    if (this.grammar === GRAM.pads && FakeDevice.MARKER_OPS.has(op)) { const c = this.lastCount; this.edKeysFromMarkers(); this.lastCount = c; }   /* Pads: keys follow the markers */
    /* JOIN (AUTOSLICE flags b1): the previous AUTOSLICE's undo record keeps its before-image — a whole drag is one step */
    if (join) { this.bumpGen(); this.journalGen = this.tableGen; } else { this.journalCheck(); this.bumpGen(); this.journalPush(before); }
    if (op === 36) this.autoGen = this.tableGen;
    this.edits = (this.edits || 0) + 1;
    const out = third == null ? [0, this.tableGen] : [0, this.tableGen, third < 0 ? 255 : third];
    if (flags != null) out.push(flags);
    return out;
  }
  /* KEYS: PLAY_KEY — the key's entry sounds as a CV call would (plain style): one-shot for its length, or until the up
   * when Key Plays = Held. In Play the call writes the loop (one looper event, B2's undo takes it back); in Pads and
   * the pattern grammars nothing is written. keyFlash / keySeq and callFlash (the entry) show it on every page. */
  playKey(key, down) {
    if (key >= KEYS) return 2;
    if (!this.bufLen) return 3;
    const now = Date.now();
    if (!down) { if (this.voice && this.voice.key === key && this.voice.until === Infinity) this.voice.until = now; return 0; }
    const t = this.keySlice[key], e = this.table[t]; if (!e || !e.len) return 3;
    const held = this.keyHeld(key), loop = this.keyp[key].plays === 3;   /* KEYP: the key's own Plays, else the option */
    this.voice = { key, t: now, start: e.start, len: held && !loop ? Math.max(e.len, this.freePos - e.start) : e.len, until: held ? Infinity : now + e.len / SR * 1000, loop };
    this.keyFlash = key; this.keySeq = (this.keySeq + 1) & 0xff; this.callFlash = t; this.callT = 6;
    if (this.grammar === GRAM.play && t < 128) { const s = this.curStep(); this.loopEvents.push({ s, prev: this.src[s] }); this.src[s] = t; this.looperEvents = Math.min(255, this.looperEvents + 1); }
    return 0;
  }
  /* CHROMA: the spread of `root` — every key but the root on its entry, (j − root) × 100 + the root's cents (±2400), the
   * root's gain / Plays / rate; keepPads leaves a key playing a recording other than the root's. null = a bad root. */
  spreadPlan(root, keepPads) {
    const t = this.keySlice[root]; if (t < 0 || t >= this.nTable || this.isTomb(t)) return null;
    const base = this.keyp[root], keys = Int8Array.from(this.keySlice), kp = this.keyp.map(p => Object.assign({}, p)); let n = 0, changes = false;
    for (let j = 0; j < KEYS; j++) {
      if (j === root) continue; const u = this.keySlice[j];
      if (keepPads && u !== t && u >= this.nSlices && u < this.nTable && !this.isTomb(u) && this.table[u].key >= 0) continue;
      const p = { gain: base.gain, plays: base.plays, cents: Math.max(-KEYP.centsMax, Math.min(KEYP.centsMax, (j - root) * 100 + base.cents)), rat: base.rat | 0 };
      if (keys[j] !== t || JSON.stringify(p) !== JSON.stringify({ gain: kp[j].gain, plays: kp[j].plays, cents: kp[j].cents, rat: kp[j].rat | 0 })) changes = true;
      keys[j] = t; kp[j] = p; n++;
    }
    return { keys, kp, n, changes };
  }
  /* KEYP: does a call through `key` hold? Follow → Options → Key Plays; One-shot never; Held, Loop and Ratchet always */
  keyHeld(key) { const p = this.keyp[key] ? this.keyp[key].plays : 0; return p === 0 ? this.options[O['key.plays']] === 1 : p !== 1; }
  /* LOOPER in Play: B2 / ACTION 12 takes the last write back, B3 / ACTION 13 clears the loop */
  looperUndo() { const ev = this.loopEvents.pop(); if (ev) this.src[ev.s] = ev.prev; this.looperEvents = Math.max(0, this.looperEvents - 1); }
  looperClear() { if (this.grammar === GRAM.play) this.src.fill(-1); this.loopEvents = []; this.looperEvents = 0; }
  /* a grammar change: Play starts from an empty loop, the others draw a fill */
  setGrammar(g, before) {
    if (g !== GRAM.pads) this.lastDrum = g;
    if (g === before) return; if (g === GRAM.play) { this.src.fill(-1); this.loopEvents = []; this.looperEvents = 0; } else this.dice();
    /* entering Pads puts the markers on the keys once, as its own undo step */
    if (g === GRAM.pads && this.opts.keys && this.liveMarkers().length && this.bufLen) { const sn = this.tableSnap(); this.journalCheck(); this.edKeysFromMarkers(); this.bumpGen(); this.journalPush(sn); }
  }
  /* the J4 / J5 jacks: a slow stepped sequence on J4 (one dip below 0 V, which clamps to key 0), J5's style in the
   * Voice map (255 = the Brain map, where J5 is the gate) */
  j4mV(now = Date.now()) { const seq = [0, 3, 7, 12, -2, 5, 9, 14], i = Math.floor((now - this.t0) / 1500) % seq.length; return Math.round(seq[i] * 1000 / 12) + ((now >> 4) % 3) - 1; }
  j4Key(mv) { return Math.max(0, Math.min(KEYS - 1, Math.round(mv * 12 / 1000))); }
  /* ACTION 27 / 28: the audition voice, ten a second — inside the window the request is held (a later one replaces)
   * and goes out on the next state poll after the window ends, as the firmware's OnPoll does. */
  postAudition(a) { this.audition = a; this.auditionAt = Date.now(); this.auditionPosted++; }
  auditionTick() { if (this.auditionHeld && Date.now() - this.auditionAt >= 100) { const a = this.auditionHeld; this.auditionHeld = null; this.postAudition(a); } }
  /* Advance the take's state machine (called per state poll). */
  recTick() {
    const r = this.rec, now = Date.now(); if (!r.state) { this.inLevel = Math.max(0, this.inLevel - 40); return; }
    this.inLevel = 90 + Math.floor(Math.random() * 130);
    if (r.state === 1 && now - r.t > 700) { r.state = 2; r.t = now; r.pos = this.freePos; }
    else if (r.state === 2) { r.pos = Math.min(BUF_CAP, this.freePos + Math.round((now - r.t) / 1000 * SR)); if (now - r.t > 1200 || r.pos >= BUF_CAP) { r.state = 3; r.t = now; } }
    else if (r.state === 3 && now - r.t > 300) { this.recordKey(r.key, (r.pos - this.freePos) / SR, 1 + (r.key % 3)); r.state = 0; r.key = -1; }
  }
  get clkFlags() {
    if (this.ext !== this._lastExt) { this._lastExt = this.ext; this.extTrans = (this.extTrans + 1) & 0xff; }
    return (this.p2Engaged ? 1 : 0) | (this.pending ? 2 : 0) | (this.aligned ? 4 : 0) | ((this.extTrans & 15) << 4);
  }
  /* SCOPE: the ten channels as functions of the module's virtual sample clock, never
   * materialised (a 64 K ring per request would be too slow here) — the step's class
   * plays a kick / snare / hat / tone burst on out L (out R a little different), a
   * 110 Hz saw sits on in L and a 440 Hz tone with tremolo on in R (both silent with
   * opts.inputs = false); muted stops the outputs. The six CV jacks follow their Brain-
   * map roles: J3 a 5 ms clock pulse per step (only while `ext`), J4 a 0.3 Hz ±5 V LFO
   * (something patched into v/oct), J5 a 50 % gate per step, J6 / J7 / J8 the kick /
   * snare / hat gates (6 ms of 5 V on their class's steps) — what the DACs drive reads
   * back on the ADC. Positions are on the firmware's clock (wrap 24·2^26; cv = pos / 24). */
  scopePos() { const p = Math.floor((Date.now() - this.t0) / 1000 * SR); return (p - p % SC.cvDiv) % SC.wrap; }
  scopeStep(p) { const stepLen = Math.max(1, Math.round(60 / (this.opts.bpm * this.mult) / 4 * SR)), stepIdx = Math.floor(p / stepLen); return { stepLen, stepIdx, t: (p - stepIdx * stepLen) / SR, step: ((stepIdx % this.nSlices) + this.nSlices) % this.nSlices }; }
  scopeSample(c, p) {
    const { stepIdx, t, step } = this.scopeStep(p);
    const hash = q => (Math.imul(q | 0, 1664525) + 1013904223 >>> 0) / 4294967296 * 2 - 1;
    if (c >= 2) {
      if (!this.opts.inputs) return 0;
      return c === 2 ? 0.45 * (2 * (((p * 110 / SR) % 1) + 1) % 1 - 1) : 0.3 * Math.sin(2 * Math.PI * 440 * p / SR) * (0.75 + 0.25 * Math.sin(2 * Math.PI * 3 * p / SR));
    }
    const src = this.src[step], cls = src < 0 ? -1 : this.stepClass[src]; let v = 0;
    if (this.patternSounds(step) && cls >= 0) {
      if (cls === 0) v = Math.sin(2 * Math.PI * (55 + 40 * Math.exp(-t * 30)) * t) * Math.exp(-t * 9) * 0.9;
      else if (cls === 1) v = (hash(p) * 0.7 + Math.sin(2 * Math.PI * 190 * t) * 0.5) * Math.exp(-t * 14) * 0.6;
      else if (cls === 2) v = hash(p) * Math.exp(-t * 45) * 0.5;
      else v = Math.sin(2 * Math.PI * 330 * t) * Math.exp(-t * 6) * 0.5;
    }
    void stepIdx;
    return c === 0 ? v : v * 0.8 + 0.08 * Math.sin(2 * Math.PI * 1000 * p / SR);
  }
  /* a cv jack's volts at cv index k (one per block) */
  scopeCvVolts(j, k) {
    const p = k * SC.cvDiv, { t, step } = this.scopeStep(p), src = this.src[step], cls = src < 0 ? -1 : this.stepClass[src];
    switch (j) {
      case 0: return this.ext && t < 0.005 ? 5 : 0.01 * Math.sin(k);
      case 1: return 5 * Math.sin(2 * Math.PI * 0.3 * p / SR);
      case 2: return t < 0.0625 ? 5 : 0;
      default: return (this.patternSounds(step) && cls === j - 3 && t < 0.006) ? 5 : 0.02;
    }
  }
  scopeAud(c, p) { return Math.trunc(Math.max(-1, Math.min(1, this.scopeSample(c, p))) * 127); }
  scopeCv(j, k) { return HL.scopeCvCode(this.scopeCvVolts(j, k)); }
  /* core/bb_scope.h's ScopeCrossing, ported: the most recent rising crossing of `level` that leaves `span`, Schmitt ±hyst, walked backwards */
  scopeCrossing(get, usable, wrap, wpos, span, level) {
    const hi = level + SC.hyst, lo = level - SC.hyst, prev = p => p === 0 ? wrap - 1 : p - 1;
    if (span < 1 || span > usable) return -1;
    let p = prev(wpos), next = get(p), age = 1, left = usable - 1, high = false;
    for (;;) {
      if (left <= 0) return -1;
      let q = prev(p), v = get(q); left--; age++;
      if (next >= hi) high = true;
      if (v < level) {
        if (next >= level && high && age - 1 >= span) {
          const cand = p;
          while (!(v <= lo || v >= hi) && left > 0) { q = prev(q); v = get(q); left--; age++; }
          if (v <= lo) return cand;
          if (v < hi) return -1;
          p = q; next = v; high = true; continue;
        }
        high = false;
      }
      p = q; next = v;
    }
  }
  dice() { const n = this.nSlices; for (let s = 0; s < n; s++) this.src[s] = Math.random() < 0.3 ? -1 : Math.floor(Math.random() * n); this.histN = Math.min(16, this.histN + 1); }
  curStep() { const beat = (Date.now() - this.t0) / 60000 * this.opts.bpm * this.mult * 4; return Math.floor(beat) % this.nSlices; }
  /* Responds to a decoded request; returns the response body (with status). */
  handle(type, body) {
    this.log.push(type);
    const w = []; const U8 = v => w.push(v & 0xff); const U16 = v => { U8(v); U8(v >> 8); }; const U32 = v => { U16(v); U16(v >>> 16); };
    const F32 = v => { const b = new Uint8Array(4); new DataView(b.buffer).setFloat32(0, v, true); w.push(...b); };
    const STR = s => { U8(s.length); for (const c of s) U8(c.charCodeAt(0)); };
    switch (type) {
      case CMD.hello: {
        U8(0); U8(1); U8(2); U8(16); U8(0); for (let i = 0; i < 12; i++) U8(0x10 + i);
        U32(SCHEMA); U32(4096); U32(this.desc.length); U32(crc32(this.desc)); U16(this.opts.maxBody); U16(LIVE_SIZE);
        STR('audiothurgist'); STR('Audiothurgist'); STR(this.opts.keys ? FW : FW_OLD); STR('demo'); STR('demo'); break; }
      case CMD.getDescriptor: {
        const off = u32(body, 0), max = u16(body, 4), n = Math.max(0, Math.min(max, this.desc.length - off));
        U8(0); U32(off); U16(n); for (let i = 0; i < n; i++) U8(this.desc[off + i]); break; }
      case CMD.getLive: {
        const off = u32(body, 0), max = u16(body, 4), n = Math.max(0, Math.min(max, LIVE_SIZE - off));
        U8(0); U8(0xff); U32(off); U16(n); U16(LIVE_SIZE); U32(SCHEMA); for (let i = 0; i < n; i++) U8(this.live[off + i]); break; }
      case CMD.blobBegin: {
        if (body[0] !== 0xff || u32(body, 1) !== LIVE_SIZE || u32(body, 5) !== SCHEMA) { U8(7); break; }
        this.blob = { got: 0, bytes: new Uint8Array(LIVE_SIZE) }; U8(0); break; }
      case CMD.blobData: {
        if (!this.blob) { U8(3); break; } const off = u32(body, 0); if (off !== this.blob.got) { U8(2); this.blob = null; break; }
        this.blob.bytes.set(body.subarray(4), off); this.blob.got += body.length - 4; U8(0); U32(this.blob.got); break; }
      case CMD.blobCommit: {
        if (!this.blob || this.blob.got !== LIVE_SIZE) { U8(3); break; }
        if (u32(body, 0) !== crc32(this.blob.bytes)) { U8(4); break; }
        const before = Uint8Array.from(this.options); this.live.set(this.blob.bytes); this.blob = null;
        this.setGrammar(this.grammar, before[O['grammar.mode']]);      /* Play starts from an empty loop, the rest reroll */
        if (before[O.bars] !== this.options[O.bars]) { const b = this.options[O.bars]; this.opts.nSlices = b ? Math.min(64, 16 << (b - 1)) : 32; this.newMaterial(); }
        if (this.live[OFF.sample] !== this.file) { this.file = this.live[OFF.sample]; this.newMaterial(); }
        U8(0); break; }
      case CMD.state: {
        if (this.opts.tail >= 223) this.setTick();   /* SETS: the op finishes before the body is built, as the firmware's main loop does */
        if (this.opts.card) this.cardTick();           /* CARD: the same for an upload's write, a copy, a rescan */
        if (this.opts.pads) this.padTick();            /* PADS: a drop's commit, New empty kit, a quiet drop dropped */
        if (this.opts.edit) this.auditionTick();       /* EDIT-AUDITION: a held request whose window has ended */
        if (this.callT > 0 && --this.callT === 0) this.callFlash = 255;
        const flags = (this.muted ? 1 : 0) | (this.ext ? 2 : 0) | (this.locked ? 4 : 0) | (this.flowing ? 8 : 0) | (this.brain ? 16 : 0)
          | (this.opts.pads && this.startIn === 1 ? 32 : 0);   /* START: b5 Start in = As saved */
        U8(0); U8(this.nSlices); U8(this.curStep()); U8(flags); U8(this.matGen); U8(this.grammar); U8(this.options[O['class.mode']]); U8(this.file);
        U8(this.opts.chop != null ? this.opts.chop : 200); U8(this.histN); U8(this.histDepth); U8(this.callFlash);
        F32(this.opts.bpm * this.mult); F32(this.mult);
        for (let i = 0; i < 64; i++) U8(this.src[i]); for (let i = 0; i < 64; i++) U8(this.stepClass[i]);
        const tail = this.opts.tail;
        if (tail >= 156) { U16(900 + Math.floor(Math.random() * 300)); U16(180 + Math.floor(Math.random() * 40)); U16(0); U16(this.histN); }
        if (tail >= 164) { U16(this.lockMask); F32(this.opts.bpm); U8(this.nSlices / 16); U8(this.options[O.bars]); }
        if (tail >= 167) { U16(this.ext ? 120 + Math.floor(Math.random() * 60) : 0); U8(this.multZone); }
        if (tail >= 170) { U8(this.clkFlags); U16(this.ext ? 40 + Math.floor(Math.random() * 20) : 0); }
        if (tail >= 175) { U8(0); U16(0); U16(0); }                       /* v0.9.5 forensics */
        if (tail >= 176) U8(this.jackMap);                                 /* 175 jack map: 0 Brain · 1 Voice */
        if (tail >= 177) U8(this.looperEvents);                            /* 176 LOOPER */
        if (tail >= 194) {                                                 /* 177.. SAMPLE / WEB */
          this.recTick(); const r = this.rec;
          U8(r.state); U8(r.state ? r.key : 255); U32(this.freePos); U32(BUF_CAP); U8(this.stereo ? 1 : 0); U8(this.inLevel); U8(this.nTable); U32(this.bufLen);
        }
        if (tail >= 218) for (let k = 0; k < KEYS; k++) U8(this.keySlice[k]);
        if (tail >= 219) U8(this.options[O['key.plays']]);                 /* 218 Key Plays */
        if (tail >= 223) { const s = this.set, c = this.opts.card && this.cardOp.job && this.cardOp.job !== 'rescan' && !s.state; U8(c ? 3 : s.state); U8(c ? this.cardOp.progress : s.progress); U8(s.result); U8(s.seq); }   /* 219.. SETS; 3 = CARD's write or copy, as the firmware */
        if (tail >= 255) { const s = this.set; for (let i = 0; i < 32; i++) U8(i < s.name.length ? s.name.charCodeAt(i) : 0); }
        if (tail >= 258) { this.journalCheck(); U8(this.tableGen); U8(Math.min(255, this.journal.length)); U8(this.selSlice); }   /* 255.. EDIT: tableGen, undoDepth, selSlice */
        if (tail >= 262) U32(this.srcPos());                                /* 258 EDIT: the position snapshot */
        if (tail >= 263) U8(Math.min(255, this.redoStack.length));          /* 262 EDIT-UNDO: redoDepth */
        if (tail >= 276) {                                                  /* 263.. KEYS (v0.13) */
          const now = Date.now(), valid = this.srcValid(), nm = this.liveMarkers().length, mv = this.j4mV(now), voice = this.jackMap === 1;
          U8((valid ? 1 : 0) | (this.grammar === GRAM.pads && nm > 0 ? 2 : 0));
          const avg = this.opts.load != null ? this.opts.load : 310 + Math.floor(Math.random() * 40); U16(avg); U16(Math.min(2000, avg + 90 + Math.floor(Math.random() * 120)));
          U16(mv < 0 ? mv + 0x10000 : mv); U8(this.j4Key(mv)); U8(voice ? (Math.floor((now - this.t0) / 6000) % 4 === 3 ? 1 : 0) : 255);
          U8(this.keyFlash); U8(this.keySeq); U8(this.autoK); U8(Math.min(255, nm));
        }
        if (tail >= 293) {                                                  /* 276.. CARD */
          const o = this.cardOp, v = this.survey;
          U8(o.state); U8(o.progress); U8(o.result); U8(o.seq); U8(this.cardFlags());
          U16(Math.min(65535, v.rootWavs)); U16(Math.min(65535, v.pending)); U16(Math.min(65535, v.breaks));
          U8(o.lastIdx); U16(o.copied); U16(o.skipped); U8(Math.min(255, this.files.length));
        }
        if (tail >= 315) {                                                  /* 293.. PADS */
          const o = this.padOp, u = this.padUp;
          U8(o.state); U8(o.state === 1 && u.total ? Math.floor(u.have * 255 / u.total) : o.progress); U8(o.result); U8(o.seq); U8(o.key);
          U8((this.kitUnsaved() ? 1 : 0) | (this.noBreak ? 2 : 0) | (this.padStereo ? 4 : 0));
          U32(this.padRoom()); U32(Math.max(0, BUF_CAP - this.bufLen)); U32(this.padHeld()); U32(this.padMask());
        }
        break; }
      case CMD.wave: {
        if (body.length < 7) { U8(2); break; }
        const start = u32(body, 0), plane = body[6], step = Math.max(1, Math.min(128, body.length >= 8 ? body[7] : 1)); let n = Math.min(480, u16(body, 4));
        if (plane > 2 || start >= this.freePos) { U8(2); break; }
        if (start + n * step > this.freePos) n = Math.floor((this.freePos - start) / step);
        if (!n) { U8(2); break; }
        U8(0); U32(start); U16(n); U8(step);
        const P = plane === 1 && this.stereo && this.bufR ? this.bufR : this.buf, Q = plane === 2 && this.stereo && this.bufR ? this.bufR : null;
        for (let i = 0; i < n; i++) { let best = 0, mag = -1; for (let k = 0; k < step; k++) { const j = start + i * step + k, v = Q ? 0.5 * (this.buf[j] + Q[j]) : P[j], m = Math.abs(v); if (m > mag) { mag = m; best = v; } }
          U8(Math.round(Math.max(-127, Math.min(127, best * 127)))); }
        break; }
      case CMD.table: {
        const first = body.length >= 1 ? body[0] : 0; let count = body.length >= 2 ? body[1] : 39;
        if (first > this.nTable) { U8(2); break; }
        count = Math.min(count, 39, this.nTable - first);
        const wide = this.opts.edit && body.length >= 3 && body[2] !== 0;   /* EDIT: form 1 → the 14-byte entry */
        U8(0); U8(this.nSlices); U8(this.nTable);
        for (let t = first; t < first + count; t++) { const e = this.table[t]; U32(e.start); U32(e.len); U8(e.key); U8(e.cls); U8(e.sal); U8(e.hit ? 1 : 0); U8(e.dens); if (wide) U8(e.flags); }
        break; }
      case CMD.hits: {
        const first = body.length >= 1 ? body[0] : 0, n = this.hits.length;
        if (first > n) { U8(2); break; }
        const count = Math.min(127, n - first); U8(0); U8(n); U8(first); for (let i = first; i < first + count; i++) U32(this.hits[i]);
        break; }
      case CMD.env: {
        const first = body[0] || 0, n = Math.min(body[1] || 30, 30, 64 - first);
        if (first >= 64) { U8(2); break; }
        U8(0); U8(first); U8(n); for (let i = 0; i < n * 16; i++) U8(this.env[first * 16 + i]); break; }
      /* SCOPE: the firmware's 0x62 — the empty body is the old mono reply, a body is the
       * ten-channel one with the same parse / clamp / window / trigger / at-pos / decimation
       * rules as core/bb_scope.h (kept in step by hand: selftest.mjs pins both). */
      case CMD.scope: {
        const wpos = this.scopePos(), cvW = Math.floor(wpos / SC.cvDiv);
        const sub = (p, k, wrap) => { k %= wrap; return p >= k ? p - k : p + (wrap - k); }, add = (p, k, wrap) => (p + k) % wrap;
        if (!body.length) {
          U8(0); U16(256); let p = sub(sub(wpos, 1, SC.wrap) & ~3, 1020, SC.wrap);
          for (let i = 0; i < 256; i++) { U8(Math.trunc((this.scopeAud(0, p) + this.scopeAud(1, p)) / 2)); p = add(p, 4, SC.wrap); }
          break;
        }
        if (body.length < SC.reqMin) { U8(2); break; }
        const mask = body[0], decim = body[1], flags = body[4]; let n = u16(body, 2), pos = 0, maskHi = 0, cvDecim = 1, level = 0, trig = 0;
        if (mask > 15 || !decim || decim > SC.maxDecim || !n) { U8(2); break; }
        if (flags & (SC.at | SC.ext)) { if (body.length < SC.reqPos) { U8(2); break; } pos = u32(body, 5); if ((flags & SC.at) && pos >= SC.wrap) { U8(2); break; } }
        if (flags & SC.ext) {
          if (body.length < SC.reqExt) { U8(2); break; }
          maskHi = body[9]; cvDecim = body[10]; level = body[11] > 127 ? body[11] - 256 : body[11]; trig = body[12];
          if (maskHi > 63 || !cvDecim || cvDecim > SC.cvMaxDecim) { U8(2); break; }
          if ((trig & SC.trigSet) ? (trig & 0x7f) >= 10 : trig !== 0) { U8(2); break; }
        }
        if (!mask && !maskHi) { U8(2); break; }
        const count = m => { let c = 0; for (let i = 0; i < 8; i++) if (m & (1 << i)) c++; return c; }, first = m => { for (let i = 0; i < 8; i++) if (m & (1 << i)) return i; return -1; };
        const hdr = SC.hdr + ((flags & SC.ext) ? SC.hdrExt : 0);
        n = Math.min(n, SC.maxN, Math.floor((this.opts.maxBody - hdr) / (count(mask) + count(maskHi))));
        if (mask) n = Math.min(n, Math.floor(SC.usable / decim)); if (maskHi) n = Math.min(n, Math.floor(SC.cvUsable / cvDecim)); n = Math.max(1, n);
        const spanA = mask ? n * decim : 0, spanC = maskHi ? n * cvDecim : 0;
        const holdsA = p => { const a = HL.scopeAge(wpos, p); return a >= spanA && a <= SC.usable; }, holdsC = k => { const a = HL.scopeAge(cvW, k, SC.cvWrap); return a >= spanC && a <= SC.cvUsable; };
        const place = (p, fl) => { const w = { pos: p, cvPos: Math.floor(p / SC.cvDiv), flags: fl };
          if (mask && !holdsA(w.pos)) { w.pos = sub(wpos, spanA, SC.wrap); w.flags |= SC.detached; }
          if (maskHi && !holdsC(w.cvPos)) { w.cvPos = sub(cvW, spanC, SC.cvWrap); w.flags |= SC.detached; } return w; };
        let newest = mask ? sub(wpos, spanA, SC.wrap) : wpos;
        if (maskHi) { const nc = sub(cvW, spanC, SC.cvWrap) * SC.cvDiv; if (HL.scopeAge(wpos, nc) > HL.scopeAge(wpos, newest)) newest = nc; }
        const src = (trig & SC.trigSet) ? (trig & 0x7f) : first(mask) >= 0 ? first(mask) : first(maskHi) >= 0 ? 4 + first(maskHi) : -1;
        let win;
        if (flags & SC.at) {
          if ((!mask || holdsA(pos)) && (!maskHi || holdsC(Math.floor(pos / SC.cvDiv)))) win = { pos, cvPos: Math.floor(pos / SC.cvDiv), flags: SC.atHonoured };
          else win = place(newest, 0);
        } else if ((flags & SC.trig) && src >= 0) {
          if (src < 4) {
            let need = spanA; if (maskHi && spanC * SC.cvDiv > need) need = spanC * SC.cvDiv; if (need > SC.usable) need = spanA > 0 ? spanA : SC.usable; need = Math.max(1, need);
            const c = this.scopeCrossing(p => this.scopeAud(src, p), SC.usable, SC.wrap, wpos, need, level);
            win = c >= 0 ? place(c, SC.triggered) : place(newest, 0);
          } else {
            let need = spanC; const fromA = Math.ceil(spanA / SC.cvDiv); if (fromA > need) need = fromA; need = Math.max(1, Math.min(need, SC.cvUsable));
            const c = this.scopeCrossing(k => this.scopeCv(src - 4, k), SC.cvUsable, SC.cvWrap, cvW, need, level);
            win = c >= 0 ? place(c * SC.cvDiv, SC.triggered) : place(newest, 0);
          }
        } else win = place(newest, 0);
        U8(0); U8(mask); U8(decim); U16(n); U8(win.flags | ((flags & SC.ext) ? SC.extReply : 0)); U32(win.pos);
        if (flags & SC.ext) { U8(maskHi); U8(cvDecim); U8(SC.cvDiv); U8((flags & SC.trig) && !(flags & SC.at) && src >= 0 ? src : SC.trigNone); }
        const extract = (get, wrap, start, dec) => { let p = start;
          for (let i = 0; i < n; i++) { let v = get(p);
            if ((flags & SC.peak) && dec > 1) { let mag = -1, q = p; for (let k = 0; k < dec; k++) { const x = get(q), m = Math.abs(x); if (m > mag) { mag = m; v = x; } q = add(q, 1, wrap); } }
            U8(v); p = add(p, dec, wrap); } };
        for (let c = 0; c < 4; c++) if (mask & (1 << c)) extract(p => this.scopeAud(c, p), SC.wrap, win.pos, decim);
        for (let j = 0; j < 6; j++) if (maskHi & (1 << j)) extract(k => this.scopeCv(j, k), SC.cvWrap, win.cvPos, cvDecim);
        break; }
      case CMD.action: {
        const op = body[0], a = body[1], b = body[2]; let ok = true;
        switch (op) {
          case 0: this.dice(); break;
          case 1: this.newMaterial(); this.histN = Math.min(16, this.histN + 1); break;
          case 2: if (this.muted) this.transportStart(); else this.muted = true; break;   /* the toggle (0.12's only transport) */
          case 3: if (a < this.nSlices && b < this.nSlices) { this.src[a] = b; this.callFlash = b; this.callT = 4; } break;   /* the flash is the slice */
          case 4: this.flowing = !!a; break;
          case 5: this.histDepth = Math.min(this.histN - 1, a); break;
          case 6: if (this.padBusy()) { U8(3); return Uint8Array.from(w); } if (a < this.files.length) { const f = this.files[a]; this.file = a; this.live[OFF.sample] = a; const sn = this.sets.get(f.endsWith(' [set]') ? f.slice(0, -6) : ''); if (sn) { this.restore(sn); this.set.name = f.slice(0, -6); } else { this.loadBreak(); this.set.name = ''; } } break;
          case 7: this.lockMask = 0; break;
          case 8: this.brain = !!a; break;
          case 9:  if (body.length < 2 || a >= KEYS) ok = false; else if (this.rec.state || this.padBusy()) { U8(3); return Uint8Array.from(w); } else this.rec = { state: 1, key: a, t: Date.now(), pos: 0 }; break;
          case 10: if (body.length < 2 || a >= KEYS) ok = false; else if (!this.clearKey(a)) { U8(3); return Uint8Array.from(w); } break;
          case 11: if (body.length < 2 || a > 1) ok = false; else this.options[O['key.plays']] = a; break;
          case 12: this.looperUndo(); break;
          case 13: this.looperClear(); break;
          case 14: case 15: {   /* SETS: the name is the rest of the body; the work is asynchronous */
            const name = new TextDecoder('ascii').decode(body.subarray(1));
            if (!SET_NAME_RE.test(name)) { ok = false; break; }
            if (this.set.state || this.set.pending || this.padBusy() || (op === 14 && this.rec.state)) { U8(3); return Uint8Array.from(w); }
            this.set.pending = { op, name }; break; }
          /* EDIT: 16..29 — unknown to an older firmware (BAD_ARGS); EDIT-UNDO: 30 / 31 */
          case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 29: case 30: case 31:
            if (!this.opts.edit) { ok = false; break; }
            return Uint8Array.from(this.applyEdit(op, body));
          /* KEYS (v0.13): 32 PLAY_KEY · 33 KEYS_FROM_MARKERS · 34 CLEAR_MARKERS · 35 SET_MUTE · 36 AUTOSLICE — unknown to 0.12 */
          case 33: case 34: case 36:
            if (!this.opts.keys) { ok = false; break; }
            return Uint8Array.from(this.applyEdit(op, body));
          /* KEYP: 37 KEY_PARAMS — unknown before it */
          case 37:
            if (!this.opts.keyp) { ok = false; break; }
            return Uint8Array.from(this.applyEdit(op, body));
          /* CHROMA: 38 SPREAD_KEYS — unknown before it */
          case 38:
            if (!this.opts.spread) { ok = false; break; }
            return Uint8Array.from(this.applyEdit(op, body));
          case 32: { if (!this.opts.keys || body.length < 3) { ok = false; break; } const r = this.playKey(a, b); if (r) { U8(r); return Uint8Array.from(w); } break; }
          case 35: if (!this.opts.keys || body.length < 2 || a > 1) ok = false; else if (a) this.muted = true; else this.transportStart(); break;
          /* START: 40 START_IN(0 Sampler / 1 As saved) — written to the card for the next power-up; NO_CARD without one */
          case 40: if (!this.opts.pads || body.length < 2 || a > 1) ok = false;
            else if (!this.cardMounted) { U8(21); return Uint8Array.from(w); }
            else { this.startIn = a; this.cardPut('Audiothurgist/startup.json', JSON.stringify({ audiothurgist: 1, startIn: a ? 'saved' : 'sampler' }) + '\n'); }
            break;
          case 27: case 28: {
            if (!this.opts.edit) { ok = false; break; }
            if (!this.bufLen) { U8(3); return Uint8Array.from(w); }
            if (op === 28) { this.auditionHeld = null; this.audition = null; break; }
            if (body.length < 8) { ok = false; break; }
            const start = u32(body, 1), len = u16(body, 5) * MS, flags = body[7];
            if (start > this.freePos || len < GRID_MIN || (flags & ~3)) { ok = false; break; }
            const a = { start, len, flags };
            if (Date.now() - this.auditionAt >= 100) { this.auditionHeld = null; this.postAudition(a); } else this.auditionHeld = a;
            break; }
          default: ok = false;
        }
        U8(ok ? 0 : 2); break; }
      case CMD.card: if (!this.opts.card) { U8(1); break; } this.cardHandle(body, U8, U32); break;   /* CARD; UNSUPPORTED before it */
      case CMD.pads: if (!this.opts.pads) { U8(1); break; } this.padHandle(body, U8, U32); break;   /* PADS; UNSUPPORTED before it */
      case CMD.keys:   /* KEYP: 0x69 GET_KEYS → status, 24, then i8 gain · u8 plays · i16 cents · u8 rat per key; UNSUPPORTED before it */
        if (!this.opts.keyp) { U8(1); break; }
        U8(0); U8(KEYS);
        for (const p of this.keyp) { U8(p.gain & 0xff); U8(p.plays); U8(p.cents & 0xff); U8((p.cents >> 8) & 0xff); U8(p.rat | 0); }
        break;
      default: U8(1);
    }
    return Uint8Array.from(w);
  }
}

/* A SerialPort look-alike wrapping a FakeDevice: frames written to
 * `writable` are parsed, answered after `latencyMs`, and the answer frames
 * appear on `readable` in a few chunks (as CDC delivers them). */
class FakePort {
  constructor(device) {
    this.device = device; const parser = new FrameParser(); let ctrl = null;
    const self = this;
    this.readable = new ReadableStream({ start(c) { ctrl = c; self._ctrl = c; }, cancel() { ctrl = null; self._ctrl = null; } });
    this.writable = new WritableStream({
      write: chunk => { for (const byte of chunk) parser.push(byte, f => {
        const resp = f.ok ? buildFrame(f.type | 0x80, f.seq, device.handle(f.type, f.body)) : buildFrame(0xff, f.seq, Uint8Array.of(10));
        setTimeout(() => { if (!ctrl || !self._ctrl) return; const cut = Math.min(resp.length, 64); ctrl.enqueue(resp.subarray(0, cut)); if (cut < resp.length) ctrl.enqueue(resp.subarray(cut)); }, device.opts.latencyMs);
      }); },
    });
  }
  async open() {} async close() {}
  /* the cable pulled: the byte stream ends (a SerialTransport's read loop sees `done` and reports onClose) */
  unplug() { if (this._ctrl) { try { this._ctrl.close(); } catch {} this._ctrl = null; } }
}

const api = { FakeDevice, FakePort, buildDescriptor, OPTION_FIELDS, O, GRAM, OPTIONS_HASH, JACKS, KNOBS, FW, FILES, UP_MAX, LIVE_SIZE, SCHEMA, OFF, SR, BUF_CAP, SLICE_LEN, KEYS, MAX_BODY, MAX_TABLE, REC_MIN, GRID_MIN, TAP_REUSE, AUTO_MAX, PAD_STUB, KEYP };
if (typeof module === 'object' && module.exports) module.exports = api; else root.DEMO = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
