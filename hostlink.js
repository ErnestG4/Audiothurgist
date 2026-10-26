/* hostlink.js — Audiothurgist's wire layer.
 *
 * HostLink v1 framing (CRC32 / COBS / frame header, a faithful port of
 * alchemy-sdk's tools/hostlink-cli/hostlink.mjs), a single-outstanding-
 * request Link with a priority queue and round-trip measurement, the
 * standard commands the page needs (HELLO, GET_DESCRIPTOR, GET_LIVE, live
 * BLOB writes) and the firmware's StateExt parsers (0x60–0x66: state, envelope,
 * scope — the old mono reply and the four-channel SCOPE body —, actions, the
 * sample-mode wave / table / hits reads, the set save / load, and CARD's 0x67:
 * a break uploaded from a file the browser decoded, an old card copied in).
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
  state: 0x60, env: 0x61, scope: 0x62, action: 0x63, wave: 0x64, table: 0x65, hits: 0x66, card: 0x67, pads: 0x68,
  keys: 0x69,   /* KEYP: GET_KEYS — every key's gain / tune / plays */
};
const ACT = { dice: 0, reslice: 1, mute: 2, punch: 3, flow: 4, scrub: 5, load: 6, clearLocks: 7, page: 8,
  recArm: 9, recClear: 10, keyPlays: 11, looperUndo: 12, looperClear: 13, saveSet: 14, loadSet: 15,
  /* EDIT (capability "edit": 1): the slice-editing ops, the audition voice, the tap mark */
  setBoundary: 16, setEntry: 17, addEntry: 18, split: 19, merge: 20, tombstone: 21, setKey: 22, swapKeys: 23,
  nudgeAll: 24, promote: 25, fromOnsets: 26, audition: 27, release: 28, tapMark: 29,
  /* EDIT-UNDO: the journal's two ops (no fields) — the page enables Undo / Redo from the tail's undoDepth / redoDepth */
  undoEdit: 30, redoEdit: 31,
  /* KEYS (v0.13, capability "keys": 1): the on-screen pads, the two marker ops, the transport and the autoslice count */
  playKey: 32, keysFromMarkers: 33, clearMarkers: 34, setMute: 35, autoslice: 36,
  /* KEYP (capability "keyp": 1): one key's gain / tune / plays */
  keyParams: 37 };
/* v0.13 "keys": the J5 style the Voice map reads (state byte 271; 255 = the Brain map, where J5 is the gate) and the
 * autoslice count's range (ACTION 36's k). TAP_MARK's fourth reply byte: b0 = an existing marker was reused. */
const KEYS = { n: 24, autoMax: 64, reroll: 1, join: 2, reused: 1,
  styles: ['plain', 'reverse', 'half speed', 'double', 'ratchet', 'staccato', 'punch / choke flip', 'record', 'clear'] };
/* EDIT: flag bits and constants. Table flags (GET_TABLE's 14-byte form, ADD_ENTRY's flags): orange = user-placed
 * or user-moved, tomb = deleted (len 0, skipped by everything). AUDITION flags: fromEnd = `start` is the region's
 * END (the last lenMs up to it), loop = repeat until RELEASE. NUDGE_ALL's `which`: grid or markers. */
const EDIT = { orange: 1, tomb: 2, fromEnd: 1, loop: 2, grid: 0, markers: 1, noKey: 255, maxNudgeMs: 32767, maxAuditionMs: 65535,
  tapAllowMs: [0, 20, 40, 60, 80], tapAllowDefault: 40 };   /* Options → Tap Allowance (tap.allow): index → ms */
/* SETS (state tail 219..254): a set is sets/<name>.wav + sets/<name>.json on the card. The op's
 * state, its progress, the result of the last op and a sequence that bumps when one finishes;
 * then the name of the set the material came from. A name is 1..31 of [A-Za-z0-9_-]. */
const SET_STATE = ['idle', 'saving', 'loading', 'card'];   /* 3: CARD — an upload being written or a copy (every set op waits) */
const SET_RESULT = ['ok', 'no card', 'card full', 'write failed', 'bad name', 'not found', 'busy', 'bad file'];
const SET_NAME_RE = /^[A-Za-z0-9_-]{1,31}$/;
const setNameOk = n => typeof n === 'string' && SET_NAME_RE.test(n);
/* "amen [set]" → "amen"; null for a loose file's label. */
const setLabelName = l => (typeof l === 'string' && /^[A-Za-z0-9_-]{1,31} \[set\]$/.test(l)) ? l.slice(0, -6) : null;
const nameBytes = n => Array.from(n, c => c.charCodeAt(0) & 0x7f);
/* Record state (state tail byte 177) and the 24 Voice-map keys, two octaves from C. */
const REC = ['idle', 'armed', 'taking', 'pending'];
const NOTE_NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
const keyName = k => NOTE_NAMES[k % 12] + (1 + Math.floor(k / 12));
const STATUS = ['OK', 'UNSUPPORTED', 'BAD_ARGS', 'BAD_STATE', 'BAD_CRC', 'BAD_SLOT', 'TOO_LARGE',
  'SCHEMA_MISMATCH', 'FLASH_FAIL', 'BUSY', 'FRAME_ERROR'];
STATUS[20] = 'EXISTS'; STATUS[21] = 'NO_CARD'; STATUS[22] = 'CARD_FULL';   /* CARD (0x67): the card's own answers (Kyklophoria's 20) */

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
/* The cap is not optional (Kyklophoria, 2026-09): a frame is at most maxBody
 * plus header, CRC and COBS overhead, so anything longer is not a frame — a
 * device that died mid-transmit, a wrong transport, line noise — and the answer
 * to all three is to drop the chunk at the next delimiter and resync. Without
 * it 4 MB of undelimited input took a tab's heap to 96 MB. 2048 covers a
 * 1024-byte body with room; `dropped` counts resyncs for the link pill. */
