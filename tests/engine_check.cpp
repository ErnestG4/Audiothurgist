// engine_check — host test suite for the pure engine (core/). Build + run:
//   tests/run.sh            (sanitized build, plus grammar + classifier checks)
// or by hand:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib \
//       tests/engine_check.cpp -o /tmp/ec && /tmp/ec
//
// Material is synthetic and self-describing: 2.0 s at 48 kHz → 16 grid steps of
// 6000 samples; step k carries a sine at (200 + 50·k) Hz, so the slice that
// actually sounded in any rendered step is read back from its zero-crossing
// rate. Every assertion below is a behaviour the module relies on.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "bb_engine.h"
#include "bb_classify.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static constexpr float kSr    = 48000.f;
static constexpr int   kSteps = 16;
static constexpr int   kStep  = 6000;               // 2.0 s / 16
static constexpr int   kLen   = kSteps * kStep;
static float gMat[kLen];
static uint8_t gCls[bb::Engine::kMaxSlices];

static float StepFreq(int k) { return 200.f + 50.f * (float)k; }

static void BuildMaterial()
{
    for(int k = 0; k < kSteps; k++)
    {
        const float f = StepFreq(k);
        for(int i = 0; i < kStep; i++)
        {
            float t = (float)i / kSr;
            float env = i < 48 ? (float)i / 48.f : 1.f;              // 1 ms attack
            gMat[k * kStep + i] = 0.5f * env * sinf(6.2831853f * f * t);
        }
    }
    // step 15 silent: a class-3 step for the classifier and a rest-at-end case
    for(int i = 0; i < kStep; i++) gMat[15 * kStep + i] = 0.f;
    // classes: kick on beats, snare on 2/4 (steps 4,12), hats elsewhere
    for(int k = 0; k < kSteps; k++) gCls[k] = (k % 8 == 0) ? 0 : (k % 4 == 0 ? 1 : 2);
    gCls[15] = 3;
}

// dominant frequency of a rendered step from its zero-crossing count
static float MeasureFreq(const float* y, int n)
{
    int zc = 0;
    for(int i = 1; i < n; i++) if((y[i - 1] < 0.f) != (y[i] < 0.f)) zc++;
    return (float)zc * kSr / (2.f * (float)n);
}
static float Rms(const float* y, int n)
{
    double s = 0; for(int i = 0; i < n; i++) s += (double)y[i] * y[i];
    return (float)sqrt(s / (n > 0 ? n : 1));
}
// which source slice does this frequency correspond to? (-1 = silence/unknown)
static int SliceOfFreq(float f, float rms)
{
    if(rms < 0.02f) return -1;
    int k = (int)floorf((f - 200.f) / 50.f + 0.5f);
    return (k >= 0 && k < kSteps) ? k : -2;
}

struct Rendered
{
    std::vector<float> y;          // whole render
    std::vector<uint8_t> gates;    // gateFired per block
    int blocks = 0;
};
// Drive the engine like the shell does: extPhase, barPhase written per block.
static Rendered RenderLoops(bb::Engine& e, int loops, int block = 24)
{
    Rendered r;
    e.extPhase = true;
    const double total = (double)loops * e.barSeconds;
    double t = 0.;
    float out[128];
    while(t < total - 1e-9)
    {
        e.barPhase = fmod(t / (double)e.barSeconds, 1.0);
        e.Process(out, block);
        r.y.insert(r.y.end(), out, out + block);
        r.gates.push_back(e.gateFired); e.gateFired = 0;
        r.blocks++;
        t += (double)block / kSr;
    }
    return r;
}
static int GateEvents(const Rendered& r, int cls = -1)
{
    int n = 0;
    for(uint8_t g : r.gates) { if(cls < 0) n += g != 0; else n += (g >> cls) & 1; }
    return n;
}
static void Fresh(bb::Engine& e, int mode, float knob)
{
    e = bb::Engine();
    e.Init(0x1234);
    e.patternMode = mode;
    e.patternKnob = knob;
    e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls);
    e.SetBpm(0.f);
    e.RegenPattern();
}
static int SliceAtStep(const Rendered& r, int step)
{
    const int a = step * kStep + 300, n = kStep - 600;   // skip the attack/edge
    return SliceOfFreq(MeasureFreq(&r.y[a], n), Rms(&r.y[a], n));
}
// "What the map says is what plays": over the second rendered loop, every step
// must sound its stepSrc slice (silence for -1 and for the silent source 15).
static int MapMismatches(const bb::Engine& e, const Rendered& r)
{
    int wrong = 0;
    for(int k = 0; k < 16; k++)
    {
        const int src    = e.stepSrc[k];
        const int expect = (src < 0 || src == 15) ? -1 : src;
        if(SliceAtStep(r, 16 + k) != expect) wrong++;
    }
    return wrong;
}

