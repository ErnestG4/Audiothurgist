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
  check('hello', info.name === 'Audiothurgist' && info.maxBody === 512 && info.liveSize === DEMO.LIVE_SIZE && info.schema === DEMO.SCHEMA);
  const desc = await HL.getDescriptor(link, info);
  check('descriptor crc+json', desc && desc.dv === 1);
  const opts = HL.optionsInfo(desc);
  const wantIds = DEMO.OPTION_FIELDS.map(f => f[0]);
  check('options block: 17 enum fields with the firmware ids (Key Plays included)', opts && opts.fields.length === DEMO.OPTION_FIELDS.length && opts.fields.length === 17 && wantIds.every(id => opts.fields.some(f => f.id === id)) && opts.fields.some(f => f.id === 'key.plays'));
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
  /* ACTION 10: clear key 3 → the table shrinks, later keys re-index, key 3 falls back to its span slice. */
  await kl.request(HL.CMD.action, Uint8Array.of(HL.ACT.recClear, 3), { urgent: true });
  const st4 = HL.parseState(await kl.request(HL.CMD.state));
  check('rec clear: entry gone, keys re-pointed', st4.nTable === 67 && st4.keySlice[3] === 8 && st4.keySlice[7] === 64 && st4.keySlice[14] === 65 && st4.keySlice[5] === 66);
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
  const s4 = HL.parseState(await l.request(HL.CMD.state)); check('kit changed', s4.nTable === 66 && s4.keySlice[3] === 8);
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
}

console.log(`${fail ? 'FAIL' : 'OK'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
