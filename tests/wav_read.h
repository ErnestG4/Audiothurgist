#pragma once
// Minimal RIFF/WAVE reader for the host tests: PCM 16/24/32 and float 32,
// any channel count (mixed to mono). Not for the firmware.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

inline bool LoadWav(const char* path, std::vector<float>& mono, float& sr)
{
    FILE* f = fopen(path, "rb");
    if(!f) return false;
    uint8_t h[12];
    if(fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return false; }
    int fmt = 0, ch = 0, bits = 0; sr = 0;
    for(;;)
    {
        uint8_t c[8];
        if(fread(c, 1, 8, f) != 8) break;
        uint32_t sz = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24);
        if(!memcmp(c, "fmt ", 4))
        {
            std::vector<uint8_t> b(sz); if(fread(b.data(), 1, sz, f) != sz) break;
            fmt = b[0] | (b[1] << 8); ch = b[2] | (b[3] << 8);
            sr = (float)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24));
            bits = b[14] | (b[15] << 8);
            if(fmt == 0xFFFE && sz >= 26) fmt = b[24] | (b[25] << 8);
        }
        else if(!memcmp(c, "data", 4))
        {
            std::vector<uint8_t> b(sz); size_t got = fread(b.data(), 1, sz, f);
            int bps = bits / 8, frames = bps * ch > 0 ? (int)(got / (bps * ch)) : 0;
            mono.resize(frames);
            for(int i = 0; i < frames; i++)
            {
                double acc = 0;
                for(int k = 0; k < ch; k++)
                {
                    const uint8_t* p = b.data() + (size_t)(i * ch + k) * bps;
                    double v = 0;
                    if(fmt == 3 && bits == 32) { float x; memcpy(&x, p, 4); v = x; }
                    else if(bits == 16) v = (int16_t)(p[0] | (p[1] << 8)) / 32768.0;
                    else if(bits == 24) v = ((int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) >> 8) / 8388608.0;
                    else if(bits == 32) v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) / 2147483648.0;
                    acc += v;
                }
                mono[i] = (float)(acc / (ch > 0 ? ch : 1));
            }
            fclose(f);
            return frames > 0 && sr > 0;
        }
        else fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR);
    }
    fclose(f);
    return false;
}
