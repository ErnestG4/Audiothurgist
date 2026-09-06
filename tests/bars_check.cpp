// bars_check — bar/tempo estimate on real breaks, through the firmware's own
// load pipeline: condition (trim/normalize/whole bars) → onset table → bar
// estimate → slice count, exactly as src/audiothurgist.cpp ServiceLoad does.
// Reports the duration prior, the v0.8.0 backbeat estimate, and the SLICE
// package's IOI-cluster estimate against a stated truth.
//   g++ -O2 -std=gnu++17 -Icore -Ivendor/capicola/lib tests/bars_check.cpp -o /tmp/bc
//   /tmp/bc [--onsets 0|1] [--speed S] [--bpm N] break.wav [...]
//   /tmp/bc --truth tests/bars_truth.txt --root analysis/breaks [--speed S]
//   /tmp/bc --root analysis/breaks --list files.txt      (no truth: grid vs prior)
// Flags before a file apply to it: --bpm states its truth, --speed resamples
// it (varispeed: tempo × S). --truth reads "<bpm>\t<file>" lines (# comments)
// and runs every file under --root. --quiet prints the summary only.
// Exit status 0; the summary line is what tests/bars_corpus.sh reads.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "wav_read.h"
#include "bb_engine.h"
#include "bb_classify.h"
#include "bb_onset.h"
#include "bb_bars.h"
#include "bb_condition.h"
#include "bb_swing.h"

static bb::OnsetDetector gDet;

struct Tally
{
    int tried = 0, priorOk = 0, newOk = 0, newSpoke = 0, newWrong = 0, newOctave = 0;
};

static void RunOne(const char* path, float truth, float speed, int onsetMode, bool quiet, Tally& t)
{
    std::vector<float> x; float sr;
    if(!LoadWav(path, x, sr)) { printf("%s: cannot read\n", path); return; }
    if(speed != 1.f)
    {
        std::vector<float> y((size_t)(x.size() / speed));
        for(size_t i = 0; i < y.size(); i++) { double p = i * (double)speed; size_t k = (size_t)p; float fr = (float)(p - k); y[i] = k + 1 < x.size() ? x[k] + fr * (x[k + 1] - x[k]) : 0.f; }
        x.swap(y); truth *= speed;
    }
    int len = (int)x.size(); if(len > (int)(sr * 25)) len = (int)(sr * 25);   /* kMaxSec */
    x.resize((size_t)len);

    /* firmware pipeline */
    gDet.mode = onsetMode;
    bb::ConditionInfo ci;
    len = bb::ConditionBuffer(x.data(), len, sr, gDet, &ci);
    static int ons[128];
    const int n = gDet.Detect(x.data(), len, sr, ons, 128, nullptr);
    const bb::BarEstimate est = bb::EstimateBar(x.data(), len, sr, ons, n);
    const int   priorSl  = bb::GridSlices(len, sr);
    const float priorBpm = 240.f * sr / ((float)len / (priorSl / 16.f));
    /* the shell's rule (SliceCountFor): the conditioning's verified bar
     * count; else a confident estimate whose bar count for this buffer is a
     * power of two within 6 %; else the prior */
    int bars = ci.bars;
    if(bars == 0 && est.bar > 0.f)
    {
        const float b = (float)len / est.bar;
        for(int p = 1; p <= 16; p <<= 1) if(fabsf(b - (float)p) < 0.06f * (float)p) bars = p;
    }
    int nsl = bars > 0 ? bars * 16 : priorSl;
    while(nsl > bb::Engine::kMaxSlices) nsl /= 2;
    const float gridBpm = 240.f * sr / ((float)len / (nsl / 16.f));
    const float estBpm  = est.bar > 0.f ? est.bpm : (ci.bar > 0.f ? 240.f * sr / ci.bar : 0.f);
    const bb::SwingEstimate sw = bb::EstimateSwing(ons, n, (float)len / (float)nsl);

    const char* base = strrchr(path, '/'); base = base ? base + 1 : path;
    const bool  spoke = bars > 0;
    if(truth > 0.f)
    {
        t.tried++;
        const bool pOk = fabsf(priorBpm - truth) < 0.04f * truth;
        const bool nOk = fabsf(gridBpm - truth) < 0.04f * truth;
        t.priorOk += pOk; t.newOk += nOk; t.newSpoke += spoke;
        if(spoke && !nOk)
        {
            t.newWrong++;
            const float r = gridBpm / truth;
            if(fabsf(r - 2.f) < 0.08f || fabsf(r - 0.5f) < 0.02f) t.newOctave++;
        }
        if(!quiet)
            printf("%-44.44s %5.1fs %3d on | prior %5.1f %s | est %5.1f grid %5.1f %s %s | truth %5.1f | conf %.2f fit %.2f swing %.2f\n",
                   base, len / sr, n, priorBpm, pOk ? "OK  " : "miss", estBpm, gridBpm,
                   spoke ? (est.bar > 0.f ? "est " : "cond") : "prior", nOk ? "OK  " : "MISS", truth, est.conf, est.fit, sw.ratio);
    }
    else if(!quiet)
        printf("%-44.44s %5.1fs %3d on | prior %2d bars %5.1f BPM | est %5.1f grid %5.1f BPM (%d bars, %s) conf %.2f fit %.2f swing %.2f\n",
               base, len / sr, n, priorSl / 16, priorBpm, estBpm, gridBpm, nsl / 16, spoke ? "est" : "prior", est.conf, est.fit, sw.ratio);
}

