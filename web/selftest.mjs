#!/usr/bin/env node
/* selftest.mjs — pins web/hostlink.js to the SDK's golden vectors and
 * exercises the page's protocol layer against the fake module in demo.js.
 *
 *   node web/selftest.mjs            (from the repo root; ../alchemy-sdk beside it)
 *   node web/selftest.mjs --golden /path/to/hostlink_golden.json
 *
 * No hardware, no browser. Exit status 0 = all green. */
import { createRequire } from 'node:module';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const require = createRequire(import.meta.url);
const HL = require('./hostlink.js');
const DEMO = require('./demo.js');
const here = path.dirname(fileURLToPath(import.meta.url));

let pass = 0, fail = 0;
const check = (name, ok, detail = '') => { if (ok) pass++; else { fail++; console.error('  FAIL ' + name + (detail ? ' — ' + detail : '')); } };
const hex = b => Array.from(b, x => x.toString(16).padStart(2, '0')).join('');
const unhex = h => Uint8Array.from((h.match(/../g) || []).map(x => parseInt(x, 16)));

/* canvas geometry calls the DOM stub (section 9) watches for NaN / Infinity arguments */
const GEOM = new Set(['arc', 'arcTo', 'ellipse', 'moveTo', 'lineTo', 'rect', 'fillRect', 'strokeRect', 'clearRect', 'quadraticCurveTo', 'bezierCurveTo', 'fillText', 'strokeText', 'translate', 'scale', 'rotate', 'setTransform', 'drawImage', 'roundRect']);
const nanDraws = [];
/* ── 1. golden vectors (the SDK's hostlink_golden.json) ── */
const gArg = process.argv.indexOf('--golden');
/* ../alchemy-sdk beside the repo — or beside any directory above it, so a git worktree under .claude/ finds it too */
const goldenPath = gArg > 0 ? process.argv[gArg + 1] : (() => {
  const rel = ['alchemy-sdk', 'tests', 'host', 'golden', 'hostlink_golden.json']; let dir = path.resolve(here, '..', '..');
  for (;;) { const p = path.join(dir, ...rel); if (existsSync(p)) return p; const up = path.dirname(dir); if (up === dir) return path.resolve(here, '..', '..', ...rel); dir = up; }
})();
if (!existsSync(goldenPath)) { console.log('golden: skipped (no ' + goldenPath + ')'); }
else {
  const g = JSON.parse(readFileSync(goldenPath, 'utf8'));
  const before = pass + fail;
  for (const v of g.crc32) check('crc32 ' + v.hex, HL.crc32(unhex(v.hex)) === v.value);
  for (const v of g.cobs) { const enc = hex(HL.cobsEncode(unhex(v.dec))); const dec = HL.cobsDecode(unhex(v.enc)); check('cobs ' + v.dec.slice(0, 12), enc === v.enc && dec && hex(dec) === v.dec); }
  check('frame HELLO seq1', hex(HL.buildFrame(HL.CMD.hello, 1, new Uint8Array(0))) === g.exchange[0].hex);
  { const p = new HL.FrameParser(); let parsed = null; for (const b of unhex(g.exchange[1].hex)) p.push(b, f => { parsed = f; }); check('frame HELLO response', parsed && parsed.ok && parsed.body[0] === 0); }
  console.log(`golden: ${pass + fail - before - fail}/${pass + fail - before} vectors (crc32 ${g.crc32.length}, cobs ${g.cobs.length}, frame 2)`);
  /* Beyond the CLI's own selftest: every recorded frame in both exchanges must parse clean, and the
   * golden descriptor must pass the same CRC + JSON path the page uses. */
  let n = 0, ok = 0;
  for (const x of [...g.exchange, ...(g.fs_exchange || [])]) { const p = new HL.FrameParser(); let f = null; for (const b of unhex(x.hex)) p.push(b, r => { f = r; }); n++; if (f && f.ok && (x.dir === 'h2d' || f.type & 0x80)) ok++; }
  check('all recorded frames parse', ok === n, `${ok}/${n}`);
  if (g.descriptor) { const d = unhex(g.descriptor.hex); check('descriptor crc', HL.crc32(d) === g.descriptor.crc); let j = null; try { j = JSON.parse(new TextDecoder().decode(d)); } catch {} check('descriptor json', j && j.dv === 1 && Array.isArray(j.components)); check('descriptor pagerInfo', !!HL.pagerInfo(j)); }
}

/* ── 2. StateExt parsing on a synthetic body: every field where the firmware puts it ── */
function stateBody(len = HL.STATE_LEN) {
  const b = new Uint8Array(len); b[0] = 0; b[1] = 32; b[2] = 7; b[3] = 0b10110; b[4] = 9; b[5] = 2; b[6] = 1; b[7] = 3; b[8] = 200; b[9] = 5; b[10] = 2; b[11] = 255;
  const dv = new DataView(b.buffer); dv.setFloat32(12, 174.5, true); dv.setFloat32(16, 0.5, true);
  for (let i = 0; i < 64; i++) { b[20 + i] = i < 32 ? (i % 5 === 0 ? 0xff : i) : 0; b[84 + i] = i % 4; }
  if (len < HL.STATE_LEN) return b;
  dv.setUint16(148, 1234, true); dv.setUint16(150, 210, true); dv.setUint16(152, 3, true); dv.setUint16(154, 17, true);
  dv.setUint16(156, 0x0041, true); dv.setFloat32(158, 172.25, true); b[162] = 2; b[163] = 3;
  dv.setUint16(164, 350, true); b[166] = 1;
  b[167] = 0x33; dv.setUint16(168, 120, true);   /* engaged · pending · 3 ext transitions; 120 µs rms */
  b[170] = 4; dv.setUint16(171, 1500, true); dv.setUint16(173, 2, true);
  b[175] = 1;                                     /* Voice jack map */
  b[176] = 5;                                     /* LOOPER: five events */
  b[177] = 2; b[178] = 7; dv.setUint32(179, 250000, true); dv.setUint32(183, 480000, true); b[187] = 1; b[188] = 180; b[189] = 35; dv.setUint32(190, 192000, true);
  for (let k = 0; k < 24; k++) b[194 + k] = k === 7 ? 34 : Math.floor(k * 32 / 24);
  b[218] = 1;                                     /* Key Plays = Held */
  b[219] = 1; b[220] = 128; b[221] = 2; b[222] = 9;   /* SETS: saving, 50 %, last result card full, seq 9 */
  { const nm = 'Amen_kit-2'; for (let i = 0; i < nm.length; i++) b[223 + i] = nm.charCodeAt(i); }
  b[255] = 77; b[256] = 3; b[257] = 255;          /* EDIT: tableGen 77, undo depth 3, no selection */
  dv.setUint32(258, 123456, true);                /* EDIT: srcPos */
  b[262] = 2;                                     /* EDIT-UNDO: redo depth 2 */
  b[263] = 3; dv.setUint16(264, 412, true); dv.setUint16(266, 905, true); dv.setInt16(268, -250, true); b[270] = 0; b[271] = 255; b[272] = 255; b[273] = 17; b[274] = 12; b[275] = 5;   /* KEYS */
  b[287] = 255; b[297] = 255;                     /* CARD: no last upload · PADS: no drop */
  return b;
}
{
  check('state too short → null (not this firmware\'s body)', HL.parseState(stateBody(100)) === null && HL.parseState(stateBody(HL.STATE_LEN - 1)) === null && HL.STATE_LEN === 315);
  const s0 = HL.parseState(stateBody());
  check('state fixed part', s0 && s0.nSlices === 32 && s0.cur === 7 && s0.ext && s0.locked && !s0.muted && !s0.flowing && s0.brain && s0.grammar === 2 && s0.cls === 1 && s0.file === 3 && s0.chop === 200 && s0.histN === 5 && s0.histDepth === 2 && s0.callFlash === 255);
  check('state floats', Math.abs(s0.bpm - 174.5) < 1e-4 && s0.mult === 0.5);
  check('state src signed', s0.src[0] === -1 && s0.src[1] === 1 && s0.src[5] === -1 && s0.stepClass[7] === 3);
  check('state diag', s0.diag.gap === 1234 && s0.diag.audio === 210 && s0.diag.jumps === 3 && s0.diag.regens === 17);
  check('state locks, native tempo, bars', s0.lockMask === 0x41 && Math.abs(s0.nativeBpm - 172.25) < 1e-4 && s0.gridBars === 2 && s0.barsOpt === 3);
  check('state CLOCK: jitter, the requested zone, the flags', s0.jitterUs === 350 && s0.multZone === 1 && s0.clkFlags === 0x33 && s0.jitterRmsUs === 120 && s0.p2Engaged === true && s0.multPending === true && s0.aligned === false && s0.extTrans === 3);
  check('state stall forensics', s0.stallPhase === 4 && s0.gateLateUs === 1500 && s0.overruns === 2);
  const s4 = s0;
  check('zone table', HL.ZONE_MULT.length === 5 && HL.ZONE_MULT[2] === 1 && HL.ZONE_MULT[s4.multZone] === 0.5);
  const sc = HL.parseScope(Uint8Array.of(0, 4, 0, 1, 0xff, 0x7f, 0x80)); check('scope signed', sc.length === 4 && sc[1] === -1 && sc[2] === 127 && sc[3] === -128);
  /* SCOPE: the four-channel body — request builder, reply parser, the frame planner. */
  { const r = HL.scopeReq(0x0f, 4, 253, HL.SCOPE.trig | HL.SCOPE.peak);
    check('scope2 request: 5 bytes, mask/decim/n/flags', r.length === 5 && r[0] === 15 && r[1] === 4 && HL.u16(r, 2) === 253 && r[4] === 3);
    const ra = HL.scopeReq(0x0c, 4, 253, HL.SCOPE.trig, 0xfffffff0);
    check('scope2 request with pos: 9 bytes, at flag added (the firmware lets it win over trig)', ra.length === 9 && ra[4] === (HL.SCOPE.at | HL.SCOPE.trig) && HL.u32(ra, 5) === 0xfffffff0);
    const b = new Uint8Array(10 + 3 * 4); b[0] = 0; b[1] = 0x0b; b[2] = 2; b[3] = 4; b[4] = 0; b[5] = 1; new DataView(b.buffer).setUint32(6, 123456789, true);
    b.set([1, 2, 3, 4], 10); b.set([0xff, 0x7f, 0x80, 0], 14); b.set([9, 8, 7, 6], 18);
    const f = HL.parseScope2(b);
    check('scope2 reply: header', f && f.mask === 0x0b && f.decim === 2 && f.n === 4 && f.pos === 123456789 && f.triggered && !f.atHonoured);
    check('scope2 reply: channels in mask order, signed', f.ch[0][3] === 4 && f.ch[1][0] === -1 && f.ch[1][1] === 127 && f.ch[1][2] === -128 && !f.ch[2] && f.ch[3][0] === 9 && f.ch[3][3] === 6);
    check('scope2 reply: short → null', HL.parseScope2(Uint8Array.of(0, 1, 1, 0)) === null);
    check('scope2 reply: at honoured flag', HL.parseScope2(Uint8Array.of(0, 1, 1, 0, 0, 2, 0, 0, 0, 0)).atHonoured === true);
    const p1 = HL.scopePlan(1024, 0x0f, 4), p2 = HL.scopePlan(512, 0x0f, 4), p3 = HL.scopePlan(512, 0x03, 4), p4 = HL.scopePlan(1024, 0x0f, 16), p5 = HL.scopePlan(2048, 0x0f, 1), p6 = HL.scopePlan(512, 0x01, 1);
    check('scope plan: 1024 holds four channels in one request (253)', p1.n === 253 && p1.masks.length === 1 && p1.masks[0] === 0x0f && 10 + 4 * p1.n <= 1024);
    check('scope plan: 512 splits four channels into two requests of two (251)', p2.n === 251 && p2.masks.length === 2 && p2.masks[0] === 0x03 && p2.masks[1] === 0x0c && 10 + 2 * p2.n <= 512);
    check('scope plan: 512 holds two channels in one request', p3.n === 251 && p3.masks.length === 1);
    check('scope plan: ×16 at 1024 is still one request of four (253: the 64 K ring holds 512 × 16)', p4.n === 253 && p4.masks.length === 1 && HL.scopePlan(1024, 0x01, 16).n === 512);
    const p7 = HL.scopePlan(512, 0x0f, 16);
    check('scope plan: a split frame at 512 is two requests of two channels (251)', p7.n === 251 && p7.masks.length === 2 && p7.reqs[1].mask === 0x0c);
    /* the ext form: jacks, their own decimation, the trigger level and source */
    const pe = HL.scopePlan(1024, 0x01, 4, 512, 200, 0x38, 1), pe2 = HL.scopePlan(1024, 0x0f, 4, 512, 200, 0x3f, 4), pe3 = HL.scopePlan(1024, 0, 1, 512, 200, 0x02, 32), pe4 = HL.scopePlan(512, 0x01, 16, 512, 200, 0x38, 1);
    check('scope plan: out L + J6/J7/J8 at 1024 in one request (252)', pe.ext && pe.n === 252 && pe.reqs.length === 1 && pe.reqs[0].mask === 1 && pe.reqs[0].maskHi === 0x38 && 14 + 4 * 252 <= 1024);
    check('scope plan: all ten channels at 1024 → two requests of five (202)', pe2.n === 202 && pe2.reqs.length === 2 && pe2.reqs[0].mask === 0x0f && pe2.reqs[0].maskHi === 1 && pe2.reqs[1].mask === 0 && pe2.reqs[1].maskHi === 0x3e);
    check('scope plan: a lone jack at ×32 is held to the cv ring (511)', pe3.n === 511 && pe3.reqs.length === 1 && 511 * 32 <= HL.SCOPE.cvUsable);
    check('scope plan: the gates preset at 512 splits into two requests', pe4.ext && pe4.reqs.length === 2 && pe4.n === 249);
    const re = HL.scopeReq(0x01, 16, 249, HL.SCOPE.trig, null, { maskHi: 0x38, cvDecim: 1, level: 25, trig: HL.SCOPE.trigSet | 7 });
    check('scope2 ext request: 13 bytes, ext flag, pos 0, maskHi / cvDecim / level / trigSrc', re.length === 13 && re[4] === (HL.SCOPE.trig | HL.SCOPE.ext) && HL.u32(re, 5) === 0 && re[9] === 0x38 && re[10] === 1 && re[11] === 25 && re[12] === 0x87);
    const rea = HL.scopeReq(0x01, 16, 249, HL.SCOPE.trig, 777, { maskHi: 0x38, cvDecim: 1, level: -3, trig: 0 });
    check('scope2 ext request with pos: at + ext, a negative level as i8', rea.length === 13 && rea[4] === (HL.SCOPE.trig | HL.SCOPE.at | HL.SCOPE.ext) && HL.u32(rea, 5) === 777 && rea[11] === 253);
    { const b = new Uint8Array(14 + 3 * 2); b[0] = 0; b[1] = 0x01; b[2] = 4; b[3] = 2; b[4] = 0; b[5] = 1 | 4 | 8; new DataView(b.buffer).setUint32(6, 1610612000, true); b[10] = 0x28; b[11] = 8; b[12] = 24; b[13] = 7;
      b.set([5, 6], 14); b.set([127, 0], 16); b.set([-64 & 0xff, 25], 18);
      const f = HL.parseScope2(b);
      check('scope2 ext reply: header, flags, jacks keyed 4..9 in bit order', f && f.ext && f.detached && f.triggered && f.maskHi === 0x28 && f.cvDecim === 8 && f.cvDiv === 24 && f.trigSrc === 7 && f.ch[0][1] === 6 && f.ch[7][0] === 127 && f.ch[9][0] === -64 && f.ch[9][1] === 25 && !f.ch[4] && !f.ch[8]);
      check('scope2 ext reply too short for its header → null', HL.parseScope2(b.subarray(0, 12)) === null); }
    check('scope volts', HL.scopeVolts(127) > 4.99 && HL.scopeVolts(127) < 5.01 && HL.scopeCvCode(1) === 25 && HL.scopeCvCode(-9) === -127 && HL.scopeCvCode(0) === 0);
    check('scope age wraps at the clock', HL.scopeAge(10, 5) === 5 && HL.scopeAge(3, HL.SCOPE.wrap - 2) === 5 && HL.scopeAge(3, HL.SCOPE.cvWrap - 2, HL.SCOPE.cvWrap) === 5 && HL.SCOPE.cvWrap * 24 === HL.SCOPE.wrap);
    check('jack roles under both maps', HL.SCOPE_JACK_ROLE[0][3] === 'kick gate out' && HL.SCOPE_JACK_ROLE[1][3] === 'gate in' && HL.SCOPE_JACK_IN[0].filter(Boolean).length === 3 && HL.SCOPE_JACK_IN[1].filter(Boolean).length === 4 && HL.SCOPE_CH.length === 10 && HL.SCOPE_CH[9] === 'J8');
    check('scope plan: 2048 gives 509 × 4 in one request', p5.n === 509 && p5.masks.length === 1);
    check('scope plan: one channel at 512 → 502', p6.n === 502 && p6.masks.length === 1);
    check('scope plan: empty mask → nothing', HL.scopePlan(1024, 0, 4).masks.length === 0);
    check('scope channel names', HL.SCOPE_CH.length === 10 && HL.SCOPE_CH[2] === 'in L' && HL.SCOPE_CH[4] === 'J3'); }
  /* SAMPLE: the jack map, the looper, the record state, the buffer, the key map, Key Plays; SETS: the op, the name */
  const s7 = HL.parseState(stateBody());
  check('state: jack map, looper events', s7.jackMap === 1 && s7.looperEvents === 5);
  check('state: record + buffer', s7.recState === 2 && HL.REC[s7.recState] === 'taking' && s7.recKey === 7 && s7.freePos === 250000 && s7.bufCap === 480000 && s7.stereo === true && s7.inLevel === 180 && s7.nTable === 35 && s7.bufLen === 192000);
  check('state: key table, key plays', s7.keySlice.length === 24 && s7.keySlice[7] === 34 && s7.keySlice[23] === 30 && s7.keySlice[0] === 0 && s7.keyPlays === 1);
  check('state: set op', s7.setState === 1 && HL.SET_STATE[s7.setState] === 'saving' && s7.setProgress === 128 && s7.setResult === 2 && HL.SET_RESULT[s7.setResult] === 'card full' && s7.setSeq === 9);
  check('state: set name', s7.setName === 'Amen_kit-2');
  { const b = stateBody(); b[223] = 0; check('state: empty name', HL.parseState(b).setName === ''); }
  check('set names', HL.setNameOk('amen') && HL.setNameOk('Amen_170-v2') && HL.setNameOk('a'.repeat(31)) && !HL.setNameOk('') && !HL.setNameOk('a'.repeat(32)) && !HL.setNameOk('a b') && !HL.setNameOk('a/b') && !HL.setNameOk('a.wav') && !HL.setNameOk(null));
  check('set labels', HL.setLabelName('amen [set]') === 'amen' && HL.setLabelName('AMEN_170.WAV') === null && HL.setLabelName('a b [set]') === null && HL.setLabelName('sets/amen.wav') === null);
  check('name bytes', HL.nameBytes('ab').length === 2 && HL.nameBytes('ab')[0] === 97 && HL.ACT.saveSet === 14 && HL.ACT.loadSet === 15);
  { const b = stateBody(); b[178] = 255; check('recKey 255 → -1', HL.parseState(b).recKey === -1); }
  /* GET_WAVE / GET_TABLE / GET_HITS bodies. */
  const wv = HL.parseWave(Uint8Array.of(0, 0x10, 0x27, 0, 0, 3, 0, 4, 0x7f, 0x81, 0));
  check('wave body', wv.start === 10000 && wv.n === 3 && wv.step === 4 && wv.data.length === 3 && wv.data[0] === 127 && wv.data[1] === -127 && wv.data[2] === 0);
  { const h = new Uint8Array(3 + 8); h[0] = 0; h[1] = 130; h[2] = 128; new DataView(h.buffer).setUint32(3, 1, true); new DataView(h.buffer).setUint32(7, 0xfffffffe, true);
    const hc = HL.parseHitsChunk(h); check('hits chunk', hc.total === 130 && hc.first === 128 && hc.positions.length === 2 && hc.positions[1] === 0xfffffffe); }
  check('key names: two octaves from C', HL.keyName(0) === 'C1' && HL.keyName(11) === 'B1' && HL.keyName(12) === 'C2' && HL.keyName(23) === 'B2' && HL.keyName(1) === 'C♯1');
}

/* ── 2b. Link hygiene (Kyklophoria's two fixes): the accumulator cap, and the lone zero that resyncs a
 *        module left holding half a frame ── */
{
  const fp = new HL.FrameParser(); let got = 0;
  for (let i = 0; i < 5000; i++) fp.push(1, () => got++);
  check('accumulator capped', fp.acc.length <= HL.kMaxFrameBytes && fp.dropped === 1 && fp.over === true);
  fp.push(0, () => got++);                                              /* the delimiter ends the junk */
  for (const b of HL.buildFrame(HL.CMD.hello, 7, new Uint8Array(0))) fp.push(b, f => { got++; check('frame after resync parses', f.ok && f.seq === 7); });
  check('junk produced no frame', got === 1);
  /* a module holding half a frame: write half a HELLO into the fake port before the link starts */
  const half = new DEMO.FakeDevice({ latencyMs: 2 }); const hp = new DEMO.FakePort(half);
  const w0 = hp.writable.getWriter(); const fr = HL.buildFrame(HL.CMD.hello, 1, new Uint8Array(0)); await w0.write(fr.subarray(0, fr.length >> 1)); w0.releaseLock();
  const hl = new HL.Link(hp); await hl.start();
  const t0 = Date.now(); const info = await HL.hello(hl); const dt = Date.now() - t0;
  check('hello answers first time after a half frame', info && info.name && hl.stats.timeouts === 0, `timeouts ${hl.stats.timeouts} in ${dt} ms`);
  await hl.close();
}

/* ── 2c. Transports (Kyklophoria's split): Link(transport), bytesIn / kbps, and unplug detection — the read loop
 *        ending without a close() is onClose, which the page turns into a disconnect ── */
{
  const nap = ms => new Promise(res => setTimeout(res, ms));
  const dv = new DEMO.FakeDevice({ latencyMs: 1 }), port = new DEMO.FakePort(dv);
  const l = new HL.Link(new HL.SerialTransport(port)); let closed = 0; l.onClose = () => closed++;
  await l.start(); const info = await HL.hello(l);
  check('Link over an explicit SerialTransport (the fake port), kind serial, the port reachable', l.kind === 'serial' && l.port === port && info.name === 'Audiothurgist');
  check('Link given a bare port wraps it in a SerialTransport', new HL.Link(new DEMO.FakePort(dv)).t instanceof HL.SerialTransport);
  check('bytesIn counts what arrived', l.stats.bytesIn > 60);
  l._win.t0 -= 1000; await l.request(HL.CMD.state); check('kbps from the last one-second window', l.stats.kbps > 0 && l.stats.rate > 0);
  port.unplug(); await nap(30);
  check('the cable pulled: the read loop ends and onClose fires, once', closed === 1);
  const p2 = new DEMO.FakePort(dv), l2 = new HL.Link(p2); let closed2 = 0; l2.onClose = () => closed2++;
  await l2.start(); await HL.hello(l2); await l2.close(); await nap(20);
  check('a close() the page asked for is not an unplug', closed2 === 0 && l2.closed);
  const ws = { sent: null, send(b) { this.sent = b; }, close() { this.shut = true; } }, wt = new HL.WsTransport(ws); let got = null, wc = 0;
  wt.onClose = () => wc++; await wt.start(b => { got = b; }); await wt.write(Uint8Array.of(9));
  ws.onmessage({ data: Uint8Array.of(1, 2, 3).buffer }); ws.onclose();
  check('WsTransport: bytes in, bytes out, a socket that closes by itself is onClose', got && got.length === 3 && ws.sent[0] === 9 && wc === 1 && wt.kind === 'bridge');
}

/* ── 3. The Link end to end over the fake module: priority, coalescing, RTT, live blob ── */
const dev = new DEMO.FakeDevice({ latencyMs: 3 });
const link = new HL.Link(new DEMO.FakePort(dev));
await link.start();
{
  const info = await HL.hello(link);
  check('hello', info.name === 'Audiothurgist' && info.maxBody === DEMO.MAX_BODY && info.maxBody === 1024 && info.liveSize === DEMO.LIVE_SIZE && info.schema === DEMO.SCHEMA);
  const desc = await HL.getDescriptor(link, info);
  check('descriptor crc+json, the firmware\'s api = the page\'s', desc && desc.dv === 1 && HL.apiOf(desc) === HL.API && Object.keys(desc.audiothurgist).join() === 'api');
  const opts = HL.optionsInfo(desc);
  const wantIds = DEMO.OPTION_FIELDS.map(f => f[0]);
  check('options block: 25 enum fields with the firmware ids (OPT9: Jacks, Key Plays, the record trio, Quantize, Tap Allowance, Autosave, Start In)', opts && opts.fields.length === DEMO.OPTION_FIELDS.length && opts.fields.length === 25 && opts.comp.hash === 0x4F505439 && ['jacks', 'sampling', 'rec.src', 'record', 'quantize', 'autosave', 'start.in'].every(id => opts.fields.some(f => f.id === id)) && opts.fields.some(f => f.id === 'tap.allow') && wantIds.every(id => opts.fields.some(f => f.id === id)) && opts.fields.some(f => f.id === 'key.plays'));
  const gl = HL.fieldLabels(desc, 'grammar.mode');
  check('labels from disp', gl[3] === 'Euclid' && HL.fieldLabels(desc, 'sample.idx').length === DEMO.FILES.length);
  check('grammar has seven labels: Camp at 4, Play at 5, Pads at 6', gl.length === 7 && gl[4] === 'Camp' && /^Play/.test(gl[5]) && /^Pads/.test(gl[6]) && opts.fields.find(f => f.id === 'grammar.mode').zones === 7);
  const pg = HL.pagerInfo(desc); check('pager: 2 pages × 6 knobs with help', pg && pg.pages === 2 && Object.keys(pg.knobs).length === 12 && /Grids/.test(pg.knobs['1:0'].help));
  const man = HL.manualInfo(desc); check('manual block', man && man.sections.length >= 5 && man.sections.some(s => s.id === 'keymap' && /0 V = key 0/.test(s.body)) && man.sections.some(s => s.id === 'buttons' && /start \/ stop/.test(s.body)) && man.jacks.length === 10 && man.tagline);

  /* Priority: queue five polls, then an urgent action → the action reaches the device before the polls still queued. */
  dev.log.length = 0;
  const polls = []; for (let i = 0; i < 5; i++) polls.push(link.request(HL.CMD.state));
  const act = link.request(HL.CMD.action, Uint8Array.of(HL.ACT.setMute, 1), { urgent: true });
  await Promise.all([...polls, act]);
  const iAct = dev.log.indexOf(HL.CMD.action);
  check('urgent action jumps the poll queue', iAct === 1, 'device saw ' + dev.log.map(t => t.toString(16)).join(','));
  check('action applied', dev.muted === true);
  /* Coalescing: the same key queued twice is one wire request. */
  dev.log.length = 0;
  const a = link.request(HL.CMD.scope, undefined, { key: 'scope' }), b = link.request(HL.CMD.scope, undefined, { key: 'scope' });
  await Promise.all([a, b]); check('keyed requests coalesce', a === b && dev.log.filter(t => t === HL.CMD.scope).length === 1);
  /* Status errors surface by name; unknown ops are BAD_ARGS. */
  let err = null; try { await link.request(HL.CMD.action, Uint8Array.of(99), { urgent: true }); } catch (e) { err = e; }
  check('device status → error', err && /BAD_ARGS/.test(err.message));
  check('ACTION 2 (the old mute toggle) is gone: BAD_ARGS', await link.request(HL.CMD.action, Uint8Array.of(2), { urgent: true }).then(() => false, e => /BAD_ARGS/.test(e.message)));
  /* State through the real parser. */
  const st = HL.parseState(await link.request(HL.CMD.state));
  check('demo state parses: tempo, bars, clock', st && st.muted && st.nSlices === 32 && st.nativeBpm === 120 && st.gridBars === 2 && st.multZone === 2 && st.jitterUs === 0);
  check('demo state carries the clock flags', st.clkFlags === 0 && st.jitterRmsUs === 0 && st.p2Engaged === false && st.extTrans === 0);
  /* Flip the fake module onto a clock with a pending ×4 switch: engaged + pending + one ext transition. */
  dev.ext = true; dev.locked = true; dev.mult = 2; dev.multZone = 4; dev.p2Engaged = true; dev.pending = true;
  const se = HL.parseState(await link.request(HL.CMD.state));
  check('demo clock flags: engaged, pending, ext transition counted', se.ext && se.mult === 2 && se.multZone === 4 && se.p2Engaged && se.multPending && !se.aligned && se.extTrans === 1 && se.jitterRmsUs > 0 && se.jitterUs > se.jitterRmsUs);
  dev.ext = false; dev.locked = false; dev.mult = 1; dev.multZone = 2; dev.p2Engaged = false; dev.pending = false;
  const env = await HL.fetchEnv(link, st.nSlices); check('env fetched', env[1] > 128 && env[0] < 128);
  /* Live blob: read, flip Grammar to Euclid, write back, read again. */
  const live = await HL.getLive(link, info); check('getLive size', live.bytes.length === DEMO.LIVE_SIZE && live.schema === DEMO.SCHEMA);
  const gf = opts.fields.find(f => f.id === 'grammar.mode'); const bytes = Uint8Array.from(live.bytes); bytes[opts.comp.off + gf.off] = 3;
  await HL.setLive(link, info, bytes);
  const again = await HL.getLive(link, info); check('setLive round-trip', again.bytes[opts.comp.off + gf.off] === 3);
  check('state reflects the option', HL.parseState(await link.request(HL.CMD.state)).grammar === 3);
  /* …and Camp (index 4) is a legal value the fake module accepts and reports. */
  bytes[opts.comp.off + gf.off] = 4; await HL.setLive(link, info, bytes);
  const stc = HL.parseState(await link.request(HL.CMD.state)); check('Camp selectable: state grammar 4 labelled Camp', stc.grammar === 4 && gl[stc.grammar] === 'Camp');
  check('rtt measured', link.stats.rtt > 0 && link.stats.reqs > 10);
  await link.close();
}

/* ── 4. The Kit: table / wave / hits and the record + looper actions over a 64-slice kit demo ── */
{
  const kit = new DEMO.FakeDevice({ kit: true, nSlices: 64, latencyMs: 2, swing: true });
  const kl = new HL.Link(new DEMO.FakePort(kit)); await kl.start();
  const info = await HL.hello(kl); const desc = await HL.getDescriptor(kl, info);
  const s = HL.parseState(await kl.request(HL.CMD.state));
  check('kit state tail', s.nSlices === 64 && s.nTable === 67 && s.bufLen === 64 * DEMO.SLICE_LEN && s.freePos > s.bufLen && s.bufCap === DEMO.BUF_CAP && s.recState === 0 && s.recKey === -1 && s.looperEvents === 3 && s.keySlice.length === 24 && s.keyPlays === 0);
  check('kit keys: recorded keys point past the grid, others at their span slice', s.keySlice[3] === 64 && s.keySlice[7] === 65 && s.keySlice[14] === 66 && s.keySlice[0] === 0 && s.keySlice[23] === 61);
  /* GET_TABLE chunks: 67 entries = 39 + 28. */
  kit.log.length = 0;
  const tb = await HL.fetchTable(kl);
  check('table fetched in two chunks', kit.log.filter(t => t === HL.CMD.table).length === 2 && tb.nSlices === 64 && tb.nTable === 67 && tb.entries.length === 67);
  check('39 × 14 fit max_body 1024', 3 + 39 * HL.TABLE_ENTRY <= 1024 && HL.TABLE_CHUNK === 39 && HL.TABLE_ENTRY === 14);
  check('table: grid entries keyless and contiguous, swung odd boundaries', tb.entries[0].key === -1 && tb.entries[1].start === DEMO.SLICE_LEN + 900 && tb.entries[0].len === DEMO.SLICE_LEN + 900 && tb.entries[2].start === 2 * DEMO.SLICE_LEN && tb.entries.slice(0, 64).every((e, i, a) => i === 0 || e.start === a[i - 1].start + a[i - 1].len));
  check('table: recorded entries carry their key, class, hit', tb.entries[64].key === 3 && tb.entries[64].cls === 0 && tb.entries[65].key === 7 && tb.entries[65].len === Math.round(0.9 * DEMO.SR) && tb.entries[66].key === 14 && tb.entries[66].hit === true && tb.entries[64].start === s.bufLen);
  { let bad = null; try { await kl.request(HL.CMD.table, Uint8Array.of(70, 10)); } catch (e) { bad = e; } check('table: first past nTable → BAD_ARGS', bad && /BAD_ARGS/.test(bad.message)); }
  /* GET_HITS: 64 hits minus the muted ones, plus ghosts; every position inside the material and ascending. */
  const hits = await HL.fetchHits(kl);
  check('hits fetched', hits.length === kit.hits.length && hits.length > 64 && hits.every((p, i) => p < s.bufLen && (i === 0 || p >= hits[i - 1])));
  /* GET_WAVE: a raw window, a decimated window, the clamp at freePos, BAD_ARGS past it. */
  const e0 = tb.entries[64];
  const w1 = await HL.fetchWave(kl, e0.start, 480, 0, 1);
  check('wave raw window', w1.start === e0.start && w1.n === 480 && w1.step === 1 && w1.data.length === 480 && w1.data.some(v => v > 40));
  const w2 = await HL.fetchWave(kl, e0.start, 480, 2, 128);
  check('wave decimated window', w2.step === 128 && w2.n === Math.min(480, Math.floor((s.freePos - e0.start) / 128)) && w2.data.some(v => v !== 0));
  const w3 = await HL.fetchWave(kl, s.freePos - 100, 480, 0, 1);
  check('wave clamps at freePos', w3.n === 100);
  { let bad = null; try { await HL.fetchWave(kl, s.freePos, 480, 0, 1); } catch (e) { bad = e; } check('wave past freePos → BAD_ARGS', bad && /BAD_ARGS/.test(bad.message)); }
  { let bad = null; try { await HL.fetchWave(kl, 0, 480, 3, 1); } catch (e) { bad = e; } check('wave bad plane → BAD_ARGS', bad && /BAD_ARGS/.test(bad.message)); }
  kit.log.length = 0; await Promise.all([HL.fetchWave(kl, 0, 10), HL.fetchWave(kl, 0, 10)]);
  check('wave requests coalesce on their key', kit.log.filter(t => t === HL.CMD.wave).length === 1);
  /* ACTION 9: arm → the tail says armed on that key; a second arm is BAD_STATE. */
  await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.recArm, 5), { urgent: true });
  const sa = HL.parseState(await kl.request(HL.CMD.state));
  check('rec arm: state armed on key 5', sa.recState === 1 && HL.REC[sa.recState] === 'armed' && sa.recKey === 5 && sa.inLevel > 0);
  { let bad = null; try { await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.recArm, 6), { urgent: true }); } catch (e) { bad = e; } check('rec arm while armed → BAD_STATE', bad && /BAD_STATE/.test(bad.message)); }
  { let bad = null; try { await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.recArm, 24), { urgent: true }); } catch (e) { bad = e; } check('rec arm key 24 → BAD_ARGS', bad && /BAD_ARGS/.test(bad.message)); }
  /* Let the fake take run its course: armed → taking → pending → committed into key 5. */
  kit.rec.t -= 5000; HL.parseState(await kl.request(HL.CMD.state));   /* → taking */
  kit.rec.t -= 5000; const st2 = HL.parseState(await kl.request(HL.CMD.state));
  check('rec taking → pending', st2.recState === 3 && st2.recKey === 5);
  kit.rec.t -= 5000; const st3 = HL.parseState(await kl.request(HL.CMD.state));
  check('rec committed: idle, key 5 points at a new entry, freePos advanced', st3.recState === 0 && st3.recKey === -1 && st3.nTable === 68 && st3.keySlice[5] === 67 && st3.freePos > s.freePos);
  /* ACTION 10: clear key 3 → the recording stays, keyless and at its index (Engine::ClearKey: nothing shifts), key 3
   * falls back to its span slice, every other key holds. */
  await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.recClear, 3), { urgent: true });
  const st4 = HL.parseState(await kl.request(HL.CMD.state));
  check('rec clear: index-stable — the entry stays keyless, key 3 → its span slice, the others hold', st4.nTable === 68 && st4.keySlice[3] === 8 && st4.keySlice[7] === 65 && st4.keySlice[14] === 66 && st4.keySlice[5] === 67);
  { const t4 = await HL.fetchTable(kl); check('rec clear: entry 64 is still there, keyless, its span intact', t4.entries[64].key === -1 && t4.entries[64].start === s.bufLen && t4.entries[64].len === Math.round(0.35 * DEMO.SR)); }
  { let bad = null; try { await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.recClear, 3), { urgent: true }); } catch (e) { bad = e; } check('rec clear an empty key → BAD_STATE', bad && /BAD_STATE/.test(bad.message)); }
  /* ACTION 11: Key Plays lands in the Options byte the blob carries. */
  await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.keyPlays, 1), { urgent: true });
  const live = await HL.getLive(kl, info); const opts = HL.optionsInfo(desc); const kp = opts.fields.find(f => f.id === 'key.plays');
  check('key plays → options byte and the state tail', live.bytes[opts.comp.off + kp.off] === 1 && HL.parseState(await kl.request(HL.CMD.state)).keyPlays === 1);
  { let bad = null; try { await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.keyPlays, 2), { urgent: true }); } catch (e) { bad = e; } check('key plays 2 → BAD_ARGS', bad && /BAD_ARGS/.test(bad.message)); }
  /* ACTION 12 / 13: looper undo and clear. */
  await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.looperUndo), { urgent: true });
  check('looper undo', HL.parseState(await kl.request(HL.CMD.state)).looperEvents === 2);
  await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.looperClear), { urgent: true });
  check('looper clear', HL.parseState(await kl.request(HL.CMD.state)).looperEvents === 0);
  await kl.close();
}

