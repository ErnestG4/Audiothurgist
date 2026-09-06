#pragma once
#include <cmath>
#include <cstring>
#include "bb_yield.h"

namespace bb {

// Offline onset detection: spectral flux with an adaptive median threshold.
// Runs once, after recording stops, before playback — never in the audio
// callback, so it has no real-time constraint. Same pipeline as the Phase 0
// Python (analysis/phase0.py); keep them in agreement.
//
// Two modes (Options → Onsets), selected by `mode` before Detect():
//  0  linear-magnitude spectral flux, 1.5 × local-median threshold (v0.8.0;
//     this path is byte-identical to the original — do not touch it)
//  1  log-compressed, filterbank-smoothed spectral flux (Böck, Krebs &
//     Schedl 2012: SF 74.5 → log-filtered SF 80.3 F-measure) with Dixon
//     2006's two-condition peak rule (local maximum over ±w frames AND
//     above the local mean over [n−m·w, n+w] by δ). Same 512-pt FFT, hop,
//     minimum gap and waveform refine as mode 0. Quiet hats under loud
//     kicks survive the log compression; the band sums stop a single
//     resonant bin from voting alone.
//
// ~100 KB of scratch: instantiate once, statically.
struct OnsetDetector
{
    static constexpr int   kN         = 512;
    static constexpr int   kHop       = 256;
    static constexpr int   kBins      = kN / 2;
    static constexpr int   kMaxFrames = 8192; // 43 s @ 48 kHz
    static constexpr int   kMed       = 8;    // median half-window (frames)
    static constexpr float kPi        = 3.14159265f;

    // mode 1 parameters (Böck 2012 / Dixon 2006, scaled to the 5.3 ms hop)
    static constexpr int   kMaxBands  = 64;
    static constexpr float kBandRatio = 1.10f;  // ~quarter-tone-ish at the top,
                                                // ≥1 bin per band at the bottom
    static constexpr float kLogLambda = 10.f;   // log(1 + λ·|X|), |X| ≈ 1 full scale
    static constexpr int   kPeakW     = 6;      // ±32 ms local maximum
    static constexpr int   kPeakM     = 3;      // mean window −m·w … +w
    static constexpr float kPeakDelta = 0.35f;  // × global mean of the function

    int mode;   // no in-class initialiser: instances live in NOLOAD SDRAM (review M4)

    float win[kN];
    float re[kN], im[kN];
    float mag[kBins], prevMag[kBins];
    float flux[kMaxFrames];
    float thr[kMaxFrames];
    // mode 1 scratch
    int   bandEdge[kMaxBands + 1];
    int   nBands;
    float bandL[kMaxBands], prevBandL[kMaxBands];

    // Returns onset count. out[0] is always 0 (the buffer start is a slice).
    // salience (optional, same length as out) receives each onset's flux peak
    // height, so callers can keep the K most salient onsets when a break has
    // more onsets than slice slots. The start onset gets effectively infinite
    // salience — the downbeat slice always survives selection.
    int Detect(const float* x, int len, float sr, int* out, int maxOut, float* salience = nullptr)
    {
        if(mode == 1) return DetectLogFiltered(x, len, sr, out, maxOut, salience);
        if(len < kN || maxOut < 1)
        {
            if(maxOut > 0)
            {
                out[0] = 0;
                if(salience) salience[0] = 1e9f;
            }
            return maxOut > 0 ? 1 : 0;
        }

        for(int i = 0; i < kN; i++)
            win[i] = 0.5f - 0.5f * cosf(2.f * kPi * i / kN);

        int nf = (len - kN) / kHop + 1;
        if(nf > kMaxFrames) nf = kMaxFrames;

        memset(prevMag, 0, sizeof(prevMag));
        float meanFlux = 0.f;
        for(int f = 0; f < nf; f++)
        {
            if((f & 31) == 0) Yield();
            const float* seg = x + f * kHop;
            for(int i = 0; i < kN; i++)
            {
                re[i] = seg[i] * win[i];
                im[i] = 0.f;
            }
            Fft(re, im);
            float fl = 0.f;
            for(int b = 1; b < kBins; b++)
            {
                float m = sqrtf(re[b] * re[b] + im[b] * im[b]);
                float d = m - prevMag[b];
                if(d > 0.f) fl += d;
                prevMag[b] = m;
                mag[b]     = m;
            }
            flux[f] = fl;
            meanFlux += fl;
        }
        meanFlux /= (float)nf;

        // adaptive threshold: local median × 1.5 + floor
        for(int f = 0; f < nf; f++)
        {
            if((f & 31) == 0) Yield();
            float w[2 * kMed + 1];
            int   m = 0;
            for(int k = f - kMed; k <= f + kMed; k++)
            {
                float v = (k < 0 || k >= nf) ? 0.f : flux[k];
                int   p = m++;
                while(p > 0 && w[p - 1] > v)
                {
                    w[p] = w[p - 1];
                    p--;
                }
                w[p] = v;
            }
            thr[f] = w[kMed] * 1.5f + 0.1f * meanFlux;
        }

        // peak pick, ≥35 ms apart
        const int minGap = (int)(0.035f * sr / kHop) + 1;
        int       no     = 0;
        if(salience) salience[no] = 1e9f;
        out[no++] = 0;
        int last  = -minGap;
        for(int f = 1; f < nf - 1 && no < maxOut; f++)
        {
            if(flux[f] <= thr[f]) continue;
            if(flux[f] < flux[f - 1] || flux[f] <= flux[f + 1]) continue;
            if(f - last < minGap) continue;
            int smp = Refine(x, len, f * kHop);
            if(smp > (int)(0.02f * sr)) // don't duplicate the start slice
            {
                if(salience) salience[no] = flux[f];
                out[no++] = smp;
            }
            last = f;
        }
        return no;
    }