int main(int argc, char** argv)
{
    float truth = 0.f, speed = 1.f;
    int   onsetMode = 0;
    bool  quiet = false;
    std::string root, truthFile;
    Tally t;
    for(int a = 1; a < argc; a++)
    {
        if(!strcmp(argv[a], "--bpm") && a + 1 < argc)    { truth = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--speed") && a + 1 < argc)  { speed = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--onsets") && a + 1 < argc) { onsetMode = atoi(argv[++a]); continue; }
        if(!strcmp(argv[a], "--root") && a + 1 < argc)   { root = argv[++a]; continue; }
        if(!strcmp(argv[a], "--truth") && a + 1 < argc)  { truthFile = argv[++a]; continue; }
        if(!strcmp(argv[a], "--quiet"))                  { quiet = true; continue; }
        if(!strcmp(argv[a], "--list") && a + 1 < argc)   /* one path per line, no truth */
        {
            FILE* f = fopen(argv[++a], "r");
            if(!f) { printf("cannot read %s\n", argv[a]); return 1; }
            char line[512];
            while(fgets(line, sizeof line, f))
            {
                char* nl = strchr(line, '\n'); if(nl) *nl = 0;
                if(!line[0] || line[0] == '#') continue;
                std::string p = root.empty() ? std::string(line) : root + "/" + line;
                RunOne(p.c_str(), 0.f, speed, onsetMode, quiet, t);
            }
            fclose(f);
            continue;
        }
        RunOne(argv[a], truth, speed, onsetMode, quiet, t);
        truth = 0.f;
    }
    if(!truthFile.empty())
    {
        FILE* f = fopen(truthFile.c_str(), "r");
        if(!f) { printf("cannot read %s\n", truthFile.c_str()); return 1; }
        char line[512];
        while(fgets(line, sizeof line, f))
        {
            if(line[0] == '#' || line[0] == '\n') continue;
            char* tab = strchr(line, '\t'); if(!tab) continue;
            *tab = 0;
            char* name = tab + 1; char* nl = strchr(name, '\n'); if(nl) *nl = 0;
            /* second tab-separated column (rip-adjusted BPM) wins when present */
            float bpm = (float)atof(line);
            char* tab2 = strchr(name, '\t');
            if(tab2) { *tab2 = 0; float b2 = (float)atof(tab2 + 1); if(b2 > 0.f) bpm = b2; }
            std::string p = root.empty() ? std::string(name) : root + "/" + name;
            RunOne(p.c_str(), bpm, speed, onsetMode, quiet, t);
        }
        fclose(f);
    }
    if(t.tried)
        printf("\nSUMMARY speed %.2f onsets %d: %d files | prior %d ok | new %d ok, spoke %d, wrong-confident %d (octave %d)\n",
               speed, onsetMode, t.tried, t.priorOk, t.newOk, t.newSpoke, t.newWrong, t.newOctave);
    return 0;
}
