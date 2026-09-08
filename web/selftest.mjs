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

/* ── 1. golden vectors (the SDK's hostlink_golden.json) ── */
const gArg = process.argv.indexOf('--golden');
const goldenPath = gArg > 0 ? process.argv[gArg + 1] : path.resolve(here, '..', '..', 'alchemy-sdk', 'tests', 'host', 'golden', 'hostlink_golden.json');
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

/* ── 2. StateExt parsing on synthetic bodies (v0.7.2 → CLOCK tails, 148/156/164/167/170) ── */
function stateBody(len) {
  const b = new Uint8Array(len); b[0] = 0; b[1] = 32; b[2] = 7; b[3] = 0b10110; b[4] = 9; b[5] = 2; b[6] = 1; b[7] = 3; b[8] = 200; b[9] = 5; b[10] = 2; b[11] = 255;
  const dv = new DataView(b.buffer); dv.setFloat32(12, 174.5, true); dv.setFloat32(16, 0.5, true);
  for (let i = 0; i < 64; i++) { b[20 + i] = i < 32 ? (i % 5 === 0 ? 0xff : i) : 0; b[84 + i] = i % 4; }
  if (len >= 156) { dv.setUint16(148, 1234, true); dv.setUint16(150, 210, true); dv.setUint16(152, 3, true); dv.setUint16(154, 17, true); }
  if (len >= 164) { dv.setUint16(156, 0x0041, true); dv.setFloat32(158, 172.25, true); b[162] = 2; b[163] = 3; }
  if (len >= 167) { dv.setUint16(164, 350, true); b[166] = 1; }
  if (len >= 170) { b[167] = 0x33; dv.setUint16(168, 120, true); }   /* engaged · pending · 3 ext transitions; 120 µs rms */
  if (len >= 175) { b[170] = 4; dv.setUint16(171, 1500, true); dv.setUint16(173, 2, true); }
  if (len >= 176) b[175] = 1;                                             /* Voice jack map */
  if (len >= 177) b[176] = 5;                                             /* LOOPER: five events */
  if (len >= 194) { b[177] = 2; b[178] = 7; dv.setUint32(179, 250000, true); dv.setUint32(183, 480000, true); b[187] = 1; b[188] = 180; b[189] = 35; dv.setUint32(190, 192000, true); }
  if (len >= 218) { for (let k = 0; k < 24; k++) b[194 + k] = k === 7 ? 34 : Math.floor(k * 32 / 24); }
  if (len >= 219) b[218] = 1;                                             /* Key Plays = Held */
  if (len >= 223) { b[219] = 1; b[220] = 128; b[221] = 2; b[222] = 9; }   /* SETS: saving, 50 %, last result card full, seq 9 */
  if (len >= 255) { const nm = 'Amen_kit-2'; for (let i = 0; i < nm.length; i++) b[223 + i] = nm.charCodeAt(i); }
  if (len >= 258) { b[255] = 77; b[256] = 3; b[257] = 255; }              /* EDIT: tableGen 77, undo depth 3, no selection */
  if (len >= 262) dv.setUint32(258, 123456, true);                        /* EDIT: srcPos */
  return b;
}
{
  check('state too short → null', HL.parseState(stateBody(100)) === null);
  const s0 = HL.parseState(stateBody(148));
  check('state v0.7.2 fixed part', s0 && s0.nSlices === 32 && s0.cur === 7 && s0.ext && s0.locked && !s0.muted && !s0.flowing && s0.brain && s0.grammar === 2 && s0.cls === 1 && s0.file === 3 && s0.chop === 200 && s0.histN === 5 && s0.histDepth === 2 && s0.callFlash === 255);
  check('state floats', Math.abs(s0.bpm - 174.5) < 1e-4 && s0.mult === 0.5);
  check('state src signed', s0.src[0] === -1 && s0.src[1] === 1 && s0.src[5] === -1 && s0.stepClass[7] === 3);
  check('state no tails', s0.diag === null && s0.lockMask === null && s0.jitterUs === null);
  const s1 = HL.parseState(stateBody(156));
  check('state diag tail', s1.diag && s1.diag.gap === 1234 && s1.diag.audio === 210 && s1.diag.jumps === 3 && s1.diag.regens === 17 && s1.lockMask === null);
  const s2 = HL.parseState(stateBody(164));
  check('state v0.8.0 tail', s2.lockMask === 0x41 && Math.abs(s2.nativeBpm - 172.25) < 1e-4 && s2.gridBars === 2 && s2.barsOpt === 3 && s2.jitterUs === null);
  const s3 = HL.parseState(stateBody(167));
  check('state CLOCK tail', s3.jitterUs === 350 && s3.multZone === 1 && s3.clkFlags === null && s3.jitterRmsUs === null && s3.multPending === null);
  const s4 = HL.parseState(stateBody(170));
  check('state CLOCK flags tail', s4.clkFlags === 0x33 && s4.jitterRmsUs === 120 && s4.p2Engaged === true && s4.multPending === true && s4.aligned === false && s4.extTrans === 3);
  check('state longer body still parses', HL.parseState(stateBody(180)).jitterRmsUs === 120);
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
  /* SAMPLE / WEB tails: 176 (looper), 177..193 (record + buffer), 194..217 (key table). */
  const s5 = HL.parseState(stateBody(176));
  check('state 176: jack map, no looper', s5.jackMap === 1 && s5.looperEvents === undefined && s5.recState === undefined && s5.keySlice === undefined);
  const s6 = HL.parseState(stateBody(177));
  check('state 177: looper events, no record tail', s6.looperEvents === 5 && s6.recState === undefined);
  check('state 180: record tail needs all 17 bytes', HL.parseState(stateBody(180)).recState === undefined);
  const s7 = HL.parseState(stateBody(194));
  check('state 194: record + buffer tail', s7.recState === 2 && HL.REC[s7.recState] === 'taking' && s7.recKey === 7 && s7.freePos === 250000 && s7.bufCap === 480000 && s7.stereo === true && s7.inLevel === 180 && s7.nTable === 35 && s7.bufLen === 192000 && s7.keySlice === undefined);
  const s8 = HL.parseState(stateBody(218));
  check('state 218: key table', s8.keySlice && s8.keySlice.length === 24 && s8.keySlice[7] === 34 && s8.keySlice[23] === 30 && s8.keySlice[0] === 0 && s8.keyPlays === undefined);
  check('state 219: key plays', HL.parseState(stateBody(219)).keyPlays === 1);
  /* SETS tail: 219..222 the op, 223..254 the name. */
  const s9 = HL.parseState(stateBody(219)); check('state 219: no sets tail', s9.setState === undefined && s9.setName === undefined);
  const s10 = HL.parseState(stateBody(223));
  check('state 223: set op tail', s10.setState === 1 && HL.SET_STATE[s10.setState] === 'saving' && s10.setProgress === 128 && s10.setResult === 2 && HL.SET_RESULT[s10.setResult] === 'card full' && s10.setSeq === 9 && s10.setName === undefined);
  const s11 = HL.parseState(stateBody(255)); check('state 255: set name', s11.setName === 'Amen_kit-2');
  { const b = stateBody(255); b[223] = 0; check('state 255: empty name', HL.parseState(b).setName === ''); }
  check('set names', HL.setNameOk('amen') && HL.setNameOk('Amen_170-v2') && HL.setNameOk('a'.repeat(31)) && !HL.setNameOk('') && !HL.setNameOk('a'.repeat(32)) && !HL.setNameOk('a b') && !HL.setNameOk('a/b') && !HL.setNameOk('a.wav') && !HL.setNameOk(null));
  check('set labels', HL.setLabelName('amen [set]') === 'amen' && HL.setLabelName('AMEN_170.WAV') === null && HL.setLabelName('a b [set]') === null && HL.setLabelName('sets/amen.wav') === null);
  check('name bytes', HL.nameBytes('ab').length === 2 && HL.nameBytes('ab')[0] === 97 && HL.ACT.saveSet === 14 && HL.ACT.loadSet === 15);
  { const b = stateBody(218); b[178] = 255; check('recKey 255 → -1', HL.parseState(b).recKey === -1); }
  /* GET_WAVE / GET_TABLE / GET_HITS bodies. */
  const wv = HL.parseWave(Uint8Array.of(0, 0x10, 0x27, 0, 0, 3, 0, 4, 0x7f, 0x81, 0));
  check('wave body', wv.start === 10000 && wv.n === 3 && wv.step === 4 && wv.data.length === 3 && wv.data[0] === 127 && wv.data[1] === -127 && wv.data[2] === 0);
  { const t = new Uint8Array(3 + 2 * HL.TABLE_ENTRY); t[0] = 0; t[1] = 32; t[2] = 34; const dv = new DataView(t.buffer);
    dv.setUint32(3, 100, true); dv.setUint32(7, 6000, true); t[11] = 0xff; t[12] = 0; t[13] = 240; t[14] = 1; t[15] = 60;
    dv.setUint32(16, 192000, true); dv.setUint32(20, 48000, true); t[24] = 7; t[25] = 2; t[26] = 255; t[27] = 0; t[28] = 210;
    const tb = HL.parseTableChunk(t);
    check('table chunk', tb.nSlices === 32 && tb.nTable === 34 && tb.entries.length === 2 && tb.entries[0].key === -1 && tb.entries[0].start === 100 && tb.entries[0].len === 6000 && tb.entries[0].hit === true && tb.entries[0].sal === 240 && tb.entries[1].key === 7 && tb.entries[1].cls === 2 && tb.entries[1].hit === false && tb.entries[1].dens === 210);
    check('table chunk: 39 entries fit 512', 3 + 39 * HL.TABLE_ENTRY <= 512 && 3 + 40 * HL.TABLE_ENTRY > 512); }
  { const h = new Uint8Array(3 + 8); h[0] = 0; h[1] = 130; h[2] = 128; new DataView(h.buffer).setUint32(3, 1, true); new DataView(h.buffer).setUint32(7, 0xfffffffe, true);
    const hc = HL.parseHitsChunk(h); check('hits chunk', hc.total === 130 && hc.first === 128 && hc.positions.length === 2 && hc.positions[1] === 0xfffffffe); }
  check('key names: two octaves from C', HL.keyName(0) === 'C1' && HL.keyName(11) === 'B1' && HL.keyName(12) === 'C2' && HL.keyName(23) === 'B2' && HL.keyName(1) === 'C♯1');
}

