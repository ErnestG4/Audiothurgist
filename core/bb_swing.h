#pragma once
// Swing of the material's own 16ths, from onset phases (Options → Swing: Auto).
//
// Frane 2017 measured the 16th-note swing ratio of 30 canon breaks: median
// 1.2, mean 1.3, range 1.0–2.1, uncorrelated with tempo. Straight 16th
// slicing of a swung break puts the odd-16th boundary 5–18 % of an 8th
// BEFORE the hat: the odd slice opens with the previous drum's tail, the
// even slice loses its own, and any rearrangement plays the hat late (or the
// kick tail early). So we measure where the off-beat cluster actually sits
// and hand the engine that position for both the slice boundary and the
// odd steps' trigger time.
//
// Definition: p = mean phase of the off-beat onset cluster inside its 8th
// (0.5 = straight); swing ratio = p / (1 − p). Accepted only when the
// on-beat cluster confirms the grid (|mean phase| small, concentrated) and
// the off-beat cluster is tight (circular R ≥ kMinR) with a ratio in
// [1.05, 2.2]; otherwise 0.5. Pure, no allocation, offline.
#include <cmath>

namespace bb {

struct SwingEstimate
{
    float p      = 0.5f; // odd-16th position inside the 8th, 0.5 = straight
    float ratio  = 1.f;  // p / (1 − p)
    float r      = 0.f;  // circular concentration of the off-beat cluster
    int   nOff   = 0;    // onsets in the off-beat cluster
    int   nOn    = 0;
};

namespace swingdetail {
constexpr float kMinR      = 0.60f;
constexpr int   kMinOff    = 4;
constexpr float kMinRatio  = 1.05f;
constexpr float kMaxRatio  = 2.20f;
constexpr float kOnTol     = 0.12f;   // on-beat cluster half-width (of the 8th)
constexpr float kOnMaxMean = 0.05f;   // grid misaligned beyond this → no swing
} // namespace swingdetail

// step = samples per 16th of the grid the engine will play (len / nSlices);
// phase = sample position of grid step 0 (0 after conditioning).
// sal (optional) weights each onset; nullptr = equal weights.
inline SwingEstimate EstimateSwing(const int* onsets, int n, float step, float phase = 0.f,
                                   const float* sal = nullptr)
{
    using namespace swingdetail;
    SwingEstimate e;
    if(!onsets || n < 2 || step <= 1.f) return e;
    const float cell = 2.f * step;
    const float kTwoPi = 6.28318531f;
    double onC = 0., onS = 0., onW = 0.;
    double offC = 0., offS = 0., offW = 0.;
    int nOn = 0, nOff = 0;
    for(int i = 0; i < n; i++)
    {
        float u = ((float)onsets[i] - phase) / cell;
        u -= floorf(u);                         // [0,1) inside the 8th
        const float w = sal ? (sal[i] > 0.f ? sal[i] : 0.f) : 1.f;
        if(w <= 0.f) continue;
        // on-beat cluster around 0 (wrapping), off-beat cluster in (0.3, 0.85)
        const float uc = u > 0.5f ? u - 1.f : u;
        if(fabsf(uc) <= kOnTol)
        {
            onC += w * cos(kTwoPi * (double)uc); onS += w * sin(kTwoPi * (double)uc); onW += w; nOn++;
        }
        else if(u > 0.30f && u < 0.85f)
        {
            offC += w * cos(kTwoPi * (double)u); offS += w * sin(kTwoPi * (double)u); offW += w; nOff++;
        }
    }
    e.nOn = nOn; e.nOff = nOff;
    if(nOn < 2 || nOff < kMinOff || onW <= 0. || offW <= 0.) return e;
    const double onMean = atan2(onS, onC) / kTwoPi;         // (−0.5, 0.5]
    if(fabs(onMean) > kOnMaxMean) return e;                  // the grid itself is off
    const double offMean = atan2(offS, offC) / kTwoPi;
    double p = offMean < 0. ? offMean + 1. : offMean;
    const double R = sqrt(offC * offC + offS * offS) / offW;
    e.r = (float)R;
    if(R < kMinR) return e;
    const double ratio = p / (1. - p);
    if(ratio < kMinRatio || ratio > kMaxRatio) return e;
    e.p     = (float)p;
    e.ratio = (float)ratio;
    return e;
}

} // namespace bb
