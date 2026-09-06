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
  check('options block: 16 enum fields with the firmware ids', opts && opts.fields.length === 16 && wantIds.every(id => opts.fields.some(f => f.id === id)));
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
  await link.close();
}

console.log(`${fail ? 'FAIL' : 'OK'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
