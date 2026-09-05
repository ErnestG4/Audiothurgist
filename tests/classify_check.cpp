// classify_check — run every classifier lens over real breaks and print them
// side by side, one character per grid step (K kick · S snare · H hat · .
// silent), plus the Density lens's raw keyframe density per step and each
// lens's agreement with Hockman. Build:
//   g++ -O2 -std=gnu++17 -Icore -Ivendor/capicola/lib tests/classify_check.cpp -o /tmp/cc
//   /tmp/cc "path/to/break.wav" [more.wav ...]
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include "bb_classify.h"

static bool LoadWav(const char* path, std::vector<float>& mono, float& sr)
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
            int bps = bits / 8, frames = (int)(got / (bps * ch));
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
                mono[i] = (float)(acc / ch);
            }
            fclose(f);
            return frames > 0 && sr > 0;
        }
        else fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR);
    }
    fclose(f);
    return false;
}

int main(int argc, char** argv)
{
    static const char* names[4] = {"Hockman", "Legacy ", "Two-band", "Density"};
    for(int a = 1; a < argc; a++)
    {
        std::vector<float> x; float sr;
        if(!LoadWav(argv[a], x, sr)) { printf("%s: cannot read\n", argv[a]); continue; }
        /* engine caps material at 25 s; analyse the first 8 bars' worth at most */
        int len = (int)x.size(); if(len > (int)(sr * 25)) len = (int)(sr * 25);
        int nsl = bb::GridSlices(len, sr);
        const char* base = strrchr(argv[a], '/'); base = base ? base + 1 : argv[a];
        printf("\n%s  (%.1f s, %d steps, %g Hz)\n", base, len / sr, nsl, sr);
        static uint8_t cls[4][bb::Engine::kMaxSlices];
        static float   dens[bb::Engine::kMaxSlices];
        static const float epsSweep[] = {0.2f, 0.35f, 0.5f};
        auto line = [&](int m, const char* label) {
            int agree = 0, live = 0;
            printf("  %-12s ", label);
            for(int s = 0; s < nsl; s++)
            {
                putchar("KSH."[cls[m][s]]);
                if((s & 15) == 15 && s != nsl - 1) putchar('|');
                if(cls[0][s] != 3) { live++; if(cls[m][s] == cls[0][s]) agree++; }
            }
            int nK = 0, nS = 0, nH = 0;
            for(int s = 0; s < nsl; s++) { nK += cls[m][s] == 0; nS += cls[m][s] == 1; nH += cls[m][s] == 2; }
            printf("   K%2d S%2d H%2d", nK, nS, nH);
            if(m) printf("  agree %3d%%", live ? 100 * agree / live : 0);
            putchar('\n');
        };
        for(int m = 0; m < 3; m++)
        {
            bb::ClassifySteps(x.data(), len, sr, cls[m], m, nullptr, nullptr);
            line(m, names[m]);
        }
        for(float eps : epsSweep)
        {
            bb::gDensityEps = eps;
            bb::ClassifySteps(x.data(), len, sr, cls[3], 3, nullptr, dens);
            char label[32]; snprintf(label, sizeof label, "Density ε%.2f", eps);
            line(3, label);
            float mx = 1e-9f;
            for(int s = 0; s < nsl; s++) if(dens[s] > mx) mx = dens[s];
            printf("   d/max ×9    ");
            for(int s = 0; s < nsl; s++) { int v = (int)(dens[s] / mx * 9.f + 0.5f); printf("%d", v > 9 ? 9 : v); if((s & 15) == 15 && s != nsl - 1) putchar('|'); }
            printf("   max %.3f kf/sample\n", mx);
        }
    }
    return 0;
}
