/* edit.js — the Kit tab's slice editor, the part that has no DOM in it.
 *
 * EDIT-UI (docs/packages/slice-edit-ui.md, the design's Part B). Everything a
 * finger does on the Kit waveform is decided here — snap magnets, handle
 * hit-testing with touch slop, the clamp rules for linked grid boundaries and
 * free markers, the gesture recogniser (tap / double-tap / long-press / drag /
 * pinch / second finger), the key-drop rule (swap / replace / red), the onset
 * histogram's preview, the history labels, bar:beat:16th formatting and the
 * Editor controller that turns gestures into ACTION ops over a HostLink with
 * one edit in flight, auditions rate-limited and a refetch-and-retry on a
 * stale gen. index.html only draws what the Editor holds and forwards pointer
 * events; selftest.mjs drives the same Editor against the fake module.
 *
 * Plain script in the browser (globals via window.ED); CommonJS in node. */
(function (root) {
'use strict';
const HL = typeof module === 'object' && module.exports ? require('./hostlink.js') : root.HL;

const SR = 48000, MS = 48;
/* The rules, in samples and pixels: B1.7's magnets (±12 ms onset, ±2 ms zero crossing), the engine's minimums
 * (64 per grid slice, 480 per marker), the touch geometry (44 px handles, half of it as slop either side), the
 * gesture clocks, the audition window (≤ 10/s) and preview length, the tap mark's snap and the onset gap. */
const K = { snapOnset: 12 * MS, snapZero: 2 * MS, zeroMaxStep: 8, gridMin: 64, recMin: 480, keys: 24,
  handlePx: 44, slopPx: 22, tapPx: 10, dblMs: 350, holdMs: 500, auditionMs: 100, previewMs: 250, tapSnapMs: 60,
  minGapMs: 35, tapAllowMs: [0, 20, 40, 60, 80], tapAllowDefault: 40, histMax: 64, stripPx: 46 };

/* ───────────── table helpers ───────────── */
const spanSlice = (key, n) => { const span = Math.min(n, K.keys), kk = key >= span ? span - 1 : key; return Math.floor(kk * n / span); };
const isTomb = e => !e || !!(e.flags & HL.EDIT.tomb) || e.len <= 0;
const isLive = e => !isTomb(e);
const isMarker = (tb, t) => !!tb && t >= tb.nSlices && t < tb.entries.length;
const isGrid = (tb, t) => !!tb && t >= 0 && t < tb.nSlices;
const isOrange = e => !!(e && (e.flags & HL.EDIT.orange));
/* A recording is a marker with a recorder (tableKey ≥ 0) — the red-warning test's definition, not the map's. */
const isRecording = (tb, t) => isMarker(tb, t) && tb.entries[t].key >= 0;
const endOf = e => e.start + e.len;

/* ───────────── snap magnets ───────────── */
/* The nearest onset within ±r of p (the earlier wins a tie), else null. */
function nearestOnset(hits, p, r = K.snapOnset) {
  if (!hits) return null; let best = null, bd = r + 1;
  for (const o of hits) { const d = Math.abs(o - p); if (d < bd) { bd = d; best = o; } }
  return best;
}
/* The nearest sign change within ±r of p. sampleAt(i) gives the sample (any decimation the caller accepts) or null
 * when it is not cached — then the search gives up: a zero crossing is a claim about the audio, not a guess. */
function zeroCross(p, r, sampleAt) {
  if (typeof sampleAt !== 'function') return null;
  const sgn = i => { const v = sampleAt(i); return v == null ? null : v > 0 ? 1 : v < 0 ? -1 : 0; };
  for (let d = 0; d <= r; d++) for (const i of d ? [p - d, p + d] : [p]) {
    if (i < 1) continue; const a = sgn(i - 1), b = sgn(i); if (a == null || b == null) return null;
    if (a !== b || b === 0) return i;
  }
  return null;
}
/* B1.7: onset first, then a zero crossing (when `zero` is on), both off with `off` (a second finger, Shift). */
function snap(p, o = {}) {
  p = Math.round(p);
  if (o.off) return { pos: p, by: null };
  const on = nearestOnset(o.hits, p, o.onsetR == null ? K.snapOnset : o.onsetR);
  let pos = on == null ? p : on, by = on == null ? null : 'onset';
  if (o.zero) { const z = zeroCross(pos, o.zeroR == null ? K.snapZero : o.zeroR, o.sampleAt); if (z != null && z !== pos) { pos = z; by = by ? 'onset+zero' : 'zero'; } }
  return { pos, by };
}

/* ───────────── clamps ───────────── */
/* A grid boundary i (0 < i < nSlices) stays ≥ 64 from both neighbours' far ends; null for a boundary that cannot move. */
function clampBoundary(tb, i, pos) {
  if (!tb || i <= 0 || i >= tb.nSlices) return null;
  const lo = tb.entries[i - 1].start + K.gridMin, hi = endOf(tb.entries[i]) - K.gridMin;
  if (hi < lo) return null;
  return Math.max(lo, Math.min(hi, Math.round(pos)));
}
/* A marker's start moves with its end fixed (len ≥ 480, start ≥ 0); its end moves with the start fixed (≤ freePos). */
function clampStart(e, start, freePos) { const end = Math.min(endOf(e), freePos); start = Math.max(0, Math.min(end - K.recMin, Math.round(start))); return { start, len: end - start }; }
function clampEnd(e, end, freePos) { end = Math.max(e.start + K.recMin, Math.min(freePos, Math.round(end))); return { start: e.start, len: end - e.start }; }

/* ───────────── handles and hit-testing ───────────── */
/* Every movable start as a handle: grid boundaries 1..n−1 (boundary 0 is the buffer's edge), every live marker's
 * start, and the selected marker's end. Tombstones have none. */
function handleList(tb, selT = -1) {
  const out = []; if (!tb) return out;
  for (let i = 1; i < tb.nSlices; i++) out.push({ t: i, kind: 'grid', pos: tb.entries[i].start });
  for (let t = tb.nSlices; t < tb.entries.length; t++) { const e = tb.entries[t]; if (isLive(e)) out.push({ t, kind: 'start', pos: e.start }); }
  if (isMarker(tb, selT) && isLive(tb.entries[selT])) out.push({ t: selT, kind: 'end', pos: endOf(tb.entries[selT]) });
  return out;
}
/* The handle under x (canvas px) within `slop`, nearest first; the selection wins a tie so a start and an end that
 * sit together resolve to what the user is looking at. */
function hitHandle(list, x, xOf, slop = K.slopPx, selT = -1) {
  let best = null, bd = slop + 0.001;
  for (const h of list) { const d = Math.abs(xOf(h.pos) - x) - (h.t === selT ? 0.5 : 0); if (d < bd) { bd = d; best = h; } }
  return best;
}
/* The entry under a sample position: the smallest live marker covering it, else the grid slice, else −1. */
function entryAt(tb, p) {
  if (!tb) return -1; let best = -1, bl = Infinity;
  for (let t = tb.nSlices; t < tb.entries.length; t++) { const e = tb.entries[t]; if (isLive(e) && p >= e.start && p < endOf(e) && e.len < bl) { best = t; bl = e.len; } }
  if (best >= 0) return best;
  for (let i = 0; i < tb.nSlices; i++) { const e = tb.entries[i]; if (p >= e.start && p < endOf(e)) return i; }
  return -1;
}
/* What MERGE(t) would swallow (the engine's rule): the live marker ending at t's start — adjacent — else the previous
 * live marker by index; null when there is none. The page offers merge only when `adjacent`. */
function mergeTarget(tb, t) {
  if (!isMarker(tb, t) || isTomb(tb.entries[t])) return null;
  const E = tb.entries, s = E[t].start;
  for (let v = tb.nSlices; v < E.length; v++) if (v !== t && isLive(E[v]) && endOf(E[v]) === s) return { u: v, adjacent: true };
  for (let v = t - 1; v >= tb.nSlices; v--) if (isLive(E[v])) return { u: v, adjacent: false };
  return null;
}
/* The grid boundary nearest a position (for PROMOTE), 1..n−1; −1 with fewer than two slices. */
function nearestBoundary(tb, pos) { let best = -1, bd = Infinity; for (let i = 1; i < (tb ? tb.nSlices : 0); i++) { const d = Math.abs(tb.entries[i].start - pos); if (d < bd) { bd = d; best = i; } } return best; }
/* Every key pointing at entry t. */
const keysOn = (keySlice, t) => { const out = []; if (keySlice) for (let k = 0; k < K.keys; k++) if (keySlice[k] === t) out.push(k); return out; };

/* ───────────── positions: samples · ms · bar:beat:16th ───────────── */
/* A 16th is 60 / nativeBpm / 4 s; without a native tempo the grid's own step (bufLen / nSlices) stands in. */
function stepLen(ctx) { if (ctx && ctx.nativeBpm > 0) return SR * 60 / ctx.nativeBpm / 4; if (ctx && ctx.bufLen > 0 && ctx.nSlices > 0) return ctx.bufLen / ctx.nSlices; return 0; }
function fmtBbs(p, ctx) { const sl = stepLen(ctx); if (!sl) return '—'; const s = Math.floor(p / sl + 1e-9), bar = Math.floor(s / 16) + 1, beat = Math.floor((s % 16) / 4) + 1, six = (s % 4) + 1; return `${bar}:${beat}:${six}`; }
const fmtMs = p => { const ms = p / MS; return (Math.abs(ms) >= 100 ? ms.toFixed(0) : ms.toFixed(1)).replace(/\.0$/, '') + ' ms'; };
const fmtDelta = d => (d < 0 ? '−' : '+') + fmtMs(Math.abs(d));
function fmtPos(p, unit, ctx) { return unit === 'ms' ? fmtMs(p) : unit === 'bbs' ? fmtBbs(p, ctx) : String(Math.round(p)); }
/* "1234" (samples, or the field's unit), "12.5ms", "0.5 s", "2:3:1" (bar:beat:16th, 1-based) → samples; null when unreadable. */
function parsePos(str, unit = 'samples', ctx = null) {
  const s = String(str).trim().replace(/−/g, '-'); let m;
  if ((m = /^(\d+)\s*:\s*(\d+)\s*:\s*(\d+)$/.exec(s))) { const sl = stepLen(ctx); if (!sl) return null; return Math.round(((+m[1] - 1) * 16 + (+m[2] - 1) * 4 + (+m[3] - 1)) * sl); }
  if ((m = /^(-?\d+(?:\.\d+)?)\s*ms$/i.exec(s))) return Math.round(+m[1] * MS);
  if ((m = /^(-?\d+(?:\.\d+)?)\s*s$/i.exec(s))) return Math.round(+m[1] * SR);
  if ((m = /^(-?\d+(?:\.\d+)?)$/.exec(s))) { const v = +m[1]; if (unit === 'ms') return Math.round(v * MS); if (unit === 'bbs') return null; return Math.round(v); }
  return null;
}

/* ───────────── history labels ───────────── */
const kn = k => HL.keyName(k);
function histLabel(op, a = {}) {
  switch (op) {
    case 'boundary': return `boundary ${a.i} ${fmtDelta(a.d)}`;
    case 'start': return `start ${a.t} ${fmtDelta(a.d)}`;
    case 'end': return `end ${a.t} ${fmtDelta(a.d)}`;
    case 'split': return `split ${a.t}` + (a.newIdx != null && a.newIdx >= 0 ? ` → ${a.newIdx}` : '');
    case 'merge': return `merge ${a.u} into ${a.t}`;
    case 'delete': return `delete ${a.t}`;
    case 'copy': return a.newIdx == null ? `copy ${a.from} as a marker` : `marker ${a.newIdx} from ${a.from}`;
    case 'key': return `${kn(a.key)} → ${a.t}`;
    case 'swap': return `${kn(a.a)} ↔ ${kn(a.b)}`;
    case 'nudge': return `nudge ${a.which === HL.EDIT.grid ? 'grid' : 'markers'} ${fmtDelta(a.ms * MS)}`;
    case 'onsets': return `onsets ≥ ${Math.round(a.thresh / 2.55)} % → ${a.count == null ? '…' : a.count}`;
    case 'tap': return a.newIdx == null ? 'tap' : `tap → marker ${a.newIdx}`;
    case 'promote': return `promote ${a.t} → boundary ${a.i}`;
    case 'follow': return 'keys follow the grid';
    case 'mkeys': return a.count == null ? 'markers → keys' : `markers → keys (${a.count})`;
    case 'mclear': return a.count == null ? 'clear markers' : `clear markers (${a.count})`;
    case 'auto': return `autoslice ${a.k}${a.reroll ? ' (re-roll)' : ''}` + (a.count == null ? '' : ` → ${a.count}`);
    case 'keyp': return `${kn(a.key)} settings: ${(a.p && HL.keypText(a.p)) || 'neutral'}`;   /* KEYP */
    case 'undo': return 'undo'; case 'redo': return 'redo';
    case 'module': return 'changed on the module';
    default: return op;
  }
}
/* The page's own label list keyed by tableGen — a viewer over the firmware's journal (EDIT-UNDO), never a second
 * source of truth. A gen the page did not cause (a take, the panel, a re-slice) gets the 'module' label. An undo
 * marks the newest live entry `undone` (it stays listed, struck through), a redo un-marks the oldest undone one, a new
 * op drops the undone entries (the module cleared its redo half), and the tail's redoDepth reconciles: 0 → nothing is
 * undone any more. The depths are the module's; the marks are only a picture of them. */
class History {
  constructor(max = K.histMax) { this.max = max; this.items = []; this.gen = null; this.undoDepth = 0; this.redoDepth = 0; }
  push(gen, label) { this.items = this.items.filter(x => !x.undone); this.items.push({ gen, label, undone: false }); if (this.items.length > this.max) this.items.shift(); this.gen = gen; }
  /* the newest live line, if its label matches, takes the new gen and label (a continued drag) */
  joinTop(gen, label, re) { const t = this.items[this.items.length - 1]; if (!t || t.undone || !re.test(t.label)) return false; t.gen = gen; t.label = label; this.gen = gen; return true; }
  undo(gen) { for (let i = this.items.length - 1; i >= 0; i--) if (!this.items[i].undone) { this.items[i].undone = true; break; } this.gen = gen; }
  redo(gen) { for (let i = 0; i < this.items.length; i++) if (this.items[i].undone) { this.items[i].undone = false; break; } this.gen = gen; }
  /* the state's tail: a gen the page has not labelled is the module's; a redo depth of 0 means nothing is undone */
  note(gen, undoDepth = 0, redoDepth = 0) {
    this.undoDepth = undoDepth; this.redoDepth = redoDepth;
    if (!redoDepth && this.items.some(x => x.undone)) this.items = this.items.filter(x => !x.undone);
    if (gen == null) return false; if (this.gen == null) { this.gen = gen; return false; } if (gen !== this.gen) { this.push(gen, histLabel('module')); return true; } return false;
  }
  list() { return this.items.slice().reverse(); }
}

/* ───────────── keys: the drop rule (B4) ───────────── */
/* src = { t } (a slice or marker dragged from the waveform) or { key } (a cell). Returns null for nothing to do, else
 * { kind: 'set' | 'swap' | 'choose', red, def, choices: [{ id, ops: [[op, ...args]] }] }. Occupied = the target key
 * points somewhere other than its span default; red = the drop would leave a recording with no key on it, and then
 * the default is swap (the recording moves to the dragged key) whenever a swap is possible. */
function dropPlan(ctx, src, dst) {
  const { table: tb, keySlice } = ctx; if (!tb || !keySlice || dst < 0 || dst >= K.keys) return null;
  const srcKey = src.key != null ? src.key : -1, t = srcKey >= 0 ? keySlice[srcKey] : src.t;
  if (srcKey === dst || t == null || t < 0 || t >= tb.entries.length || isTomb(tb.entries[t])) return null;
  const cur = keySlice[dst], occupied = cur !== spanSlice(dst, tb.nSlices);
  const others = keysOn(keySlice, cur).filter(k => k !== dst && k !== srcKey).length;
  const red = occupied && isRecording(tb, cur) && others === 0 && cur !== t;
  const swapWith = srcKey >= 0 ? srcKey : (keysOn(keySlice, t).find(k => k !== dst) ?? -1);
  const setOp = ['setKey', dst, t], swapOp = swapWith >= 0 ? ['swapKeys', swapWith, dst] : null;
  if (!occupied || cur === t) return srcKey >= 0 && swapOp ? { kind: 'swap', red: false, def: 'swap', choices: [{ id: 'swap', ops: [swapOp] }] } : { kind: 'set', red: false, def: 'replace', choices: [{ id: 'replace', ops: [setOp] }] };
  const choices = []; if (swapOp) choices.push({ id: 'swap', ops: [swapOp] }); choices.push({ id: 'replace', ops: [setOp] }, { id: 'cancel', ops: [] });
  return { kind: 'choose', red, def: red && swapOp ? 'swap' : 'replace', choices };
}

/* ───────────── the onset histogram (B2.2) ───────────── */
/* An onset's strength is the salience of the grid slice it falls in (the table carries positions only). */
function onsetBars(hits, tb) {
  const out = []; if (!hits || !tb) return out;
  for (const p of hits) { let s = 0; for (let i = 0; i < tb.nSlices; i++) { const e = tb.entries[i]; if (p >= e.start && p < endOf(e)) { s = e.sal; break; } } out.push({ pos: p, strength: s }); }
  return out;
}
/* What FROM_ONSETS(thresh, minGap) would make: onsets ≥ thresh, ≥ minGap apart, each spanning to the next accepted
 * (the last to the break's end), spans under 480 skipped. */
function onsetAccept(bars, thresh, minGap, bufLen) {
  const acc = []; let last = -1;
  for (const b of bars) { if (b.pos < 0 || b.pos >= bufLen || b.strength < thresh) continue; if (last >= 0 && b.pos - last < minGap) continue; acc.push(b.pos); last = b.pos; }
  const spans = []; for (let j = 0; j < acc.length; j++) { const end = j + 1 < acc.length ? acc[j + 1] : bufLen; if (end - acc[j] >= K.recMin) spans.push({ start: acc[j], len: end - acc[j] }); }
  return { positions: acc, spans, count: spans.length };
}

/* ───────────── the audition window ───────────── */
/* ≤ one item per minMs: offer() returns the item to send now or holds it (a later one replaces); due() returns the
 * held item once the window has passed — so a drag's last position always sounds and nothing queues. */
class RateGate {
  constructor(minMs = K.auditionMs) { this.minMs = minMs; this.last = -Infinity; this.held = null; }
  offer(item, now) { if (now - this.last >= this.minMs) { this.last = now; this.held = null; return item; } this.held = item; return null; }
  due(now) { if (this.held && now - this.last >= this.minMs) { const i = this.held; this.held = null; this.last = now; return i; } return null; }
  get wait() { return this.held ? Math.max(0, this.minMs - 0) : 0; }
  clear() { this.held = null; }
}

/* ───────────── the gesture recogniser ───────────── */
/* Pointer events in, gestures out: press · hold (holdMs still) · tap · dbltap (within dblMs, same tag) · dragstart /
 * drag / dragend (past slop) · second / secondup (another finger while the first is down) · pinch (both moving) ·
 * release (every lift of the first finger). The caller tags the press with what it landed on and gets the tag back. */
class Gesture {
  constructor(o = {}) { this.o = Object.assign({ slop: K.tapPx, dblMs: K.dblMs, holdMs: K.holdMs }, o); this.p = null; this.q = null; this.lastTap = null; }
  down(id, x, y, t, tag = null) {
    if (this.p && this.p.id !== id) { this.q = { id, x, y }; this.p.pinch0 = null; this.p.multi = true; return [{ type: 'second', id, x, y, tag: this.p.tag }]; }
    this.p = { id, x, y, x0: x, y0: y, t0: t, tag, drag: false, held: false, moved: false };
    return [{ type: 'press', id, x, y, tag }];
  }
  move(id, x, y, t) {
    const p = this.p; if (!p) return [];
    if (this.q && this.q.id === id) { const q = this.q; q.x = x; q.y = y; return this._pinch(); }
    if (p.id !== id) return [];
    const out = []; p.x = x; p.y = y;
    if (this.q) return this._pinch();
    if (!p.drag && (Math.abs(x - p.x0) > this.o.slop || Math.abs(y - p.y0) > this.o.slop)) { if (p.held) return []; p.drag = true; out.push({ type: 'dragstart', id, x: p.x0, y: p.y0, dx: x - p.x0, dy: y - p.y0, tag: p.tag }); }
    if (p.drag) { p.moved = true; out.push({ type: 'drag', id, x, y, dx: x - p.x0, dy: y - p.y0, tag: p.tag }); }
    return out;
  }
  _pinch() {
    const p = this.p, q = this.q, d = Math.hypot(q.x - p.x, q.y - p.y), cx = (p.x + q.x) / 2;
    if (p.pinch0 == null) { p.pinch0 = { d: Math.max(1, d), cx }; return []; }
    const ev = { type: 'pinch', scale: d / p.pinch0.d, cx, dcx: cx - p.pinch0.cx, tag: p.tag }; p.pinch0 = { d: Math.max(1, d), cx }; return [ev];
  }
  up(id, x, y, t) {
    const p = this.p; if (!p) return [];
    if (this.q && this.q.id === id) { this.q = null; p.pinch0 = null; return [{ type: 'secondup', id, tag: p.tag }]; }
    if (p.id !== id) return [];
    const out = []; this.p = null; this.q = null;
    if (p.drag) out.push({ type: 'dragend', id, x, y, dx: x - p.x0, dy: y - p.y0, tag: p.tag });
    else if (!p.held && !p.multi) {
      const lt = this.lastTap, dbl = lt && t - lt.t <= this.o.dblMs && Math.abs(lt.x - p.x0) <= this.o.slop * 2 && Math.abs(lt.y - p.y0) <= this.o.slop * 2 && sameTag(lt.tag, p.tag);
      if (dbl) { this.lastTap = null; out.push({ type: 'dbltap', id, x: p.x0, y: p.y0, tag: p.tag }); }
      else { this.lastTap = { t, x: p.x0, y: p.y0, tag: p.tag }; out.push({ type: 'tap', id, x: p.x0, y: p.y0, tag: p.tag }); }
    }
    out.push({ type: 'release', id, x, y, tag: p.tag, held: p.held, dragged: p.drag });
    return out;
  }
  cancel(id) { const p = this.p; if (!p) return []; if (this.q && this.q.id === id) { this.q = null; return [{ type: 'secondup', id, tag: p.tag }]; } if (p.id !== id) return []; this.p = null; this.q = null; return [{ type: 'cancel', id, tag: p.tag }, ...(p.drag ? [{ type: 'dragend', id, x: p.x, y: p.y, dx: p.x - p.x0, dy: p.y - p.y0, tag: p.tag, cancelled: true }] : []), { type: 'release', id, tag: p.tag, held: p.held, dragged: p.drag, cancelled: true }]; }
  /* the clock: a still press past holdMs is a hold (once) */
  tick(t) { const p = this.p; if (!p || p.drag || p.held || this.q || t - p.t0 < this.o.holdMs) return []; p.held = true; return [{ type: 'hold', id: p.id, x: p.x0, y: p.y0, tag: p.tag }]; }
  get active() { return !!this.p; }
}
function sameTag(a, b) { if (a === b) return true; if (!a || !b || typeof a !== 'object' || typeof b !== 'object') return false; return a.zone === b.zone && a.t === b.t && (a.h ? a.h.kind : null) === (b.h ? b.h.kind : null) && (a.h ? a.h.t : null) === (b.h ? b.h.t : null); }

/* ───────────── the Editor ─────────────
 * deps: { link(): Link (null when disconnected), ctx(): { table, st, hits, sampleAt, editOk, tapAllowMs }, view: { a, b }
 * (shared with the page), cb: { redraw(), status(msg, kind), select(t), refetch(): Promise, setView(a, b),
 * zoomView(f, at), fitEntry(t), menu(items, x, y), choose(plan, x, y): Promise<id|null>, dropTarget(cx, cy): key|-1 },
 * now(), setTimer / clearTimer }. Pointer events arrive as pointer(kind, { id, x, y, w, h, t, cx, cy }) in canvas px. */
class Editor {
  constructor(deps) {
    this.d = deps; this.cb = deps.cb || {}; this.now = deps.now || (() => (typeof performance !== 'undefined' ? performance.now() : Date.now()));
    this.setTimer = deps.setTimer || ((f, ms) => setTimeout(f, ms)); this.clearTimer = deps.clearTimer || (id => clearTimeout(id));
    this.g = new Gesture(deps.gesture || {}); this.gate = new RateGate(deps.auditionMs || K.auditionMs); this.history = new History();
    this.selT = -1; this.drag = null; this.busy = null; this.snapOff = false; this.zero = { grid: true, markers: false };
    this.looping = false; this.holdTimer = null; this.gateTimer = null; this.unit = 'samples'; this.lastErr = '';
    this.stats = { auditions: 0, releases: 0, commits: 0, refused: 0, retried: 0 };
    this.size = { w: 1, h: 1 };
  }
  /* ── layout: the wave, the handle row, the pan / zoom strip (an old firmware: the wave alone, drag = pan) ── */
  layout(w, h) {
    this.size = { w, h }; const c = this.d.ctx();
    if (!c.editOk) return { wave: [0, h], handles: [h, h], strip: [h, h], edit: false };   /* an old firmware: the wave alone; a drag anywhere pans */
    const hw = Math.max(40, h - K.handlePx - K.stripPx); return { wave: [0, hw], handles: [hw, hw + K.handlePx], strip: [hw + K.handlePx, h], edit: true };
  }
  zoneAt(y, L) { return L.edit && y >= L.handles[0] && y < L.handles[1] ? 'handles' : y >= L.strip[0] ? 'strip' : 'wave'; }
  xOf(p, w = this.size.w) { const v = this.d.view; return (p - v.a) / Math.max(1, v.b - v.a) * w; }
  pOf(x, w = this.size.w) { const v = this.d.view; return v.a + x / w * (v.b - v.a); }
  handles() { const c = this.d.ctx(); return handleList(c.table, this.selT); }
  /* a handle's drawn position: the drag's local one while it moves */
  handlePos(h) { const dr = this.drag; return dr && dr.h && dr.h.t === h.t && dr.h.kind === h.kind ? dr.pos : h.pos; }
  /* ── the tag: what a press landed on ── */
  tagAt(x, y, w, h) {
    const c = this.d.ctx(), L = this.layout(w, h), zone = this.zoneAt(y, L), p = Math.round(this.pOf(x, w));
    if (!c.table) return { zone, h: null, t: -1, p };
    if (zone === 'handles') return { zone, h: hitHandle(this.handles(), x, q => this.xOf(q, w), K.slopPx, this.selT), t: -1, p };
    if (zone === 'wave') { const hh = L.edit ? hitHandle(this.handles(), x, q => this.xOf(q, w), 6, this.selT) : null; return { zone, h: hh && y >= L.wave[1] - 12 ? hh : null, t: entryAt(c.table, p), p }; }
    return { zone, h: null, t: -1, p };
  }
  /* ── pointer input from the canvas ── */
  pointer(kind, ev) {
    const t = ev.t != null ? ev.t : this.now(); let evs = [];
    if (kind === 'down') { const tag = this.tagAt(ev.x, ev.y, ev.w, ev.h); tag.cx = ev.cx; tag.cy = ev.cy; evs = this.g.down(ev.id, ev.x, ev.y, t, tag); this._armHold(); }
    else if (kind === 'move') evs = this.g.move(ev.id, ev.x, ev.y, t);
    else if (kind === 'up') evs = this.g.up(ev.id, ev.x, ev.y, t);
    else if (kind === 'cancel') evs = this.g.cancel(ev.id);
    else if (kind === 'tick') evs = this.g.tick(t);
    for (const e of evs) { e.cx = ev.cx; e.cy = ev.cy; e.w = ev.w; e.h = ev.h; this.onGesture(e); }
    return evs;
  }
  _armHold() { if (this.holdTimer) this.clearTimer(this.holdTimer); this.holdTimer = this.setTimer(() => { this.holdTimer = null; for (const e of this.g.tick(this.now())) this.onGesture(e); }, this.g.o.holdMs + 1); }
  wheel(x, w, dy) { const c = this.d.ctx(); if (!c.st || !c.st.freePos) return; this.cb.zoomView(Math.pow(1.25, dy / 100), this.pOf(x, w)); }
  keyModifier(shift) { this.snapOff = !!shift; }
  /* ── gesture → action (the map of docs/packages/slice-edit-ui.md) ── */
  onGesture(e) {
    const c = this.d.ctx(), tag = e.tag || {}, L = this.layout(e.w || this.size.w, e.h || this.size.h);
    switch (e.type) {
      case 'press': break;
      case 'second': this.snapOff = true; if (this.drag) this._dragTo(this.drag.x); break;
      case 'secondup': this.snapOff = false; break;
      case 'hold':
        if (tag.zone === 'handles' && tag.h) { if (tag.h.kind === 'end') this._loopStart(tag.h); else this.menuFor(tag.h.t, e.cx, e.cy); }
        else if (tag.zone === 'wave' && tag.t >= 0) this.menuFor(tag.t, e.cx, e.cy);
        break;
      case 'dragstart':
        if (tag.zone === 'handles' && tag.h) this._dragBegin(tag.h, e.x);
        else if (tag.zone === 'wave' && tag.h) this._dragBegin(tag.h, e.x);
        else if (tag.zone === 'wave' && tag.t >= 0 && L.edit) { this.drag = { kind: 'toKey', t: tag.t, x: e.x, y: e.y }; this.cb.redraw(); }
        else if (tag.zone === 'strip' || !L.edit) this.drag = { kind: Math.abs(e.dy) > Math.abs(e.dx) && L.edit ? 'zoom' : 'pan', a: this.d.view.a, b: this.d.view.b, x0: e.x, y0: e.y, at: this.pOf(e.x, e.w) };
        break;
      case 'drag': {
        const dr = this.drag; if (!dr) break;
        if (dr.kind === 'handle') this._dragTo(e.x);
        else if (dr.kind === 'toKey') { dr.x = e.x; dr.y = e.y; dr.cx = e.cx; dr.cy = e.cy; this.cb.redraw(); }
        else if (dr.kind === 'pan') { const d = e.dx / e.w * (dr.b - dr.a); this.cb.setView(dr.a - d, dr.b - d); }
        else if (dr.kind === 'zoom') { const f = Math.pow(2, e.dy / 80);   /* down = closer */ const span = (dr.b - dr.a) / f; const at = dr.at, fa = (at - dr.a) / (dr.b - dr.a); this.cb.setView(at - fa * span, at - fa * span + span); }
        break; }
      case 'pinch': if (!this.drag || this.drag.kind !== 'handle') this.cb.zoomView(1 / Math.max(0.2, Math.min(5, e.scale)), this.pOf(e.cx, e.w)); break;
      case 'dragend': {
        const dr = this.drag; this.drag = null;
        if (!dr) break;
        if (dr.kind === 'handle') { if (e.cancelled) { this.cb.redraw(); break; } this._dragCommit(dr); }
        else if (dr.kind === 'toKey') { const key = e.cancelled ? -1 : this.cb.dropTarget ? this.cb.dropTarget(e.cx, e.cy) : -1; if (key >= 0) this.dropOnKey({ t: dr.t }, key, e.cx, e.cy); else this.cb.redraw(); }
        break; }
      case 'tap':
        if (tag.zone === 'handles' && tag.h) { this.select(tag.h.t); this.auditionEntry(tag.h.t); }
        else if (tag.zone === 'wave' && tag.h) { this.select(tag.h.t); this.auditionEntry(tag.h.t); }
        else if (tag.zone === 'wave' && tag.t >= 0) { if (isMarker(c.table, tag.t) && L.edit) this.splitAt(tag.t, tag.p); else this.select(tag.t); }
        break;
      case 'dbltap':
        if ((tag.zone === 'handles' || tag.zone === 'wave') && tag.h && isMarker(c.table, tag.h.t)) this.merge(tag.h.t);
        else if (tag.zone === 'strip') this.cb.fitEntry(this.selT >= 0 ? this.selT : (c.st && c.st.keySlice ? c.st.keySlice[0] : 0));
        break;
      case 'release': if (this.looping) this._loopStop(); if (this.holdTimer) { this.clearTimer(this.holdTimer); this.holdTimer = null; } if (!this.g.active) this.snapOff = false; break;
      case 'cancel': break;
    }
  }
  /* ── selection ── */
  select(t) { const c = this.d.ctx(); if (!c.table || t < 0 || t >= c.table.entries.length) t = -1; if (t === this.selT) return; this.selT = t; if (this.cb.select) this.cb.select(t); this.cb.redraw(); }
  /* ── the drag of a handle: local at 60 fps, snapped and clamped, auditioned ≤ 10/s, committed on release ── */
  _dragBegin(h, x) {
    const c = this.d.ctx(), e = c.table.entries[h.t];
    this.select(h.t);
    this.drag = { kind: 'handle', h, pos: h.pos, pos0: h.pos, x, x0: x, entry: Object.assign({}, e), by: null };
    this.cb.redraw();
  }
  _snapFor(h, p) {
    const c = this.d.ctx(), grid = h.kind === 'grid';
    return snap(p, { hits: c.hits, sampleAt: c.sampleAt, zero: grid ? this.zero.grid : this.zero.markers, off: this.snapOff });
  }
  _dragTo(x) {
    const dr = this.drag, c = this.d.ctx(); if (!dr || dr.kind !== 'handle') return;
    dr.x = x; const w = this.size.w, raw = dr.pos0 + (x - dr.x0) / w * (this.d.view.b - this.d.view.a);
    const s = this._snapFor(dr.h, raw); let pos = s.pos; dr.by = s.by;
    if (dr.h.kind === 'grid') pos = clampBoundary(c.table, dr.h.t, pos);
    else if (dr.h.kind === 'start') pos = clampStart(dr.entry, pos, c.st.freePos).start;
    else pos = dr.entry.start + clampEnd(dr.entry, pos, c.st.freePos).len;
    if (pos == null) return;
    if (pos !== dr.pos) { dr.pos = pos; this._auditionDrag(dr); }
    this.cb.redraw();
  }
  _auditionDrag(dr) {
    if (dr.h.kind === 'end') this.audition(dr.pos, K.previewMs, HL.EDIT.fromEnd);
    else this.audition(dr.pos, K.previewMs, 0);
  }
  _dragCommit(dr) {
    const c = this.d.ctx(), tb = c.table, e = dr.entry, h = dr.h, d = dr.pos - dr.pos0;
    if (!tb || d === 0) { this.cb.redraw(); return; }
    const gen = c.st ? c.st.tableGen : 0;
    if (h.kind === 'grid') {
      const len = endOf(e) - dr.pos; this.auditionNow(dr.pos, Math.max(1, len / MS), 0);
      this.local = { t: h.t, start: dr.pos, len, prevLen: dr.pos - tb.entries[h.t - 1].start, gen };
      this.commit('boundary', { i: h.t, d }, g => HL.editBoundary(this.d.link(), h.t, dr.pos, g), gen, () => isGrid(this.d.ctx().table, h.t));
    } else if (h.kind === 'start') {
      const r = clampStart(e, dr.pos, c.st.freePos); this.auditionNow(r.start, Math.max(1, r.len / MS), 0);
      this.local = { t: h.t, start: r.start, len: r.len, gen };
      this.commit('start', { t: h.t, d }, g => HL.editEntry(this.d.link(), h.t, r.start, r.len, g), gen, () => isMarker(this.d.ctx().table, h.t) && isLive(this.d.ctx().table.entries[h.t]));
    } else {
      const r = clampEnd(e, dr.pos, c.st.freePos); this.auditionNow(r.start, Math.max(1, r.len / MS), 0);
      this.local = { t: h.t, start: r.start, len: r.len, gen };
      this.commit('end', { t: h.t, d }, g => HL.editEntry(this.d.link(), h.t, r.start, r.len, g), gen, () => isMarker(this.d.ctx().table, h.t) && isLive(this.d.ctx().table.entries[h.t]));
    }
  }
  /* the typed field under the handle: start or end of the selection, in the field's unit */
  typed(field, str) {
    const c = this.d.ctx(), tb = c.table, t = this.selT; if (!tb || t < 0) return false;
    const p = parsePos(str, this.unit, this.posCtx()); if (p == null) { this.cb.status('unreadable: ' + str, 'warn'); return false; }
    const e = tb.entries[t], gen = c.st ? c.st.tableGen : 0;
    if (isGrid(tb, t)) {
      const i = field === 'end' ? t + 1 : t; const pos = clampBoundary(tb, i, p); if (pos == null) { this.cb.status('that boundary cannot move', 'warn'); return false; }
      const d = pos - tb.entries[i].start; if (!d) return false;
      this.commit('boundary', { i, d }, g => HL.editBoundary(this.d.link(), i, pos, g), gen); return true;
    }
    const r = field === 'end' ? clampEnd(e, p, c.st.freePos) : clampStart(e, p, c.st.freePos);
    const d = field === 'end' ? endOf(r) - endOf(e) : r.start - e.start; if (!d) return false;
    this.commit(field, { t, d }, g => HL.editEntry(this.d.link(), t, r.start, r.len, g), gen); return true;
  }
  posCtx() { const s = this.d.ctx().st || {}; return { nativeBpm: s.nativeBpm, gridBars: s.gridBars, bufLen: s.bufLen, nSlices: s.nSlices }; }
  /* ── auditions: the drag's ≤ 10/s window with the last position always sent; one-shots go straight out ── */
  audition(start, lenMs, flags) {
    const item = { start, lenMs, flags }, now = this.now(), go = this.gate.offer(item, now);
    if (go) this._send(go); else if (!this.gateTimer) this.gateTimer = this.setTimer(() => { this.gateTimer = null; const d = this.gate.due(this.now()); if (d) this._send(d); }, this.gate.minMs);
  }
  auditionNow(start, lenMs, flags) { this.gate.clear(); this.gate.last = this.now(); this._send({ start, lenMs, flags }); }
  _send(a) { const l = this.d.link(); if (!l) return; this.stats.auditions++; HL.audition(l, a.start, Math.min(HL.EDIT.maxAuditionMs, a.lenMs), a.flags).catch(() => {}); }
  auditionEntry(t) { const tb = this.d.ctx().table; if (!tb || t < 0) return; const e = tb.entries[t]; if (isLive(e)) this.auditionNow(e.start, Math.max(1, e.len / MS), 0); }
  _loopStart(h) { this.looping = true; this.stats.loops = (this.stats.loops || 0) + 1; this.auditionNow(h.pos, K.previewMs, HL.EDIT.fromEnd | HL.EDIT.loop); this.cb.status('looping the tail — lift to stop', ''); }
  _loopStop() { this.looping = false; const l = this.d.link(); if (l) { this.stats.releases++; HL.release(l).catch(() => {}); } this.cb.status('', ''); }
  /* ── the commit pipeline: one edit in flight on the link; the reply frees it, then a refetch (a poll) confirms the
   * table; on BAD_STATE a refetch first, then the reason (a take, a set op, or — the gen moved — stale: retried once
   * with the fresh gen when the target still stands). `queue` chains behind a pending edit instead of refusing. ── */
  commit(op, args, fn, gen = 0, stillValid = null, queue = false) {
    if (queue && this.busy && this.pending) { const p = this.pending.then(() => this._commit(op, args, fn, gen, stillValid)); this.pending = p; return p; }
    const p = this._commit(op, args, fn, gen, stillValid); this.pending = p; return p;
  }
  async _commit(op, args, fn, gen, stillValid) {
    const l = this.d.link(); if (!l) return null;
    if (this.busy) { this.cb.status('one edit at a time — ' + this.busy + ' is still going', 'warn'); return null; }
    const label = histLabel(op, args); this.busy = label; this.busyOp = op; this.cb.status(label + '…', '');
    try {
      let r = await fn(gen), retried = false, why = '';
      if (!r.ok && r.status === 3) {
        await this.cb.refetch(); why = this.whyBadState(gen);
        if (why === 'stale' && (!stillValid || stillValid())) {
          this.cb.status('stale: the table changed under you — refetched, retrying', 'warn');
          this.stats.retried++; retried = true; r = await fn(this.gen());
          if (!r.ok && r.status === 3) why = this.whyBadState(this.gen());
        }
      }
      this.busy = null;   /* the link is free: what follows is a poll */
      if (r.ok) {
        this.stats.commits++;
        /* v0.13: a tap within 10 ms of a live marker selects it — nothing changed, nothing to label (the gen is the same) */
        if (op === 'tap' && r.reused) { this.select(r.newIdx); this.cb.status(`tap: marker ${r.newIdx} is already there — selected it`, 'ok'); this.cb.redraw(); return r; }
        const full = op === 'split' || op === 'copy' || op === 'tap' ? histLabel(op, Object.assign({}, args, { newIdx: r.newIdx })) : op === 'onsets' || op === 'mkeys' || op === 'mclear' || op === 'auto' ? histLabel(op, Object.assign({}, args, { count: r.count })) : label;
        if ((op === 'mkeys' || op === 'mclear') && !r.count) { this.cb.status(op === 'mkeys' ? 'no markers to put on keys' : 'no markers to clear', ''); this.cb.redraw(); return r; }
        if (op === 'undo') this.history.undo(r.tableGen); else if (op === 'redo') this.history.redo(r.tableGen); else if (op === 'auto' && args.join && this.history.joinTop(r.tableGen, full, /^autoslice /)) { /* one line per drag */ }
        else if (op === 'keyp' && args.join && this.history.joinTop(r.tableGen, full, new RegExp('^' + kn(args.key) + ' settings: '))) { /* KEYP: one line per drag */ } else this.history.push(r.tableGen, full);
        this.cb.status(full + (retried ? ' (after a refetch)' : ''), 'ok');
        await this.cb.refetch(); this.local = null; this.cb.redraw();
      } else {
        this.stats.refused++; this.local = null;
        if (r.status === 3 && why === 'stale') why = 'stale: the table changed under you — refetched; the target is gone, try again';
        else if (r.status !== 3) why = r.status === 2 ? (op === 'undo' || op === 'redo' ? 'refused (BAD_ARGS): nothing to ' + op + ' — the journal is empty, or this firmware has none' : 'refused (BAD_ARGS): the module rejected the span, index or key') : HL.statusName(r.status);
        this.lastErr = why; this.cb.status(label + ' — ' + why, 'warn'); this.cb.redraw();
      }
      return r;
    } catch (e) { this.local = null; this.lastErr = e.message; this.cb.status(label + ' failed: ' + e.message, 'warn'); this.cb.redraw(); return null; }
    finally { this.busy = null; }
  }
  whyBadState(gen) {
    const s = this.d.ctx().st || {};
    if (this.busyOp === 'tap' && s.srcValid === false) return 'nothing is sounding — press ▶ (or hold a pad): a tap needs the playhead';
    if (s.recState) return 'a take is in flight (' + (HL.REC[s.recState] || s.recState) + ') — edits wait for it';
    if (s.setState) return 'a set is ' + (HL.SET_STATE[s.setState] || 'busy') + ' — edits wait for it';
    if (gen && s.tableGen != null && s.tableGen !== gen) return 'stale';
    return 'the module refused (BAD_STATE): no material, the table full, or a swap pending';
  }
  gen() { const s = this.d.ctx().st; return s && s.tableGen ? s.tableGen : 0; }
  /* ── the ops behind the gestures and the panel ── */
  splitAt(t, p) {
    const c = this.d.ctx(), tb = c.table, e = tb.entries[t];
    const s = snap(p, { hits: c.hits, sampleAt: c.sampleAt, zero: this.zero.markers, off: this.snapOff }).pos;
    if (s - e.start < K.recMin || endOf(e) - s < K.recMin) { this.select(t); this.cb.status('too close to the edge to split (10 ms minimum)', 'warn'); return null; }
    this.select(t); return this.commit('split', { t }, g => HL.editSplit(this.d.link(), t, s, g), this.gen(), () => isMarker(this.d.ctx().table, t) && isLive(this.d.ctx().table.entries[t]));
  }
  merge(t) {
    const tb = this.d.ctx().table, m = mergeTarget(tb, t);
    if (!m || !m.adjacent) { this.cb.status(m ? `marker ${m.u} does not end where ${t} starts — merge joins adjacent markers` : 'nothing before this marker to merge', 'warn'); return null; }
    this.select(t); return this.commit('merge', { t, u: m.u }, g => HL.editMerge(this.d.link(), t, g), this.gen(), () => { const m2 = mergeTarget(this.d.ctx().table, t); return !!(m2 && m2.adjacent); });
  }
  tombstone(t) { if (!isMarker(this.d.ctx().table, t)) return null; if (this.selT === t) this.select(-1); return this.commit('delete', { t }, g => HL.editTombstone(this.d.link(), t, g), this.gen(), () => isMarker(this.d.ctx().table, t) && isLive(this.d.ctx().table.entries[t])); }
  copyOut(t, key = -1) { const tb = this.d.ctx().table; if (!tb) return null; const e = tb.entries[t]; if (!isLive(e)) return null; return this.commit('copy', { from: t }, g => HL.editAdd(this.d.link(), key, e.start, e.len, HL.EDIT.orange, g), this.gen()); }
  promote(t) { const tb = this.d.ctx().table; if (!isMarker(tb, t)) return null; const i = nearestBoundary(tb, tb.entries[t].start); if (i < 0) return null; const pos = clampBoundary(tb, i, tb.entries[t].start); if (pos !== tb.entries[t].start) { this.cb.status(`boundary ${i} cannot reach marker ${t}'s start (64 samples per slice)`, 'warn'); return null; } return this.commit('promote', { t, i }, g => HL.editPromote(this.d.link(), t, i, g), this.gen()); }
  /* the panel's ops queue behind a pending reply (a button, not a finger on a position); gesture ops refuse instead */
  nudge(ms, which) { if (!ms) return null; return this.commit('nudge', { ms, which }, g => HL.editNudgeAll(this.d.link(), ms, which, g), this.gen(), null, true); }
  fromOnsets(thresh, minGapMs = K.minGapMs) { return this.commit('onsets', { thresh }, g => HL.editFromOnsets(this.d.link(), thresh, minGapMs, g), this.gen(), null, true); }
  /* the Chop pad: taps queue behind a pending edit rather than being refused — the module timestamps each at receipt */
  tapMark() { const c = this.d.ctx(); const allow = c.tapAllowMs != null ? c.tapAllowMs : K.tapAllowDefault; return this.commit('tap', {}, () => HL.tapMark(this.d.link(), K.tapSnapMs, allow, 0), 0, null, true); }
  /* KEYS (v0.13): the marker ops — each one undo step on the module, queued like the panel's other buttons */
  keysFromMarkers() { return this.commit('mkeys', {}, g => HL.keysFromMarkers(this.d.link(), g), this.gen(), null, true); }
  clearMarkers() { if (this.selT >= 0 && isMarker(this.d.ctx().table, this.selT)) this.select(-1); return this.commit('mclear', {}, g => HL.clearMarkers(this.d.link(), g), this.gen(), null, true); }
  /* join: this send continues the last one's drag — the module folds it into that AUTOSLICE's undo step, and the history
   * relabels its line instead of adding one */
  autoslice(k, reroll = false, join = false) { return this.commit('auto', { k, reroll, join }, g => HL.autoslice(this.d.link(), k, reroll, g, join), this.gen(), null, true); }
  /* KEYP: one key's gain / tune / plays — join: this send continues the last one's drag (one undo step, one history line) */
  keyParams(key, p, join = false) { return this.commit('keyp', { key, p, join }, g => HL.keyParams(this.d.link(), key, p, join, g), this.gen(), null, true); }
  setKey(key, t) { return this.commit('key', { key, t }, g => HL.editSetKey(this.d.link(), key, t, g), this.gen(), null, true); }
  swapKeys(a, b) { return this.commit('swap', { a, b }, g => HL.editSwapKeys(this.d.link(), a, b, g), this.gen(), null, true); }
  undo() { return this.commit('undo', {}, () => HL.editUndo(this.d.link()), 0, null, true); }
  redo() { return this.commit('redo', {}, () => HL.editRedo(this.d.link()), 0, null, true); }
  /* every key back to its span slice, one SET_KEY at a time (only the ones that moved) */
  async keysFollowGrid() {
    let n = 0;
    for (let k = 0; k < K.keys; k++) {
      const c = this.d.ctx(); if (!c.table || !c.st || !c.st.keySlice || !this.d.link()) break;   /* re-read each time: a disconnect or new material ends it */
      const want = spanSlice(k, c.table.nSlices); if (c.st.keySlice[k] === want) continue;
      const r = await this.commit('key', { key: k, t: want }, g => HL.editSetKey(this.d.link(), k, want, g), this.gen(), null, true); if (r && r.ok) n++; else break;
    }
    if (n) this.history.push(this.history.gen, histLabel('follow'));
    return n;
  }
  /* the plan for a drop, then either straight to the op or the page's swap / replace / cancel prompt */
  async dropOnKey(src, dst, cx, cy) {
    const c = this.d.ctx(), plan = dropPlan({ table: c.table, keySlice: c.st && c.st.keySlice, nSlices: c.table && c.table.nSlices }, src, dst);
    this.cb.redraw(); if (!plan) return null;
    let id = plan.choices[0].id;
    if (plan.kind === 'choose') { id = this.cb.choose ? await this.cb.choose(plan, cx, cy) : plan.def; if (!id || id === 'cancel') { this.cb.status('cancelled', ''); return null; } }
    const ch = plan.choices.find(x => x.id === id); if (!ch) return null;
    let r = null; for (const [op, a, b] of ch.ops) r = op === 'swapKeys' ? await this.swapKeys(a, b) : await this.setKey(a, b);
    return r;
  }
  /* the long-press menu for an entry: what the page shows, the ops it runs */
  menuItems(t) {
    const c = this.d.ctx(), tb = c.table; if (!tb || t < 0) return [];
    const items = [{ id: 'audition', label: 'audition', run: () => this.auditionEntry(t) }];
    if (isMarker(tb, t)) {
      const m = mergeTarget(tb, t);
      if (m && m.adjacent) items.push({ id: 'merge', label: `merge with ${m.u}`, run: () => this.merge(t) });
      items.push({ id: 'promote', label: `promote → boundary ${nearestBoundary(tb, tb.entries[t].start)}`, run: () => this.promote(t) });
      items.push({ id: 'copy', label: 'copy as a marker', run: () => this.copyOut(t) });
      items.push({ id: 'delete', label: 'delete (tombstone)', run: () => this.tombstone(t), danger: true });
    } else {
      items.push({ id: 'copy', label: 'copy out as a marker', run: () => this.copyOut(t) });
    }
    return items;
  }
  menuFor(t, cx, cy) { this.select(t); const items = this.menuItems(t); if (items.length && this.cb.menu) this.cb.menu(items, cx, cy, t); }
  /* the state tail: history from tableGen / undoDepth / redoDepth */
  noteState(st) { if (!st) return; this.history.note(st.tableGen, st.undoDepth || 0, st.redoDepth || 0); }
  /* the strip's overview needs nothing of the editor; the field under the handle reads these */
  fieldValues() {
    const c = this.d.ctx(), tb = c.table, t = this.selT; if (!tb || t < 0 || t >= tb.entries.length) return null;
    const e = tb.entries[t], dr = this.drag && this.drag.kind === 'handle' && this.drag.h.t === t ? this.drag : null;
    let start = e.start, end = endOf(e);
    if (dr) { if (dr.h.kind === 'end') end = dr.pos; else start = dr.pos; }
    const cx = this.posCtx();
    return { t, start, end, startS: fmtPos(start, this.unit, cx), endS: fmtPos(end, this.unit, cx), lenS: fmtMs(end - start), by: dr ? dr.by : null, grid: isGrid(tb, t) };
  }
}

const api = { K, SR, MS, spanSlice, isTomb, isLive, isMarker, isGrid, isOrange, isRecording, endOf, nearestOnset, zeroCross, snap,
  clampBoundary, clampStart, clampEnd, handleList, hitHandle, entryAt, mergeTarget, nearestBoundary, keysOn,
  stepLen, fmtBbs, fmtMs, fmtDelta, fmtPos, parsePos, histLabel, History, dropPlan, onsetBars, onsetAccept, RateGate, Gesture, Editor };
if (typeof module === 'object' && module.exports) module.exports = api; else root.ED = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
