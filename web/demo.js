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

const OPTION_FIELDS = [
  ['class.mode', 'Classifier', ['Hockman 3-band', 'Legacy 120/900 (hi/lo)', 'Two-band lo/hi', 'Density (keyframes)']],
  ['clock.ppqn', 'Clock In', ['24 PPQN (MIDI/DIN)', '4 PPQN (16ths)', '2 PPQN (8ths)', '1 PPQN (quarters)', '48 PPQN', '96 PPQN', '16 PPQN', '8 PPQN']],
  ['voice.mode', 'Voice', ['Choke (momentary)', 'Punch (writes the bar)']],
  ['gate.len', 'Gate Length', ['6 ms trig', '20 ms', '50 ms', 'Hold to next hit']],
  ['gate.lvl', 'Gate Level', ['Fixed 5 V', 'Velocity (1.5–5 V)']],
  ['grammar.mode', 'Grammar', ['Classic', 'Collins', 'Grids', 'Euclid', 'Camp']],
  ['gate.mode', 'Gate Fires', ['Every step', 'Onsets only (once per hit)']],
  ['bars', 'Bars', ['Auto (backbeat)', '1 bar', '2 bars', '4 bars', '8 bars (8th grid)']],
  ['rests', 'Rests', ['Continue the break', 'Gate (silence)']],
  ['gate.lanes', 'Gate Lanes', ['Dominant class', 'Multi-label (every band over threshold)']],
  ['fades', 'Fades', ['Fixed', 'Adaptive (keyframe density)']],
  ['tail', 'Slice Tail', ['Gate (honest)', 'Loop forward', 'Ping-pong']],
  ['swaps', 'Swaps', ['Class only', 'Class + salience tier']],
  ['flow.dejavu', 'Flow Déjà Vu', ['Off', 'Some (¼ of flow bars from history)', 'Lots (¾)']],
  ['swing', 'Swing', ['Straight 16ths', 'Auto (the material\'s swing)']],
  ['onsets', 'Onsets', ['Spectral flux', 'Log-filtered flux (Böck 2012)']],
  ['key.plays', 'Key Plays', ['One-shot (the slice)', 'Held (read on until the gate falls)']],
];
/* The card's list as the firmware labels it: loose WAVs, then the sets of sets/ as "<name> [set]". */
const FILES = ['amen_170.wav', 'apache_120.wav', 'funky_drummer_100.wav', 'think_110.wav', 'amen_kit [set]'];
const SET_NAME_RE = /^[A-Za-z0-9_-]{1,31}$/;
const KNOBS = [
  [['chop', 'Chop', '**Chop character** — the journey from verbatim (full right) to rolls, bursts and reverb (full left). In Camp: the number of bells (rounds → 4 → 8 → 16).'],
   ['tempo', 'Tempo', '**Tempo**, 70–180 BPM, varispeed like vinyl. Snaps to the break\'s own tempo within ±2 BPM. While following an external clock it becomes a ÷4…×4 multiplier.'],
   ['history', 'History', '**History** — the last sixteen patterns. Counter-clockwise is older; full clockwise is now.'],
   ['pitch', 'Pitch', '**Pitch**, ±12 semitones. Onsets stay on the grid; the kit retunes.'],
   ['sample', 'Sample', '**Sample** — highlight a file on the card; hold **B3** to load it.'],
   ['volume', 'Volume', '**Volume**, with the reverb tail riding the cluster zone.']],
  [['lang.x', 'Map X', '**Map X** — Grids drum-map coordinate.'],
   ['lang.y', 'Map Y', '**Map Y** — Grids drum-map coordinate.'],
   ['lang.kick', 'Kick', '**Kick density** — Grids threshold / Euclid hits per bar.'],
   ['lang.snare', 'Snare', '**Snare density** — Grids threshold / Euclid hits per bar.'],
   ['lang.hat', 'Hat', '**Hat density** — Grids threshold / Euclid hits per bar.'],
   ['lang.rot', 'Rotation', '**Rotation** — Euclid: rotates every class\'s hits together.']],
];

