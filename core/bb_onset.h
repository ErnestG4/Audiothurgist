#pragma once
#include <cmath>
#include <cstring>

namespace bb {

// Offline onset detection: spectral flux with an adaptive median threshold.
// Runs once, after recording stops, before playback — never in the audio
// callback, so it has no real-time constraint. Same pipeline as the Phase 0
// Python (analysis/phase0.py); keep them in agreement.
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

    float win[kN];
    float re[kN], im[kN];
    float mag[kBins], prevMag[kBins];
    float flux[kMaxFrames];
    float thr[kMaxFrames];

    // Returns onset count. out[0] is always 0 (the buffer start is a slice).
    // salience (optional, same length as out) receives each onset's flux peak
    // height, so callers can keep the K most salient onsets when a break has
    // more onsets than slice slots. The start onset gets effectively infinite
    // salience — the downbeat slice always survives selection.
    int Detect(const float* x, int len, float sr, int* out, int maxOut, float* salience = nullptr)
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

        int nf = (len - kN) / kHop + 1;
        if(nf > kMaxFrames) nf = kMaxFrames;

        memset(prevMag, 0, sizeof(prevMag));
        float meanFlux = 0.f;
        for(int f = 0; f < nf; f++)
        {
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