const kMaxFrameBytes = 2048;
class FrameParser {
  constructor() { this.acc = []; this.over = false; this.dropped = 0; }
  push(byte, onFrame) {
    if (byte !== 0) {
      if (this.over) return;
      if (this.acc.length >= kMaxFrameBytes) { this.over = true; this.acc.length = 0; this.dropped++; return; }
      this.acc.push(byte); return;
    }
    if (this.over) { this.over = false; this.acc.length = 0; return; }
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

/* ───────────── transports ─────────────
 * Kyklophoria's split (2026-09): the Link speaks frames, a transport moves
 * bytes. A transport has start(onBytes) → Promise, write(Uint8Array) →
 * Promise, close() → Promise, a `kind` label, and calls its `onError(e)` for
 * a read error it survived and `onClose()` when the byte stream ENDED without
 * close() being asked for — the cable pulled, the module reset, the bridge
 * gone. That one callback is the page's unplug detection: before it, a pulled
 * cable left the page polling a dead port with every readout frozen as if live. */

/* Web Serial (and the WebUSB polyfill, and demo.js's FakePort, which has the same
 * surface): a port already open()ed. A non-fatal read error (overrun, framing)
 * closes the reader but leaves port.readable — reacquire and carry on; `done`
 * or a fatal error ends the loop, and an end nobody asked for is onClose. */
class SerialTransport {
  constructor(port) { this.port = port; this.kind = 'serial'; this.closed = false; this.onError = null; this.onClose = null; }
  async start(onBytes) { this.writer = this.port.writable.getWriter(); this.readLoop(onBytes); }
  async readLoop(onBytes) {
    let ended = false;
    while (this.port.readable && !this.closed && !ended) {
      const reader = this.port.readable.getReader(); this.reader = reader;
      try { for (;;) { const { value, done } = await reader.read(); if (done) { ended = true; break; } onBytes(value); } }
      catch (e) { if (!this.closed && this.onError) this.onError(e); }
      finally { try { reader.releaseLock(); } catch {} }
    }
    if (!this.closed && this.onClose) this.onClose();
  }
  write(bytes) { return this.writer.write(bytes); }
  async close() {
    this.closed = true;
    try { await this.reader?.cancel(); } catch {} try { this.writer?.releaseLock(); } catch {} try { await this.port.close(); } catch {}
  }
}
/* A WebSocket carrying raw wire bytes (a bridge on this machine, as Kyklophoria's
 * tools/bridge). Nothing in Audiothurgist serves one yet; the class costs nothing. */
class WsTransport {
  constructor(ws) { this.ws = ws; this.kind = 'bridge'; this.closed = false; this.onError = null; this.onClose = null; }
  start(onBytes) {
    this.ws.binaryType = 'arraybuffer';
    this.ws.onmessage = ev => { if (ev.data instanceof ArrayBuffer) onBytes(new Uint8Array(ev.data)); };
    this.ws.onerror = e => { if (this.onError) this.onError(e); };
    this.ws.onclose = () => { if (!this.closed && this.onClose) this.onClose(); this.closed = true; };
    return Promise.resolve();
  }
  write(bytes) { try { this.ws.send(bytes); return Promise.resolve(); } catch (e) { return Promise.reject(e); } }
  close() { this.closed = true; try { this.ws.close(); } catch {} return Promise.resolve(); }
}
/* What the Link was given: a transport, or a SerialPort-shaped object (open, with readable / writable) to wrap. */
const asTransport = x => x && typeof x.start === 'function' && typeof x.write === 'function' ? x : new SerialTransport(x);

/* ───────────── Link ─────────────
 * HostLink is stop-and-wait: one request in flight, answers matched by seq.
 * Two queues feed the wire — `urgent` (anything the user did) drains before
 * `normal` (polls) — so a click never waits behind queued telemetry. A
 * request may carry a `key`: while a request with that key is queued or in
 * flight, the next one with the same key joins it instead of queuing again,
 * which keeps a slow link from piling up polls. Round-trips are timed per
 * request; bytesIn / kbps count what arrives. */
class Link {
  constructor(transport) {
    this.t = asTransport(transport); this.seq = 1; this.parser = new FrameParser(); this.pending = null;
    this.urgent = []; this.normal = []; this.keyed = new Map(); this.closed = false;
    this.stats = { rtt: 0, rttAvg: 0, rttMax: 0, reqs: 0, rate: 0, bytesIn: 0, kbps: 0, errors: 0, timeouts: 0 };
    this._win = { t0: _now(), n: 0, max: 0, bytes: 0 };
    this.onError = null; this.onClose = null;
    this.t.onError = e => { this.stats.errors++; if (this.onError) this.onError(e); };
    this.t.onClose = () => { if (!this.closed && this.onClose) this.onClose(); };
  }
  get kind() { return this.t.kind; }
  get port() { return this.t.port; }
  /* A link that did not close tidily leaves the module's parser holding half
   * a frame; our HELLO gets appended to it, delimited into one malformed frame
   * and dropped — correctly — so nothing answers and the FIRST connect times
   * out while the second works (its delimiter is what cleared the module).
   * A lone zero is the documented "throw away what you have": one byte, and
   * the parser has always known what to do with it (Kyklophoria's fix). */
  async start() {
    await this.t.start(bytes => {
      this.stats.bytesIn += bytes.length; this._win.bytes += bytes.length;
      for (let i = 0; i < bytes.length; i++) this.parser.push(bytes[i], f => this.onFrame(f));
    });
    try { await this.t.write(Uint8Array.of(0)); } catch { /* the first request will report it */ }
  }
  /* raw bytes on the wire (hello's lone-zero resync between attempts) */
  writeRaw(bytes) { return this.t.write(bytes); }
  _measure(t0) {
    const now = _now(), rtt = now - t0, s = this.stats, w = this._win;
    s.rtt = rtt; s.rttAvg = s.rttAvg ? s.rttAvg * 0.8 + rtt * 0.2 : rtt; s.reqs++;
    if (rtt > w.max) w.max = rtt; w.n++;
    if (now - w.t0 >= 1000) { const dt = Math.max(1, now - w.t0); s.rate = w.n * 1000 / dt; s.kbps = w.bytes / dt; s.rttMax = w.max; w.t0 = now; w.n = 0; w.max = 0; w.bytes = 0; }
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
    this.t.write(buildFrame(q.type, seq, q.body)).catch(e => { if (this.pending === p) { clearTimeout(p.timer); this.pending = null; p.reject(e); this.pump(); } });
  }
  async close() {
    this.closed = true;
    for (const q of [...this.urgent, ...this.normal]) q.reject(new Error('link closed'));
    this.urgent.length = this.normal.length = 0; this.keyed.clear();
    if (this.pending) { clearTimeout(this.pending.timer); this.pending.reject(new Error('link closed')); this.pending = null; }
    await this.t.close();
  }
}
const _now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

/* ───────────── standard commands ───────────── */
/* HELLO is asked up to three times, a lone zero between attempts: a reset, a
 * half-sent reply or a buffer the driver held across the close all look like
 * silence, and one 400 ms retry is cheaper than a second click. */
async function hello(link, tries = 3) {
  let last = null;
  for (let i = 0; i < tries; i++) {
    try { return await helloOnce(link, i === 0 ? 1500 : 400); }
    catch (e) {
      last = e; if (link.closed) break;
      await new Promise(r => setTimeout(r, 120));
      try { await link.writeRaw(Uint8Array.of(0)); } catch {}
    }
  }
  throw last || new Error('no answer');
}
async function helloOnce(link, timeoutMs) {
  const b = await link.request(CMD.hello, new Uint8Array(0), { urgent: true, timeoutMs }); let at = 5;
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
  if (b.length >= 176) {   // v0.9.9 jack map
    st.jackMap = b[175];
  }
  if (b.length >= 177) st.looperEvents = b[176];   // LOOPER: events on the bar strip
  if (b.length >= 194) {   // SAMPLE / WEB: record state, buffer bookkeeping
    st.recState = b[177]; st.recKey = b[178] === 255 ? -1 : b[178];
    st.freePos = u32(b, 179); st.bufCap = u32(b, 183); st.stereo = !!b[187]; st.inLevel = b[188];
    st.nTable = b[189]; st.bufLen = u32(b, 190);
  }
  if (b.length >= 218) st.keySlice = Int8Array.from(b.subarray(194, 218));   // table entry each key plays
  if (b.length >= 219) st.keyPlays = b[218];                                  // Options → Key Plays (0 one-shot, 1 held)
  if (b.length >= 223) { st.setState = b[219]; st.setProgress = b[220]; st.setResult = b[221]; st.setSeq = b[222]; }   // SETS
  if (b.length >= 255) { let n = 223; while (n < 255 && b[n]) n++; st.setName = new TextDecoder('ascii').decode(b.subarray(223, n)); }
  /* EDIT tail: 255 tableGen (never 0; carry it back as an op's gen), 256 undoDepth (EDIT-UNDO: journal steps back; 0
   * on a firmware without the journal), 257 selSlice (the panel's selection, 255 = none, until EDIT-PANEL), 258..261
   * srcPos (the youngest voice's material position). EDIT-UNDO tail: 262 redoDepth (steps forward) — its presence is
   * how the page knows the journal exists (a 262-byte body is EDIT-PROTO without package 6). */
  if (b.length >= 258) { st.tableGen = b[255]; st.undoDepth = b[256]; st.selSlice = b[257] === 255 ? -1 : b[257]; }
  if (b.length >= 262) st.srcPos = u32(b, 258);
  if (b.length >= 263) st.redoDepth = b[262];
  /* KEYS tail (v0.13, → 276): 263 flags2 (b0 srcValid: a voice sounds, so TAP_MARK will be taken · b1 keys follow
   * markers: Pads with markers), 264 loadAvg / 266 loadMax (the audio callback, ‰ of the block budget, mean and peak
   * over the last second), 268 j4mV (i16, J4 now), 270 j4Key (the key J4 would play), 271 j5Style (0..8; 255 in the
   * Brain map → -1), 272 keyFlash (the last key called by any source, 255 none → -1), 273 keySeq (bumps per call),
   * 274 autoK (the autoslice count last applied, 0 none), 275 nMarkers. An older body leaves them all undefined. */
  if (b.length >= 276) {
    st.flags2 = b[263]; st.srcValid = !!(b[263] & 1); st.keysFollow = !!(b[263] & 2);
    st.loadAvg = u16(b, 264); st.loadMax = u16(b, 266);
    const mv = u16(b, 268); st.j4mV = mv > 0x7fff ? mv - 0x10000 : mv; st.j4Key = b[270]; st.j5Style = b[271] === 255 ? -1 : b[271];
    st.keyFlash = b[272] === 255 ? -1 : b[272]; st.keySeq = b[273]; st.autoK = b[274]; st.nMarkers = b[275];
  }
  /* CARD tail (→ 293, capability "card": 1): 276 state (CARD.STATE), 277 progress 0..255, 278 result of the last op
   * (CARD.RESULT), 279 seq (bumps per finished op), 280 flags (CARD.F), 281 rootWavs, 283 pending (old files not in the
   * folder yet), 285 breaks (WAVs in Audiothurgist/breaks), 287 lastIdx (the last upload's list index, -1 none), 288 copied
   * · 290 skipped (the last copy), 292 nFiles. An older body leaves st.card null. */
  st.card = null;
  if (b.length >= 293) {
    const f = b[280];
    st.card = { state: b[276], progress: b[277], result: b[278], seq: b[279], flags: f, mounted: !!(f & CARD.F.mounted),
      oldRoot: !!(f & CARD.F.rootWavs), oldSets: !!(f & CARD.F.oldSets), oldPresets: !!(f & CARD.F.oldPresets),
      hasBreaks: !!(f & CARD.F.breaks), rootWavs: u16(b, 281), pending: u16(b, 283), breaks: u16(b, 285),
      lastIdx: b[287] === 255 ? -1 : b[287], copied: u16(b, 288), skipped: u16(b, 290), nFiles: b[292] };
  }
  /* PADS tail (→ 315, capability "pads": 1): 293 state (PADS.STATE), 294 progress 0..255, 295 result of the last op
   * (PADS.RESULT), 296 seq (bumps per finished op), 297 key (the drop's, -1 none), 298 flags (PADS.F: the kit holds audio
   * the card does not · an empty kit, no break · the last drop made the kit stereo), 299 free (frames past freePos),
   * 303 total (the pads' share: bufCap − bufLen), 307 held (frames below freePos that replaced or undone recordings
   * still hold — not reclaimed until new material), 311 the pads (bit k = key k holds a recording). Older: null. */
  st.pads = null;
  if (b.length >= 315) {
    const f = b[298];
    st.pads = { state: b[293], progress: b[294], result: b[295], seq: b[296], key: b[297] === 255 ? -1 : b[297], flags: f,
      unsaved: !!(f & PADS.F.unsaved), noBreak: !!(f & PADS.F.noBreak), madeStereo: !!(f & PADS.F.madeStereo),
      free: u32(b, 299), total: u32(b, 303), held: u32(b, 307), mask: u32(b, 311) };
  }
  return st;
}
/* GET_WAVE: one window of the material buffer — n ≤ 480 output bytes, each
 * the largest-magnitude sample of `step` (1..128) source samples, so a
 * window spans n·step samples. plane 0 L · 1 R · 2 mono sum. Resolves
 * { start, n, step, data: Int8Array }; BAD_ARGS past freePos. */
async function fetchWave(link, start, n = 480, plane = 0, step = 1, opts = {}) {
  const req = new Uint8Array(8); const dv = new DataView(req.buffer);
  dv.setUint32(0, start >>> 0, true); dv.setUint16(4, Math.min(480, n), true); req[6] = plane; req[7] = Math.max(1, Math.min(128, step));
  const b = await link.request(CMD.wave, req, Object.assign({ key: 'wave' }, opts));
  return parseWave(b);
}
function parseWave(b) { const n = u16(b, 5); return { start: u32(b, 1), n, step: b[7] || 1, data: Int8Array.from(b.subarray(8, 8 + n)) }; }
/* GET_TABLE: every table entry (grid [0, nSlices) then markers [nSlices, nTable): recordings and,
 * with "edit", spans of the break), 13 bytes each, ≤ 39 per reply. Resolves { nSlices, nTable,
 * entries[] } with entries of { start, len, key (-1 grid / keyless), cls, sal, hit, dens, flags }.
 * EDIT: a firmware with "edit": 1 answers a third request byte (form 1) with a trailing u8 flags
 * per entry (EDIT.orange · EDIT.tomb) — 14 bytes; pass { edit: true } to fetchTable (the page does
 * when the capability is present) or the entry size to parseTableChunk. Without it the 13-byte form
 * comes back with flags 0, whatever the firmware — an old page keeps working against a new module. */
const TABLE_ENTRY = 13, TABLE_ENTRY_EDIT = 14, TABLE_CHUNK = 39;
function parseTableChunk(b, entrySize = TABLE_ENTRY) {
  const n = Math.floor((b.length - 3) / entrySize), entries = [];
  for (let i = 0; i < n; i++) { const at = 3 + i * entrySize; const k = b[at + 8];
    entries.push({ start: u32(b, at), len: u32(b, at + 4), key: k > 127 ? k - 256 : k, cls: b[at + 9], sal: b[at + 10], hit: !!b[at + 11], dens: b[at + 12],
      flags: entrySize >= TABLE_ENTRY_EDIT ? b[at + 13] : 0 }); }
  return { nSlices: b[1], nTable: b[2], entries };
}
async function fetchTable(link, opts = {}) {
  let first = 0, out = null;
  const edit = !!opts.edit, size = edit ? TABLE_ENTRY_EDIT : TABLE_ENTRY;
  do {
    const req = edit ? Uint8Array.of(first, TABLE_CHUNK, 1) : Uint8Array.of(first, TABLE_CHUNK);
    const c = parseTableChunk(await link.request(CMD.table, req, Object.assign({ key: 'table' + first }, opts)), size);
    if (!out) out = { nSlices: c.nSlices, nTable: c.nTable, entries: [] };
    out.entries.push(...c.entries); first += c.entries.length;
    if (!c.entries.length) break;
  } while (first < out.nTable);
  return out;
}
/* GET_HITS: the shell's onset table for the current material (u32 sample
 * positions, ≤ 127 per reply). Resolves a Uint32Array. */
function parseHitsChunk(b) { const n = Math.floor((b.length - 3) / 4), p = new Uint32Array(n); for (let i = 0; i < n; i++) p[i] = u32(b, 3 + i * 4); return { total: b[1], first: b[2], positions: p }; }
async function fetchHits(link, opts = {}) {
  let first = 0, total = 0; const parts = [];
  do {
    const c = parseHitsChunk(await link.request(CMD.hits, Uint8Array.of(first), Object.assign({ key: 'hits' + first }, opts)));
    total = c.total; parts.push(c.positions); first += c.positions.length;
    if (!c.positions.length) break;
  } while (first < total);
  const out = new Uint32Array(parts.reduce((s, p) => s + p.length, 0)); let o = 0; for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
/* SETS: ACTION 14 / 15 carry the name as the rest of the body. The device answers at once
 * (BAD_ARGS for a bad name, BAD_STATE while an op runs or a load waits); the work happens
 * in its main loop and the state tail (setState / setProgress / setResult / setSeq) tells. */
function saveSet(link, name) { if (!setNameOk(name)) return Promise.reject(new Error('bad set name')); return link.request(CMD.action, Uint8Array.of(ACT.saveSet, ...nameBytes(name)), { urgent: true }); }
function loadSet(link, name) { if (!setNameOk(name)) return Promise.reject(new Error('bad set name')); return link.request(CMD.action, Uint8Array.of(ACT.loadSet, ...nameBytes(name)), { urgent: true }); }
/* ───────────── EDIT (ACTION 16..29, capability "edit": 1) ─────────────
 * Every op's body is its fields, little-endian, then an optional trailing u8 gen: the tableGen the
 * page last saw (state byte 255), 0 = unchecked. A stale gen is BAD_STATE — refetch the table and
 * retry; the design sends it for split / merge / delete, not for drags. The reply is u8 status,
 * then on success u8 tableGen (the new generation) and, for ADD / SPLIT / TAP_MARK, u8 newIdx (the
 * new entry's index) — FROM_ONSETS puts the number of markers added there. These helpers resolve
 * { status, ok, tableGen, newIdx, count } for every device answer (0 OK · 2 BAD_ARGS · 3 BAD_STATE:
 * a stale gen, a take in flight, a set op, a swap pending, the table full, no material) and reject
 * only on a link failure. All urgent: they are the user's hand. */
const u32b = v => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const u16b = v => [v & 0xff, (v >>> 8) & 0xff];
const i16b = v => u16b(v < 0 ? v + 0x10000 : v);
async function editOp(link, op, fields, gen = 0) {
  const body = Uint8Array.of(op, ...fields, ...(gen ? [gen & 0xff] : []));
  try {
    const b = await link.request(CMD.action, body, { urgent: true });
    const third = b.length > 2 ? b[2] : null;
    const flags = b.length > 3 ? b[3] : 0;   /* v0.13 TAP_MARK: b0 = an existing marker was reused (selected, nothing added) */
    return { status: 0, ok: true, tableGen: b.length > 1 ? b[1] : null, newIdx: third === 255 ? -1 : third, count: third, flags, reused: !!(flags & KEYS.reused) };
  } catch (e) {
    const s = STATUS.indexOf(e.message); if (s < 0) throw e;
    return { status: s, ok: false, tableGen: null, newIdx: null, count: null, flags: 0, reused: false };
  }
}
const editBoundary   = (link, i, pos, gen = 0)             => editOp(link, ACT.setBoundary, [i & 0xff, ...u32b(pos)], gen);
const editEntry      = (link, t, start, len, gen = 0)      => editOp(link, ACT.setEntry, [t & 0xff, ...u32b(start), ...u32b(len)], gen);
/* key null / -1 / 255 = none; flags EDIT.orange for a user-placed marker (0 = grey, an auto marker) */
const editAdd        = (link, key, start, len, flags = EDIT.orange, gen = 0) => editOp(link, ACT.addEntry, [key == null || key < 0 ? EDIT.noKey : key & 0xff, ...u32b(start), ...u32b(len), flags & 0xff], gen);
const editSplit      = (link, t, pos, gen = 0)             => editOp(link, ACT.split, [t & 0xff, ...u32b(pos)], gen);
const editMerge      = (link, t, gen = 0)                  => editOp(link, ACT.merge, [t & 0xff], gen);
const editTombstone  = (link, t, gen = 0)                  => editOp(link, ACT.tombstone, [t & 0xff], gen);
const editSetKey     = (link, key, t, gen = 0)             => editOp(link, ACT.setKey, [key & 0xff, t & 0xff], gen);
const editSwapKeys   = (link, a, b, gen = 0)               => editOp(link, ACT.swapKeys, [a & 0xff, b & 0xff], gen);
/* ms → samples happens in the firmware (48 per ms); which: EDIT.grid or EDIT.markers */
const editNudgeAll   = (link, ms, which, gen = 0)          => editOp(link, ACT.nudgeAll, [...i16b(Math.max(-EDIT.maxNudgeMs, Math.min(EDIT.maxNudgeMs, Math.round(ms)))), which & 0xff], gen);
const editPromote    = (link, t, i, gen = 0)               => editOp(link, ACT.promote, [t & 0xff, i & 0xff], gen);
/* thresh 0..255 against each onset's strength (its slice's salience); grey markers inside the break are replaced */
const editFromOnsets = (link, thresh, minGapMs, gen = 0)   => editOp(link, ACT.fromOnsets, [thresh & 0xff, ...u16b(Math.max(0, Math.min(65535, Math.round(minGapMs))))], gen);
/* the scratch voice: start (samples), lenMs, flags EDIT.fromEnd | EDIT.loop; ≤ 10/s (a later one replaces) */
const audition       = (link, start, lenMs, flags = 0)     => editOp(link, ACT.audition, [...u32b(start), ...u16b(Math.max(1, Math.min(EDIT.maxAuditionMs, Math.round(lenMs)))), flags & 0xff]);
const release        = link                                => editOp(link, ACT.release, []);
/* the firmware timestamps: srcPos at receipt − allowMs, snapped to an onset within ±snapMs; an orange marker */
const tapMark        = (link, snapMs, allowMs, gen = 0)    => editOp(link, ACT.tapMark, [snapMs & 0xff, allowMs & 0xff], gen);
/* EDIT-UNDO: one journal step either way → { ok, tableGen }; BAD_ARGS = nothing to undo / redo (or a firmware without the journal) */
const editUndo       = link                                => editOp(link, ACT.undoEdit, []);
const editRedo       = link                                => editOp(link, ACT.redoEdit, []);
/* ───────────── KEYS (ACTION 32..36, capability "keys": 1, v0.13) ─────────────
 * Same reply shape as the edit ops. PLAY_KEY and SET_MUTE answer a bare status; the marker ops and AUTOSLICE answer
 * status, tableGen, count (one undo step each). An older firmware answers BAD_ARGS (unknown op) — the page hides
 * them unless the descriptor says "keys". */
const playKey        = (link, key, down)                   => editOp(link, ACT.playKey, [key & 0xff, down ? 1 : 0]);
const keysFromMarkers = (link, gen = 0)                    => editOp(link, ACT.keysFromMarkers, [], gen);
const clearMarkers   = (link, gen = 0)                     => editOp(link, ACT.clearMarkers, [], gen);
/* stopped = true → the pattern stops (the clock runs on, keys still play) */
const setMute        = (link, stopped)                     => editOp(link, ACT.setMute, [stopped ? 1 : 0]);
/* k 0..64 markers from the seeded draw (0 removes the grey ones); reroll draws a new seed; join folds this apply into the
 * previous AUTOSLICE's undo step (every send of one slider drag after the first: a whole drag is one undo step) */
const autoslice      = (link, k, reroll = false, gen = 0, join = false) => editOp(link, ACT.autoslice, [Math.max(0, Math.min(KEYS.autoMax, Math.round(k))) & 0xff, (reroll ? KEYS.reroll : 0) | (join ? KEYS.join : 0)], gen);
/* What the descriptor's "audiothurgist" object says the firmware can do (each 0 when absent). */
function caps(desc) {
  const a = (desc && desc.audiothurgist) || {};
  return { ext: a.ext | 0, table: !!a.table, sets: a.sets | 0, scope2: a.scope2 | 0, edit: a.edit | 0, keys: a.keys | 0, card: a.card | 0, pads: a.pads | 0, keyp: a.keyp | 0 };
}

/* ───────────── KEYP (ACTION 37 + 0x69 GET_KEYS, capability "keyp": 1) ─────────────
 * Each of the 24 keys has a gain (½ dB steps, −24 … +6 dB), a tune (cents, ±24 semitones) and how it plays: 0 follow
 * Options → Key Plays · 1 one-shot · 2 held (until the gate falls / the pad comes up) · 3 loop (held, the entry looping).
 * All zero = neutral: the key plays exactly as it always did. They ride every call made through the key — the CV keys,
 * the pads, Play's loop replaying a key (with the settings as they are now) — never the pattern's steps. KEY_PARAMS is
 * an edit (the journal: one undo step; `join` = every send of one slider drag after the first, folded into this key's
 * step) and answers like one: { ok, status, tableGen }. GET_KEYS reads all 24; the tableGen in the state tail moves with
 * every change (an edit, an undo, a load), which is when the page reads them again. */
const KEYP = { gainMin: -48, gainMax: 12, centsMax: 2400, join: 1,
  PLAYS: ['follow', 'one-shot', 'held', 'loop'],
  PLAYS_LONG: ['Follow Key Plays', 'One-shot', 'Held (while down)', 'Loop (while held)'] };
const keypNeutral = p => !p || (!p.gain && !p.cents && !p.plays);
function keyParams(link, key, p, join = false, gen = 0) {
  const g = Math.max(KEYP.gainMin, Math.min(KEYP.gainMax, Math.round(p.gain || 0)));
  const c = Math.max(-KEYP.centsMax, Math.min(KEYP.centsMax, Math.round(p.cents || 0)));
  const pl = Math.max(0, Math.min(3, p.plays | 0));
  return editOp(link, ACT.keyParams, [key & 0xff, g & 0xff, ...i16b(c), pl, join ? KEYP.join : 0], gen);
}
/* → [{ gain (½ dB), plays, cents } × 24] */
function parseKeyParams(b) {
  const n = Math.min(b[1] || 0, Math.floor((b.length - 2) / 4)), out = [];
  for (let k = 0; k < n; k++) { const o = 2 + k * 4, g = b[o], c = u16(b, o + 2); out.push({ gain: g > 127 ? g - 256 : g, plays: b[o + 1], cents: c > 0x7fff ? c - 0x10000 : c }); }
  return out;
}
const fetchKeyParams = async (link, opts = {}) => parseKeyParams(await link.request(CMD.keys, new Uint8Array(0), opts));
/* the cell's words: "−6 dB", "+7 st", "−0.25 st" … — empty when neutral */
const keypGainText = g => g ? `${g > 0 ? '+' : '−'}${Math.abs(g / 2)} dB` : '';
const keypTuneText = c => { if (!c) return ''; const a = Math.abs(c); return `${c > 0 ? '+' : '−'}${a % 100 ? (a / 100).toFixed(2).replace(/0$/, '') : a / 100} st`; };
const keypText = p => keypNeutral(p) ? '' : [keypGainText(p.gain), keypTuneText(p.cents), p.plays ? KEYP.PLAYS[p.plays] : ''].filter(Boolean).join(' · ');

/* ───────────── CARD (0x67, capability "card": 1) ─────────────
 * The card is one folder, Audiothurgist/: breaks/*.wav (the list), sets/, presets/. The page turns any file the
 * browser can decode into what the module loads — at most 17 s, 48 kHz, mono or stereo, PCM16 WAV — and sends it in
 * order from byte 0, one chunk per round trip: UP_BEGIN(total, overwrite, name), then UP_DATA(total, offset, bytes)
 * until the last byte; the module answers each with the bytes it holds, and once all are in it writes
 * Audiothurgist/breaks/<name>.wav itself (the state tail's card state / progress / result / seq tell). COPY_OLD copies
 * an old card's root WAVs, sets/ and presets/ in (never moves, never deletes, skips what is there); RESCAN looks again;
 * LIST reads the module's list as it is now (the descriptor's dropdown is the list at boot). */
const CARD = { begin: 0, data: 1, abort: 2, copy: 3, rescan: 4, list: 5, sr: 48000, loadSec: 17, maxName: 100, overwrite: 1,
  STATE: ['idle', 'receiving', 'writing', 'copying'],
  RESULT: ['ok', 'no card', 'card full', 'write failed', 'bad name', 'not found', 'busy', 'bad file', 'already on the card', 'timed out', 'nothing to copy'],
  F: { mounted: 1, rootWavs: 2, oldSets: 4, oldPresets: 8, breaks: 16, pending: 32 },
  chunkFor: maxBody => Math.max(64, Math.min(1000, (maxBody || 512) - 24)) };
/* The module's name rule (core/bb_card.h BreakNameOk): 1..100 printable ASCII, none of / \ : * ? " < > |, not starting
 * with a space or a dot, not ending with a space or a dot. */
function breakNameOk(n) {
  if (typeof n !== 'string' || !n.length || n.length > CARD.maxName) return false;
  if (!/^[\x20-\x7e]+$/.test(n) || /[\/\\:*?"<>|]/.test(n)) return false;
  return n[0] !== ' ' && n[0] !== '.' && !/[ .]$/.test(n);
}
/* A file name → a stem the module takes: the extension off, accents folded, anything else outside printable ASCII
 * or reserved by FAT replaced, runs of spaces / dashes squeezed, the ends trimmed, at most 100; "break" if nothing is left. */
function breakName(fileName) {
  let s = String(fileName == null ? '' : fileName).replace(/\.[A-Za-z0-9]{1,5}$/, '');
  try { s = s.normalize('NFKD').replace(/[\u0300-\u036f]/g, ''); } catch {}
  s = s.replace(/\s+/g, ' ').replace(/[^\x20-\x7e]/g, '_').replace(/[\/\\:*?"<>|]/g, '-').replace(/-{2,}/g, '-');
  s = s.slice(0, CARD.maxName).replace(/^[ .]+/, '').replace(/[ .]+$/, '');
  return breakNameOk(s) ? s : 'break';
}
/* What the module will get from a decoded buffer: ≤ 2 channels (more: the first two), at 48 kHz, cut at 17 s. */
function uploadPlan(channels, sampleRate, frames) {
  const ch = Math.max(1, Math.min(2, channels | 0)), sr = sampleRate || CARD.sr;
  const at48 = Math.round(frames * CARD.sr / sr), keep = Math.min(at48, CARD.loadSec * CARD.sr);
  return { channels: ch, frames: keep, seconds: keep / CARD.sr, sourceSeconds: frames / sr, trimmed: at48 > keep, resample: sr !== CARD.sr, bytes: 44 + keep * ch * 2 };
}
/* Planes (Float32Array each, one or two) at `sr` → a canonical PCM16 WAV: RIFF / fmt (16 bytes, format 1) / data,
 * interleaved, each sample clamped to ±1 and scaled symmetrically (±1 → ±32767, NaN → 0). Pure. */
function encodeWav(planes, sr = CARD.sr, frames) {
  const ch = planes.length, n = frames == null ? Math.min(...planes.map(p => p.length)) : frames, data = n * ch * 2;
  const out = new Uint8Array(44 + data), dv = new DataView(out.buffer);
  const tag = (at, t) => { for (let i = 0; i < 4; i++) out[at + i] = t.charCodeAt(i); };
  tag(0, 'RIFF'); dv.setUint32(4, 36 + data, true); tag(8, 'WAVE'); tag(12, 'fmt '); dv.setUint32(16, 16, true);
  dv.setUint16(20, 1, true); dv.setUint16(22, ch, true); dv.setUint32(24, sr, true); dv.setUint32(28, sr * ch * 2, true);
  dv.setUint16(32, ch * 2, true); dv.setUint16(34, 16, true); tag(36, 'data'); dv.setUint32(40, data, true);
  let at = 44;
  for (let i = 0; i < n; i++) for (let c = 0; c < ch; c++) {
    let v = planes[c][i]; v = v > 1 ? 1 : v < -1 ? -1 : (v === v ? v : 0);
    dv.setInt16(at, Math.round(v * 32767), true); at += 2;
  }
  return out;
}
/* One card job on the wire at a time, whoever starts it (Kyklophoria's putQueued: two chunked transfers interleaved
 * make the module refuse one mid-stream). serial() returns a runner; every job waits for the one before. */
function serial() { let chain = Promise.resolve(); return job => { const p = chain.then(job); chain = p.catch(() => {}); return p; }; }
/* Send `bytes` as Audiothurgist/breaks/<name>.wav. One chunk per round trip at normal priority, so the page's clicks
 * (urgent) go first and its polls interleave. onProgress(acked, total, kbps). Resolves { bytes, ms, kbps } when the
 * module holds every byte (its write follows: watch the tail); rejects with the module's status name (EXISTS,
 * NO_CARD, CARD_FULL, BAD_STATE, TOO_LARGE, BAD_ARGS) or 'cancelled' (opts.cancel() true between chunks). */
async function uploadBreak(link, name, bytes, opts = {}) {
  if (!breakNameOk(name)) throw new Error('bad name');
  const total = bytes.length, chunk = opts.chunk || CARD.chunkFor(opts.maxBody), t0 = _now();
  await link.request(CMD.card, Uint8Array.of(CARD.begin, ...u32b(total), opts.overwrite ? CARD.overwrite : 0, ...nameBytes(name)), { urgent: true, timeoutMs: 4000 });
  let off = 0;
  while (off < total) {
    if (opts.cancel && opts.cancel()) { await link.request(CMD.card, Uint8Array.of(CARD.abort), { urgent: true }).catch(() => {}); throw new Error('cancelled'); }
    const n = Math.min(chunk, total - off), body = new Uint8Array(9 + n);
    body[0] = CARD.data; body.set(u32b(total), 1); body.set(u32b(off), 5); body.set(bytes.subarray(off, off + n), 9);
    const r = await link.request(CMD.card, body, { timeoutMs: 4000 });
    const have = u32(r, 1);
    if (have !== off + n) throw new Error('out of step (the module holds ' + have + ' of ' + total + ')');
    off = have;
    const ms = Math.max(1, _now() - t0);
    if (opts.onProgress) opts.onProgress(off, total, off / ms);
    if (r[5] === 2) throw new Error('not a WAV the module reads');
  }
  const ms = Math.max(1, _now() - t0);
  return { bytes: total, ms, kbps: total / ms };
}
const cardOp = (link, op) => link.request(CMD.card, Uint8Array.of(op), { urgent: true });
const copyOldCard = link => cardOp(link, CARD.copy);
const cardRescan = link => cardOp(link, CARD.rescan);
/* The module's list as it is now — labels as P5 shows them ("<name> [set]" for a set). */
async function cardList(link) {
  const out = []; let first = 0, total = 0;
  do {
    const b = await link.request(CMD.card, Uint8Array.of(CARD.list, first), { key: 'cardlist' + first });
    total = b[1]; const n = b[3]; let at = 4;
    for (let i = 0; i < n; i++) { const k = b[at]; out.push(new TextDecoder('ascii').decode(b.subarray(at + 1, at + 1 + k))); at += 1 + k; }
    if (!n) break; first += n;
  } while (first < total);
  return out;
}

/* ───────────── PADS (0x68, capability "pads": 1) ─────────────
 * A sample dropped onto a pad: the page decodes the file (48 kHz, as the card upload does), keeps two channels at most,
 * cuts it to the room the module has left past freePos (the state tail's pads.free) and sends raw PCM16 — little-endian,
 * interleaved L R for stereo, no header — in order from byte 0, one chunk per round trip: BEGIN(key, ch, frames), then
 * DATA(total, offset, bytes) until the last byte. The module holds the bytes in its sample memory past freePos, then its
 * main loop converts them in place and publishes the pad as key `key`'s recording — one undo step (the tail's pads seq
 * says when). Nothing is written to the card: a set save keeps the kit, a power-off loses it (pads.unsaved).
 * NEW_KIT clears the break (a silent ¼ s stub), every pad and marker, and puts the module in Pads. */
const PADS = { begin: 0, data: 1, abort: 2, newKit: 3, sr: 48000, minFrames: 480, stubFrames: 12000,
  STATE: ['idle', 'receiving', 'committing', 'clearing'],
  RESULT: ['ok', 'no room', 'busy', 'bad args', 'the table is full', 'timed out', 'cancelled', 'the module moved under it'],
  F: { unsaved: 1, noBreak: 2, madeStereo: 4 } };
/* What a pad gets from a decoded buffer: ≤ 2 channels (more: the first two), at 48 kHz, cut to `room` frames (the
 * module's free frames; null = uncut). tooShort under 480 frames (10 ms, the module's floor). */
function padPlan(channels, sampleRate, frames, room = null) {
  const ch = Math.max(1, Math.min(2, channels | 0)), sr = sampleRate || PADS.sr;
  const at48 = Math.round(frames * PADS.sr / sr), keep = room == null ? at48 : Math.max(0, Math.min(at48, room));
  return { channels: ch, frames: keep, seconds: keep / PADS.sr, sourceSeconds: frames / sr, cut: keep < at48, resample: sr !== PADS.sr,
    bytes: keep * ch * 2, tooShort: keep < PADS.minFrames };
}
/* Planes (one or two Float32Arrays) → raw PCM16 LE, interleaved, the encodeWav scale (±1 → ±32767, clamped, NaN → 0). */
function encodePcm16(planes, frames) {
  const ch = planes.length, n = frames == null ? Math.min(...planes.map(p => p.length)) : frames;
  const out = new Uint8Array(n * ch * 2), dv = new DataView(out.buffer);
  let at = 0;
  for (let i = 0; i < n; i++) for (let c = 0; c < ch; c++) {
    let v = planes[c][i]; v = v > 1 ? 1 : v < -1 ? -1 : (v === v ? v : 0);
    dv.setInt16(at, Math.round(v * 32767), true); at += 2;
  }
  return out;
}
/* Send `bytes` (encodePcm16's) as key `key`'s pad. One chunk per round trip at normal priority (the page's clicks go
 * first, its polls interleave). onProgress(acked, total, kbps). Resolves { bytes, ms, kbps, room } once the module holds
 * every byte (its commit follows: watch the tail's pads seq); rejects with the module's status name (TOO_LARGE: more
 * than the room · BAD_STATE: a set op, a load, a take, a card job or another drop's commit · BAD_ARGS) or 'cancelled'. */
async function uploadPad(link, key, ch, bytes, opts = {}) {
  const frames = Math.floor(bytes.length / (ch * 2)), total = frames * ch * 2, chunk = opts.chunk || CARD.chunkFor(opts.maxBody), t0 = _now();
  if (key < 0 || key > 23 || (ch !== 1 && ch !== 2) || frames < PADS.minFrames) throw new Error('BAD_ARGS');
  const r0 = await link.request(CMD.pads, Uint8Array.of(PADS.begin, key, ch, ...u32b(frames)), { urgent: true, timeoutMs: 4000 });
  const room = r0.length >= 5 ? u32(r0, 1) : null;
  let off = 0;
  while (off < total) {
    if (opts.cancel && opts.cancel()) { await link.request(CMD.pads, Uint8Array.of(PADS.abort), { urgent: true }).catch(() => {}); throw new Error('cancelled'); }
    const n = Math.min(chunk, total - off), body = new Uint8Array(9 + n);
    body[0] = PADS.data; body.set(u32b(total), 1); body.set(u32b(off), 5); body.set(bytes.subarray(off, off + n), 9);
    const r = await link.request(CMD.pads, body, { timeoutMs: 4000 });
    const have = u32(r, 1);
    if (have !== off + n) throw new Error('out of step (the module holds ' + have + ' of ' + total + ')');
    off = have;
    if (opts.onProgress) opts.onProgress(off, total, off / Math.max(1, _now() - t0));
  }
  const ms = Math.max(1, _now() - t0);
  return { bytes: total, ms, kbps: total / ms, room };
}
const newKit = link => link.request(CMD.pads, Uint8Array.of(PADS.newKit), { urgent: true });

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

/* ───────────── SCOPE (0x62 with a body; descriptor "scope2": 1 audio, 2 + cv) ─────────────
 * Ten channels on one sample clock in the firmware's rings: four audio at
 * 48 kHz — out L · out R · in L (J1) · in R (J2), 64 K samples — and six CV
 * jacks J3..J8 DC-coupled at ±5 V = ±127, one sample per audio block (2 kHz,
 * 16 K samples). Request: u8 chanMask, u8 decim 1..16, u16 n, u8 flags
 * (trig · peak · at + u32 pos · ext + u8 maskHi (J3..J8), u8 cvDecim 1..64,
 * i8 trigLevel, u8 trigSrc). Reply: u8 status, u8 chanMask, u8 decim, u16 n
 * (as clamped), u8 flags (triggered · pos honoured · ext header · detached),
 * u32 pos (on the audio clock, wrapping at 24·2^26; the cv start is pos / 24),
 * [u8 maskHi, u8 cvDecim, u8 cvDiv, u8 trigSrc], then n × i8 per audio channel
 * in mask order, then per cv jack in bit order. An empty body is the old
 * reply (parseScope). */
const SCOPE_CH = ['out L', 'out R', 'in L', 'in R', 'J3', 'J4', 'J5', 'J6', 'J7', 'J8'];
/* The jacks' roles under each map (state byte 175: 0 Brain, 1 Voice) and which of them are inputs. */
const SCOPE_JACK_ROLE = [['clock in', 'v/oct in', 'gate in', 'kick gate out', 'snare gate out', 'hat gate out'],
                         ['clock in', 'slice v/oct in', 'style v/oct in', 'gate in', 'hit trigger out', 'velocity out']];
const SCOPE_JACK_IN = [[true, true, true, false, false, false], [true, true, true, true, false, false]];
const SCOPE = { trig: 1, peak: 2, at: 4, ext: 8, triggered: 1, atHonoured: 2, extReply: 4, detached: 8,
  ring: 65536, usable: 65280, cvRing: 16384, cvUsable: 16368, cvDiv: 24, wrap: 1610612736, cvWrap: 67108864,
  slack: 480, maxN: 512, maxDecim: 16, cvMaxDecim: 64, hdr: 10, hdrExt: 4, reqMin: 5, reqPos: 9, reqExt: 13,
  cvPerVolt: 25.4, trigAuto: 0, trigSet: 0x80, trigNone: 255, hyst: 4 };
const scopeVolts = code => code / SCOPE.cvPerVolt;
const scopeCvCode = volts => Math.max(-127, Math.min(127, Math.round(volts * SCOPE.cvPerVolt)));
const scopeAge = (now, p, wrap = SCOPE.wrap) => now >= p ? now - p : now + (wrap - p);
/* ext: { maskHi, cvDecim, level, trig } — trig 0 = the first selected channel (audio before cv), or SCOPE.trigSet | 0..9. */
function scopeReq(mask, decim, n, flags = 0, pos = null, ext = null) {
  const withExt = !!ext, len = withExt ? SCOPE.reqExt : pos == null ? SCOPE.reqMin : SCOPE.reqPos;
  const b = new Uint8Array(len); const dv = new DataView(b.buffer);
  b[0] = mask & 0x0f; b[1] = decim; dv.setUint16(2, n, true);
  b[4] = (flags & ~(SCOPE.at | SCOPE.ext)) | (pos == null ? 0 : SCOPE.at) | (withExt ? SCOPE.ext : 0);
  if (len >= SCOPE.reqPos) dv.setUint32(5, (pos == null ? 0 : pos) >>> 0, true);
  if (withExt) { b[9] = (ext.maskHi || 0) & 0x3f; b[10] = ext.cvDecim || 1; b[11] = (ext.level || 0) & 0xff; b[12] = ext.trig || 0; }
  return b;
}
function parseScope2(b) {
  if (!b || b.length < SCOPE.hdr) return null;
  const mask = b[1], decim = b[2], n = u16(b, 3), flags = b[5], pos = u32(b, 6), ch = {};
  const f = { mask, decim, n, flags, pos, triggered: !!(flags & SCOPE.triggered), atHonoured: !!(flags & SCOPE.atHonoured),
    ext: !!(flags & SCOPE.extReply), detached: !!(flags & SCOPE.detached), maskHi: 0, cvDecim: 1, cvDiv: SCOPE.cvDiv, trigSrc: SCOPE.trigNone, ch };
  let at = SCOPE.hdr;
  if (f.ext) { if (b.length < SCOPE.hdr + SCOPE.hdrExt) return null; f.maskHi = b[10]; f.cvDecim = b[11]; f.cvDiv = b[12] || SCOPE.cvDiv; f.trigSrc = b[13]; at += SCOPE.hdrExt; }
  for (let c = 0; c < 4; c++) if (mask & (1 << c)) { ch[c] = Int8Array.from(b.subarray(at, at + n)); at += n; }
  for (let c = 0; c < 6; c++) if (f.maskHi & (1 << c)) { ch[4 + c] = Int8Array.from(b.subarray(at, at + n)); at += n; }
  return f;
}
/* The requests of one frame for this link's max_body: every selected channel
 * (audio mask + cv maskHi) in one request when that leaves ≥ minN samples per
 * channel (or all it could ever give), else the channels split over as few
 * requests as hold the largest n — the later requests are pinned to the first
 * one's pos so the tiers stay time-aligned. n is held to each tier's ring
 * (n · decim ≤ 65280 audio, n · cvDecim ≤ 16368 cv). Returns { n, reqs:
 * [{ mask, maskHi }], ext, masks } (masks: the audio masks, for old callers). */
function scopePlan(maskOrMaxBody, maskArg, decimArg, nWant = SCOPE.maxN, minN = 200, maskHi = 0, cvDecim = 1) {
  const maxBody = maskOrMaxBody, mask = maskArg & 0x0f, decim = decimArg; maskHi &= 0x3f;
  const chans = []; for (let c = 0; c < 4; c++) if (mask & (1 << c)) chans.push(c); for (let c = 0; c < 6; c++) if (maskHi & (1 << c)) chans.push(4 + c);
  const ext = maskHi !== 0, hdr = SCOPE.hdr + (ext ? SCOPE.hdrExt : 0);
  const toReq = list => ({ mask: list.filter(c => c < 4).reduce((m, c) => m | (1 << c), 0), maskHi: list.filter(c => c >= 4).reduce((m, c) => m | (1 << (c - 4)), 0) });
  const done = (n, groups) => { const reqs = groups.map(toReq); return { n, reqs, ext, masks: reqs.map(r => r.mask) }; };
  if (!chans.length) return done(0, []);
  let cap = Math.min(SCOPE.maxN, nWant); if (mask) cap = Math.min(cap, Math.floor(SCOPE.usable / decim)); if (maskHi) cap = Math.min(cap, Math.floor(SCOPE.cvUsable / cvDecim)); cap = Math.max(1, cap);
  const room = Math.max(1, maxBody - hdr);
  const one = Math.min(cap, Math.floor(room / chans.length));
  if (one >= Math.min(minN, cap)) return done(one, [chans]);
  /* split: the later requests must still find the first one's window in the ring, so leave
   * SCOPE.slack audio samples (10 ms) of the usable span for the link's round trips */
  let capS = Math.min(cap, Math.floor((SCOPE.usable - SCOPE.slack) / decim)); if (maskHi) capS = Math.min(capS, Math.floor((SCOPE.cvUsable - Math.ceil(SCOPE.slack / SCOPE.cvDiv)) / cvDecim)); capS = Math.max(1, capS);
  const perMax = Math.max(1, Math.floor(room / Math.min(minN, capS))), nReqs = Math.ceil(chans.length / perMax), per = Math.ceil(chans.length / nReqs);
  const n = Math.min(capS, Math.floor(room / per)), groups = [];
  for (let i = 0; i < chans.length; i += per) groups.push(chans.slice(i, i + per));
  return done(n, groups);
}
/* One frame over the plan: the first request carries the trigger, the rest
 * ask for its pos; a later request the ring could no longer honour tears the
 * frame (an error — the caller just takes the next one). `ext` = { cvDecim,
 * level, trig } is sent when the plan has cv channels (or when given).
 * Resolves { n, decim, cvDecim, cvDiv, pos, triggered, detached, trigSrc,
 * ch: {c: Int8Array} (0..3 audio, 4..9 J3..J8), reqs, bytes }. */
async function fetchScopeFrame(link, plan, decim, flags = 0, opts = {}, ext = null) {
  const out = { n: plan.n, decim, cvDecim: ext ? (ext.cvDecim || 1) : 1, cvDiv: SCOPE.cvDiv, pos: 0, triggered: false, detached: false, trigSrc: SCOPE.trigNone, ch: {}, reqs: plan.reqs.length, bytes: 0 }; let pos = null;
  for (const r of plan.reqs) {
    const e = (plan.ext || ext) ? { maskHi: r.maskHi, cvDecim: ext ? (ext.cvDecim || 1) : 1, level: ext ? (ext.level || 0) : 0, trig: ext ? (ext.trig || 0) : 0 } : null;
    const b = await link.request(CMD.scope, scopeReq(r.mask, decim, out.n, flags, pos, e), Object.assign({ key: 'scope' + r.mask + ':' + r.maskHi }, opts));
    const f = parseScope2(b); if (!f) throw new Error('short scope reply');
    out.bytes += b.length;
    if (pos == null) { pos = f.pos; out.pos = f.pos; out.triggered = f.triggered; out.detached = f.detached; out.n = f.n; out.cvDecim = f.cvDecim; out.cvDiv = f.cvDiv; out.trigSrc = f.trigSrc; }
    else if (!f.atHonoured || f.pos !== pos || f.n !== out.n) throw new Error('scope frame torn');
    Object.assign(out.ch, f.ch);
  }
  return out;
}

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

const api = { PROTO, CMD, ACT, REC, NOTE_NAMES, keyName, STATUS, statusName, crc32, cobsEncode, cobsDecode, buildFrame, FrameParser, kMaxFrameBytes, Link, SerialTransport, WsTransport, asTransport,
  u16, u32, f32, readStr, hello, getDescriptor, getLive, setLive, concat,
  STATE_MIN, ZONE_MULT, parseState, fetchEnv, parseScope, SCOPE, SCOPE_CH, SCOPE_JACK_ROLE, SCOPE_JACK_IN, scopeVolts, scopeCvCode, scopeAge, scopeReq, parseScope2, scopePlan, fetchScopeFrame, allFields, findField, fieldLabels, pagerInfo, optionsInfo, manualInfo,
  TABLE_ENTRY, TABLE_ENTRY_EDIT, TABLE_CHUNK, fetchWave, parseWave, fetchTable, parseTableChunk, fetchHits, parseHitsChunk,
  SET_STATE, SET_RESULT, setNameOk, setLabelName, nameBytes, saveSet, loadSet,
  EDIT, editOp, editBoundary, editEntry, editAdd, editSplit, editMerge, editTombstone, editSetKey, editSwapKeys, editNudgeAll, editPromote, editFromOnsets, audition, release, tapMark, editUndo, editRedo,
  KEYS, playKey, keysFromMarkers, clearMarkers, setMute, autoslice, caps,
  CARD, breakNameOk, breakName, uploadPlan, encodeWav, serial, uploadBreak, copyOldCard, cardRescan, cardList,
  PADS, padPlan, encodePcm16, uploadPad, newKit,
  KEYP, keyParams, parseKeyParams, fetchKeyParams, keypNeutral, keypText, keypGainText, keypTuneText };
if (typeof module === 'object' && module.exports) module.exports = api; else root.HL = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