/* Blob layout, in Manage() order: pager 48 · locks 64 · sample 1 · options 17 · settings 2. */
const OFF = { pager: 0, locks: 48, sample: 112, options: 113, settings: 130 };
const N_OPTIONS = OPTION_FIELDS.length;   /* 17: the firmware's byte 16 (Jacks) is not modelled; Key Plays sits at 16 here */
const LIVE_SIZE = 132;
const SCHEMA = 0x0a0d1072;
/* The fake material: 48 kHz, one plane, 16 s of capacity; the break fills
 * nSlices × 6000 samples (32 slices = 4 s = two bars at 120 BPM). */
const SR = 48000, BUF_CAP = SR * 16, SLICE_LEN = 6000, KEYS = 24;
const REC_MIN = 480;
/* SCOPE: the firmware's ring geometry (core/bb_scope.h) and this module's max_body
 * (1024 in the firmware's Makefile; 512 is the protocol default an older build has). */
const MAX_BODY = 1024;
const SC = HL.SCOPE;

function buildDescriptor() {
  const pagerFields = [];
  KNOBS.forEach((page, pg) => page.forEach(([id, name, help], pot) => pagerFields.push({ id, name, off: (pg * 6 + pot) * 4, type: 'f32', page: pg, pot, def: 0.5, disp: pot === 3 && pg === 0 ? { kind: 'linear', lo: -12, hi: 12, unit: 'st' } : { kind: 'norm' }, help })));
  return {
    dv: 1,
    module: { id: 'audiothurgist', name: 'Audiothurgist', fw: '0.8.0-demo', git: 'demo', sdk: 'demo', board: 'v2', tagline: 'A sampling, slicing, and rearranging toolkit' },
    schemaHash: SCHEMA, size: LIVE_SIZE,
    components: [
      { id: 'perf', kind: 'pager', hash: 1, size: 48, off: OFF.pager, pages: 2, pots: 6,
        pageNames: ['Perform', 'Brain'], pageColors: ['#eb7414', '#3a9fe0'],
        pageHelp: ['The performance page: everything BreakFix was, plus tempo as a turntable, history as a dial, and file selection.',
                   'The pattern-language console. Live for Grids and Euclid (Grammar, in Options); a change regenerates the pattern.'],
        fields: pagerFields },
      { id: 'locks', kind: 'param_locks', hash: 2, size: 64, off: OFF.locks, name: 'Param Locks', slots: 2, slotSize: 32, bufLen: 4, layout: { activeOff: 0, lengthOff: 1, baseOff: 3, bufOff: 7 } },
      { id: 'sample', kind: 'fields', hash: 0x53414d50, size: 1, off: OFF.sample, name: 'Sample',
        fields: [{ id: 'sample.idx', name: 'File', off: 0, type: 'enum', zones: FILES.length, def: 0, disp: { kind: 'enum', labels: FILES } }] },
      { id: 'options', kind: 'fields', hash: 0x4f505434, size: N_OPTIONS, off: OFF.options, name: 'Options',
        fields: OPTION_FIELDS.map(([id, name, labels], i) => ({ id, name, off: i, type: 'enum', zones: labels.length, def: 0, disp: { kind: 'enum', labels } })) },
      { id: 'settings', kind: 'settings', hash: 4, size: 2, off: OFF.settings, pages: 1, pageNames: ['Setup'],
        fields: [{ id: 'brightness', name: 'Brightness', off: 0, type: 'f32', page: 0, pot: 0, def: 0.7, disp: { kind: 'brightness', lo: 0.1, hi: 1 } }] },
    ],
    jacks: [
      { id: 'audio_l', name: 'Audio In L', sig: 'audioIn', help: 'Audio in, left (record/resample — coming).' },
      { id: 'audio_r', name: 'Audio In R', sig: 'audioIn', help: 'Audio in, right.' },
      { id: 'clock', name: 'Clock In', sig: 'trig', help: '**Clock in.** Set PPQN in Options. B1 = pulse/lock indicator.', see: ['tempo'] },
      { id: 'voct', name: 'V/Oct', sig: 'voct', help: '**V/Oct** — the keymap\'s pitch: mute key, modifiers, sixteen slice keys. Pair with J5.' },
      { id: 'gate', name: 'Gate', sig: 'gate', help: '**Gate** — each rising edge samples J4 and dispatches.' },
      { id: 'kick', name: 'Kick Gate', sig: 'gate', help: '**Kick gate out** — fires with every kick-class step.' },
      { id: 'snare', name: 'Snare Gate', sig: 'gate', help: '**Snare gate out** — fires with every snare-class step.' },
      { id: 'hat', name: 'Hat Gate', sig: 'gate', help: '**Hat gate out** — fires with every hat-class step.' },
      { id: 'out_l', name: 'Audio Out L', sig: 'audioOut', help: 'Audio out, left.' },
      { id: 'out_r', name: 'Audio Out R', sig: 'audioOut', help: 'Audio out, right.' },
    ],
    audiothurgist: { ext: 1, state: 96, env: 97, scope: 98, action: 99, wave: 100, table: 101, hits: 102, sets: 1, scope2: 2 },
    manual: {
      preamble: '**Audiothurgist chops breaks to a grid and rearranges them** with one knob that travels from *the record, verbatim* to rolls, bursts, and reverb rubble. Patterns hold until you ask for new ones. (Demo descriptor — the module\'s own text replaces this.)',
      sections: [
        { id: 'roles', title: 'Three roles, one engine', body: '**Chopper** — the knobs and buttons. **Voice** — the rack plays *it*: a v/oct + gate pair on J4/J5. **Brain** — it plays the rack: kick, snare and hat gates on J6/J7/J8, clock on J3.' },
        { id: 'brain', title: 'Clock in, gates out', body: 'Patch a clock into J3 and set its PPQN in Options; B1 flashes white per pulse and sits green when locked. While following, P2 becomes a clock divider/multiplier (÷4 · ÷2 · ×1 · ×2 · ×4); a change lands on the next beat.' },
        { id: 'camp', title: 'Camp', body: '**Camp** rings the bar\'s cells through change-ringing permutations — deterministic, nothing drawn. Chop is the number of bells (rounds, 4, 8, 16); B2 rings the next change; the Brain knobs are unused.' },
      ],
      presets: { help: 'Preset slots store the complete state of the module. Slot 0 is the boot slot.' },
    },
  };
}

