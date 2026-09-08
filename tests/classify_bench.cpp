// classify_bench — every classifier lens against a hit-level ground truth,
// through the firmware's own load pipeline (condition → onset table → bar
// estimate → slice count, as src/audiothurgist.cpp ServiceLoad does), on the
// grid the engine ends up with. Truth = a CSV of (file, t, cls, act, midi)
// hits: tests/neural/adt_corpus.py (ADTOF, a neural transcriber) or
// tests/neural/idmt_truth.py (IDMT-SMT-Drums, hand-annotated).
//   g++ -O2 -std=gnu++17 -Icore -Ivendor/capicola/lib tests/classify_bench.cpp -o /tmp/cb
//   /tmp/cb --truth tests/neural/adt.csv --root analysis/breaks [--sample 200 --seed 1]
//           [--list tests/bars_truth.txt] [--onsets 0|1] [--verbose] [--strongest] [--modes 0,4]
// A step's truth class is the highest-priority hit inside it (kick > snare >
// hat; --strongest: the hit with the highest activation instead), silent if
// none. Hits within 10 ms before a step boundary belong to the next step
// (the engine's own hit rule). Prints, per lens, a 4×4 confusion, per-class
// precision / recall / F1 over kick / snare / hat and the overall accuracy
// over every step, plus the accuracy over steps both call sounding.
// tests/classify_bench.sh runs the standard tables.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "wav_read.h"
#include "bb_engine.h"
#include "bb_classify.h"
#include "bb_onset.h"
#include "bb_bars.h"
#include "bb_condition.h"

static bb::OnsetDetector gDet;

struct Hit { float t; int cls; float act; };
struct Conf
{
    long c[4][4] = {};   /* [truth][lens] */
    long files = 0, steps = 0;
    void Add(int t, int l) { c[t][l]++; steps++; }
};
static const char* kNames[5] = {"Hockman", "Legacy", "Two-band", "Density", "Adaptive"};

static void Report(const char* name, const Conf& k)
{
    printf("\n%-9s %ld files, %ld steps            lens: K      S      H      .\n", name, k.files, k.steps);
    for(int t = 0; t < 4; t++)
        printf("  truth %c %6ld %6ld %6ld %6ld\n", "KSH."[t], k.c[t][0], k.c[t][1], k.c[t][2], k.c[t][3]);
    long ok = 0, sound = 0, soundOk = 0;
    for(int t = 0; t < 4; t++) ok += k.c[t][t];
    for(int t = 0; t < 3; t++) for(int l = 0; l < 3; l++) { sound += k.c[t][l]; soundOk += (t == l) ? k.c[t][l] : 0; }
    float f1sum = 0.f;
    for(int c = 0; c < 3; c++)
    {
        long tp = k.c[c][c], fp = 0, fn = 0;
        for(int i = 0; i < 4; i++) { if(i != c) { fp += k.c[i][c]; fn += k.c[c][i]; } }
        const float p = tp + fp ? (float)tp / (float)(tp + fp) : 0.f, r = tp + fn ? (float)tp / (float)(tp + fn) : 0.f;
        const float f1 = p + r > 0.f ? 2.f * p * r / (p + r) : 0.f;
        f1sum += f1;
        printf("  %c  precision %5.1f%%  recall %5.1f%%  F1 %5.1f%%\n", "KSH"[c], 100.f * p, 100.f * r, 100.f * f1);
    }
    printf("  accuracy %5.1f%% of all steps · %5.1f%% of the %ld steps both call sounding · macro-F1 %5.1f%%\n",
           k.steps ? 100.f * (float)ok / (float)k.steps : 0.f, sound ? 100.f * (float)soundOk / (float)sound : 0.f, sound, 100.f * f1sum / 3.f);
}