  private:
    /* ── mode 1: log-filtered spectral flux + Dixon peak rule ─────────── */
    void BuildBands()
    {
        // geometric band edges in bins, at least one bin per band; the first
        // bin (DC) is skipped as in mode 0
        int e = 1, nb = 0;
        bandEdge[0] = 1;
        while(e < kBins && nb < kMaxBands)
        {
            int next = (int)((float)e * kBandRatio + 0.5f);
            if(next <= e) next = e + 1;
            if(next > kBins) next = kBins;
            bandEdge[++nb] = next;
            e = next;
        }
        if(nb > 0 && bandEdge[nb] < kBins) bandEdge[nb] = kBins;
        nBands = nb;
    }

    int DetectLogFiltered(const float* x, int len, float sr, int* out, int maxOut, float* salience)
    {
        if(len < kN || maxOut < 1)
        {
            if(maxOut > 0)
            {
                out[0] = 0;
                if(salience) salience[0] = 1e9f;
            }
            return maxOut > 0 ? 1 : 0;
        }
        for(int i = 0; i < kN; i++)
            win[i] = 0.5f - 0.5f * cosf(2.f * kPi * i / kN);
        BuildBands();

        int nf = (len - kN) / kHop + 1;
        if(nf > kMaxFrames) nf = kMaxFrames;

        // |X| scaled so a full-scale sine reads ≈ 1 in its bin (Hann: N/4)
        const float magScale = 4.f / (float)kN;
        for(int b = 0; b < nBands; b++) prevBandL[b] = 0.f;
        double meanFlux = 0.;
        for(int f = 0; f < nf; f++)
        {
            if((f & 31) == 0) Yield();
            const float* seg = x + f * kHop;
            for(int i = 0; i < kN; i++)
            {
                re[i] = seg[i] * win[i];
                im[i] = 0.f;
            }
            Fft(re, im);
            for(int b = 1; b < kBins; b++) mag[b] = sqrtf(re[b] * re[b] + im[b] * im[b]) * magScale;
            float fl = 0.f;
            for(int b = 0; b < nBands; b++)
            {
                float acc = 0.f;
                const int a0 = bandEdge[b], a1 = bandEdge[b + 1];
                for(int k = a0; k < a1; k++) acc += mag[k];
                acc /= (float)(a1 - a0);
                const float L = logf(1.f + kLogLambda * acc);
                const float d = L - prevBandL[b];
                if(d > 0.f) fl += d;
                prevBandL[b] = L;
            }
            flux[f] = fl;
            meanFlux += fl;
        }
        meanFlux /= (double)nf;
        const float delta = kPeakDelta * (float)meanFlux;

        // Dixon 2006: f(n) ≥ f(k) for k ∈ [n−w, n+w], and
        //             f(n) ≥ mean(f[n−m·w … n+w]) + δ
        const int minGap = (int)(0.035f * sr / kHop) + 1;
        int       no     = 0;
        if(salience) salience[no] = 1e9f;
        out[no++] = 0;
        int last = -minGap;
        for(int f = 1; f < nf - 1 && no < maxOut; f++)
        {
            const float v = flux[f];
            if(v <= 0.f) continue;
            bool  isMax = true;
            float sum   = 0.f;
            int   cnt   = 0;
            for(int k = f - kPeakM * kPeakW; k <= f + kPeakW; k++)
            {
                const float u = (k < 0 || k >= nf) ? 0.f : flux[k];
                sum += u;
                cnt++;
                if(k >= f - kPeakW && k != f)
                {
                    // strict on the right so a plateau yields one peak
                    if((k < f && u > v) || (k > f && u >= v)) { isMax = false; break; }
                }
            }
            if(!isMax) continue;
            if(v < sum / (float)cnt + delta) continue;
            if(f - last < minGap) continue;
            int smp = RefineWide(x, len, f * kHop);
            if(smp > (int)(0.02f * sr))
            {
                if(salience) salience[no] = v;
                out[no++] = smp;
            }
            last = f;
        }
        return no;
    }

