/* hostlink.js — Audiothurgist's wire layer.
 *
 * HostLink v1 framing (CRC32 / COBS / frame header, a faithful port of
 * alchemy-sdk's tools/hostlink-cli/hostlink.mjs), a single-outstanding-
 * request Link with a priority queue and round-trip measurement, the
 * standard commands the page needs (HELLO, GET_DESCRIPTOR, GET_LIVE, live
 * BLOB writes) and the firmware's StateExt parsers (0x60–0x63).
 *
 * Plain script in the browser (globals via window.HL); CommonJS in node so
 * selftest.mjs can pin it to the SDK's golden vectors. No dependencies.
 */
(function (root) {
'use strict';

const PROTO = 1;
const CMD = {
  hello: 0x01, getDescriptor: 0x02, blobBegin: 0x12, blobData: 0x13, blobCommit: 0x14,
  getLive: 0x20, reboot: 0x40,
  state: 0x60, env: 0x61, scope: 0x62, action: 0x63,
};
const ACT = { dice: 0, reslice: 1, mute: 2, punch: 3, flow: 4, scrub: 5, load: 6, clearLocks: 7, page: 8 };
const STATUS = ['OK', 'UNSUPPORTED', 'BAD_ARGS', 'BAD_STATE', 'BAD_CRC', 'BAD_SLOT', 'TOO_LARGE',
  'SCHEMA_MISMATCH', 'FLASH_FAIL', 'BUSY', 'FRAME_ERROR'];

/* ───────────── codecs ───────────── */
const CRC_TABLE = (() => { const t = new Uint32Array(256); for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[i] = c >>> 0; } return t; })();
function crc32(buf, seed = 0) { let crc = ~seed >>> 0; for (let i = 0; i < buf.length; i++) crc = (CRC_TABLE[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8)) >>> 0; return ~crc >>> 0; }
function cobsEncode(input) {
  const out = new Uint8Array(input.length + Math.ceil(input.length / 254) + 1);
  let o = 0, codeAt = o++, code = 1;
  for (let i = 0; i < input.length; i++) {
    if (input[i] === 0) { out[codeAt] = code; codeAt = o++; code = 1; }
    else { out[o++] = input[i]; if (++code === 0xff) { out[codeAt] = code; codeAt = o++; code = 1; } }
  }
  out[codeAt] = code; return out.subarray(0, o);
}
function cobsDecode(input) {
  if (input.length === 0) return null;
  const out = new Uint8Array(input.length); let o = 0, i = 0;
  while (i < input.length) {
    const code = input[i++]; if (code === 0) return null;
    const run = code - 1; if (i + run > input.length) return null;
    for (let k = 0; k < run; k++) { const b = input[i++]; if (b === 0) return null; out[o++] = b; }
    if (code !== 0xff && i < input.length) out[o++] = 0;
  }
  return out.subarray(0, o);
}
function buildFrame(type, seq, body) {
  const dec = new Uint8Array(6 + body.length + 4); const dv = new DataView(dec.buffer);
  dec[0] = PROTO; dec[1] = type; dv.setUint16(2, seq, true); dv.setUint16(4, body.length, true); dec.set(body, 6);
  dv.setUint32(6 + body.length, crc32(dec.subarray(0, 6 + body.length)), true);
  const enc = cobsEncode(dec); const out = new Uint8Array(enc.length + 1); out.set(enc); out[enc.length] = 0; return out;
}
/* Byte-at-a-time frame scanner: collects up to each 0x00 delimiter, COBS-
 * decodes, validates header + CRC and hands the frame to onFrame. */
class FrameParser {
  constructor() { this.acc = []; }
  push(byte, onFrame) {
    if (byte !== 0) { this.acc.push(byte); return; }
    const chunk = Uint8Array.from(this.acc); this.acc = [];
    if (chunk.length === 0) return;
    const dec = cobsDecode(chunk); if (!dec || dec.length < 10) return;
    const dv = new DataView(dec.buffer, dec.byteOffset, dec.byteLength);
    const bodyLen = dec.length - 10;
    const ok = dec[0] === PROTO && dv.getUint16(4, true) === bodyLen && crc32(dec.subarray(0, dec.length - 4)) === dv.getUint32(dec.length - 4, true);
    onFrame({ type: dec[1], seq: dv.getUint16(2, true), body: dec.subarray(6, 6 + bodyLen), ok });
  }
}

const u16 = (b, at) => b[at] | (b[at + 1] << 8);
const u32 = (b, at) => (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
const f32 = (b, at) => new DataView(b.buffer, b.byteOffset + at, 4).getFloat32(0, true);
function readStr(b, at) { const n = b[at]; return [new TextDecoder('ascii').decode(b.subarray(at + 1, at + 1 + n)), at + 1 + n]; }
const statusName = s => STATUS[s] || ('device error ' + s);

/* ───────────── Link ─────────────
 * HostLink is stop-and-wait: one request in flight, answers matched by seq.
 * Two queues feed the wire — `urgent` (anything the user did) drains before
 * `normal` (polls) — so a click never waits behind queued telemetry. A
 * request may carry a `key`: while a request with that key is queued or in
 * flight, the next one with the same key joins it instead of queuing again,
 * which keeps a slow link from piling up polls. Round-trips are timed per request. */
class Link {
  constructor(port) {
    this.port = port; this.seq = 1; this.parser = new FrameParser(); this.pending = null;
    this.urgent = []; this.normal = []; this.keyed = new Map(); this.closed = false;
    this.stats = { rtt: 0, rttAvg: 0, rttMax: 0, reqs: 0, rate: 0, errors: 0, timeouts: 0 };
    this._win = { t0: 0, n: 0, max: 0 };
    this.onError = null;
  }
  async start() { this.writer = this.port.writable.getWriter(); this.readLoop(); }
  /* Web Serial pattern: a non-fatal read error (overrun, framing) closes the
   * reader but leaves `port.readable` — reacquire and carry on; `done` or a
   * fatal error ends the loop. */
  async readLoop() {
    let ended = false;
    while (this.port.readable && !this.closed && !ended) {
      const reader = this.port.readable.getReader(); this.reader = reader;
      try { for (;;) { const { value, done } = await reader.read(); if (done) { ended = true; break; } for (let i = 0; i < value.length; i++) this.parser.push(value[i], f => this.onFrame(f)); } }
      catch (e) { if (!this.closed) { this.stats.errors++; if (this.onError) this.onError(e); } }
      finally { try { reader.releaseLock(); } catch {} }
    }
  }
  _measure(t0) {
    const now = _now(), rtt = now - t0, s = this.stats, w = this._win;
    s.rtt = rtt; s.rttAvg = s.rttAvg ? s.rttAvg * 0.8 + rtt * 0.2 : rtt; s.reqs++;
    if (rtt > w.max) w.max = rtt; w.n++;
    if (now - w.t0 >= 1000) { s.rate = w.n * 1000 / Math.max(1, now - w.t0); s.rttMax = w.max; w.t0 = now; w.n = 0; w.max = 0; }
  }
  onFrame(f) {
    const p = this.pending; if (!p || f.seq !== p.seq) return;
    clearTimeout(p.timer); this.pending = null; this._measure(p.t0);
    if (!f.ok) p.reject(new Error('bad frame'));
    else if (f.type === 0xff) p.reject(new Error('ERR frame (' + statusName(f.body[0]) + ')'));
    else if (f.type !== (p.type | 0x80)) p.reject(new Error('unexpected response type ' + f.type));
    else if (f.body.length && f.body[0] !== 0) p.reject(new Error(statusName(f.body[0])));
    else p.resolve(f.body);
    this.pump();
  }
  /** opts: { urgent: bool, key: string, timeoutMs: number } */
  request(type, body = new Uint8Array(0), opts = {}) {
    if (typeof opts === 'number') opts = { timeoutMs: opts };
    if (opts.key && this.keyed.has(opts.key)) return this.keyed.get(opts.key).promise;
    const q = { type, body, key: opts.key, timeoutMs: opts.timeoutMs || 1500 };
    q.promise = new Promise((resolve, reject) => { q.resolve = resolve; q.reject = reject; });
    if (this.closed) { q.reject(new Error('link closed')); return q.promise; }
    (opts.urgent ? this.urgent : this.normal).push(q);
    if (q.key) this.keyed.set(q.key, q);
    this.pump();
    return q.promise;
  }
  get queued() { return this.urgent.length + this.normal.length + (this.pending ? 1 : 0); }
  pump() {
    if (this.pending || this.closed) return;
    const q = this.urgent.length ? this.urgent.shift() : this.normal.shift();
    if (!q) return;
    const seq = this.seq = (this.seq % 0xfffe) + 1;
    const done = () => { if (q.key && this.keyed.get(q.key) === q) this.keyed.delete(q.key); };
    const p = this.pending = { seq, type: q.type, resolve: v => { done(); q.resolve(v); }, reject: e => { done(); q.reject(e); }, t0: _now(), timer: null };
    p.timer = setTimeout(() => { if (this.pending === p) { this.pending = null; this.stats.timeouts++; p.reject(new Error('timeout')); this.pump(); } }, q.timeoutMs);
    this.writer.write(buildFrame(q.type, seq, q.body)).catch(e => { if (this.pending === p) { clearTimeout(p.timer); this.pending = null; p.reject(e); this.pump(); } });
  }
  async close() {
    this.closed = true;
    for (const q of [...this.urgent, ...this.normal]) q.reject(new Error('link closed'));
    this.urgent.length = this.normal.length = 0; this.keyed.clear();
    if (this.pending) { clearTimeout(this.pending.timer); this.pending.reject(new Error('link closed')); this.pending = null; }
    try { await this.reader?.cancel(); } catch {} try { this.writer?.releaseLock(); } catch {} try { await this.port.close(); } catch {}
  }
}
const _now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

/* ───────────── standard commands ───────────── */
async function hello(link) {
  const b = await link.request(CMD.hello, new Uint8Array(0), { urgent: true }); let at = 5;
  const uid = Array.from(b.subarray(at, at + 12)).map(x => x.toString(16).padStart(2, '0')).join(''); at += 12;
  const schema = u32(b, at); at += 4; const cap = u32(b, at); at += 4; const dlen = u32(b, at); at += 4; const dcrc = u32(b, at); at += 4;
  const maxBody = u16(b, at); at += 2; const liveSize = u16(b, at); at += 2;
  let id, name, fw, git, sdk; [id, at] = readStr(b, at); [name, at] = readStr(b, at); [fw, at] = readStr(b, at); [git, at] = readStr(b, at); [sdk, at] = readStr(b, at);
  return { proto: b[1], board: b[2], slots: b[3], uid, schema, cap, descriptorLen: dlen, descriptorCrc: dcrc, maxBody, liveSize, id, name, fw, git, sdk };
}
async function getDescriptor(link, info) {
  if (!info.descriptorLen) return null;
  const chunkMax = Math.max(16, info.maxBody - 7); const parts = []; let off = 0;
  while (off < info.descriptorLen) {
    const req = new Uint8Array(6); const dv = new DataView(req.buffer); dv.setUint32(0, off, true); dv.setUint16(4, chunkMax, true);
    const b = await link.request(CMD.getDescriptor, req, { urgent: true }); const n = u16(b, 5); if (!n) break;
    parts.push(b.slice(7, 7 + n)); off += n;
  }
  const all = concat(parts);
  if (crc32(all) !== info.descriptorCrc) throw new Error('descriptor CRC mismatch');
  return JSON.parse(new TextDecoder('utf-8').decode(all));
}
/* GET_LIVE: the running managed set as one byte stream (offset 0 snapshots). */
async function getLive(link, info) {
  const chunkMax = Math.max(16, info.maxBody - 14); let total = -1, got = 0, schema = 0; const parts = [];
  do {
    const req = new Uint8Array(6); const dv = new DataView(req.buffer); dv.setUint32(0, got, true); dv.setUint16(4, chunkMax, true);
    const b = await link.request(CMD.getLive, req, { urgent: true });
    const n = u16(b, 6); if (total < 0) { total = u16(b, 8); schema = u32(b, 10); } if (!n) break;
    parts.push(b.slice(14, 14 + n)); got += n;
  } while (got < total);
  return { bytes: concat(parts), schema };
}
/* Live audition of a whole blob: BLOB_BEGIN(0xFF) → BLOB_DATA… → BLOB_COMMIT.
 * All urgent — this is a user edit. */
async function setLive(link, info, bytes) {
  const begin = new Uint8Array(9); const dv = new DataView(begin.buffer); begin[0] = 0xff; dv.setUint32(1, bytes.length, true); dv.setUint32(5, info.schema >>> 0, true);
  await link.request(CMD.blobBegin, begin, { urgent: true });
  const chunkMax = Math.max(16, info.maxBody - 4);
  for (let off = 0; off < bytes.length; off += chunkMax) {
    const n = Math.min(chunkMax, bytes.length - off); const d = new Uint8Array(4 + n); new DataView(d.buffer).setUint32(0, off, true); d.set(bytes.subarray(off, off + n), 4);
    await link.request(CMD.blobData, d, { urgent: true });
  }
  const c = new Uint8Array(4); new DataView(c.buffer).setUint32(0, crc32(bytes), true);
  await link.request(CMD.blobCommit, c, { urgent: true, timeoutMs: 12000 });
}
function concat(parts) { const all = new Uint8Array(parts.reduce((s, p) => s + p.length, 0)); let o = 0; for (const p of parts) { all.set(p, o); o += p.length; } return all; }

/* ───────────── StateExt (0x60–0x63) ─────────────
 * GET_STATE body (after status): fixed 148 bytes, then append-only tails
 * parsed by length — diag (v0.7.3, → 156), locks/native/bars (v0.8.0, → 164),
 * clock jitter + REQUESTED multiplier zone (CLOCK package, → 167; `mult`
 * above is the applied multiplier), clock flags + RMS jitter (→ 170):
 * clkFlags b0 P2 engaged · b1 multiplier switch pending · b2 lock alignment
 * just happened (~2 s) · b4–7 external-clock transitions mod 16. */
const STATE_MIN = 148;
const ZONE_MULT = [0.25, 0.5, 1, 2, 4];   /* P2 zones 0..4 while following: ÷4 ÷2 ×1 ×2 ×4 */
function parseState(b) {
  if (!b || b.length < STATE_MIN) return null;
  const st = {
    nSlices: b[1], cur: b[2], flags: b[3], matGen: b[4], grammar: b[5], cls: b[6], file: b[7], chop: b[8],
    histN: b[9], histDepth: b[10], callFlash: b[11], bpm: f32(b, 12), mult: f32(b, 16),
    src: Int8Array.from(b.subarray(20, 84)), stepClass: Uint8Array.from(b.subarray(84, 148)),
    diag: null, lockMask: null, nativeBpm: null, gridBars: null, barsOpt: null, jitterUs: null, multZone: null,
    clkFlags: null, jitterRmsUs: null, p2Engaged: null, multPending: null, aligned: null, extTrans: null,
  };
  st.muted = !!(st.flags & 1); st.ext = !!(st.flags & 2); st.locked = !!(st.flags & 4); st.flowing = !!(st.flags & 8); st.brain = !!(st.flags & 16);
  if (b.length >= 156) st.diag = { gap: u16(b, 148), audio: u16(b, 150), jumps: u16(b, 152), regens: u16(b, 154) };
  if (b.length >= 164) { st.lockMask = u16(b, 156); st.nativeBpm = f32(b, 158); st.gridBars = b[162]; st.barsOpt = b[163]; }
  if (b.length >= 167) { st.jitterUs = u16(b, 164); st.multZone = b[166]; }
  if (b.length >= 170) {
    st.clkFlags = b[167]; st.jitterRmsUs = u16(b, 168);
    st.p2Engaged = !!(st.clkFlags & 1); st.multPending = !!(st.clkFlags & 2); st.aligned = !!(st.clkFlags & 4); st.extTrans = st.clkFlags >> 4;
  }
  if (b.length >= 175) {   // v0.9.5 forensics
    st.stallPhase = b[170]; st.gateLateUs = u16(b, 171); st.overruns = u16(b, 173);
  }
  return st;
}
/* GET_ENV: fetch nSlices × 16 envelope bytes in ≤30-slice chunks. */
async function fetchEnv(link, n) {
  const out = new Uint8Array(64 * 16);
  for (let first = 0; first < n; first += 30) {
    const cnt = Math.min(30, n - first);
    const b = await link.request(CMD.env, Uint8Array.of(first, cnt), { key: 'env' + first });
    out.set(b.subarray(3, 3 + b[2] * 16), first * 16);
  }
  return out;
}
const parseScope = b => Int8Array.from(b.subarray(3, 3 + u16(b, 1)));

/* ───────────── descriptor helpers ───────────── */
/* Every field-bearing component's fields, flattened with their component. */
function allFields(desc) {
  const out = [];
  for (const c of (desc && desc.components) || []) for (const f of c.fields || []) out.push({ comp: c, f });
  return out;
}
function findField(desc, id) { return allFields(desc).find(x => x.f.id === id) || null; }
function fieldLabels(desc, id) { const x = findField(desc, id); return (x && x.f.disp && Array.isArray(x.f.disp.labels)) ? x.f.disp.labels : []; }
/* The pager component: page names/help and per-(page,pot) knob name/help. */
function pagerInfo(desc) {
  const c = ((desc && desc.components) || []).find(c => c.kind === 'pager'); if (!c) return null;
  const knobs = {};
  for (const f of c.fields || []) if (f.page != null && f.pot != null) knobs[f.page + ':' + f.pot] = f;
  return { comp: c, pages: c.pages, pots: c.pots, names: c.pageNames || [], help: c.pageHelp || [], colors: c.pageColors || [], knobs };
}
/* The Options block: the component carrying the enum ids OnPoll reads. */
function optionsInfo(desc) {
  const x = findField(desc, 'grammar.mode') || findField(desc, 'class.mode'); if (!x) return null;
  const fields = (x.comp.fields || []).filter(f => f.type === 'enum');
  return { comp: x.comp, fields };
}
/* What the descriptor says about the manual: null when absent. */
function manualInfo(desc) {
  if (!desc) return null;
  const m = desc.manual, mod = desc.module || {};
  if (!m && !mod.tagline && !desc.jacks) return null;
  return { tagline: mod.tagline || '', preamble: (m && m.preamble) || '', sections: (m && m.sections) || [],
           presetsHelp: (m && m.presets && m.presets.help) || '', jacks: desc.jacks || [] };
}

const api = { PROTO, CMD, ACT, STATUS, statusName, crc32, cobsEncode, cobsDecode, buildFrame, FrameParser, Link,
  u16, u32, f32, readStr, hello, getDescriptor, getLive, setLive, concat,
  STATE_MIN, ZONE_MULT, parseState, fetchEnv, parseScope, allFields, findField, fieldLabels, pagerInfo, optionsInfo, manualInfo };
if (typeof module === 'object' && module.exports) module.exports = api; else root.HL = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