/* ── 5. Sets over the demo: save, refusals, load, not found ── */
{
  const d = new DEMO.FakeDevice({ kit: true, nSlices: 64, latencyMs: 2, swing: true });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l); const desc = await HL.getDescriptor(l, info);
  const labs = HL.fieldLabels(desc, 'sample.idx');
  check('sample list carries the set label', labs.includes('amen_kit [set]') && labs.map(HL.setLabelName).filter(Boolean).join() === 'amen_kit');
  /* an op starts on the first state poll after its request and finishes 600 ms later: poll, rewind, poll */
  const finish = async () => { await l.request(HL.CMD.state); d.set.t -= 2000; return HL.parseState(await l.request(HL.CMD.state)); };
  const s0 = HL.parseState(await l.request(HL.CMD.state));
  check('sets tail idle', s0.setState === 0 && s0.setProgress === 0 && s0.setResult === 0 && s0.setSeq === 0 && s0.setName === '');
  { let bad = null; try { await HL.saveSet(l, 'a b'); } catch (e) { bad = e; } check('saveSet refuses a bad name locally', bad && /bad set name/.test(bad.message)); }
  { let bad = null; try { await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.saveSet, 0x61, 0x20), { urgent: true }); } catch (e) { bad = e; } check('ACTION 14 with a bad name → BAD_ARGS', bad && /BAD_ARGS/.test(bad.message)); }
  { let bad = null; try { await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.saveSet), { urgent: true }); } catch (e) { bad = e; } check('ACTION 14 with no name → BAD_ARGS', bad && /BAD_ARGS/.test(bad.message)); }
  /* save: accepted at once, then the state shows saving with progress, then done with the seq bumped */
  await HL.saveSet(l, 'kit_1');
  const s1 = HL.parseState(await l.request(HL.CMD.state));
  check('save accepted: state saving', s1.setState === 1 && HL.SET_STATE[s1.setState] === 'saving' && s1.setSeq === 0);
  { let bad = null; try { await HL.loadSet(l, 'kit_1'); } catch (e) { bad = e; } check('an op while saving → BAD_STATE', bad && /BAD_STATE/.test(bad.message)); }
  d.set.t -= 300; const s2 = HL.parseState(await l.request(HL.CMD.state));
  check('save progresses', s2.setState === 1 && s2.setProgress > 100 && s2.setProgress < 255);
  d.set.t -= 1000; const s3 = HL.parseState(await l.request(HL.CMD.state));
  check('save done: idle, ok, seq bumped, the material is the set', s3.setState === 0 && s3.setResult === 0 && s3.setSeq === 1 && s3.setName === 'kit_1' && d.sets.has('kit_1'));
  /* the firmware appends it to its list (AddSetEntry) and keeps its boot descriptor: the list grows, the dropdown does not */
  check('the saved set joined the list at the end (the module list; the descriptor is the boot one)', d.files.slice(-1)[0] === 'kit_1 [set]' && s3.file === labs.length && HL.fieldLabels(await HL.getDescriptor(l, await HL.hello(l)), 'sample.idx').length === labs.length);
  /* change the kit, then load the set back: the table and keys return */
  await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.recClear, 3), { urgent: true });
  const s4 = HL.parseState(await l.request(HL.CMD.state)); check('kit changed', s4.nTable === 67 && s4.keySlice[3] === 8);
  await HL.loadSet(l, 'kit_1');
  const s5 = await finish();
  check('load done: table and keys back, material generation moved', s5.setState === 0 && s5.setResult === 0 && s5.setSeq === 2 && s5.setName === 'kit_1' && s5.nTable === 67 && s5.keySlice[3] === 64 && s5.matGen !== s4.matGen);
  /* a set that is not on the card: accepted, then reported */
  await HL.loadSet(l, 'nope');
  const s6 = await finish();
  check('load of a missing set → result not found', s6.setState === 0 && s6.setResult === 5 && HL.SET_RESULT[s6.setResult] === 'not found' && s6.setSeq === 3 && s6.setName === 'kit_1');
  /* the card's refusals: full, no card (the demo keys them on the name) */
  await HL.saveSet(l, 'full');
  check('card full reported', (await finish()).setResult === 2);
  await HL.saveSet(l, 'nocard');
  check('no card reported', (await finish()).setResult === 1);
  /* a save while a take runs is refused */
  await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.recArm, 2), { urgent: true });
  { let bad = null; try { await HL.saveSet(l, 'kit_2'); } catch (e) { bad = e; } check('save while a take runs → BAD_STATE', bad && /BAD_STATE/.test(bad.message)); }
  /* ACTION 6 on a set entry loads the set (the firmware routes list entries under sets/ to LoadSet) */
  await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.load, labs.indexOf('amen_kit [set]')), { urgent: true });
  const s7 = HL.parseState(await l.request(HL.CMD.state)); check('list entry of a set loads it', s7.setName === 'amen_kit' && s7.file === labs.indexOf('amen_kit [set]'));
  await l.close();
}

/* ── 6. SCOPE over the demo: the small output trace, the four-channel body, trigger, at-pos frames, BAD_ARGS, the jacks ── */
{
  const d = new DEMO.FakeDevice({ latencyMs: 1 }); d.t0 -= 2000;   /* two seconds of signal in the rings */
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l); const desc = await HL.getDescriptor(l, info);
  check('max_body 1024', info.maxBody === 1024);
  /* the empty body: the Play tab's trace, 256 × ×4 of the mono sum */
  const old = await l.request(HL.CMD.scope);
  const os = HL.parseScope(old);
  check('empty body → the output trace (u16 256, 256 × i8)', old.length === 259 && HL.u16(old, 1) === 256 && os.length === 256 && os.some(v => v !== 0));
  /* one four-channel frame: 253 × 4 in one request, triggered on out L, four distinct traces */
  const plan = HL.scopePlan(info.maxBody, 0x0f, 4);
  const f = await HL.fetchScopeFrame(l, plan, 4, HL.SCOPE.trig);
  check('scope2 frame: one request, 253 per channel, 1022 bytes', f.reqs === 1 && f.n === 253 && f.bytes === 1022 && Object.keys(f.ch).length === 4);
  check('scope2 frame: triggered on a rising crossing of out L', f.triggered && f.ch[0][0] >= 0);
  const differ = (a, b) => { let n = 0; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++; return n > a.length / 4; };
  check('scope2 frame: the four traces differ', differ(f.ch[0], f.ch[1]) && differ(f.ch[0], f.ch[2]) && differ(f.ch[2], f.ch[3]) && f.ch[2].some(v => v !== 0) && f.ch[3].some(v => v !== 0));
  /* consecutive free-running frames tell apart by pos (the newest window moves with the clock) */
  const f2 = await HL.fetchScopeFrame(l, plan, 4, 0);
  await new Promise(r => setTimeout(r, 25));
  const fu = await HL.fetchScopeFrame(l, plan, 4, 0);
  check('untriggered frames: newest window, no trigger flag, pos moving with the clock', !f2.triggered && !fu.triggered && fu.pos - f2.pos >= 900 && fu.pos - f2.pos < 48000);
  /* peak decimation at ×16 keeps the transients plain picking drops: compare the two on the same window */
  const pp = HL.scopePlan(info.maxBody, 0x01, 16);
  const fa = await HL.fetchScopeFrame(l, pp, 16, HL.SCOPE.trig | HL.SCOPE.peak);
  check('peak frame: ×16 × 512 (the 64 K ring)', fa.n === 512 && fa.ch[0].length === 512);
  const fb = await HL.fetchScopeFrame(l, HL.scopePlan(info.maxBody, 0x01, 4), 4, HL.SCOPE.trig);   /* ×4: 2048 of the 3840 usable, room for two more round trips */
  const bPlain = HL.parseScope2(await l.request(HL.CMD.scope, HL.scopeReq(0x01, 4, fb.n, 0, fb.pos)));
  const bPeak = HL.parseScope2(await l.request(HL.CMD.scope, HL.scopeReq(0x01, 4, fb.n, HL.SCOPE.peak, fb.pos)));
  const energy = a => a.reduce((s, v) => s + Math.abs(v), 0);
  check('at-pos: both requests honoured at the same pos', bPlain.atHonoured && bPeak.atHonoured && bPlain.pos === fb.pos && bPeak.pos === fb.pos && bPlain.n === fb.n);
  check('at-pos: the plain re-read equals the triggered frame sample for sample', bPlain.ch[0].every((v, i) => v === fb.ch[0][i]));
  { const back = k => fa.pos >= k ? fa.pos - k : fa.pos + HL.SCOPE.wrap - k; const full = HL.parseScope2(await l.request(HL.CMD.scope, HL.scopeReq(0x01, 16, 512, 0, back(50000)))); check('a pos 50 000 samples back at ×16 × 512 (8192 span, 65 280 usable) is honoured', full.atHonoured); }
  { const gone = HL.parseScope2(await l.request(HL.CMD.scope, HL.scopeReq(0x01, 16, 512, 0, HL.scopeAge(fa.pos, 70000) >= 0 ? (fa.pos >= 70000 ? fa.pos - 70000 : fa.pos + HL.SCOPE.wrap - 70000) : 0))); check('a pos 70 000 samples back has left the audio ring: not honoured', !gone.atHonoured); }
  check('peak decimation keeps at least the energy plain picking sees', energy(bPeak.ch[0]) >= energy(bPlain.ch[0]) && bPeak.ch[0].every((v, i) => Math.abs(v) >= Math.abs(bPlain.ch[0][i])));
  /* a pos the ring no longer holds is not honoured → the frame is reported torn */
  { let bad = null; try { const t = HL.parseScope2(await l.request(HL.CMD.scope, HL.scopeReq(0x0c, 4, 251, 0, f.pos >= 100000 ? f.pos - 100000 : f.pos + HL.SCOPE.wrap - 100000))); if (!t.atHonoured) bad = 'not honoured'; } catch (e) { bad = e.message; } check('a stale pos is answered but not honoured', bad === 'not honoured'); }
  /* BAD_ARGS: mask 0, mask 16, decim 0 / 17, n 0, a short body, at without pos */
  const bad = async body => { try { await l.request(HL.CMD.scope, body); return null; } catch (e) { return e.message; } };
  check('scope2 BAD_ARGS: mask 0', /BAD_ARGS/.test(await bad(HL.scopeReq(0, 4, 100))));
  check('scope2 BAD_ARGS: mask 16', /BAD_ARGS/.test(await bad(Uint8Array.of(16, 4, 100, 0, 0))));
  check('scope2 BAD_ARGS: decim 0', /BAD_ARGS/.test(await bad(HL.scopeReq(1, 0, 100))));
  check('scope2 BAD_ARGS: decim 17', /BAD_ARGS/.test(await bad(HL.scopeReq(1, 17, 100))));
  check('scope2 BAD_ARGS: n 0', /BAD_ARGS/.test(await bad(HL.scopeReq(1, 4, 0))));
  check('scope2 BAD_ARGS: short body', /BAD_ARGS/.test(await bad(Uint8Array.of(1, 4, 100, 0))));
  check('scope2 BAD_ARGS: at flag without pos', /BAD_ARGS/.test(await bad(Uint8Array.of(1, 4, 100, 0, HL.SCOPE.at))));
  /* clamps: n over 512, and over what fits */
  const big = HL.parseScope2(await l.request(HL.CMD.scope, HL.scopeReq(0x03, 1, 9999)));
  check('scope2 clamps n to what fits two channels at 1024 (507)', big.n === 507 && big.ch[1].length === 507);
  /* ── the jacks: cv-only, mixed aligned, gate-triggered, detached, ext BAD_ARGS ── */
  const cvPlan = HL.scopePlan(info.maxBody, 0, 1, 512, 200, 0x3f, 4);
  const fc = await HL.fetchScopeFrame(l, cvPlan, 1, 0, {}, { cvDecim: 4, level: 0, trig: 0 });
  check('cv-only frame: six jacks split 3 + 3 × 336 (168 in one request would be too coarse), ext header, 1 s window', cvPlan.reqs.length === 2 && cvPlan.reqs[0].maskHi === 0x07 && fc.reqs === 2 && fc.n === 336 && fc.cvDecim === 4 && fc.cvDiv === 24 && Object.keys(fc.ch).join() === '4,5,6,7,8,9' && fc.bytes === 2 * (14 + 3 * 336));
  check('cv-only: J4 carries the slow LFO (a fifth of its period in 672 ms), J5 a 0/5 V gate, J3 (no clock patched) rests near 0', Math.max(...fc.ch[5]) - Math.min(...fc.ch[5]) >= 40 && fc.ch[6].every(v => v === 127 || v === 0) && fc.ch[6].includes(127) && fc.ch[6].includes(0) && fc.ch[4].every(v => Math.abs(v) <= 1));
  { const lf = await HL.fetchScopeFrame(l, HL.scopePlan(info.maxBody, 0, 1, 512, 200, 0x02, 32), 1, 0, {}, { cvDecim: 32, level: 0, trig: 0 });
    check('J4 over 8 s (×32): the full ±5 V swing of the LFO', lf.n === 511 && Math.min(...lf.ch[5]) <= -125 && Math.max(...lf.ch[5]) >= 125); }
  check('cv-only: pos is on a block (the cv clock)', fc.pos % 24 === 0);
  const mixed = await HL.fetchScopeFrame(l, HL.scopePlan(info.maxBody, 0x01, 4, 512, 200, 0x08, 1), 4, HL.SCOPE.trig, {}, { cvDecim: 1, level: 0, trig: 0 });
  check('mixed frame triggered on out L (auto): aligned, not detached, J6 along', mixed.triggered && !mixed.detached && mixed.trigSrc === 0 && mixed.ch[0] && mixed.ch[7] && mixed.n === 505);
  const gate = await HL.fetchScopeFrame(l, HL.scopePlan(info.maxBody, 0x01, 16, 512, 200, 0x38, 1), 16, HL.SCOPE.trig, {}, { cvDecim: 1, level: 25, trig: HL.SCOPE.trigSet | 7 });
  check('gates preset: triggered on J6 at 1 V, the gate high from the first cv sample, the kick under it', gate.triggered && gate.trigSrc === 7 && !gate.detached && gate.ch[7][0] === 127 && gate.ch[7][11] === 127 && gate.ch[7][12] < 25 && gate.pos % 6000 === 0 && gate.ch[0].slice(0, 20).some(v => v !== 0));
  const lfo = await HL.fetchScopeFrame(l, HL.scopePlan(info.maxBody, 0x01, 4, 512, 200, 0x02, 32), 4, 0, {}, { cvDecim: 32, level: 0, trig: 0 });
  check('an 8 s cv window with audio shown: the audio tier detaches to its newest window', lfo.detached && lfo.ch[0] && lfo.ch[5] && lfo.n * 32 <= HL.SCOPE.cvUsable);
  const badE = async body => { try { await l.request(HL.CMD.scope, body); return null; } catch (e) { return e.message; } };
  check('ext BAD_ARGS: maskHi bit 6', /BAD_ARGS/.test(await badE(HL.scopeReq(0, 4, 100, 0, null, { maskHi: 0x40, cvDecim: 1 }))));
  check('ext BAD_ARGS: cvDecim 0 / 65', /BAD_ARGS/.test(await badE(Uint8Array.of(0, 4, 100, 0, HL.SCOPE.ext, 0, 0, 0, 0, 1, 0, 0, 0))) && /BAD_ARGS/.test(await badE(HL.scopeReq(0, 4, 100, 0, null, { maskHi: 1, cvDecim: 65 }))));
  check('ext BAD_ARGS: trigSrc 10, trigSrc without the set bit', /BAD_ARGS/.test(await badE(HL.scopeReq(1, 4, 100, 0, null, { maskHi: 1, cvDecim: 1, trig: HL.SCOPE.trigSet | 10 }))) && /BAD_ARGS/.test(await badE(HL.scopeReq(1, 4, 100, 0, null, { maskHi: 1, cvDecim: 1, trig: 3 }))));
  check('ext BAD_ARGS: no channel at all', /BAD_ARGS/.test(await badE(HL.scopeReq(0, 4, 100, 0, null, { maskHi: 0, cvDecim: 1 }))));
  check('ext BAD_ARGS: a short ext body', /BAD_ARGS/.test(await badE(Uint8Array.of(1, 4, 100, 0, HL.SCOPE.ext, 0, 0, 0, 0, 1, 1, 0))));
  { const ten = await HL.fetchScopeFrame(l, HL.scopePlan(info.maxBody, 0x0f, 4, 512, 200, 0x3f, 4), 4, HL.SCOPE.trig, {}, { cvDecim: 4, level: 0, trig: 0 });
    check('all ten channels: two requests pinned to one pos, 202 each', ten.reqs === 2 && ten.n === 202 && Object.keys(ten.ch).length === 10 && ten.triggered); }
  const one = HL.parseScope2(await l.request(HL.CMD.scope, HL.scopeReq(0x01, 1, 9999)));
  check('scope2 clamps n to 512 for one channel', one.n === 512);
  /* silent inputs: the traces are flat, the outputs still play */
  const dq = new DEMO.FakeDevice({ latencyMs: 1, inputs: false }); dq.t0 -= 2000;
  const lq = new HL.Link(new DEMO.FakePort(dq)); await lq.start();
  const fq = await HL.fetchScopeFrame(lq, HL.scopePlan(1024, 0x0f, 4), 4, HL.SCOPE.trig);
  check('noin: inputs flat, outputs alive', fq.ch[2].every(v => v === 0) && fq.ch[3].every(v => v === 0) && fq.ch[0].some(v => v !== 0));
  /* muted: the outputs fall silent, no trigger on out L, the inputs carry on */
  d.muted = true; const fm = await HL.fetchScopeFrame(l, plan, 4, HL.SCOPE.trig);
  check('muted: outputs flat and untriggered, inputs still there', fm.ch[0].every(v => v === 0) && !fm.triggered && fm.ch[2].some(v => v !== 0));
  d.muted = false;
  /* and the Play tab's page-side path: its request coalesces on its own key beside the Scope tab's */
  d.log.length = 0; await Promise.all([l.request(HL.CMD.scope, undefined, { key: 'scope' }), l.request(HL.CMD.scope, undefined, { key: 'scope' }), l.request(HL.CMD.scope, HL.scopeReq(0x0f, 4, 253), { key: 'scope15' })]);
  check('the two scope requests keep their own keys', d.log.filter(t => t === HL.CMD.scope).length === 2);
  await lq.close(); await l.close();
}

/* ── 7. EDIT: the state's bytes and the table body, then every op over the fake module — OK / BAD_ARGS / BAD_STATE,
 *       tableGen, the table's flags, tombstones, keys, FROM_ONSETS, the audition voice, TAP_MARK ── */
{
  const e1 = HL.parseState(stateBody()); check('state: tableGen, undoDepth, selSlice −1, srcPos, redoDepth', e1.tableGen === 77 && e1.undoDepth === 3 && e1.selSlice === -1 && e1.srcPos === 123456 && e1.redoDepth === 2);
  { const b = stateBody(); b[257] = 9; check('selSlice 9', HL.parseState(b).selSlice === 9); }
  check('ACT: the journal ops 30 / 31', HL.ACT.undoEdit === 30 && HL.ACT.redoEdit === 31);
  { const t = new Uint8Array(3 + 2 * HL.TABLE_ENTRY); t[0] = 0; t[1] = 32; t[2] = 34; const dv = new DataView(t.buffer);
    dv.setUint32(3, 100, true); dv.setUint32(7, 6000, true); t[11] = 0xff; t[12] = 0; t[13] = 240; t[14] = 1; t[15] = 60; t[16] = HL.EDIT.orange;
    dv.setUint32(17, 192000, true); dv.setUint32(21, 0, true); t[25] = 0xff; t[26] = 2; t[27] = 255; t[28] = 0; t[29] = 210; t[30] = HL.EDIT.tomb;
    const tb = HL.parseTableChunk(t);
    check('table chunk: start, len, key, class, salience, hit, density, flags', tb.nSlices === 32 && tb.nTable === 34 && tb.entries.length === 2 && tb.entries[0].flags === HL.EDIT.orange && tb.entries[0].start === 100 && tb.entries[0].len === 6000 && tb.entries[0].key === -1 && tb.entries[0].hit === true && tb.entries[0].sal === 240
      && tb.entries[1].flags === HL.EDIT.tomb && tb.entries[1].len === 0 && tb.entries[1].cls === 2 && tb.entries[1].hit === false && tb.entries[1].dens === 210); }
  check('ACT: the edit ops 16..29', HL.ACT.setBoundary === 16 && HL.ACT.fromOnsets === 26 && HL.ACT.audition === 27 && HL.ACT.release === 28 && HL.ACT.tapMark === 29 && HL.EDIT.orange === 1 && HL.EDIT.tomb === 2);

  const SL = DEMO.SLICE_LEN;
  const d = new DEMO.FakeDevice({ kit: true, nSlices: 32, latencyMs: 1 });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l); const desc = await HL.getDescriptor(l, info);
  const state = async () => HL.parseState(await l.request(HL.CMD.state));
  const table = () => HL.fetchTable(l);
  const s0 = await state();
  check('tail: tableGen ≥ 1, undoDepth 0, redoDepth 0, selSlice −1, srcPos inside the material', s0.tableGen >= 1 && s0.undoDepth === 0 && s0.redoDepth === 0 && s0.selSlice === -1 && s0.srcPos >= 0 && s0.srcPos < s0.bufLen && s0.nTable === 35);
  const t0 = await table();
  check('the table: 32 grid + 3 recordings, every flag white', t0.entries.length === 35 && t0.entries.every(e => e.flags === 0) && t0.entries[32].key === 3);
  const gen0 = s0.tableGen;
  /* SET_BOUNDARY: ok with the gen, refetch shows the linked move and the orange flag; the state's tableGen follows */
  let r = await HL.editBoundary(l, 5, 5 * SL - 1000, gen0);
  check('SET_BOUNDARY ok: the reply carries the new tableGen, no index', r.ok && r.status === 0 && r.tableGen !== gen0 && r.newIdx === null);
  let t = await table();
  check('refetch: boundary 5 moved, linked, slice 5 orange, slice 4 white', t.entries[5].start === 5 * SL - 1000 && t.entries[4].len === SL - 1000 && t.entries[5].len === SL + 1000 && (t.entries[5].flags & HL.EDIT.orange) && !(t.entries[4].flags & HL.EDIT.orange));
  let s = await state(); check('the state tail carries the reply\'s tableGen', s.tableGen === r.tableGen);
  r = await HL.editBoundary(l, 6, 6 * SL - 1000, gen0);
  check('a stale gen → BAD_STATE, nothing done, tableGen unchanged', r.status === 3 && !r.ok && r.tableGen === null && (await table()).entries[6].start === 6 * SL && (await state()).tableGen === s.tableGen);
  r = await HL.editBoundary(l, 6, 6 * SL - 1000);
  check('gen 0 is unchecked', r.ok && (await table()).entries[6].start === 6 * SL - 1000);
  s = await state();
  const bad = await Promise.all([HL.editBoundary(l, 0, 100), HL.editBoundary(l, 7, 6 * SL - 1000 + 63), HL.editBoundary(l, 7, s.bufLen), HL.editBoundary(l, 32, 100)]);
  check('SET_BOUNDARY BAD_ARGS: i 0, a 63-sample neighbour, pos = bufLen, i = nSlices; tableGen holds', bad.every(x => x.status === 2) && (await state()).tableGen === s.tableGen);
  { let e = null; try { await l.request(HL.CMD.action, Uint8Array.of(16, 5), { urgent: true }); } catch (x) { e = x; } check('a short body → BAD_ARGS', e && /BAD_ARGS/.test(e.message)); }
  /* ADD_ENTRY: a marker over slice 7 on key 5, orange; a keyless grey one over slice 2 */
  r = await HL.editAdd(l, 5, 7 * SL, SL, HL.EDIT.orange, s.tableGen);
  s = await state();
  check('ADD_ENTRY: newIdx 35, key 5 → 35, nTable 36', r.ok && r.newIdx === 35 && s.keySlice[5] === 35 && s.nTable === 36 && s.tableGen === r.tableGen);
  t = await table();
  check('the marker: slice 7\'s span, no owner, orange, its slice\'s class', t.entries[35].start === 7 * SL && t.entries[35].len === SL && t.entries[35].key === -1 && t.entries[35].flags === HL.EDIT.orange && t.entries[35].cls === t.entries[7].cls);
  r = await HL.editAdd(l, null, 2 * SL, SL, 0);
  check('a keyless grey marker: 36, flags 0, no key moved', r.ok && r.newIdx === 36 && (await table()).entries[36].flags === 0 && (await state()).keySlice[2] === 2);
  check('ADD BAD_ARGS: len 479, past freePos', (await HL.editAdd(l, null, 0, 479)).status === 2 && (await HL.editAdd(l, null, s.freePos - 100, 480)).status === 2);
  /* SPLIT / MERGE: keys stay on the left half; merge swallows the adjacent one and leaves a tombstone */
  r = await HL.editSplit(l, 35, 7 * SL + 2400, (await state()).tableGen);
  t = await table(); s = await state();
  check('SPLIT 35 at +2400 → 37; keys stay on 35; both orange', r.ok && r.newIdx === 37 && t.entries[35].len === 2400 && t.entries[37].start === 7 * SL + 2400 && t.entries[37].len === SL - 2400 && (t.entries[37].flags & HL.EDIT.orange) && s.keySlice[5] === 35 && s.nTable === 38);
  check('SPLIT BAD_ARGS: a half under 480, a grid slice, an index past nTable', (await HL.editSplit(l, 37, 7 * SL + 2400 + 100)).status === 2 && (await HL.editSplit(l, 3, 3 * SL + 3000)).status === 2 && (await HL.editSplit(l, 200, 0)).status === 2);
  r = await HL.editMerge(l, 37);
  t = await table(); s = await state();
  check('MERGE 37 swallows 35 (it ends at 37\'s start): the union, key 5 → 37, 35 a tombstone', r.ok && t.entries[37].start === 7 * SL && t.entries[37].len === SL && s.keySlice[5] === 37 && (t.entries[35].flags & HL.EDIT.tomb) && t.entries[35].len === 0 && t.entries[35].key === -1 && s.nTable === 38);
  const gT = s.tableGen;
  check('a tombstone is skipped: SET_ENTRY / MERGE / SET_KEY / SPLIT / TOMBSTONE on it → BAD_ARGS, nothing bumps',
    (await HL.editEntry(l, 35, 0, 4800)).status === 2 && (await HL.editMerge(l, 35)).status === 2 && (await HL.editSetKey(l, 9, 35)).status === 2 && (await HL.editSplit(l, 35, 100)).status === 2 && (await HL.editTombstone(l, 35)).status === 2 && (await state()).tableGen === gT);
  r = await HL.editAdd(l, null, 3 * SL, SL, HL.EDIT.orange);
  check('ADD reuses the tombstone\'s slot 35, orange again', r.ok && r.newIdx === 35 && (await table()).entries[35].flags === HL.EDIT.orange && (await state()).nTable === 38);
  /* SET_ENTRY: anywhere in [0, freePos); refused on the grid, under 480, past freePos */
  r = await HL.editEntry(l, 36, 4 * SL, 2 * SL);
  t = await table();
  check('SET_ENTRY 36 → [4 SL, 6 SL), orange now', r.ok && t.entries[36].start === 4 * SL && t.entries[36].len === 2 * SL && (t.entries[36].flags & HL.EDIT.orange));
  check('SET_ENTRY BAD_ARGS: a grid slice, len 479, past freePos', (await HL.editEntry(l, 3, 0, 4800)).status === 2 && (await HL.editEntry(l, 36, 0, 479)).status === 2 && (await HL.editEntry(l, 36, s.freePos - 100, 4800)).status === 2);
  /* TOMBSTONE with two keys on it: both go back to their span slices */
  check('SET_KEY 6 → 37 (two keys on one entry)', (await HL.editSetKey(l, 6, 37)).ok && (await state()).keySlice[6] === 37);
  r = await HL.editTombstone(l, 37, (await state()).tableGen);
  s = await state(); t = await table();
  check('TOMBSTONE 37: keys 5 and 6 → their span slices, the index stays', r.ok && s.keySlice[5] === Math.floor(5 * 32 / 24) && s.keySlice[6] === Math.floor(6 * 32 / 24) && (t.entries[37].flags & HL.EDIT.tomb) && s.nTable === 38);
  check('TOMBSTONE BAD_ARGS: a grid slice', (await HL.editTombstone(l, 2)).status === 2);
  /* SET_KEY onto the grid; refusals */
  check('SET_KEY 9 → grid slice 2', (await HL.editSetKey(l, 9, 2)).ok && (await state()).keySlice[9] === 2);
  check('SET_KEY BAD_ARGS: entry past nTable, key 24', (await HL.editSetKey(l, 9, 200)).status === 2 && (await HL.editSetKey(l, 24, 2)).status === 2);
  /* SWAP_KEYS: the recording on key 3 goes to key 9 and the owner follows; a == b refused */
  r = await HL.editSwapKeys(l, 3, 9);
  s = await state(); t = await table();
  check('SWAP_KEYS 3 ↔ 9: key 9 plays the recording and owns it, key 3 plays slice 2', r.ok && s.keySlice[9] === 32 && s.keySlice[3] === 2 && t.entries[32].key === 9);
  check('SWAP_KEYS BAD_ARGS: a == b, key 24', (await HL.editSwapKeys(l, 4, 4)).status === 2 && (await HL.editSwapKeys(l, 4, 24)).status === 2);
  /* NUDGE_ALL: grid +10 ms (every boundary, clamped, all orange); markers −10 ms; refusals */
  const before = (await table()).entries.map(e => e.start);
  r = await HL.editNudgeAll(l, 10, HL.EDIT.grid);
  t = await table();
  check('NUDGE_ALL grid +10 ms: every boundary 480 later, all orange, slice 0 fixed', r.ok && t.entries[1].start === before[1] + 480 && t.entries[31].start === before[31] + 480 && t.entries[0].start === 0 && t.entries.slice(1, 32).every(e => e.flags & HL.EDIT.orange) && t.entries.slice(0, 32).every((e, i, a) => i === 0 || e.start === a[i - 1].start + a[i - 1].len));
  r = await HL.editNudgeAll(l, -10, HL.EDIT.markers);
  t = await table();
  check('NUDGE_ALL markers −10 ms: live markers 480 earlier, the tombstone untouched', r.ok && t.entries[35].start === 3 * SL - 480 && t.entries[36].start === 4 * SL - 480 && t.entries[37].len === 0 && t.entries[32].start === before[32] - 480);
  check('NUDGE_ALL BAD_ARGS: 0 ms, which 2', (await HL.editNudgeAll(l, 0, 0)).status === 2 && (await HL.editNudgeAll(l, 5, 2)).status === 2);
  /* PROMOTE: a marker's start becomes a grid boundary */
  await HL.editEntry(l, 36, 5 * SL + 300, SL);
  r = await HL.editPromote(l, 36, 5);
  check('PROMOTE 36 → boundary 5: the grid opens slice 5 at the marker', r.ok && (await table()).entries[5].start === 5 * SL + 300);
  check('PROMOTE BAD_ARGS: a grid slice, a tombstone, a boundary out of reach', (await HL.editPromote(l, 2, 5)).status === 2 && (await HL.editPromote(l, 37, 5)).status === 2 && (await HL.editPromote(l, 36, 9)).status === 2);
  /* FROM_ONSETS: with the grid nudged back so every onset sits in its own slice, thresh 210 keeps the kicks
   * (salience 240) — one grey marker per kick, to the next kick; a grey marker inside the break is replaced, the
   * orange ones (35, and 36 since SET_ENTRY touched it) and the recordings stay */
  await HL.editNudgeAll(l, -10, HL.EDIT.grid);
  r = await HL.editAdd(l, null, 9 * SL, SL, 0); const grey0 = r.newIdx;
  s = await state();
  r = await HL.editFromOnsets(l, 210, 35, s.tableGen);
  t = await table(); s = await state();
  const kicks = d.hits.filter(p => d.onsetStrength(p) >= 210);
  const greys = t.entries.map((e, i) => [e, i]).filter(([e, i]) => i >= 32 && !(e.flags & (HL.EDIT.orange | HL.EDIT.tomb)) && e.key < 0);
  check('FROM_ONSETS(210, 35): eight kick markers, grey, each to the next kick, the last to the break\'s end', r.ok && kicks.length === 8 && r.count === 8 && greys.length === 8 && greys.every(([e], j) => e.start === kicks[j] && (j < 7 ? e.len === greys[j + 1][0].start - e.start : e.start + e.len === s.bufLen)));
  check('…the grey marker over 9 SL was replaced (its slot reused by a kick marker), the orange 35 / 36 and the recordings kept',
    !t.entries.some((e, i) => i >= 32 && e.start === 9 * SL && e.len === SL) && t.entries[grey0].start === kicks[greys.findIndex(([, i]) => i === grey0)] && (t.entries[35].flags & HL.EDIT.orange) && (t.entries[36].flags & HL.EDIT.orange) && t.entries[32].len > 0 && t.entries[33].key === 7);
  r = await HL.editFromOnsets(l, 100, 35);
  t = await table();
  check('FROM_ONSETS(100, 35): every onset (28 hits + 8 ghosts = 36), the previous greys replaced', r.ok && r.count === 36 && t.entries.filter((e, i) => i >= 32 && !(e.flags & (HL.EDIT.orange | HL.EDIT.tomb)) && e.key < 0).length === 36);
  r = await HL.editFromOnsets(l, 100, 200);
  check('FROM_ONSETS(100, 200 ms): the min gap thins them to one per 200 ms (16)', r.ok && r.count === 16);
  check('FROM_ONSETS with a stale gen → BAD_STATE', (await HL.editFromOnsets(l, 100, 35, 7)).status === 3);
  /* the audition voice: 27 posts at once outside the window; inside it the request is held and the last one wins */
  d.auditionAt = -1e9;
  r = await HL.audition(l, 7 * SL, 250);
  check('AUDITION: ok, the region posted (250 ms = 12 000 samples), no gen byte', r.ok && r.tableGen === null && d.audition && d.audition.start === 7 * SL && d.audition.len === 12000 && d.auditionPosted === 1);
  await HL.audition(l, 7 * SL + 100, 250); await HL.audition(l, 7 * SL + 200, 250, HL.EDIT.fromEnd);
  check('two more inside 100 ms: held, the later replacing the earlier; nothing posted', d.auditionPosted === 1 && d.auditionHeld && d.auditionHeld.start === 7 * SL + 200 && d.auditionHeld.flags === HL.EDIT.fromEnd);
  d.auditionAt -= 200; await state();
  check('the window ends: the held one goes out on the next poll', d.auditionPosted === 2 && d.audition.start === 7 * SL + 200 && d.audition.flags === HL.EDIT.fromEnd && !d.auditionHeld);
  d.auditionAt = -1e9; await HL.audition(l, 2 * SL, 100, HL.EDIT.loop); await HL.audition(l, 2 * SL, 100);
  r = await HL.release(l);
  check('RELEASE: the voice and the held request go', r.ok && d.audition === null && d.auditionHeld === null);
  check('AUDITION BAD_ARGS: flags 4, start past freePos, lenMs 0 (under 64 samples)', (await HL.audition(l, 0, 100, 4)).status === 2 && (await HL.audition(l, s.freePos + 1, 100)).status === 2 && (await HL.editOp(l, HL.ACT.audition, [0, 0, 0, 0, 0, 0, 0])).status === 2);
  /* TAP_MARK: the fake's position snapshot pinned — allowance, onset snap, the length to the next onset, orange. The
   * onsets above left a marker on every kick; v0.13 would select those instead of stacking, so clear them first. */
  r = await HL.clearMarkers(l); t = await table();
  check('CLEAR_MARKERS: every live marker (grey and orange) tombstoned, the count answered, recordings kept', r.ok && r.count >= 16 && !t.entries.some((e, i) => i >= 32 && e.len > 0 && e.key < 0) && t.entries.filter((e, i) => i >= 32 && e.len > 0 && e.key >= 0).length === 3);
  d.srcPos = () => 8 * SL + 1000;
  r = await HL.tapMark(l, 10, 20, (await state()).tableGen);
  t = await table();
  check('TAP_MARK(10, 20): 8 SL + 1000 − 960 → 40 from the kick at 8 SL → snapped; to the next onset (9 SL); orange; newIdx', r.ok && r.newIdx >= 32 && t.entries[r.newIdx].start === 8 * SL && t.entries[r.newIdx].len === SL && t.entries[r.newIdx].flags === HL.EDIT.orange);
  d.srcPos = () => 8 * SL + 3000;
  r = await HL.tapMark(l, 10, 0); t = await table();
  check('TAP_MARK(10, 0) at 8 SL + 3000: no onset within ±480 → raw, to the next onset', r.ok && t.entries[r.newIdx].start === 8 * SL + 3000 && t.entries[r.newIdx].len === SL - 3000);
  d.srcPos = () => 4 * SL + 100;
  r = await HL.tapMark(l, 10, 0); t = await table();
  check('TAP_MARK at 4 SL + 100: snaps to the kick; slice 5 is silent, so the next onset is 6 SL = the 250 ms cap', r.ok && t.entries[r.newIdx].start === 4 * SL && t.entries[r.newIdx].len === 12000);
  check('TAP_MARK BAD_ARGS: a short body', (await HL.editOp(l, HL.ACT.tapMark, [10])).status === 2);
  /* v0.13: a tap within 10 ms of a live marker selects it — no new entry, no gen bump, flags b0, the panel selection */
  { const m8 = t.entries.findIndex((e, i) => i >= 32 && e.start === 8 * SL && e.len > 0 && e.key < 0), g0 = (await state()).tableGen, n0 = t.nTable;
    d.srcPos = () => 8 * SL + 1000 + 200; r = await HL.tapMark(l, 10, 20); s = await state();
    check('TAP_MARK v0.13: a tap on the kick again reuses marker ' + m8 + ' (flags b0), nothing added, the gen unchanged, selSlice says it', r.ok && r.reused && r.flags === 1 && r.newIdx === m8 && s.tableGen === g0 && s.nTable === n0 && s.selSlice === m8);
    d.srcPos = DEMO.FakeDevice.prototype.srcPos; d.muted = true; d.voice = null; s = await state(); r = await HL.tapMark(l, 10, 20);
    check('TAP_MARK v0.13: nothing sounds (stopped) → srcValid clear in the tail, BAD_STATE, and srcPos holds still', s.srcValid === false && r.status === 3 && (await state()).srcPos === s.srcPos);
    d.muted = false; }
  /* BAD_STATE during a take (the audition is not gated), during a set op */
  await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.recArm, 2), { urgent: true });
  s = await state();
  check('a take in flight: every table op is BAD_STATE, the audition still plays', s.recState === 1 && (await HL.editTombstone(l, 35)).status === 3 && (await HL.editAdd(l, null, 0, 4800)).status === 3 && (await HL.editBoundary(l, 4, 4 * SL)).status === 3 && (await HL.tapMark(l, 10, 20)).status === 3 && (await HL.audition(l, 0, 100)).ok);
  d.rec = { state: 0, key: -1, t: 0, pos: 0 };
  await HL.saveSet(l, 'edited');
  check('a set op running: BAD_STATE', (await state()).setState === 1 && (await HL.editTombstone(l, 35)).status === 3);
  d.set.t -= 2000; await state();
  check('…and accepted again after it', (await state()).setState === 0 && (await HL.editSetKey(l, 10, 35)).ok);
  /* the table fills: Full → BAD_STATE; a tombstone frees a slot; tableGen bumped only on success */
  s = await state(); let adds = 0, full = null;
  for (let i = 0; i < 80 && !full; i++) { const x = await HL.editAdd(l, null, 0, 4800, 0); if (x.ok) adds++; else full = x; }
  s = await state();
  check('ADD until the table is full: 96 entries (tombstones reused first), then BAD_STATE', s.nTable === DEMO.MAX_TABLE && full && full.status === 3 && adds > 0 && (await table()).entries.every(e => !(e.flags & HL.EDIT.tomb)));
  const gFull = s.tableGen;
  check('a full table: the refusal does not bump tableGen', (await HL.editAdd(l, null, 0, 4800)).status === 3 && (await state()).tableGen === gFull);
  check('TOMBSTONE frees a slot: the next ADD takes it', (await HL.editTombstone(l, 40)).ok && (await HL.editAdd(l, null, 0, 4800)).newIdx === 40 && (await state()).nTable === DEMO.MAX_TABLE);
  await l.close();

  /* ── EDIT-UNDO (docs/packages/slice-edit-undo.md): ACTION 30 / 31 against the fake's journal — every op undone restores
   * the 14-byte table and the key map byte for byte, redo re-applies, a new op clears the redo half, the ring holds 64,
   * BAD_ARGS when empty, BAD_STATE during a take, a take ends the history. ── */
  {
    const u = new DEMO.FakeDevice({ kit: true, nSlices: 32, latencyMs: 1 });
    const ul = new HL.Link(new DEMO.FakePort(u)); await ul.start(); await HL.hello(ul);
    const ustate = async () => HL.parseState(await ul.request(HL.CMD.state));
    /* the table plus the key map, as one string */
    const snap = async () => { const t = await HL.fetchTable(ul); const st = await ustate(); return JSON.stringify([t.nSlices, t.nTable, t.entries, Array.from(st.keySlice)]); };
    let st = await ustate();
    check('undo: both depths 0; UNDO / REDO on an empty journal → BAD_ARGS, gen untouched', st.undoDepth === 0 && st.redoDepth === 0 && (await HL.editUndo(ul)).status === 2 && (await HL.editRedo(ul)).status === 2 && (await ustate()).tableGen === st.tableGen);
    let steps = 0;
    const roundTrip = async (name, fn) => {
      const before = await snap(); const g0 = (await ustate()).tableGen;
      const r = await fn(); if (!r.ok) { check(name + ': the op lands', false, 'status ' + r.status); return; }
      steps++;
      const after = await snap(); const s1 = await ustate();
      const ru = await HL.editUndo(ul); const back = await snap(); const s2 = await ustate();
      check(name + ': undo restores the table and keys byte for byte; gen moves; depths', ru.ok && ru.tableGen === s2.tableGen && back === before && s2.tableGen !== s1.tableGen && s2.tableGen !== g0 && s2.undoDepth === steps - 1 && s2.redoDepth === 1);
      const rr = await HL.editRedo(ul); const again = await snap(); const s3 = await ustate();
      check(name + ': redo re-applies it; depths', rr.ok && again === after && s3.undoDepth === steps && s3.redoDepth === 0);
    };
    await roundTrip('SET_BOUNDARY(5)', () => HL.editBoundary(ul, 5, 5 * SL - 1000));
    await roundTrip('ADD(key 5, orange)', () => HL.editAdd(ul, 5, 7 * SL, 2400));
    await roundTrip('SET_ENTRY(35)', () => HL.editEntry(ul, 35, 7 * SL + 100, 4000));
    await roundTrip('SPLIT(35)', () => HL.editSplit(ul, 35, 7 * SL + 100 + 2000));
    await roundTrip('SET_KEY(6, 36)', () => HL.editSetKey(ul, 6, 36));
    await roundTrip('SET_KEY(7, 2) — a grid slice', () => HL.editSetKey(ul, 7, 2));
    await roundTrip('SWAP_KEYS(3, 9) — the recording\'s owner trades', () => HL.editSwapKeys(ul, 3, 9));
    await roundTrip('MERGE(36) swallows 35', () => HL.editMerge(ul, 36));
    await roundTrip('PROMOTE(36, 8)', () => HL.editPromote(ul, 36, 8));
    await roundTrip('NUDGE_ALL(+5 ms, grid)', () => HL.editNudgeAll(ul, 5, HL.EDIT.grid));
    await roundTrip('NUDGE_ALL(−3 ms, markers)', () => HL.editNudgeAll(ul, -3, HL.EDIT.markers));
    await roundTrip('FROM_ONSETS(210, 35) — one step', () => HL.editFromOnsets(ul, 210, 35));
    u.srcPos = () => 8 * SL + 1000;
    await roundTrip('TAP_MARK', () => HL.tapMark(ul, 10, 40));
    await roundTrip('TOMBSTONE(36) with key 6 on it', () => HL.editTombstone(ul, 36));
    await roundTrip('TOMBSTONE(32) — the recording', () => HL.editTombstone(ul, 32));
    st = await ustate();
    check('undo depth counts every step', st.undoDepth === steps && st.redoDepth === 0);
    /* a stale gen on UNDO: BAD_STATE, nothing moved; gen 0 unchecked */
    { const before = await snap(); const r = await HL.editOp(ul, HL.ACT.undoEdit, [], (st.tableGen % 255) + 1);
      check('UNDO with a stale gen: BAD_STATE, nothing done', r.status === 3 && (await snap()) === before && (await ustate()).undoDepth === steps); }
    /* two undos, then a new op: the redo half is gone */
    { await HL.editUndo(ul); await HL.editUndo(ul); st = await ustate();
      check('two undone: redo depth 2', st.redoDepth === 2 && st.undoDepth === steps - 2);
      const r = await HL.editSetKey(ul, 0, 1); st = await ustate();
      check('a new op clears the redo half; REDO → BAD_ARGS', r.ok && st.redoDepth === 0 && st.undoDepth === steps - 1 && (await HL.editRedo(ul)).status === 2); }
    /* undo-all restores the fresh kit; redo-all comes back */
    { const end = await snap(); let n = 0; while ((await HL.editUndo(ul)).ok) n++;
      const st0 = await ustate();
      const fresh = new DEMO.FakeDevice({ kit: true, nSlices: 32, latencyMs: 1 }); const fl = new HL.Link(new DEMO.FakePort(fresh)); await fl.start(); await HL.hello(fl);
      const t0 = await HL.fetchTable(fl, { edit: true }); const f0 = HL.parseState(await fl.request(HL.CMD.state)); await fl.close();
      check('undo-all: back to the fresh kit, byte for byte', n === steps - 1 && st0.undoDepth === 0 && st0.redoDepth === steps - 1 && (await snap()) === JSON.stringify([t0.nSlices, t0.nTable, t0.entries, Array.from(f0.keySlice)]));
      let m = 0; while ((await HL.editRedo(ul)).ok) m++;
      check('redo-all: back to the end', m === n && (await snap()) === end && (await ustate()).redoDepth === 0); }
    /* the ring: 70 boundary moves keep 64 */
    { for (let k = 0; k < 70; k++) await HL.editBoundary(ul, 4, 4 * SL + 10 * (k + 1));
      st = await ustate(); check('the depth saturates at 64 (the ring)', st.undoDepth === 64 && st.redoDepth === 0);
      let n = 0; while ((await HL.editUndo(ul)).ok) n++;
      check('64 undos, the 65th BAD_ARGS; the table is where op 6 left it', n === 64 && (await HL.fetchTable(ul, { edit: true })).entries[4].start === 4 * SL + 60 && (await ustate()).redoDepth === 64); }
    /* BAD_STATE during a take; a take ends the history */
    { await HL.editBoundary(ul, 5, 5 * SL + 500);
      u.rec = { state: 1, key: 2, t: Date.now(), pos: u.freePos };
      check('UNDO / REDO during a take: BAD_STATE', (await HL.editUndo(ul)).status === 3 && (await HL.editRedo(ul)).status === 3);
      u.rec = { state: 0, key: -1, t: 0, pos: 0 };
      u.recordKey(9, 0.2, 1); st = await ustate();
      check('a take past the journal ends it: both depths 0, UNDO BAD_ARGS', st.undoDepth === 0 && st.redoDepth === 0 && (await HL.editUndo(ul)).status === 2);
      check('…the next op restarts it at 1', (await HL.editSetKey(ul, 0, 2)).ok && (await ustate()).undoDepth === 1); }
    await ul.close();
  }
}

