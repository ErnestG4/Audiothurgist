#!/usr/bin/env node
/* The page's colours under colour-vision deficiency — docs/colors.md.
 *
 *   node tools/colors-check.mjs [web/index.html] [--md]
 *
 * Reads the colour tokens from the page's <style> (the dark set, and the light set over it), simulates
 * deuteranopia, protanopia and tritanopia with the Machado, Oliveira & Fernandes (2009) matrices at severity 1.0
 * (applied to linear RGB), and prints CIEDE2000 between the two colours of every pair a user has to tell apart.
 * ΔE00 under ~10 is a weak pair for that vision; such a pair must have a second cue (a word, shape, dash or
 * position), named in the table. Exit code 1 if a pair without a second cue falls under 10 in any column. */
import { readFileSync } from 'node:fs';

const MACHADO = {
  normal: [[1, 0, 0], [0, 1, 0], [0, 0, 1]],
  deutan: [[0.367322, 0.860646, -0.227968], [0.280085, 0.672501, 0.047413], [-0.011820, 0.042940, 0.968881]],
  protan: [[0.152286, 1.052583, -0.204868], [0.114503, 0.786281, 0.099216], [-0.003882, -0.048116, 1.051998]],
  tritan: [[1.255528, -0.076749, -0.178779], [-0.078411, 0.930809, 0.147602], [0.004733, 0.691367, 0.303900]],
};
const lin = c => c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
const toLin = h => h.replace('#', '').match(/../g).map(x => lin(parseInt(x, 16) / 255));
const sim = (h, m) => { const v = toLin(h); return m.map(r => Math.min(1, Math.max(0, r[0] * v[0] + r[1] * v[1] + r[2] * v[2]))); };
function lab([r, g, b]) {
  const X = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047, Y = 0.2126 * r + 0.7152 * g + 0.0722 * b, Z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
  const f = t => t > 216 / 24389 ? Math.cbrt(t) : (24389 / 27 * t + 16) / 116, fx = f(X), fy = f(Y), fz = f(Z);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
}
function de2000([L1, a1, b1], [L2, a2, b2]) {
  const d = Math.PI / 180, C1 = Math.hypot(a1, b1), C2 = Math.hypot(a2, b2), Cb = (C1 + C2) / 2, G = 0.5 * (1 - Math.sqrt(Cb ** 7 / (Cb ** 7 + 25 ** 7)));
  const a1p = a1 * (1 + G), a2p = a2 * (1 + G), C1p = Math.hypot(a1p, b1), C2p = Math.hypot(a2p, b2);
  const h1p = (Math.atan2(b1, a1p) / d + 360) % 360, h2p = (Math.atan2(b2, a2p) / d + 360) % 360;
  let dh = h2p - h1p; if (C1p * C2p === 0) dh = 0; else if (dh > 180) dh -= 360; else if (dh < -180) dh += 360;
  const dL = L2 - L1, dC = C2p - C1p, dH = 2 * Math.sqrt(C1p * C2p) * Math.sin(dh * d / 2), Lb = (L1 + L2) / 2, Cbp = (C1p + C2p) / 2;
  let hb = h1p + h2p; if (C1p * C2p !== 0) { if (Math.abs(h1p - h2p) > 180) hb += h1p + h2p < 360 ? 360 : -360; hb /= 2; }
  const T = 1 - 0.17 * Math.cos((hb - 30) * d) + 0.24 * Math.cos(2 * hb * d) + 0.32 * Math.cos((3 * hb + 6) * d) - 0.20 * Math.cos((4 * hb - 63) * d);
  const Rc = 2 * Math.sqrt(Cbp ** 7 / (Cbp ** 7 + 25 ** 7)), Rt = -Math.sin(2 * 30 * Math.exp(-(((hb - 275) / 25) ** 2)) * d) * Rc;
  const Sl = 1 + 0.015 * (Lb - 50) ** 2 / Math.sqrt(20 + (Lb - 50) ** 2), Sc = 1 + 0.045 * Cbp, Sh = 1 + 0.015 * Cbp * T;
  return Math.sqrt((dL / Sl) ** 2 + (dC / Sc) ** 2 + (dH / Sh) ** 2 + Rt * (dC / Sc) * (dH / Sh));
}
const dE = (a, b, m) => de2000(lab(sim(a, MACHADO[m])), lab(sim(b, MACHADO[m])));

/* the tokens, as the self-test reads them */
const args = process.argv.slice(2), md = args.includes('--md'), file = args.find(a => !a.startsWith('--')) || new URL('../web/index.html', import.meta.url);
const css = (h => h.slice(h.indexOf('<style>'), h.indexOf('</style>')))(readFileSync(file, 'utf8'));
const block = re => { const m = css.match(re); return m ? Object.fromEntries([...m[1].matchAll(/--([\w-]+):\s*([^;}]+)/g)].map(x => [x[1], x[2].trim()])) : {}; };
const dark = block(/:root\{([^}]*)\}/), light = Object.assign({}, dark, block(/:root\[data-theme="light"\]\{([^}]*)\}/));

/* [group, a, b, the second cue (or '' if colour is all there is), themes] — canvas tokens are the same in both themes
 * (the displays stay dark), so those pairs are measured once */