/* ── 3. The Link end to end over the fake module: priority, coalescing, RTT, live blob ── */
const dev = new DEMO.FakeDevice({ tail: 170, latencyMs: 3 });
const link = new HL.Link(new DEMO.FakePort(dev));
await link.start();
{
  const info = await HL.hello(link);
  check('hello', info.name === 'Audiothurgist' && info.maxBody === DEMO.MAX_BODY && info.maxBody === 1024 && info.liveSize === DEMO.LIVE_SIZE && info.schema === DEMO.SCHEMA);
  const desc = await HL.getDescriptor(link, info);
  check('descriptor crc+json', desc && desc.dv === 1);
  const opts = HL.optionsInfo(desc);
  const wantIds = DEMO.OPTION_FIELDS.map(f => f[0]);
  check('options block: 18 enum fields with the firmware ids (Key Plays and Tap Allowance included)', opts && opts.fields.length === DEMO.OPTION_FIELDS.length && opts.fields.length === 18 && opts.fields.some(f => f.id === 'tap.allow') && wantIds.every(id => opts.fields.some(f => f.id === id)) && opts.fields.some(f => f.id === 'key.plays'));
  const gl = HL.fieldLabels(desc, 'grammar.mode');
  check('labels from disp', gl[3] === 'Euclid' && HL.fieldLabels(desc, 'sample.idx').length === DEMO.FILES.length);
  check('grammar has five labels, index 4 = Camp', gl.length === 5 && gl[4] === 'Camp' && opts.fields.find(f => f.id === 'grammar.mode').zones === 5);
  const pg = HL.pagerInfo(desc); check('pager: 2 pages × 6 knobs with help', pg && pg.pages === 2 && Object.keys(pg.knobs).length === 12 && /Grids/.test(pg.knobs['1:0'].help));
  const man = HL.manualInfo(desc); check('manual block', man && man.sections.length === 3 && man.sections.some(s => s.id === 'camp') && man.jacks.length === 10 && man.tagline);

  /* Priority: queue five polls, then an urgent action → the action reaches the device before the polls still queued. */
  dev.log.length = 0;
  const polls = []; for (let i = 0; i < 5; i++) polls.push(link.request(HL.CMD.state));
  const act = link.request(HL.CMD.action, Uint8Array.of(HL.ACT.mute), { urgent: true });
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
  /* State through the real parser, all tails. */
  const st = HL.parseState(await link.request(HL.CMD.state));
  check('demo state parses with CLOCK tail', st && st.muted && st.nSlices === 32 && st.nativeBpm === 120 && st.gridBars === 2 && st.multZone === 2 && st.jitterUs === 0);
  check('demo state carries the 170-byte flags', st.clkFlags === 0 && st.jitterRmsUs === 0 && st.p2Engaged === false && st.extTrans === 0);
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
  check('demo 170-byte body carries no sample tail', st.recState === undefined && st.keySlice === undefined && st.setState === undefined);
  await link.close();
}

