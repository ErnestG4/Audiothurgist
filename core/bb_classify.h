#pragma once
// Per-step class analysis over the engine's grid (mirrors its bar rule).
// Pure: no hardware, no allocation; runs in the control thread at load /
// reclassify, and on the host in tests/classify_check.cpp.
//
// Classifier modes (Options → Classifier):
//  0 Hockman 3-band  — kick <325 Hz, snare 325–4000 Hz, hat >4000 Hz
//  1 Legacy 120/900  — the original split: snares mostly land in "hat", so
//                      it behaves as hi/lo — musically its own lens (Amen!)
//  2 Two-band lo/hi  — kick <325 Hz vs everything else, no snare class
//  3 Density         — keyframes (significant local extrema) per sample of
//                      each step, via capicola's Analyzer (Matthew Nielsen,
//                      THIRD_PARTY.md): a free bandwidth estimate — kicks
//                      sparse, hats dense, snares between. No band filters.
//  4 Adaptive        — CLASSIFY package: each step judged at its ONSET (the
//                      first 40 ms after the step's hit, bleed from the
//                      previous hit subtracted), by six log-spaced bands,
//                      centroid, zero crossings, crest, decay and tail
//                      brightness; standardised over the loop's sounding
//                      steps and clustered (k-means, k = 3), the clusters
//                      named by their centroids. Adapts to the break.
//
// Classes: 0 kick · 1 snare · 2 hat · 3 silent.
#include <cmath>
#include <cstdint>
#include "bb_engine.h"
#include "bb_yield.h"
#include "Analyzer.h"   // capicola (AGPL) — vendor/capicola/lib
#include "bb_classify_anchors.h"   // CLASSIFY: kAdaptMu / kAdaptSd / kAdaptAnchor (fitted, tests/neural/adapt_fit.py)