    // Mode 1's refine: the flux peak at frame f says the transient lies in
    // that frame's WINDOW, [f·hop, f·hop + N) — mode 0's ±hop search misses
    // an onset in the second half of the frame and, finding no rise at all,
    // returns the window's first sample (up to 10 ms early; every other hat
    // on a 16th grid, depending on where the frame edges fall). Search the
    // whole window and keep the frame position when nothing rises.
    static int RefineWide(const float* x, int len, int approx)
    {
        const int w  = 32;
        int       lo = approx - kHop / 2;
        int       hi = approx + kN;
        if(lo < w) lo = w;
        if(hi > len - w) hi = len - w;
        int   best  = approx;
        float bestD = 0.f;
        for(int p = lo; p < hi; p += 8)
        {
            float ePost = 0.f, ePre = 0.f;
            for(int i = 0; i < w; i++)
            {
                ePost += fabsf(x[p + i]);
                ePre += fabsf(x[p - w + i]);
            }
            const float d = ePost - ePre;
            if(d > bestD)
            {
                bestD = d;
                best  = p;
            }
        }
        best -= 16;
        return best < 0 ? 0 : best;
    }

    // The flux peak localises the transient only to ±one hop. Refine to the
    // steepest short-term energy rise so slices keep their attack.
    static int Refine(const float* x, int len, int approx)
    {
        const int w  = 32;
        int       lo = approx - kHop;
        int       hi = approx + kHop;
        if(lo < w) lo = w;
        if(hi > len - w) hi = len - w;
        int   best  = approx;
        float bestD = -1e30f;
        for(int p = lo; p < hi; p += 8)
        {
            float ePost = 0.f, ePre = 0.f;
            for(int i = 0; i < w; i++)
            {
                ePost += fabsf(x[p + i]);
                ePre += fabsf(x[p - w + i]);
            }
            float d = ePost - ePre;
            if(d > bestD)
            {
                bestD = d;
                best  = p;
            }
        }
        best -= 16; // land just ahead of the attack
        return best < 0 ? 0 : best;
    }

    static void Fft(float* re, float* im)
    {
        for(int i = 1, j = 0; i < kN; i++)
        {
            int bit = kN >> 1;
            for(; j & bit; bit >>= 1) j ^= bit;
            j |= bit;
            if(i < j)
            {
                float t;
                t = re[i]; re[i] = re[j]; re[j] = t;
                t = im[i]; im[i] = im[j]; im[j] = t;
            }
        }
        for(int l = 2; l <= kN; l <<= 1)
        {
            float ang = -2.f * kPi / (float)l;
            float wr = cosf(ang), wi = sinf(ang);
            for(int i = 0; i < kN; i += l)
            {
                float cr = 1.f, ci = 0.f;
                for(int k = 0; k < l / 2; k++)
                {
                    int   a = i + k, b = i + k + l / 2;
                    float xr = re[b] * cr - im[b] * ci;
                    float xi = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                    float nc = cr * wr - ci * wi;
                    ci       = cr * wi + ci * wr;
                    cr       = nc;
                }
            }
        }
    }
};

} // namespace bb