int main(int argc, char** argv)
{
    std::string truthFile, root, listFile;
    int  sample = 0, seed = 1, onsetMode = 0;
    bool verbose = false, strongest = false;
    float leadMs = 10.f;   /* a truth hit this close before a step boundary belongs to the next step */
    FILE* dump = nullptr;   /* --dump feat.csv: per step, the Adaptive lens's features + truth */
    bool modes[5] = {true, true, true, true, true};
    for(int a = 1; a < argc; a++)
    {
        if(!strcmp(argv[a], "--truth") && a + 1 < argc)  { truthFile = argv[++a]; continue; }
        if(!strcmp(argv[a], "--root") && a + 1 < argc)   { root = argv[++a]; continue; }
        if(!strcmp(argv[a], "--list") && a + 1 < argc)   { listFile = argv[++a]; continue; }
        if(!strcmp(argv[a], "--sample") && a + 1 < argc) { sample = atoi(argv[++a]); continue; }
        if(!strcmp(argv[a], "--seed") && a + 1 < argc)   { seed = atoi(argv[++a]); continue; }
        if(!strcmp(argv[a], "--onsets") && a + 1 < argc) { onsetMode = atoi(argv[++a]); continue; }
        if(!strcmp(argv[a], "--verbose"))                { verbose = true; continue; }
        if(!strcmp(argv[a], "--strongest"))              { strongest = true; continue; }
        if(!strcmp(argv[a], "--early") && a + 1 < argc)  { bb::gAdaptEarlyMs = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--full") && a + 1 < argc)   { bb::gAdaptFullMs = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--iters") && a + 1 < argc)  { bb::gAdaptIters = atoi(argv[++a]); continue; }
        if(!strcmp(argv[a], "--rise") && a + 1 < argc)   { bb::gAdaptRise = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--shrink") && a + 1 < argc) { bb::gAdaptShrink = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--pre") && a + 1 < argc)    { bb::gAdaptPreMs = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--bleed") && a + 1 < argc)  { bb::gAdaptBleed = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--lead") && a + 1 < argc)   { leadMs = (float)atof(argv[++a]); continue; }
        if(!strcmp(argv[a], "--w") && a + 1 < argc)      { int j = 0; for(char* p = strtok(argv[++a], ","); p && j < bb::kAdaptNF; p = strtok(nullptr, ","), j++) bb::gAdaptW[j] = (float)atof(p); continue; }
        if(!strcmp(argv[a], "--dump") && a + 1 < argc)   { dump = fopen(argv[++a], "w"); if(dump) { fprintf(dump, "file,step,truth,hit,hock,adapt,lateok"); for(int j = 0; j < bb::kAdaptNF; j++) fprintf(dump, ",f%d", j); fputc('\n', dump); } continue; }
        if(!strcmp(argv[a], "--modes") && a + 1 < argc)
        {
            for(int m = 0; m < 5; m++) modes[m] = false;
            for(const char* p = argv[++a]; *p; p++) if(*p >= '0' && *p <= '4') modes[*p - '0'] = true;
            continue;
        }
        printf("unknown argument %s\n", argv[a]); return 1;
    }
    if(truthFile.empty()) { printf("--truth <csv> required\n"); return 1; }

    /* truth: file → hits */
    std::map<std::string, std::vector<Hit>> truth;
    {
        FILE* f = fopen(truthFile.c_str(), "r");
        if(!f) { printf("cannot read %s\n", truthFile.c_str()); return 1; }
        char line[1024];
        while(fgets(line, sizeof line, f))
        {
            /* file,t,cls,act,midi — the file name may hold commas only if quoted; the corpus has none */
            char* nl = strchr(line, '\n'); if(nl) *nl = 0;
            char* c1 = strrchr(line, ','); if(!c1) continue; *c1 = 0;
            char* c2 = strrchr(line, ','); if(!c2) continue; *c2 = 0;
            char* c3 = strrchr(line, ','); if(!c3) continue; *c3 = 0;
            char* c4 = strrchr(line, ','); if(!c4) continue; *c4 = 0;
            if(!strcmp(line, "file")) continue;
            truth[line].push_back({(float)atof(c4 + 1), atoi(c3 + 1), (float)atof(c2 + 1)});
        }
        fclose(f);
    }
    /* which files: --list (bars_truth format: bpm<TAB>file[<TAB>bpm]), else every file with truth, --sample N of them */
    std::vector<std::string> files;
    if(!listFile.empty())
    {
        FILE* f = fopen(listFile.c_str(), "r");
        if(!f) { printf("cannot read %s\n", listFile.c_str()); return 1; }
        char line[512];
        while(fgets(line, sizeof line, f))
        {
            if(line[0] == '#' || line[0] == '\n') continue;
            char* nl = strchr(line, '\n'); if(nl) *nl = 0;
            char* tab = strchr(line, '\t');
            char* name = tab ? tab + 1 : line;
            char* tab2 = strchr(name, '\t'); if(tab2) *tab2 = 0;
            files.push_back(name);
        }
        fclose(f);
    }
    else
    {
        for(auto& kv : truth) files.push_back(kv.first);
        if(sample > 0 && sample < (int)files.size())
        {
            /* deterministic sample: LCG shuffle of the sorted list */
            uint32_t s = (uint32_t)seed * 2654435761u + 12345u;
            for(int i = (int)files.size() - 1; i > 0; i--)
            {
                s = s * 1664525u + 1013904223u;
                std::swap(files[i], files[(s >> 8) % (uint32_t)(i + 1)]);
            }
            files.resize(sample);
            std::sort(files.begin(), files.end());
        }
    }

    Conf conf[5], confHit[5], confNoHit[5];   /* all steps; steps that open with a detected hit; the rest */
    int  noTruth = 0, unreadable = 0;
    long truthHits = 0, hitsInSteps = 0, phase[16] = {0};
    for(const std::string& name : files)
    {
        auto it = truth.find(name);
        if(it == truth.end()) { noTruth++; continue; }
        std::string path = root.empty() ? name : root + "/" + name;
        std::vector<float> x; float sr;
        if(!LoadWav(path.c_str(), x, sr)) { unreadable++; continue; }
        int len = (int)x.size(); if(len > (int)(sr * 25)) len = (int)(sr * 25);
        x.resize((size_t)len);

        /* firmware pipeline, as bars_check / ServiceLoad */
        gDet.mode = onsetMode;
        bb::ConditionInfo ci;
        len = bb::ConditionBuffer(x.data(), len, sr, gDet, &ci);
        static int ons[128];
        const int n = gDet.Detect(x.data(), len, sr, ons, 128, nullptr);
        const bb::BarEstimate est = bb::EstimateBar(x.data(), len, sr, ons, n);
        int bars = ci.bars;
        if(bars == 0 && est.bar > 0.f)
        {
            const float b = (float)len / est.bar;
            for(int p = 1; p <= 16; p <<= 1) if(fabsf(b - (float)p) < 0.06f * (float)p) bars = p;
        }
        int nsl = bars > 0 ? bars * 16 : bb::GridSlices(len, sr);
        while(nsl > bb::Engine::kMaxSlices) nsl /= 2;
        const int step = len / nsl;
        if(step < 1) continue;

        /* step truth */
        uint8_t tcls[bb::Engine::kMaxSlices];
        float   tact[bb::Engine::kMaxSlices];
        for(int s = 0; s < nsl; s++) { tcls[s] = 3; tact[s] = 0.f; }
        const float t0 = (float)ci.trimmedStart / sr, lead = leadMs * 0.001f;
        for(const Hit& h : it->second)
        {
            truthHits++;
            const float tb = h.t - t0 + lead;   /* buffer time, shifted so 10 ms early still counts for the next step */
            if(tb < 0.f) continue;
            int s = (int)(tb * sr) / step;
            if(s >= nsl) { if(tb * sr < (float)len + lead * sr) s = nsl - 1; else continue; }
            hitsInSteps++;
            { int ph = (int)(((tb * sr) - (float)(s * step)) * 16.f / (float)step); phase[ph < 0 ? 0 : (ph > 15 ? 15 : ph)]++; }
            const bool better = strongest ? (tcls[s] == 3 || h.act > tact[s]) : (h.cls < tcls[s]);
            if(better) { tcls[s] = (uint8_t)h.cls; tact[s] = h.act; }
        }

        static uint8_t cls[5][bb::Engine::kMaxSlices];
        for(int m = 0; m < 5; m++)
        {
            if(!modes[m]) continue;
            bb::ClassifySteps(x.data(), len, sr, cls[m], m, nullptr, nullptr, nsl, nullptr, ons, n);
            conf[m].files++;
            for(int s = 0; s < nsl; s++) conf[m].Add(tcls[s], cls[m][s]);
        }
        /* the Adaptive lens's hit placement (valid after it ran, whatever the mode list: run it if needed) */
        if(!modes[4]) { static uint8_t tmp[bb::Engine::kMaxSlices]; bb::ClassifySteps(x.data(), len, sr, tmp, 4, nullptr, nullptr, nsl, nullptr, ons, n); }
        for(int m = 0; m < 5; m++)
        {
            if(!modes[m]) continue;
            for(int s = 0; s < nsl; s++) (bb::gAdaptHit[s] ? confHit[m] : confNoHit[m]).Add(tcls[s], cls[m][s]);
        }
        if(dump && modes[4])
            for(int s = 0; s < nsl; s++)
            {
                if(cls[4][s] == 3) continue;
                fprintf(dump, "%s,%d,%d,%d,%d,%d,%d", name.c_str(), s, tcls[s], bb::gAdaptHit[s] ? 1 : 0, cls[0][s], cls[4][s], bb::gAdaptLateOk[s] ? 1 : 0);
                for(int j = 0; j < bb::kAdaptNF; j++) fprintf(dump, ",%.4f", bb::gAdaptFeat[s][j]);
                fputc('\n', dump);
            }
        if(verbose)
        {
            printf("\n%s  (%.1f s, %d steps, %d onsets, %s%d bars)\n", name.c_str(), len / sr, nsl, n, bars > 0 ? "" : "prior ", nsl / 16);
            auto line = [&](const char* label, const uint8_t* c) {
                printf("  %-9s ", label);
                for(int s = 0; s < nsl; s++) { putchar("KSH."[c[s]]); if((s & 15) == 15 && s != nsl - 1) putchar('|'); }
                putchar('\n');
            };
            line("truth", tcls);
            for(int m = 0; m < 5; m++) if(modes[m]) { line(kNames[m], cls[m]); }
        }
    }
    printf("%d files listed, %d without truth, %d unreadable; %ld truth hits, %ld inside the analysed steps\n",
           (int)files.size(), noTruth, unreadable, truthHits, hitsInSteps);
    printf("truth-hit position inside its step (16ths of a step, 10 ms lead applied):");
    for(int i = 0; i < 16; i++) printf(" %ld", phase[i]);
    printf("\n");
    for(int m = 0; m < 5; m++) if(modes[m]) Report(kNames[m], conf[m]);
    printf("\nOn steps with a truth hit, accuracy where the engine found an onset / where it did not (the lens judges those from the step start):\n");
    for(int m = 0; m < 5; m++)
    {
        if(!modes[m]) continue;
        long h = 0, hOk = 0, q = 0, qOk = 0;
        for(int t = 0; t < 3; t++) for(int l = 0; l < 4; l++) { h += confHit[m].c[t][l]; q += confNoHit[m].c[t][l]; if(t == l) { hOk += confHit[m].c[t][l]; qOk += confNoHit[m].c[t][l]; } }
        printf("  %-9s hit steps %5.1f%% of %ld · no-hit steps %5.1f%% of %ld\n", kNames[m], h ? 100.f * hOk / h : 0.f, h, q ? 100.f * qOk / q : 0.f, q);
    }
    if(dump) fclose(dump);
    return 0;
}
