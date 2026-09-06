#pragma once
// Bar length, downbeat and tempo from the material itself.
//
// The engine's duration prior ("a bar is 1.6–3.2 s") is a tempo prior of
// 75–150 BPM: anything faster gets its bar doubled, so a jungle-tempo break
// played every "16th" as an 8th, reported half its tempo, and ran at half
// speed against a matched clock (field, 2026-09-06). v0.8.0 answered with a
// consecutive-snare-interval histogram; this is the SLICE package's
// replacement, validated on the 1,230-break corpus (docs/packages/slice.md):
//
//  1. Dixon 2001 IOI clustering: every pair of onsets 50 ms–2.5 s apart
//     votes, weighted by the salience of both onsets, into 25 ms-wide
//     clusters; clusters that are near-integer multiples of each other
//     reinforce one another (f(k) = 6−k for k ≤ 4, 1 for 5..8). The top
//     clusters are the tempo hypotheses — a 16th, an 8th, a dotted 8th, a
//     beat, a half bar: we do not know which yet.
//  2. Each hypothesis is read as k 16ths for the musical k (1,2,3,4,6,8,12,
//     16,32) to give candidate BAR lengths inside 60–220 BPM; each is
//     refined against the onsets by a least-squares 16th-grid fit (period
//     and phase) so the long IOIs pin the bar to a fraction of a percent,
//     and candidates whose grid the onsets do not sit on are dropped.
//  3. The octave is settled by a backbeat template in the spirit of
//     Hockman, Davies & Fujinaga 2012: each onset's energy RISE is split
//     into kick (< 120 Hz), snare (150 Hz–3 kHz) and hat (> 4 kHz) shares;
//     a kick share wants beats 1 and 3 ("breakbeats almost invariably begin
//     with kick drums"), a snare share wants 2 and 4, and the best rotation
//     of the 16-step profile is the DOWNBEAT. A doubled tempo puts the
//     snare on "3" or a kick on "4" with an empty "1"; a halved tempo puts
//     snares on the off-8ths; both score low. Bar-to-bar profile
//     consistency and a gentle 70–190 BPM prior break the remaining ties.
//  4. A wrong confident answer is worse than the prior: the winning bar must
//     beat the runner-up by a margin (kConfMin, tuned on the corpus) or we
//     return nothing and the caller keeps the prior.
//
// Pure, no allocation: fixed static scratch (~8 KB). Offline, control thread.
#include <cmath>
#include "bb_yield.h"
#ifdef BB_BARS_DEBUG
#include <cstdio>
#endif

