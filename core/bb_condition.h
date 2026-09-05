#pragma once
#include <cmath>
#include <cstring>
#include "bb_onset.h"

namespace bb {

// Offline buffer conditioning, run once per load/record before analysis:
//  1) trim leading/trailing silence (-40 dB re peak, keeping a breath of air)
//  2) peak-normalize to 0.95 (idempotent: self-resampling doesn't creep)
//  3) snap loop length to a whole number of bars of the CONTENT's own 16th
//     grid, estimated from inter-onset intervals — so the wrap lands on a
//     beat and the internal groove agrees with the playback grid.
// Returns the new length (modifies x in place). Conservative: bails to the
// unmodified/partially-modified length whenever an estimate looks unsafe.
inline int ConditionBuffer(float* x, int len, float sr, OnsetDetector& det)
{
    if(len < (int)(0.25f * sr)) return len;

    float peak = 0.f;
    for(int i = 0; i < len; i++)
    {
        float a = fabsf(x[i]);
        if(a > peak) peak = a;
    }
    if(peak < 1e-4f) return len;

    // 1) trim
    float thr = 0.01f * peak;
    int   a   = 0;
    while(a < len && fabsf(x[a]) < thr) a++;
    int b = len - 1;
    while(b > a && fabsf(x[b]) < thr) b--;
    a = a > 32 ? a - 32 : 0;             // keep the attack's breath
    b = (b + 240 < len) ? b + 240 : len - 1;
    int n2 = b - a + 1;
    if(n2 < (int)(0.25f * sr)) return len;
    if(a > 0) memmove(x, x + a, sizeof(float) * (size_t)n2);
    len = n2;

    // 2) normalize
    float g = 0.95f / peak;
    for(int i = 0; i < len; i++) x[i] *= g;

    // 2b) cap at 4 musical bars (engine's own bar rule): longer files exceed
    // the 64-slice budget, degrading the grid to 8ths and computing native
    // tempo in the wrong domain (a 19 s break reported BPM below the pot's
    // floor and could never play at its own speed)
    {
        int   bars = 1;
        float dur  = (float)len / sr;
        while(dur / (float)bars >= 3.2f && bars < 16) bars <<= 1;
        if(bars > 4) len = (int)((long long)len * 4 / bars);
    }

    // 3) loop-length snap
    int onsets[128];
    int no = det.Detect(x, len, sr, onsets, 128);
    if(no >= 8)
    {
        float ioi[127];
        int   ni = 0;
        for(int i = 1; i < no && ni < 127; i++)
            ioi[ni++] = (float)(onsets[i] - onsets[i - 1]);
        for(int i = 1; i < ni; i++) // insertion sort for the median
            for(int j = i; j > 0 && ioi[j - 1] > ioi[j]; j--)
            {
                float t    = ioi[j - 1];
                ioi[j - 1] = ioi[j];
                ioi[j]     = t;
            }
        float s16 = ioi[ni / 2];
        while(s16 > 0.22f * sr) s16 *= 0.5f; // fold multi-16th IOIs down
        if(s16 >= 0.04f * sr)
        {
            for(int it = 0; it < 2; it++) // refine against all IOIs
            {
                float acc = 0.f;
                int   cnt = 0;
                for(int i = 0; i < ni; i++)
                {
                    float k = floorf(ioi[i] / s16 + 0.5f);
                    if(k < 1.f || k > 8.f) continue;
                    float e = ioi[i] / k;
                    if(fabsf(e - s16) < 0.15f * s16)
                    {
                        acc += e;
                        cnt++;
                    }
                }
                if(cnt >= 4) s16 = acc / (float)cnt;
            }
            int bars = (int)floorf((float)len / s16 / 16.f + 0.5f);
            if(bars >= 1)
            {
                int newLen = (int)((float)bars * 16.f * s16 + 0.5f);
                if(newLen <= len && (float)newLen >= 0.90f * (float)len
                   && newLen != len)
                {
                    // Adopt the snap ONLY if the onsets actually concentrate
                    // better on the resulting grid — the IOI tempo estimate
                    // is fooled by swing (it sheared 6% off a perfectly cut
                    // Amen and rotated the grid through the bar). Judged by
                    // circular mean resultant of onset phases at step period.
                    auto gridScore = [&](int L) -> float {
                        int   nb = 1;
                        float d  = (float)L / sr;
                        while(d / (float)nb >= 3.2f && nb < 16) nb <<= 1;
                        int ns = nb * 16;
                        while(ns > 64) ns /= 2;
                        float  step = (float)L / (float)ns;
                        double cr = 0., ci = 0.;
                        int    cnt = 0;
                        for(int i = 0; i < no; i++)
                        {
                            if(onsets[i] >= L) break;
                            float ph = fmodf((float)onsets[i], step) / step
                                       * 6.2831853f;
                            cr += cosf(ph);
                            ci += sinf(ph);
                            cnt++;
                        }
                        return cnt ? (float)sqrt(cr * cr + ci * ci) / cnt : 0.f;
                    };
                    if(gridScore(newLen) > gridScore(len) + 0.03f) len = newLen;
                }
            }
        }
    }
    return len;
}

} // namespace bb
