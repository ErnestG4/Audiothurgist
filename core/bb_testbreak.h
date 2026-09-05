#pragma once
#include <cmath>
#include "bb_rng.h"

namespace bb {

// Deterministic synthesized one-bar break @ 172 BPM (kick/snare/hats), so the
// Pod makes breakbeat-shaped sound at boot with no SD card and no samples.
// Placeholder material — real breaks arrive with the SD path.
inline int MakeTestBreak(float* buf, int maxLen, float sr)
{
    const float bpm     = 172.f;
    const int   stepLen = (int)(sr * 60.f / bpm / 4.f); // one 16th
    int         len     = stepLen * 16;
    if(len > maxLen) len = maxLen;
    for(int i = 0; i < len; i++) buf[i] = 0.f;

    Rng rng;
    rng.Seed(0xBEA7B1A57ULL);

    auto addKick = [&](int at, float amp) {
        float ph = 0.f;
        int   n  = (int)(0.22f * sr);
        for(int i = 0; i < n && at + i < len; i++)
        {
            float t = (float)i / sr;
            float f = 52.f + 95.f * expf(-t * 32.f);
            ph += 2.f * 3.14159265f * f / sr;
            buf[at + i] += amp * sinf(ph) * expf(-t * 16.f);
        }
    };
    auto addSnare = [&](int at, float amp) {
        int   n  = (int)(0.18f * sr);
        float ph = 0.f;
        for(int i = 0; i < n && at + i < len; i++)
        {
            float t  = (float)i / sr;
            float nz = (rng.Uniform() * 2.f - 1.f) * expf(-t * 26.f);
            ph += 2.f * 3.14159265f * 187.f / sr;
            float tone = sinf(ph) * expf(-t * 42.f);
            buf[at + i] += amp * (0.75f * nz + 0.5f * tone);
        }
    };
    auto addHat = [&](int at, float amp) {
        int   n    = (int)(0.05f * sr);
        float prev = 0.f;
        for(int i = 0; i < n && at + i < len; i++)
        {
            float t  = (float)i / sr;
            float nz = rng.Uniform() * 2.f - 1.f;
            float hp = nz - prev; // one-pole highpass-ish
            prev     = nz;
            buf[at + i] += amp * hp * expf(-t * 90.f);
        }
    };

    const int kicks[]  = {0, 7, 10};
    const int snares[] = {4, 12};
    for(int k : kicks) addKick(k * stepLen, 0.95f);
    for(int s : snares) addSnare(s * stepLen, 0.85f);
    addSnare(14 * stepLen, 0.35f); // ghost
    for(int h = 0; h < 16; h++) addHat(h * stepLen, (h & 1) ? 0.28f : 0.42f);

    float peak = 0.f;
    for(int i = 0; i < len; i++)
        if(fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
    if(peak > 0.f)
    {
        float g = 0.9f / peak;
        for(int i = 0; i < len; i++) buf[i] *= g;
    }
    return len;
}

} // namespace bb
