#pragma once
#include <cmath>
#include <cstring>
#include "bb_onset.h"
#include "bb_bars.h"
#include "bb_yield.h"

namespace bb {

// Offline buffer conditioning, run once per load/record before analysis:
//  1) trim leading/trailing silence (-40 dB re peak, keeping a breath of air)
//  2) peak-normalize to 0.95 (idempotent: self-resampling doesn't creep)
//  3) cut the loop to a whole number of bars of the CONTENT's own bar:
//     3a) when bb::EstimateBar is confident (bb_bars.h): start at its
//         downbeat when the file has slack before it, keep 1/2/4 bars of its
//         bar, and leave a file that already IS a whole number of bars
//         untouched (the cut is the truth; ±3 %). A jungle-tempo 4-bar
//         break therefore stays 4 bars instead of "2 bars of 8ths".
//     3b) otherwise the v0.8.0 path: cap at 4 bars of the 1.6–3.2 s duration
//         prior and snap the length from the folded median IOI, adopted
//         only if the onsets concentrate better on the resulting grid.
// Returns the new length (modifies x in place). `barOut` (optional) receives
// the confident bar estimate re-based on the returned buffer (bars = whole
// bars kept; bar = 0 when the prior applied) so the shell can slice without
// re-deriving it. Conservative: bails to the unmodified/partially-modified
// length whenever an estimate looks unsafe.
struct ConditionInfo
{
    float bar  = 0.f; // samples per bar on the returned buffer, 0 = prior
    int   bars = 0;   // whole bars kept (1, 2, 4), 0 = prior
    float conf = 0.f;
    int   trimmedStart = 0; // samples removed at the head (silence + downbeat)
};

// allowBarCut = false (Options → Bars set by hand): skip 3a so the user's bar
// count applies to the file as it is, not to an estimator-cut buffer (review M5).
inline int ConditionBuffer(float* x, int len, float sr, OnsetDetector& det, ConditionInfo* info = nullptr,
                           bool allowBarCut = true)
{
    if(info) *info = ConditionInfo();
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
    /* shift EVERYTHING past the head trim, not just the body: 3a may take the
     * trimmed tail back (lenValid), and it must be shifted data (review H1) */
    if(a > 0) memmove(x, x + a, sizeof(float) * (size_t)(len - a));
    const int lenValid = len - a;   // samples still in memory past the tail trim
    len = n2;
    const int headTrim = a;

    // 2) normalize (the trimmed tail too: 3a may take it back)
    float g = 0.95f / peak;
    for(int i = 0; i < lenValid; i++) x[i] *= g;

    /* SLICE: 3a) whole bars of the material's own bar, when confident. Two
     * attempts: the whole file, then (a long song excerpt can carry a tempo
     * change or a fill that muddles the clusters) the first 4 prior-bars. */
    for(int attempt = 0; allowBarCut && attempt < 2; attempt++)
    {
        Yield();
        int (&onsets)[256] = bardetail::ScratchRef().condOnsets;
        int lenA = len;
        if(attempt == 1)
        {
            /* a shorter second look: the first 8 s (a live take drifts; a
             * song excerpt changes) — only when that is a real reduction */
            lenA = (int)(8.f * sr);
            if(len < (int)(9.5f * sr)) break;
        }
        const int  no  = det.Detect(x, lenA, sr, onsets, 256);
        BarEstimate est = EstimateBar(x, lenA, sr, onsets, no);
        if(est.bar > 0.f)
        {
            /* `len` is NOT truncated to lenA here: a vetoed attempt must leave
             * the full file for the prior path (review H2) */
            const float B = est.bar;
            float db = est.downbeat;
            if(db > 0.5f * B) db -= B;                   // just before the start
            int start = db > 0.1f * B / 16.f ? (int)(db + 0.5f) : 0;  // within a
                                                          // tenth of a 16th = the start
            int avail = lenA - start;
            if(avail < (int)(0.97f * B) && start > 0) { start = 0; avail = lenA; }
            /* a 16th of slack: a loop whose last 16th is quiet lost it to the
             * tail trim above; the samples are still in memory, so the cut
             * can keep the whole bar */
            int nb = (int)((float)avail / B + 1.f / 16.f);
            if(nb >= 1)
            {
                int keep = nb >= 4 ? 4 : (nb >= 2 ? 2 : 1);
                int newLen = (int)((float)keep * B + 0.5f);
                if(newLen > lenValid - start) newLen = lenValid - start;
                // a file that already is `keep` bars: keep all of it
                if(newLen < avail && avail - newLen < (int)(B / 16.f)) newLen = avail;
                /* verify before cutting: the trimmed window's own onsets must
                 * tell the same bar (a wrong confident cut is destructive —
                 * the head is gone). Onsets are reused, shifted: no second
                 * detection. A silent second opinion is allowed; a
                 * dissenting one vetoes. */
                if(newLen != lenA || start > 0)
                {
                    int (&sub)[256] = bardetail::ScratchRef().condSub;
                    int ns = 0;
                    for(int i = 0; i < no; i++)
                    {
                        const int q = onsets[i] - start;
                        if(q >= 0 && q < newLen && ns < 256) sub[ns++] = q;
                    }
                    if(ns > 0 && sub[0] != 0) { for(int i = ns; i > 0 && i < 256; i--) sub[i] = sub[i - 1]; if(ns < 256) ns++; sub[0] = 0; }
                    const BarEstimate v = EstimateBar(x + start, newLen, sr, sub, ns);
                    if(v.bar > 0.f && fabsf(v.bar - B) > 0.06f * B) continue;   // next attempt
                }
                if(start > 0) memmove(x, x + start, sizeof(float) * (size_t)newLen);
                if(info)
                {
                    info->bar  = (float)newLen / (float)keep;
                    info->bars = keep;
                    info->conf = est.conf;
                    info->trimmedStart = headTrim + start;
                }
                return newLen;
            }
        }
    }

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