/* ── 7b. KEYS: the state's bytes from 263, ACTION 32..36 and TAP_MARK over the fake module — the transport (SET_MUTE),
 *        the pads (PLAY_KEY), markers → keys, clear markers, the autoslice count (nested draws, re-roll, undo), Pads'
 *        keys following the markers, Play's loop, the jack map following the grammar ── */
{
  const k1 = HL.parseState(stateBody());
  check('state: flags2 (srcValid, keys follow), load avg / max ‰, J4 −250 mV → key 0, J5 255 → −1 (Brain map), no key flash, keySeq, autoK, nMarkers',
    k1.srcValid === true && k1.keysFollow === true && k1.loadAvg === 412 && k1.loadMax === 905 && k1.j4mV === -250 && k1.j4Key === 0 && k1.j5Style === -1 && k1.keyFlash === -1 && k1.keySeq === 17 && k1.autoK === 12 && k1.nMarkers === 5 && k1.redoDepth === 2);
  { const b = stateBody(); b[263] = 0; b[272] = 9; b[271] = 4; const s = HL.parseState(b); check('state: flags clear, key 9 flashed, style 4', s.srcValid === false && s.keysFollow === false && s.keyFlash === 9 && s.j5Style === 4); }
  check('ACT: the KEYS ops 32..36', HL.ACT.playKey === 32 && HL.ACT.keysFromMarkers === 33 && HL.ACT.clearMarkers === 34 && HL.ACT.setMute === 35 && HL.ACT.autoslice === 36 && HL.KEYS.styles.length === 9);

  const SL = DEMO.SLICE_LEN;
  const d = new DEMO.FakeDevice({ kit: true, nSlices: 32, latencyMs: 1 });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l); const desc = await HL.getDescriptor(l, info);
  const state = async () => HL.parseState(await l.request(HL.CMD.state));
  const table = () => HL.fetchTable(l);
  const nap = ms => new Promise(res => setTimeout(res, ms));
  let s = await state(), r, t;
  check('hello: the beta version; no markers, something sounds, the load meter', info.fw === DEMO.FW && info.fw === '0.13.0-beta-demo' && s.nMarkers === 0 && s.srcValid === true && s.loadAvg > 0 && s.loadMax >= s.loadAvg);
  /* the transport */
  r = await HL.setMute(l, true); s = await state();
  check('SET_MUTE 1: stopped (flags b0), nothing sounds → srcValid clear', r.ok && s.muted === true && s.srcValid === false);
  const held = s.srcPos; await nap(60); s = await state();
  check('…and srcPos holds still while nothing sounds', s.srcPos === held);
  r = await HL.setMute(l, true); s = await state(); check('SET_MUTE is idempotent (not a toggle)', r.ok && s.muted === true);
  check('SET_MUTE 2 → BAD_ARGS', (await HL.editOp(l, HL.ACT.setMute, [2])).status === 2);
  /* the pads: a key sounds while stopped, so a tap is taken */
  const seq0 = s.keySeq; r = await HL.playKey(l, 3, true); s = await state();
  check('PLAY_KEY(3, down) while stopped: the flash names key 3, keySeq bumps, callFlash is its entry (the recording, 32), srcValid while it sounds', r.ok && s.keyFlash === 3 && s.keySeq === ((seq0 + 1) & 255) && s.callFlash === 32 && s.srcValid === true && s.muted === true);
  r = await HL.tapMark(l, 10, 0); check('TAP_MARK is taken while a pad sounds (stopped)', r.ok && r.newIdx >= 35 && r.reused === false && r.flags === 0);
  check('PLAY_KEY BAD_ARGS: key 24, a short body', (await HL.playKey(l, 24, true)).status === 2 && (await HL.editOp(l, HL.ACT.playKey, [3])).status === 2);
  await HL.editUndo(l);
  /* Key Plays = Held: the voice lasts until the up */
  d.options[DEMO.O['key.plays']] = 1; await HL.playKey(l, 0, true); await nap(250);
  check('Held: the voice still sounds 250 ms on (slice 0 is 125 ms)', d.voiceOn() === true);
  await HL.playKey(l, 0, false); check('…and the up releases it', d.voiceOn() === false);
  d.options[DEMO.O['key.plays']] = 0; await HL.setMute(l, false);
  /* the autoslice count: a seeded weighted draw, nested in k, a re-roll draws again, 0 clears the greys, orange stays */
  const greys = tb => tb.entries.map((e, i) => [e, i]).filter(([e, i]) => i >= tb.nSlices && e.len > 0 && e.key < 0 && !(e.flags & HL.EDIT.orange)).map(([e]) => e.start).sort((a, b) => a - b);
  r = await HL.autoslice(l, 8); t = await table(); s = await state(); const g8 = greys(t);
  check('AUTOSLICE(8): eight grey markers inside the break, the count answered, autoK 8, nMarkers 8', r.ok && r.count === 8 && g8.length === 8 && g8.every(p => p > 0 && p < s.bufLen) && s.autoK === 8 && s.nMarkers === 8);
  r = await HL.autoslice(l, 12); t = await table(); const g12 = greys(t);
  check('AUTOSLICE(12), same seed: the 8 are a subset of the 12 (turning up adds onsets)', r.ok && g12.length === 12 && g8.every(p => g12.includes(p)));
  r = await HL.autoslice(l, 5); t = await table(); const g5 = greys(t);
  check('AUTOSLICE(5): a subset of the 8 (turning down removes the last drawn)', r.ok && g5.length === 5 && g5.every(p => g8.includes(p)));
  r = await HL.autoslice(l, 8, true); t = await table(); const r8 = greys(t);
  check('AUTOSLICE(8, re-roll): a new seed, a different chop', r.ok && r8.length === 8 && r8.join() !== g8.join());
  const gBefore = r8.join(); r = await HL.editUndo(l); t = await table();
  check('one undo step: the re-roll undone gives the 5 back', r.ok && greys(t).join() === g5.join());
  await HL.editRedo(l); t = await table(); check('…redo gives the re-roll back', greys(t).join() === gBefore);
  check('AUTOSLICE BAD_ARGS: k 65 on the wire (the helper clamps to 64); a stale gen → BAD_STATE', (await HL.editOp(l, HL.ACT.autoslice, [65, 0])).status === 2 && (await HL.autoslice(l, 4, false, ((await state()).tableGen + 7) % 255 + 1)).status === 3);
  /* markers → keys: in time order onto keys 0.., the recording keys (3, 7, 14) skipped */
  s = await state(); r = await HL.keysFromMarkers(l, s.tableGen); s = await state(); t = await table();
  const ms = t.entries.map((e, i) => [e, i]).filter(([e, i]) => i >= 32 && e.len > 0 && e.key < 0).sort((a, b) => a[0].start - b[0].start).map(([, i]) => i);
  const freeKeys = [0, 1, 2, 4, 5, 6, 8, 9];
  check('KEYS_FROM_MARKERS: 8 markers → keys 0 1 2 4 5 6 8 9 in time order; the recordings keep keys 3, 7, 14', r.ok && r.count === 8 && freeKeys.every((k, j) => s.keySlice[k] === ms[j]) && s.keySlice[3] === 32 && s.keySlice[7] === 33 && s.keySlice[14] === 34);
  check('KEYS_FROM_MARKERS: the keys past the last marker are on their span slices', [10, 11, 15, 23].every(k => s.keySlice[k] === d.spanSlice(k)));
  r = await HL.editUndo(l); s = await state(); check('…one undo step puts the keys back on the grid', r.ok && s.keySlice[0] === 0 && s.keySlice[1] === 1);
  await HL.editRedo(l);
  /* re-spanned in place: a key on a grey marker keeps playing a marker after the count moves */
  { const t0 = (await state()).keySlice[0]; r = await HL.autoslice(l, 10); s = await state(); t = await table();
    check('AUTOSLICE re-spans the grey markers in place — key 0 still on a live marker, its index kept', r.ok && s.keySlice[0] === t0 && t.entries[t0].len > 0 && t.entries[t0].key < 0);
    await HL.autoslice(l, 8); }
  /* JOIN (flags b1): a drag's sends after the first fold into one undo step */
  { const u0 = (await state()).undoDepth; await HL.autoslice(l, 9); await HL.autoslice(l, 11, false, 0, true); await HL.autoslice(l, 13, false, 0, true); s = await state(); t = await table();
    check('AUTOSLICE JOIN: three sends of one drag are one undo step (flags b1 on the wire after the first)', s.undoDepth === u0 + 1 && greys(t).length === 13);
    r = await HL.editUndo(l); t = await table(); check('…and one undo takes the whole drag back (to the 8)', r.ok && greys(t).length === 8); }
  /* an orange marker's onset is not drawn again (10 ms) */
  { const o = d.hits.find(p => p > 4 * DEMO.SLICE_LEN && d.onsetStrength(p) >= 200); await HL.editAdd(l, null, o, 2400, HL.EDIT.orange); await HL.autoslice(l, 30); t = await table();
    check('AUTOSLICE skips an onset within 10 ms of an orange marker', !greys(t).some(p => Math.abs(p - o) <= 480));
    await HL.editUndo(l); await HL.editUndo(l); await HL.autoslice(l, 8); }
  /* an orange marker survives the autoslice and is cleared by CLEAR_MARKERS with the rest */
  await HL.editAdd(l, null, 20 * SL, SL, HL.EDIT.orange); r = await HL.autoslice(l, 0); t = await table(); s = await state();
  check('AUTOSLICE(0): the grey markers go, the orange one stays', r.ok && r.count === 0 && greys(t).length === 0 && s.nMarkers === 1 && s.autoK === 0);
  r = await HL.clearMarkers(l); t = await table(); s = await state();
  check('CLEAR_MARKERS: the count, every key off a marker back to its grid slice, the recordings untouched', r.ok && r.count === 1 && s.nMarkers === 0 && Array.from(s.keySlice).every((tt, k) => [3, 7, 14].includes(k) ? tt === { 3: 32, 7: 33, 14: 34 }[k] : tt < 32));
  r = await HL.clearMarkers(l); check('CLEAR_MARKERS with none: OK, count 0, the gen unchanged', r.ok && r.count === 0 && r.tableGen === (await state()).tableGen);
  /* the grammars: the jack map follows (Follow grammar: Play and Pads → Voice), J5 reads a style there, 255 in Brain */
  const gram = g => { d.setGrammar(g, d.grammar); d.options[DEMO.O['grammar.mode']] = g; };
  gram(DEMO.GRAM.grids); s = await state(); check('Grids: the Brain map (175 = 0), J5 is a gate (style −1)', s.jackMap === 0 && s.j5Style === -1 && s.j4Key >= 0 && s.j4Key <= 23);
  { await HL.autoslice(l, 4); await HL.keysFromMarkers(l); for (let k = 0; k < 24; k++) if (![3, 7, 14].includes(k)) await HL.editSetKey(l, k, d.spanSlice(k)); const u0 = (await state()).undoDepth;
    gram(DEMO.GRAM.pads); s = await state();
    check('entering Pads puts the markers on the keys once, as its own undo step', s.undoDepth === u0 + 1 && s.keySlice[0] >= 32 && s.keysFollow === true);
    await HL.editUndo(l); await HL.clearMarkers(l); gram(DEMO.GRAM.grids); }
  gram(DEMO.GRAM.pads); s = await state(); check('Pads: the Voice map, J5 a style 0..8, the pattern silent (nothing sounds)', s.jackMap === 1 && s.j5Style >= 0 && s.j5Style <= 8 && s.srcValid === false);
  d.options[DEMO.O.jacks] = 1; s = await state(); check('Jacks = Brain overrides the grammar', s.jackMap === 0); d.options[DEMO.O.jacks] = 0;
  check('Pads: TAP_MARK with nothing sounding → BAD_STATE', (await HL.tapMark(l, 10, 0)).status === 3);
  r = await HL.autoslice(l, 6); s = await state(); t = await table();
  const pm = t.entries.map((e, i) => [e, i]).filter(([e, i]) => i >= 32 && e.len > 0 && e.key < 0).sort((a, b) => a[0].start - b[0].start).map(([, i]) => i);
  check('Pads: AUTOSLICE(6) puts the markers on keys by itself (keys follow markers, tail 263 b1)', r.ok && s.keysFollow === true && pm.length === 6 && [0, 1, 2, 4, 5, 6].every((k, j) => s.keySlice[k] === pm[j]));
  r = await HL.editUndo(l); s = await state(); check('…and one undo takes the markers and the keys back together', r.ok && s.nMarkers === 0 && s.keySlice[0] === 0 && s.keysFollow === false);
  const ev0 = s.looperEvents; await HL.playKey(l, 5, true); s = await state();
  check('Pads: a pad sounds and writes nothing', s.keyFlash === 5 && s.looperEvents === ev0 && s.srcValid === true);
  gram(DEMO.GRAM.play); s = await state();
  check('Play: an empty loop to start (every step a rest, no events), the Voice map', s.looperEvents === 0 && Array.from(s.src).slice(0, 32).every(x => x === -1) && s.jackMap === 1);
  await HL.playKey(l, 2, true); s = await state(); const wrote = Array.from(s.src).findIndex(x => x === s.keySlice[2]);
  check('Play: a pad writes the loop — one event, its entry on a step', s.looperEvents === 1 && wrote >= 0);
  await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.looperUndo), { urgent: true }); s = await state();
  check('Play: ACTION 12 takes it back (B2\'s undo)', s.looperEvents === 0 && Array.from(s.src).slice(0, 32).every(x => x === -1));
  await HL.playKey(l, 2, true); await HL.playKey(l, 4, true); await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.looperClear), { urgent: true }); s = await state();
  check('Play: ACTION 13 clears the loop', s.looperEvents === 0 && Array.from(s.src).slice(0, 32).every(x => x === -1));
  gram(DEMO.GRAM.classic); await l.close();

}

/* ── 7c. CARD (0x67): the WAV encoder, the name rule, the plan, the state's bytes from 276, then the upload
 *        sequencer, the refusals, the serialiser and the copy over the fake module ── */
{
  /* the encoder, read back byte by byte */
  const L = Float32Array.of(0, 0.5, -0.5, 1, -1, 2, -2, NaN), R = Float32Array.of(0.25, -0.25, 0, 0, 0, 0, 0, 0);
  const w = HL.encodeWav([L, R], 48000), dv = new DataView(w.buffer), tag = at => String.fromCharCode(...w.subarray(at, at + 4));
  check('encodeWav: RIFF / WAVE / fmt 16 PCM / data, the sizes', w.length === 44 + 8 * 2 * 2 && tag(0) === 'RIFF' && dv.getUint32(4, true) === w.length - 8 && tag(8) === 'WAVE' && tag(12) === 'fmt '
    && dv.getUint32(16, true) === 16 && dv.getUint16(20, true) === 1 && dv.getUint16(22, true) === 2 && dv.getUint32(24, true) === 48000 && dv.getUint32(28, true) === 48000 * 4 && dv.getUint16(32, true) === 4 && dv.getUint16(34, true) === 16 && tag(36) === 'data' && dv.getUint32(40, true) === 32);
  const smp = (i, c) => dv.getInt16(44 + (i * 2 + c) * 2, true);
  check('encodeWav: interleaved L R, symmetric scale, clamped, NaN → 0', smp(0, 0) === 0 && smp(0, 1) === 8192 && smp(1, 0) === 16384 && smp(1, 1) === -8192 && smp(2, 0) === Math.round(-0.5 * 32767) && smp(3, 0) === 32767 && smp(4, 0) === -32767 && smp(5, 0) === 32767 && smp(6, 0) === -32767 && smp(7, 0) === 0,
    [0, 1, 2, 3, 4, 5, 6, 7].map(i => smp(i, 0)).join());
  const m = HL.encodeWav([new Float32Array(1000).fill(0.1)], 48000, 600);
  check('encodeWav: mono, and a frame count shorter than the plane', m.length === 44 + 1200 && new DataView(m.buffer).getUint16(22, true) === 1 && new DataView(m.buffer).getUint32(40, true) === 1200);
  /* the name rule: the module's table (tests/card_check.cpp), and the sanitiser always lands inside it */
  const good = ['amen', 'Amen Break 170', 'a', 'think (live) #2', 'x-y_z.v2', '~tilde', '100%', 'a'.repeat(100)];
  const bad = ['', ' lead', 'trail ', 'dot.', '.hidden', 'a/b', 'a\\b', 'c:d', 'star*', 'q?', 'quote"', 'lt<', 'gt>', 'pipe|', 'tab\t', 'del\x7f', 'é', 'a'.repeat(101), null, 7];
  check('breakNameOk: the module\'s rule (the same table as card_check)', good.every(HL.breakNameOk) && !bad.some(HL.breakNameOk), bad.filter(HL.breakNameOk).join('|'));
  const san = { 'Amen Break.wav': 'Amen Break', 'Ämen Brëak: 170?.WAV': 'Amen Break- 170-', '  .hidden.mp3': 'hidden', 'a/b\\c.flac': 'a-b-c', '...': 'break', '': 'break', 'tab\there.aiff': 'tab here', 'こんにちは.wav': '_____' };
  check('breakName: extension off, accents folded, reserved → "-", ends trimmed, "break" when nothing is left', Object.entries(san).every(([a, b]) => HL.breakName(a) === b), Object.keys(san).map(k => HL.breakName(k)).join(' | '));
  let fuzzBad = 0; let seed = 7; const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
  for (let i = 0; i < 2000; i++) { let t = ''; const n = rnd(140); for (let k = 0; k < n; k++) t += String.fromCharCode(rnd(4) ? rnd(128) : rnd(0x3000)); const out = HL.breakName(t); if (!HL.breakNameOk(out) || out.length > 100) fuzzBad++; }
  check('breakName: 2000 random names (ASCII, control, CJK) all pass the module\'s rule', fuzzBad === 0, fuzzBad + ' failed');
  /* the plan: 17 s, two channels, 48 kHz */
  const p1 = HL.uploadPlan(2, 44100, 44100 * 20), p2 = HL.uploadPlan(6, 48000, 48000), p3 = HL.uploadPlan(1, 96000, 96000 * 3);
  check('uploadPlan: 20 s at 44.1 → 17 s at 48 k (trimmed, resampled); 6 channels → 2; 96 k → half the frames', p1.frames === 17 * 48000 && p1.trimmed && p1.resample && Math.abs(p1.sourceSeconds - 20) < 1e-9 && p1.channels === 2 && p1.bytes === 44 + 17 * 48000 * 4
    && p2.channels === 2 && p2.frames === 48000 && !p2.trimmed && !p2.resample && p3.frames === 3 * 48000 && p3.channels === 1, JSON.stringify([p1, p2, p3]));
  check('uploadPlan: the most the module takes fits its buffer (UP_MAX)', HL.uploadPlan(2, 48000, 48000 * 60).bytes <= DEMO.UP_MAX && DEMO.UP_MAX === 17 * 48000 * 4 + 4096);
  check('the chunk fits max_body with its 9-byte header (1024 and 512)', HL.CARD.chunkFor(1024) + 9 <= 1024 && HL.CARD.chunkFor(512) + 9 <= 512 && HL.CARD.chunkFor(1024) >= 900);

  /* the tail, synthetic */
  const b = stateBody(); const dvb = new DataView(b.buffer);
  b[276] = 2; b[277] = 128; b[278] = 8; b[279] = 4; b[280] = 1 | 2 | 4 | 32; dvb.setUint16(281, 12, true); dvb.setUint16(283, 15, true); dvb.setUint16(285, 0, true); b[287] = 255; dvb.setUint16(288, 3, true); dvb.setUint16(290, 9, true); b[292] = 41;
  const sc = HL.parseState(b).card;
  check('state: the card (state, progress, result, seq, flags, counts, lastIdx −1, copied / skipped, nFiles)', sc && sc.state === 2 && HL.CARD.STATE[sc.state] === 'writing' && sc.progress === 128 && HL.CARD.RESULT[sc.result] === 'already on the card' && sc.seq === 4
    && sc.mounted && sc.oldRoot && sc.oldSets && !sc.oldPresets && !sc.hasBreaks && sc.rootWavs === 12 && sc.pending === 15 && sc.breaks === 0 && sc.lastIdx === -1 && sc.copied === 3 && sc.skipped === 9 && sc.nFiles === 41);
  check('a set op\'s state 3 is the card\'s', HL.SET_STATE[3] === 'card');

  /* over the fake: the upload */
  const d = new DEMO.FakeDevice({ latencyMs: 1 });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l), desc = await HL.getDescriptor(l, info);
  const state = async () => HL.parseState(await l.request(HL.CMD.state));
  const settle = async seq0 => { for (let i = 0; i < 200; i++) { const s = await state(); if (s.card.seq !== seq0 && s.card.state === 0) return s; d.cardOp.t -= 200; await new Promise(r => setTimeout(r, 5)); } return null; };
  let s = await state();
  check('card: a mounted card with the four breaks, nothing old', s.card.mounted && s.card.breaks === 4 && s.card.pending === 0 && !s.card.oldRoot && s.card.state === 0 && s.card.lastIdx === -1);
  check('card list: the module\'s list as it is now', JSON.stringify(await HL.cardList(l)) === JSON.stringify(DEMO.FILES));
  const bodies = []; { const h = d.handle.bind(d); d.handle = (t, x) => { if (t === HL.CMD.card) bodies.push(x.length); return h(t, x); }; }
  const planes = [new Float32Array(48000 * 2).map((_, i) => Math.sin(i / 20) * 0.5), new Float32Array(48000 * 2).map((_, i) => Math.cos(i / 20) * 0.5)];
  const wav = HL.encodeWav(planes, 48000), prog = [];
  let polls = 0; const poller = setInterval(() => { l.request(HL.CMD.state, undefined, { key: 'state' }).then(() => polls++, () => {}); }, 20);
  const r = await HL.uploadBreak(l, 'Think Break', wav, { maxBody: info.maxBody, onProgress: (a, t, k) => prog.push([a, t, k]) });
  clearInterval(poller);
  check('upload: every byte acknowledged in order, progress monotonic to the end, a rate', r.bytes === wav.length && prog.length === Math.ceil(wav.length / HL.CARD.chunkFor(info.maxBody)) && prog.every((p, i) => i === 0 || p[0] > prog[i - 1][0]) && prog[prog.length - 1][0] === wav.length && r.kbps > 0);
  check('upload: every request fits max_body', bodies.length > 1 && Math.max(...bodies) <= info.maxBody, Math.max(...bodies) + ' > ' + info.maxBody);
  check('upload: the state polls interleave (the page stays live)', polls >= 3, 'polls ' + polls);
  s = await state(); check('upload done: the module is writing', s.card.state === 2);
  s = await settle(s.card.seq);
  check('…then idle, ok, lastIdx = the new list entry, the file on the card byte for byte', s && s.card.result === 0 && s.card.lastIdx === 5 && d.files[5] === 'Think Break.wav' && s.card.nFiles === 6 && s.card.breaks === 5
    && Buffer.from(d.cardFs.get('Audiothurgist/breaks/Think Break.wav')).equals(Buffer.from(wav)));
  check('…and in the module\'s list at the end (nothing re-sorted)', (await HL.cardList(l)).slice(-1)[0] === 'Think Break.wav' && (await HL.cardList(l))[0] === DEMO.FILES[0]);
  await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.load, s.card.lastIdx), { urgent: true });
  check('"Load it now": ACTION 6 with lastIdx loads it', d.file === 5);
  /* the refusals */
  const err = async f => { try { await f(); return 'ok'; } catch (e) { return e.message; } };
  check('the same name again: EXISTS, nothing sent', await err(() => HL.uploadBreak(l, 'think break', wav)) === 'EXISTS');
  const wav2 = HL.encodeWav([new Float32Array(24000)], 48000), seqA = (await state()).card.seq;
  check('…with overwrite: replaced', await err(() => HL.uploadBreak(l, 'Think Break', wav2, { overwrite: true })) === 'ok' && (await settle(seqA)) && d.cardFs.get('Audiothurgist/breaks/Think Break.wav').length === wav2.length && d.files.length === 6);
  check('a bad name is refused before the wire', await err(() => HL.uploadBreak(l, 'a/b', wav2)) === 'bad name');
  check('a raw bad name on the wire: BAD_ARGS', await err(() => l.request(HL.CMD.card, Uint8Array.of(0, 10, 0, 0, 0, 0, 0x61, 0x2f, 0x62))) === 'BAD_ARGS');
  check('UP_DATA with nothing open: BAD_STATE', await err(() => l.request(HL.CMD.card, Uint8Array.of(1, 10, 0, 0, 0, 0, 0, 0, 0, 1, 2))) === 'BAD_STATE');
  await l.request(HL.CMD.card, Uint8Array.of(0, 100, 0, 0, 0, 0, ...HL.nameBytes('seq')));
  check('out of sequence: BAD_ARGS and the transfer dropped', await err(() => l.request(HL.CMD.card, Uint8Array.of(1, 100, 0, 0, 0, 5, 0, 0, 0, 1))) === 'BAD_ARGS' && (await state()).card.state === 0
    && await err(() => l.request(HL.CMD.card, Uint8Array.of(1, 100, 0, 0, 0, 0, 0, 0, 0, 1))) === 'BAD_STATE');
  check('too large: TOO_LARGE', await err(() => l.request(HL.CMD.card, Uint8Array.of(0, ...[DEMO.UP_MAX + 1].flatMap(v => [v & 255, v >> 8 & 255, v >> 16 & 255, v >>> 24]), 0, ...HL.nameBytes('big')))) === 'TOO_LARGE');
  d.cardFree = 1000; check('a full card: CARD_FULL at the begin', await err(() => HL.uploadBreak(l, 'full', wav2)) === 'CARD_FULL'); d.cardFree = Infinity;
  check('not a WAV: the module drops it (result bad file)', await err(() => HL.uploadBreak(l, 'junk', new Uint8Array(5000).fill(7))) === 'not a WAV the module reads' && HL.CARD.RESULT[(await state()).card.result] === 'bad file' && !d.cardHas('Audiothurgist/breaks/junk.wav'));
  d.set.pending = { op: 14, name: 'x' }; check('BAD_STATE while a set op waits', await err(() => HL.uploadBreak(l, 'busy', wav2)) === 'BAD_STATE' && await err(() => HL.copyOldCard(l)) === 'BAD_STATE'); d.set.pending = null;
  d.rec.state = 2; check('BAD_STATE while a take runs', await err(() => HL.uploadBreak(l, 'busy', wav2)) === 'BAD_STATE'); d.rec.state = 0;
  let cancelAt = 0; check('cancel between chunks: UP_ABORT, nothing written', await err(() => HL.uploadBreak(l, 'gone', wav, { cancel: () => ++cancelAt > 3 })) === 'cancelled' && (await state()).card.state === 0 && !d.cardHas('Audiothurgist/breaks/gone.wav'));
  await l.request(HL.CMD.card, Uint8Array.of(0, 100, 0, 0, 0, 0, ...HL.nameBytes('quiet'))); d.cardUp.last -= 6000;
  { const q = await state(); check('a transfer quiet for 5 s is dropped (result timed out)', q.card.state === 0 && HL.CARD.RESULT[q.card.result] === 'timed out'); }
  /* one card job at a time: two uploads started together interleave on the wire and one is refused; through serial() both land */
  const wa = HL.encodeWav([new Float32Array(9000).fill(0.2)], 48000), wb = HL.encodeWav([new Float32Array(9000).fill(-0.2)], 48000);
  const raw = await Promise.all([err(() => HL.uploadBreak(l, 'raw a', wa, { chunk: 1000 })), err(() => HL.uploadBreak(l, 'raw b', wb, { chunk: 1000 }))]);
  check('two uploads started together without the queue: one of them fails mid-stream (the hazard is real)', raw.filter(x => x !== 'ok').length >= 1, raw.join(' / '));
  for (let i = 0; i < 40 && (await state()).card.state; i++) { d.cardOp.t -= 1000; d.cardUp.last -= 6000; }
  const run = HL.serial(), done = [];
  const job = (name, bytes) => run(async () => { const q0 = (await state()).card.seq; await HL.uploadBreak(l, name, bytes, { chunk: 1000 }); done.push(name); await settle(q0); });
  await Promise.all([job('queued a', wa), job('queued b', wb)]);
  check('…through serial() both land, in order, each whole', done.join() === 'queued a,queued b' && Buffer.from(d.cardFs.get('Audiothurgist/breaks/queued a.wav')).equals(Buffer.from(wa)) && Buffer.from(d.cardFs.get('Audiothurgist/breaks/queued b.wav')).equals(Buffer.from(wb)));
  await l.close();

  /* the old card: nothing listed, the survey, the copy, a second copy */
  const o = new DEMO.FakeDevice({ latencyMs: 1, oldcard: true });
  const lo = new HL.Link(new DEMO.FakePort(o)); await lo.start(); const io = await HL.hello(lo), dsc = await HL.getDescriptor(lo, io);
  const st2 = async () => HL.parseState(await lo.request(HL.CMD.state));
  let so = await st2();
  check('old card: nothing listed (the root is not read), no dropdown, the survey says so', so.card.nFiles === 0 && HL.fieldLabels(dsc, 'sample.idx').length === 0 && so.card.mounted && so.card.oldRoot && so.card.oldSets && so.card.oldPresets && !so.card.hasBreaks && so.card.rootWavs === 4 && so.card.pending === 8, JSON.stringify(so.card));
  const before = new Map(o.cardFs);
  await HL.copyOldCard(lo);
  so = await st2(); check('copy: accepted, copying', so.card.state === 3);
  for (let i = 0; i < 50 && so.card.state; i++) { o.cardOp.t -= 300; so = await st2(); }
  check('copy done: 8 copied, nothing skipped, ok, the list has the breaks and the set', so.card.state === 0 && so.card.result === 0 && so.card.copied === 8 && so.card.skipped === 0 && so.card.pending === 0 && so.card.hasBreaks && so.card.breaks === 4
    && JSON.stringify(await HL.cardList(lo)) === JSON.stringify(DEMO.FILES));
  check('…copied, never moved: every original is still there, byte for byte; the shadow and another module\'s folder untouched', [...before].every(([k, v]) => o.cardFs.get(k) === v) && !o.cardHas('Audiothurgist/breaks/._amen_170.wav') && !o.cardHas('Audiothurgist/breaks/kyk.wav')
    && o.cardHas('Audiothurgist/sets/think_110.wav.slices.json') && o.cardHas('Audiothurgist/presets/0.json'));
  await HL.copyOldCard(lo); so = await st2(); for (let i = 0; i < 50 && so.card.state; i++) { o.cardOp.t -= 300; so = await st2(); }
  check('a second copy: everything skipped', so.card.copied === 0 && so.card.skipped === 8 && so.card.result === 0);
  await lo.close();
  /* no card, and a firmware before the card work */
  const nc = new DEMO.FakeDevice({ latencyMs: 1, nocard: true }), ln = new HL.Link(new DEMO.FakePort(nc)); await ln.start(); await HL.hello(ln);
  const sn = HL.parseState(await ln.request(HL.CMD.state));
  check('no card: flags 0, NO_CARD for an upload and a copy', sn.card.flags === 0 && !sn.card.mounted && await err(() => HL.uploadBreak(ln, 'x', wav2)) === 'NO_CARD' && await err(() => HL.copyOldCard(ln)) === 'NO_CARD');
  await ln.close();
}