namespace bb {

struct BarEstimate
{
    float bar      = 0.f; // samples per bar; 0 = no confident answer
    float downbeat = 0.f; // sample position of the first downbeat, in [0, bar)
    float conf     = 0.f; // winner-minus-runner-up template margin
    float fit      = 0.f; // salience fraction of onsets on the 16th grid
    float bpm      = 0.f; // 240·sr/bar
};

namespace bardetail {
constexpr int   kMaxO      = 256;
constexpr int   kHistBins  = 512;   // 5 ms bins → 2.56 s
constexpr float kHistBinS  = 0.005f;
constexpr float kMinIoiS   = 0.050f;
constexpr float kMaxIoiS   = 2.500f;
constexpr int   kMaxClus   = 48;
constexpr int   kTopClus   = 6;
constexpr int   kMaxCand   = 40;
constexpr float kBpmLo     = 60.f, kBpmHi = 220.f;
#ifndef BB_BARS_CONF
#define BB_BARS_CONF 0.12f           // tests/bars_corpus.sh sweeps this
#endif
constexpr float kConfMin   = BB_BARS_CONF;  // template margin winner − runner-up
constexpr float kFitMin    = 0.60f;  // on-8th-grid salience ÷ (on + off-grid)
constexpr int   kFitMinN   = 8;      // …and at least this many onsets on it
constexpr float kGridTol   = 0.15f;  // of a 16th, either side
constexpr int   kSubdiv[]  = {1, 2, 3, 4, 6, 8, 12, 16, 32};

inline float RelWeight(int k) { return k <= 4 ? (float)(6 - k) : (k <= 8 ? 1.f : 0.f); }

// All of EstimateBar's scratch (≈15 KB) in one object the shell places in
// SDRAM (review M7: it sat in DTCM as function statics). Written before read
// on every path; the shell zeroes it once at boot anyway (NOLOAD section).
struct BarScratch
{
    float low[kMaxO], mid[kMaxO], high[kMaxO], all[kMaxO], sal[kMaxO], kick[kMaxO], snare[kMaxO];
    float hist[kHistBins], sm[kHistBins];
    float cMean[kMaxClus], cW[kMaxClus], cScore[kMaxClus];
    int   top[kTopClus];
    float cand[kMaxCand], candPhase[kMaxCand], candFit[kMaxCand];
    float score[kMaxCand], rot[kMaxCand];
    int   condOnsets[256], condSub[256];   // bb_condition.h
};
inline BarScratch* gBarScratch = nullptr;
#ifndef BB_SCRATCH_EXTERNAL
inline BarScratch& ScratchRef() { static BarScratch s; return gBarScratch ? *gBarScratch : s; }
#else
inline BarScratch& ScratchRef() { return *gBarScratch; }   // firmware: the shell must set it
#endif
} // namespace bardetail

// Per-onset band features: rise of energy across the onset (40 ms after minus
// 20 ms before) in a kick band, a snare band, a hat band and full band. Rises
// rather than levels: a bass note sustaining under a hat must not make the
// hat a "kick".
inline void OnsetBandRises(const float* x, int len, float sr, const int* onsets, int n,
                           float* low, float* mid, float* high, float* all)
{
    const int   wPost = (int)(0.040f * sr), wPre = (int)(0.020f * sr);
    const float aL  = 1.f - expf(-2.f * 3.14159f * 120.f / sr);
    const float aM0 = 1.f - expf(-2.f * 3.14159f * 150.f / sr);
    const float aM1 = 1.f - expf(-2.f * 3.14159f * 3000.f / sr);
    const float aH  = 1.f - expf(-2.f * 3.14159f * 4000.f / sr);
    for(int i = 0; i < n; i++)
    {
        const int o  = onsets[i] < 0 ? 0 : onsets[i];
        int       a0 = o - wPre; if(a0 < 0) a0 = 0;
        int       b1 = o + wPost; if(b1 > len) b1 = len;
        float lp = 0.f, m0 = 0.f, m1 = 0.f, hp = 0.f;
        double sl[2] = {0, 0}, sm[2] = {0, 0}, sh[2] = {0, 0}, st[2] = {0, 0};
        int    cnt[2] = {0, 0};
        for(int k = a0; k < b1; k++)
        {
            const float v = x[k];
            lp += aL * (v - lp);
            m0 += aM0 * (v - m0);
            m1 += aM1 * (v - m1);
            hp += aH * (v - hp);
            const float mid = m1 - m0, hi = v - hp;
            const int   seg = k < o ? 0 : 1;
            sl[seg] += lp * lp; sm[seg] += mid * mid; sh[seg] += hi * hi; st[seg] += v * v; cnt[seg]++;
        }
        const double pre = cnt[0] ? 1.0 / cnt[0] : 0., post = cnt[1] ? 1.0 / cnt[1] : 0.;
        const float rl = (float)(sl[1] * post - sl[0] * pre), rm = (float)(sm[1] * post - sm[0] * pre),
                    rh = (float)(sh[1] * post - sh[0] * pre), ra = (float)(st[1] * post - st[0] * pre);
        low[i]  = rl > 0.f ? rl : 0.f;
        mid[i]  = rm > 0.f ? rm : 0.f;
        high[i] = rh > 0.f ? rh : 0.f;
        all[i]  = ra > 0.f ? ra : 0.f;
    }
}

inline BarEstimate EstimateBar(const float* x, int len, float sr, const int* onsets, int n)
{
    using namespace bardetail;
    BarEstimate none;
    if(!x || len < (int)sr || !onsets || n < 6) return none;
    const int m = n > kMaxO ? kMaxO : n;

    BarScratch& S = ScratchRef();
    float (&low)[kMaxO] = S.low; float (&mid)[kMaxO] = S.mid; float (&high)[kMaxO] = S.high; float (&all)[kMaxO] = S.all;
    float (&sal)[kMaxO] = S.sal; float (&kick)[kMaxO] = S.kick; float (&snare)[kMaxO] = S.snare;
    OnsetBandRises(x, len, sr, onsets, m, low, mid, high, all);
    float maxAll = 1e-12f;
    for(int i = 0; i < m; i++) if(all[i] > maxAll) maxAll = all[i];
    /* kick-vs-snare CONTRAST: (low − mid) share of the rise, centred on the
     * file's salience-weighted mean so a bass-heavy or thin recording still
     * reads ± (kick +, snare −, hats ≈ 0 by their small low+mid share) */
    double cMeanW = 0., cSumW = 0.;
    for(int i = 0; i < m; i++)
    {
        sal[i] = sqrtf(all[i] / maxAll);            // 0..1, loudness rank
        const float tot = low[i] + mid[i] + high[i] + 1e-12f;
        kick[i]  = low[i] / tot;
        snare[i] = mid[i] / tot;
        cMeanW += (kick[i] - snare[i]) * sal[i]; cSumW += sal[i];
    }
    const float cCentre = cSumW > 0. ? (float)(cMeanW / cSumW) : 0.f;

    /* 1. IOI clustering: salience-weighted histogram of ALL pairs, 5 ms bins,
     * smoothed over ±2 bins (a 25 ms cluster); peaks are the clusters. */
    float (&hist)[kHistBins] = S.hist; float (&sm)[kHistBins] = S.sm;
    Yield();
    for(int i = 0; i < kHistBins; i++) hist[i] = 0.f;
    const float binS = kHistBinS * sr;
    const float minI = kMinIoiS * sr, maxI = kMaxIoiS * sr;
    for(int i = 0; i < m; i++)
        for(int j = i + 1; j < m; j++)
        {
            const float d = (float)(onsets[j] - onsets[i]);
            if(d < minI) continue;
            if(d > maxI) break;
            const int b = (int)(d / binS);
            if(b < kHistBins) hist[b] += sal[i] * sal[j];
        }
    for(int i = 0; i < kHistBins; i++)
    {
        float s = 0.f;
        for(int k = -2; k <= 2; k++)
        {
            const int q = i + k;
            if(q >= 0 && q < kHistBins) s += hist[q];
        }
        sm[i] = s;
    }
    float (&cMean)[kMaxClus] = S.cMean; float (&cW)[kMaxClus] = S.cW; float (&cScore)[kMaxClus] = S.cScore;
    Yield();
    int nc = 0;
    for(int i = 2; i + 2 < kHistBins && nc < kMaxClus; i++)
    {
        if(sm[i] <= 0.f) continue;
        if(sm[i] < sm[i - 1] || sm[i] <= sm[i + 1]) continue;
        if(sm[i] < sm[i - 2] || sm[i] <= sm[i + 2]) continue;
        double acc = 0, w = 0;
        for(int k = -2; k <= 2; k++)
        {
            const int q = i + k;
            if(q < 0 || q >= kHistBins) continue;
            acc += hist[q] * ((double)q + 0.5) * binS;
            w += hist[q];
        }
        if(w <= 0.) continue;
        cMean[nc] = (float)(acc / w);
        cW[nc]    = sm[i];
        nc++;
    }
    if(nc < 1) return none;
    /* relations: near-integer multiples reinforce (Dixon 2001) */
    for(int c = 0; c < nc; c++)
    {
        float s = 5.f * cW[c];
        for(int d = 0; d < nc; d++)
        {
            if(d == c) continue;
            const float big = cMean[d] > cMean[c] ? cMean[d] : cMean[c];
            const float sml = cMean[d] > cMean[c] ? cMean[c] : cMean[d];
            const float r   = big / sml;
            const int   k   = (int)(r + 0.5f);
            if(k < 1 || k > 8) continue;
            if(fabsf(r - (float)k) > 0.04f * (float)k) continue;
            s += RelWeight(k) * cW[d];
        }
        cScore[c] = s;
    }
    int (&top)[kTopClus] = S.top;
    int nt = 0;
    for(int t = 0; t < kTopClus && t < nc; t++)
    {
        int best = -1;
        for(int c = 0; c < nc; c++)
        {
            bool used = false;
            for(int u = 0; u < nt; u++) if(top[u] == c) used = true;
            if(used) continue;
            if(best < 0 || cScore[c] > cScore[best]) best = c;
        }
        if(best < 0) break;
        top[nt++] = best;
    }

    /* 2. bar candidates: each top cluster read as k 16ths, inside the tempo
     * range, deduplicated at 3 %, refined by a least-squares 16th-grid fit */
    float (&cand)[kMaxCand] = S.cand; float (&candPhase)[kMaxCand] = S.candPhase; float (&candFit)[kMaxCand] = S.candFit;
    Yield();
    int ncand = 0;
    const float barMin = 240.f * sr / kBpmHi, barMax = 240.f * sr / kBpmLo;
    for(int t = 0; t < nt; t++)
        for(int k : kSubdiv)
        {
            const float B = cMean[top[t]] * 16.f / (float)k;
            if(B < barMin || B > barMax || B > (float)len * 1.03f) continue;
            bool dup = false;
            for(int c = 0; c < ncand; c++) if(fabsf(cand[c] - B) < 0.03f * B) dup = true;
            if(dup || ncand >= kMaxCand) continue;
            cand[ncand++] = B;
        }
    if(ncand < 1) return none;

    // Where an onset sits relative to a candidate's 8TH grid (swing never
    // touches 8ths): u8 = position in 8ths relative to the phase.
    //   0 on the 8th grid (±kGridTol of a 16th), 1 in the odd-16th zone
    //   (straight or swung off-beat: neutral), 2 off the grid (evidence
    //   against). Zone bounds: 0.35–0.85 of the 8th (ratio 1.0 … 2.3).
    auto zone = [&](float u8) -> int {
        float fr = u8 - floorf(u8);
        if(fr > 0.5f) fr -= 1.f;                       // (−0.5, 0.5]
        if(fabsf(fr) <= 0.5f * kGridTol) return 0;     // ±0.15 of a 16th
        const float v = fr < 0.f ? fr + 1.f : fr;      // [0,1)
        return (v >= 0.35f && v <= 0.85f) ? 1 : 2;
    };
    auto gridFit = [&](float& B, float& phase) -> float {
        float cell = B / 8.f;
        float bestPh = 0.f, bestS = -1.f;
        for(int p = 0; p < 24; p++)
        {
            const float ph = cell * (float)p / 24.f;
            float s = 0.f;
            for(int i = 0; i < m; i++)
                if(zone(((float)onsets[i] - ph) / cell) == 0) s += sal[i];
            if(s > bestS) { bestS = s; bestPh = ph; }
        }
        phase = bestPh;
        for(int it = 0; it < 3; it++)   // weighted LS on the near-grid hits
        {
            cell = B / 8.f;
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            int    cnt = 0;
            for(int i = 0; i < m; i++)
            {
                const float u = ((float)onsets[i] - phase) / cell;
                float fr = u - floorf(u + 0.5f);
                /* capture twice as wide as the fit window so a drifting
                 * drummer's far bars can still pull the period */
                if(fabsf(fr) > kGridTol) continue;
                const double k = floor(u + 0.5), w = sal[i];
                sw += w; sx += w * k; sy += w * onsets[i]; sxx += w * k * k; sxy += w * k * (double)onsets[i];
                cnt++;
            }
            const double den = sw * sxx - sx * sx;
            if(cnt < kFitMinN || sw < 1e-6 || fabs(den) < 1e-9) break;
            const double slope = (sw * sxy - sx * sy) / den;
            const double icpt  = (sy - slope * sx) / sw;
            if(slope <= 0.) break;
            const float nb = (float)(slope * 8.);
            if(fabsf(nb - B) > 0.05f * B) break;
            B     = nb;
            phase = (float)icpt;
        }
        cell = B / 8.f;
        phase -= cell * floorf(phase / cell);
        float on = 0.f, off = 0.f;
        int   cnt = 0;
        for(int i = 0; i < m; i++)
        {
            const int z = zone(((float)onsets[i] - phase) / cell);
            if(z == 0) { on += sal[i]; cnt++; }
            else if(z == 2) off += sal[i];
        }
        return cnt >= kFitMinN ? on / (on + off + 1e-9f) : 0.f;
    };
    for(int c = 0; c < ncand; c++)
    {
        float ph = 0.f;
        candFit[c]   = gridFit(cand[c], ph);
        candPhase[c] = ph;
    }
    /* refinement can merge candidates: keep the better-fitting of a pair */
    for(int c = 0; c < ncand; c++)
        for(int d = c + 1; d < ncand; d++)
            if(fabsf(cand[c] - cand[d]) < 0.02f * cand[c])
            {
                if(candFit[d] > candFit[c]) { cand[c] = cand[d]; candPhase[c] = candPhase[d]; candFit[c] = candFit[d]; }
                candFit[d] = -1.f;   // dropped
            }

    /* 3. backbeat template per candidate: 16-step profiles of kick-share,
     * snare-share and total salience; best rotation = downbeat */
    float (&score)[kMaxCand] = S.score; float (&rot)[kMaxCand] = S.rot;
    Yield();
    for(int c = 0; c < ncand; c++)
    {
        score[c] = -1e9f; rot[c] = 0.f;
        if(candFit[c] < kFitMin) continue;
        const float B = cand[c], step = B / 16.f, ph = candPhase[c];
        float D[16], E[16];
        for(int p = 0; p < 16; p++) D[p] = E[p] = 0.f;
        for(int i = 0; i < m; i++)
        {
            const float u8 = ((float)onsets[i] - ph) / (2.f * step);
            const int   z  = zone(u8);
            if(z == 2) continue;
            int p;
            if(z == 0) p = 2 * (int)floorf(u8 + 0.5f);          // on the 8th
            else       p = 2 * (int)floorf(u8) + 1;             // the odd 16th
            p %= 16; if(p < 0) p += 16;
            D[p] += (kick[i] - snare[i] - cCentre) * sal[i];
            E[p] += sal[i];
        }
        float eSum = 1e-9f;
        for(int p = 0; p < 16; p++) eSum += E[p];
        for(int p = 0; p < 16; p++) { D[p] /= eSum; E[p] /= eSum; }
        /* rotation = downbeat, on the 8th grid the phase was fit to (an odd
         * rotation parks every real 8th on an odd 16th: the doubled tempo's
         * favourite disguise) */
        float bestT = -1e9f; int bestR = 0;
        for(int r = 0; r < 16; r += 2)
        {
            auto d = [&](int p) { return D[(p + r) & 15]; };
            auto e = [&](int p) { return E[(p + r) & 15]; };
            float offSnare = 0.f;
            for(int p = 2; p < 16; p += 4) if(d(p) < 0.f) offSnare -= d(p);
            const float T = 2.f * (d(0) + 0.5f * d(8) - d(4) - d(12) - 0.5f * offSnare)
                            + 0.5f * e(0);
            if(T > bestT) { bestT = T; bestR = r; }
        }
#ifdef BB_BARS_DEBUG
        printf("  profile %6.1f BPM rot %2d T %.3f\n    D:", 240.f * sr / B, bestR, bestT);
        for(int p = 0; p < 16; p++) printf(" %3.0f", 100.f * D[(p + bestR) & 15]);
        printf("\n    E:");
        for(int p = 0; p < 16; p++) printf(" %3.0f", 100.f * E[(p + bestR) & 15]);
        printf("\n");
#endif
        /* bar-to-bar consistency of the total profile: a doubled tempo whose
         * "bars" alternate (kick half / snare half) scores low here */
        float cons = 0.f;
        {
            const float db = ph + (float)bestR * step;
            const int   nb = (int)(((float)len - db) / B);
            if(nb >= 2)
            {
                float prev[16]; bool have = false; int pairs = 0; float acc = 0.f;
                for(int b = 0; b < nb && b < 8; b++)
                {
                    float pr[16]; for(int p = 0; p < 16; p++) pr[p] = 0.f;
                    for(int i = 0; i < m; i++)
                    {
                        const float u = ((float)onsets[i] - db) / B - (float)b;
                        if(u < 0.f || u >= 1.f) continue;
                        int p = (int)(u * 16.f + 0.5f) & 15;
                        pr[p] += sal[i];
                    }
                    float mean = 0.f; for(int p = 0; p < 16; p++) mean += pr[p]; mean /= 16.f;
                    for(int p = 0; p < 16; p++) pr[p] -= mean;
                    if(have)
                    {
                        float dot = 0.f, na = 0.f, nb2 = 0.f;
                        for(int p = 0; p < 16; p++) { dot += pr[p] * prev[p]; na += pr[p] * pr[p]; nb2 += prev[p] * prev[p]; }
                        if(na > 0.f && nb2 > 0.f) { acc += dot / sqrtf(na * nb2); pairs++; }
                    }
                    for(int p = 0; p < 16; p++) prev[p] = pr[p];
                    have = true;
                }
                if(pairs) cons = acc / (float)pairs;
            }
        }
        const float bpm   = 240.f * sr / B;
        const float prior = (bpm >= 70.f && bpm <= 190.f) ? 0.f : -0.10f;
        /* alternation is evidence against; agreement is not evidence for (a
         * halved tempo's "bars" agree by construction) */
        score[c] = bestT + (cons < 0.f ? 0.4f * cons : 0.f) + prior;
        rot[c]   = (float)bestR;
    }

#ifdef BB_BARS_DEBUG
    for(int t = 0; t < nt; t++)
        printf("  cluster %6.1f ms  w %.2f  score %.2f\n", cMean[top[t]] / sr * 1000.f, cW[top[t]], cScore[top[t]]);
    for(int c = 0; c < ncand; c++)
        if(candFit[c] >= 0.f)
            printf("  cand %6.1f BPM  fit %.2f  score %7.3f  rot %2.0f  phase %6.1f ms%s\n", 240.f * sr / cand[c], candFit[c], score[c], rot[c], candPhase[c] / sr * 1000.f, candFit[c] < kFitMin ? "  (unfit)" : "");
#endif
    /* 4. decide, with the confidence gate */
    int best = -1;
    for(int c = 0; c < ncand; c++) if(candFit[c] >= kFitMin && (best < 0 || score[c] > score[best])) best = c;
    if(best < 0) return none;
    float second = -1e9f;
    for(int c = 0; c < ncand; c++) if(c != best && candFit[c] >= kFitMin && score[c] > second) second = score[c];
    const float margin = second > -1e8f ? score[best] - second : score[best];
    if(margin < kConfMin) return none;

    BarEstimate e;
    e.bar  = cand[best];
    e.fit  = candFit[best];
    e.conf = margin;
    e.bpm  = 240.f * sr / e.bar;
    float db = candPhase[best] + rot[best] * e.bar / 16.f;
    db -= e.bar * floorf(db / e.bar);
    e.downbeat = db;
    return e;
}

// v0.8.0 entry point, kept for callers that only want the bar length
// (tests/engine_check.cpp, the shell). 0 = keep the prior.
inline float EstimateBarSamples(const float* x, int len, float sr, const int* onsets, int n)
{
    return EstimateBar(x, len, sr, onsets, n).bar;
}

} // namespace bb