namespace bb {

// Extrema deadband, × loop peak. This is the lens's focus: a hat ripple
// under a loud kick only counts if it swings more than ε, so a large ε reads
// the LOUD layer's bandwidth and a small ε reads the busiest layer present
// (which on a ride-carpet break is "hat" everywhere). Runtime-tunable so the
// host harness (tests/classify_check.cpp) can sweep it on real breaks.
inline float gDensityEps = 0.2f;      // chosen by sweep on the Amen, Apache,
                                      // Funky Drummer, Hot Pants (see tests/):
                                      // 0.2 / 0.35 / 0.5 agree with Hockman
                                      // 58 / 58 / 55 % on average; 0.2 is
                                      // best on the Amen (60 %)
// Rank-based split of the sounding steps: the lens is a brightness RANKING
// (a kick under an equally loud mid layer reads as that layer — Apache), so
// quantiles give every break a usable gate spread. Proportions follow the
// Hockman lens's typical shares on the canon breaks.
constexpr float kDensityKickQ = 0.30f; // sparsest 30 % → kick
constexpr float kDensityHatQ  = 0.55f; // densest 45 % → hat; between → snare

// Grid rule shared with Engine::SetMaterial: bars = power of 2 with a bar
// of 1.6–3.2 s, 16 steps per bar, capped at kMaxSlices.
inline int GridSlices(int len, float sr)
{
    float dur  = (float)len / sr;
    int   bars = 1;
    while(dur / (float)bars >= 3.2f && bars < 16) bars <<= 1;
    int nsl = bars * 16;
    while(nsl > Engine::kMaxSlices) nsl /= 2;
    return nsl < 1 ? 1 : nsl;
}

/* GATES: band-presence rule for Options → Gate Lanes = Multi-label.
 * Per step, three SECOND-order bands at the Hockman edges (low = LP²(325),
 * high = HP²(4000), mid = LP²(4000) of HP²(325)) — the classifier's own
 * first-order split is byte-identical to before but leaks a 100 Hz kick
 * into the mid band at only −10 dB (6 dB/oct); two poles make that −21 dB
 * and put a hat's noise under 325 Hz at −20 dB. A band is PRESENT when
 *   e_c ≥ gBandTheta × mean_c   (the class is loud for THIS break: the loop
 *                                mean is duty × average hit, so a rare
 *                                class — snares on 2 and 4 — is judged more
 *                                leniently than a hat carpet, which is what
 *                                lets a ghost snare mark the snare lane)
 *   e_c ≥ kBandFloor × e_max    (the class is a real part of THIS hit, not
 *                                crossover leakage: −20 dB under the step's
 *                                loudest band; measured on the host suite's
 *                                drums a 60 Hz kick reaches the mid band at
 *                                −27 dB and the high band at −66 dB, while
 *                                a hat riding that kick sits at −16 dB)
 * The loudest band always counts, so a sounding step marks at least one
 * lane; silent steps mark nothing. Lens-independent: the same mask under
 * every Classifier — the lens still labels the ring and drives swaps.
 * Known cross-talk (Wu et al. 2018): a snare's noise is bright, so most
 * snare steps also mark the hat lane, and a kick with a strong beater click
 * can mark the snare lane — the rule reads the drum, not the label. Both
 * numbers are first guesses for ears; gBandTheta is sweepable from the host. */
inline float     gBandTheta = 0.5f;    // × band's loop mean
constexpr float  kBandFloor = 0.01f;   // × step's loudest band (−20 dB)

/* CLASSIFY: the Adaptive lens (mode 4).
 * The three band lenses judge the WHOLE step: a snare's ring, a kick's tail
 * and the hat carpet all fold into the next step's decision, and the edges
 * are absolute. This lens judges each step where the drum is — from the
 * step's hit (the engine's onset table; the step start when the step has no
 * hit) — with the gAdaptPreMs just before it subtracted band by band so
 * what the previous hit left ringing does not vote. Two windows, because a
 * snare's attack is broadband noise like a hat's and its body comes later:
 *   early  the first gAdaptEarlyMs after the hit
 *   full   from the hit to the next hit, at most gAdaptFullMs
 * Per sounding step, kAdaptNF features (gAdaptFeat, readable by the host):
 *   0–5    log share of six log-spaced bands in the early window (< 100,
 *          100–250, 250–600, 600–1.5k, 1.5k–4k, > 4k Hz; differences of
 *          cascaded one-pole pairs, 12 dB/oct each side), bleed subtracted
 *   6      band centroid of those shares (0..5)
 *   7–12   the same six shares over the full window
 *   13     its centroid
 *   14     zero crossings of the > 100 Hz signal, early window (Gouyon,
 *          Pachet & Delerue 2000: the cheapest kick/snare feature)
 *   15     log peak / RMS, early window (a hat tick is peaky)
 *   16     decay: log energy of the rest of the full window over the early
 *          window's (kicks and snares ring, closed hats stop)
 *   17     tail brightness: late centroid − early centroid (a snare's tail
 *          is noise, a kick's stays low)
 * The features are scaled by corpus statistics (kAdaptMu / kAdaptSd, fitted
 * once on a 200-break training sample with neural hit labels, held out of
 * every measurement — docs/packages/classify.md) and weighted (gAdaptW);
 * in that space three ANCHORS (kAdaptAnchor: the corpus's mean kick, snare
 * and hat) seed a k-means that runs gAdaptIters rounds over the steps that
 * open with a hit, so the centroids move to where THIS break's drums are
 * (a tuned 80s kick with a click, a brushed snare, a ride instead of a hat)
 * — SHRUNK: each round a centroid goes gAdaptShrink of the way from its
 * anchor to its members' mean, never further, so a cluster cannot wander
 * into another class's territory (unshrunk k-means measured worse than no
 * adaptation at all). Each cluster is named by the anchor nearest its
 * centroid, ties broken by seed. A cluster that collects fewer than
 * 5 % of the hits (two at least) is not a class this break has — it dies
 * and its members go to the nearest live centroid (a break with no hats
 * never gets its bright snares called hats to fill the third cluster). The
 * decay-only steps are then labelled by nearest live centroid too. Under
 * six hit steps every sounding step votes; under six sounding steps the
 * Hockman labels stand. Silence and salience are the common rule (above).
 * Pure: static scratch, no allocation, Yield() every four steps; a 64-step
 * loop costs ~4 M multiply-adds (≤ 280 ms of audio per step through ten
 * one-poles), the k-means is negligible. Per-loop standardisation with
 * tercile seeds (no anchors) was built first and measured WORSE than
 * Hockman on every truth set (the doc has the table): a loop with 30 hits
 * cannot rediscover what a kick is; the anchors carry that, the shrunk
 * k-means adapts it. */
inline float gAdaptEarlyMs = 40.f;   // early window
inline float gAdaptFullMs  = 250.f;  // full window cap
inline float gAdaptPreMs   = 10.f;   // bleed window, subtracted
inline float gAdaptBleed   = 1.0f;   // × pre-window energy taken off each band
inline int   gAdaptIters   = 3;      // k-means rounds from the anchors (0 = nearest anchor)
inline float gAdaptShrink  = 0.5f;   // how far a centroid may follow its members (0 = never moves)
inline float gAdaptRise    = 3.f;    // hitless step: energy ratio between 128-sample blocks that counts as a hit (+4.8 dB)
constexpr int kAdaptNF     = 18;
inline float gAdaptW[kAdaptNF] = {kAdaptWFit[0], kAdaptWFit[1], kAdaptWFit[2], kAdaptWFit[3], kAdaptWFit[4], kAdaptWFit[5],
                                  kAdaptWFit[6], kAdaptWFit[7], kAdaptWFit[8], kAdaptWFit[9], kAdaptWFit[10], kAdaptWFit[11],
                                  kAdaptWFit[12], kAdaptWFit[13], kAdaptWFit[14], kAdaptWFit[15], kAdaptWFit[16], kAdaptWFit[17]};
/* corpus statistics, anchors and the fitted weights: bb_classify_anchors.h, tests/neural/adapt_fit.py */

// Scratch, at namespace scope so a host harness can read the features the
// lens saw (tests/classify_bench.cpp --dump); static, no allocation.
inline int   gAdaptAt[Engine::kMaxSlices];
inline bool  gAdaptHit[Engine::kMaxSlices];
inline float gAdaptFeat[Engine::kMaxSlices][kAdaptNF];
inline bool  gAdaptLateOk[Engine::kMaxSlices];

inline void ClassifyAdaptive(const float* x, int len, float sr, int nsl, uint8_t* cls,
                             const int* onsets, int nOnsets)
{
    const int step = len / nsl;
    int*  at     = gAdaptAt;
    auto& feat   = gAdaptFeat;
    bool* hasHit = gAdaptHit;
    bool* lateOk = gAdaptLateOk;
    /* 1. where each step's drum is: the engine's own hit rule (SetMaterial) —
     * an onset in the first quarter of the step, or within 10 ms before the
     * next step's start, opens that step; the earliest such onset wins */
    for(int s = 0; s < nsl; s++) { at[s] = s * step; hasHit[s] = false; }
    const int lead = (int)(0.010f * sr);
    for(int o = 0; o < nOnsets; o++)
    {
        const int p = onsets ? onsets[o] : -1;
        if(p < 0 || p >= len) continue;
        int s = p / step;
        if(s >= nsl) s = nsl - 1;
        int tgt = -1;
        if(p - s * step <= step / 4) tgt = s;
        else if(s + 1 < nsl && (s + 1) * step - p <= lead) tgt = s + 1;
        if(tgt < 0) continue;
        if(!hasHit[tgt] || p < at[tgt]) { at[tgt] = p; hasHit[tgt] = true; }
    }
    /* a step the detector left without a hit may still hold one (quiet hats
     * under a loud kick are what onset detectors miss): the biggest short-
     * term energy rise inside its first three quarters, if it is at least
     * +6 dB over the block before, places the window; else the step start */
    {
        const int blk = 128;
        for(int s = 0; s < nsl; s++)
        {
            if(hasHit[s] || cls[s] == 3) continue;
            const int s0 = s * step, s1 = s0 + step * 3 / 4;
            float prev = -1.f, bestR = gAdaptRise; int bestAt = -1;
            for(int b = s0; b + blk <= s1 && b + blk <= len; b += blk)
            {
                float e = 0.f;
                for(int i = 0; i < blk; i++) e += x[b + i] * x[b + i];
                if(prev >= 0.f && b > s0)
                {
                    const float r = e / (prev + 1e-12f);
                    if(r > bestR) { bestR = r; bestAt = b; }
                }
                prev = e;
            }
            if(bestAt > s0) at[s] = bestAt - 16 > s0 ? bestAt - 16 : s0;
        }
    }
    /* 2. features. One filter run per step from a 20 ms pre-roll (the 100 Hz
     * pair settles in 2 ms) through pre, early and late, accumulating band
     * energies per region. */
    const int earlyN = (int)(gAdaptEarlyMs * 0.001f * sr), fullN = (int)(gAdaptFullMs * 0.001f * sr);
    const int preN = (int)(gAdaptPreMs * 0.001f * sr), rollN = (int)(0.020f * sr);
    static const float kEdge[5] = {100.f, 250.f, 600.f, 1500.f, 4000.f};
    float a[5];
    for(int k = 0; k < 5; k++) a[k] = 1.f - expf(-2.f * 3.14159f * kEdge[k] / sr);
    for(int s = 0; s < nsl; s++)
    {
        if((s & 3) == 3) Yield();
        for(int j = 0; j < kAdaptNF; j++) feat[s][j] = 0.f;
        lateOk[s] = false;
        if(cls[s] == 3) continue;
        const int on0 = at[s];
        /* the next step's hit (or start) ends the full window: a flam or a
         * 32nd must not fold into this hit */
        const int nextAt = s + 1 < nsl ? at[s + 1] : len;
        int on1 = on0 + earlyN;
        if(nextAt > on0 + earlyN / 4 && on1 > nextAt) on1 = nextAt;
        if(on1 > len) on1 = len;
        int fl1 = on0 + fullN;
        if(fl1 > nextAt) fl1 = nextAt;
        if(fl1 > len) fl1 = len;
        if(fl1 < on1) fl1 = on1;
        const int pre0 = on0 - preN < 0 ? 0 : on0 - preN;
        const int from = pre0 - rollN < 0 ? 0 : pre0 - rollN;
        double eb[3][6] = {{0}};   /* [pre, early, late][band] */
        double sq = 0.; float pk = 0.f; int zc = 0; float prevHp = 0.f;
        float l1[5] = {0, 0, 0, 0, 0}, l2[5] = {0, 0, 0, 0, 0};
        for(int i = from; i < fl1; i++)
        {
            const float v = x[i];
            float b[6];
            for(int k = 0; k < 5; k++) { l1[k] += a[k] * (v - l1[k]); l2[k] += a[k] * (l1[k] - l2[k]); }
            b[0] = l2[0];
            for(int k = 1; k < 5; k++) b[k] = l2[k] - l2[k - 1];
            b[5] = v - l2[4];
            const int r = i < on0 ? (i < pre0 ? -1 : 0) : (i < on1 ? 1 : 2);
            if(r < 0) continue;
            for(int k = 0; k < 6; k++) eb[r][k] += (double)b[k] * b[k];
            if(r == 1)
            {
                const float hp = v - l2[0];
                sq += (double)v * v;
                const float av = fabsf(v); if(av > pk) pk = av;
                if((hp > 0.f) != (prevHp > 0.f)) zc++;
                prevHp = hp;
            }
        }
        const int nPre = on0 - pre0, nOn = on1 - on0, nLate = fl1 - on1;
        if(nOn < 8) continue;
        float e[6], tot = 0.f, f[6], ftot = 0.f, lt[6], ltot = 0.f;
        for(int k = 0; k < 6; k++)
        {
            const float preE  = nPre > 0 ? (float)(eb[0][k] / nPre) : 0.f;
            const float onE   = (float)(eb[1][k] / nOn);
            const float lateE = nLate > 0 ? (float)(eb[2][k] / nLate) : 0.f;
            const float fullE = (float)((eb[1][k] + eb[2][k]) / (nOn + nLate));
            e[k] = onE - gAdaptBleed * preE;     if(e[k] < 0.05f * onE) e[k] = 0.05f * onE;
            f[k] = fullE - gAdaptBleed * preE;   if(f[k] < 0.05f * fullE) f[k] = 0.05f * fullE;
            lt[k] = lateE;
            tot += e[k]; ftot += f[k]; ltot += lt[k];
        }
        if(tot <= 1e-20f || ftot <= 1e-20f) continue;
        float cen = 0.f, fcen = 0.f, lcen = 0.f;
        for(int k = 0; k < 6; k++)
        {
            const float p = e[k] / tot, q = f[k] / ftot;
            feat[s][k]     = log10f(p + 1e-3f);
            feat[s][7 + k] = log10f(q + 1e-3f);
            cen += (float)k * p;
            fcen += (float)k * q;
            if(ltot > 1e-20f) lcen += (float)k * lt[k] / ltot;
        }
        feat[s][6]  = cen;
        feat[s][13] = fcen;
        feat[s][14] = log10f((float)zc / (float)nOn + 1e-4f);
        const float rms = sqrtf((float)(sq / nOn)) + 1e-9f;
        feat[s][15] = log10f(pk / rms + 1e-3f);
        /* decay needs a late window of at least a quarter of the early one */
        if(nLate >= earlyN / 4 && ltot > 1e-20f)
        {
            lateOk[s]   = true;
            const float onTot = (float)((eb[1][0] + eb[1][1] + eb[1][2] + eb[1][3] + eb[1][4] + eb[1][5]) / nOn);
            feat[s][16] = log10f((ltot + 1e-12f) / (onTot + 1e-12f));
            feat[s][17] = lcen - cen;
        }
    }
    /* 3. into the anchor space: z = w × (f − μ) / σ; a step with no late
     * window sits at the corpus mean on the two decay features */
    static int idx[Engine::kMaxSlices], rest[Engine::kMaxSlices];
    int n = 0, nRest = 0, nSound = 0;
    /* the population is the steps that OPEN with a hit — a step that only
     * carries the previous drum's decay must not vote for what a class is
     * (with 3 hits per bar and 13 decays, k-means would split hit from decay,
     * not kick from snare); they are labelled afterwards by nearest centroid */
    int nHits = 0;
    for(int s = 0; s < nsl; s++) { nHits += cls[s] != 3 && hasHit[s]; nSound += cls[s] != 3; }
    if(nSound < 6) return;   /* too few hits to learn from: the Hockman labels stand */
    const bool hitsOnly = nHits >= 6;
    for(int s = 0; s < nsl; s++)
    {
        if(cls[s] == 3 || (feat[s][6] == 0.f && feat[s][14] == 0.f)) continue;
        if(!hitsOnly || hasHit[s]) idx[n++] = s; else rest[nRest++] = s;
    }
    if(n < 3) return;
    static float z[Engine::kMaxSlices][kAdaptNF];
    for(int i = 0; i < n + nRest; i++)
    {
        const int s = i < n ? idx[i] : rest[i - n];
        for(int j = 0; j < kAdaptNF; j++)
            z[s][j] = (j >= 16 && !lateOk[s]) ? 0.f : gAdaptW[j] * (feat[s][j] - kAdaptMu[j]) / kAdaptSd[j];
    }
    float cent[3][kAdaptNF];
    int   cnt[3] = {0, 0, 0};
    static uint8_t lab[Engine::kMaxSlices];
    for(int c = 0; c < 3; c++) for(int j = 0; j < kAdaptNF; j++) cent[c][j] = gAdaptW[j] * kAdaptAnchor[c][j];
    auto nearest = [&](const float* v, const bool* alive) {
        int best = -1; float bd = 1e30f;
        for(int c = 0; c < 3; c++)
        {
            if(alive && !alive[c]) continue;
            float d = 0.f;
            for(int j = 0; j < kAdaptNF; j++) { const float q = v[j] - cent[c][j]; d += q * q; }
            if(d < bd) { bd = d; best = c; }
        }
        return best;
    };
    /* 4. k-means from the anchors */
    for(int it = 0; it <= gAdaptIters; it++)
    {
        for(int c = 0; c < 3; c++) cnt[c] = 0;
        for(int i = 0; i < n; i++) { const int s = idx[i]; lab[s] = (uint8_t)nearest(z[s], nullptr); cnt[lab[s]]++; }
        if(it == gAdaptIters) break;
        /* shrunk update: a centroid moves gAdaptShrink of the way from its
         * anchor to its members' mean, so it follows this break's drums but
         * cannot wander into another class's territory */
        float nc[3][kAdaptNF] = {{0}};
        for(int i = 0; i < n; i++) { const int s = idx[i]; for(int j = 0; j < kAdaptNF; j++) nc[lab[s]][j] += z[s][j]; }
        for(int c = 0; c < 3; c++)
            if(cnt[c] >= 2)
                for(int j = 0; j < kAdaptNF; j++)
                    cent[c][j] = (1.f - gAdaptShrink) * gAdaptW[j] * kAdaptAnchor[c][j] + gAdaptShrink * nc[c][j] / (float)cnt[c];
    }
    /* 5. names: the anchor nearest each centroid, ties by seed; the guard */
    bool alive[3];
    const int minCnt = n / 20 > 2 ? n / 20 : 2;
    for(int c = 0; c < 3; c++) alive[c] = cnt[c] >= minCnt;
    if(!alive[0] && !alive[1] && !alive[2]) return;
    uint8_t name[3];
    bool    taken[3] = {false, false, false};
    for(int c = 0; c < 3; c++)
    {
        int best = -1; float bd = 1e30f;
        for(int k = 0; k < 3; k++)
        {
            float d = 0.f;
            for(int j = 0; j < kAdaptNF; j++) { const float q = cent[c][j] - gAdaptW[j] * kAdaptAnchor[k][j]; d += q * q; }
            if(!taken[k] && d < bd) { bd = d; best = k; }
        }
        name[c] = (uint8_t)best;
        taken[best] = true;
    }
    for(int i = 0; i < n; i++)
    {
        const int s = idx[i];
        if(!alive[lab[s]]) lab[s] = (uint8_t)nearest(z[s], alive);
        cls[s] = name[lab[s]];
    }
    /* decay-only steps: the nearest live centroid */
    for(int i = 0; i < nRest; i++) { const int s = rest[i]; cls[s] = name[nearest(z[s], alive)]; }
}

// Optional `dens` (kMaxSlices) receives the per-step keyframe density for
// mode 3 (0 otherwise) — the host test prints it. Optional `bandsOut`
// (kMaxSlices) receives the GATES band mask (bit c = class c present).
// Optional `onsets` (sample positions, `nOnsets` of them) place the Adaptive
// lens's windows on the hits the engine already found; other lenses ignore
// them. Without a table every step is judged from its start.
inline void ClassifySteps(const float* x, int len, float sr, uint8_t* cls, int mode,
                          float* sal = nullptr, float* densOut = nullptr, int nslOverride = 0,
                          uint8_t* bandsOut = nullptr, const int* onsets = nullptr, int nOnsets = 0)
{
    int nsl = nslOverride >= 1 ? nslOverride : GridSlices(len, sr);
    if(nsl > Engine::kMaxSlices) nsl = Engine::kMaxSlices;
    const int step = nsl > 0 ? len / nsl : 0;
    if(step < 1)   /* sub-slice material: all silent, no NaN */
    {
        for(int s = 0; s < Engine::kMaxSlices; s++) { cls[s] = 3; if(sal) sal[s] = 0.f; if(densOut) densOut[s] = 0.f; if(bandsOut) bandsOut[s] = 0; }
        return;
    }
    float lpK = 0.f, lpM = 0.f;
    const float fK = (mode == 1) ? 120.f : 325.f;
    const float fM = (mode == 1) ? 900.f : 4000.f;
    float aK = 1.f - expf(-2.f * 3.14159f * fK / sr);
    float aM = 1.f - expf(-2.f * 3.14159f * fM / sr);
    static float eL[Engine::kMaxSlices], eM[Engine::kMaxSlices], eH[Engine::kMaxSlices],
        eRaw[Engine::kMaxSlices];
    for(int s = 0; s < nsl; s++)
    {
        const float* p = x + s * step;
        int n = (s == nsl - 1) ? len - s * step : step;
        double sl = 0, sm = 0, sh = 0, sx = 0;
        for(int i = 0; i < n; i++)
        {
            float v = p[i];
            lpK += aK * (v - lpK);
            lpM += aM * (v - lpM);
            float mid = lpM - lpK, hi = v - lpM;
            sl += lpK * lpK; sm += mid * mid; sh += hi * hi; sx += v * v;
        }
        eL[s] = (float)(sl / n); eM[s] = (float)(sm / n); eH[s] = (float)(sh / n);
        eRaw[s] = (float)(sx / n);
        if((s & 3) == 3) Yield();
    }
    float mL = 1e-12f, mM = 1e-12f, mH = 1e-12f;
    for(int s = 0; s < nsl; s++) { mL += eL[s]; mM += eM[s]; mH += eH[s]; }
    if(sal) /* salience: loudness of the step relative to the loop's loudest */
    {
        float mx = 1e-12f;
        for(int s = 0; s < nsl; s++) { float e = sqrtf(eL[s] + eM[s] + eH[s]); if(e > mx) mx = e; }
        for(int s = 0; s < Engine::kMaxSlices; s++)
            sal[s] = s < nsl ? sqrtf(eL[s] + eM[s] + eH[s]) / mx : 0.f;
    }
    mL /= nsl; mM /= nsl; mH /= nsl;
    /* Silence is RELATIVE and judged on the RAW signal: a step 50 dB under
     * the loop's loudest is class 3 with zero salience. (Judging the band
     * energies let the filters' ringing — about −33 dB for a step after a
     * loud one — classify a digitally silent step as a kick; host suite.) */
    float eMax = 1e-12f;
    for(int s = 0; s < nsl; s++) if(eRaw[s] > eMax) eMax = eRaw[s];
    const float eSil = eMax * 1e-5f;
    for(int s = 0; s < nsl; s++)
    {
        float l = eL[s] / mL, m = eM[s] / mM, h = eH[s] / mH;
        if(eRaw[s] < eSil || eRaw[s] < 1e-9f) { cls[s] = 3; if(sal) sal[s] = 0.f; }
        else if(mode == 2)              cls[s] = (l >= (m > h ? m : h)) ? 0 : 2;
        else                            cls[s] = (l >= m && l >= h) ? 0 : (m >= h ? 1 : 2);
    }
    if(bandsOut) /* GATES band mask (rule above): its own second-order pass */
    {
        static float b2[3][Engine::kMaxSlices];
        const float a1 = 1.f - expf(-2.f * 3.14159f * 325.f / sr);
        const float a2 = 1.f - expf(-2.f * 3.14159f * 4000.f / sr);
        /* seven one-pole states: two cascades make each edge second order.
         * (v − LP² is NOT a second-order highpass — it is (2s+s²)/(1+s)²,
         * first order with twice the leakage; HP² must cascade v − LP(v).) */
        float l1 = 0.f, l2 = 0.f, lB = 0.f, g1 = 0.f, gB = 0.f, m1 = 0.f, m2 = 0.f;
        for(int s = 0; s < nsl; s++)
        {
            const float* p = x + s * step;
            int n = (s == nsl - 1) ? len - s * step : step;
            double sl = 0, sm = 0, sh = 0;
            for(int i = 0; i < n; i++)
            {
                const float v = p[i];
                l1 += a1 * (v - l1);   l2 += a1 * (l1 - l2);           /* low  = LP²(325)      */
                const float hA = v - l1;                                /* HP(325)              */
                lB += a1 * (hA - lB);  const float hp = hA - lB;        /* HP²(325)             */
                g1 += a2 * (v - g1);   const float gA = v - g1;         /* HP(4000)             */
                gB += a2 * (gA - gB);  const float hi = gA - gB;        /* high = HP²(4000)     */
                m1 += a2 * (hp - m1);  m2 += a2 * (m1 - m2);           /* mid  = LP²(4000)(hp) */
                sl += l2 * l2; sm += m2 * m2; sh += hi * hi;
            }
            b2[0][s] = (float)(sl / n); b2[1][s] = (float)(sm / n); b2[2][s] = (float)(sh / n);
        }
        float mB[3] = {1e-12f, 1e-12f, 1e-12f};
        for(int c = 0; c < 3; c++) { for(int s = 0; s < nsl; s++) mB[c] += b2[c][s]; mB[c] /= (float)nsl; }
        for(int s = 0; s < Engine::kMaxSlices; s++) bandsOut[s] = 0;
        for(int s = 0; s < nsl; s++)
        {
            if(eRaw[s] < eSil || eRaw[s] < 1e-9f) continue;   /* silent: no lane */
            const float e[3] = {b2[0][s], b2[1][s], b2[2][s]};
            const int   arg  = (e[0] >= e[1] && e[0] >= e[2]) ? 0 : (e[1] >= e[2] ? 1 : 2);
            uint8_t m = (uint8_t)(1u << arg);
            for(int c = 0; c < 3; c++)
                if(e[c] >= gBandTheta * mB[c] && e[c] >= kBandFloor * e[arg]) m |= (uint8_t)(1u << c);
            bandsOut[s] = m;
        }
    }
    if(densOut) for(int s = 0; s < Engine::kMaxSlices; s++) densOut[s] = 0.f;
    if(mode == 4) { ClassifyAdaptive(x, len, sr, nsl, cls, onsets, nOnsets); return; }
    if(mode != 3) return;

    /* Density lens. One continuous extrema analysis over the loop (the
     * analyzer's B-spline front end carries across step edges), a keyframe
     * count per step, and thresholds as RATIOS to the median density of the
     * sounding steps so the lens self-calibrates per break. Silent steps
     * keep class 3. Ratios are first guesses for ears. */
    static float dens[Engine::kMaxSlices];
    float pk = 1e-6f;
    for(int i = 0; i < len; i++) { float a = fabsf(x[i]); if(a > pk) pk = a; }
    static capicola::SparseLine<4> ring;   /* only the latest keyframe is read */
    static capicola::Analyzer<4>   an;
    ring.Init();
    an.Init(ring, sr);
    for(int s = 0; s < nsl; s++)
    {
        const float* p = x + s * step;
        int n = (s == nsl - 1) ? len - s * step : step;
        /* Deadband relative to THIS step's peak (floored at 5 % of the loop's),
         * so the lens reads the bandwidth of whatever dominates the step
         * regardless of its level: a quiet hat-only step still counts its own
         * ripples; a loud kick step ignores the hat riding on it. A global
         * deadband did neither (measured, tests/classify_check.cpp). */
        float ps = 0.05f * pk;
        for(int i = 0; i < n; i++) { float a = fabsf(p[i]); if(a > ps) ps = a; }
        an.SetThreshold(gDensityEps * ps);
        int c = 0;
        for(int i = 0; i < n; i++) if(an.Analyze(p[i])) c++;
        if((s & 3) == 3) Yield();
        dens[s] = (float)c / (float)n;
        if(densOut) densOut[s] = dens[s];
    }
    float srt[Engine::kMaxSlices];
    int   ns = 0;
    for(int s = 0; s < nsl; s++)
    {
        if(cls[s] == 3) continue;
        int p = ns++;
        while(p > 0 && srt[p - 1] > dens[s]) { srt[p] = srt[p - 1]; p--; }
        srt[p] = dens[s];
    }
    if(ns == 0) return;
    int iK = (int)(kDensityKickQ * (float)ns), iH = (int)(kDensityHatQ * (float)ns);
    if(iK >= ns) iK = ns - 1;
    if(iH >= ns) iH = ns - 1;
    const float qK = srt[iK], qH = srt[iH];
    for(int s = 0; s < nsl; s++)
    {
        if(cls[s] == 3) continue;
        cls[s] = dens[s] <= qK ? 0 : (dens[s] >= qH ? 2 : 1);
    }
}

} // namespace bb