/* ── 4. The Kit: table / wave / hits and the record + looper actions over a 64-slice kit demo ── */
{
  const kit = new DEMO.FakeDevice({ tail: 219, kit: true, nSlices: 64, latencyMs: 2, swing: true });   /* 219: the pre-SETS body */
  const kl = new HL.Link(new DEMO.FakePort(kit)); await kl.start();
  const info = await HL.hello(kl); const desc = await HL.getDescriptor(kl, info);
  check('kit demo advertises wave/table/hits', desc.audiothurgist.wave === 100 && desc.audiothurgist.table === 101 && desc.audiothurgist.hits === 102);
  const s = HL.parseState(await kl.request(HL.CMD.state));
  check('kit state tail', s.nSlices === 64 && s.nTable === 67 && s.bufLen === 64 * DEMO.SLICE_LEN && s.freePos > s.bufLen && s.bufCap === DEMO.BUF_CAP && s.recState === 0 && s.recKey === -1 && s.looperEvents === 3 && s.keySlice.length === 24 && s.keyPlays === 0);
  check('kit keys: recorded keys point past the grid, others at their span slice', s.keySlice[3] === 64 && s.keySlice[7] === 65 && s.keySlice[14] === 66 && s.keySlice[0] === 0 && s.keySlice[23] === 61);
  /* GET_TABLE chunks: 67 entries = 39 + 28. */
  kit.log.length = 0;
  const tb = await HL.fetchTable(kl);
  check('table fetched in two chunks', kit.log.filter(t => t === HL.CMD.table).length === 2 && tb.nSlices === 64 && tb.nTable === 67 && tb.entries.length === 67);
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
  check('kit demo 219-byte body carries no sets tail', HL.parseState(await kl.request(HL.CMD.state)).setState === undefined);
  await kl.close();
}