class FakeDevice {
  constructor(opts = {}) {
    this.opts = Object.assign({ tail: 255, bpm: 120, nSlices: 32, latencyMs: 8, swing: false, kit: false, maxBody: MAX_BODY, inputs: true }, opts);
    this.live = new Uint8Array(LIVE_SIZE); this.live.fill(0x80, OFF.pager, OFF.pager + 48);
    this.desc = new TextEncoder().encode(JSON.stringify(buildDescriptor()));
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
    this.table = []; this.keySlice = new Int8Array(KEYS); this.hits = [];
    this.rec = { state: 0, key: -1, t: 0, pos: 0 }; this.inLevel = 0; this.looperEvents = 0;
    /* SETS: the card's sets (name → a snapshot of everything a set carries) and the op in flight,
     * driven by time on each state poll: pending → saving / loading for 600 ms → done (seq bumps). */
    this.sets = new Map(); this.set = { state: 0, progress: 0, result: 0, seq: 0, name: '', t: 0, pending: null };
    this.newMaterial();
    if (this.opts.kit) { this.recordKey(3, 0.35, 0); this.recordKey(7, 0.9, 1); this.recordKey(14, 1.6, 2); this.looperEvents = 3; this.sets.set('amen_kit', this.snapshot()); }
    this.blob = null;
  }
  /* SETS: what sets/<name>.wav + .json would hold. */
  snapshot() {
    return { nSlices: this.nSlices, bufLen: this.bufLen, freePos: this.freePos, table: this.table.map(e => Object.assign({}, e)),
      keySlice: Int8Array.from(this.keySlice), hits: this.hits.slice(), looperEvents: this.looperEvents, options: Uint8Array.from(this.options),
      buf: this.buf.slice(0, this.freePos), src: Int8Array.from(this.src), stepClass: Uint8Array.from(this.stepClass), env: Uint8Array.from(this.env) };
  }
  restore(sn) {
    this.opts.nSlices = sn.nSlices; this.bufLen = sn.bufLen; this.freePos = sn.freePos; this.table = sn.table.map(e => Object.assign({}, e));
    this.keySlice.set(sn.keySlice); this.hits = sn.hits.slice(); this.looperEvents = sn.looperEvents; this.options.set(sn.options);
    this.buf.fill(0); this.buf.set(sn.buf); this.src.set(sn.src); this.stepClass.set(sn.stepClass); this.env.set(sn.env);
    this.rec = { state: 0, key: -1, t: 0, pos: 0 }; this.matGen = (this.matGen + 1) & 0xff;
  }
  setTick() {
    const s = this.set, now = Date.now();
    if (!s.state && s.pending) { s.state = s.pending.op === 14 ? 1 : 2; s.progress = 0; s.t = now; }
    if (!s.state) return;
    const el = now - s.t; s.progress = Math.min(255, Math.round(el / 600 * 255));
    if (el < 600) return;
    const { op, name } = s.pending; s.pending = null; s.state = 0; s.progress = 0;
    if (op === 14) {
      if (name === 'nocard') s.result = 1; else if (name === 'full') s.result = 2;
      else { this.sets.set(name, this.snapshot()); s.result = 0; s.name = name; const lab = name + ' [set]'; if (!FILES.includes(lab)) { FILES.push(lab); this.desc = new TextEncoder().encode(JSON.stringify(buildDescriptor())); } this.file = FILES.indexOf(lab); this.live[OFF.sample] = this.file; }
    } else {
      const sn = this.sets.get(name);
      if (!sn) s.result = 5; else { this.restore(sn); s.result = 0; s.name = name; const i = FILES.indexOf(name + ' [set]'); this.file = i >= 0 ? i : 255; if (i >= 0) this.live[OFF.sample] = i; }
    }
    s.seq = (s.seq + 1) & 0xff;
  }
  get nSlices() { return this.opts.nSlices; }
  get options() { return this.live.subarray(OFF.options, OFF.options + N_OPTIONS); }
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
      this.table.push({ start, len: end - start, key: -1, cls, sal: cls === 0 ? 240 : cls === 1 ? 200 : 120, hit, dens: cls === 2 ? 220 : cls === 0 ? 60 : 130 });
      if (hit) { this.synthHit(start, Math.min(end - start, 9000), cls, cls === 0 ? 0.95 : 0.7); this.hits.push(start); }
      if (s % 4 === 3) { const g = start + Math.round((end - start) * 0.5); this.synthHit(g, 2400, 2, 0.25); this.hits.push(g); }   /* a ghost hat */
    }
    const span = Math.min(n, KEYS); for (let k = 0; k < KEYS; k++) this.keySlice[k] = Math.min(n - 1, Math.floor(k * n / span));
    this.rec = { state: 0, key: -1, t: 0, pos: 0 };
    this.matGen = (this.matGen + 1) & 0xff;
  }
  /* CommitSlice's twin: publish [freePos, freePos + len) as key's entry (a tone burst is synthesised into it). */
  recordKey(key, seconds, cls = 1) {
    const room = BUF_CAP - this.freePos; if (room < REC_MIN) return -1;
    const len = Math.max(REC_MIN, Math.min(Math.round(seconds * SR), room));
    const start = this.freePos; this.buf.fill(0, start, start + len);
    this.synthHit(start, Math.min(len, 12000), cls, 0.9); if (len > 20000) this.synthHit(start + 16000, Math.min(len - 16000, 12000), 2, 0.5);
    const e = { start, len, key, cls, sal: 255, hit: true, dens: cls === 2 ? 210 : 90 };
    let t = this.table.findIndex(x => x.key === key);
    if (t < 0) { t = this.table.length; this.table.push(e); } else this.table[t] = e;
    this.keySlice[key] = t; this.freePos = start + len; return t;
  }
  clearKey(key) {
    const t = this.table.findIndex(x => x.key === key); if (t < 0) return false;
    this.table.splice(t, 1);
    for (let k = 0; k < KEYS; k++) if (this.keySlice[k] > t) this.keySlice[k]--;
    const n = this.nSlices, span = Math.min(n, KEYS); this.keySlice[key] = Math.min(n - 1, Math.floor(key * n / span));
    return true;
  }
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
    if (!this.muted && cls >= 0) {
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
      default: return (!this.muted && cls === j - 3 && t < 0.006) ? 5 : 0.02;
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
        STR('audiothurgist'); STR('Audiothurgist'); STR('0.8.0-demo'); STR('demo'); STR('demo'); break; }
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
        if (before[5] !== this.options[5]) this.dice();               /* grammar change rerolls */
        if (before[7] !== this.options[7]) { const b = this.options[7]; this.opts.nSlices = b ? Math.min(64, 16 << (b - 1)) : 32; this.newMaterial(); }
        if (this.live[OFF.sample] !== this.file) { this.file = this.live[OFF.sample]; this.newMaterial(); }
        U8(0); break; }
      case CMD.state: {
        if (this.opts.tail >= 223) this.setTick();   /* SETS: the op finishes before the body is built, as the firmware's main loop does */
        if (this.callT > 0 && --this.callT === 0) this.callFlash = 255;
        const flags = (this.muted ? 1 : 0) | (this.ext ? 2 : 0) | (this.locked ? 4 : 0) | (this.flowing ? 8 : 0) | (this.brain ? 16 : 0);
        U8(0); U8(this.nSlices); U8(this.curStep()); U8(flags); U8(this.matGen); U8(this.options[5]); U8(this.options[0]); U8(this.file);
        U8(this.opts.chop != null ? this.opts.chop : 200); U8(this.histN); U8(this.histDepth); U8(this.callFlash);
        F32(this.opts.bpm * this.mult); F32(this.mult);
        for (let i = 0; i < 64; i++) U8(this.src[i]); for (let i = 0; i < 64; i++) U8(this.stepClass[i]);
        const tail = this.opts.tail;
        if (tail >= 156) { U16(900 + Math.floor(Math.random() * 300)); U16(180 + Math.floor(Math.random() * 40)); U16(0); U16(this.histN); }
        if (tail >= 164) { U16(this.lockMask); F32(this.opts.bpm); U8(this.nSlices / 16); U8(this.options[7]); }
        if (tail >= 167) { U16(this.ext ? 120 + Math.floor(Math.random() * 60) : 0); U8(this.multZone); }
        if (tail >= 170) { U8(this.clkFlags); U16(this.ext ? 40 + Math.floor(Math.random() * 20) : 0); }
        if (tail >= 175) { U8(0); U16(0); U16(0); }                       /* v0.9.5 forensics */
        if (tail >= 176) U8(0);                                            /* jack map: Brain */
        if (tail >= 177) U8(this.looperEvents);                            /* 176 LOOPER */
        if (tail >= 194) {                                                 /* 177.. SAMPLE / WEB */
          this.recTick(); const r = this.rec;
          U8(r.state); U8(r.state ? r.key : 255); U32(this.freePos); U32(BUF_CAP); U8(0); U8(this.inLevel); U8(this.nTable); U32(this.bufLen);
        }
        if (tail >= 218) for (let k = 0; k < KEYS; k++) U8(this.keySlice[k]);
        if (tail >= 219) U8(this.options[16]);                             /* 218 Key Plays */
        if (tail >= 223) { const s = this.set; U8(s.state); U8(s.progress); U8(s.result); U8(s.seq); }   /* 219.. SETS */
        if (tail >= 255) { const s = this.set; for (let i = 0; i < 32; i++) U8(i < s.name.length ? s.name.charCodeAt(i) : 0); }
        break; }
      case CMD.wave: {
        if (body.length < 7) { U8(2); break; }
        const start = u32(body, 0), plane = body[6], step = Math.max(1, Math.min(128, body.length >= 8 ? body[7] : 1)); let n = Math.min(480, u16(body, 4));
        if (plane > 2 || start >= this.freePos) { U8(2); break; }
        if (start + n * step > this.freePos) n = Math.floor((this.freePos - start) / step);
        if (!n) { U8(2); break; }
        U8(0); U32(start); U16(n); U8(step);
        for (let i = 0; i < n; i++) { let best = 0, mag = -1; for (let k = 0; k < step; k++) { const v = this.buf[start + i * step + k], m = Math.abs(v); if (m > mag) { mag = m; best = v; } }
          U8(Math.round(Math.max(-127, Math.min(127, best * 127)))); }
        break; }
      case CMD.table: {
        const first = body.length >= 1 ? body[0] : 0; let count = body.length >= 2 ? body[1] : 39;
        if (first > this.nTable) { U8(2); break; }
        count = Math.min(count, 39, this.nTable - first);
        U8(0); U8(this.nSlices); U8(this.nTable);
        for (let t = first; t < first + count; t++) { const e = this.table[t]; U32(e.start); U32(e.len); U8(e.key); U8(e.cls); U8(e.sal); U8(e.hit ? 1 : 0); U8(e.dens); }
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
          case 2: this.muted = !this.muted; break;
          case 3: if (a < this.nSlices && b < this.nSlices) { this.src[a] = b; this.callFlash = a; this.callT = 4; } break;
          case 4: this.flowing = !!a; break;
          case 5: this.histDepth = Math.min(this.histN - 1, a); break;
          case 6: if (a < FILES.length) { this.file = a; this.live[OFF.sample] = a; const sn = this.sets.get(FILES[a].endsWith(' [set]') ? FILES[a].slice(0, -6) : ''); if (sn) { this.restore(sn); this.set.name = FILES[a].slice(0, -6); } else { this.newMaterial(); this.set.name = ''; } } break;
          case 7: this.lockMask = 0; break;
          case 8: this.brain = !!a; break;
          case 9:  if (body.length < 2 || a >= KEYS) ok = false; else if (this.rec.state) { U8(3); return Uint8Array.from(w); } else this.rec = { state: 1, key: a, t: Date.now(), pos: 0 }; break;
          case 10: if (body.length < 2 || a >= KEYS) ok = false; else if (!this.clearKey(a)) { U8(3); return Uint8Array.from(w); } break;
          case 11: if (body.length < 2 || a > 1) ok = false; else this.options[16] = a; break;
          case 12: this.looperEvents = Math.max(0, this.looperEvents - 1); break;
          case 13: this.looperEvents = 0; break;
          case 14: case 15: {   /* SETS: the name is the rest of the body; the work is asynchronous */
            const name = new TextDecoder('ascii').decode(body.subarray(1));
            if (!SET_NAME_RE.test(name)) { ok = false; break; }
            if (this.set.state || this.set.pending || (op === 14 && this.rec.state)) { U8(3); return Uint8Array.from(w); }
            this.set.pending = { op, name }; break; }
          default: ok = false;
        }
        U8(ok ? 0 : 2); break; }
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
    this.readable = new ReadableStream({ start(c) { ctrl = c; }, cancel() { ctrl = null; } });
    this.writable = new WritableStream({
      write: chunk => { for (const byte of chunk) parser.push(byte, f => {
        const resp = f.ok ? buildFrame(f.type | 0x80, f.seq, device.handle(f.type, f.body)) : buildFrame(0xff, f.seq, Uint8Array.of(10));
        setTimeout(() => { if (!ctrl) return; const cut = Math.min(resp.length, 64); ctrl.enqueue(resp.subarray(0, cut)); if (cut < resp.length) ctrl.enqueue(resp.subarray(cut)); }, device.opts.latencyMs);
      }); },
    });
  }
  async open() {} async close() {}
}

const api = { FakeDevice, FakePort, buildDescriptor, OPTION_FIELDS, FILES, LIVE_SIZE, SCHEMA, OFF, SR, BUF_CAP, SLICE_LEN, KEYS, MAX_BODY };
if (typeof module === 'object' && module.exports) module.exports = api; else root.DEMO = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