int main()
{
    BuildMaterial();
    bb::Engine e;

    SECTION("SetMaterial: grid rule and native tempo");
    {
        Fresh(e, 0, 1.f);
        CHECK(e.nSlices == 16, "2.0 s → 16 slices, got %d", e.nSlices);
        CHECK(fabsf(e.NativeBpm() - 120.f) < 0.01f, "native BPM 120, got %.2f", e.NativeBpm());
        CHECK(fabsf(e.barSeconds - 2.f) < 1e-4f, "barSeconds 2.0, got %.4f", e.barSeconds);
        static float longBuf[48000 * 9];
        for(int i = 0; i < 48000 * 9; i++) longBuf[i] = 0.1f * sinf(0.01f * i);
        bb::Engine e2; e2.Init(1); e2.SetMaterial(longBuf, 48000 * 9, kSr, nullptr, 0, nullptr);
        CHECK(e2.nSlices == 64, "9 s → 4 bars → 64 slices, got %d", e2.nSlices);
        bb::Engine e3; e3.Init(1); e3.SetMaterial(longBuf, 48000 / 2, kSr, nullptr, 0, nullptr);
        CHECK(e3.nSlices == 16, "0.5 s → 16 slices, got %d", e3.nSlices);
    }

    SECTION("Pattern::Generate: coverage, ranges, cleared arrays, determinism");
    {
        bb::Pattern p;
        uint8_t cls[bb::Engine::kMaxSlices];
        for(int i = 0; i < 64; i++) cls[i] = (uint8_t)(i % 3);
        int bad = 0, cover = 0, stale = 0, nondet = 0;
        for(int mode = 0; mode < 4; mode++)
            for(int ns : {16, 32, 64})
                for(int ki = 0; ki <= 4; ki++)
                    for(uint64_t seed = 1; seed < 40; seed++)
                    {
                        p.langX = 0.3f; p.langY = 0.7f; p.langDens[0] = 0.4f; p.langDens[1] = 0.5f; p.langDens[2] = 0.8f; p.langRot = 0.25f;
                        p.Generate(ns, 4, cls, ki / 4.f, seed, seed * 7 + 1, mode);
                        int8_t src1[64]; memcpy(src1, p.src, 64);
                        // tiling: cells cover [0, ns) exactly
                        int c = 0;
                        while(c < ns) { int L = p.len[c] > 0 ? p.len[c] : 1; if(p.src[c] == -1 && mode == 0) { cover++; break; } if(p.src[c] < -2 || p.src[c] >= ns) bad++; c += L; }
                        if(c != ns) cover++;
                        if(mode >= 2)
                            for(int s = 0; s < ns; s++) if(p.rev[s] || p.ratN[s] || p.ratAccel[s] || p.rep[s]) stale++;
                        p.Generate(ns, 4, cls, ki / 4.f, seed, seed * 7 + 1, mode);
                        if(memcmp(src1, p.src, 64)) nondet++;
                    }
        CHECK(bad == 0, "source index out of range ×%d", bad);
        CHECK(cover == 0, "tiling coverage errors ×%d", cover);
        CHECK(stale == 0, "language modes must clear rev/ratN/rep (×%d stale)", stale);
        CHECK(nondet == 0, "Generate must be deterministic in its inputs (×%d)", nondet);
    }

    SECTION("Classic verbatim: identity, continuity, one gate per step");
    {
        Fresh(e, 0, 1.f);
        Rendered r = RenderLoops(e, 2);
        int wrong = 0;
        for(int k = 0; k < 15; k++) if(SliceAtStep(r, 16 + k) != k) wrong++;   // second loop
        CHECK(wrong == 0, "verbatim: %d steps played the wrong slice", wrong);
        CHECK(e.trigFlash <= 4, "continuity: verbatim run re-spliced %u times over 2 loops (expect ≤4)", e.trigFlash);
        CHECK(GateEvents(r) == 32, "one gate event per step over 2 loops, got %d", GateEvents(r));
        CHECK(GateEvents(r, 0) == 4 && GateEvents(r, 1) == 4, "class lanes follow the classes (kick %d snare %d)", GateEvents(r, 0), GateEvents(r, 1));
        bool finite = true; for(float v : r.y) if(!std::isfinite(v)) finite = false;
        CHECK(finite, "output is finite");
    }

    SECTION("Rests: Euclid kick-only, gated silence vs continuation");
    {
        // kick density 4/16 → E(4,16) fires four steps; everything else rests
        for(int sustain = 0; sustain < 2; sustain++)
        {
            Fresh(e, 3, 1.f);
            e.restSustain = sustain != 0;
            e.langDens[0] = 4.f / 16.f; e.langDens[1] = 0.f; e.langDens[2] = 0.f; e.langRot = 0.f;
            e.RegenPattern();
            int hits = 0, rests = 0, conts = 0, chain = 0;
            for(int s = 0; s < 16; s++) { if(e.stepCont[s]) conts++; else if(e.stepSrc[s] >= 0) hits++; else rests++; }
            for(int s = 1; s < 16; s++) if(e.stepCont[s] && e.stepSrc[s] == (e.stepSrc[s - 1] + 1) % 16) chain++;
            CHECK(hits == 4, "sustain=%d: 4 hit steps, got %d", sustain, hits);
            if(!sustain) CHECK(rests == 12, "gated: 12 rest steps, got %d", rests);
            else         CHECK(conts == 12 && rests == 0 && chain >= 11, "sustain: 12 continuation steps chaining the source (%d cont, %d rests, %d chained)", conts, rests, chain);
            Rendered r = RenderLoops(e, 2);
            const int wrong = MapMismatches(e, r);
            CHECK(wrong == 0, "sustain=%d: what the map says is what plays (%d wrong)", sustain, wrong);
            CHECK(GateEvents(r) == 8, "sustain=%d: only HITS raise gates: 8 over 2 loops, got %d", sustain, GateEvents(r));
            CHECK(GateEvents(r, 0) == 8, "…and on the kick lane (%d)", GateEvents(r, 0));
        }
    }

    SECTION("Grids: densities at zero = plain break under sustain; the map plays");
    {
        Fresh(e, 2, 1.f);
        e.restSustain = true;
        e.langDens[0] = e.langDens[1] = e.langDens[2] = 0.f;
        e.RegenPattern();
        int inPlace = 0; for(int s = 0; s < 16; s++) inPlace += e.stepCont[s] && e.stepSrc[s] == s;
        CHECK(inPlace == 16, "all-rest pattern = the plain break in place (%d/16)", inPlace);
        Rendered r = RenderLoops(e, 2);
        CHECK(GateEvents(r) == 0, "no hits → no gates (%d)", GateEvents(r));
        CHECK(MapMismatches(e, r) == 0, "plain break plays verbatim");
        CHECK(e.trigFlash <= 4, "…as one voice per loop (%u triggers)", e.trigFlash);
        e.langDens[0] = e.langDens[1] = e.langDens[2] = 1.f;
        e.RegenPattern();
        int hits = 0; for(int s = 0; s < 16; s++) hits += !e.stepCont[s] && e.stepSrc[s] >= 0;
        CHECK(hits >= 6, "full densities fire the map's nonzero steps (%d/16)", hits);
        r = RenderLoops(e, 2);
        CHECK(MapMismatches(e, r) == 0, "Grids map plays through the break's own slices");
        CHECK(GateEvents(r) == 2 * hits, "one gate per hit (%d events, %d hits/loop)", GateEvents(r), hits);
    }

    SECTION("Mute and Voice calls");
    {
        Fresh(e, 0, 1.f);
        e.muted = true;
        Rendered r = RenderLoops(e, 1);
        CHECK(Rms(r.y.data(), (int)r.y.size()) < 1e-4f, "muted: pattern silent");
        CHECK(GateEvents(r) == 0, "muted: no gates");
        e.CallSlice(3, 1.f, false, false, false);
        float out[24]; float peak = 0.f;
        e.barPhase = 0.4;
        for(int b = 0; b < 200; b++) { e.Process(out, 24); for(float v : out) if(fabsf(v) > peak) peak = fabsf(v); }
        CHECK(peak > 0.1f, "a call sounds while muted (peak %.3f)", peak);
        // punch writes the bar at the current step
        Fresh(e, 0, 1.f);
        RenderLoops(e, 1);
        e.barPhase = 0.5; float o[24]; e.Process(o, 24);          // curStep = 8
        e.CallSlice(2, 1.f, false, true, false);
        CHECK(e.stepSrc[8] == 2 && !e.stepCont[8], "punch wrote slice 2 at step 8 (src %d)", e.stepSrc[8]);
        CHECK(e.callCountdown == -1, "no ratchet requested → no pending call");
    }

    SECTION("Collins: fill toggle keeps punches outside the fill region");
    {
        bb::Engine ec; ec = bb::Engine(); ec.Init(0x77);
        static float four[48000 * 8];
        for(int i = 0; i < 48000 * 8; i++) four[i] = 0.3f * sinf(0.02f * i);
        uint8_t c64[64]; for(int i = 0; i < 64; i++) c64[i] = (uint8_t)(i % 3);
        ec.patternMode = 1; ec.patternKnob = 0.5f;
        ec.SetMaterial(four, 48000 * 8, kSr, nullptr, 0, c64);
        ec.SetBpm(0.f); ec.RegenPattern();
        CHECK(ec.nSlices == 64 && ec.pat.fillStart == 48, "4-bar loop: fill region = last bar (fillStart %d)", ec.pat.fillStart);
        RenderLoops(ec, 1);
        ec.barPhase = 5.5 / 64.; float o[24]; ec.Process(o, 24);   // curStep = 5 (bar 1)
        ec.CallSlice(9, 1.f, false, true, false);
        CHECK(ec.stepSrc[5] == 9, "punched step 5 ← slice 9");
        RenderLoops(ec, 9);                                       // through two fill toggles
        CHECK(ec.loopCount >= 8, "ran %d loops", ec.loopCount);
        CHECK(ec.stepSrc[5] == 9, "punch survived the fill toggles (step 5 = %d)", ec.stepSrc[5]);
    }

    SECTION("History: reslice, scrub back restores the previous pattern");
    {
        Fresh(e, 0, 0.4f);
        int8_t a[64]; memcpy(a, e.stepSrc, 64);
        e.Reslice();
        int8_t b[64]; memcpy(b, e.stepSrc, 64);
        CHECK(memcmp(a, b, 64) != 0, "reslice changes the pattern");
        e.Reslice();
        CHECK(e.histN == 3, "history holds boot + 2 rolls (%d)", e.histN);
        int depth = e.Scrub(-1);
        CHECK(depth == 1 && memcmp(e.stepSrc, b, 64) == 0, "scrub -1 → previous pattern (depth %d)", depth);
        depth = e.Scrub(-1);
        CHECK(depth == 2 && memcmp(e.stepSrc, a, 64) == 0, "scrub -1 again → the boot pattern (depth %d)", depth);
        depth = e.Scrub(+1); depth = e.Scrub(+1); depth = e.Scrub(+1);
        CHECK(depth == 0, "scrub clamps at the head (depth %d)", depth);
    }

    SECTION("Ratchets: rolls zone survives mid-loop regeneration (no negative reads)");
    {
        Fresh(e, 0, 0.3f);
        int rolls = 0; for(int s = 0; s < 16; s++) rolls += e.stepRatN[s] > 0;
        CHECK(rolls > 0, "the rolls zone sets ratchets (%d)", rolls);
        bool finite = true;
        for(int i = 0; i < 40; i++)
        {
            Rendered r = RenderLoops(e, 1);
            for(float v : r.y) if(!std::isfinite(v)) finite = false;
            e.barPhase = (i % 16) / 16. + 0.01; float o[24]; e.Process(o, 24);
            if(i & 1) e.Reslice(); else e.RerollFill();
            CHECK(e.ratchetCountdown == -1, "regeneration drops the pending ratchet");
        }
        CHECK(finite, "output finite through 40 regenerations mid-step");
        e.patternMode = 1; e.patternKnob = 0.2f; e.RegenPattern();
        Rendered r = RenderLoops(e, 4);
        for(float v : r.y) if(!std::isfinite(v)) finite = false;
        CHECK(finite, "Collins rolls: finite output");
    }

    SECTION("Classifier lenses");
    {
        uint8_t cls[64]; float sal[64], dens[64];
        for(int m = 0; m < 4; m++)
        {
            bb::ClassifySteps(gMat, kLen, kSr, cls, m, sal, dens);
            int bad = 0; for(int s = 0; s < 16; s++) bad += cls[s] > 3;
            CHECK(bad == 0, "mode %d: classes in 0..3", m);
            CHECK(cls[15] == 3, "mode %d: the silent step is class 3 (got %d)", m, cls[15]);
            CHECK(sal[15] < 1e-3f && sal[0] > 0.5f, "mode %d: salience 0 on silence, high on tone", m);
        }
        int nK = 0, nS = 0, nH = 0; for(int s = 0; s < 15; s++) { nK += cls[s] == 0; nS += cls[s] == 1; nH += cls[s] == 2; }
        CHECK(nK >= 3 && nH >= 5, "density lens splits by rank (K%d S%d H%d of 15)", nK, nS, nH);
        // rising frequency per step → density rises with the step index
        int mono = 0; for(int s = 1; s < 15; s++) mono += dens[s] >= dens[s - 1];
        CHECK(mono >= 12, "keyframe density tracks bandwidth (%d/14 rising)", mono);
    }

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
