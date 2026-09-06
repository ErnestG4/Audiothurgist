#pragma once
// Bar length from the material's own BACKBEAT.
//
// The engine's duration prior ("a bar is 1.6–3.2 s") is a tempo prior of
// 75–150 BPM: anything faster gets its bar doubled, so a jungle-tempo break
// played every "16th" as an 8th, reported half its tempo, and ran at half
// speed against a matched clock (field, 2026-09-06). A break's snares sit on
// 2 and 4 almost without exception, so the dominant interval between
// consecutive loud, mid-band (snare-like) onsets is half a bar. This is a
// first cut of the literature review's IOI-clustering recommendation; the
// slicing package refines it against the corpus. Returns the bar length in
// samples, or 0 when there is no confident answer (callers keep the prior).
#include <cmath>

namespace bb {

inline float EstimateBarSamples(const float* x, int len, float sr, const int* onsets, int n)
{
    if(!x || len < (int)sr || !onsets || n < 4) return 0.f;
    constexpr int kMaxO = 256;
    static float eLow[kMaxO], eMid[kMaxO], eAll[kMaxO];
    static int   snare[kMaxO];
    const int    m  = n > kMaxO ? kMaxO : n;
    const int    w  = (int)(0.040f * sr);   /* 40 ms after the onset */
    const float  aL = 1.f - expf(-2.f * 3.14159f * 200.f / sr);
    const float  aH = 1.f - expf(-2.f * 3.14159f * 4000.f / sr);
    float eMax = 1e-12f;
    for(int i = 0; i < m; i++)
    {
        int a = onsets[i] < 0 ? 0 : onsets[i];
        int b = a + w; if(b > len) b = len;
        float lp = 0.f, hp = 0.f; double sl = 0, sm = 0, st = 0;
        for(int k = a; k < b; k++)
        {
            const float v = x[k];
            lp += aL * (v - lp); hp += aH * (v - hp);
            const float mid = hp - lp;
            sl += lp * lp; sm += mid * mid; st += v * v;
        }
        const int cnt = b - a > 0 ? b - a : 1;
        eLow[i] = (float)(sl / cnt); eMid[i] = (float)(sm / cnt); eAll[i] = (float)(st / cnt);
        if(eAll[i] > eMax) eMax = eAll[i];
    }
    /* snare-like = mid dominates low, and loud enough to be a backbeat, not a
     * ghost (−11 dB of the loudest onset) */
    int ns = 0;
    for(int i = 0; i < m; i++)
        if(eMid[i] > 1.5f * eLow[i] && eAll[i] > 0.08f * eMax) snare[ns++] = onsets[i];
    if(ns < 3) return 0.f;

    /* histogram of CONSECUTIVE snare intervals, 20 ms bins, 0.3–1.6 s
     * (half-bars at 75–200 BPM); 3-bin peak, refined by the mean inside it */
    constexpr int kBins = 128;
    const float   binS  = 0.020f * sr;
    static float  hist[kBins];
    for(int i = 0; i < kBins; i++) hist[i] = 0.f;
    for(int i = 0; i + 1 < ns; i++)
    {
        const float d = (float)(snare[i + 1] - snare[i]);
        if(d < 0.3f * sr || d > 1.6f * sr) continue;
        const int bi = (int)(d / binS);
        if(bi < kBins) hist[bi] += 1.f;
    }
    int best = -1; float bestV = 0.f;
    for(int i = 1; i + 1 < kBins; i++)
    {
        const float v = hist[i - 1] + hist[i] + hist[i + 1];
        if(v > bestV) { bestV = v; best = i; }
    }
    /* speak only for a CLEAR backbeat: the winning interval must hold at least
     * half of all consecutive snare intervals (a wrong confident answer is
     * worse than the prior — a sped-up Amen fooled a looser rule) */
    if(best < 0 || bestV < 2.f || bestV < 0.5f * (float)(ns - 1)) return 0.f;
    double acc = 0; int cnt = 0;
    for(int i = 0; i + 1 < ns; i++)
    {
        const float d  = (float)(snare[i + 1] - snare[i]);
        const int   bi = (int)(d / binS);
        if(bi >= best - 1 && bi <= best + 1) { acc += d; cnt++; }
    }
    if(cnt < 2) return 0.f;
    const float half = (float)(acc / cnt);

    /* half-bar → bar. Some breaks put the loudest mid-band hit on every beat;
     * then the interval is a beat. Take the candidate whose bar count for
     * this file lands within 4 % of a power of two, at a sane tempo. */
    const float cands[2] = {2.f * half, 4.f * half};
    for(int c = 0; c < 2; c++)
    {
        const float bar = cands[c];
        const float bpm = 240.f * sr / bar;
        if(bpm < 60.f || bpm > 220.f) continue;
        const float bars = (float)len / bar;
        for(int p = 1; p <= 16; p <<= 1)
            if(fabsf(bars - (float)p) < 0.04f * (float)p) return bar;
    }
    return 0.f;
}

} // namespace bb
