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

// Optional `dens` (kMaxSlices) receives the per-step keyframe density for
// mode 3 (0 otherwise) — the host test prints it.
inline void ClassifySteps(const float* x, int len, float sr, uint8_t* cls, int mode,
                          float* sal = nullptr, float* densOut = nullptr, int nslOverride = 0)
{
    const int nsl  = nslOverride >= 1 ? nslOverride : GridSlices(len, sr);
    const int step = len / nsl;
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