/* ── 7d. PADS (0x68): the state's bytes from 293, the plan and the PCM16 encoder, then a drop over the fake
 *        module byte for byte, one undo step, a replaced pad held, every refusal both ways (a card job or a load while a
 *        drop is open), the abort and the 5 s timeout, New empty kit, and a pads-only kit end to end through a set save,
 *        a power-cycle and the load back ── */
{
  /* the tail, synthetic */
  const b = stateBody(); const dvb = new DataView(b.buffer);
  b[293] = 1; b[294] = 64; b[295] = 1; b[296] = 7; b[297] = 5; b[298] = 1 | 2; dvb.setUint32(299, 1234567, true); dvb.setUint32(303, 2148000, true); dvb.setUint32(307, 9600, true); dvb.setUint32(311, 0x800021, true);
  const sp = HL.parseState(b).pads;
  check('state: the pads (state, progress, result, seq, key, flags, free, total, held, the pads\' mask)', sp && HL.PADS.STATE[sp.state] === 'receiving' && sp.progress === 64 && HL.PADS.RESULT[sp.result] === 'no room' && sp.seq === 7 && sp.key === 5
    && sp.unsaved && sp.noBreak && !sp.madeStereo && sp.free === 1234567 && sp.total === 2148000 && sp.held === 9600 && sp.mask === 0x800021, JSON.stringify(sp));
  check('state: pads key 255 → −1', (b[297] = 255, HL.parseState(b).pads.key === -1));
  /* the plan and the encoder */
  const q1 = HL.padPlan(2, 44100, 44100 * 3, null), q2 = HL.padPlan(6, 48000, 48000, 12000), q3 = HL.padPlan(1, 96000, 96000, 300);
  check('padPlan: 3 s at 44.1 → 144000 frames at 48 k, stereo, uncut; 6 channels → 2, cut to the room; under 480 frames: too short', q1.frames === 144000 && q1.channels === 2 && !q1.cut && q1.resample && q1.bytes === 144000 * 4
    && q2.channels === 2 && q2.frames === 12000 && q2.cut && !q2.resample && q3.frames === 300 && q3.tooShort && q3.cut, JSON.stringify([q1, q2, q3]));
  const pc = HL.encodePcm16([Float32Array.of(0, 0.5, -1, 2, NaN), Float32Array.of(0.25, -0.25, 1, -2, 0)]), pdv = new DataView(pc.buffer);
  check('encodePcm16: raw interleaved L R, no header, the encodeWav scale, clamped, NaN → 0', pc.length === 20 && pdv.getInt16(0, true) === 0 && pdv.getInt16(2, true) === 8192 && pdv.getInt16(4, true) === 16384 && pdv.getInt16(6, true) === -8192
    && pdv.getInt16(8, true) === -32767 && pdv.getInt16(10, true) === 32767 && pdv.getInt16(12, true) === 32767 && pdv.getInt16(14, true) === -32767 && pdv.getInt16(16, true) === 0);
  check('encodePcm16 matches encodeWav\'s data chunk sample for sample', (() => { const L = new Float32Array(500).map((_, i) => Math.sin(i / 7)), R = L.map(v => -v / 2); const a = HL.encodePcm16([L, R]), w = HL.encodeWav([L, R], 48000); return Buffer.from(a).equals(Buffer.from(w.subarray(44))); })());

  /* over the fake: a drop */
  const d = new DEMO.FakeDevice({ latencyMs: 1 });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l), desc = await HL.getDescriptor(l, info);
  const state = async () => HL.parseState(await l.request(HL.CMD.state));
  const settle = async seq0 => { for (let i = 0; i < 200; i++) { const s = await state(); if (s.pads.seq !== seq0 && s.pads.state === 0) return s; d.padOp.t -= 500; await new Promise(r => setTimeout(r, 5)); } return null; };
  const err = async f => { try { await f(); return 'ok'; } catch (e) { return e.message; } };
  let s = await state();
  check('pads: the 45 s planes, a 4 s break: 41 s for pads, nothing held, nothing unsaved', s.bufCap === 45 * 48000 && s.pads.total === 45 * 48000 - s.bufLen && s.pads.free === s.pads.total && s.pads.held === 0 && !s.pads.unsaved && !s.pads.noBreak && s.pads.mask === 0);
  const bodies = []; { const h = d.handle.bind(d); d.handle = (t, x) => { if (t === HL.CMD.pads) bodies.push(x.length); return h(t, x); }; }
  const L1 = new Float32Array(30000).map((_, i) => Math.sin(i / 9) * 0.6), pcm1 = HL.encodePcm16([L1]), prog = [];
  let polls = 0; const poller = setInterval(() => { l.request(HL.CMD.state, undefined, { key: 'state' }).then(() => polls++, () => {}); }, 20);
  const u0 = s.undoDepth, f0 = s.freePos, seq0 = s.pads.seq;
  const r = await HL.uploadPad(l, 5, 1, pcm1, { maxBody: info.maxBody, onProgress: (a, t, k) => prog.push([a, t, k]) });
  clearInterval(poller);
  check('drop: every byte acknowledged in order, progress to the end, the room said at the begin', r.bytes === pcm1.length && prog.length === Math.ceil(pcm1.length / HL.CARD.chunkFor(info.maxBody)) && prog.every((p, i) => i === 0 || p[0] > prog[i - 1][0]) && r.room === s.pads.free);
  check('drop: every request fits max_body, the polls interleave', bodies.length > 1 && Math.max(...bodies) <= info.maxBody && polls >= 3, Math.max(...bodies) + ' / polls ' + polls);
  s = await state(); check('…then the module commits (state 2, "committing")', s.pads.state === 2 && HL.PADS.STATE[s.pads.state] === 'committing');
  s = await settle(seq0);
  const t5 = s && s.keySlice[5], e5 = t5 != null ? d.table[t5] : null;
  check('…committed: key 5 owns [freePos, +30000), freePos moved, one undo step, the mask, unsaved', s && s.pads.result === 0 && e5 && e5.key === 5 && e5.start === f0 && e5.len === 30000 && s.freePos === f0 + 30000 && s.undoDepth === u0 + 1 && s.pads.mask === 1 << 5 && s.pads.unsaved && s.pads.free === s.pads.total - 30000);
  check('…the samples in the plane exactly as sent (no trim, no normalise)', Array.from(d.buf.subarray(f0, f0 + 30000)).every((v, i) => v === new DataView(pcm1.buffer).getInt16(i * 2, true) / 32768));
  /* replace it: the slot reused, the old region held; undo puts the first back */
  const pcm2 = HL.encodePcm16([new Float32Array(9600).fill(0.3)]), seqB = (await state()).pads.seq;
  await HL.uploadPad(l, 5, 1, pcm2); s = await settle(seqB);
  check('a second drop on key 5: the same slot, the new region, the old one held (30000), one more undo step', s.keySlice[5] === t5 && d.table[t5].start === f0 + 30000 && d.table[t5].len === 9600 && s.pads.held === 30000 && s.undoDepth === u0 + 2);
  await HL.editUndo(l); s = await state();
  check('Undo: the first sample back on key 5, freePos stays (nothing reclaimed), the second region held now', d.table[t5].start === f0 && d.table[t5].len === 30000 && s.freePos === f0 + 39600 && s.pads.held === 9600);
  await HL.editRedo(l); s = await state();
  check('Redo: the second again', d.table[t5].start === f0 + 30000 && s.pads.held === 30000);
  /* a stereo drop onto the mono kit: the kit turns stereo (Sampling → Stereo), both planes as sent */
  const SL = new Float32Array(4800).map((_, i) => Math.sin(i / 3) * 0.5), SR = SL.map(v => -v * 0.5), pcmS = HL.encodePcm16([SL, SR]), seqS = (await state()).pads.seq, fS = (await state()).freePos;
  check('the kit is mono before', !(await state()).stereo);
  await HL.uploadPad(l, 6, 2, pcmS); s = await settle(seqS);
  const dvS = new DataView(pcmS.buffer);
  check('a stereo drop: the kit is stereo, Sampling → Stereo, "made stereo" said, both planes exactly as sent, the old material copied into R', s.stereo && s.pads.madeStereo && d.options[DEMO.O.sampling] === 1
    && Array.from(d.buf.subarray(fS, fS + 4800)).every((v, i) => v === dvS.getInt16(i * 4, true) / 32768) && Array.from(d.bufR.subarray(fS, fS + 4800)).every((v, i) => v === dvS.getInt16(i * 4 + 2, true) / 32768)
    && d.bufR[100] === d.buf[100] && d.bufR[f0 + 10] === d.buf[f0 + 10]);
  const w1 = await HL.fetchWave(l, fS, 100, 1), w0 = await HL.fetchWave(l, fS, 100, 0);
  check('GET_WAVE plane 1 reads the R plane', w1.data[10] !== w0.data[10] && Math.sign(w1.data[10]) === -Math.sign(w0.data[10]));
  /* the refusals */
  check('BEGIN: key 24, three channels, under 480 frames → BAD_ARGS', await err(() => l.request(HL.CMD.pads, Uint8Array.of(0, 24, 1, 0, 4, 0, 0))) === 'BAD_ARGS' && await err(() => l.request(HL.CMD.pads, Uint8Array.of(0, 3, 3, 0, 4, 0, 0))) === 'BAD_ARGS' && await err(() => HL.uploadPad(l, 3, 1, new Uint8Array(400))) === 'BAD_ARGS' && await err(() => l.request(HL.CMD.pads, Uint8Array.of(0, 3, 1, 100, 0, 0, 0))) === 'BAD_ARGS');
  const room = (await state()).pads.free;
  check('BEGIN: more than the room → TOO_LARGE, nothing taken', await err(() => l.request(HL.CMD.pads, Uint8Array.of(0, 3, 1, ...[room + 1].flatMap(v => [v & 255, v >> 8 & 255, v >> 16 & 255, v >>> 24])))) === 'TOO_LARGE' && (await state()).pads.state === 0);
  check('DATA with nothing open: BAD_STATE', await err(() => l.request(HL.CMD.pads, Uint8Array.of(1, 4, 0, 0, 0, 0, 0, 0, 0, 1, 2))) === 'BAD_STATE');
  d.set.pending = { op: 14, name: 'x' }; check('BAD_STATE while a set op waits', await err(() => HL.uploadPad(l, 3, 1, pcm2)) === 'BAD_STATE' && await err(() => HL.newKit(l)) === 'BAD_STATE'); d.set.pending = null;
  d.rec.state = 2; check('BAD_STATE while a take runs', await err(() => HL.uploadPad(l, 3, 1, pcm2)) === 'BAD_STATE'); d.rec.state = 0;
  await l.request(HL.CMD.card, Uint8Array.of(0, 100, 0, 0, 0, 0, ...HL.nameBytes('card first')));
  check('BAD_STATE while a card upload arrives', await err(() => HL.uploadPad(l, 3, 1, pcm2)) === 'BAD_STATE'); await l.request(HL.CMD.card, Uint8Array.of(2));
  /* the reverse: while a drop is open, no card job, no load, no set op, no take */
  await l.request(HL.CMD.pads, Uint8Array.of(0, 3, 1, 0x80, 0x25, 0, 0));
  check('while a drop arrives: a card upload, a copy, a load, a set save, a take, New empty kit are all BAD_STATE', await err(() => HL.uploadBreak(l, 'later', HL.encodeWav([new Float32Array(24000)], 48000))) === 'BAD_STATE' && await err(() => HL.copyOldCard(l)) === 'BAD_STATE'
    && await err(() => l.request(HL.CMD.action, Uint8Array.of(HL.ACT.load, 1))) === 'BAD_STATE' && await err(() => HL.saveSet(l, 'during')) === 'BAD_STATE' && await err(() => l.request(HL.CMD.action, Uint8Array.of(HL.ACT.recArm, 2))) === 'BAD_STATE' && await err(() => HL.newKit(l)) === 'BAD_STATE');
  check('…edits stay open while the bytes arrive (they never touch freePos)', (await HL.editNudgeAll(l, 5, HL.EDIT.grid)).ok);
  await l.request(HL.CMD.pads, Uint8Array.of(2));
  s = await state(); check('ABORT: the drop forgotten, result "cancelled", nothing published', s.pads.state === 0 && HL.PADS.RESULT[s.pads.result] === 'cancelled' && s.keySlice[3] === d.spanSlice(3));
  await l.request(HL.CMD.pads, Uint8Array.of(0, 3, 1, 0x80, 0x25, 0, 0)); d.padUp.last -= 6000;
  s = await state(); check('a drop quiet for 5 s is dropped (result timed out)', s.pads.state === 0 && HL.PADS.RESULT[s.pads.result] === 'timed out');
  { const tot = 9600 * 2; await l.request(HL.CMD.pads, Uint8Array.of(0, 3, 1, 0x80, 0x25, 0, 0));
    check('out of sequence: BAD_ARGS, the drop dropped', await err(() => l.request(HL.CMD.pads, Uint8Array.of(1, tot & 255, tot >> 8, 0, 0, 10, 0, 0, 0, 1, 2))) === 'BAD_ARGS' && (await state()).pads.state === 0); }
  /* two drops at once through the shared queue both land, in order (the card's serial()) */
  { const run = HL.serial(), done = [];
    const job = (k, v) => run(async () => { const q0 = (await state()).pads.seq; await HL.uploadPad(l, k, 1, HL.encodePcm16([new Float32Array(4800).fill(v)])); done.push(k); await settle(q0); });
    await Promise.all([job(10, 0.1), job(11, -0.1)]);
    s = await state(); check('two drops through the queue: both land, in order', done.join() === '10,11' && d.table[s.keySlice[10]].key === 10 && d.table[s.keySlice[11]].key === 11 && d.buf[d.table[s.keySlice[11]].start] < 0); }
  /* paths 1 and 2 still work beside the pads at the new memory size: AUTOSLICE markers in the break, a take into a key */
  { const a = await HL.autoslice(l, 6, false, (await state()).tableGen); await l.request(HL.CMD.action, Uint8Array.of(HL.ACT.recArm, 20));
    for (let i = 0; i < 60 && (await state()).recState; i++) { d.rec.t -= 800; await new Promise(r => setTimeout(r, 2)); }
    s = await state(); check('path 1 (autoslice into the break) and path 2 (a take into key 20) beside the pads', a.ok && s.nMarkers === 6 && d.table[s.keySlice[20]].key === 20 && d.table[s.keySlice[5]].key === 5 && s.pads.mask === ((1 << 5) | (1 << 6) | (1 << 10) | (1 << 11) | (1 << 20))); }
  await l.close();

  /* a pads-only kit, end to end: New empty kit, drops, a key played, a set save, a power-cycle, the load back */
  const e = new DEMO.FakeDevice({ latencyMs: 1 }), le = new HL.Link(new DEMO.FakePort(e)); await le.start(); await HL.hello(le);
  const ste = async () => HL.parseState(await le.request(HL.CMD.state));
  const settleE = async q0 => { for (let i = 0; i < 200; i++) { const x = await ste(); if (x.pads.seq !== q0 && x.pads.state === 0) return x; e.padOp.t -= 500; await new Promise(r => setTimeout(r, 5)); } return null; };
  let x = await ste();
  await HL.newKit(le); x = await settleE(x.pads.seq);
  check('New empty kit: no break (the ¼ s stub, 16 slices), Pads, 44.75 s for pads, the undo history ended, nothing to lose', x && x.pads.noBreak && x.bufLen === DEMO.PAD_STUB && x.nSlices === 16 && x.grammar === DEMO.GRAM.pads && x.pads.total === 45 * 48000 - DEMO.PAD_STUB && x.pads.free === x.pads.total && x.undoDepth === 0 && !x.pads.unsaved && x.jackMap === 1, JSON.stringify(x && x.pads));
  const kick = new Float32Array(9600).map((_, i) => Math.sin(i * 0.02) * Math.exp(-i / 2000)), hatL = new Float32Array(4800).map((_, i) => ((i * 7919) % 13 - 6) / 7 * Math.exp(-i / 500)), hatR = hatL.map(v => v * 0.7);
  for (const [k, planes] of [[0, [kick]], [2, [hatL, hatR]]]) { const q0 = (await ste()).pads.seq; await HL.uploadPad(le, k, planes.length, HL.encodePcm16(planes)); x = await settleE(q0); }
  check('two pads dropped onto the empty kit (a mono kick, a stereo hat): the kit stereo, unsaved', x.pads.mask === 0b101 && x.stereo && x.pads.unsaved && x.pads.noBreak);
  const pk = await HL.playKey(le, 2, true);
  check('PLAY_KEY on the hat\'s pad: the voice starts at the hat', pk.ok && e.voice && e.voice.start === e.table[x.keySlice[2]].start && e.keyFlash === 2);
  await HL.saveSet(le, 'pads_only'); for (let i = 0; i < 60 && (await ste()).setState; i++) { e.set.t -= 300; }
  for (let i = 0; i < 60 && (x = await ste()).setSeq === 0; i++) { e.set.t -= 300; await new Promise(r => setTimeout(r, 2)); }
  check('Save set: the kit on the card — nothing unsaved now', x.setResult === 0 && x.setName === 'pads_only' && !x.pads.unsaved && e.sets.has('pads_only'));
  await le.close();
  /* the power-cycle: a fresh module, the same card */
  const e2 = new DEMO.FakeDevice({ latencyMs: 1 }); e2.sets = e.sets; e2.cardFs = e.cardFs; e2.files = e.files.slice();
  const l2 = new HL.Link(new DEMO.FakePort(e2)); await l2.start(); await HL.hello(l2);
  const st2 = async () => HL.parseState(await l2.request(HL.CMD.state));
  let y = await st2();
  check('after the power-cycle: the boot break, not the kit (the sounds were in memory)', !y.pads.noBreak && y.pads.mask === 0 && y.bufLen !== DEMO.PAD_STUB);
  await HL.loadSet(l2, 'pads_only'); for (let i = 0; i < 60 && ((y = await st2()).setSeq === 0 || y.setState); i++) { e2.set.t -= 300; await new Promise(r => setTimeout(r, 2)); }
  check('Load set "pads_only": the pads-only kit back — no break, the two pads on their keys, stereo, both planes as dropped, nothing unsaved',
    y.setResult === 0 && y.pads.noBreak && y.bufLen === DEMO.PAD_STUB && y.pads.mask === 0b101 && y.stereo && !y.pads.unsaved
    && Array.from(e2.buf.subarray(e2.table[y.keySlice[0]].start, e2.table[y.keySlice[0]].start + 9600)).every((v, i) => v === e.buf[e.table[x.keySlice[0]].start + i])
    && Array.from(e2.bufR.subarray(e2.table[y.keySlice[2]].start, e2.table[y.keySlice[2]].start + 4800)).every((v, i) => v === e.bufR[e.table[x.keySlice[2]].start + i]));
  const pk2 = await HL.playKey(l2, 0, true);
  check('…and the kick\'s pad plays it', pk2.ok && e2.voice && e2.voice.start === e2.table[y.keySlice[0]].start);
  const q9 = y.pads.seq; await HL.uploadPad(l2, 9, 1, HL.encodePcm16([new Float32Array(2400).fill(0.2)])); for (let i = 0; i < 100 && ((y = await st2()).pads.seq === q9 || y.pads.state); i++) { e2.padOp.t -= 500; }
  check('a drop after the load: unsaved again (the card holds the kit as it was)', y.pads.unsaved && y.pads.mask === 0b1000000101);
  await l2.close();
}

/* ── 7e. KEYP (ACTION 37 + 0x69): every key neutral to begin with, KEY_PARAMS and its refusals,
 *        GET_KEYS, one undo step per edit, a drag's joined sends one step (a join for another key a step of its own),
 *        a key's own Plays on PLAY_KEY (Held, Loop, Ratchet hold; One-shot does not), a set save / load carrying the
 *        settings, new material neutral ── */
{
  const d = new DEMO.FakeDevice({ latencyMs: 1 });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l), desc = await HL.getDescriptor(l, info);
  const state = async () => HL.parseState(await l.request(HL.CMD.state));
  const keys = () => HL.fetchKeyParams(l);
  const err = async f => { try { await f(); return 'ok'; } catch (e) { return e.message; } };
  let k = await keys();
  check('keyp: GET_KEYS: 24 keys, 5 bytes each, every one neutral', k.length === 24 && k.every(HL.keypNeutral) && (await l.request(HL.CMD.keys, new Uint8Array(0))).length === 2 + 24 * 5);
  let s0 = await state();
  let r = await HL.keyParams(l, 3, { gain: -12, cents: 700, plays: 2 });
  k = await keys(); let s = await state();
  check('KEY_PARAMS: OK with the new tableGen, read back exactly, one undo step', r.ok && r.tableGen === s.tableGen && r.tableGen !== s0.tableGen && k[3].gain === -12 && k[3].cents === 700 && k[3].plays === 2 && s.undoDepth === s0.undoDepth + 1);
  check('the words: "−6 dB · +7 st · held"; a ratchet says its rate', HL.keypText(k[3]) === '−6 dB · +7 st · held' && HL.keypText({ gain: 0, cents: 0, plays: 4, rat: 5 }) === 'ratchet 1/16' && HL.keypText(k[4]) === '');
  const raw = async b => (await HL.editOp(l, HL.ACT.keyParams, b)).status;
  const bad = [[24, 0, 0, 0, 0, 0, 0], [3, 13, 0, 0, 0, 0, 0], [3, 0xCF, 0, 0, 0, 0, 0], [3, 0, 0x61, 0x09, 0, 0, 0], [3, 0, 0, 0, 5, 0, 0], [3, 0, 0, 0, 4, 7, 0], [3, 0, 0, 0, 0, 0, 2]];
  const st2 = []; for (const b of bad) st2.push(await raw(b));
  check('KEY_PARAMS: key 24, gain +6.5 / −24.5 dB, 24.01 st, plays 5, ratchet rate 7, unknown flags → BAD_ARGS, nothing written', st2.every(x => x === 2) && (await keys())[3].gain === -12, JSON.stringify(st2));
  check('KEY_PARAMS short (no flags byte) → BAD_ARGS', await raw([3, 0, 0, 0, 0, 0]) === 2);
  check('a stale gen → BAD_STATE', (await HL.keyParams(l, 3, { gain: -2 }, false, ((await state()).tableGen % 255) + 1)).status === 3);
  r = await HL.editUndo(l); k = await keys();
  check('Undo: key 3 neutral again', r.ok && HL.keypNeutral(k[3]));
  r = await HL.editRedo(l); k = await keys();
  check('Redo: back', r.ok && k[3].gain === -12 && k[3].plays === 2);
  /* a drag: one step */
  s0 = await state();
  for (let i = 0; i < 12; i++) await HL.keyParams(l, 5, { gain: -i * 2, cents: 0, plays: 0 }, i > 0);
  s = await state(); k = await keys();
  check('a slider drag (12 sends, every one after the first with JOIN): one undo step, the last value', s.undoDepth === s0.undoDepth + 1 && k[5].gain === -22);
  await HL.keyParams(l, 6, { gain: -4 }, true);
  check('a JOIN for another key: a step of its own', (await state()).undoDepth === s0.undoDepth + 2);
  await HL.editUndo(l); await HL.editUndo(l); k = await keys();
  check('two undos: both keys neutral, key 3 kept', HL.keypNeutral(k[5]) && HL.keypNeutral(k[6]) && k[3].gain === -12);
  /* the key's own Plays on PLAY_KEY (the option is One-shot) */
  const plays = async (key, p) => { await HL.keyParams(l, key, { gain: 0, cents: 0, plays: p, rat: 1 }); await HL.playKey(l, key, true); return d.voice && d.voice.key === key && d.voice.until === Infinity; };
  const holds = [await plays(8, 0), await plays(8, 1), await plays(8, 2), await plays(8, 3), await plays(8, 4)];
  await HL.playKey(l, 8, false);
  check('PLAY_KEY holds as the key says: Follow (the option: one-shot) no, One-shot no, Held / Loop / Ratchet yes; the up releases', JSON.stringify(holds) === '[false,false,true,true,true]' && d.voice.until !== Infinity, JSON.stringify(holds));
  /* a set carries them */
  const setOp = async (fn, name) => { const q = (await state()).setSeq; await fn(l, name); for (let i = 0; i < 100; i++) { d.set.t -= 1000; const x = await state(); if (x.setSeq !== q && !x.setState) return x; } return null; };
  await HL.keyParams(l, 7, { gain: 4, cents: -1200, plays: 4, rat: 6 });
  const want = JSON.stringify(await keys());
  check('Save set: done', !!(await setOp(HL.saveSet, 'keyp_kit')));
  await HL.keyParams(l, 7, { gain: 0, cents: 0, plays: 0 }); await HL.keyParams(l, 3, { gain: 0, cents: 0, plays: 0 });
  check('Load set: the settings come back with it (a 1/32 ratchet among them)', !!(await setOp(HL.loadSet, 'keyp_kit')) && JSON.stringify(await keys()) === want && (await keys())[7].rat === 6);
  d.loadBreak(); d.bumpGen();
  check('new material (a loose break): every key neutral', (await keys()).every(HL.keypNeutral));
  await l.close();
}

/* ── 7f. CHROMA (ACTION 38 SPREAD_KEYS): one key's sample on all 24 keys, a semitone a key — the
 *        tunes, gain and Plays read back, one undo step that puts every key's map and settings back, keep pads, a
 *        spread in place moving nothing, the refusals, a set carrying it, the plan the page asks first ── */
{
  const d = new DEMO.FakeDevice({ latencyMs: 1, kit: true });   /* the kit: recordings on D♯1 (3), G1 (7), D2 (14) */
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l), desc = await HL.getDescriptor(l, info);
  const state = async () => HL.parseState(await l.request(HL.CMD.state));
  const keys = () => HL.fetchKeyParams(l);
  const table = async () => HL.fetchTable(l);
  const snap = async () => JSON.stringify({ k: await keys(), m: Array.from((await state()).keySlice) });
  await HL.keyParams(l, 3, { gain: -12, cents: 30, plays: 2 });   /* the root: −6 dB, +30 cents, Held */
  await HL.keyParams(l, 11, { gain: 6, cents: -500, plays: 1 });
  let s0 = await state(); const before = await snap(), tb = await table(), t = s0.keySlice[3];
  const plan = HL.spreadPlan(s0.keySlice, tb.entries, tb.nSlices, 3);
  check('the page\'s plan for D♯1: the two other pads (G1, D2) would be replaced', JSON.stringify(plan.pads) === '[7,14]' && plan.t === t, JSON.stringify(plan));
  check('spreadCents: (j − root) × 100 + the root\'s cents, clamped at ±2400', HL.spreadCents(0, 3, 30) === -270 && HL.spreadCents(23, 0, 500) === 2400 && HL.spreadCents(0, 23, -200) === -2400);
  let r = await HL.spreadKeys(l, 3);
  let s = await state(), k = await keys();
  check('SPREAD_KEYS(D♯1): OK, count 23, the new tableGen, one undo step', r.ok && r.count === 23 && r.tableGen === s.tableGen && r.tableGen !== s0.tableGen && s.undoDepth === s0.undoDepth + 1, JSON.stringify(r));
  check('…every key plays D♯1\'s pad', Array.from(s.keySlice).every(x => x === t));
  check('…key j: (j − 3) semitones + 30 cents, −6 dB, Held; D♯1 itself as it was', k.every((p, j) => p.cents === (j - 3) * 100 + 30 && p.gain === -12 && p.plays === 2 && p.rat === 0), JSON.stringify(k.map(p => p.cents)));
  check('…the words: C1 "−6 dB · −2.7 st · held", C2 "−6 dB · +9.3 st · held"', HL.keypText(k[0]) === '−6 dB · −2.7 st · held' && HL.keypText(k[12]) === '−6 dB · +9.3 st · held', HL.keypText(k[0]) + ' | ' + HL.keypText(k[12]));
  const spread = await snap();
  r = await HL.spreadKeys(l, 3);
  check('the same spread again: OK, nothing moves (the gen and the undo depth as they were)', r.ok && r.count === 23 && r.tableGen === s.tableGen && (await state()).undoDepth === s.undoDepth);
  r = await HL.editUndo(l);
  check('Undo: every key\'s map and settings exactly as before (the pads on G1 and D2 back, key 11\'s own settings back)', r.ok && await snap() === before && (await state()).undoDepth === s0.undoDepth);
  r = await HL.editRedo(l);
  check('Redo: the spread again, exactly', r.ok && await snap() === spread);
  await HL.editUndo(l);
  r = await HL.spreadKeys(l, 3, true); s = await state(); k = await keys();
  check('keep pads (b0): 21 keys spread, G1 and D2 keep their own pads and settings', r.ok && r.count === 21 && s.keySlice[7] !== t && s.keySlice[14] !== t && s.keySlice[0] === t && k[11].cents === 830 && JSON.stringify(k[7]) === JSON.stringify(JSON.parse(before).k[7]));
  await HL.editUndo(l);
  check('…undone: as before', await snap() === before);
  const raw = async b => (await HL.editOp(l, HL.ACT.spreadKeys, b)).status;
  check('SPREAD_KEYS: root 24, an unknown flag, a short body → BAD_ARGS; nothing moved', await raw([24, 0]) === 2 && await raw([3, 2]) === 2 && await raw([3]) === 2 && await snap() === before);
  check('a stale gen → BAD_STATE', (await HL.spreadKeys(l, 3, false, ((await state()).tableGen % 255) + 1)).status === 3 && await snap() === before);
  /* a set carries the spread: the map onto D♯1's pad, 24 tunes */
  const setOp = async (fn, name) => { const q = (await state()).setSeq; await fn(l, name); for (let i = 0; i < 100; i++) { d.set.t -= 1000; const x = await state(); if (x.setSeq !== q && !x.setState) return x; } return null; };
  await HL.spreadKeys(l, 3); const want = await snap();
  check('Save set with a spread: done', !!(await setOp(HL.saveSet, 'chroma_kit')));
  await HL.editUndo(l);
  check('Load set: the spread comes back (the map and every tune)', !!(await setOp(HL.loadSet, 'chroma_kit')) && await snap() === want);
  await l.close();
}

/* ── 8. EDIT-UI: the pure editor (edit.js) — magnets, clamps, handles, positions, labels, the drop rule, the onset
 *       preview, the audition window, the gesture recogniser — then the Editor driven against the fake module ── */