/* ── 5. Sets over the demo (the 255-byte body): save, refusals, load, not found ── */
{
  const d = new DEMO.FakeDevice({ tail: 255, kit: true, nSlices: 64, latencyMs: 2, swing: true });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l); const desc = await HL.getDescriptor(l, info);
  check('demo advertises sets', desc.audiothurgist.sets === 1);
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
  check('the saved set joined the list at the end', HL.fieldLabels(await HL.getDescriptor(l, await HL.hello(l)), 'sample.idx').slice(-1)[0] === 'kit_1 [set]' && s3.file === labs.length);
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

/* ── 6. SCOPE over the demo: the old reply unchanged, the four-channel body, trigger, at-pos frames, BAD_ARGS, the split plan at 512 ── */
{
  const d = new DEMO.FakeDevice({ tail: 255, latencyMs: 1 }); d.t0 -= 2000;   /* two seconds of signal in the rings */
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l); const desc = await HL.getDescriptor(l, info);
  check('demo advertises scope2 = 2 (jacks) and max_body 1024', desc.audiothurgist.scope2 === 2 && desc.audiothurgist.scope === 98 && info.maxBody === 1024);
  /* the empty body: the pre-SCOPE reply, 256 × ×4 of the mono sum */
  const old = await l.request(HL.CMD.scope);
  const os = HL.parseScope(old);
  check('empty body → the old reply (u16 256, 256 × i8)', old.length === 259 && HL.u16(old, 1) === 256 && os.length === 256 && os.some(v => v !== 0));
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
  /* the split plan (a 512-byte module): two requests of two channels pinned to one pos */
  const d5 = new DEMO.FakeDevice({ tail: 255, latencyMs: 1, maxBody: 512 }); d5.t0 -= 2000;
  const l5 = new HL.Link(new DEMO.FakePort(d5)); await l5.start(); const i5 = await HL.hello(l5);
  check('a 512-byte module says so in HELLO', i5.maxBody === 512);
  d5.log.length = 0;
  const p5 = HL.scopePlan(i5.maxBody, 0x0f, 4), f5 = await HL.fetchScopeFrame(l5, p5, 4, HL.SCOPE.trig);
  check('512: a four-channel frame is two requests of 251, both ≤ 512 bytes', f5.reqs === 2 && f5.n === 251 && d5.log.filter(t => t === HL.CMD.scope).length === 2 && f5.bytes === 2 * (10 + 2 * 251) && Object.keys(f5.ch).length === 4);
  check('512: the second request was pinned to the first pos (channels aligned)', f5.triggered && f5.ch[2].length === 251 && f5.ch[3].length === 251);
  /* a pos the ring no longer holds is not honoured → the frame is reported torn */
  { let bad = null; try { const t = HL.parseScope2(await l5.request(HL.CMD.scope, HL.scopeReq(0x0c, 4, 251, 0, f5.pos >= 100000 ? f5.pos - 100000 : f5.pos + HL.SCOPE.wrap - 100000))); if (!t.atHonoured) bad = 'not honoured'; } catch (e) { bad = e.message; } check('a stale pos is answered but not honoured', bad === 'not honoured'); }
  await l5.close();
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
  const dq = new DEMO.FakeDevice({ tail: 255, latencyMs: 1, inputs: false }); dq.t0 -= 2000;
  const lq = new HL.Link(new DEMO.FakePort(dq)); await lq.start();
  const fq = await HL.fetchScopeFrame(lq, HL.scopePlan(1024, 0x0f, 4), 4, HL.SCOPE.trig);
  check('noin: inputs flat, outputs alive', fq.ch[2].every(v => v === 0) && fq.ch[3].every(v => v === 0) && fq.ch[0].some(v => v !== 0));
  /* muted: the outputs fall silent, no trigger on out L, the inputs carry on */
  d.muted = true; const fm = await HL.fetchScopeFrame(l, plan, 4, HL.SCOPE.trig);
  check('muted: outputs flat and untriggered, inputs still there', fm.ch[0].every(v => v === 0) && !fm.triggered && fm.ch[2].some(v => v !== 0));
  d.muted = false;
  /* and the Play tab's page-side path: the old request still coalesces on its key beside the new ones */
  d.log.length = 0; await Promise.all([l.request(HL.CMD.scope, undefined, { key: 'scope' }), l.request(HL.CMD.scope, undefined, { key: 'scope' }), l.request(HL.CMD.scope, HL.scopeReq(0x0f, 4, 253), { key: 'scope15' })]);
  check('old and new scope requests keep their own keys', d.log.filter(t => t === HL.CMD.scope).length === 2);
  await lq.close(); await l.close();
}