const C = 'canvas';
const PAIRS = [
  ['class', 'kick', 'snare', 'name on the key cell; letter on the bar strip', C],
  ['class', 'kick', 'hat', 'name on the key cell; letter on the bar strip', C],
  ['class', 'kick', 'other', 'name on the key cell; letter on the bar strip', C],
  ['class', 'snare', 'hat', 'name on the key cell; letter on the bar strip', C],
  ['class', 'snare', 'other', 'name on the key cell; letter on the bar strip', C],
  ['class', 'hat', 'other', 'name on the key cell; letter on the bar strip', C],
  ['lane', 'cv-orange', 'cv-marker', 'untouched markers dotted, hollow handle; dotted key border', C],
  ['lane', 'cv-rec', 'cv-orange', 'a recording is labelled with its key, a marker m<n>', C],
  ['lane', 'cv-rec', 'cv-marker', 'key label vs m<n>; dotted', C],
  ['lane', 'cv-play', 'cv-rec', 'grid in the top lane, recordings in the bottom one', C],
  ['lane', 'cv-play', 'cv-orange', 'grid in the top lane, markers in the bottom one', C],
  ['lane', 'cv-play', 'cv-marker', 'top lane vs bottom lane', C],
  ['overlay', 'cv-call', 'cv-play', 'call outline dashed, playhead solid', C],
  ...['kick', 'snare', 'hat', 'other'].flatMap(k => [
    ['overlay', 'cv-call', k, 'call outline dashed over the class fill', C],
    ['overlay', 'cv-play', k, 'outline over the fill', C],
    ['overlay', 'cv-ok', k, 'a moving line over the wave', C]]),
  ['status', 'ok-ink', 'bad-ink', 'the words', 'both'],
  ['status', 'ok-ink', 'warn-ink', 'the words', 'both'],
  ['status', 'warn-ink', 'bad-ink', 'the words', 'both'],
  ['status', 'ok-ink', 'brain-ink', 'the words', 'both'],
  ['status', 'accent', 'bad-ink', 'filled button vs text', 'both'],
  /* the scope's channels as the presets and the auto inputs show them together */
  ['scope: Audio', 'sc0', 'sc1', 'L / R buttons', C],
  ['scope: + inputs', 'sc0', 'sc2', 'in dashed', C], ['scope: + inputs', 'sc0', 'sc3', 'in dashed', C],
  ['scope: + inputs', 'sc1', 'sc2', 'in dashed', C], ['scope: + inputs', 'sc1', 'sc3', 'in dashed', C],
  ['scope: + inputs', 'sc2', 'sc3', 'L / R buttons', C],
  ['scope: CV', 'sc4', 'sc5', '', C], ['scope: CV', 'sc4', 'sc6', '', C], ['scope: CV', 'sc4', 'sc7', '', C],
  ['scope: CV', 'sc5', 'sc6', '', C], ['scope: CV', 'sc5', 'sc7', '', C], ['scope: CV', 'sc6', 'sc7', '', C],
  ['scope: Gates', 'sc0', 'sc7', 'out solid, jacks dashed', C], ['scope: Gates', 'sc0', 'sc8', 'out solid, jacks dashed', C],
  ['scope: Gates', 'sc0', 'sc9', 'out solid, jacks dashed', C],
  ['scope: Gates', 'sc7', 'sc8', '', C], ['scope: Gates', 'sc7', 'sc9', '', C], ['scope: Gates', 'sc8', 'sc9', '', C],
];
const VIS = ['normal', 'deutan', 'protan', 'tritan'], WEAK = 10;
const rows = [];
let fail = 0;
for (const [g, a, b, cue, th] of PAIRS) for (const [tn, T] of th === 'both' ? [['dark', dark], ['light', light]] : [['', dark]]) {
  if (!T[a] || !T[b]) { console.error(`missing token --${!T[a] ? a : b}`); process.exit(2); }
  const v = VIS.map(m => dE(T[a], T[b], m)), min = Math.min(...v), bad = !cue && min < WEAK; if (bad) fail++;
  rows.push({ g: g + (tn ? ` (${tn})` : ''), a, b, ha: T[a], hb: T[b], v, min, cue, bad });
}
const f1 = x => x.toFixed(1);
if (md) {
  console.log('| group | pair | normal | deutan | protan | tritan | second cue |\n|---|---|---:|---:|---:|---:|---|');
  for (const r of rows) console.log(`| ${r.g} | ${r.a} \`${r.ha}\` / ${r.b} \`${r.hb}\` | ${r.v.map(x => x < WEAK ? `**${f1(x)}**` : f1(x)).join(' | ')} | ${r.cue || '—'} |`);
} else {
  for (const r of rows) console.log(`${r.g.padEnd(22)} ${(r.a + '/' + r.b).padEnd(22)} ${r.v.map(f1).map(x => x.padStart(5)).join(' ')}  ${r.min < WEAK ? (r.cue ? 'weak, cue: ' + r.cue : 'WEAK, no other cue') : ''}`);
}
console.log(`${md ? '\n' : ''}${rows.length} pairs; ${rows.filter(r => r.min < WEAK).length} under ΔE00 ${WEAK} in some vision${fail ? `, ${fail} of them with no other cue` : ', each with another cue'}`);
process.exit(fail ? 1 : 0);