const ED = require('./edit.js');
{
  const SL = DEMO.SLICE_LEN;
  check('spanSlice: the engine\'s arithmetic', ED.spanSlice(0, 32) === 0 && ED.spanSlice(23, 32) === 30 && ED.spanSlice(5, 16) === 5 && ED.spanSlice(20, 16) === 15);
  /* magnets */
  const hits = [1000, 2000];
  check('nearest onset within ±12 ms, the earlier wins a tie, none past the radius', ED.nearestOnset(hits, 1300) === 1000 && ED.nearestOnset(hits, 1500) === 1000 && ED.nearestOnset(hits, 1700) === 2000 && ED.nearestOnset(hits, 3000) === null && ED.nearestOnset(null, 1000) === null);
  const wav = [1, 1, 1, -1, -1, -1, 1, 1], sAt = i => (i >= 0 && i < wav.length ? wav[i] : null);
  check('zero crossing: the nearest sign change, a gap in the cache gives up, a flat run has none', ED.zeroCross(2, 4, sAt) === 3 && ED.zeroCross(5, 4, sAt) === 6 && ED.zeroCross(4, 1, sAt) === 3 && ED.zeroCross(1, 1, () => 1) === null && ED.zeroCross(7, 3, sAt) === 6 && ED.zeroCross(2, 3, i => (i < 4 ? 1 : null)) === null && ED.zeroCross(2, 4, null) === null);
  check('snap: onset first, then the zero crossing; off = raw', ED.snap(1300, { hits, zero: false }).by === 'onset' && ED.snap(1300, { hits, zero: false }).pos === 1000 && ED.snap(1300, { hits, off: true }).pos === 1300 && ED.snap(1300, { hits, off: true }).by === null
    && ED.snap(2, { hits: [4], zero: true, sampleAt: sAt, onsetR: 3, zeroR: 2 }).pos === 3 && ED.snap(2, { hits: [4], zero: true, sampleAt: sAt, onsetR: 3, zeroR: 2 }).by === 'onset+zero' && ED.snap(2, { hits: [], zero: true, sampleAt: sAt, zeroR: 2 }).by === 'zero');
  /* clamps on a synthetic table: four grid slices of 6000, a marker over slice 1, a tombstone, a recording past the break */
  const tb = { nSlices: 4, entries: [] }; for (let i = 0; i < 4; i++) tb.entries.push({ start: i * 6000, len: 6000, key: -1, cls: i & 3, sal: 200, hit: true, dens: 0, flags: 0 });
  tb.entries.push({ start: 6000, len: 6000, key: -1, cls: 1, sal: 0, hit: false, dens: 0, flags: HL.EDIT.orange });            /* 4: a marker over slice 1 */
  tb.entries.push({ start: 12000, len: 0, key: -1, cls: 0, sal: 0, hit: false, dens: 0, flags: HL.EDIT.tomb });                /* 5: a tombstone */
  tb.entries.push({ start: 24000, len: 9600, key: 3, cls: 0, sal: 255, hit: true, dens: 0, flags: 0 });                       /* 6: a recording (key 3) */
  tb.entries.push({ start: 12000, len: 6000, key: -1, cls: 1, sal: 0, hit: false, dens: 0, flags: 0 });                       /* 7: a grey marker ending where 4… no, adjacent to 4's end */
  check('clampBoundary: 64 from either neighbour\'s far end; 0 and n cannot move', ED.clampBoundary(tb, 1, 10) === 64 && ED.clampBoundary(tb, 1, 11990) === 11936 && ED.clampBoundary(tb, 1, 8000) === 8000 && ED.clampBoundary(tb, 0, 100) === null && ED.clampBoundary(tb, 4, 100) === null && ED.clampBoundary(null, 1, 1) === null);
  check('clampStart keeps the end and 480; clampEnd keeps the start, ≤ freePos', JSON.stringify(ED.clampStart(tb.entries[4], 11800, 33600)) === '{"start":11520,"len":480}' && ED.clampStart(tb.entries[4], -50, 33600).start === 0 && ED.clampEnd(tb.entries[6], 99999, 33600).len === 9600 && ED.clampEnd(tb.entries[6], 24000, 33600).len === 480);
  /* handles and hit-testing */
  const hl = ED.handleList(tb, 4);
  check('handleList: boundaries 1..3, the live markers\' starts, the selected marker\'s end, no tombstone', hl.length === 3 + 3 + 1 && hl.filter(h => h.kind === 'grid').map(h => h.t).join() === '1,2,3' && hl.filter(h => h.kind === 'start').map(h => h.t).join() === '4,6,7' && hl.find(h => h.kind === 'end').pos === 12000 && ED.handleList(tb, 6).find(h => h.kind === 'end').t === 6 && !ED.handleList(tb, 2).some(h => h.kind === 'end'));
  const xOf = p => p / 24000 * 600;   /* the break over 600 px: 25 px per 1000 samples */
  check('hitHandle: within the slop, nearest first, the selection wins a tie, nothing past 22 px', ED.hitHandle(hl, 150 + 10, xOf).t === 1 && ED.hitHandle(hl, 150 + 23, xOf) === null && ED.hitHandle(hl, 300, xOf, 22, 4).kind === 'end' && ED.hitHandle(hl, 300, xOf, 22, 7).kind === 'start' && ED.hitHandle(hl, 300, xOf, 22, 2).t === 2);
  check('entryAt: the smallest marker over the grid, the grid where none, −1 past the end', ED.entryAt(tb, 7000) === 4 && ED.entryAt(tb, 1000) === 0 && ED.entryAt(tb, 13000) === 7 && ED.entryAt(tb, 25000) === 6 && ED.entryAt(tb, 99999) === -1);
  check('mergeTarget: adjacent when a marker ends at the start, else the previous live one, null with none', ED.mergeTarget(tb, 7).u === 4 && ED.mergeTarget(tb, 7).adjacent && ED.mergeTarget(tb, 6).u === 4 && !ED.mergeTarget(tb, 6).adjacent && ED.mergeTarget(tb, 4) === null && ED.mergeTarget(tb, 5) === null && ED.mergeTarget(tb, 1) === null);
  check('nearestBoundary for PROMOTE', ED.nearestBoundary(tb, 11000) === 2 && ED.nearestBoundary(tb, 500) === 1 && ED.nearestBoundary({ nSlices: 1, entries: [tb.entries[0]] }, 5) === -1);
  check('keysOn', ED.keysOn(Int8Array.from([0, 6, 6, 2]), 6).join() === '1,2' && ED.keysOn(null, 0).length === 0);
  /* positions */
  const pc = { nativeBpm: 120, gridBars: 2, bufLen: 192000, nSlices: 32 };
  check('bar:beat:16th from nativeBpm (120 → 6000 per 16th), 1-based', ED.stepLen(pc) === 6000 && ED.fmtBbs(0, pc) === '1:1:1' && ED.fmtBbs(6000 * 17, pc) === '2:1:2' && ED.fmtBbs(6000 * 7 + 100, pc) === '1:2:4' && ED.fmtBbs(100, { bufLen: 192000, nSlices: 32 }) === '1:1:1' && ED.fmtBbs(100, {}) === '—');
  check('parsePos: bar:beat:16th, ms, s, samples, the field\'s unit, garbage', ED.parsePos('2:1:2', 'samples', pc) === 102000 && ED.parsePos('12.5ms') === 600 && ED.parsePos(' 0.5 s') === 24000 && ED.parsePos('1234') === 1234 && ED.parsePos('10', 'ms') === 480 && ED.parsePos('−4 ms') === -192 && ED.parsePos('x') === null && ED.parsePos('1:1:1', 'bbs', {}) === null && ED.parsePos('3', 'bbs', pc) === null);
  check('fmtPos / fmtMs / fmtDelta', ED.fmtPos(4800, 'ms') === '100 ms' && ED.fmtPos(120, 'ms') === '2.5 ms' && ED.fmtPos(102000, 'bbs', pc) === '2:1:2' && ED.fmtPos(7.4, 'samples') === '7' && ED.fmtDelta(-192) === '−4 ms' && ED.fmtDelta(96) === '+2 ms');
  /* labels and the history viewer */
  check('history labels', ED.histLabel('split', { t: 7, newIdx: 40 }) === 'split 7 → 40' && ED.histLabel('start', { t: 3, d: -192 }) === 'start 3 −4 ms' && ED.histLabel('boundary', { i: 5, d: 96 }) === 'boundary 5 +2 ms' && ED.histLabel('onsets', { thresh: 128, count: 8 }) === 'onsets ≥ 50 % → 8' && ED.histLabel('key', { key: 0, t: 33 }) === 'C1 → 33' && ED.histLabel('swap', { a: 0, b: 13 }) === 'C1 ↔ C♯2' && ED.histLabel('nudge', { ms: 5, which: HL.EDIT.grid }) === 'nudge grid +5 ms' && ED.histLabel('merge', { t: 36, u: 35 }) === 'merge 35 into 36' && ED.histLabel('tap', { newIdx: 37 }) === 'tap → marker 37');
  { const h = new ED.History(3);
    check('History: the first gen seen is silent, a foreign gen is the module\'s, the page\'s own gen is not', h.note(5) === false && h.items.length === 0 && h.note(6) === true && h.items[0].label === 'changed on the module' && (h.push(7, 'split 7'), h.note(7)) === false && h.items.length === 2);
    h.push(8, 'a'); h.push(9, 'b'); check('History: newest first, capped', h.list().map(x => x.label).join('|') === 'b|a|split 7' && h.items.length === 3 && h.note(9, 4, 1) === false && h.undoDepth === 4 && h.redoDepth === 1);
    /* EDIT-UNDO: undo marks the newest live entry, redo un-marks the oldest undone, a new op drops the undone ones, the tail reconciles */
    h.undo(10); check('History: undo strikes the newest entry, keeps it listed', h.list()[0].label === 'b' && h.list()[0].undone === true && h.list()[1].undone === false && h.gen === 10);
    h.undo(11); check('History: a second undo strikes the next', h.list()[1].undone === true && h.list()[2].undone === false);
    h.redo(12); check('History: redo un-marks the oldest undone', h.list()[1].undone === false && h.list()[0].undone === true);
    h.push(13, 'c'); check('History: a new op drops the undone entries (the redo half is gone)', h.list().map(x => x.label).join('|') === 'c|a|split 7' && h.items.every(x => !x.undone));
    h.undo(14); h.note(14, 2, 0); check('History: the tail\'s redo depth 0 drops the marks the page still had', h.items.every(x => !x.undone) && h.items.length === 2);
    h.push(15, 'd'); h.undo(16); check('History: a stale state (gen 15, redo 0) after the undo\'s reply (16) changes nothing — no module line, the mark kept', h.note(15, 3, 0) === false && h.list()[0].label === 'd' && h.list()[0].undone === true && h.gen === 16 && h.undoDepth === 2);
    check('…and the next state (16, redo 1) is quiet; a gen past the wrap (16 → 3) is a new one', h.note(16, 2, 1) === false && h.list()[0].undone === true && new ED.History(3).note(250) === false);
    { const w = new ED.History(3); w.note(250); check('History: gens wrap at 256 (250 → 3 is forward: the module\'s)', w.note(3) === true && w.gen === 3); } }
  /* the drop rule */
  const ks = Int8Array.from({ length: 24 }, (_, k) => ED.spanSlice(k, 4)); ks[3] = 6;   /* key 3 plays its recording (entry 6) */
  const dctx = { table: tb, keySlice: ks, nSlices: 4 };
  let pl = ED.dropPlan(dctx, { t: 4 }, 5);
  check('drop a marker on an unoccupied key: SET_KEY, no prompt', pl.kind === 'set' && !pl.red && pl.choices[0].ops[0].join() === 'setKey,5,4');
  pl = ED.dropPlan(dctx, { key: 3 }, 5);
  check('drop a cell on an unoccupied cell: SWAP_KEYS', pl.kind === 'swap' && pl.choices[0].ops[0].join() === 'swapKeys,3,5');
  pl = ED.dropPlan(dctx, { t: 4 }, 3);
  check('drop a keyless marker on the recording\'s only key: red, replace or cancel (no swap possible), default replace', pl.kind === 'choose' && pl.red && pl.def === 'replace' && pl.choices.map(c => c.id).join() === 'replace,cancel');
  ks[7] = 7; pl = ED.dropPlan(dctx, { key: 7 }, 3);
  check('drop a cell on the recording\'s only key: red, swap / replace / cancel, default swap', pl.kind === 'choose' && pl.red && pl.def === 'swap' && pl.choices.map(c => c.id).join() === 'swap,replace,cancel' && pl.choices[0].ops[0].join() === 'swapKeys,7,3');
  ks[9] = 6; pl = ED.dropPlan(dctx, { key: 7 }, 3);
  check('…not red when another key still plays the recording; default replace', pl.kind === 'choose' && !pl.red && pl.def === 'replace'); ks[9] = ED.spanSlice(9, 4);
  ks[10] = 4; pl = ED.dropPlan(dctx, { t: 4 }, 3);
  check('a marker with a key can swap: the key that plays it trades with the target', pl.choices[0].id === 'swap' && pl.choices[0].ops[0].join() === 'swapKeys,10,3' && pl.def === 'swap');
  pl = ED.dropPlan(dctx, { key: 10 }, 7);
  check('drop on an occupied grey key (not a recording): a prompt, not red', pl.kind === 'choose' && !pl.red && pl.def === 'replace');
  check('drop on itself, a tombstone, a bad key: nothing', ED.dropPlan(dctx, { key: 3 }, 3) === null && ED.dropPlan(dctx, { t: 5 }, 1) === null && ED.dropPlan(dctx, { t: 4 }, 24) === null && ED.dropPlan({ table: null }, { t: 4 }, 1) === null);
  /* the onset histogram */
  const bars = ED.onsetBars([100, 6100, 12100, 30000], tb);
  check('onsetBars: strength = the grid slice\'s salience, 0 past the break', bars.map(b => b.strength).join() === '200,200,200,0');
  const acc = ED.onsetAccept([{ pos: 0, strength: 250 }, { pos: 100, strength: 250 }, { pos: 6000, strength: 100 }, { pos: 12000, strength: 250 }, { pos: 23800, strength: 250 }], 150, 1680, 24000);
  check('onsetAccept: threshold, the min gap, spans to the next accepted, the short tail skipped', acc.positions.join() === '0,12000,23800' && acc.count === 2 && acc.spans[1].len === 11800);
  /* the audition window */
  const rg = new ED.RateGate(100);
  check('RateGate: the first goes, the next is held, the held one goes when due, a later one replaces', rg.offer('a', 0) === 'a' && rg.offer('b', 50) === null && rg.due(80) === null && rg.offer('c', 90) === null && rg.due(100) === 'c' && rg.due(150) === null && rg.offer('d', 250) === 'd');
  /* the gesture recogniser */
  const g = new ED.Gesture({ slop: 10, dblMs: 300, holdMs: 500 }); const types = evs => evs.map(e => e.type).join();
  check('tap', types(g.down(1, 10, 10, 0, 'a')) === 'press' && types(g.move(1, 12, 11, 20)) === '' && types(g.up(1, 12, 11, 40)) === 'tap,release');
  check('double tap (same tag, within 300 ms), then a fresh tap', types(g.down(1, 11, 11, 100, 'a').concat(g.up(1, 11, 11, 120))) === 'press,dbltap,release' && types(g.down(1, 11, 11, 200, 'a').concat(g.up(1, 11, 11, 220))) === 'press,tap,release');
  check('a different tag is not a double tap', types(g.down(1, 11, 11, 300, 'b').concat(g.up(1, 11, 11, 320))) === 'press,tap,release');
  check('drag past the slop: dragstart, drag…, dragend, release — no tap', types(g.down(1, 0, 0, 1000).concat(g.move(1, 5, 0, 1010), g.move(1, 20, 0, 1020), g.move(1, 30, 0, 1030), g.up(1, 30, 0, 1040))) === 'press,dragstart,drag,drag,dragend,release');
  check('hold: the clock fires it once, a still lift is no tap, a later move is no drag', types(g.down(1, 0, 0, 2000)) === 'press' && types(g.tick(2400)) === '' && types(g.tick(2600)) === 'hold' && types(g.tick(2700)) === '' && types(g.move(1, 40, 0, 2800)) === '' && types(g.up(1, 40, 0, 2900)) === 'release' && g.up(1, 40, 0, 2900).length === 0);
  check('second finger: second / secondup around the first, pinch when both move', types(g.down(1, 0, 0, 3000)) === 'press' && types(g.down(2, 100, 0, 3010)) === 'second' && types(g.move(2, 120, 0, 3020)) === '' && g.move(2, 150, 0, 3030)[0].type === 'pinch' && g.move(2, 175, 0, 3040)[0].scale > 1 && types(g.up(2, 175, 0, 3050)) === 'secondup' && types(g.up(1, 0, 0, 3060)) === 'release' && !g.active);
  check('cancel: dragend (cancelled) and release', types(g.down(1, 0, 0, 4000).concat(g.move(1, 50, 0, 4010), g.cancel(1))) === 'press,dragstart,drag,cancel,dragend,release' && !g.active);

  /* ── the Editor over the fake module: a page-shaped harness with the view over the whole break at 600 × 290 ── */
  const d = new DEMO.FakeDevice({ kit: true, nSlices: 32, latencyMs: 1 });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start(); await HL.hello(l);
  const sent = []; const req0 = l.request.bind(l); l.request = (type, body, o) => { if (type === HL.CMD.action) sent.push(Array.from(body)); return req0(type, body, o); };
  const Hh = { table: null, st: null, hits: null, view: { a: 0, b: 1 }, status: [], redraws: 0, menus: [], answer: null, dropKey: -1, sel: -1, fit: null, plans: [] };
  const refetch = async () => { Hh.st = HL.parseState(await l.request(HL.CMD.state)); Hh.table = await HL.fetchTable(l); Hh.hits = await HL.fetchHits(l); };
  await refetch(); Hh.view.a = 0; Hh.view.b = Hh.st.bufLen;
  let clock = 0; const timers = []; const now = () => clock;
  const setTimer = (f, ms) => { const id = { f, at: clock + ms }; timers.push(id); return id; }, clearTimer = id => { const i = timers.indexOf(id); if (i >= 0) timers.splice(i, 1); };
  const fire = () => { for (const t of timers.splice(0)) if (t.at <= clock) t.f(); else timers.push(t); };
  const ed = new ED.Editor({ link: () => l, view: Hh.view, now, setTimer, clearTimer, gesture: { holdMs: 60, dblMs: 200 },
    ctx: () => ({ table: Hh.table, st: Hh.st, hits: Hh.hits, sampleAt: null, tapAllowMs: 40 }),
    cb: { redraw: () => Hh.redraws++, status: (m, k) => Hh.status.push(m), select: t => { Hh.sel = t; }, refetch,
      setView: (a, b) => { Hh.view.a = Math.round(a); Hh.view.b = Math.round(b); }, zoomView: (f, at) => { const s = (Hh.view.b - Hh.view.a) * f; Hh.view.a = Math.round(at - (at - Hh.view.a) * f); Hh.view.b = Hh.view.a + Math.round(s); },
      fitEntry: t => { Hh.fit = t; }, menu: (items, x, y, t) => Hh.menus.push({ items, t }), choose: async plan => { Hh.plans.push(plan); return Hh.answer || plan.def; }, dropTarget: () => Hh.dropKey } });
  const W = 600, HH = 290, px = p => p / Hh.st.bufLen * W;
  const L = ed.layout(W, HH);
  check('layout: wave / a 44 px handle row / the strip', L.wave[1] === 200 && L.handles[0] === 200 && L.handles[1] === 244 && L.strip[0] === 244 && ed.zoneAt(100, L) === 'wave' && ed.zoneAt(220, L) === 'handles' && ed.zoneAt(260, L) === 'strip');
  const P = (kind, id, x, y, extra = {}) => ed.pointer(kind, Object.assign({ id, x, y, w: W, h: HH, t: clock, cx: x, cy: y }, extra));
  const settle = async () => { for (let i = 0; i < 50 && (ed.busy || ed.pending); i++) { await ed.pending; ed.pending = null; } };
  /* select by a tap on a handle: an audition of that slice */
  const hx5 = px(5 * SL);
  check('tagAt: the handle row finds boundary 5', ed.tagAt(hx5 + 8, 220, W, HH).h.kind === 'grid' && ed.tagAt(hx5 + 8, 220, W, HH).h.t === 5 && ed.tagAt(hx5 + 30, 220, W, HH).h.t === 7 && ed.tagAt(hx5 + 8, 100, W, HH).t === 5 && ed.tagAt(hx5 + 8, 100, W, HH).h === null);
  sent.length = 0; P('down', 1, hx5 + 3, 220); clock += 30; P('up', 1, hx5 + 3, 220);
  check('tap a handle: selected, the whole slice auditioned once', ed.selT === 5 && Hh.sel === 5 && sent.filter(b => b[0] === 27).length === 1 && HL.u32(Uint8Array.from(sent[0]), 1) === 5 * SL && HL.u16(Uint8Array.from(sent[0]), 5) === SL / 48);
  /* drag boundary 5 by −15 px = −4800 samples (no onset within 12 ms): local at every move, ≤ 1 audition per 100 ms, SET_BOUNDARY on release */
  clock += 200; sent.length = 0; Hh.redraws = 0;   /* the tap's audition opened the window; let it pass */
  P('down', 1, hx5, 220); clock += 10;
  for (let i = 1; i <= 20; i++) { P('move', 1, hx5 - i * 0.75, 220); clock += 2; fire(); }
  check('the handle follows the finger locally before anything is sent', ed.drag && ed.drag.kind === 'handle' && ed.drag.pos === 5 * SL - 4800 && Hh.redraws >= 5 && sent.filter(b => b[0] === 16).length === 0);
  const previews = sent.filter(b => b[0] === 27).length;
  check('auditions on the move: rate-limited to the 100 ms window (40 ms of moves → 1 sent, the rest held)', previews === 1 && ed.gate.held && ed.gate.held.start === 5 * SL - 4800);
  P('up', 1, hx5 - 15, 220); await settle();
  const t1 = Hh.table;
  check('release: the whole slice auditioned from the new start, SET_BOUNDARY(5) committed, the table refetched, orange', sent.some(b => b[0] === 27 && HL.u32(Uint8Array.from(b), 1) === 5 * SL - 4800 && HL.u16(Uint8Array.from(b), 5) === (SL + 4800) / 48) && sent.some(b => b[0] === 16 && b[1] === 5) && t1.entries[5].start === 5 * SL - 4800 && t1.entries[4].len === SL - 4800 && (t1.entries[5].flags & HL.EDIT.orange) && ed.drag === null && ed.stats.commits === 1);
  check('history: "boundary 5 −100 ms" keyed by the reply\'s gen', ed.history.list()[0].label === 'boundary 5 −100 ms' && ed.history.list()[0].gen === Hh.st.tableGen);
  clock += 200; fire(); check('the trailing audition timer finds nothing held after the release', ed.gate.held === null);
  /* the second finger turns the magnets off mid-drag */
  const hx8 = px(8 * SL); sent.length = 0;
  P('down', 1, hx8, 220); clock += 10; P('move', 1, hx8 + 15, 220); clock += 10;   /* +4800: past the slop, no onset within 12 ms */
  check('past the magnets: raw', ed.drag.pos === 8 * SL + 4800 && ed.drag.by === null);
  P('move', 1, hx8 + 1.5, 220);   /* +480 samples: within 576 of the onset at 8 SL → snapped back to it */
  check('snap: within ±12 ms of an onset the handle sits on it', ed.drag.pos === 8 * SL && ed.drag.by === 'onset');
  P('down', 2, 300, 100); P('move', 1, hx8 + 1.5, 220);
  check('a second finger: the magnets are off, the position is raw', ed.snapOff && ed.drag.pos === 8 * SL + 480 && ed.drag.by === null);
  P('up', 2, 300, 100); P('cancel', 1, hx8 + 1.5, 220); await settle();
  check('a cancelled drag commits nothing, the magnets come back', !sent.some(b => b[0] === 16) && !ed.snapOff && Hh.table.entries[8].start === 8 * SL);
  /* copy slice 6 out as a marker (ADD_ENTRY of its span), then the marker's start and end handles */
  const r0 = await ed.copyOut(6); await settle();
  const m0 = r0.newIdx;
  check('copy out: an orange marker over slice 6\'s span, labelled', Hh.table.entries[m0].start === Hh.table.entries[6].start && Hh.table.entries[m0].len === Hh.table.entries[6].len && Hh.table.entries[m0].flags === HL.EDIT.orange && ed.history.list()[0].label === `marker ${m0} from 6`);
  ed.select(m0);
  const hs = ed.handles(); const endH = hs.find(h => h.kind === 'end');
  check('the selected marker has an end handle at its right edge', endH && endH.t === m0 && endH.pos === ED.endOf(Hh.table.entries[m0]));
  clock += 200; sent.length = 0; const ex = px(endH.pos);
  P('down', 1, ex, 220); clock += 10; P('move', 1, ex + 20, 220); clock += 10; P('move', 1, ex + 25, 220);
  check('dragging the end: the preview plays the last 250 ms up to the new end (flags from-end)', sent.some(b => b[0] === 27 && (b[7] & HL.EDIT.fromEnd)) && ed.drag.h.kind === 'end');
  clock += 10; P('up', 1, ex + 25, 220); await settle();
  const grown = Hh.table.entries[m0];
  check('release: SET_ENTRY with the start fixed and the end moved (+25 px = +8000; no onset within 12 ms), "end N +… ms"', sent.some(b => b[0] === 17 && b[1] === m0) && grown.start === Hh.table.entries[6].start && ED.endOf(grown) === 7 * SL + 8000 && /^end \d+ \+/.test(ed.history.list()[0].label));
  /* press-and-hold the end handle without moving: a loop audition until the lift */
  clock += 200; sent.length = 0; const ex2 = px(ED.endOf(grown));
  P('down', 1, ex2, 220); clock += 100; fire();
  check('hold an end handle: AUDITION(from-end | loop) of the tail', ed.looping && sent.some(b => b[0] === 27 && b[7] === (HL.EDIT.fromEnd | HL.EDIT.loop)));
  P('up', 1, ex2, 220);
  check('lift: RELEASE, no edit', !ed.looping && sent.some(b => b[0] === 28) && !sent.some(b => b[0] === 17) && ed.stats.releases === 1);
  /* a tap inside the marker splits it at the (snapped) point; the grid never splits */
  sent.length = 0; const mid = px(grown.start + grown.len / 2);
  P('down', 1, mid, 100); clock += 30; P('up', 1, mid, 100); await settle();
  const m1 = Hh.table.entries.length - 1;
  check('tap in a marker: SPLIT with the gen, the right half is a new orange marker, "split N → M"', sent.some(b => b[0] === 19 && b[1] === m0 && b.length === 7) && Hh.table.entries[m1].start === ED.endOf(Hh.table.entries[m0]) && Hh.table.entries[m1].flags === HL.EDIT.orange && ed.history.list()[0].label === `split ${m0} → ${m1}`);
  sent.length = 0; const gx = px(2 * SL + SL / 2);
  P('down', 1, gx, 100); clock += 30; P('up', 1, gx, 100); await settle();
  check('tap in a grid slice: selected, nothing sent but its audition', ed.selT === 2 && !sent.some(b => b[0] === 19) && sent.filter(b => b[0] === 27).length === 0);
  /* double-tap the right half's handle merges it with the adjacent left half */
  sent.length = 0; ed.select(m1); const sx = px(Hh.table.entries[m1].start);
  P('down', 1, sx, 220); clock += 20; P('up', 1, sx, 220); clock += 50; P('down', 1, sx, 220); clock += 20; P('up', 1, sx, 220); await settle();
  check('double-tap a marker handle: MERGE (adjacent), the other half a tombstone, "merge A into B"', sent.some(b => b[0] === 20 && b[1] === m1) && ED.isTomb(Hh.table.entries[m0]) && Hh.table.entries[m1].start === Hh.table.entries[6].start && ed.history.list()[0].label === `merge ${m0} into ${m1}`);
  check('merge is refused by the page when nothing is adjacent', ed.merge(m1) === null && /adjacent|nothing/.test(Hh.status[Hh.status.length - 1]) && !sent.filter(b => b[0] === 20).slice(1).length);
  /* the long-press menu */
  Hh.menus.length = 0; const sx2 = px(Hh.table.entries[m1].start);
  P('down', 1, sx2 + 2, 220); clock += 100; fire();
  check('long-press a marker handle: the menu (audition · promote · copy · delete; no merge — nothing adjacent)', Hh.menus.length === 1 && Hh.menus[0].t === m1 && Hh.menus[0].items.map(i => i.id).join() === 'audition,promote,copy,delete');
  P('up', 1, sx2 + 2, 220); await settle();
  check('the lift after a hold is neither a tap nor a split', Hh.table.entries.length === m1 + 1);
  Hh.menus.length = 0; P('down', 1, gx, 100); clock += 100; fire(); P('up', 1, gx, 100);
  check('long-press a grid slice: copy out only', Hh.menus.length === 1 && Hh.menus[0].items.map(i => i.id).join() === 'audition,copy');
  /* promote: the marker's start onto the nearest grid boundary */
  const r2 = await ed.copyOut(9); await settle(); const m2 = r2.newIdx; ed.select(m2);
  check('copy out reuses the tombstone\'s slot', m2 === m0);
  await ed.typed('start', String(9 * SL + 1000)); await settle();
  check('typed start (samples): SET_ENTRY, the end kept', Hh.table.entries[m2].start === 9 * SL + 1000 && ED.endOf(Hh.table.entries[m2]) === 10 * SL);
  ed.unit = 'ms'; await ed.typed('end', String((10 * SL + 2400) / 48)); await settle();
  check('typed end in ms: the start kept, the end moved', Hh.table.entries[m2].start === 9 * SL + 1000 && ED.endOf(Hh.table.entries[m2]) === 10 * SL + 2400 && ed.fieldValues().endS === '1300 ms' && ed.fieldValues().lenS === '154 ms');
  ed.unit = 'samples';
  await ed.promote(m2); await settle();
  check('promote: SET_BOUNDARY(9) to the marker\'s start, orange', sent.some(b => b[0] === 25 && b[1] === m2 && b[2] === 9) && Hh.table.entries[9].start === 9 * SL + 1000 && (Hh.table.entries[9].flags & HL.EDIT.orange));
  check('typed garbage: nothing sent, a status line', await ed.typed('start', 'zzz') === false && /unreadable/.test(Hh.status[Hh.status.length - 1]));
  /* delete from the menu: keys on it re-point; the tombstone has no handle and is not drawn */
  await ed.setKey(5, m2); await settle();
  check('SET_KEY from the editor', Hh.st.keySlice[5] === m2 && ed.history.list()[0].label === `F1 → ${m2}`);
  const del = ed.menuItems(m2).find(i => i.id === 'delete'); await del.run(); await settle();
  check('delete = TOMBSTONE with the gen: key 5 back to its span, no handle for it', ED.isTomb(Hh.table.entries[m2]) && Hh.st.keySlice[5] === ED.spanSlice(5, 32) && !ed.handles().some(h => h.t === m2) && sent.some(b => b[0] === 21 && b[1] === m2 && b.length === 3));
  /* a stale gen: the module moved on behind the page; the page refetches and retries once */
  const r3 = await ed.copyOut(12); await settle(); const m3 = r3.newIdx;
  d.bumpGen(); sent.length = 0; Hh.status.length = 0;
  const rs = await ed.tombstone(m3); await settle();
  check('stale: BAD_STATE → refetch → retried with the fresh gen → OK, said so', rs && rs.ok && ed.stats.retried === 1 && sent.filter(b => b[0] === 21).length === 2 && Hh.status.some(s => /stale/.test(s)) && /after a refetch/.test(Hh.status[Hh.status.length - 1]) && ED.isTomb(Hh.table.entries[m3]));
  /* a take in flight: refused, explained, the handle reverts */
  const r4 = await ed.copyOut(13); await settle(); const m4 = r4.newIdx; ed.select(m4);
  d.rec = { state: 1, key: 2, t: Date.now(), pos: 0 }; sent.length = 0; Hh.status.length = 0;
  const sx4 = px(Hh.table.entries[m4].start);
  P('down', 1, sx4, 220); clock += 10; P('move', 1, sx4 + 15, 220); clock += 10; P('up', 1, sx4 + 15, 220); await settle();
  check('a take in flight: SET_ENTRY refused (BAD_STATE), the reason names the take, the handle reverts, no history', /take is in flight \(armed\)/.test(Hh.status[Hh.status.length - 1]) && Hh.table.entries[m4].start === 13 * SL && ed.drag === null && !/^start/.test(ed.history.list()[0].label) && ed.stats.refused === 1);
  d.rec = { state: 0, key: -1, t: 0, pos: 0 }; await refetch();
  /* keys: drag a marker's span onto a key cell, cell-to-cell swaps, the red rule, copy to a second key */
  Hh.dropKey = 5; sent.length = 0; const mx4 = px(Hh.table.entries[m4].start + 2000);
  P('down', 1, mx4, 100); clock += 10; P('move', 1, mx4 + 30, 120); P('move', 1, mx4 + 60, 140);
  check('dragging a slice from the wave: a ghost to the key cells, no pan', ed.drag && ed.drag.kind === 'toKey' && ed.drag.t === m4 && Hh.view.a === 0);
  P('up', 1, mx4 + 60, 140); await settle();
  check('dropped on an unoccupied key: SET_KEY(F1, marker)', Hh.st.keySlice[5] === m4 && sent.some(b => b[0] === 22 && b[1] === 5 && b[2] === m4));
  Hh.plans.length = 0; Hh.answer = null; await ed.dropOnKey({ key: 7 }, 3); await settle();
  check('cell 7 (its recording) onto key 3 (its recording, alone): red, default swap → SWAP_KEYS, both recordings keep a key', Hh.plans.length === 1 && Hh.plans[0].red && Hh.plans[0].def === 'swap' && Hh.st.keySlice[3] === 33 && Hh.st.keySlice[7] === 32 && Hh.table.entries[33].key === 3 && Hh.table.entries[32].key === 7);
  Hh.answer = 'cancel'; const before = Array.from(Hh.st.keySlice).join(); sent.length = 0; await ed.dropOnKey({ t: m4 }, 3); await settle();
  check('cancel leaves everything', Array.from(Hh.st.keySlice).join() === before && !sent.some(b => b[0] === 22 || b[0] === 23));
  Hh.answer = 'replace'; await ed.dropOnKey({ t: m4 }, 3); await settle();
  check('replace: SET_KEY on the recording\'s key (the choice was red — the user said so)', Hh.st.keySlice[3] === m4 && Hh.table.entries[33].key === 3);
  Hh.answer = null; await ed.dropOnKey({ key: 5 }, 6); await settle();
  check('cell onto an empty cell: SWAP_KEYS', Hh.st.keySlice[6] === m4 && Hh.st.keySlice[5] === ED.spanSlice(6, 32));
  await ed.setKey(8, m4); await settle();
  check('copy to another key: many keys on one entry', Hh.st.keySlice[8] === m4 && Hh.st.keySlice[6] === m4 && Hh.st.keySlice[3] === m4);
  const nf = await ed.keysFollowGrid(); await settle();
  check('keys follow the grid: every key off its span goes back (the recorded ones too — the design\'s span map), one SET_KEY each', nf === 6 && Array.from(Hh.st.keySlice).every((t, k) => t === ED.spanSlice(k, 32)) && ed.history.list()[0].label === 'keys follow the grid');
  /* the onset histogram preview matches FROM_ONSETS; nudge; tap mark with the option's allowance; undo on this firmware */
  const bars2 = ED.onsetBars(Hh.hits, Hh.table), prev = ED.onsetAccept(bars2, 210, 35 * 48, Hh.st.bufLen);
  const ro = await ed.fromOnsets(210); await settle();
  check('FROM_ONSETS(210, 35 ms): the preview\'s count is the module\'s, grey markers made, labelled', ro.ok && ro.count === prev.count && prev.count >= 6 && Hh.table.entries.filter((e, i) => i >= Hh.table.nSlices && ED.isLive(e) && !ED.isOrange(e) && e.key < 0 && e.start < Hh.st.bufLen).length === ro.count && ed.history.list()[0].label === `onsets ≥ 82 % → ${ro.count}`);
  const b5 = Hh.table.entries[5].start; await ed.nudge(5, HL.EDIT.grid); await settle();
  check('nudge all +5 ms (grid)', Hh.table.entries[5].start === b5 + 240 && ed.history.list()[0].label === 'nudge grid +5 ms');
  d.srcPos = () => 10 * SL + 1000; sent.length = 0; const rt = await ed.tapMark(); await settle();   /* 10 SL: a snare, no marker there (the kicks have them) */
  check('the Chop pad: TAP_MARK(60, 40 ms from the option), gen unchecked, an orange marker, "tap → marker N"', rt.ok && sent.some(b => b[0] === 29 && b[1] === 60 && b[2] === 40 && b.length === 3) && Hh.table.entries[rt.newIdx].flags === HL.EDIT.orange && ed.history.list()[0].label === `tap → marker ${rt.newIdx}`);
  Hh.status.length = 0; const nBefore = Hh.table.nTable; const ru = await ed.undo(); await settle(); ed.noteState(Hh.st);
  check('undo through the editor: the tap marker is gone, the label is struck through, the tail says redo 1', ru.ok && Hh.table.nTable === nBefore - 1 && ed.history.list()[0].undone === true && /tap → marker/.test(ed.history.list()[0].label) && Hh.st.redoDepth === 1 && ed.history.redoDepth === 1);
  const rr = await ed.redo(); await settle(); ed.noteState(Hh.st);
  check('redo through the editor: the marker is back, the mark cleared', rr.ok && Hh.table.nTable === nBefore && ed.history.list()[0].undone === false && Hh.st.redoDepth === 0);
  { const c0 = ed.stats.commits; const p1 = ed.copyOut(14); const p2 = ed.copyOut(15); const r2 = await p2; await p1; await settle(); await refetch();
    check('one edit at a time: a second gesture op while one is in flight is turned away, the first lands', r2 === null && Hh.status.some(s => /one edit at a time/.test(s)) && Hh.table.entries.some(e => ED.isLive(e) && e.start === Hh.table.entries[14].start && e.len === Hh.table.entries[14].len && ED.isOrange(e)) && ed.stats.commits === c0 + 1); }
  { const before = Hh.st.keySlice[11]; const q1 = ed.setKey(11, 4); const q2 = ed.setKey(11, 6); const r = await q2; await q1; await settle();
    check('a panel op queues behind a pending reply instead: both SET_KEYs land in order', r && r.ok && Hh.st.keySlice[11] === 6 && before !== 6); }
  /* history from the tail: a change the page did not make gets the module's label */
  d.recordKey(9, 0.2, 1); await refetch(); ed.noteState(Hh.st);
  check('a take on the module: "changed on the module" from tableGen', ed.history.list()[0].label === 'changed on the module' && ed.history.undoDepth === 0);
  /* the strip: pan, vertical drag = zoom, pinch, double-tap = fit; the wheel */
  Hh.view.a = 0; Hh.view.b = Hh.st.bufLen; const span0 = Hh.st.bufLen;
  P('down', 1, 300, 260); clock += 10; P('move', 1, 330, 262); P('up', 1, 330, 262);
  check('strip drag = pan (the view slides the other way, clamped by the page)', Hh.view.a === -Math.round(30 / 600 * span0) && Hh.view.b - Hh.view.a === span0);
  Hh.view.a = 0; Hh.view.b = span0; P('down', 1, 300, 260); clock += 10; P('move', 1, 302, 300); P('up', 1, 302, 300);
  check('strip vertical drag (down) = zoom in around the finger', Hh.view.b - Hh.view.a < span0 * 0.8 && Hh.view.a > 0);
  Hh.view.a = 0; Hh.view.b = span0; P('down', 1, 300, 260); P('down', 2, 400, 260); P('move', 2, 420, 260); P('move', 2, 500, 260); P('up', 2, 500, 260); P('up', 1, 300, 260);
  check('pinch = zoom', Hh.view.b - Hh.view.a < span0);
  ed.select(4); Hh.fit = null; P('down', 1, 100, 260); clock += 20; P('up', 1, 100, 260); clock += 50; P('down', 1, 100, 260); clock += 20; P('up', 1, 100, 260);
  check('strip double-tap = fit the selected slice', Hh.fit === 4);
  Hh.view.a = 0; Hh.view.b = span0; ed.wheel(300, W, -100); check('wheel up = zoom in', Hh.view.b - Hh.view.a < span0);
  await l.close();
}

/* ── 10. Descriptor parity: demo.js against the firmware's own source. The fake is only worth its tests while it
 *        says what the module says, and it had drifted far (five grammars of seven, an 18-byte Options block of 24,
 *        every default 0). This reads src/audiothurgist.cpp — the Options block's Describe (ids, names, offsets,
 *        zones, the enum labels from each k…Disp string), its size and schema hash, the live defaults (Serialize's
 *        byte → struct, the struct's initialiser), the jacks, the pager's knobs and the descriptor root (the "api" the
 *        page checks: the firmware's, the fake's and HL.API agree) — and fails on any difference. String parsing,
 *        deliberately: no build step. ── */