/* ── 7. EDIT: the tail and table bodies, then every op over the fake module — OK / BAD_ARGS / BAD_STATE, tableGen,
 *       the 14-byte table, tombstones, keys, FROM_ONSETS, the audition voice, TAP_MARK, and an older firmware ── */
{
  /* the synthetic bodies: old lengths still parse, the new bytes appear at 258 and 262 */
  const e0 = HL.parseState(stateBody(255)); check('state 255: no edit tail', e0.setName === 'Amen_kit-2' && e0.tableGen === undefined && e0.srcPos === undefined);
  check('state 257: the three bytes need 258', HL.parseState(stateBody(257)).tableGen === undefined);
  const e1 = HL.parseState(stateBody(258)); check('state 258: tableGen, undoDepth, selSlice −1; no srcPos', e1.tableGen === 77 && e1.undoDepth === 3 && e1.selSlice === -1 && e1.srcPos === undefined);
  const e2 = HL.parseState(stateBody(262)); check('state 262: srcPos', e2.srcPos === 123456 && e2.tableGen === 77);
  { const b = stateBody(262); b[257] = 9; check('selSlice 9', HL.parseState(b).selSlice === 9); }
  check('state 300: a longer body still parses', HL.parseState(stateBody(300)).srcPos === 123456);
  { const t = new Uint8Array(3 + 2 * HL.TABLE_ENTRY_EDIT); t[0] = 0; t[1] = 32; t[2] = 34; const dv = new DataView(t.buffer);
    dv.setUint32(3, 100, true); dv.setUint32(7, 6000, true); t[11] = 0xff; t[12] = 0; t[13] = 240; t[14] = 1; t[15] = 60; t[16] = HL.EDIT.orange;
    dv.setUint32(17, 192000, true); dv.setUint32(21, 0, true); t[25] = 0xff; t[26] = 2; t[27] = 255; t[28] = 0; t[29] = 210; t[30] = HL.EDIT.tomb;
    const tb = HL.parseTableChunk(t, HL.TABLE_ENTRY_EDIT);
    check('table chunk, 14-byte form: flags read, the rest as before', tb.entries.length === 2 && tb.entries[0].flags === HL.EDIT.orange && tb.entries[0].len === 6000 && tb.entries[0].key === -1 && tb.entries[1].flags === HL.EDIT.tomb && tb.entries[1].len === 0 && tb.entries[1].cls === 2);
    check('table chunk, 13-byte form: flags 0', HL.parseTableChunk(t.subarray(0, 3 + 2 * HL.TABLE_ENTRY)).entries[0].flags === 0);
    check('39 × 14 fit max_body 1024', 3 + 39 * HL.TABLE_ENTRY_EDIT <= 1024 && HL.TABLE_CHUNK === 39); }
  check('ACT: the edit ops 16..29', HL.ACT.setBoundary === 16 && HL.ACT.fromOnsets === 26 && HL.ACT.audition === 27 && HL.ACT.release === 28 && HL.ACT.tapMark === 29 && HL.EDIT.orange === 1 && HL.EDIT.tomb === 2);

  const SL = DEMO.SLICE_LEN;
  const d = new DEMO.FakeDevice({ kit: true, nSlices: 32, latencyMs: 1 });
  const l = new HL.Link(new DEMO.FakePort(d)); await l.start();
  const info = await HL.hello(l); const desc = await HL.getDescriptor(l, info);
  const state = async () => HL.parseState(await l.request(HL.CMD.state));
  const table = () => HL.fetchTable(l, { edit: true });
  check('demo advertises edit', desc.audiothurgist.edit === 1 && desc.audiothurgist.sets === 1);
  const s0 = await state();
  check('tail: tableGen ≥ 1, undoDepth 0, selSlice −1, srcPos inside the material', s0.tableGen >= 1 && s0.undoDepth === 0 && s0.selSlice === -1 && s0.srcPos >= 0 && s0.srcPos < s0.bufLen && s0.nTable === 35);
  const t0 = await table();
  check('14-byte form: 32 grid + 3 recordings, every flag white', t0.entries.length === 35 && t0.entries.every(e => e.flags === 0) && t0.entries[32].key === 3);
  const t13 = await HL.fetchTable(l);
  check('the 13-byte form still answers, flags 0', t13.entries.length === 35 && t13.entries[1].start === t0.entries[1].start && t13.entries[32].flags === 0);
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
  /* TAP_MARK: the fake's position snapshot pinned — allowance, onset snap, the length to the next onset, orange */
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
  /* an older firmware (noedit): no capability, the 13-byte table whatever is asked, the 255-byte body, ops unknown */
  const o = new DEMO.FakeDevice({ kit: true, nSlices: 32, latencyMs: 1, edit: false });
  const ol = new HL.Link(new DEMO.FakePort(o)); await ol.start();
  const oi = await HL.hello(ol); const od = await HL.getDescriptor(ol, oi);
  check('noedit: no "edit" capability, sets still there', od.audiothurgist.edit === undefined && od.audiothurgist.sets === 1);
  const os = await state.call(null).catch(() => null) || HL.parseState(await ol.request(HL.CMD.state));
  const os2 = HL.parseState(await ol.request(HL.CMD.state));
  check('noedit: the 255-byte body — no tableGen, no srcPos, the set name still parses', os2.tableGen === undefined && os2.srcPos === undefined && os2.setName === '' && os2.keySlice.length === 24);
  const raw = await ol.request(HL.CMD.table, Uint8Array.of(0, 39, 1));
  check('noedit: form 1 is ignored — 13-byte entries', (raw.length - 3) % 13 === 0 && (raw.length - 3) / 13 === 35);
  check('noedit: the page\'s 13-byte fetch parses it', (await HL.fetchTable(ol)).entries.length === 35);
  check('noedit: ACTION 16 / 27 / 29 → BAD_ARGS (unknown op)', (await HL.editBoundary(ol, 5, 100)).status === 2 && (await HL.audition(ol, 0, 100)).status === 2 && (await HL.tapMark(ol, 10, 20)).status === 2);
  void os;
  await ol.close();
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
    h.push(8, 'a'); h.push(9, 'b'); check('History: newest first, capped', h.list().map(x => x.label).join('|') === 'b|a|split 7' && h.items.length === 3 && h.note(9, 4) === false && h.undoDepth === 4); }
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
  const refetch = async () => { Hh.st = HL.parseState(await l.request(HL.CMD.state)); Hh.table = await HL.fetchTable(l, { edit: true }); Hh.hits = await HL.fetchHits(l); };
  await refetch(); Hh.view.a = 0; Hh.view.b = Hh.st.bufLen;
  let clock = 0; const timers = []; const now = () => clock;
  const setTimer = (f, ms) => { const id = { f, at: clock + ms }; timers.push(id); return id; }, clearTimer = id => { const i = timers.indexOf(id); if (i >= 0) timers.splice(i, 1); };
  const fire = () => { for (const t of timers.splice(0)) if (t.at <= clock) t.f(); else timers.push(t); };
  let editOk = true;
  const ed = new ED.Editor({ link: () => l, view: Hh.view, now, setTimer, clearTimer, gesture: { holdMs: 60, dblMs: 200 },
    ctx: () => ({ table: Hh.table, st: Hh.st, hits: Hh.hits, sampleAt: null, editOk, tapAllowMs: 40 }),
    cb: { redraw: () => Hh.redraws++, status: (m, k) => Hh.status.push(m), select: t => { Hh.sel = t; }, refetch,
      setView: (a, b) => { Hh.view.a = Math.round(a); Hh.view.b = Math.round(b); }, zoomView: (f, at) => { const s = (Hh.view.b - Hh.view.a) * f; Hh.view.a = Math.round(at - (at - Hh.view.a) * f); Hh.view.b = Hh.view.a + Math.round(s); },
      fitEntry: t => { Hh.fit = t; }, menu: (items, x, y, t) => Hh.menus.push({ items, t }), choose: async plan => { Hh.plans.push(plan); return Hh.answer || plan.def; }, dropTarget: () => Hh.dropKey } });
  const W = 600, HH = 290, px = p => p / Hh.st.bufLen * W;
  const L = ed.layout(W, HH);
  check('layout: wave / a 44 px handle row / the strip', L.edit && L.wave[1] === 200 && L.handles[0] === 200 && L.handles[1] === 244 && L.strip[0] === 244 && ed.zoneAt(100, L) === 'wave' && ed.zoneAt(220, L) === 'handles' && ed.zoneAt(260, L) === 'strip');
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
  d.srcPos = () => 8 * SL + 1000; sent.length = 0; const rt = await ed.tapMark(); await settle();
  check('the Chop pad: TAP_MARK(60, 40 ms from the option), gen unchecked, an orange marker, "tap → marker N"', rt.ok && sent.some(b => b[0] === 29 && b[1] === 60 && b[2] === 40 && b.length === 3) && Hh.table.entries[rt.newIdx].flags === HL.EDIT.orange && ed.history.list()[0].label === `tap → marker ${rt.newIdx}`);
  Hh.status.length = 0; const ru = await ed.undo(); await settle();
  check('undo on a firmware without package 6: BAD_ARGS, said, nothing else', ru.status === 2 && /BAD_ARGS/.test(Hh.status[Hh.status.length - 1]));
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
  /* an older firmware: the wave alone, drag = pan, tap = select, no split, no handles */
  editOk = false; const L0 = ed.layout(W, 180); ed.select(-1);
  check('noedit layout: no handle row, the whole canvas pans', !L0.edit && L0.wave[1] === 180 && L0.handles[0] === L0.handles[1] && L0.strip[0] === L0.strip[1] && ed.zoneAt(170, L0) === 'wave' && ed.zoneAt(20, L0) === 'wave');
  sent.length = 0; Hh.view.a = 0; Hh.view.b = span0; const mx = px(Hh.table.entries[m1].start + 1000);
  P('down', 1, mx, 90, { h: 180 }); clock += 10; P('move', 1, mx + 30, 90, { h: 180 }); P('up', 1, mx + 30, 90, { h: 180 });
  check('noedit: a drag on the wave pans, no drag-to-key', Hh.view.a < 0 && !ed.drag);
  Hh.view.a = 0; Hh.view.b = span0; P('down', 1, mx, 90, { h: 180 }); clock += 20; P('up', 1, mx, 90, { h: 180 }); await settle();
  check('noedit: a tap inside a marker selects, never splits', ed.selT === m1 && !sent.some(b => b[0] === 19));
  check('noedit: no handles are offered from the wave', ed.tagAt(px(5 * SL), 170, W, 180).h === null);
  await l.close();
}

