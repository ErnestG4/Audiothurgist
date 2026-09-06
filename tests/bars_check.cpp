// bars_check — bar/tempo estimate on real breaks: the duration prior vs the
// backbeat estimate (bb_bars.h), with the onset detector the firmware uses.
//   g++ -O2 -std=gnu++17 -Icore -Ivendor/capicola/lib tests/bars_check.cpp -o /tmp/bc
//   /tmp/bc break.wav [...]        (add "--bpm N" before a file to state its truth)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "wav_read.h"
#include "bb_engine.h"
#include "bb_classify.h"
#include "bb_onset.h"
#include "bb_bars.h"

static bb::OnsetDetector gDet;

int main(int argc, char** argv)
{
    float truth = 0.f, speed = 1.f;
    int ok = 0, tried = 0;
    for(int a = 1; a < argc; a++)
    {
        if(!strcmp(argv[a], "--bpm") && a + 1 < argc) { truth = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--speed") && a + 1 < argc) { speed = (float)atof(argv[++a]); continue; }   /* varispeed: tempo × speed */
        std::vector<float> x; float sr;
        if(!LoadWav(argv[a], x, sr)) { printf("%s: cannot read\n", argv[a]); continue; }
        if(speed != 1.f)
        {
            std::vector<float> y((size_t)(x.size() / speed));
            for(size_t i = 0; i < y.size(); i++) { double p = i * (double)speed; size_t k = (size_t)p; float fr = (float)(p - k); y[i] = k + 1 < x.size() ? x[k] + fr * (x[k + 1] - x[k]) : 0.f; }
            x.swap(y); truth *= speed; speed = 1.f;
        }
        int len = (int)x.size(); if(len > (int)(sr * 25)) len = (int)(sr * 25);
        static int ons[128];
        const int n = gDet.Detect(x.data(), len, sr, ons, 128, nullptr);
        const float bar = bb::EstimateBarSamples(x.data(), len, sr, ons, n);
        const int priorSl = bb::GridSlices(len, sr);
        const float priorBpm = 240.f * sr / ((float)len / (priorSl / 16.f));
        const char* base = strrchr(argv[a], '/'); base = base ? base + 1 : argv[a];
        printf("%-52.52s %5.1fs %3d onsets | prior %2d bars %6.1f BPM | backbeat ", base, len / sr, n, priorSl / 16, priorBpm);
        if(bar > 0.f)
        {
            const float bars = (float)len / bar, bpm = 240.f * sr / bar;
            printf("%4.2f bars %6.1f BPM", bars, bpm);
            if(truth > 0.f) { tried++; bool hit = fabsf(bpm - truth) < 0.03f * truth; ok += hit; printf("  %s (truth %.0f)", hit ? "OK " : "MISS", truth); }
        }
        else { printf("(no estimate → prior)"); if(truth > 0.f) { tried++; printf("  truth %.0f", truth); } }
        putchar('\n');
        truth = 0.f;
    }
    if(tried) printf("\n%d/%d within 3 %% of the stated tempo\n", ok, tried);
    return 0;
}
