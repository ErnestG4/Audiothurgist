/* demo.js — a fake Audiothurgist on a fake serial port.
 *
 * Answers HostLink frames exactly as the firmware would (same framing, same
 * StateExt layouts including the v0.8.0 and CLOCK tails, a descriptor in
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
  ['grammar.mode', 'Grammar', ['Classic', 'Collins', 'Grids', 'Euclid']],
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
];
const FILES = ['amen_170.wav', 'apache_120.wav', 'funky_drummer_100.wav', 'think_110.wav'];
const KNOBS = [
  [['chop', 'Chop', '**Chop character** — the journey from verbatim (full right) to rolls, bursts and reverb (full left).'],
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

/* Blob layout, in Manage() order: pager 48 · locks 64 · sample 1 · options 16 · settings 2. */
const OFF = { pager: 0, locks: 48, sample: 112, options: 113, settings: 129 };
const LIVE_SIZE = 131;
const SCHEMA = 0x0a0d1071;

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
      { id: 'options', kind: 'fields', hash: 0x4f505432, size: 16, off: OFF.options, name: 'Options',
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
    audiothurgist: { ext: 1, state: 96, env: 97, scope: 98, action: 99 },
    manual: {
      preamble: '**Audiothurgist chops breaks to a grid and rearranges them** with one knob that travels from *the record, verbatim* to rolls, bursts, and reverb rubble. Patterns hold until you ask for new ones. (Demo descriptor — the module\'s own text replaces this.)',
      sections: [
        { id: 'roles', title: 'Three roles, one engine', body: '**Chopper** — the knobs and buttons. **Voice** — the rack plays *it*: a v/oct + gate pair on J4/J5. **Brain** — it plays the rack: kick, snare and hat gates on J6/J7/J8, clock on J3.' },
        { id: 'brain', title: 'Clock in, gates out', body: 'Patch a clock into J3 and set its PPQN in Options; B1 flashes white per pulse and sits green when locked. While following, P2 becomes a clock divider/multiplier (÷4 · ÷2 · ×1 · ×2 · ×4).' },
      ],
      presets: { help: 'Preset slots store the complete state of the module. Slot 0 is the boot slot.' },
    },
  };
}

class FakeDevice {
  constructor(opts = {}) {
    this.opts = Object.assign({ tail: 167, bpm: 120, nSlices: 32, latencyMs: 8 }, opts);
    this.live = new Uint8Array(LIVE_SIZE); this.live.fill(0x80, OFF.pager, OFF.pager + 48);
    this.desc = new TextEncoder().encode(JSON.stringify(buildDescriptor()));
    this.muted = false; this.flowing = false; this.brain = false; this.ext = false; this.locked = false;
    this.lockMask = 0; this.matGen = 1; this.file = 0; this.histN = 3; this.histDepth = 0; this.callFlash = 255; this.callT = 0;
    this.mult = 1; this.multZone = 2; this.t0 = Date.now(); this.log = [];
    this.src = new Int8Array(64); this.stepClass = new Uint8Array(64);
    this.env = new Uint8Array(64 * 16); this.scopePhase = 0;
    this.newMaterial();
    this.blob = null;
  }
  get nSlices() { return this.opts.nSlices; }
  get options() { return this.live.subarray(OFF.options, OFF.options + 16); }
  newMaterial() {
    const n = this.nSlices;
    for (let s = 0; s < n; s++) { this.stepClass[s] = s % 4 === 0 ? 0 : s % 4 === 2 ? 1 : (s % 8 === 7 ? 3 : 2); this.src[s] = s; }
    for (let s = 0; s < n; s++) for (let p = 0; p < 8; p++) { const a = Math.exp(-p / 2.5) * (this.stepClass[s] === 0 ? 0.9 : 0.6); this.env[s * 16 + p * 2] = Math.round(128 - a * 127); this.env[s * 16 + p * 2 + 1] = Math.round(128 + a * 127); }
    this.matGen = (this.matGen + 1) & 0xff;
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
        U32(SCHEMA); U32(4096); U32(this.desc.length); U32(crc32(this.desc)); U16(512); U16(LIVE_SIZE);
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
        break; }
      case CMD.env: {
        const first = body[0] || 0, n = Math.min(body[1] || 30, 30, 64 - first);
        if (first >= 64) { U8(2); break; }
        U8(0); U8(first); U8(n); for (let i = 0; i < n * 16; i++) U8(this.env[first * 16 + i]); break; }
      case CMD.scope: {
        U8(0); U16(256); const cur = this.curStep(); const amp = this.muted ? 0 : (this.stepClass[cur] === 0 ? 0.9 : 0.5);
        for (let i = 0; i < 256; i++) { this.scopePhase += 0.19; const v = Math.sin(this.scopePhase) * amp * Math.exp(-((i % 64) / 40)) * 120; U8(Math.round(v)); }
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
          case 6: if (a < FILES.length) { this.file = a; this.live[OFF.sample] = a; this.newMaterial(); } break;
          case 7: this.lockMask = 0; break;
          case 8: this.brain = !!a; break;
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

const api = { FakeDevice, FakePort, buildDescriptor, OPTION_FIELDS, FILES, LIVE_SIZE, SCHEMA, OFF };
if (typeof module === 'object' && module.exports) module.exports = api; else root.DEMO = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