/* ── 9. EDIT-UI, the page itself: index.html's inline script under a DOM stub, connected to the fake module through
 *       `?demo=kit`, then the DOM paths driven with synthetic pointer events — the handle drag from the canvas, the
 *       key cells' tap / drag / swap prompt, the Chop pad, the field, and `?demo=noedit`'s view-only fallback ── */
import vm from 'node:vm';
const ED9 = require('./edit.js');
function domStub(html) {
  const ids = new Set([...html.matchAll(/ id="([^"]+)"/g)].map(m => m[1]));
  const errors = [], warns = [];
  class El {
    constructor(tag, id) { this.tagName = String(tag).toUpperCase(); this.id = id || ''; this.className = ''; this.style = {}; this.dataset = {}; this.hidden = false; this.disabled = false; this.value = ''; this._text = ''; this._html = ''; this._on = {}; this.children = []; this._q = new Map(); this._parent = null; this._calls = 0; this.offsetWidth = 200; this.offsetHeight = 100; this.width = 0; this.height = 0; this._w = 600; this._h = 290; this.checked = false; this.title = ''; this.textContentSet = 0; }
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
    getContext() { const el = this; return new Proxy({}, { get: (o, k) => { if (k === 'measureText') return () => ({ width: 10 }); if (typeof k === 'string' && k in o) return o[k]; return (...a) => { el._calls++; }; }, set: (o, k, v) => { o[k] = v; return true; } }); }
  }
  const byId = new Map();
  const document = {
    getElementById: id => { if (!ids.has(id)) throw new Error('no element with id ' + id); if (!byId.has(id)) byId.set(id, new El(/canvas id="/.test(html.slice(html.indexOf(`id="${id}"`) - 12, html.indexOf(`id="${id}"`))) ? 'canvas' : 'div', id)); return byId.get(id); },
    createElement: tag => new El(tag), addEventListener: () => {}, removeEventListener: () => {}, querySelectorAll: () => [], visibilityState: 'visible', activeElement: null, _at: null,
    elementFromPoint: () => document._at,
  };
  const sandbox = { document, console: { log: () => {}, warn: (...a) => warns.push(a.join(' ')), error: (...a) => errors.push(a.join(' ')) }, errors, warns,
    navigator: { userAgent: 'node', usb: undefined, serial: undefined }, location: { search: '', hash: '', protocol: 'http:', href: 'http://localhost/' }, history: { replaceState: () => {} },
    performance, TextDecoder, TextEncoder, ReadableStream, WritableStream, Uint8Array, Int8Array, Uint32Array, Float32Array, DataView, Promise, Map, Set, Math, JSON, Number, String, Array, Object, Error, Date, isNaN, parseInt, parseFloat, RegExp, Symbol,
    setTimeout: (f, ms) => setTimeout((...a) => { try { f(...a); } catch (e) { errors.push('timer: ' + (e.stack || e)); } }, ms), clearTimeout, setInterval: (f, ms) => { const t = setInterval(() => { try { f(); } catch (e) { errors.push('interval: ' + (e.stack || e)); } }, ms); t.unref(); return t; }, clearInterval,
    requestAnimationFrame: f => setTimeout(() => { try { f(performance.now()); } catch (e) { errors.push('rAF: ' + (e.stack || e)); } }, 16),
    confirm: () => true, alert: () => {}, isSecureContext: true, devicePixelRatio: 1, addEventListener: () => {}, HL, DEMO, ED: ED9 };
  sandbox.window = sandbox; sandbox.globalThis = sandbox;
  return { sandbox, errors, warns, El, byId };
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
  check('page: with "edit" the editor is on — the panel, the field row, the status pill, the 290 px canvas; the 14-byte form was asked for', G('editOk') === true && !G("$('editor').hidden") && !G("$('ed_field').hidden") && G("$('kitwave').style.height") === '290px' && G('table.entries[0].flags') === 0 && G('demoDevice.log').includes(HL.CMD.table));
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
  /* nudge; undo stays disabled (undoDepth 0); keys follow the grid asks (confirm → true) */
  const b6 = G('demoDevice.table[6].start'); G("$('ed_nudge_p').onclick()");
  check('page: nudge all +5 ms', await until(() => G('demoDevice.table[6].start') === b6 + 240));
  check('page: Undo / Redo disabled while undoDepth is 0, the depth line says why', G("$('ed_undo').disabled") === true && G("$('ed_redo').disabled") === true && /no journal/.test(G("$('ed_depth').textContent")));
  G("$('ed_follow').onclick()");
  check('page: keys follow the grid (confirmed, queued behind the nudge\'s reply) → every key on its span', await until(() => { const ks = G('Array.from(demoDevice.keySlice)'); return ks.every((t, k) => t === ED9.spanSlice(k, 32)); }, 6000));
  check('page: no runtime errors or link warnings through all of it', D.errors.length === 0 && !D.warns.some(w => /table|state|hits/.test(w)), D.errors.concat(D.warns).join(' | ').slice(0, 400));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
  check('page: disconnect resets the editor and hides the panel', G('editOk') === false && G("$('editor').hidden") === true && G('ed.selT') === -1);
}
{
  const { D, G } = await loadPage('?demo=noedit,kit');
  check('noedit page: loads, connects', D.errors.length === 0 && await until(() => G('table && st')), D.errors.join(' | ').slice(0, 300));
  await sleep(120);
  check('noedit page: no editor — the panel hidden, no field, the 180 px canvas, the 13-byte table, no handles, no Chop', G('editOk') === false && G("$('editor').hidden") === true && G("$('ed_field').hidden") === true && G("$('kitwave').style.height") === '180px' && G('ed.layout(600, 180).edit') === false && G('demoDevice.table[0].flags') === 0 && G('table.entries[32].flags') === 0);
  const kw = G("$('kitwave')"); kw._h = 180; G('view.a = 0; view.b = st.bufLen'); await sleep(30); const a0 = G('view.a');
  fireEv(kw, 'pointerdown', { clientX: 300, clientY: 90 }); for (let i = 1; i <= 15; i++) fireEv(kw, 'pointermove', { clientX: 300 - 2 * i, clientY: 90 }); fireEv(kw, 'pointerup', { clientX: 270, clientY: 90 });
  check('noedit page: a drag on the wave pans, exactly as before; nothing was sent', G('view.a') > a0 && !G('demoDevice.log').slice(-40).some(t => t === HL.CMD.action));
  check('noedit page: no runtime errors', D.errors.length === 0, D.errors.join(' | ').slice(0, 300));
  await G('(async () => { const l = link; setDisconnected(); await l.close(); })()');
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
  check('page links manifest and loads the polyfill only without navigator.serial', /rel="manifest"/.test(html) && /!navigator\.serial && navigator\.usb/.test(html) && /serialApi\(\)/.test(html));
  check('page has the Scope tab wired to scope2', /id="tab_scope"/.test(html) && /id="bigscope"/.test(html) && /desc\.audiothurgist\.scope2/.test(html) && /fetchScopeFrame\(/.test(html) && /SCOPE_TAB_STATE_MS/.test(html) && /id="sc_cvchans"/.test(html) && /id="sc_p_gates"/.test(html) && /scope2 >= 2/.test(html));
}

console.log(`${fail ? 'FAIL' : 'OK'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
