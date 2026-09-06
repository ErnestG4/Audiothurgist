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
//
// Classes: 0 kick · 1 snare · 2 hat · 3 silent.
#include <cmath>
#include <cstdint>
#include "bb_engine.h"
#include "bb_yield.h"
#include "Analyzer.h"   // capicola (AGPL) — vendor/capicola/lib

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

// Optional `dens` (kMaxSlices) receives the per-step keyframe density for
// mode 3 (0 otherwise) — the host test prints it. Optional `bandsOut`
// (kMaxSlices) receives the GATES band mask (bit c = class c present).
inline void ClassifySteps(const float* x, int len, float sr, uint8_t* cls, int mode,
                          float* sal = nullptr, float* densOut = nullptr, int nslOverride = 0,
                          uint8_t* bandsOut = nullptr)
{
    int nsl = nslOverride >= 1 ? nslOverride : GridSlices(len, sr);
    if(nsl > Engine::kMaxSlices) nsl = Engine::kMaxSlices;
    const int step = nsl > 0 ? len / nsl : 0;
    if(step < 1)   /* sub-slice material: all silent, no NaN (review L3) */
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