function firmwareDescriptor(cpp) {
  const unC = lit => lit.replace(/\\(["\\])/g, '$1');
  const disp = {};
  for (const m of cpp.matchAll(/static\s+const\s+char\s*\*\s*(k\w+Disp)\s*=\s*(?:\/\*[\s\S]*?\*\/\s*)?((?:"(?:[^"\\]|\\.)*"\s*)+);/g))
    disp[m[1]] = JSON.parse(unC([...m[2].matchAll(/"((?:[^"\\]|\\.)*)"/g)].map(x => x[1]).join('')));
  const a = cpp.indexOf('struct Options : Serializable'), b = cpp.indexOf('static Options gOptions;');
  const block = a >= 0 && b > a ? cpp.slice(a, b) : '';
  const fields = [...block.matchAll(/w\.Field\("([^"]+)",\s*"([^"]+)",\s*(\d+),\s*FieldType::Enum,\s*([\d.]+)f,\s*(k\w+Disp),\s*(\d+)u/g)]
    .map(m => ({ id: m[1], name: m[2], off: +m[3], def: +m[4], disp: m[5], zones: +m[6], labels: disp[m[5]] ? disp[m[5]].labels : null }));
  const size = +((block.match(/kBytes\s*=\s*(\d+)u/) || [])[1]);
  const hash = parseInt((block.match(/SchemaHash\(\)\s*const\s*override\s*\{\s*return\s*(0x[0-9A-Fa-f]+)u/) || [])[1], 16);
  /* the live defaults: Serialize says which struct holds byte i; its declaration's `= {N}` (or the struct's member initialiser) the value */
  const defaults = [];
  for (const m of block.matchAll(/o\[(\d+)\]\s*=\s*(g\w+)\.(sel|mode)/g)) {
    const decl = cpp.match(new RegExp('static\\s+(\\w+)\\s+' + m[2] + '\\s*(?:=\\s*\\{\\s*(\\d+)\\s*\\})?\\s*;'));
    let v = decl && decl[2] != null ? +decl[2] : null;
    if (v == null && decl) { const st = cpp.match(new RegExp('struct\\s+' + decl[1] + '\\s*\\{[^}]*?\\b' + m[3] + '\\s*=\\s*(\\d+)')); v = st ? +st[1] : 0; }
    defaults[+m[1]] = v;
  }
  const jacks = [...cpp.matchAll(/^Jack\s+jk\d+\s*\(\s*"([^"]+)",\s*"([^"]+)"/gm)].map(m => ({ id: m[1], name: m[2] }));
  const knob = {}; for (const m of cpp.matchAll(/VirtualKnob\s+(\w+)\s*=\s*VirtualKnob\(\w+,\s*"([^"]+)"\)[\s\S]*?\.Ident\("([^"]+)"\)/g)) knob[m[1]] = { name: m[2], id: m[3] };
  const pages = [...cpp.matchAll(/Page\s+\w+\s*=\s*Page\((\d+)\)\.Name\("([^"]+)"\)[\s\S]*?\.Knobs\(([^)]*)\)/g)].map(m => ({ page: +m[1], name: m[2], knobs: m[3].split(',').map(x => knob[x.trim()] || null) }));
  const root = cpp.match(/return\s+"(\\"audiothurgist\\":\{(?:[^"\\]|\\.)*\})";/);
  const rootObj = root ? JSON.parse('{' + unC(root[1]) + '}').audiothurgist : null;
  return { fields, size, hash, defaults, jacks, pages, root: rootObj };
}
function parityDiffs(fw, demoDesc, demoDefaults) {
  const out = [], opt = demoDesc.components.find(c => c.id === 'options'), pager = demoDesc.components.find(c => c.kind === 'pager');
  if (!fw.fields.length) out.push('no Options fields parsed from the firmware');
  if (opt.size !== fw.size) out.push(`Options size: demo ${opt.size}, firmware ${fw.size}`);
  if (opt.hash !== fw.hash) out.push(`Options schema hash: demo 0x${opt.hash.toString(16)}, firmware 0x${(fw.hash || 0).toString(16)}`);
  if (opt.fields.length !== fw.fields.length) out.push(`Options fields: demo ${opt.fields.length}, firmware ${fw.fields.length}`);
  fw.fields.forEach((f, i) => {
    const d = opt.fields.find(x => x.id === f.id);
    if (!d) { out.push(`field ${f.id}: missing from the demo`); return; }
    for (const k of ['name', 'off', 'zones', 'def']) if (d[k] !== f[k]) out.push(`field ${f.id}.${k}: demo ${JSON.stringify(d[k])}, firmware ${JSON.stringify(f[k])}`);
    if (!f.labels) out.push(`field ${f.id}: the firmware's ${f.disp} did not parse`);
    else if (JSON.stringify(d.disp.labels) !== JSON.stringify(f.labels)) out.push(`field ${f.id} labels: demo ${JSON.stringify(d.disp.labels)}, firmware ${JSON.stringify(f.labels)}`);
    if (f.labels && f.labels.length !== f.zones) out.push(`field ${f.id}: the firmware's own zones ${f.zones} ≠ its ${f.labels.length} labels`);
    if (opt.fields[i] && opt.fields[i].id !== f.id) out.push(`field order at ${i}: demo ${opt.fields[i].id}, firmware ${f.id}`);
    if (demoDefaults[f.off] !== fw.defaults[f.off]) out.push(`live default of ${f.id} (byte ${f.off}): demo ${demoDefaults[f.off]}, firmware ${fw.defaults[f.off]}`);
  });
  if (JSON.stringify(demoDesc.jacks.map(j => [j.id, j.name])) !== JSON.stringify(fw.jacks.map(j => [j.id, j.name]))) out.push('jacks: ' + JSON.stringify(demoDesc.jacks.map(j => j.id)) + ' vs ' + JSON.stringify(fw.jacks.map(j => j.id)));
  for (const pg of fw.pages) {
    if (pager.pageNames[pg.page] !== pg.name) out.push(`page ${pg.page}: demo ${pager.pageNames[pg.page]}, firmware ${pg.name}`);
    pg.knobs.forEach((k, pot) => { const d = pager.fields.find(f => f.page === pg.page && f.pot === pot); if (!k || !d || d.id !== k.id || d.name !== k.name) out.push(`knob ${pg.page}:${pot}: demo ${d && d.id + '/' + d.name}, firmware ${k && k.id + '/' + k.name}`); });
  }
  /* the descriptor root: the one number the page checks on connect */
  if (!fw.root) out.push('the descriptor root (DescriptorRootJson) did not parse');
  else {
    if (JSON.stringify(fw.root) !== JSON.stringify(demoDesc.audiothurgist)) out.push(`descriptor root: demo ${JSON.stringify(demoDesc.audiothurgist)}, firmware ${JSON.stringify(fw.root)}`);
    if (fw.root.api !== HL.API) out.push(`api: firmware ${fw.root.api}, the page (HL.API) ${HL.API}`);
  }
  return out;
}
{
  const fArg = process.argv.indexOf('--fw-src'), cppPath = fArg > 0 ? path.resolve(process.argv[fArg + 1]) : path.resolve(here, '..', 'src', 'audiothurgist.cpp');
  if (!existsSync(cppPath)) console.log('parity: skipped (no ' + cppPath + ')');
  else {
    const cpp = readFileSync(cppPath, 'utf8'), fw = firmwareDescriptor(cpp), dd = DEMO.buildDescriptor(), defs = DEMO.OPTION_FIELDS.map(f => f[3]);
    check('parity: the firmware source parses — 25 Options fields with labels, size, hash, defaults, ten jacks, two pages of six knobs, the root',
      fw.fields.length === 25 && fw.fields.every(f => Array.isArray(f.labels)) && fw.size === 25 && fw.hash === 0x4F505439 && fw.defaults.length === 25 && fw.defaults.every(v => Number.isInteger(v)) && fw.jacks.length === 10 && fw.pages.length === 2 && fw.pages.every(p => p.knobs.length === 6 && p.knobs.every(Boolean)) && !!fw.root,
      JSON.stringify({ n: fw.fields.length, size: fw.size, hash: fw.hash, d: fw.defaults, j: fw.jacks.length, p: fw.pages.length, root: fw.root }).slice(0, 300));
    const diffs = parityDiffs(fw, dd, defs);
    check('parity: demo.js = the firmware (fields, labels, offsets, zones, defaults, jacks, knobs, the root\'s api = the page\'s)', diffs.length === 0, diffs.slice(0, 6).join(' · '));
    const live = new DEMO.FakeDevice({ latencyMs: 1 }).options, want = fw.defaults.slice(); want[DEMO.O['start.in']] = 1;
    check('parity: the fake\'s live blob starts at the firmware\'s defaults (Start In As saved: the fake\'s default boot)', Array.from(live).join() === want.join());
    /* the check must bite: one changed label, one moved byte, one default, one missing jack, another api each fail it */
    const bite = [['Zoom (read on, BreakFix)', 'Zoom (read on)'], ['"quantize", "Quantize", 21', '"quantize", "Quantize", 22'], ['static Sel gTail = {3};', 'static Sel gTail = {2};'], ['Jack jk10("out_r"', 'Jack jk10("out_x"'], ['\\"api\\":' + HL.API + '}', '\\"api\\":' + (HL.API + 1) + '}']]
      .map(([x, y]) => cpp.includes(x) ? parityDiffs(firmwareDescriptor(cpp.replace(x, y)), dd, defs).length : -1);
    check('parity: the check bites — a changed label, a moved byte, a changed default, a renamed jack and another api each fail it', bite.every(n => n > 0), JSON.stringify(bite));
  }
}

/* ── 9. EDIT-UI, the page itself: index.html's inline script under a DOM stub, connected to the fake module through
 *       `?demo=kit`, then the DOM paths driven with synthetic pointer events — the handle drag from the canvas, the
 *       key cells' tap / drag / swap prompt, the Chop pad, the field ── */
import vm from 'node:vm';
const ED9 = require('./edit.js');
/* The page's colour tokens, per theme, from its own <style> (the stub's getComputedStyle and the contrast check) */
function cssThemes(html) {
  const css = html.slice(html.indexOf('<style>'), html.indexOf('</style>'));
  const block = re => { const m = css.match(re); return m ? Object.fromEntries([...m[1].matchAll(/--([\w-]+):\s*([^;}]+)/g)].map(x => [x[1], x[2].trim()])) : {}; };
  const dark = block(/:root\{([^}]*)\}/);
  return { css, dark, light: Object.assign({}, dark, block(/:root\[data-theme="light"\]\{([^}]*)\}/)), lightMedia: Object.assign({}, dark, block(/:root:not\(\[data-theme="dark"\]\)\{([^}]*)\}/)) };
}
function domStub(html) {
  const ids = new Set([...html.matchAll(/ id="([^"]+)"/g)].map(m => m[1]));
  const errors = [], warns = [], unknownIds = new Set(), themes = cssThemes(html), store = new Map();
  class El {
    constructor(tag, id) { this._attr = {}; this._log = { text: [], arcs: 0, rects: 0, fills: [] }; this.tagName = String(tag).toUpperCase(); this.id = id || ''; this.className = ''; this.style = {}; this.dataset = {}; this.hidden = false; this.disabled = false; this.value = ''; this._text = ''; this._html = ''; this._on = {}; this.children = []; this._q = new Map(); this._parent = null; this._calls = 0; this.offsetWidth = 200; this.offsetHeight = 100; this.width = 0; this.height = 0; this._w = 600; this._h = 290; this.checked = false; this.title = ''; this.textContentSet = 0; }
    get classList() { const el = this; return { add: (...c) => { for (const x of c) if (!el.classList.contains(x)) el.className = (el.className + ' ' + x).trim(); }, remove: (...c) => { el.className = el.className.split(/\s+/).filter(x => x && !c.includes(x)).join(' '); }, toggle: (x, on) => { const has = el.classList.contains(x); const want = on == null ? !has : !!on; if (want && !has) el.classList.add(x); if (!want && has) el.classList.remove(x); return want; }, contains: x => el.className.split(/\s+/).includes(x) }; }
    get textContent() { return this._text; } set textContent(v) { this._text = String(v); this.textContentSet++; }
    get innerHTML() { return this._html; } set innerHTML(v) { this._html = String(v); this.children.length = 0; this._q.clear(); }
    addEventListener(t, f) { (this._on[t] = this._on[t] || []).push(f); } removeEventListener() {}
    appendChild(c) { c._parent = this; this.children.push(c); return c; } remove() { if (this._parent) this._parent.children = this._parent.children.filter(c => c !== this); }
    querySelector(sel) { if (!this._q.has(sel)) { const c = new El(sel.startsWith('.') ? 'button' : sel, ''); c.className = sel.replace(/^\./, ''); c._parent = this; this._q.set(sel, c); } return this._q.get(sel); }
    querySelectorAll() { return []; }
    contains(x) { for (let e = x; e; e = e._parent) if (e === this) return true; return false; }
    closest(sel) { for (let e = this; e; e = e._parent) if (e.classList.contains(sel.replace(/^\./, ''))) return e; return null; }
    getBoundingClientRect() { return { left: 0, top: 0, width: this._w, height: this._h, right: this._w, bottom: this._h }; }
    setPointerCapture() {} releasePointerCapture() {} focus() {} blur() {}
    setAttribute(k, v) { this._attr[k] = String(v); } removeAttribute(k) { delete this._attr[k]; } getAttribute(k) { return k in this._attr ? this._attr[k] : null; }
    /* The NaN-draw watch (Kyklophoria): arc(NaN, …) is not an error on any canvas, it draws nothing,
     * indistinguishable from an off-screen mark. Every geometry call is checked for finite numbers. */
    /* The draw log (Kyklophoria's pagecheck): what the last frame drew — its text with the fill it was drawn in, its
     * arcs, its rects — reset by a full clearRect, so a check can assert what is on the canvas, not that something was. */
    getContext() { const el = this; return new Proxy({}, { get: (o, k) => { if (k === 'measureText') return () => ({ width: 10 }); if (typeof k === 'string' && k in o) return o[k]; return (...a) => { el._calls++; if (GEOM.has(k)) for (const v of a) if (typeof v === 'number' && !Number.isFinite(v)) { nanDraws.push(k + '(' + a.map(x => String(x)).join(',') + ')'); }
      if (k === 'clearRect' && a[0] === 0 && a[1] === 0) el._log = { text: [], arcs: 0, rects: 0, fills: [] };
      else if (k === 'fillText') el._log.text.push(String(a[0])); else if (k === 'arc') el._log.arcs++; else if (k === 'fillRect' || k === 'strokeRect') { el._log.rects++; if (el._log.fills.length < 4000) el._log.fills.push(String(o.fillStyle)); } }; }, set: (o, k, v) => { o[k] = v; return true; } }); }
  }
  const byId = new Map();
  const document = {
    /* the unknown-element-id trap: an id the markup does not have is a typo or a stale reference — recorded (a page
     * catch could swallow the throw) and thrown */
    getElementById: id => { if (!ids.has(id)) { unknownIds.add(id); throw new Error('no element with id ' + id); } if (!byId.has(id)) byId.set(id, new El(/canvas id="/.test(html.slice(html.indexOf(`id="${id}"`) - 12, html.indexOf(`id="${id}"`))) ? 'canvas' : 'div', id)); return byId.get(id); },
    createElement: tag => new El(tag), addEventListener: () => {}, removeEventListener: () => {}, querySelectorAll: () => [], visibilityState: 'visible', activeElement: null, _at: null,
    elementFromPoint: () => document._at, documentElement: new El('html'), querySelector: () => null,
  };
  /* getComputedStyle on the root: the token of the theme the page set (data-theme), else the dark set */
  const getComputedStyle = () => ({ getPropertyValue: n => { const t = document.documentElement.getAttribute('data-theme') === 'light' ? themes.light : themes.dark; return t[n.replace(/^--/, '')] || ''; } });
  const localStorage = { getItem: k => store.has(k) ? store.get(k) : null, setItem: (k, v) => { store.set(k, String(v)); }, removeItem: k => { store.delete(k); } };
  const matchMedia = q => ({ matches: false, media: q, addEventListener() {}, removeEventListener() {} });
  const sandbox = { document, console: { log: () => {}, warn: (...a) => warns.push(a.join(' ')), error: (...a) => errors.push(a.join(' ')) }, errors, warns,
    navigator: { userAgent: 'node', usb: undefined, serial: undefined }, location: { search: '', hash: '', protocol: 'http:', href: 'http://localhost/' }, history: { replaceState: () => {} },
    performance, TextDecoder, TextEncoder, ReadableStream, WritableStream, Uint8Array, Int8Array, Uint32Array, Float32Array, DataView, Promise, Map, Set, Math, JSON, Number, String, Array, Object, Error, Date, isNaN, parseInt, parseFloat, RegExp, Symbol,
    setTimeout: (f, ms) => setTimeout((...a) => { try { f(...a); } catch (e) { errors.push('timer: ' + (e.stack || e)); } }, ms), clearTimeout, setInterval: (f, ms) => { const t = setInterval(() => { try { f(); } catch (e) { errors.push('interval: ' + (e.stack || e)); } }, ms); t.unref(); return t; }, clearInterval,
    requestAnimationFrame: f => setTimeout(() => { try { f(performance.now()); } catch (e) { errors.push('rAF: ' + (e.stack || e)); } }, 16),
    confirm: () => { sandbox.confirms++; return sandbox.confirmAnswer; }, confirms: 0, confirmAnswer: true, alert: () => {}, isSecureContext: true, devicePixelRatio: 1, addEventListener: () => {}, HL, DEMO, ED: ED9,
    getComputedStyle, localStorage, matchMedia };
  sandbox.window = sandbox; sandbox.globalThis = sandbox;
  return { sandbox, errors, warns, El, byId, unknownIds, themes };
}
const fireEv = (el, type, ev) => { const e = Object.assign({ preventDefault() {}, stopPropagation() {}, target: el, currentTarget: el, pointerId: 1, clientX: 0, clientY: 0, key: '' }, ev); for (const f of el._on[type] || []) f(e); if (typeof el['on' + type] === 'function') el['on' + type](e); };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const until = async (f, ms = 3000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (f()) return true; await sleep(20); } return f(); };
async function loadPage(search) {
  const html = readFileSync(path.join(here, 'index.html'), 'utf8');
  const script = [...html.matchAll(/<script>\n([\s\S]*?)<\/script>/g)].pop()[1];
  const D = domStub(html); D.sandbox.location.search = search; const ctx = vm.createContext(D.sandbox);
  vm.runInContext(script, ctx, { filename: 'index.html' });
  const G = expr => vm.runInContext(expr, ctx);
  return { D, ctx, G, html };
}
{
  const { D, G } = await loadPage('?demo=kit');
  check('page: the inline script runs to the end under the stub (no throw), the Kit tab is on', D.errors.length === 0 && G('kitShown') === true, D.errors.join(' | ').slice(0, 300));
  check('page: the table, the hits and the state arrive from the fake', await until(() => G('table && hits && st && st.srcPos != null')), D.errors.concat(D.warns).join(' | ').slice(0, 300));
  await sleep(150);
  check('page: connected, the editor is on — the panel, the field row, the status pill, the 290 px canvas; the table carries its flags', !G("$('editor').hidden") && !G("$('ed_field').hidden") && G("$('kitwave').style.height") === '290px' && G('table.entries[0].flags') === 0 && G('demoDevice.log').includes(HL.CMD.table));
  check('page: the Tap Allowance option is read from the live blob (40 ms)', G('tapAllowMs()') === 40 && G("$('chop_allow').textContent") === '40 ms');
  check('page: the kit waveform, the histogram and the cells drew without a runtime error', G("$('kitwave')._calls") > 100 && G("$('onsethist')._calls") > 10 && G('keyCells[3].el.className').includes('rec') && D.errors.length === 0, D.errors.join(' | ').slice(0, 300));
  /* a handle drag on the canvas: boundary 5, −15 px */
  const SL = DEMO.SLICE_LEN; G('view.a = 0; view.b = st.bufLen'); await sleep(40);
  const s5 = G('demoDevice.table[5].start'), kw = G("$('kitwave')"), x5 = G('demoDevice.table[5].start / st.bufLen * 600');   /* the kit demo swings: slice 5 opens late */
  const posted0 = G('demoDevice.auditionPosted');
  fireEv(kw, 'pointerdown', { clientX: x5, clientY: 222 }); for (let i = 1; i <= 15; i++) fireEv(kw, 'pointermove', { clientX: x5 - i, clientY: 222 });
  check('page: the drag is local (the editor holds it, the fake is untouched)', G('ed.drag && ed.drag.kind') === 'handle' && G('ed.drag.pos') === s5 - 4800 && G('demoDevice.table[5].start') === s5);
  fireEv(kw, 'pointerup', { clientX: x5 - 15, clientY: 222 });
  check('page: release → SET_BOUNDARY on the fake, then the page\'s table follows (the refetch), orange, the history line', await until(() => G('demoDevice.table[5].start') === s5 - 4800) && await until(() => G('table.entries[5].start') === s5 - 4800) && G('table.entries[5].flags & 1') === 1 && await until(() => G("$('ed_hist').innerHTML").includes('boundary 5 −100 ms')) && G('demoDevice.auditionPosted') > posted0);
  check('page: the wave windows survived the edit (no cache clear on a table-only change)', G('wave.cache.size') > 0);
  /* the key cells: a tap selects and auditions; cell 7 dragged onto cell 3 (both recordings) → the red prompt → swap */
  const c3 = G('keyCells[3].el'), c7 = G('keyCells[7].el');
  const aud0 = G('demoDevice.auditionPosted');
  fireEv(c3, 'pointerdown', { clientX: 50, clientY: 50 }); await sleep(30); fireEv(c3, 'pointerup', { clientX: 50, clientY: 50 });
  check('page: tap a cell → selected key and entry, the cell pulses, one audition', G('selKey') === 3 && G('ed.selT') === 32 && await until(() => G('keyCells[3].el.className').includes('pulse')) && await until(() => G('demoDevice.auditionPosted') === aud0 + 1));
  D.sandbox.document._at = c3;
  fireEv(c7, 'pointerdown', { clientX: 300, clientY: 50 }); fireEv(c7, 'pointermove', { clientX: 280, clientY: 52 }); fireEv(c7, 'pointermove', { clientX: 120, clientY: 55 });
  check('page: dragging a cell shows the ghost and lights the target red (the recording would be orphaned)', !G("$('ed_ghost').hidden") && G("$('ed_ghost').textContent").includes('swap') && G('keyCells[3].el.className').includes('drop') && G('keyCells[3].el.className').includes('red'));
  fireEv(c7, 'pointerup', { clientX: 120, clientY: 55 }); await sleep(30);
  const menu = G("$('ed_menu')");
  check('page: the prompt opens, red, swap as the default', !menu.hidden && menu.className.includes('red') && menu.children.some(b => b.className === 'primary' && /swap/.test(b.textContent)));
  menu.children.find(b => /swap/.test(b.textContent)).onclick();
  check('page: swap → SWAP_KEYS(7, 3) on the fake, both recordings keep a key', await until(() => G('demoDevice.keySlice[3]') === 33 && G('demoDevice.keySlice[7]') === 32) && await until(() => G('st.keySlice[3]') === 33) && G("$('ed_menu').hidden") === true && G("$('ed_ghost').hidden") === true);
  /* the Chop pad: TAP_MARK with the option's allowance; a marker lands */
  G('demoDevice.srcPos = () => 8 * ' + SL + ' + 1000'); const n0 = G('demoDevice.nTable');
  fireEv(G("$('chop')"), 'pointerdown', {});
  check('page: Chop → TAP_MARK → an orange marker at the kick (8 SL, the allowance backed off 1920 and the snap found it), the history says so', await until(() => G('demoDevice.nTable') === n0 + 1) && G('demoDevice.table[' + n0 + '].start') === 8 * SL && G('demoDevice.table[' + n0 + '].flags') === 1 && await until(() => G("$('ed_hist').innerHTML").includes('tap → marker')));
  /* the typed field: select the new marker, type an end in ms */
  await until(() => G('table.entries.length') === n0 + 1); G('ed.select(' + n0 + ')'); await sleep(50);
  const mEnd = G('demoDevice.table[' + n0 + '].start + demoDevice.table[' + n0 + '].len');
  check('page: the field shows the selection\'s start and end', G("$('ed_sel').textContent") === `marker ${n0}` && G("$('ed_start').value") === String(8 * SL) && G("$('ed_end').value") === String(mEnd));
  G("$('ed_unit').value = 'ms'"); fireEv(G("$('ed_unit')"), 'change', { target: G("$('ed_unit')") }); await sleep(40);
  check('page: the unit switches the field', G("$('ed_end').value") === ED9.fmtMs(mEnd));
  G("$('ed_end').value = '1150'"); fireEv(G("$('ed_end')"), 'change', { target: G("$('ed_end')") });
  check('page: a typed end commits SET_ENTRY', await until(() => G('demoDevice.table[' + n0 + '].len') === 1150 * 48 - 8 * SL));
  /* the onset panel: the threshold from a pointer on the histogram, Make markers */
  const oh = G("$('onsethist')"); oh._h = 96; fireEv(oh, 'pointerdown', { clientX: 10, clientY: 8 }); fireEv(oh, 'pointerup', {});
  await sleep(40); const thr = G('edThresh');
  check('page: the histogram\'s pointer sets the threshold near the top, the button says how many', thr > 220 && /^Make \d+ marker/.test(G("$('ed_onsets').textContent")));
  const want = +G("$('ed_onsets').textContent").match(/\d+/)[0]; G("$('ed_onsets').onclick()");
  check('page: Make markers → FROM_ONSETS with that count', await until(() => G("$('ed_hist').innerHTML").includes(`→ ${want}`)) && G('demoDevice.table.filter((e, i) => i >= demoDevice.nSlices && e.len > 0 && !(e.flags & 1) && e.key < 0).length') === want);
  /* nudge; Undo enables from the tail's depth, Redo from the redo depth, the list strikes the undone entry; keys follow the grid asks (confirm → true) */
  const b6 = G('demoDevice.table[6].start'); G("$('ed_nudge_p').onclick()");
  check('page: nudge all +5 ms', await until(() => G('demoDevice.table[6].start') === b6 + 240));
  check('page: Undo enabled from undoDepth, Redo disabled while redoDepth is 0, the depth line says both', await until(() => G("$('ed_undo').disabled") === false) && G("$('ed_redo').disabled") === true && /undo \d+ · redo 0/.test(G("$('ed_depth').textContent")));
  G("$('ed_undo').onclick()");
  check('page: Undo → the nudge is gone, Redo enabled, the entry struck through in the list', await until(() => G('demoDevice.table[6].start') === b6) && await until(() => G("$('ed_redo').disabled") === false) && await until(() => /class="undone"/.test(G("$('ed_hist').innerHTML")) && /redo 1/.test(G("$('ed_depth').textContent"))));
  G("$('ed_redo').onclick()");
  check('page: Redo → the nudge is back, nothing struck', await until(() => G('demoDevice.table[6].start') === b6 + 240) && await until(() => !/class="undone"/.test(G("$('ed_hist').innerHTML"))));
  G("$('ed_follow').onclick()");
  check('page: keys follow the grid (confirmed, queued behind the nudge\'s reply) → every key on its span', await until(() => { const ks = G('Array.from(demoDevice.keySlice)'); return ks.every((t, k) => t === ED9.spanSlice(k, 32)); }, 6000));
  check('page: no runtime errors or link warnings through all of it', D.errors.length === 0 && !D.warns.some(w => /table|state|hits/.test(w)), D.errors.concat(D.warns).join(' | ').slice(0, 400));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
  check('page: disconnect resets the editor and hides the panel', !G('link') && G("$('editor').hidden") === true && G('ed.selT') === -1);
}

/* ── 9b. The revamp, the page itself over `?demo` — every check performs the real gesture through the captured
 *        listeners (a click, a pointerdown / up, a change), then asserts what reached the fake module and what the
 *        page drew: the transport, the mode chips, the pads, the kit view in Pads, markers → keys, clear markers,
 *        the autoslice count, the Chop pad's guard, Play's buttons, Flow held, the Options groups, the theme, the
 *        CPU meter, a draw error caught, the unplug — then a 0.12 firmware's fallbacks ── */
const spyActs = G => G('(() => { const d = demoDevice, h = d.handle.bind(d); d.acts = []; d.handle = (t, b) => { if (t === 0x63) d.acts.push(Array.from(b)); return h(t, b); }; return true; })()');
const sentAct = (G, ...pre) => G('demoDevice.acts').some(a => pre.every((v, i) => a[i] === v));
{
  const { D, G } = await loadPage('?demo');
  check('revamp page: loads and connects to the fake — the pads card shown, seven mode chips', await until(() => G('!!(st && table)')) && G("$('padsCard').hidden") === false && G('modeChips.length') === 7 && G('modeChips.filter(c => c.main).length') === 4, D.errors.join(' | ').slice(0, 300));
  spyActs(G); await sleep(150);
  /* the transport */
  check('transport: running shows ■ and "playing", BPM steady to one decimal', G("$('t_play').textContent") === '■' && /playing/.test(G("$('t_sub').innerHTML")) && /^\d+\.\d$/.test(G("$('t_bpm').textContent")));
  G("showTab('tab_kit')");   /* the Chop pad lives on the Kit tab; its state is drawn while the tab shows */
  fireEv(G("$('t_play')"), 'click', {});
  check('transport ▶/■: SET_MUTE(1) on the wire, the fake stops, the button turns to ▶, "stopped"', await until(() => sentAct(G, 35, 1) && G('demoDevice.muted') === true && G("$('t_play').textContent") === '▶' && /stopped/.test(G("$('t_sub').innerHTML"))));
  check('Chop pad guard: stopped → srcValid clear → the pad disabled and the reason shown', await until(() => G('st.srcValid') === false && G("$('chop').disabled") === true && G("$('chop_why').hidden") === false));
  fireEv(G("$('t_play')"), 'click', {});
  check('…▶ again: SET_MUTE(0), running, the Chop pad back', await until(() => sentAct(G, 35, 0) && G('demoDevice.muted') === false && G("$('chop').disabled") === false && G("$('chop_why').hidden") === true));
  G("showTab('tab_play')");
  /* Flow is held */
  const fl = G("$('t_flow')"); fireEv(fl, 'pointerdown', { pointerId: 4 });
  check('Flow held: pointerdown → ACTION 4 on, the button lit', await until(() => sentAct(G, 4, 1) && G('demoDevice.flowing') === true) && fl.className.includes('held'));
  fireEv(fl, 'pointerup', { pointerId: 4 });
  check('…pointerup → ACTION 4 off', await until(() => sentAct(G, 4, 0) && G('demoDevice.flowing') === false));
  /* CPU, jacks */
  check('CPU meter: average and peak from the tail (‰ → %), the bar width set', /^\d+% · max \d+%$/.test(G("$('cpuText').textContent")) && /^\d+%$/.test(G("$('cpuAvg').style.width")));
  check('Jacks card: Classic → the Brain map and why; J4 live as a key with its volts', G("$('jk_map').textContent") === 'Brain map' && /pattern grammar/.test(G("$('jk_why').textContent")) && /key \d+/.test(G("$('jk_key').textContent")) && /V/.test(G("$('jk_mv').textContent")));
  /* the slice view in Classic: grid envelopes, the punch path works */
  G('selSlice = 3'); fireEv(G("$('strip')"), 'click', { clientX: 5 });
  check('Classic: a strip click punches the selected slice (ACTION 3)', await until(() => sentAct(G, 3)));
  /* mode chips → Pads */
  const chip = g => G(`modeChips.find(c => c.g === ${g}).el`);
  fireEv(chip(6), 'click', {});
  check('mode chip Pads: grammar.mode written through the blob, the fake in Pads, the chip lit, the line says Voice map', await until(() => G('demoDevice.grammar') === 6 && G('st.grammar') === 6) && await until(() => chip(6).className.includes('on') && /Voice map/.test(G("$('mode_what').innerHTML")) && G("$('play').className").includes('m-pads')));
  check('Pads: no dice, no flow; the jack map follows (Voice)', await until(() => G("$('t_dice').disabled") === true && G("$('t_flow').disabled") === true && G('st.jackMap') === 1 && G("$('jk_map').textContent") === 'Voice map'));
  await sleep(120);
  const wl = G("$('wave')._log");
  check('Pads: no grid ghosts — the slice view draws the 24 keys (C1 … B2), not the grid', G("$('wave_title').textContent").startsWith('The kit') && wl.text.includes('C1') && wl.text.includes('B2') && wl.text.filter(t => /^[A-G]/.test(t)).length >= 12);
  G('selSlice = 3'); const n3 = G('demoDevice.acts.filter(a => a[0] === 3).length'); fireEv(G("$('strip')"), 'click', { clientX: 5 }); await sleep(80);
  check('Pads: a strip click sends no ACTION 3', G('demoDevice.acts.filter(a => a[0] === 3).length') === n3);
  /* the pads: down / up, several fingers */
  const pad = k => G(`padEls[${k}].el`);
  fireEv(pad(5), 'pointerdown', { pointerId: 11 });
  check('pad F1 down: PLAY_KEY(5, 1), the flash comes back from the tail (keyFlash 5), the pad lit', await until(() => sentAct(G, 32, 5, 1)) && await until(() => G('st.keyFlash') === 5) && pad(5).className.includes('down') && pad(5).className.includes('flash'));
  fireEv(pad(2), 'pointerdown', { pointerId: 12 }); fireEv(pad(9), 'pointerdown', { pointerId: 13 });
  check('multi-touch: three fingers, three pads down at once', pad(2).className.includes('down') && pad(9).className.includes('down') && pad(5).className.includes('down') && await until(() => sentAct(G, 32, 9, 1)));
  for (const [k, id] of [[5, 11], [2, 12], [9, 13]]) fireEv(pad(k), 'pointerup', { pointerId: id });
  check('…each up is PLAY_KEY(k, 0), every pad released', await until(() => sentAct(G, 32, 5, 0) && sentAct(G, 32, 2, 0) && sentAct(G, 32, 9, 0)) && ![2, 5, 9].some(k => pad(k).className.includes('down')));
  G('demoDevice.keyFlash = 17; demoDevice.keySeq = (demoDevice.keySeq + 1) & 255');
  check('a key called elsewhere (CV, the panel) lights its pad', await until(() => pad(17).className.includes('flash')));
  /* the Kit: autoslice → in Pads the keys follow */
  G("showTab('tab_kit')"); await sleep(60);
  G("$('as_k').value = '8'"); fireEv(G("$('as_k')"), 'input', { target: G("$('as_k')") }); fireEv(G("$('as_k')"), 'change', { target: G("$('as_k')") });
  check('autoslice slider → AUTOSLICE(8) with the gen; eight markers; Pads puts them on the keys (keys follow markers shown)', await until(() => sentAct(G, 36, 8, 0)) && await until(() => G('st.nMarkers') === 8 && G('st.keysFollow') === true) && await until(() => G("$('pads_follow').hidden") === false && G("$('mk_follow').hidden") === false && /marker/.test(G('padEls[0].sub.textContent'))));
  { const u0 = G('st.undoDepth'), asK = G("$('as_k')"), n36 = G('demoDevice.acts.filter(a => a[0] === 36).length');
    for (const v of ['9', '10', '11', '12']) { G(`$('as_k').value = '${v}'`); fireEv(asK, 'input', { target: asK }); await sleep(30); }
    fireEv(asK, 'change', { target: asK });
    check('autoslice drag: the values go out live, JOIN (b1) on every send after the first, one undo step, one history line', await until(() => G('st.nMarkers') === 12) && G('demoDevice.acts.filter(a => a[0] === 36).slice(' + n36 + ')').every((a, i) => i === 0 ? !(a[2] & 2) : !!(a[2] & 2)) && await until(() => G('st.undoDepth') === u0 + 1) && await until(() => /autoslice 12/.test(G("$('ed_hist').innerHTML"))) && !/autoslice 9/.test(G("$('ed_hist').innerHTML")), JSON.stringify([u0, G('st.undoDepth'), G('st.nMarkers'), G('demoDevice.acts.filter(a => a[0] === 36)'), G("$('ed_hist').innerHTML").slice(0, 400)]));
    G(`$('as_k').value = '8'`); fireEv(asK, 'input', { target: asK }); fireEv(asK, 'change', { target: asK }); await until(() => G('st.nMarkers') === 8); }
  const m8 = G('Array.from(demoDevice.liveMarkers()).map(t => demoDevice.table[t].start).sort((a, b) => a - b).join()');
  /* a re-roll is a new seed; the strong kicks often win again, so up to four rolls to see a different chop */
  const chopNow = () => G('Array.from(demoDevice.liveMarkers()).map(t => demoDevice.table[t].start).sort((a, b) => a - b).join()');
  let rolled = false;
  for (let i = 0; i < 4 && !rolled; i++) { const g0 = G('st.tableGen'); fireEv(G("$('as_reroll')"), 'click', {}); await until(() => G('st.tableGen') !== g0 && !G('ed.busy')); rolled = chopNow() !== m8; }
  check('Re-roll → AUTOSLICE(8, re-roll): a different chop, the history labels it', sentAct(G, 36, 8, 1) && rolled && await until(() => /autoslice 8 \(re-roll\)/.test(G("$('ed_hist').innerHTML"))), JSON.stringify([rolled, G('demoDevice.acts.filter(a => a[0] === 36)'), G("$('ed_hist').innerHTML").slice(0, 300), G("$('ed_status').textContent")]));
  await until(() => !G('ed.busy') && G('st.nMarkers') === 8);
  /* back to Classic: markers → keys by hand, then clear (asked first) */
  fireEv(chip(0), 'click', {}); await until(() => G('st.grammar') === 0);
  G('ed.keysFollowGrid && null'); G('for (let k = 0; k < 24; k++) demoDevice.keySlice[k] = demoDevice.spanSlice(k)'); await sleep(150);
  fireEv(G("$('mk_keys')"), 'click', {});
  check('Markers → keys: KEYS_FROM_MARKERS on the wire, key 0 plays the first marker, the history says it', await until(() => sentAct(G, 33)) && await until(() => G('st.keySlice[0]') >= 32) && await until(() => /markers → keys \(8\)/.test(G("$('ed_hist').innerHTML"))));
  G('confirmAnswer = false'); const c0 = G('confirms'); fireEv(G("$('mk_clear')"), 'click', {}); await sleep(80);
  check('Clear markers asks first — a "no" sends nothing', G('confirms') === c0 + 1 && !sentAct(G, 34));
  G('confirmAnswer = true'); fireEv(G("$('mk_clear')"), 'click', {});
  check('…a "yes": CLEAR_MARKERS, no markers left, the keys back on the grid', await until(() => sentAct(G, 34)) && await until(() => G('st.nMarkers') === 0 && G('st.keySlice[0]') === 0));
  /* the Chop pad over a reused marker: two taps on one spot select instead of stacking */
  /* a held voice keeps something sounding (the dice may have left a rest under the playhead) */
  G('demoDevice.srcPos = () => 8 * 6000 + 1000; demoDevice.voice = { key: 0, t: Date.now(), start: 0, len: 1e9, until: Infinity }');
  fireEv(G("$('chop')"), 'pointerdown', {});
  check('Chop: a tap while it sounds → one orange marker at the kick', await until(() => G('demoDevice.liveMarkers().length') === 1) && await until(() => /tap → marker/.test(G("$('ed_status').textContent"))));
  const idx = G('demoDevice.liveMarkers()[0]'); await sleep(100);
  fireEv(G("$('chop')"), 'pointerdown', {});
  check('Chop twice on the same hit: still one marker, the second tap selects it (TAP_MARK reused) and says so', await until(() => /already there/.test(G("$('ed_status').textContent"))) && G('demoDevice.liveMarkers().length') === 1 && G('ed.selT') === idx && G('demoDevice.acts.filter(a => a[0] === 29).length') === 2);
  G('demoDevice.voice = null');
  G('demoDevice.srcPos = DEMO.FakeDevice.prototype.srcPos');
  /* Play: Undo / Clear */
  G("showTab('tab_play')"); fireEv(chip(5), 'click', {});
  check('mode chip Loop (Play): the transport turns Dice → Undo and Reslice → Clear', await until(() => G('st.grammar') === 5) && await until(() => G("$('t_dice').textContent") === 'Undo' && G("$('t_reslice').textContent") === 'Clear' && G("$('t_flow').disabled") === true));
  fireEv(pad(4), 'pointerdown', { pointerId: 21 }); fireEv(pad(4), 'pointerup', { pointerId: 21 });
  check('Play: a pad writes the loop (one event), Undo enables', await until(() => G('st.looperEvents') === 1) && await until(() => G("$('t_dice').disabled") === false));
  fireEv(G("$('t_dice')"), 'click', {});
  check('Play: Undo → ACTION 12 (the looper\'s undo, never a dice)', await until(() => sentAct(G, 12)) && await until(() => G('st.looperEvents') === 0) && !G('demoDevice.acts').slice(-3).some(a => a[0] === 0));
  G('confirmAnswer = false'); fireEv(G("$('t_reslice')"), 'click', {}); await sleep(60);
  check('Play: Clear asks — a "no" sends nothing (no reslice, no clear)', !sentAct(G, 13) && !G('demoDevice.acts').slice(-2).some(a => a[0] === 1));
  G('confirmAnswer = true'); fireEv(G("$('t_reslice')"), 'click', {});
  check('Play: Clear, confirmed → ACTION 13', await until(() => sentAct(G, 13)));
  /* Options: regrouped, nothing in Other */
  const grp = G("$('optgroups').children.map(c => c._html)");
  check('Options: Pattern · Sound · Looper · Jacks & CV · Recording · Editing · Analysis · Power-up, nothing in "Other"', ['Pattern', 'Sound', 'Looper', 'Jacks &amp; CV', 'Recording', 'Editing', 'Analysis', 'Power-up'].every(n => grp.some(h => h.includes(`<h3>${n}</h3>`))) && !grp.some(h => h.includes('Other')) && G('GROUPS.flatMap(g => g[1]).length') === 25);
  /* the theme: auto → light, tokens re-read, remembered */
  const darkBg = G("TOK['cv-bg']"); fireEv(G("$('theme')"), 'click', {});
  check('theme toggle: light on the root, the canvas tokens re-read from the CSS, remembered', G("document.documentElement.getAttribute('data-theme')") === 'light' && G("TOK['cv-bg']") === D.themes.light['cv-bg'] && G("TOK['cv-bg']") !== darkBg && G("localStorage.getItem('audiothurgist.theme')") === 'light');
  fireEv(G("$('theme')"), 'click', {}); fireEv(G("$('theme')"), 'click', {});
  check('…dark, then back to following the system', G("document.documentElement.getAttribute('data-theme')") === null && G("TOK['cv-bg']") === darkBg);
  /* a draw error costs a frame, not the session */
  G('window.__dj = drawJacks; drawJacks = () => { throw new Error("boom in the jacks card"); }; invalidate(D.jacks)');
  check('a draw error lands on the message line as a fault', await until(() => /^draw: boom/.test(G("$('msg').textContent")) && G("$('msg').className").includes('bad')));
  const calls0 = G("$('ring')._calls"); G('drawJacks = window.__dj; invalidate(D.all)'); await sleep(250);
  check('…and the page draws on (the frame after it, and the next state)', G("$('ring')._calls") > calls0 && G('!!st'));
  D.errors.length = 0;   /* the injected draw error's console.error */
  /* the unplug */
  G('demoPort.unplug()');
  check('unplugged: onClose → disconnected, said as a fault, the readouts cleared (BPM, CPU, J4, pills)', await until(() => G('link') === null) && /went away/.test(G("$('msg').textContent")) && G("$('t_bpm').textContent") === '—' && G("$('cpuText').textContent") === '—' && G("$('jk_key').textContent") === '—' && G("$('modpill').textContent") === 'not connected' && G("$('t_play').disabled") === true && G("$('connect').textContent") === 'Connect');
  check('revamp page: no unknown element ids asked for, no runtime errors', D.unknownIds.size === 0 && D.errors.filter(e => !/draw/.test(e)).length === 0, [...D.unknownIds].join(',') + ' ' + D.errors.join(' | ').slice(0, 300));
}
{ /* a module built for another page: one plain message, then nothing — no partial mode */
  const { D, G } = await loadPage('');
  await G(`(async () => { const d = new DEMO.FakeDevice({ latencyMs: 2, api: HL.API + 1 }); window.__wd = d; demoDevice = d; demoPort = new DEMO.FakePort(d); await connectPort(demoPort, true); })()`);
  const n = G("$('notice').textContent");
  check('the wrong firmware: one message (this page needs its own firmware, the module\'s version and API, flash the matching .bin), disconnected, the Connect button back',
    new RegExp('needs the Audiothurgist firmware it came with \\(page API ' + HL.API + '\\).*0\\.13\\.0-beta-demo \\(API ' + (HL.API + 1) + '\\).*Flash the matching \\.bin').test(n) && G('link') === null && G("$('connect').textContent") === 'Connect' && /wrong firmware/.test(G("$('msg').textContent")), n);
  check('…nothing past the handshake was asked (no state, no Options, no card list), nothing shown as connected', G('__wd.log').length > 0 && !G('__wd.log').some(t => t === HL.CMD.state || t === HL.CMD.getLive || t === HL.CMD.card) && G("$('padsCard').hidden") === true && G("$('editor').hidden") === true && G('st') === null);
  check('wrong-firmware page: no runtime errors', D.errors.length === 0, D.errors.join(' | ').slice(0, 300));
}
/* ── 9d. CARD, the page itself: the Card panel over `?demo=card` — a file picked (the change event), two dropped at
 *        once (the drop event), each decoded by a stand-in for the browser's decoder, converted, sent, written; the
 *        row's progress, KB/s and result; "Load it now"; the same name again and "Replace it"; then `?demo=oldcard`'s
 *        banner and "Copy them in" ── */
{
  const { D, G } = await loadPage('?demo=card');
  check('card page: connects with "card" — the panel and Play\'s "Add breaks…" shown, the list from the module, no banner',
    await until(() => G('!!(st && st.card)')) && G("$('cardCard').hidden") === false && G("$('p_add').hidden") === false && G("$('cardnote').hidden") === true && G('kitShown') === true, D.errors.join(' | ').slice(0, 300));
  check('card page: the pill counts the breaks in the folder', await until(() => /4 breaks in Audiothurgist\/breaks/.test(G("$('cd_status').textContent"))), G("$('cd_status').textContent"));
  /* the browser's decoder, stood in: 2 s of stereo at 44.1 kHz (the page converts to 48 k); the name says how long */
  G(`cardDecode = async f => { const secs = /long/.test(f.name) ? 20 : 2, sr = 44100, n = sr * secs; return { numberOfChannels: 2, sampleRate: sr, length: n, getChannelData: c => new Float32Array(n).fill(c ? -0.25 : 0.25) }; }`);
  spyActs(G);
  fireEv(G("$('cd_file')"), 'change', { target: { files: [{ name: 'Think Break (live).flac', size: 812345 }], value: 'x' } });
  check('picked: a row named by the sanitiser, then decoded → converted → sent → written', await until(() => G("cardJobs.length === 1 && cardJobs[0].state === 'done'"), 15000), G('cardJobs[0] && cardJobs[0].stl.textContent'));
  const bytes = G("demoDevice.cardFs.get('Audiothurgist/breaks/Think Break (live).wav')");
  check('…the module holds 2 s of stereo at 48 kHz PCM16 (the 44.1 kHz file converted)', bytes && bytes.length === 44 + 96000 * 4 && new DataView(bytes.buffer, bytes.byteOffset).getUint32(24, true) === 48000 && new DataView(bytes.buffer, bytes.byteOffset).getUint16(22, true) === 2, bytes && bytes.length);
  check('…the row says where it went and how fast, the bar is full', /on the card as Audiothurgist\/breaks\/Think Break \(live\)\.wav/.test(G('cardJobs[0].stl.textContent')) && /KB\/s/.test(G('cardJobs[0].stl.textContent')) && G('cardJobs[0].fill.style.width') === '100%' && G('cardJobs[0].li.className') === 'ok'
    && /stereo, 2\.00 s, 48 kHz PCM16/.test(G('cardJobs[0].inf.textContent')), G('cardJobs[0].inf.textContent'));
  check('…the Sample list re-read from the module (the new break at the end)', G('labels.sample[labels.sample.length - 1]') === 'Think Break (live).wav' && /Think Break \(live\)\.wav/.test(G("$('sample').innerHTML")));
  fireEv(G("cardJobs[0].btns['Load it now']"), 'click', {});
  check('"Load it now": ACTION 6 with the module\'s lastIdx, the fake loads it', await until(() => sentAct(G, HL.ACT.load, 5) && G('demoDevice.file') === 5));
  /* two dropped at once: one card job at a time, both land; a long one is cut to 17 s. 17 s of stereo is 3.3 MB, one
   * ~1000-byte chunk per round trip: at the demo's 6 ms a reply that alone is 20 s of waiting, so the fake answers at
   * 1 ms for these two (the chunking and the order are what is tested, not the wait) */
  G('demoDevice.opts.latencyMs = 1');
  fireEv(G("$('cd_drop')"), 'drop', { dataTransfer: { files: [{ name: 'one long.wav', size: 10 }, { name: 'two.aiff', size: 10 }], types: ['Files'] } });
  check('dropped two: both land, one after the other', await until(() => G("cardJobs.length === 3 && cardJobs[1].state === 'done' && cardJobs[2].state === 'done'"), 30000), G('cardJobs.map(j => j.state).join()'));
  G('demoDevice.opts.latencyMs = 6');
  check('…the long one cut to the first 17 s, and says so', G("demoDevice.cardFs.get('Audiothurgist/breaks/one long.wav').length") === 44 + 17 * 48000 * 4 && /the first 17 of 20\.0 s/.test(G('cardJobs[1].inf.textContent')));
  /* the same name again: refused, then replaced on request */
  fireEv(G("$('cd_file')"), 'change', { target: { files: [{ name: 'two.wav', size: 10 }] } });
  check('the same name again: "already on the card", a Replace button', await until(() => G("cardJobs.length === 4 && cardJobs[3].state === 'failed'")) && /already on the card/.test(G('cardJobs[3].stl.textContent')) && !!G("cardJobs[3].btns['Replace it']"));
  fireEv(G("cardJobs[3].btns['Replace it']"), 'click', {});
  check('…Replace it: sent with overwrite, done, the list not grown', await until(() => G("cardJobs[3].state === 'done'"), 15000) && G('demoDevice.files.length') === 8 && G('labels.sample.length') === 8);
  /* busy: a set op waiting → BAD_STATE → said, with a retry */
  G("demoDevice.set.pending = { op: 14, name: 'x' }");
  fireEv(G("$('cd_file')"), 'change', { target: { files: [{ name: 'busy.wav', size: 10 }] } });
  check('busy module: said plainly, Try again offered', await until(() => G("cardJobs.length === 5 && cardJobs[4].state === 'failed'")) && /busy/.test(G('cardJobs[4].stl.textContent')) && !!G("cardJobs[4].btns['Try again']"));
  G("demoDevice.set.pending = null; demoDevice.set.state = 0");   /* both: a held op the fake had begun would otherwise finish with no op */
  fireEv(G("cardJobs[4].btns['Try again']"), 'click', {});
  check('…Try again once it is free: done', await until(() => G("cardJobs[4].state === 'done'"), 15000));
  /* a file the browser cannot decode */
  G(`cardDecode = async () => { throw new Error('EncodingError'); }`);
  fireEv(G("$('cd_file')"), 'change', { target: { files: [{ name: 'notes.txt', size: 10 }] } });
  check('an undecodable file: said, nothing sent', await until(() => G("cardJobs.length === 6 && cardJobs[5].state === 'failed'")) && /cannot decode/.test(G('cardJobs[5].stl.textContent')) && !G("demoDevice.cardHas('Audiothurgist/breaks/notes.wav')"));
  /* bench 2026-10-25, "it thinks it fails when it didn't" (the row said it did not finish): the background poll stops
   * while the tab is hidden, so the old wait never saw the write end. The wait now asks for the state itself. */
  G(`cardDecode = async f => { const sr = 48000, n = sr; return { numberOfChannels: 1, sampleRate: sr, length: n, getChannelData: () => new Float32Array(n).fill(0.1) }; }`);
  G("document.visibilityState = 'hidden'");
  fireEv(G("$('cd_file')"), 'change', { target: { files: [{ name: 'hidden tab.wav', size: 10 }] } });
  check('a hidden tab (no background polls): the upload still ends "on the card", not "did not finish"', await until(() => G("cardJobs.length === 7 && cardJobs[6].state !== 'queued' && cardJobs[6].state !== 'decoding' && cardJobs[6].state !== 'sending' && cardJobs[6].state !== 'writing'"), 15000) && G('cardJobs[6].state') === 'done' && /on the card/.test(G('cardJobs[6].stl.textContent')), G('cardJobs[6] && cardJobs[6].stl.textContent'));
  G("document.visibilityState = 'visible'");
  /* the module's "finished" lost (its seq never moves) but the file is on the card: judged by the card's list */
  G("cardStallMs = 1500; demoDevice._fin = demoDevice.cardFinish; demoDevice.cardFinish = function (r) { const s = this.cardOp.seq; this._fin(r); this.cardOp.seq = s; }");
  fireEv(G("$('cd_file')"), 'change', { target: { files: [{ name: 'lost finish.wav', size: 10 }] } });
  check('…the finish never reported, the break listed: the row says on the card and offers Load it now', await until(() => G("cardJobs.length === 8 && (cardJobs[7].state === 'done' || cardJobs[7].state === 'failed')"), 15000) && G('cardJobs[7].state') === 'done' && !!G("cardJobs[7].btns['Load it now']") && G('cardJobs[7].idx') === G("labels.sample.indexOf('lost finish.wav')"), G('cardJobs[7] && cardJobs[7].stl.textContent'));
  /* …and the same lost finish on a name already listed (a Replace): it can't vouch, so it says it did not finish */
  fireEv(G("$('cd_over')"), 'change', {}); G("$('cd_over').checked = true");
  fireEv(G("$('cd_file')"), 'change', { target: { files: [{ name: 'lost finish.wav', size: 10 }] } });
  check('…the same on a break already listed: no false "done", it says it did not finish', await until(() => G("cardJobs.length === 9 && (cardJobs[8].state === 'done' || cardJobs[8].state === 'failed')"), 15000) && G('cardJobs[8].state') === 'failed' && /did not finish/.test(G('cardJobs[8].stl.textContent')), G('cardJobs[8] && cardJobs[8].stl.textContent'));
  G("$('cd_over').checked = false; demoDevice.cardFinish = demoDevice._fin; cardStallMs = 30000");
  check('card page: no unknown ids, no runtime errors', D.unknownIds.size === 0 && D.errors.length === 0, [...D.unknownIds].join(',') + ' ' + D.errors.join(' | ').slice(0, 300));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
}
{
  const { D, G } = await loadPage('?demo=oldcard,card');
  check('old card page: the banner and the panel say it plainly — the root\'s WAVs, sets and presets, nothing moved for you',
    await until(() => G('!!(st && st.card)')) && await until(() => G("$('cardnote').hidden") === false) && /old way/.test(G("$('cn_text').textContent")) && /moves nothing/.test(G("$('cn_text').textContent")) && G("$('cd_old').hidden") === false
    && /4 WAVs in its root, a sets folder and a presets folder/.test(G("$('cd_old_text').textContent")) && /8 files copied into Audiothurgist\/, the originals left where they are/.test(G("$('cd_old_text').textContent")) && G('labels.sample.length') === 0, G("$('cd_old_text').textContent"));
  spyActs(G); { G('demoDevice.cardOps = []'); G(`(() => { const d = demoDevice, h = d.handle; d.handle = (t, b) => { if (t === 0x67) d.cardOps.push(b[0]); return h(t, b); }; })()`); }
  fireEv(G("$('cn_copy')"), 'click', {});
  check('"Copy them in" (the banner): COPY_OLD on the wire, then copied, the list re-read, the banner gone', await until(() => G('demoDevice.cardOps').includes(HL.CARD.copy) && /copied 8 files/.test(G("$('cd_copy_state').textContent")) && G('labels.sample.length') === 5 && G("$('cardnote').hidden") === true && G("$('cd_old').hidden") === true && /4 breaks/.test(G("$('cd_status').textContent")), 15000),
    G("$('cd_copy_state').textContent") + ' / ' + G('labels.sample.length'));
  check('…the originals still in the root', G("demoDevice.cardHas('amen_170.wav') && demoDevice.cardHas('sets/amen_kit.json') && demoDevice.cardHas('presets/0.json')") === true);
  check('old card page: no unknown ids, no runtime errors', D.unknownIds.size === 0 && D.errors.length === 0, [...D.unknownIds].join(',') + ' ' + D.errors.join(' | ').slice(0, 300));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
}
/* ── 9e. PADS, the page itself: files dropped on a Play pad (the drop event), on a Kit key cell, chosen through the
 *        picker (the change event), several onto one pad; each decoded by a stand-in for the browser's decoder, cut to
 *        the room, sent, placed; the pad's progress and result, the room meter, the unsaved note and its Save set, New
 *        empty kit (a "no" sends nothing), a stereo drop that turns the kit stereo, a busy module, a hidden tab ── */
{
  const { D, G } = await loadPage('?demo');
  check('pads page: connects with "pads" — the room meter, the picker and New empty kit shown, no unsaved note',
    await until(() => G('!!(st && st.pads && table)')) && await until(() => G("$('pd_bar').hidden") === false) && G("$('pd_unsaved').hidden") === true && G("$('pads_drop_help').hidden") === false
    && /^pads: 41\.00 s of 41\.00 s free$/.test(G("$('pd_room').textContent")), D.errors.join(' | ').slice(0, 300) + ' ' + G("$('pd_room').textContent"));
  /* the browser's decoder, stood in: the name says how long and how many channels; 44.1 kHz unless it says 48 */
  G(`cardDecode = async f => { const secs = (/(\\d+(?:\\.\\d+)?)s/.exec(f.name) || [0, 1])[1] * 1, sr = /48k/.test(f.name) ? 48000 : 44100, n = Math.round(sr * secs), ch = /stereo/.test(f.name) ? 2 : 1;
    return { numberOfChannels: ch, sampleRate: sr, length: n, getChannelData: c => new Float32Array(n).map((_, i) => (c ? -0.3 : 0.3) * Math.sin(i / 5)) }; }`);
  spyActs(G); G('demoDevice.padOps = []'); G(`(() => { const d = demoDevice, h = d.handle; d.handle = (t, b) => { if (t === 0x68) d.padOps.push(b[0]); return h(t, b); }; })()`);
  const pad = k => G(`padEls[${k}].el`), files = (...names) => ({ dataTransfer: { files: names.map(name => ({ name, size: 10 })), types: ['Files'] } });
  /* dragover lights the pad; a drop loads it */
  fireEv(pad(4), 'dragover', files('x.wav'));
  check('a file dragged over a pad lights it as a drop target', pad(4).className.includes('dropover'));
  fireEv(pad(4), 'drop', files('kick 0.5s 48k.wav'));
  check('dropped on E1: decoded, sent (0x68 BEGIN / DATA), placed — the pad says ✓ and its length, the line says what it became',
    await until(() => G("padJobs.length === 1 && padJobs[0].state === 'done'"), 15000) && G('demoDevice.padOps')[0] === HL.PADS.begin && G('demoDevice.padOps').includes(HL.PADS.data) && !pad(4).className.includes('dropover')
    && await until(() => /^✓ 0\.50 s$/.test(G('padEls[4].sub.textContent'))) && /^E1: kick 0\.5s 48k\.wav — mono, 0\.50 s/.test(G("$('pd_msg').textContent")), G("$('pd_msg').textContent") + ' / ' + G('padEls[4].sub.textContent'));
  check('…on the module: key 4 owns 24000 frames, the pad\'s fill full, the room meter down by 0.5 s, the unsaved note up',
    await until(() => G('st.pads.mask') === 16) && G('demoDevice.table[demoDevice.keySlice[4]].len') === 24000 && G('padEls[4].fill.style.width') === '100%' && await until(() => /^pads: 40\.50 s of 41\.00 s free$/.test(G("$('pd_room').textContent"))) && G("$('pd_unsaved').hidden") === false);
  check('…one undo step on the module (Undo takes the drop back)', await until(() => G('st.undoDepth') === 1));
  /* after a while the pad shows what it plays */
  await until(() => G('padJob[4]') === null, 6000); await sleep(150);
  check('…then the pad shows its sample\'s length (a recording)', await until(() => /^0\.50 s$/.test(G('padEls[4].sub.textContent'))), G('padEls[4].sub.textContent'));
  /* three files on G♯2 (key 20): 20, 21, 22 in order */
  fireEv(pad(20), 'drop', files('a 0.1s.wav', 'b 0.2s.wav', 'c 0.3s.wav'));
  check('three files dropped on G♯2: that pad and the next two, in order, one at a time', await until(() => G("padJobs.length === 4 && padJobs.slice(1).every(j => j.state === 'done')"), 20000)
    && G('padJobs.slice(1).map(j => j.key).join()') === '20,21,22' && await until(() => G('st.pads.mask') === (16 | (7 << 20))) && Math.abs(G('demoDevice.table[demoDevice.keySlice[22]].len') - Math.round(0.3 * 48000)) <= 1);
  fireEv(pad(23), 'drop', files('y 0.1s.wav', 'z 0.1s.wav'));
  check('two files on B2 (the last pad): one placed, the other said left over', await until(() => G("padJobs.length === 5 && padJobs[4].state === 'done'"), 15000) && /left over/.test(G("$('msg').textContent")));
  /* a Kit key cell, and the picker for the selected key */
  G("showTab('tab_kit')"); await sleep(80);
  check('the Kit tab: "Load into …" shown, the buffer says the pads\' room', G("$('k_load').hidden") === false && /for pads/.test(G("$('u_free').textContent")));
  fireEv(G('keyCells[9].el'), 'drop', files('cell 0.25s.wav'));
  check('dropped on the Kit\'s A1 cell: key 9 loaded', await until(() => G("padJobs.length === 6 && padJobs[5].state === 'done'"), 15000) && G('padJobs[5].key') === 9 && await until(() => (G('st.pads.mask') & 512) !== 0));
  G('selectKey(11)'); fireEv(G("$('k_load')"), 'click', {});
  fireEv(G("$('pd_file')"), 'change', { target: { files: [{ name: 'picked 0.2s.wav', size: 10 }], value: 'x' } });
  check('"Load into B1…" then a file chosen: key 11 loaded (the phone path)', await until(() => G("padJobs.length === 7 && padJobs[6].state === 'done'"), 15000) && G('padJobs[6].key') === 11);
  G("showTab('tab_play')"); G("$('pd_key').value = '13'"); fireEv(G("$('pd_pick')"), 'click', {});
  fireEv(G("$('pd_file')"), 'change', { target: { files: [{ name: 'into 0.2s.wav', size: 10 }] } });
  check('Play\'s picker with "into C♯2": key 13 loaded', await until(() => G("padJobs.length === 8 && padJobs[7].state === 'done'"), 15000) && G('padJobs[7].key') === 13);
  /* a stereo file onto the mono kit */
  fireEv(pad(1), 'drop', files('wide stereo 0.4s.wav'));
  check('a stereo file: placed stereo, the line says the kit is stereo now (Sampling → Stereo), the Options re-read', await until(() => G("padJobs.length === 9 && padJobs[8].state === 'done'"), 15000) && /the kit is stereo now/.test(G("$('pd_msg').textContent")) && await until(() => G('st.stereo') === true) && await until(() => G("optByte('sampling')") === 1), G("$('pd_msg').textContent"));
  /* a busy module */
  G("demoDevice.set.pending = { op: 14, name: 'x' }");
  fireEv(pad(17), 'drop', files('busy 0.1s.wav'));
  check('a busy module (a set op waiting): said plainly, nothing placed', await until(() => G("padJobs.length === 10 && padJobs[9].state === 'failed'"), 15000) && /busy/.test(G("$('pd_msg').textContent")) && (G('st.pads.mask') & (1 << 17)) === 0);
  G("demoDevice.set.pending = null; demoDevice.set.state = 0");
  /* a hidden tab: the waits ask for the state themselves */
  G("document.visibilityState = 'hidden'");
  fireEv(pad(17), 'drop', files('hidden 0.1s.wav'));
  check('a hidden tab (no background polls): the drop is still placed and said so', await until(() => G("padJobs.length === 11 && padJobs[10].state !== 'queued' && padJobs[10].state !== 'decoding' && padJobs[10].state !== 'sending' && padJobs[10].state !== 'committing'"), 15000) && G('padJobs[10].state') === 'done', G("$('pd_msg').textContent"));
  G("document.visibilityState = 'visible'");
  /* an undecodable file */
  G(`window.__dec = cardDecode; cardDecode = async () => { throw new Error('EncodingError'); }`);
  fireEv(pad(18), 'drop', files('notes.txt'));
  check('an undecodable file: said, nothing sent', await until(() => G("padJobs.length === 12 && padJobs[11].state === 'failed'")) && /cannot decode/.test(G("$('pd_msg').textContent")));
  G('cardDecode = window.__dec');
  /* too long for the room: cut to it, and said */
  G('demoDevice.freePos = DEMO.BUF_CAP - 48000');
  fireEv(pad(15), 'drop', files('long 3s.wav'));
  check('a file longer than the room: the first second placed (all the room left), and said', await until(() => G("padJobs.length === 13 && padJobs[12].state === 'done'"), 15000) && /the first 1\.00 s of 3\.0 s, all the room left/.test(G("$('pd_msg').textContent")) && G('demoDevice.table[demoDevice.keySlice[15]].len') === 48000, G("$('pd_msg').textContent"));
  fireEv(pad(16), 'drop', files('more 0.1s.wav'));
  check('…then no room at all: refused before the wire, the room said', await until(() => G("padJobs.length === 14 && padJobs[13].state === 'failed'"), 15000) && /no room left for pads \(0\.00 s free/.test(G("$('pd_msg').textContent")) && await until(() => /✕/.test(G('padEls[16].sub.textContent'))), G("$('pd_msg').textContent"));
  /* the unsaved note's Save set: with no name → the Kit's Set section; with a name → saved, the note goes */
  fireEv(G("$('pd_save')"), 'click', {});
  check('Save set… with no name: the Kit tab\'s Set section, asked for a name', G('kitShown') === true && /name the set/.test(G("$('msg').textContent")));
  G("showTab('tab_play'); $('s_name').value = 'my_kit'"); fireEv(G("$('pd_save')"), 'click', {});
  check('…with a name: ACTION 14 "my_kit", saved, the unsaved note gone', await until(() => sentAct(G, HL.ACT.saveSet)) && await until(() => G('st.setName') === 'my_kit' && G('st.pads.unsaved') === false, 8000) && await until(() => G("$('pd_unsaved').hidden") === true));
  /* New empty kit: asks, a "no" sends nothing; a "yes" clears the break and switches to Pads */
  G('confirmAnswer = false'); const c0 = G('confirms'); fireEv(G("$('pd_new')"), 'click', {}); await sleep(80);
  check('New empty kit asks first — a "no" sends nothing', G('confirms') === c0 + 1 && !G('demoDevice.padOps').includes(HL.PADS.newKit));
  G('confirmAnswer = true'); fireEv(G("$('pd_new')"), 'click', {});
  check('…a "yes": NEW_KIT, no break, Pads, the whole memory for pads, every pad empty, the line says so',
    await until(() => G('demoDevice.padOps').includes(HL.PADS.newKit)) && await until(() => G('st.pads && st.pads.noBreak') === true && G('st.grammar') === 6 && G('st.pads.mask') === 0, 8000)
    && await until(() => /^pads: 44\.75 s of 44\.75 s free · no break$/.test(G("$('pd_room').textContent"))) && await until(() => /New empty kit — no break, Pads/.test(G("$('msg').textContent"))) && await until(() => G('padEls[0].sub.textContent') === 'empty'), G("$('pd_room').textContent") + ' / ' + G("$('msg').textContent") + ' / ' + G('padEls[0].sub.textContent'));
  fireEv(pad(0), 'drop', files('first 0.2s.wav'));
  check('…and the empty kit takes a drop like any other', await until(() => G("padJobs.length === 15 && padJobs[14].state === 'done'"), 15000) && await until(() => G('st.pads.mask') === 1));
  check('pads page: no unknown ids, no runtime errors', D.unknownIds.size === 0 && D.errors.length === 0, [...D.unknownIds].join(',') + ' ' + D.errors.join(' | ').slice(0, 300));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
  check('disconnect: the pads controls hidden', G("$('pd_bar').hidden") === true && G("$('padsCard').hidden") === true);
}
/* ── 9f. beta-sampler: Start In, and "import and play individual samples" end to end. The protocol first (Start In is
 *        Options byte 24, a power-up with each value, ▶ in Pads, HL.padNormalize), then the page over `?demo=sampler` —
 *        the module powering up as a sampler and the Play tab reading as one; Start In on the Options tab; a mono WAV and a stereo WAV (real RIFF bytes, decoded by a PCM16 stand-in for the browser's
 *        decoder) dropped on two pads and arriving sample for sample; both played from the page's pads; the kit turned
 *        stereo; normalise drops (remembered); the set saved; the module power-cycled on the same card and the page
 *        reconnected — a sampler again, the pads gone from memory; the set loaded back — the same pads, stereo, playing;
 *        then the flip to Classic, stopped until ▶ ── */
{
  /* the protocol, over HL and the fake */
  const d = new DEMO.FakeDevice({ latencyMs: 1 }), l = new HL.Link(new DEMO.FakePort(d)); await l.start(); const inf = await HL.hello(l);
  const ds = await HL.getDescriptor(l, inf), oi = HL.optionsInfo(ds), fs = oi.fields.find(f => f.id === 'start.in');
  check('START: Start In is Options byte 24 — Sampler (the firmware\'s default) · As saved', fs && fs.off === 24 && fs.zones === 2 && /^Sampler/.test(fs.disp.labels[0]) && /^As saved/.test(fs.disp.labels[1]) && DEMO.OPTION_FIELDS[24][3] === 0);
  let s = HL.parseState(await l.request(HL.CMD.state));
  check('START: this fake\'s boot preset says As saved — Classic, running', s.grammar === 0 && s.muted === false && d.options[24] === 1);
  { const live = await HL.getLive(l, inf), bytes = Uint8Array.from(live.bytes); bytes[oi.comp.off + 24] = 0; await HL.setLive(l, inf, bytes);
    s = HL.parseState(await l.request(HL.CMD.state));
    check('Start In → Sampler through the live blob: the byte reads back 0, the running module untouched (Classic, running) — it is read at power-up', (await HL.getLive(l, inf)).bytes[oi.comp.off + 24] === 0 && s.grammar === 0 && !s.muted); }
  const p1 = new DEMO.FakeDevice({ latencyMs: 1, startIn: 0 }), lp = new HL.Link(new DEMO.FakePort(p1)); await lp.start(); await HL.hello(lp);
  s = HL.parseState(await lp.request(HL.CMD.state));
  check('…a power-up with Start In = Sampler: Pads (6), stopped, the Voice map (Jacks = Follow grammar); the preset\'s Classic kept for ▶', s.grammar === 6 && s.muted === true && s.jackMap === 1 && p1.lastDrum === 0);
  {
    /* ▶ in Pads plays a beat: a start made in Pads brings back the last drum grammar (the boot preset's here, Classic) */
    const rs = await HL.setMute(lp, false), ss = HL.parseState(await lp.request(HL.CMD.state));
    const rt = await HL.setMute(lp, true), st2 = HL.parseState(await lp.request(HL.CMD.state));
    check('START: ▶ (SET_MUTE 0) in Pads after a sampler power-up → the boot preset\'s Classic, running; ■ leaves Classic, stopped', rs.ok && ss.grammar === 0 && ss.muted === false && rt.ok && st2.grammar === 0 && st2.muted === true, JSON.stringify([ss.grammar, ss.muted, st2.grammar, st2.muted]));
    p1.options[5] = 2; p1.setGrammar(2, 0); p1.options[5] = 6; p1.setGrammar(6, 2);   /* Options byte 5 = grammar.mode */
    await HL.setMute(lp, false); const su = HL.parseState(await lp.request(HL.CMD.state));
    check('…Grids → Pads, then ▶: Grids comes back, running', su.grammar === 2 && su.muted === false, JSON.stringify([su.grammar, su.muted]));
  }
  const nc = new DEMO.FakeDevice({ latencyMs: 1, nocard: true }), ln = new HL.Link(new DEMO.FakePort(nc)); await ln.start(); await HL.hello(ln);
  const sn = HL.parseState(await ln.request(HL.CMD.state));
  check('no card changes nothing: Start In is the preset\'s (As saved here: Classic, running)', sn.grammar === 0 && sn.muted === false && !sn.card.mounted);
  check('ACTION 40 (the card file\'s START_IN) is gone: BAD_ARGS', (await HL.editOp(l, 40, [1])).status === 2);
  for (const x of [l, lp, ln]) await x.close();
  /* HL.padNormalize: one gain for both channels, −1 dBFS at the peak, only the first `frames`, silence left alone */
  const L = Float32Array.from({ length: 1000 }, (_, i) => 0.25 * Math.sin(i / 7)), R = Float32Array.from({ length: 1000 }, (_, i) => 0.125 * Math.cos(i / 5));
  const nz = HL.padNormalize([L, R], 1000, -1), pk = a => a.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
  check('padNormalize: the louder channel peaks at −1 dBFS (0.891), the other keeps its ratio, the source untouched', nz.applied && Math.abs(pk(nz.planes[0]) - Math.pow(10, -1 / 20)) < 1e-6 && Math.abs(pk(nz.planes[1]) / pk(nz.planes[0]) - pk(R) / pk(L)) < 1e-6 && Math.abs(nz.gainDb - 20 * Math.log10(0.891251 / pk(L))) < 0.01 && pk(L) < 0.26);
  const loud = Float32Array.from({ length: 100 }, (_, i) => i < 50 ? 0.1 : 1.0), part = HL.padNormalize([loud], 50);
  check('…only the frames that go (a loud tail past them does not count); silence is left as it is', part.applied && part.planes[0].length === 50 && Math.abs(part.planes[0][0] - 0.891251) < 1e-5 && HL.padNormalize([new Float32Array(64)], 64).applied === false);
}
/* the page */
const ST_SR = 48000;
function wavFile(name, chans) {   /* a real RIFF/WAVE PCM16 file, as a drop hands the page */
  const bytes = HL.encodeWav(chans, ST_SR);
  return { name, size: bytes.length, type: 'audio/wav', arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length) };
}
const tone = (n, f, a, ph = 0) => Float32Array.from({ length: n }, (_, i) => a * Math.sin(2 * Math.PI * f * i / ST_SR + ph));
{
  const { D, G, ctx } = await loadPage('?demo=sampler');
  /* the browser's decoder, stood in: it parses the dropped file's RIFF bytes (PCM16, 48 kHz — no resample needed) */
  G(`cardDecode = async f => { const b = new Uint8Array(await f.arrayBuffer()), dv = new DataView(b.buffer); let at = 12, fmt = null, data = null;
    while (at + 8 <= b.length) { const id = String.fromCharCode(...b.subarray(at, at + 4)), n = dv.getUint32(at + 4, true); if (id === 'fmt ') fmt = { ch: dv.getUint16(at + 10, true), sr: dv.getUint32(at + 12, true), bits: dv.getUint16(at + 22, true) }; if (id === 'data') data = { at: at + 8, n }; at += 8 + n + (n & 1); }
    if (!fmt || !data || fmt.bits !== 16) throw new Error('EncodingError');
    const len = data.n / 2 / fmt.ch, planes = Array.from({ length: fmt.ch }, () => new Float32Array(len));
    for (let i = 0; i < len; i++) for (let c = 0; c < fmt.ch; c++) planes[c][i] = dv.getInt16(data.at + (i * fmt.ch + c) * 2, true) / 32768;
    return { numberOfChannels: fmt.ch, sampleRate: fmt.sr, length: len, getChannelData: c => planes[c] }; }`);
  check('sampler page: the fake powers up as the module does — Pads, stopped, the Voice map', await until(() => G('!!(st && table)')) && G('st.grammar') === 6 && G('st.muted') === true && await until(() => G('st.jackMap') === 1), D.errors.join(' | ').slice(0, 300));
  await sleep(120);
  check('…the Play tab reads as a sampler: the pads card first (m-sampler), the sampler line with where a drop goes and how it powers up, the transport says "sampler", ▶ not cued (Pads has no pattern), the ring says SAMPLER',
    await until(() => G("$('play').className").includes('m-sampler') && G("$('pd_sampler').hidden") === false) && /Drop audio files on a pad/.test(G("$('pd_sampler_text').innerHTML")) && /powers up like this/.test(G("$('pd_sampler_text').innerHTML"))
    && /<b>sampler<\/b>/.test(G("$('t_sub').innerHTML")) && G("$('t_play').textContent") === '▶' && !G("$('t_play').className").includes('cue') && G("$('mode_cue').hidden") === true && G("$('ring')._log").text.includes('SAMPLER'),
    G("$('play').className") + ' / ' + G("$('t_sub').innerHTML"));
  check('…the mode chips: Pads lit; the Jacks card says the Voice map; the sampler line\'s Classic / Grids buttons are there', G('modeChips.find(c => c.g === 6).el.className').includes('on') && G("$('jk_map').textContent") === 'Voice map' && G("$('pd_go_classic').hidden") === false && G("$('pd_go_grids').hidden") === false);
  spyActs(G);
  /* Start In, on the Options tab: an Options field like the others */
  const grp = G("$('optgroups').children.map(c => c._html)"), sel = () => G("optSelects.get('start.in')[0]");
  check('Options: Start In in its own "Power-up" group (still nothing in "Other"), Sampler from the live blob', grp.some(h => /Power-up/.test(h)) && !grp.some(h => h.includes('Other')) && G("optByte('start.in')") === 0 && sel().value === '0');
  sel().value = '1'; fireEv(sel(), 'change', {});
  check('Start In → As saved: written through the live blob (byte 24 on the module), the sampler line stops saying it powers up like this',
    await until(() => G("demoDevice.options[DEMO.O['start.in']]") === 1) && await until(() => !/powers up like this/.test(G("$('pd_sampler_text').innerHTML"))));
  check('…the running module untouched by it (still Pads, stopped)', G('st.grammar') === 6 && G('st.muted') === true);
  await until(() => G('!optBusy')); sel().value = '0'; fireEv(sel(), 'change', {});
  check('…and back to Sampler: byte 24 is 0 again, the line says so', await until(() => G("demoDevice.options[DEMO.O['start.in']]") === 0) && await until(() => /powers up like this/.test(G("$('pd_sampler_text').innerHTML"))));
  /* a pad plays while stopped */
  const pad = k => G(`padEls[${k}].el`);
  fireEv(pad(3), 'pointerdown', { pointerId: 31 }); fireEv(pad(3), 'pointerup', { pointerId: 31 });
  check('stopped, a pad still plays: PLAY_KEY(3) down / up, the flash comes back, the transport still stopped', await until(() => sentAct(G, 32, 3, 1) && sentAct(G, 32, 3, 0)) && await until(() => G('st.keyFlash') === 3) && G('st.muted') === true);
  /* a mono WAV and a stereo WAV onto two pads */
  const monoL = tone(24000, 220, 0.5), stL = tone(19200, 330, 0.4), stR = tone(19200, 495, 0.3, 1);
  G('window.__files = {}'); ctx.__mono = wavFile('kick mono.wav', [monoL]); ctx.__st = wavFile('pad stereo.wav', [stL, stR]);
  const dropOn = (k, ...fs) => fireEv(pad(k), 'drop', { dataTransfer: { files: fs, types: ['Files'] } });
  check('the stand-in decoder reads the WAVs as the browser would (mono 0.50 s, stereo 0.40 s)', await G('(async () => { const a = await cardDecode(__mono), b = await cardDecode(__st); return a.numberOfChannels === 1 && a.length === 24000 && b.numberOfChannels === 2 && b.length === 19200; })()'));
  dropOn(0, ctx.__mono);
  check('a mono WAV dropped on C1: sent (0x68), placed — the pad says ✓ 0.50 s, the line "mono, 0.50 s"', await until(() => G("padJobs.length === 1 && padJobs[0].state === 'done'"), 15000) && /^C1: kick mono\.wav — mono, 0\.50 s$/.test(G("$('pd_msg').textContent")) && await until(() => (G('st.pads.mask') & 1) === 1), G("$('pd_msg').textContent"));
  const sameAs = (key, plane, src) => G(`(() => { const e = demoDevice.table[demoDevice.keySlice[${key}]], b = ${plane ? 'demoDevice.bufR' : 'demoDevice.buf'}; if (!b || e.len !== ${src.length}) return 'len ' + (e && e.len); let w = 0; const s = [${Array.from(src).join(',')}]; for (let i = 0; i < s.length; i++) w = Math.max(w, Math.abs(b[e.start + i] - s[i])); return w; })()`);
  const wm = sameAs(0, 0, monoL);
  check('…on the module, key 0 holds the file sample for sample (within one PCM16 step)', typeof wm === 'number' && wm <= 1.5 / 32767, String(wm));
  check('…the kit is still mono', G('st.stereo') === false);
  dropOn(1, ctx.__st);
  check('a stereo WAV dropped on C♯1: placed stereo, the line says the kit is stereo now (Options → Sampling: Stereo)', await until(() => G("padJobs.length === 2 && padJobs[1].state === 'done'"), 15000) && /stereo, 0\.40 s — the kit is stereo now/.test(G("$('pd_msg').textContent")) && await until(() => G('st.stereo') === true) && await until(() => G("optByte('sampling')") === 1), G("$('pd_msg').textContent"));
  const wl = sameAs(1, 0, stL), wr = sameAs(1, 1, stR), w0 = sameAs(0, 1, monoL);
  check('…L and R arrive apart, each sample for sample; the mono pad now sounds on both sides (R = L)', [wl, wr, w0].every(w => typeof w === 'number' && w <= 2 / 32767), [wl, wr, w0].join(' '));
  /* both played from the page's pads */
  fireEv(pad(0), 'pointerdown', { pointerId: 41 });
  check('pad C1 plays the mono sample: PLAY_KEY(0), the module\'s voice on its span', await until(() => sentAct(G, 32, 0, 1)) && await until(() => G('demoDevice.voice && demoDevice.voice.key') === 0) && G('demoDevice.voice.start') === G('demoDevice.table[demoDevice.keySlice[0]].start') && G('demoDevice.voice.len') === 24000);
  fireEv(pad(0), 'pointerup', { pointerId: 41 }); fireEv(pad(1), 'pointerdown', { pointerId: 42 });
  check('pad C♯1 plays the stereo sample', await until(() => sentAct(G, 32, 1, 1)) && await until(() => G('demoDevice.voice && demoDevice.voice.key') === 1) && G('demoDevice.voice.len') === 19200);
  fireEv(pad(1), 'pointerup', { pointerId: 42 });
  await until(() => G('padJob[0] === null && padJob[1] === null'), 6000); await sleep(150);
  check('…the pads show their lengths; the unsaved note is up', /^0\.50 s$/.test(G('padEls[0].sub.textContent')) && /^0\.40 s$/.test(G('padEls[1].sub.textContent')) && G("$('pd_unsaved').hidden") === false);
  /* normalise drops: off by default, remembered, applied to the next drop */
  check('normalise drops: off by default', G("$('pd_norm').checked") === false && G('padNorm') === false);
  G("$('pd_norm').checked = true"); fireEv(G("$('pd_norm')"), 'change', { target: G("$('pd_norm')") });
  check('…ticked: remembered in this browser', G('padNorm') === true && G("localStorage.getItem('audiothurgist.padNorm')") === '1');
  const quiet = tone(9600, 440, 0.25); ctx.__q = wavFile('quiet.wav', [quiet]); dropOn(2, ctx.__q);
  check('…a quiet mono WAV (peak −12 dBFS) arrives peaking at −1 dBFS, and the line says by how much', await until(() => G("padJobs.length === 3 && padJobs[2].state === 'done'"), 15000) && /normalised \+11\.0 dB/.test(G("$('pd_msg').textContent"))
    && Math.abs(G('(() => { const e = demoDevice.table[demoDevice.keySlice[2]]; let m = 0; for (let i = 0; i < e.len; i++) m = Math.max(m, Math.abs(demoDevice.buf[e.start + i])); return m; })()') - 0.8913) < 0.001, G("$('pd_msg').textContent"));
  G("$('pd_norm').checked = false"); fireEv(G("$('pd_norm')"), 'change', { target: G("$('pd_norm')") });
  check('…unticked: remembered off', G("localStorage.getItem('audiothurgist.padNorm')") === '0' && G('padNorm') === false);
  /* save the set */
  G("$('s_name').value = 'smp_kit'"); fireEv(G("$('pd_save')"), 'click', {});
  check('Save set… (named): ACTION 14 "smp_kit", saved, the unsaved note gone', await until(() => sentAct(G, HL.ACT.saveSet)) && await until(() => G('st.setName') === 'smp_kit' && G('st.pads.unsaved') === false, 8000) && await until(() => G("$('pd_unsaved').hidden") === true));
  /* the power-cycle: a fresh module (Start In = Sampler) on the same card; the page reconnects */
  G('window.__card = demoDevice.cardFs; window.__sets = demoDevice.sets');
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
  await G(`(async () => { const d = new DEMO.FakeDevice({ latencyMs: 6, startIn: 0 }); d.cardFs = __card; d.sets = __sets; demoDevice = d; demoPort = new DEMO.FakePort(d); await connectPort(demoPort, true); demoDevice = d; })()`);
  check('power-cycled on the same card, the page reconnected: a sampler again (Pads, stopped), the sampler line back, the pads empty of drops (they were in memory), nothing unsaved',
    await until(() => G('!!(st && table)')) && G('st.grammar') === 6 && G('st.muted') === true && G('st.pads.mask') === 0 && G('st.pads.unsaved') === false && await until(() => G("$('pd_sampler').hidden") === false), D.errors.join(' | ').slice(0, 300));
  spyActs(G);
  G("$('s_name').value = 'smp_kit'"); fireEv(G("$('s_load')"), 'click', {});
  check('Load set "smp_kit": ACTION 15, the three pads back, the kit stereo, nothing unsaved, still Pads and stopped', await until(() => sentAct(G, HL.ACT.loadSet)) && await until(() => G('st.setName') === 'smp_kit' && G('st.pads.mask') === 7, 8000) && await until(() => G('st.stereo') === true) && G('st.pads.unsaved') === false && G('st.grammar') === 6 && G('st.muted') === true);
  const vl = sameAs(1, 0, stL), vr = sameAs(1, 1, stR), vm = sameAs(0, 0, monoL);
  check('…sample for sample: the mono pad, and the stereo pad\'s L and R', [vl, vr, vm].every(w => typeof w === 'number' && w <= 2 / 32767), [vl, vr, vm].join(' '));
  await sleep(250);
  fireEv(pad(1), 'pointerdown', { pointerId: 51 }); fireEv(pad(1), 'pointerup', { pointerId: 51 });
  check('…and the stereo pad plays from the page again', await until(() => sentAct(G, 32, 1, 1)) && await until(() => G('demoDevice.voice && demoDevice.voice.key') === 1) && G('demoDevice.voice.len') === 19200);
  /* the flip: Classic while stopped stays stopped until ▶ */
  fireEv(G("$('pd_go_classic')"), 'click', {});
  check('the sampler line\'s Classic: the grammar written (Classic), the transport still stopped', await until(() => G('demoDevice.grammar') === 0 && G('st.grammar') === 0) && !sentAct(G, 35) && G('st.muted') === true);
  await sleep(300);
  check('…still stopped a moment later; the cue says ▶ starts it, ▶ is ringed, the sampler line gone, the transport says "stopped"', G('st.muted') === true && await until(() => G("$('mode_cue').hidden") === false && /press ▶ to start Classic/.test(G("$('mode_cue').innerHTML")) && G("$('t_play').className").includes('cue') && G("$('pd_sampler').hidden") === true && /<b>stopped<\/b>/.test(G("$('t_sub').innerHTML"))));
  fireEv(G("$('t_play')"), 'click', {});
  check('▶: SET_MUTE(0), the pattern runs, the cue and the ring gone', await until(() => sentAct(G, 35, 0) && G('demoDevice.muted') === false) && await until(() => G("$('mode_cue').hidden") === true && !G("$('t_play').className").includes('cue') && G("$('t_play').textContent") === '■'));
  check('sampler page: no unknown ids, no runtime errors', D.unknownIds.size === 0 && D.errors.length === 0, [...D.unknownIds].join(',') + ' ' + D.errors.join(' | ').slice(0, 300));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
}
{ /* normalise drops, remembered: a page opened with it stored starts ticked */
  const html = readFileSync(path.join(here, 'index.html'), 'utf8'), script = [...html.matchAll(/<script>\n([\s\S]*?)<\/script>/g)].pop()[1];
  const D = domStub(html); D.sandbox.localStorage.setItem('audiothurgist.padNorm', '1'); const c = vm.createContext(D.sandbox); vm.runInContext(script, c, { filename: 'index.html' });
  check('normalise drops: a page opened with it remembered starts ticked (and the Demo button powers up a sampler)', vm.runInContext("padNorm === true && $('pd_norm').checked === true", c) && /connectDemo\('sampler'\)/.test(html));
}

/* ── 9g. KEYP, the page itself: `?demo=keyp` (four keys set) — the cells and the pads say a key's settings in words,
 *        the row under the keys shows the selected key's (the ratchet's rate only in Ratchet), a gain slider dragged
 *        sends live and lands as one undo step and one history line, Plays and its ratchet rate, reset ── */
{
  const { D, G } = await loadPage('?demo=keyp');
  check('keyp page: connects, reads 0x69, the row shown under the keys', await until(() => G('!!(st && keyp && table)')) && G("$('kp_row').hidden") === false && G('demoDevice.log').includes(0x69), D.errors.join(' | ').slice(0, 300));
  await until(() => G('keyCells[7].kp.textContent') !== '');
  check('the cells say the set keys in words (C1 −3 dB, D♯1 +7 st · loop, G1 +2 dB · −12 st · ratchet ½, C2 +0.25 st · one-shot); the rest say nothing',
    G('keyCells[0].kp.textContent') === '−3 dB' && G('keyCells[3].kp.textContent') === '+7 st · loop' && G('keyCells[7].kp.textContent') === '+2 dB · −12 st · ratchet ½' && G('keyCells[12].kp.textContent') === '+0.25 st · one-shot'
    && G('keyCells[1].kp.hidden') === true && G('keyCells[7].kp.hidden') === false, [0, 3, 7, 12].map(k => G(`keyCells[${k}].kp.textContent`)).join(' | '));
  check('…and so do the Play pads', await until(() => G('padEls[7].kp.textContent') === '+2 dB · −12 st · ratchet ½') && G('padEls[2].kp.hidden') === true);
  G('selectKey(7)'); G('invalidate(D.kit)'); await sleep(80);
  check('G1 selected: the row says +2 dB, −12 st, Ratchet, every ½ sample (the rate shown in Ratchet only)', G("$('kp_key').textContent") === 'G1' && G("$('kp_gain').value") === '4' && G("$('kp_semi').value") === '-12' && G("$('kp_plays').value") === '4' && G("$('kp_rat').value") === '1' && G("$('kp_rat_l').hidden") === false && G("$('kp_gain_v').textContent") === '+2 dB');
  G('selectKey(1)'); G('invalidate(D.kit)'); await sleep(80);
  check('C♯1: neutral — 0 dB, 0 st, Follow, no rate, reset off', G("$('kp_gain').value") === '0' && G("$('kp_plays').value") === '0' && G("$('kp_rat_l').hidden") === true && G("$('kp_reset').disabled") === true);
  /* a drag of the gain slider: live sends, one undo step, one history line */
  spyActs(G);
  const d0 = G('st.undoDepth'), gain = G("$('kp_gain')");
  for (const v of [-2, -4, -6, -8, -10, -12]) { G(`$('kp_gain').value = '${v}'`); fireEv(gain, 'input', {}); await sleep(5); }
  fireEv(gain, 'change', {});
  check('the gain dragged to −6 dB: the module has it, sent live (KEY_PARAMS more than once, the later ones with JOIN)',
    await until(() => G('demoDevice.keyp[1].gain') === -12 && !G('kpBusy')) && G('demoDevice.acts.filter(a => a[0] === 37).length') >= 2 && G('demoDevice.acts.filter(a => a[0] === 37).slice(1).every(a => (a[7] & 1) === 1)'), JSON.stringify(G('demoDevice.acts.filter(a => a[0] === 37)')));
  check('…one undo step on the module, one history line ("C♯1 settings: −6 dB"), the cell says it', await until(() => G('st.undoDepth') === d0 + 1) && G("ed.history.list().filter(x => /^C♯1 settings/.test(x.label)).length") === 1
    && G("ed.history.list()[0].label") === 'C♯1 settings: −6 dB' && await until(() => G('keyCells[1].kp.textContent') === '−6 dB'), G("ed.history.list().map(x => x.label).join(' | ')"));
  /* Plays → Ratchet, then its rate */
  G("$('kp_plays').value = '4'"); fireEv(G("$('kp_plays')"), 'change', {});
  check('Plays → Ratchet: sent, the rate select appears', await until(() => G('demoDevice.keyp[1].plays') === 4) && G("$('kp_rat_l').hidden") === false);
  await until(() => !G('kpBusy'));
  G("$('kp_rat').value = '5'"); fireEv(G("$('kp_rat')"), 'change', {});
  check('…every 16th: sent; the cell says "−6 dB · ratchet 1/16"', await until(() => G('demoDevice.keyp[1].rat') === 5) && await until(() => G('keyCells[1].kp.textContent') === '−6 dB · ratchet 1/16'), G('keyCells[1].kp.textContent'));
  await until(() => !G('kpBusy'));
  /* tune: semitones and cents */
  G("$('kp_semi').value = '3'"); fireEv(G("$('kp_semi')"), 'input', {}); fireEv(G("$('kp_semi')"), 'change', {});
  await until(() => !G('kpBusy'));
  G("$('kp_cents').value = '-50'"); fireEv(G("$('kp_cents')"), 'change', {});
  check('tune +3 st, then −50 cents: +250 cents on the module ("+2.5 st")', await until(() => G('demoDevice.keyp[1].cents') === 250) && await until(() => /\+2\.5 st/.test(G('keyCells[1].kp.textContent'))), G('keyCells[1].kp.textContent'));
  await until(() => !G('kpBusy'));
  /* reset */
  fireEv(G("$('kp_reset')"), 'click', {});
  check('reset: the key neutral on the module, its words gone, the row back at 0', await until(() => G('demoDevice.keyp[1].gain === 0 && demoDevice.keyp[1].plays === 0 && demoDevice.keyp[1].cents === 0 && demoDevice.keyp[1].rat === 0'))
    && await until(() => G('keyCells[1].kp.hidden') === true) && G("$('kp_gain').value") === '0');
  check('keyp page: no errors, no unknown ids', D.unknownIds.size === 0 && D.errors.length === 0, D.errors.join(' | ').slice(0, 300) + [...D.unknownIds].join(','));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
  check('disconnect: the row hidden', G("$('kp_row').hidden") === true && G('keyp') === null);
}
/* ── 9h. CHROMA, the page: the Kit's "Spread D♯1 chromatically (C1 … B2)" under the settings row — a key whose spread
 *        would replace pads asks first (the key-drop prompt's red popover: keep them, the default · over them · cancel),
 *        the cells and the pads then say each key's tune, one history line, Undo puts every key back; a key with no pads
 *        in the way spreads at once; an empty kit's key cannot be spread ── */
{
  const { D, G } = await loadPage('?demo=kit');
  G("showTab('tab_kit')");
  check('chroma page: connected, the button shown in the settings row', await until(() => G('!!(st && keyp && table)')) && G("$('kp_spread').hidden") === false, D.errors.join(' | ').slice(0, 300));
  G('selectKey(3)'); G('invalidate(D.kit)'); await sleep(80);
  check('D♯1 selected: "Spread D♯1 chromatically (C1 … B2)", enabled', G("$('kp_spread').textContent") === 'Spread D♯1 chromatically (C1 … B2)' && G("$('kp_spread').disabled") === false, G("$('kp_spread').textContent"));
  spyActs(G);
  const before = G('JSON.stringify({ m: Array.from(demoDevice.keySlice), k: demoDevice.keyp })'), d0 = G('st.undoDepth');
  fireEv(G("$('kp_spread')"), 'click', { clientX: 200, clientY: 40 }); await sleep(40);
  const menu = () => G("$('ed_menu')");
  check('two other pads (G1, D2) in the way: the red prompt asks first, "keep" the default, nothing sent yet',
    !menu().hidden && menu().className.includes('red') && /G1, D2/.test(menu().children[0].textContent) && menu().children.some(b => b.className === 'primary' && /keep G1, D2/.test(b.textContent)) && !sentAct(G, 38), menu().children.map(b => b.textContent).join(' | '));
  menu().children.find(b => /^cancel$/.test(b.textContent)).onclick(); await sleep(60);
  check('cancel: nothing sent, nothing moved', !sentAct(G, 38) && G('JSON.stringify({ m: Array.from(demoDevice.keySlice), k: demoDevice.keyp })') === before);
  fireEv(G("$('kp_spread')"), 'click', { clientX: 200, clientY: 40 }); await sleep(40);
  menu().children.find(b => /keep G1, D2/.test(b.textContent)).onclick();
  check('keep: SPREAD_KEYS(3, keep pads) — 21 keys on D♯1\'s pad, G1 and D2 kept', await until(() => sentAct(G, 38, 3, 1)) && await until(() => G('demoDevice.keySlice[0] === demoDevice.keySlice[3] && demoDevice.keySlice[23] === demoDevice.keySlice[3]')) && G('demoDevice.keySlice[7] !== demoDevice.keySlice[3] && demoDevice.keySlice[14] !== demoDevice.keySlice[3]'));
  check('…the cells say the tunes (C1 −3 st, E1 +1 st, D♯2 +12 st), G1 and D2 say nothing new, D♯1 its own (none)',
    await until(() => G('keyCells[0].kp.textContent') === '−3 st' && G('keyCells[4].kp.textContent') === '+1 st' && G('keyCells[15].kp.textContent') === '+12 st') && G('keyCells[7].kp.hidden') === true && G('keyCells[3].kp.hidden') === true, [0, 4, 15, 7].map(k => G(`keyCells[${k}].kp.textContent`)).join(' | '));
  check('…and so do the Play pads (D♯2 "+12 st")', await until(() => G('padEls[15].kp.textContent') === '+12 st'));
  check('…one undo step, one history line ("D♯1 spread chromatically (21 keys, pads kept)")', await until(() => G('st.undoDepth') === d0 + 1) && await until(() => G('ed.history.list()[0].label') === 'D♯1 spread chromatically (21 keys, pads kept)'), G("ed.history.list().map(x => x.label).join(' | ')"));
  fireEv(G("$('ed_undo')"), 'click', {});
  check('Undo: every key\'s map and settings back, the words gone', await until(() => G('JSON.stringify({ m: Array.from(demoDevice.keySlice), k: demoDevice.keyp })') === before) && await until(() => G('keyCells[0].kp.hidden') === true && G('keyCells[15].kp.hidden') === true));
  await until(() => !G('ed.busy') && G('st.undoDepth') === d0);
  fireEv(G("$('kp_spread')"), 'click', { clientX: 200, clientY: 40 }); await sleep(40);
  menu().children.find(b => /spread over them too/.test(b.textContent)).onclick();
  check('over them: SPREAD_KEYS(3, 0) — all 23 on D♯1\'s pad, G1 now +4 st', await until(() => sentAct(G, 38, 3, 0)) && await until(() => G('demoDevice.keySlice[7] === demoDevice.keySlice[3] && demoDevice.keySlice[14] === demoDevice.keySlice[3]')) && await until(() => G('keyCells[7].kp.textContent') === '+4 st'));
  /* a key with no pads in the way: no prompt — C1 (a slice of the break; the spread follows the tempo) */
  fireEv(G("$('ed_undo')"), 'click', {}); await until(() => G('JSON.stringify({ m: Array.from(demoDevice.keySlice), k: demoDevice.keyp })') === before);
  await until(() => !G('ed.busy'));
  G('demoDevice.keySlice[7] = demoDevice.keySlice[3]; demoDevice.keySlice[14] = demoDevice.keySlice[3]'); await until(() => G('st.keySlice[14] === st.keySlice[3]'));   /* (the fake re-pointed by hand: the pads out of the way) */
  G('selectKey(3)'); G('invalidate(D.kit)'); await sleep(60); G('demoDevice.acts = []');
  fireEv(G("$('kp_spread')"), 'click', { clientX: 200, clientY: 40 }); await sleep(40);
  check('no pads in the way: no prompt, sent at once', G("$('ed_menu').hidden") === true && await until(() => sentAct(G, 38, 3, 0)));
  check('chroma page: no errors, no unknown ids', D.unknownIds.size === 0 && D.errors.length === 0, D.errors.join(' | ').slice(0, 300) + [...D.unknownIds].join(','));
  /* an empty kit's key: nothing to spread */
  G('demoDevice.padNewKit()'); await until(() => G('!!(st && st.pads && st.pads.noBreak && table && table.nSlices === 16)')); await sleep(200); G('selectKey(0)'); G('invalidate(D.kit)'); await sleep(60);
  check('an empty kit\'s key: the button off, its title says to drop a sample first', G("$('kp_spread').disabled") === true && /empty — drop a sample on it first/.test(G("$('kp_spread').title")));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
  check('disconnect: the button hidden', G("$('kp_spread').hidden") === true);
}
/* ── 9i. Review of the beta's page (2026-10-29): races a player can hit on the bench ── */
{
  /* KEYP: the row holds what your hand set for 1.5 s; an Undo landing inside that window must still show in the row,
   * or the next touch (the row is sent whole) writes the undone value straight back */
  const { D, G } = await loadPage('?demo=keyp');
  await until(() => G('!!(st && keyp && table && st.bufLen)'));
  G("showTab('tab_kit')"); G('selectKey(1)'); G('invalidate(D.kit)'); await sleep(80);
  const gain = G("$('kp_gain')");
  G("$('kp_gain').value = '-12'"); fireEv(gain, 'input', {}); fireEv(gain, 'change', {});
  await until(() => !G('kpBusy') && G('keyp[1].gain') === -12);
  fireEv(G("$('ed_undo')"), 'click', {});
  await until(() => G('keyp && keyp[1].gain') === 0); await sleep(60); G('invalidate(D.kit)'); await sleep(60);
  check('review: an Undo inside the row\'s hold shows in the row (0 dB again)', G("$('kp_gain').value") === '0' && G("$('kp_gain_v').textContent") === '0 dB', G("$('kp_gain_v').textContent"));
  G("$('kp_plays').value = '1'"); fireEv(G("$('kp_plays')"), 'change', {});
  check('review: …and the next touch (Plays → One-shot) does not bring the undone −6 dB back', await until(() => G('demoDevice.keyp[1].plays') === 1 && !G('kpBusy')) && G('demoDevice.keyp[1].gain') === 0, 'module gain ' + G('demoDevice.keyp[1].gain'));
  /* KEYP: a change for another key made while this key's send is in flight is sent too (it was dropped) */
  await until(() => !G('kpBusy') && !G('kpWant'));
  G('selectKey(1)'); G('invalidate(D.kit)'); await sleep(60);
  G("$('kp_plays').value = '2'"); fireEv(G("$('kp_plays')"), 'change', {});
  G('selectKey(2)'); G("$('kp_plays').value = '3'"); fireEv(G("$('kp_plays')"), 'change', {});
  check('review: C♯1 → Held, then D1 → Loop before C♯1\'s reply: both reach the module', await until(() => G('demoDevice.keyp[2].plays') === 3) && G('demoDevice.keyp[1].plays') === 2, 'C♯1 ' + G('demoDevice.keyp[1].plays') + ', D1 ' + G('demoDevice.keyp[2].plays'));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
}
{
  /* connect: a descriptor that does not come through (one lost reply) is not "the wrong firmware — flash the .bin" */
  const { G } = await loadPage('');
  G(`(() => { const d = new DEMO.FakeDevice({ latencyMs: 6, startIn: 0 }), h = d.handle.bind(d); let n = 0;
    d.handle = (t, b) => { if (t === 0x02 && n++ === 1) return null; return h(t, b); };
    demoDevice = d; demoPort = new DEMO.FakePort(d); window.__p = connectPort(demoPort, true); })()`);
  await G('__p').catch(() => {}); await sleep(50);
  check('review: a lost descriptor reply says connect again, not flash the firmware', G('link') === null && /connect again/.test(G("$('notice').textContent")) && !/Flash/.test(G("$('notice').textContent")), G("$('notice').textContent"));
}
{
  /* PADS: a drop in flight when the module goes belongs to the old session — after a reconnect it neither shows its
   * failure on the new one's pads nor (still decoding) talks to the new module */
  const { G } = await loadPage('?demo=sampler');
  await until(() => G('!!(st && table)')); await sleep(100);
  G(`window.__slow = 0; cardDecode = async f => { if (__slow) await new Promise(r => setTimeout(r, __slow)); const n = /long/.test(f.name) ? 240000 : 48000;
    return { numberOfChannels: 1, sampleRate: 48000, length: n, getChannelData: () => new Float32Array(n).map((_, i) => 0.3 * Math.sin(i / 5)) }; }`);
  const drop = name => fireEv(G('padEls[4].el'), 'drop', { dataTransfer: { files: [{ name, size: 10 }], types: ['Files'] } });
  drop('long.wav');
  await until(() => G("padJobs.length === 1 && padJobs[0].state === 'sending' && padJobs[0].frac > 0.2"), 15000);
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()'); await sleep(50);
  G("connectDemo('sampler')"); await until(() => G('!!(st && table)')); await sleep(300);
  check('review: a drop cut off by a disconnect leaves nothing on the next session\'s pads (no stale "✕", no message)', G('padJob[4]') === null && G("$('pd_msg').hidden") === true && !/not loaded/.test(G('padEls[4].sub.textContent')),
    G('padJob[4] && padJob[4].text') + ' / ' + G("$('pd_msg').textContent") + ' / ' + G('padEls[4].sub.textContent'));
  G('__slow = 1000'); drop('short.wav');
  await sleep(100);
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
  G("connectDemo('sampler')"); await until(() => G('!!(st && table)'));
  G(`demoDevice.padOps = []; (() => { const d = demoDevice, h = d.handle.bind(d); d.handle = (t, b) => { if (t === 0x68) d.padOps.push(b[0]); return h(t, b); }; })()`);
  await sleep(1600);
  check('review: …and a drop still decoding then sends nothing to the new module', G('demoDevice.padOps.length') === 0 && G('padJob[4]') === null, JSON.stringify(G('demoDevice.padOps')));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
}
{
  /* CHROMA: Spread pressed twice — the second (queued) finds the spread in place: one step on the module, one line */
  const { G } = await loadPage('?demo=keyp');
  await until(() => G('!!(st && keyp && table && st.bufLen)'));
  G("showTab('tab_kit')"); G('selectKey(7)'); G('invalidate(D.kit)'); await sleep(80);
  const d0 = G('st.undoDepth');
  fireEv(G("$('kp_spread')"), 'click', { clientX: 10, clientY: 10 }); fireEv(G("$('kp_spread')"), 'click', { clientX: 10, clientY: 10 });
  await sleep(100); await until(() => !G('ed.busy')); await sleep(400);
  check('review: Spread pressed twice — one step on the module, one history line', G('st.undoDepth') === d0 + 1 && G("ed.history.list().filter(x => /spread chromatically/.test(x.label)).length") === 1,
    'depth ' + d0 + ' → ' + G('st.undoDepth') + ' · ' + G("ed.history.list().map(x => x.label).join(' | ')"));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
}
/* ── 9c. Contrast, measured: every text token against every surface it sits on, in both themes, ≥ 4.5:1; and no
 *        colour outside the token blocks — not in a CSS rule, not in an inline style, not in the script ── */
{
  const html = readFileSync(path.join(here, 'index.html'), 'utf8'), T = cssThemes(html);
  const lum = hexc => { const h = hexc.replace('#', ''), v = h.length === 3 ? h.split('').map(c => c + c) : h.match(/../g); return v.map(x => parseInt(x, 16) / 255).map(c => c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4)).reduce((s, c, i) => s + c * [0.2126, 0.7152, 0.0722][i], 0); };
  const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
  const TEXT = ['ink', 'ink2', 'ink3', 'accent-ink', 'ok-ink', 'warn-ink', 'bad-ink', 'brain-ink', 'call-ink'], SURF = ['bg', 'panel', 'raise', 'btn', 'pad'];
  const PAIRS = [['on-accent', 'accent'], ['on-lock', 'lock'], ['warn-ink', 'warn-bg'], ['bad-ink', 'bad-bg'], ['cv-ink', 'cv-bg'], ['cv-ink2', 'cv-bg'], ['cv-ink2', 'cv-lane'], ['cv-ink', 'cv-lane'], ['cv-warn', 'cv-bg']];
  const bad = [];
  for (const [name, th] of [['dark', T.dark], ['light', T.light], ['light (system)', T.lightMedia]]) {
    for (const t of TEXT) for (const s of SURF) { const r = ratio(th[t], th[s]); if (!(r >= 4.5)) bad.push(`${name}: --${t} on --${s} ${r.toFixed(2)}`); }
    for (const [t, s] of PAIRS) { const r = ratio(th[t], th[s]); if (!(r >= 4.5)) bad.push(`${name}: --${t} on --${s} ${r.toFixed(2)}`); }
  }
  check('contrast: every text colour ≥ 4.5:1 on every surface, both themes (' + (TEXT.length * SURF.length + PAIRS.length) * 3 + ' pairs measured)', bad.length === 0, bad.slice(0, 8).join(' · '));
  check('contrast: the light theme set twice (the media query and the toggle) is the same set', JSON.stringify(T.light) === JSON.stringify(T.lightMedia));
  const rules = T.css.replace(/:root\{[^}]*\}/, '').replace(/:root:not\(\[data-theme="dark"\]\)\{[^}]*\}/, '').replace(/:root\[data-theme="light"\]\{[^}]*\}/, '');
  check('no colour outside the tokens: the CSS rules use var(--…) only', !/#[0-9a-fA-F]{3,8}\b/.test(rules), (rules.match(/.{30}#[0-9a-fA-F]{3,8}\b.{0,10}/) || [''])[0]);
  const script = [...html.matchAll(/<script>\n([\s\S]*?)<\/script>/g)].pop()[1];
  check('…none in the page script (the canvases read the tokens)', !/['"`]#[0-9a-fA-F]{3,8}\b/.test(script), (script.match(/.{30}['"`]#[0-9a-fA-F]{3,8}.{0,10}/) || [''])[0]);
  const inl = [...html.matchAll(/style="([^"]*)"/g)].map(m => m[1]).filter(s => /#[0-9a-fA-F]{3,8}\b|rgb/.test(s));
  check('…none in an inline style', inl.length === 0, inl.slice(0, 3).join(' | '));
  /* one element per id: the Play tab's output canvas was id="scope" beside main#scope until v0.13, so getElementById
   * found the canvas and the Scope tab never showed */
  const markup = html.slice(0, html.indexOf('<script src=')), idList = [...markup.matchAll(/ id="([^"]+)"/g)].map(m => m[1]), dup = idList.filter((x, i) => idList.indexOf(x) !== i);
  check('markup: no id is used twice (the Scope tab\'s panel is the only #scope)', dup.length === 0 && /<main id="scope">/.test(markup), dup.join(','));
  /* the phone layout, where a stub can tell: a scrolling tab row, the transport fixed under the thumb, 44 px targets, no column wider than the screen */
  check('phone layout: the tabs scroll, the transport sits at the bottom of a narrow screen, ▶ is ≥ 48 px, targets ≥ 44 px, grid columns can shrink',
    /nav\{[^}]*overflow-x:auto/.test(T.css) && /@media\(max-width:640px\)\{[\s\S]*?\.transport\{position:fixed;left:0;right:0;bottom:0/.test(T.css) && /\.tplay\{width:56px;height:56px/.test(T.css) && /\.tplay\{width:52px;height:52px/.test(T.css)
    && /@media \(max-width:700px\),\(pointer:coarse\)\{button,button\.mini,select,input:not\(\[type=checkbox\]\)\{min-height:44px\}/.test(T.css) && /main\.show\{display:grid;grid-template-columns:minmax\(0,1fr\)\}/.test(T.css) && /\.status\{[^}]*white-space:nowrap;overflow:hidden;text-overflow:ellipsis/.test(T.css) && /body\{[^}]*overflow-x:hidden/.test(T.css));
}

/* ── the Android path: Google's Web Serial polyfill (vendored, unmodified) and the install manifest ── */
{
  const here = new URL('.', import.meta.url);
  let m = null; try { m = await import(new URL('vendor/web-serial-polyfill/serial.js', here).href); } catch (e) { m = { err: e }; }
  check('polyfill imports', m && !m.err, m && m.err ? m.err.message : '');
  check('polyfill has requestPort/getPorts', m && m.serial && typeof m.serial.requestPort === 'function' && typeof m.serial.getPorts === 'function');
  check('polyfill SerialPort has the port surface', m && m.SerialPort && ['open', 'close'].every(k => typeof m.SerialPort.prototype[k] === 'function') && ['readable', 'writable'].every(k => Object.getOwnPropertyDescriptor(m.SerialPort.prototype, k)?.get));
  const fs = await import('node:fs');
  let man = null; try { man = JSON.parse(fs.readFileSync(new URL('manifest.webmanifest', here), 'utf8')); } catch {}
  check('manifest parses', man && man.name === 'Audiothurgist' && man.display === 'standalone' && man.start_url === './');
  check('manifest icons exist', man && man.icons.every(i => fs.existsSync(new URL(i.src, here))) && man.icons.some(i => i.sizes === '512x512'));
  const sw = fs.readFileSync(new URL('sw.js', here), 'utf8'); const shell = [...sw.matchAll(/'(\.\/[^']+)'/g)].map(x => x[1]).filter(x => x !== './');
  check('service worker shell files exist', shell.length >= 6 && shell.every(f => fs.existsSync(new URL(f, here))), shell.filter(f => !fs.existsSync(new URL(f, here))).join(','));
  const html = fs.readFileSync(new URL('index.html', here), 'utf8');
  const pub = fs.readFileSync(new URL('publish-pages.sh', here), 'utf8');
  check('publish stamp: the footer marker and the three script tags publish-pages.sh rewrites are in the page as it expects them', html.includes('<!--page-build--><b>local</b>') && ['hostlink', 'edit', 'demo'].every(s => html.includes(`<script src="${s}.js"></script>`) && pub.includes(s)) && /\?v=\$stamp/.test(pub));
  check('service worker: the cache version bumped for the beta, the query ignored in the offline fallback', /audiothurgist-v2/.test(sw) && /ignoreSearch: true/.test(sw));
  check('page links manifest and loads the polyfill only without navigator.serial', /rel="manifest"/.test(html) && /!navigator\.serial && navigator\.usb/.test(html) && /serialApi\(\)/.test(html));
  check('page has the Scope tab wired to the ten-channel body', /id="tab_scope"/.test(html) && /id="bigscope"/.test(html) && /fetchScopeFrame\(/.test(html) && /SCOPE_TAB_STATE_MS/.test(html) && /id="sc_cvchans"/.test(html) && /id="sc_p_gates"/.test(html));
  check('the page checks the firmware\'s api on connect, and nothing else of the descriptor (no capability gates left)', /HL\.apiOf\(desc\)/.test(html) && !/caps\(desc\)|cp\.\w+ >=|(edit|keys|pads|card|keyp|spread|start|sets|scope2|kit)Ok\b/.test(html));
}

check('no canvas call ever received NaN/Infinity', nanDraws.length === 0, nanDraws.slice(0, 5).join(' · '));
console.log(`${fail ? 'FAIL' : 'OK'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
