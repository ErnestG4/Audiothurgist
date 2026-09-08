#pragma once
// SAMPLE: region helpers for recording into a key (docs/packages/sample-engine.md).
// Pure, CONTROL thread only. Run between Engine::RecStop and
// Engine::CommitSlice on the raw region the engine handed back — the audio
// thread is not reading it (no table entry points there yet) and not
// writing it (the take is stopped). Both planes when xR is given. Long loops
// call bb::Yield() every 4096 samples so the shell's clock keeps ticking.
#include <cmath>
#include <cstdint>
#include "bb_classify.h"
#include "bb_yield.h"

namespace bb {

// Trim silence off both ends of [start, start + len) at -40 dB re the
// region's own peak, keeping ConditionBuffer's breaths (~0.7 ms before the
// first sound, 5 ms after the last), and peak-normalise to 0.95 in place —
// one gain for both planes, so a stereo image holds. Nothing moves: the
// trimmed region [outStart, outStart + outLen) lies inside the original and
// is what CommitSlice should be given; the few samples of silence before it
// stay orphaned in the buffer. Returns false — and leaves outStart/outLen
// untouched — when the region is silent or shorter than Engine::kRecMin
// after trimming: RecDiscard it.
inline bool TrimRegion(float* x, float* xR, int start, int len, float sr, int& outStart, int& outLen)
{
    if(!x || len <= 0 || start < 0) return false;
    const int end = start + len;
    float peak = 0.f;
    for(int i = start; i < end; i++)
    {
        float a = fabsf(x[i]);
        if(a > peak) peak = a;
        if(xR) { a = fabsf(xR[i]); if(a > peak) peak = a; }
        if((i & 4095) == 4095) Yield();
    }
    if(peak < 1e-4f) return false;

    const float thr = 0.01f * peak;
    auto quiet = [&](int i) { return fabsf(x[i]) < thr && (!xR || fabsf(xR[i]) < thr); };
    int a = start;
    while(a < end && quiet(a)) { a++; if((a & 4095) == 0) bb::Yield(); }
    int b = end - 1;
    while(b > a && quiet(b)) { b--; if((b & 4095) == 0) bb::Yield(); }
    const int head = (int)(sr / 1500.f), tail = (int)(0.005f * sr);   // 32 / 240 at 48 kHz
    a = (a - head < start) ? start : a - head;
    b = (b + tail > end - 1) ? end - 1 : b + tail;
    const int n2 = b - a + 1;
    if(n2 < Engine::kRecMin) return false;

    const float g = 0.95f / peak;
    for(int i = a; i <= b; i++)
    {
        x[i] *= g;
        if(xR) xR[i] *= g;
        if((i & 4095) == 4095) Yield();
    }
    outStart = a;
    outLen   = n2;
    return true;
}

// Per-region metadata for Engine::CommitSlice, from a trimmed, normalised
// region (the L plane, or the shell's mono sum) — the recorded twin of the
// per-step analysis a load runs on the grid:
//  cls  — the dominant band of the WHOLE region, judged absolutely (the
//         loop-relative rule of ClassifySteps has no loop here): modes 0
//         and 3 use the Hockman edges 325 / 4000 Hz (the Density lens needs
//         a population to rank, so it borrows the bands), 1 the legacy
//         120 / 900 split, 2 the two-band lo/hi rule; silence → 3.
//  sal  — the region's RMS relative to refRms, clamped to 1 (pass the
//         loudest grid slice's RMS to rank the take against the break);
//         refRms <= 0 → 1.0: a normalised take is a full hit.
//  dens — keyframe density (bb_classify's Density lens over the region as
//         one step), for Options → Fades: Adaptive.
//  hit  — does it open with an attack: the peak inside the first 10 ms
//         reaches a quarter of the region's peak (a trimmed drum does; a
//         swell does not). First guesses for ears, like the lens ratios.
inline void AnalyseRegion(const float* x, int start, int len, float sr, int mode,
                          uint8_t& cls, float& sal, float& dens, bool& hit, float refRms = 0.f)
{
    cls = 3; sal = 0.f; dens = 0.f; hit = false;
    if(!x || len < 1 || start < 0) return;
    const float* p  = x + start;
    const float  fK = (mode == 1) ? 120.f : 325.f;
    const float  fM = (mode == 1) ? 900.f : 4000.f;
    const float  aK = 1.f - expf(-2.f * 3.14159f * fK / sr);
    const float  aM = 1.f - expf(-2.f * 3.14159f * fM / sr);
    float  lpK = 0.f, lpM = 0.f, peak = 0.f;
    double sl = 0., sm = 0., sh = 0., sx = 0.;
    for(int i = 0; i < len; i++)
    {
        const float v = p[i];
        lpK += aK * (v - lpK);
        lpM += aM * (v - lpM);
        const float mid = lpM - lpK, hi = v - lpM;
        sl += lpK * lpK; sm += mid * mid; sh += hi * hi; sx += v * v;
        const float a = fabsf(v);
        if(a > peak) peak = a;
        if((i & 4095) == 4095) Yield();
    }
    const float rms = (float)sqrt(sx / (double)len);
    if(rms < 1e-5f) return;   // silent: class 3, no salience, no hit
    const double m = sm, h = sh, l = sl;
    if(mode == 2) cls = (l >= (m > h ? m : h)) ? 0 : 2;
    else          cls = (l >= m && l >= h) ? 0 : (m >= h ? 1 : 2);
    sal = refRms > 0.f ? (rms / refRms > 1.f ? 1.f : rms / refRms) : 1.f;

    int n10 = (int)(0.010f * sr);
    if(n10 > len) n10 = len;
    float pk10 = 0.f;
    for(int i = 0; i < n10; i++) { const float a = fabsf(p[i]); if(a > pk10) pk10 = a; }
    hit = pk10 >= 0.25f * peak;

    static uint8_t c[Engine::kMaxSlices];
    static float   d[Engine::kMaxSlices];
    ClassifySteps(p, len, sr, c, 3, nullptr, d, 1);   // the Density lens, one step
    dens = d[0];
}

} // namespace bb
