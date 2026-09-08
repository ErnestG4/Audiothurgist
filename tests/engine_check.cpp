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
#include "bb_bars.h"
#include "bb_gates.h"
#include "bb_onset.h"
#include "bb_condition.h"
#include "bb_swing.h"
#include "bb_testbreak.h"
#include "bb_sample.h"     // SAMPLE: TrimRegion, AnalyseRegion

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
    std::vector<uint8_t> accents;  // GATES: lanes fired WITH a grammar accent, per block
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
        uint8_t acc = 0;
        for(int c = 0; c < 4; c++) if((e.gateFired >> c) & 1 && e.gateAccent[c]) acc |= (uint8_t)(1u << c);
        r.accents.push_back(acc);
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

    SECTION("Slice-count hint (Bars option) and onsets-only gates");
    {
        Fresh(e, 0, 1.f);
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls, 32);   // "2 bars" override on 2.0 s
        CHECK(e.nSlices == 32, "hint 32 → 32 slices (got %d)", e.nSlices);
        CHECK(fabsf(e.NativeBpm() - 240.f) < 0.01f, "…and native BPM doubles to 240 (got %.1f)", e.NativeBpm());
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls, 256);
        CHECK(e.nSlices == 64, "an oversize hint halves to the cap (got %d)", e.nSlices);
        // onsets at the starts of steps 0,4,8,12 only → those are the hits
        static const int on[4] = {0, 4 * kStep + 30, 8 * kStep, 12 * kStep - 200};
        Fresh(e, 0, 1.f);
        e.SetMaterial(gMat, kLen, kSr, on, 4, gCls, 0);
        e.RegenPattern();
        int hits = 0; for(int s = 0; s < 16; s++) hits += e.stepHit[s];
        CHECK(hits == 4 && e.stepHit[0] && e.stepHit[4] && e.stepHit[8] && e.stepHit[12], "onsets mark 4 hit slices (%d)", hits);
        e.gateOnsetsOnly = false;
        Rendered r = RenderLoops(e, 2);
        CHECK(GateEvents(r) == 32, "every-step mode: 32 gate events (%d)", GateEvents(r));
        Fresh(e, 0, 1.f);
        e.SetMaterial(gMat, kLen, kSr, on, 4, gCls, 0);
        e.RegenPattern();
        e.gateOnsetsOnly = true;
        r = RenderLoops(e, 2);
        CHECK(GateEvents(r) == 8, "onsets-only: one gate per hit, 8 over 2 loops (%d)", GateEvents(r));
        CHECK(GateEvents(r, 0) == 4, "…kick lane fires at steps 0 and 8 only (%d)", GateEvents(r, 0));
        e.CallSlice(2, 1.f, false, false, false);
        CHECK(e.gateFired != 0, "a Voice call always gates");
    }

    SECTION("Bar estimate from the backbeat (bb_bars.h)");
    {
        // 4 bars at 172 BPM, 48 kHz: kick (60 Hz) on 1 and 3, snare (1.8 kHz + noise) on 2 and 4,
        // hats (8 kHz, quiet) on every 8th. The duration prior would call this 2 bars.
        const float bpm = 172.f, beat = 60.f * kSr / bpm, bar = 4.f * beat;
        const int   L = (int)(4.f * bar);
        static float mat[48000 * 6];
        static int   ons[64]; int no = 0;
        for(int i = 0; i < L; i++) mat[i] = 0.f;
        uint32_t rng = 1;
        auto burst = [&](int at, float f, float amp, int len, bool noise) {
            for(int i = 0; i < len && at + i < L; i++) {
                rng = rng * 1664525u + 1013904223u;
                float nz = noise ? ((float)(rng >> 8) / 8388608.f - 1.f) : 0.f;
                float env = expf(-(float)i / (float)len * 5.f);
                mat[at + i] += amp * env * (0.6f * sinf(6.2831853f * f * i / kSr) + 0.4f * nz);
            } };
        for(int b = 0; b < 4; b++)
            for(int q = 0; q < 4; q++)
            {
                const int at = (int)(b * bar + q * beat);
                if(q == 0 || q == 2) { burst(at, 60.f, 0.9f, 4000, false); ons[no++] = at; }
                else                 { burst(at, 1800.f, 0.8f, 3000, true);  ons[no++] = at; }
                burst(at + (int)(beat / 2), 8000.f, 0.15f, 800, true); ons[no++] = at + (int)(beat / 2);
            }
        // onsets sorted
        for(int i = 1; i < no; i++) for(int j = i; j > 0 && ons[j - 1] > ons[j]; j--) { int t = ons[j]; ons[j] = ons[j - 1]; ons[j - 1] = t; }
        const float est = bb::EstimateBarSamples(mat, L, kSr, ons, no);
        CHECK(est > 0.f && fabsf(est - bar) < 0.03f * bar, "backbeat bar estimate %.0f vs true %.0f samples", est, bar);
        bb::Engine e4; e4.Init(1); e4.SetMaterial(mat, L, kSr, ons, no, nullptr, 0);
        CHECK(e4.nSlices == 32, "the duration prior alone calls it 2 bars (%d slices)", e4.nSlices);
        e4.SetMaterial(mat, L, kSr, ons, no, nullptr, 64);
        CHECK(e4.nSlices == 64 && fabsf(e4.NativeBpm() - bpm) < 2.f, "with the estimate: 64 slices, native %.1f BPM", e4.NativeBpm());
        CHECK(bb::EstimateBarSamples(gMat, kLen, kSr, nullptr, 0) == 0.f, "no onsets → no estimate (prior applies)");
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

    // ── GRAM package (docs/packages/gram.md) ──────────────────────────────
    SECTION("GRAM Swaps: tiers ignored when off; on, no swap crosses a salience tier");
    {
        bb::Pattern p;
        uint8_t cls[64];
        for(int i = 0; i < 64; i++) cls[i] = (uint8_t)(i % 3);
        // a tier spread that cuts across the classes, so every same-class pool
        // holds all three tiers and a class-only swap WOULD cross tiers
        uint8_t tier[64], zero[64] = {};
        for(int i = 0; i < 64; i++) tier[i] = (uint8_t)((i * 7 + (i >> 2)) % 3);
        auto gen = [&](int mode, int ns, float k, uint64_t seed, bool on, const uint8_t* t) {
            p.langX = 0.3f; p.langY = 0.7f; p.langDens[0] = 0.6f; p.langDens[1] = 0.5f; p.langDens[2] = 0.9f; p.langRot = 0.25f;
            memcpy(p.salTier, t, 64); p.salSwaps = on;
            p.Generate(ns, 4, cls, k, seed, seed * 7 + 1, mode);
        };
        int diff = 0, viol = 0, swaps = 0, changed = 0;
        for(int mode = 0; mode < 4; mode++)
            for(int ns : {16, 32, 64})
                for(int ki = 0; ki <= 4; ki++)
                    for(uint64_t seed = 1; seed < 30; seed++)
                    {
                        // Classic/Collins above the cluster zone: hat ticks are not swaps
                        const float k = mode >= 2 ? ki / 4.f : 0.4f + 0.15f * (float)ki;
                        gen(mode, ns, k, seed, false, tier);
                        int8_t a[64], fa[64]; memcpy(a, p.src, 64); memcpy(fa, p.fsrc, 64);
                        gen(mode, ns, k, seed, false, zero);
                        if(memcmp(a, p.src, 64) || (mode == 1 && memcmp(fa, p.fsrc, 64))) diff++;
                        gen(mode, ns, k, seed, true, tier);
                        if(memcmp(a, p.src, 64)) changed++;
                        if(mode <= 1)
                        {
                            // a swap = a cell playing neither in place nor the previous
                            // cell's source (the generator's prevSrc, tracked the same way)
                            auto scan = [&](const int8_t* S, const int8_t* L, int from, int prev) {
                                int c = from;
                                while(c < ns)
                                {
                                    const int Lc = L[c] > 0 ? L[c] : 1, s = S[c];
                                    if(s >= 0) { if(s != c && s != prev) { swaps++; if(tier[s] != tier[c]) viol++; } prev = s; }
                                    c += Lc;
                                }
                            };
                            scan(p.src, p.len, 0, 0);
                            if(mode == 1 && p.fillStart >= 0)
                            {
                                int pv = 0; for(int c = 0; c < p.fillStart; c++) if(p.src[c] >= 0) pv = p.src[c];
                                scan(p.fsrc, p.flen, p.fillStart, pv);
                            }
                        }
                        else
                            for(int s = 0; s < ns; s++)
                            {   // same class as the step itself ⇒ the faithful pick was in place ⇒ this is a swap
                                const int q = p.src[s];
                                if(q >= 0 && q != s && cls[q] == cls[s]) { swaps++; if(tier[q] != tier[s]) viol++; }
                            }
                    }
        CHECK(diff == 0, "Swaps off: salTier is ignored (%d patterns differed)", diff);
        CHECK(swaps > 200, "the scan sees swaps (%d)", swaps);
        CHECK(viol == 0, "Swaps on: %d of %d swaps crossed a tier", viol, swaps);
        CHECK(changed > 0, "Swaps on changes some patterns (%d)", changed);

        // Engine: tiers are the loop's terciles of sliceSal; uniform salience is one tier
        Fresh(e, 0, 0.4f);
        int8_t off[64]; memcpy(off, e.stepSrc, 64);
        for(int s = 0; s < 16; s++) e.sliceSal[s] = gCls[s] < 2 ? 1.f : ((s & 1) ? 0.1f : 0.5f);
        e.sliceSal[15] = 0.f;
        e.swapsMode = 0; e.RegenPattern();
        CHECK(memcmp(off, e.stepSrc, 64) == 0 && !e.pat.salSwaps, "Swaps=0: salience never changes the pattern");
        e.swapsMode = 1; e.RegenPattern();
        int mono = 0;
        for(int i = 0; i < 16; i++) for(int j = 0; j < 16; j++)
            if(e.sliceSal[i] < e.sliceSal[j] && e.pat.salTier[i] > e.pat.salTier[j]) mono++;
        CHECK(e.pat.salSwaps && mono == 0, "tiers are monotone in salience (%d inversions)", mono);
        CHECK(e.pat.salTier[15] == 0 && e.pat.salTier[1] == 1 && e.pat.salTier[2] == 2 && e.pat.salTier[0] == 2,
              "terciles: silence 0, ghost hats 1, loud hats and drums 2 (got %d %d %d %d)",
              e.pat.salTier[15], e.pat.salTier[1], e.pat.salTier[2], e.pat.salTier[0]);
        for(int s = 0; s < 16; s++) e.sliceSal[s] = 0.7f;
        e.RegenPattern();
        int one = 0; for(int s = 0; s < 16; s++) one += e.pat.salTier[s] == 2;
        CHECK(one == 16, "uniform salience = a single tier (%d/16)", one);
    }

    SECTION("GRAM Flow Déjà Vu: replay rate, no push, exact replays, determinism, taps fresh");
    {
        struct Run { int recalls = 0, pushed = 0, mismatch = 0; uint64_t sig = 0; };
        auto run = [&](bb::Engine& en, int dv, int bars) {
            Run r;
            Fresh(en, 0, 0.5f);
            en.dejaVu = dv; en.flowRegen = true;
            static int8_t snap[bb::Engine::kHist][64];
            memcpy(snap[en.histHead], en.stepSrc, 64);
            for(int b = 0; b < bars; b++)
            {
                const uint32_t r0 = en.dejaRecalls; const int n0 = en.histN, h0 = en.histHead;
                en.RerollFill();
                if(en.dejaRecalls != r0)
                {
                    r.recalls++;
                    if(en.histN != n0 || en.histHead != h0) r.pushed++;
                    if(memcmp(snap[en.histPos], en.stepSrc, 64)) r.mismatch++;
                }
                else memcpy(snap[en.histHead], en.stepSrc, 64);
                for(int s = 0; s < 16; s++) r.sig = (r.sig * 1099511628211ULL) ^ (uint8_t)en.stepSrc[s];
            }
            return r;
        };
        Run r0 = run(e, 0, 200);
        CHECK(r0.recalls == 0 && e.dejaBars == 200 && e.histN == bb::Engine::kHist, "Off: every flow bar is fresh (%d recalls, %u bars)", r0.recalls, e.dejaBars);
        Run r1 = run(e, 1, 400);
        const float rate1 = (float)r1.recalls / 400.f;
        CHECK(rate1 > 0.18f && rate1 < 0.32f, "Some: ~¼ of flow bars replay (%.3f)", rate1);
        CHECK(r1.pushed == 0, "a replay does not push to history (%d did)", r1.pushed);
        CHECK(r1.mismatch == 0, "a replay reproduces the ring entry exactly (%d differed)", r1.mismatch);
        Run r2 = run(e, 2, 400);
        const float rate2 = (float)r2.recalls / 400.f;
        CHECK(rate2 > 0.68f && rate2 < 0.82f, "Lots: ~¾ of flow bars replay (%.3f)", rate2);
        CHECK(r2.pushed == 0 && r2.mismatch == 0, "Lots: no pushes (%d), exact replays (%d wrong)", r2.pushed, r2.mismatch);
        bb::Engine e5;
        Run r2b = run(e5, 2, 400);
        CHECK(r2b.sig == r2.sig && r2b.recalls == r2.recalls, "the bar sequence is reproducible from Init's seed");
        Fresh(e, 0, 0.5f);
        e.dejaVu = 2; e.flowRegen = false;
        for(int i = 0; i < 50; i++) e.RerollFill();
        CHECK(e.dejaRecalls == 0 && e.dejaBars == 0 && e.histN == bb::Engine::kHist, "B2 taps never replay (%u recalls)", e.dejaRecalls);
        // rendered flow: the wrap in Process drives RerollFill; output stays finite
        Fresh(e, 1, 0.3f);
        e.dejaVu = 2; e.flowRegen = true;
        Rendered r = RenderLoops(e, 8, 96);
        bool finite = true; for(float v : r.y) if(!std::isfinite(v)) finite = false;
        CHECK(finite && e.dejaBars >= 7, "flow under Lots renders finite audio over %u bars", e.dejaBars);
    }

    SECTION("GRAM accents: Grids level > 192 on the firing class; Euclid downbeat; none elsewhere");
    {
        bb::Pattern p;
        uint8_t cls[64];
        for(int i = 0; i < 64; i++) cls[i] = (uint8_t)(i % 3);
        int wrong = 0, accents = 0, fired = 0;
        for(float x : {0.1f, 0.5f, 0.9f})
            for(float y : {0.2f, 0.5f, 0.8f})
                for(float d : {0.3f, 0.6f, 1.f})
                {
                    p.langX = x; p.langY = y; p.langDens[0] = p.langDens[1] = p.langDens[2] = d; p.langRot = 0.f;
                    p.Generate(32, 4, cls, 1.f, 1, 2, 2);
                    const uint8_t X = (uint8_t)(x * 255.f + 0.5f), Y = (uint8_t)(y * 255.f + 0.5f);
                    const uint8_t thr = (uint8_t)(255 - (uint8_t)(d * 255.f + 0.5f));
                    for(int s = 0; s < 32; s++)
                    {
                        const uint8_t gs = (uint8_t)((s % 16) * 2);
                        int c = -1;
                        for(int k = 0; k < 3 && c < 0; k++) if(bb::GridsReadDrumMap(gs, (uint8_t)k, X, Y) > thr) c = k;
                        const bool expect = c >= 0 && bb::GridsReadDrumMap(gs, (uint8_t)c, X, Y) > 192;
                        if(p.accent[s] != expect) wrong++;
                        if((c < 0) != (p.src[s] == -2)) wrong++;
                        accents += p.accent[s]; fired += c >= 0;
                    }
                }
        CHECK(wrong == 0, "Grids accent = firing class's level > 192 (%d wrong)", wrong);
        CHECK(accents > 0 && accents < fired, "some, not all, hits are accented (%d of %d)", accents, fired);
        p.langDens[0] = 0.25f; p.langDens[1] = 0.5f; p.langDens[2] = 1.f; p.langRot = 0.f;
        p.Generate(32, 4, cls, 1.f, 1, 2, 3);
        int bad = 0; for(int s = 0; s < 32; s++) if(p.accent[s] != ((s % 16) == 0 && p.src[s] != -2)) bad++;
        CHECK(bad == 0 && p.accent[0] && p.accent[16], "Euclid: the firing downbeat of each bar is accented (%d wrong)", bad);
        int stray = 0;
        for(int mode = 0; mode < 2; mode++)
            for(uint64_t seed = 1; seed < 20; seed++)
            {
                p.Generate(32, 4, cls, 0.3f, seed, seed + 1, mode);
                for(int s = 0; s < 32; s++) stray += p.accent[s];
            }
        CHECK(stray == 0, "Classic/Collins set no accents (%d)", stray);
        Fresh(e, 2, 1.f);
        e.langDens[0] = e.langDens[1] = e.langDens[2] = 1.f;
        e.RegenPattern();
        int n = 0, m = 0;
        for(int s = 0; s < 16; s++) { n += e.stepAccent[s]; m += e.stepAccent[s] != e.pat.accent[s]; }
        CHECK(n > 0 && m == 0, "stepAccent follows Pattern::accent (%d accents, %d mismatches)", n, m);
    }

    SECTION("GRAM Camp: change ringing — rows permute the bar's cells, come round, step by one");
    {
        bb::Pattern p;
        uint8_t cls[64];
        for(int i = 0; i < 64; i++) cls[i] = (uint8_t)(i % 3);
        // every bar is a permutation of its own cells; the row index is the fill seed
        auto isPerm = [&](int ns, int barSteps) {
            for(int b0 = 0; b0 < ns; b0 += barSteps)
            {
                bool seen[64] = {};
                int  c = b0, cnt = 0;
                while(c < b0 + barSteps)
                {
                    const int L = p.len[c] > 0 ? p.len[c] : 1, s = p.src[c];
                    if(s < b0 || s >= b0 + barSteps || (s - b0) % L || seen[s]) return false;
                    seen[s] = true; cnt++; c += L;
                }
                if(c != b0 + barSteps) return false;
            }
            return true;
        };
        int notPerm = 0, notRounds = 0, nondet = 0, noStep = 0;
        for(uint64_t cs = 0; cs < 16; cs++)
            for(int ns : {16, 32, 64})
                for(int ki = 0; ki <= 4; ki++)
                    for(uint64_t fs = 0; fs < 60; fs += 7)
                    {
                        p.Generate(ns, 4, cls, ki / 4.f, cs, fs, 4);
                        if(!isPerm(ns, 16)) notPerm++;
                        int8_t a[64]; memcpy(a, p.src, 64);
                        p.Generate(ns, 4, cls, ki / 4.f, cs, fs, 4);
                        if(memcmp(a, p.src, 64)) nondet++;
                        if(ns >= 32 && !(cs & 4)) // forward: bar 1 of row r == bar 0 of row r+1
                        {
                            p.Generate(ns, 4, cls, ki / 4.f, cs, fs + 1, 4);
                            for(int s = 0; s < 16; s++) if(a[16 + s] != (p.src[s] < 0 ? p.src[s] : p.src[s] + 16)) { noStep++; break; }
                        }
                    }
        CHECK(notPerm == 0, "every bar is a permutation of its own cells (%d weren't)", notPerm);
        CHECK(nondet == 0, "deterministic (%d)", nondet);
        CHECK(noStep == 0, "fill seed +1 rings the next change (%d bars disagreed)", noStep);
        // plain hunt on 4 bells (knob 0.5–0.96) comes round after 8 rows, plain bob after 24
        for(uint64_t cs : {0ull, 2ull})
        {
            const int cycle = cs & 2 ? 24 : 8;
            for(uint64_t fs = 0; fs < 48; fs++)
            {
                p.Generate(16, 4, cls, 0.7f, cs, fs, 4);
                bool rounds = true; for(int c = 0; c < 16; c += 4) rounds &= p.src[c] == c && p.len[c] == 4;
                if(rounds != ((fs % (uint64_t)cycle) == 0)) notRounds++;
            }
        }
        CHECK(notRounds == 0, "rounds recur exactly at the method's cycle (%d rows wrong)", notRounds);
        p.Generate(16, 4, cls, 1.f, 5, 3, 4);
        CHECK(p.src[0] == 0 && p.len[0] == 16, "knob full right = rounds, one bar cell");
        p.Generate(16, 4, cls, 0.1f, 0, 1, 4);
        int ones = 0; for(int s = 0; s < 16; s++) ones += p.len[s] == 1;
        CHECK(ones == 16 && p.src[0] == 1 && p.src[1] == 0, "16 bells at the bottom: the first change swaps every pair");
        // engine: Camp rerolls step the row by one; a scrub back restores the previous row
        Fresh(e, 4, 0.7f);
        const uint64_t f0 = e.fillSeed;
        int8_t before[64]; memcpy(before, e.stepSrc, 64);
        e.RerollFill();
        CHECK(e.fillSeed == f0 + 1 && memcmp(before, e.stepSrc, 64) != 0, "Camp: B2 rings the next change (seed %llu → %llu)", (unsigned long long)f0, (unsigned long long)e.fillSeed);
        e.Scrub(-1);
        CHECK(memcmp(before, e.stepSrc, 64) == 0, "…and history scrubs back to the previous row");
        Rendered r = RenderLoops(e, 2);
        CHECK(MapMismatches(e, r) == 0, "the rung bar plays as mapped");
    }

    /* ── GATES package ───────────────────────────────────────────────── */

    SECTION("GATES: band mask — kick+hat marks both lanes, pure hits mark one");
    {
        // A drum loop on the 16-step grid: 60 Hz kick, 1.8 kHz+noise snare,
        // 8 kHz + band-limited noise hat (a real hat has nothing under 325 Hz;
        // white noise there would be a fake kick). Kicks at 0/8 (pure) and 6/14
        // (with a hat), snares at 4 (with a hat) and 12 (pure), pure hats at
        // 2/10, a quiet hat carpet on the odd steps, step 15 silent.
        static float drum[kLen];
        for(int i = 0; i < kLen; i++) drum[i] = 0.f;
        uint32_t rng = 7;
        auto burst = [&](int at, float f, float amp, int n, bool noise, bool bright) {
            float lp1 = 0.f, lp2 = 0.f;
            const float a = 1.f - expf(-6.2831853f * 2000.f / kSr);
            for(int i = 0; i < n && at + i < kLen; i++) {
                rng = rng * 1664525u + 1013904223u;
                float nz = noise ? ((float)(rng >> 8) / 8388608.f - 1.f) : 0.f;
                if(bright) { lp1 += a * (nz - lp1); float h = nz - lp1; lp2 += a * (h - lp2); nz = h - lp2; }
                float env = expf(-(float)i / (float)n * 5.f);
                drum[at + i] += amp * env * (0.6f * sinf(6.2831853f * f * i / kSr) + 0.4f * nz);
            } };
        auto kick  = [&](int s) { burst(s * kStep, 60.f, 0.9f, 4000, false, false); };
        auto snare = [&](int s) { burst(s * kStep, 1800.f, 0.8f, 3000, true, false); };
        auto hat   = [&](int s, float a) { burst(s * kStep, 8000.f, a, 2400, true, true); };
        kick(0); kick(8); kick(6); kick(14); snare(4); snare(12); hat(2, 0.35f); hat(10, 0.35f);
        hat(6, 0.35f); hat(14, 0.35f); hat(4, 0.35f);
        for(int s = 1; s < 15; s += 2) hat(s, 0.15f);
        uint8_t cls[64], bands[64], ref[64];
        bb::ClassifySteps(drum, kLen, kSr, cls, 0, nullptr, nullptr, 0, bands);
        const uint8_t K = 1, S = 2, H = 4;
        CHECK(bands[0] == K && bands[8] == K, "pure kick steps mark the kick lane only (0:%x 8:%x)", bands[0], bands[8]);
        CHECK(bands[6] == (K | H) && bands[14] == (K | H), "kick+hat steps mark kick AND hat (6:%x 14:%x)", bands[6], bands[14]);
        CHECK(bands[2] == H && bands[10] == H, "pure hat steps mark the hat lane only (2:%x 10:%x)", bands[2], bands[10]);
        CHECK((bands[4] & (S | H)) == (S | H) && !(bands[4] & K), "snare+hat marks snare and hat, not kick (%x)", bands[4]);
        CHECK((bands[12] & S) && !(bands[12] & K), "pure snare marks the snare lane, not kick (%x)", bands[12]);
        int carpet = 0; for(int s = 1; s < 15; s += 2) carpet += bands[s] == H;
        CHECK(carpet == 7, "the quiet hat carpet marks the hat lane only (%d/7)", carpet);
        CHECK(bands[15] == 0 && cls[15] == 3, "the silent step marks nothing (%x)", bands[15]);
        int sup = 0; for(int s = 0; s < 15; s++) sup += (bands[s] >> cls[s]) & 1;
        CHECK(sup == 15, "Multi-label is a superset of the Hockman lens's lane (%d/15)", sup);
        memcpy(ref, bands, 64);
        int lensDep = 0;
        for(int m = 1; m < 4; m++) { bb::ClassifySteps(drum, kLen, kSr, cls, m, nullptr, nullptr, 0, bands); lensDep += memcmp(ref, bands, 64) != 0; }
        CHECK(lensDep == 0, "the mask is lens-independent (%d lenses differ)", lensDep);
        // the classification itself is untouched by asking for the mask
        uint8_t c1[64], c2[64]; float s1[64], s2[64];
        for(int m = 0; m < 4; m++)
        {
            bb::ClassifySteps(gMat, kLen, kSr, c1, m, s1, nullptr, 0, nullptr);
            bb::ClassifySteps(gMat, kLen, kSr, c2, m, s2, nullptr, 0, bands);
            CHECK(memcmp(c1, c2, 16) == 0 && memcmp(s1, s2, 16 * sizeof(float)) == 0, "mode %d: classes and salience identical with bandsOut", m);
        }
    }

    SECTION("GATES: engine lanes — dominant identical, multi-label raises every marked lane");
    {
        // Dominant class with a mask present must be byte-identical to no mask.
        Fresh(e, 0, 1.f);
        Rendered r0 = RenderLoops(e, 2);
        Fresh(e, 0, 1.f);
        for(int s = 0; s < 16; s++) e.stepBands[s] = (uint8_t)(7u);   // every lane marked everywhere
        e.multiLabel = false;
        Rendered r1 = RenderLoops(e, 2);
        CHECK(r0.gates == r1.gates, "Dominant class ignores stepBands");
        CHECK(r0.y == r1.y, "…and the audio is unchanged");
        // Multi-label on constructed masks: kick+hat at step 8, kick only at 0
        Fresh(e, 0, 1.f);
        for(int s = 0; s < 16; s++) e.stepBands[s] = (uint8_t)(1u << gCls[s]);
        e.stepBands[8] = 1 | 4;                                        // a hat rides the kick
        e.multiLabel = true;
        Rendered r2 = RenderLoops(e, 2);
        CHECK(r2.y == r0.y, "multi-label changes lanes, never audio");
        CHECK(GateEvents(r2, 0) == 4, "kick lane: steps 0 and 8, 2 loops (%d)", GateEvents(r2, 0));
        CHECK(GateEvents(r2, 2) == GateEvents(r0, 2) + 2, "hat lane gains the kick+hat step (%d vs %d)", GateEvents(r2, 2), GateEvents(r0, 2));
        int both = 0, kickOnly = 0;
        for(size_t b = 0; b < r2.gates.size(); b++) { if((r2.gates[b] & 5) == 5) both++; if(r2.gates[b] == 1) kickOnly++; }
        CHECK(both == 2 && kickOnly == 2, "kick+hat block raises both lanes (%d), pure kick block the kick lane only (%d)", both, kickOnly);
        // no mask known → the dominant class (no lane is ever lost)
        Fresh(e, 0, 1.f);
        e.multiLabel = true;
        Rendered r3 = RenderLoops(e, 2);
        CHECK(r3.gates == r0.gates, "stepBands all zero → dominant lanes");
        // onsets-only and continuations gate multi-label exactly as dominant
        static const int on[4] = {0, 4 * kStep + 30, 8 * kStep, 12 * kStep - 200};
        Fresh(e, 0, 1.f);
        e.SetMaterial(gMat, kLen, kSr, on, 4, gCls, 0);
        e.RegenPattern();
        for(int s = 0; s < 16; s++) e.stepBands[s] = 7;
        e.multiLabel = true; e.gateOnsetsOnly = true;
        Rendered r4 = RenderLoops(e, 2);
        CHECK(GateEvents(r4) == 8, "onsets-only: 4 hits × 2 loops = 8 gate blocks (%d)", GateEvents(r4));
        CHECK(GateEvents(r4, 0) == 8 && GateEvents(r4, 2) == 8, "…each raising every marked lane (K%d H%d)", GateEvents(r4, 0), GateEvents(r4, 2));
        Fresh(e, 3, 1.f);
        e.restSustain = true;
        e.langDens[0] = 4.f / 16.f; e.langDens[1] = 0.f; e.langDens[2] = 0.f; e.langRot = 0.f;
        e.RegenPattern();
        for(int s = 0; s < 16; s++) e.stepBands[s] = 7;
        e.multiLabel = true;
        Rendered r5 = RenderLoops(e, 2);
        CHECK(GateEvents(r5) == 8, "continuations raise no lane under multi-label either (%d)", GateEvents(r5));
    }

    SECTION("GATES: accent → velocity ceiling; the lane law");
    {
        CHECK(bb::GateVolts(false, 0.2f, false) == 5.f && bb::GateVolts(false, 0.2f, true) == 5.f, "fixed level is 5 V, accent or not");
        CHECK(fabsf(bb::GateVolts(true, 0.5f, false) - 3.25f) < 1e-6f && bb::GateVolts(true, 0.f, false) == 1.5f && bb::GateVolts(true, 1.f, false) == 5.f, "velocity maps salience 0..1 → 1.5..5 V");
        CHECK(bb::GateVolts(true, 0.2f, true) == 5.f, "an accented velocity hit is lifted to the ceiling");
        // engine side: the grammar's accent rides the gate to the shell
        Fresh(e, 0, 1.f);
        e.stepAccent[8] = true;                                        // kick on 3 accented, kick on 1 not
        Rendered r = RenderLoops(e, 1);
        int accK = 0, plainK = 0;
        for(size_t b = 0; b < r.gates.size(); b++) if(r.gates[b] & 1) { if(r.accents[b] & 1) accK++; else plainK++; }
        CHECK(accK == 1 && plainK == 1, "kick lane: one accented hit, one plain (%d/%d)", accK, plainK);
        int accOther = 0; for(uint8_t a : r.accents) accOther += (a & ~1u) != 0;
        CHECK(accOther == 0, "the accent stays on its own lane");
        e.CallSlice(8, 1.f, false, false, false);
        CHECK((e.gateFired & 1) && !e.gateAccent[0], "a Voice call gates without an accent");

        // the lane law (bb_gates.h) as the shell drives it: audio arms per
        // 24-sample block, the poll consumes
        int cd = 0; float lvl = 0.f; bool rt = false;
        const int t6 = 288, hold = bb::kGateHold;
        bb::GateArm(cd, lvl, rt, true, t6, 5.f, 24);
        CHECK(cd == t6 && lvl == 5.f && !rt, "first hit arms a 6 ms gate, no retrigger");
        CHECK(bb::GatePollVolts(cd, lvl, rt) == 5.f, "poll writes the level");
        for(int i = 0; i < 20; i++) bb::GateArm(cd, lvl, rt, false, t6, 0.f, 24);
        CHECK(cd <= 0 && bb::GatePollVolts(cd, lvl, rt) == 0.f, "…and it closes after 6 ms");
        bb::GateArm(cd, lvl, rt, true, t6, 5.f, 24);
        bb::GateArm(cd, lvl, rt, false, t6, 0.f, 24);
        bb::GateArm(cd, lvl, rt, true, t6, 3.f, 24);                   // a ratchet 1 ms later, quieter
        CHECK(rt && cd == t6 && lvl == 3.f, "re-hit while high: retrigger flagged, gate re-armed at the new level");
        CHECK(bb::GatePollVolts(cd, lvl, rt) == 0.f && !rt, "the next poll is the low…");
        CHECK(bb::GatePollVolts(cd, lvl, rt) == 3.f, "…and the one after is up again at the new level");
        // Hold: never counts down; only a re-hit on the same lane retriggers
        cd = 0; lvl = 0.f; rt = false;
        bb::GateArm(cd, lvl, rt, true, hold, 5.f, 24);
        for(int i = 0; i < 48000; i++) bb::GateArm(cd, lvl, rt, false, hold, 0.f, 24);
        CHECK(cd == hold && bb::GatePollVolts(cd, lvl, rt) == 5.f, "Hold stays up through 24 s of silence");
        bb::GateArm(cd, lvl, rt, true, hold, 5.f, 24);
        CHECK(rt && bb::GatePollVolts(cd, lvl, rt) == 0.f && bb::GatePollVolts(cd, lvl, rt) == 5.f, "a re-hit on a held lane: one poll low, then up");
        // Leaving Hold for a timed length closes a held lane within that length
        bb::GateArm(cd, lvl, rt, false, t6, 0.f, 24);
        CHECK(cd == t6 - 24, "Hold → 6 ms: the held lane now counts down from 6 ms, not 2^28 samples (cd %d)", cd);
        for(int i = 0; i < 12; i++) bb::GateArm(cd, lvl, rt, false, t6, 0.f, 24);
        CHECK(bb::GatePollVolts(cd, lvl, rt) == 0.f, "…and is closed 6 ms later");
    }

    /* ── PLAY package: Options → Fades / Slice Tail ─────────────────────── */
    auto Fnv = [](const std::vector<float>& y) {
        uint64_t h = 1469598103934665603ULL;
        for(float v : y) { uint32_t b; memcpy(&b, &v, 4); for(int i = 0; i < 4; i++) { h ^= (b >> (8 * i)) & 255u; h *= 1099511628211ULL; } }
        return h;
    };
    auto MaxStep = [](const std::vector<float>& y) {
        float m = 0.f; for(size_t i = 1; i < y.size(); i++) { float d = fabsf(y[i] - y[i - 1]); if(d > m) m = d; } return m;
    };

    SECTION("PLAY: Fixed fades + Gate tail are byte-identical to the pre-PLAY engine");
    {
        // Golden FNV-1a checksums of the rendered float bits, computed from the
        // untouched engine (commit a4cf8ac) with this exact driver before the
        // PLAY changes landed; identical at -O1 sanitized and -O2 on x86-64
        // (SSE, no FMA contraction — an FMA host would need its own goldens).
        struct Case { int mode; float knob, pitch; uint64_t gold; };
        const Case cases[4] = { {0, 0.3f, 1.0f, 0xdca3e67a979f38d8ULL},   // chops + Classic doubles
                                {0, 0.3f, 1.5f, 0x4bf165f0ff272e75ULL},   // pitched up: honest gates
                                {0, 1.0f, 0.7f, 0x756ef6f001894408ULL},   // verbatim, pitched down: overlaps
                                {1, 0.2f, 1.2f, 0x0b44c34fb1580ea5ULL} }; // Collins rolls + chokes
        for(const Case& c : cases)
        {
            Fresh(e, c.mode, c.knob);
            e.fadeMode = 0; e.tailMode = 0;
            e.pitch = c.pitch;
            Rendered r = RenderLoops(e, 2);
            const uint64_t h = Fnv(r.y);
            CHECK(h == c.gold, "mode %d knob %.1f pitch %.1f: checksum %016llx != golden %016llx", c.mode, c.knob, c.pitch,
                  (unsigned long long)h, (unsigned long long)c.gold);
        }
    }

    SECTION("PLAY: Adaptive fades — shorter on keyframe-dense slices than sparse ones");
    {
        uint8_t cls[64]; float dens[64];
        bb::ClassifySteps(gMat, kLen, kSr, cls, 3, nullptr, dens);   // the Density lens
        Fresh(e, 0, 1.f);
        memcpy(e.sliceDens, dens, sizeof dens);
        e.fadeMode = 0;
        CHECK(e.FadeSamples(0) == 240 && e.FadeSamples(14) == 240 && e.AttackSamples(0) == 48, "Fixed: 5 ms release, 1 ms attack everywhere");
        e.fadeMode = 1;
        const int f0 = e.FadeSamples(0), f14 = e.FadeSamples(14), fS = e.FadeSamples(15);
        CHECK(f0 > f14, "sparse slice 0 (200 Hz) fades longer than dense slice 14 (900 Hz): %d vs %d samples", f0, f14);
        CHECK(f0 <= 480 && f14 >= 48, "fades stay within 1–10 ms (%d, %d)", f0, f14);
        CHECK(fS == 480, "the silent slice (only guard keyframes) takes the longest fade (%d)", fS);
        e.sliceDens[15] = 0.f;
        CHECK(e.FadeSamples(15) == 240, "unknown density (0) keeps the fixed length (%d)", e.FadeSamples(15));
        CHECK(e.AttackSamples(0) > e.AttackSamples(14) && e.AttackSamples(14) >= 48, "attack ramps follow (%d vs %d)", e.AttackSamples(0), e.AttackSamples(14));
        // Behavioural: the attack ramp measured on the output of a call from
        // silence — output / (0.8 × source) reaches unity after AttackSamples.
        auto AttackLen = [&](int sl) {
            Fresh(e, 0, 1.f); memcpy(e.sliceDens, dens, sizeof dens); e.fadeMode = 1; e.muted = true;
            float o[24]; e.barPhase = 0.9; e.Process(o, 24);            // settle curStep
            e.CallSlice(sl, 1.f, false, false, false);
            std::vector<float> y; for(int b = 0; b < 40; b++) { e.Process(o, 24); y.insert(y.end(), o, o + 24); }
            for(int i = 0; i < (int)y.size(); i++)
            {
                const float src = 0.8f * gMat[sl * kStep + i];
                if(fabsf(src) > 0.1f && y[i] / src > 0.98f) return i;
            }
            return -1;
        };
        const int a0 = AttackLen(0), a14 = AttackLen(14);
        CHECK(a0 > a14 && a14 > 0, "rendered attack: sparse slice 0 ramps over %d samples, dense slice 14 over %d", a0, a14);
        CHECK(a0 >= e.AttackSamples(0) - 30 && a0 <= e.AttackSamples(0) + 30, "…and slice 0's ramp matches AttackSamples %d (%d)", e.AttackSamples(0), a0);
        // Adaptive on a chopped loop with rolls: finite, still the map
        Fresh(e, 1, 0.2f); memcpy(e.sliceDens, dens, sizeof dens); e.fadeMode = 1; e.pitch = 1.2f;
        Rendered r = RenderLoops(e, 2);
        bool finite = true; for(float v : r.y) if(!std::isfinite(v)) finite = false;
        CHECK(finite, "Adaptive + Collins rolls: finite output");
        Fresh(e, 0, 1.f); memcpy(e.sliceDens, dens, sizeof dens); e.fadeMode = 1;
        r = RenderLoops(e, 2);
        CHECK(MapMismatches(e, r) == 0 && e.trigFlash <= 4, "Adaptive: verbatim still plays the map as one voice (%u triggers)", e.trigFlash);
    }

    SECTION("PLAY: Slice Tail — a slot 1.5× the slice: Gate is silent after the natural end, loops keep sounding without seams");
    {
        // pitch 1.5: each 6000-sample slot consumes 9000 source samples; the
        // slice runs out after 4000 output samples. Judge the last third of
        // every slot in the second loop (steps whose source is not silent).
        // (The synthetic material restarts each slice's sine from 0, so it has a
        // hard step at every boundary; Gate mode's lookahead grace plays ~8 ms
        // across it at pitch-up — pre-existing, byte-identical, and why the
        // seam check below looks only at the tail region, where the loop
        // policies alone are sounding.)
        auto TailRms = [&](const Rendered& r, int step) { return Rms(&r.y[step * kStep + 4400], 1400); };
        auto Slope   = [](int k) { return 0.8f * 0.5f * 6.2831853f * StepFreq(k) * 1.5f / kSr; }; // slice k's own max |Δ| at rate 1.5
        for(int tail = 0; tail < 3; tail++)
        {
            Fresh(e, 0, 1.f);
            e.fadeMode = 0; e.tailMode = tail; e.pitch = 1.5f;
            Rendered r = RenderLoops(e, 2);
            int silent = 0, sounding = 0, tooMany = 0, wrongF = 0, seams = 0;
            float worst = 0.f;
            for(int k = 0; k < 15; k++)
            {
                const float t = TailRms(r, 16 + k); silent += t < 1e-3f; sounding += t > 0.1f;
                // the slot plays ITS slice, repitched ×1.5 (whole slot for loops, first two thirds for Gate)
                const int a = (16 + k) * kStep + 300, n = (tail ? kStep - 400 : 3600);
                const float f = MeasureFreq(&r.y[a], n);
                if(fabsf(f - 1.5f * StepFreq(k)) > 0.05f * 1.5f * StepFreq(k)) wrongF++;
                // seams: inside the tail region no sample-to-sample step may exceed the slice's own slope (+10 %)
                float m = 0.f;
                for(int i = (16 + k) * kStep + 4200; i < (16 + k) * kStep + 5900; i++) { float d = fabsf(r.y[i] - r.y[i - 1]); if(d > m) m = d; }
                if(tail && m > 1.1f * Slope(k)) seams++;
                if(m / Slope(k) > worst) worst = m / Slope(k);
            }
            CHECK(wrongF == 0, "tail %d: every slot plays its own slice at ×1.5 (%d wrong)", tail, wrongF);
            if(tail == 0)
                CHECK(silent == 15, "Gate: every slot's tail is silent after the natural end (%d/15)", silent);
            else
            {
                CHECK(sounding == 15, "tail %d: every slot's tail keeps sounding (%d/15)", tail, sounding);
                CHECK(seams == 0, "tail %d: no seam clicks in the looped tails (%d slots over; worst |Δ| = %.2f× the slice's slope)", tail, seams, worst);
                CHECK(e.trigFlash == 32, "tail %d: one trigger per step (%u)", tail, e.trigFlash);
            }
            // a tail always yields to the next attack: never more than 2 voices alive
            Fresh(e, 0, 0.3f); e.tailMode = tail; e.pitch = 1.5f;
            e.extPhase = true; float o[24];
            for(int b = 0; b < 4000; b++)
            {
                e.barPhase = fmod((double)b * 24. / kSr / e.barSeconds, 1.0);
                e.Process(o, 24);
                int alive = 0; for(int v = 0; v < bb::Engine::kVoices; v++) alive += e.voices[v].active;
                if(alive > 2) tooMany++;
            }
            CHECK(tooMany == 0, "tail %d: at most 2 voices alive (the fading tail + the new hit) — %d blocks over", tail, tooMany);
        }
        // Native pitch: the tail never engages, continuity still holds verbatim runs as one voice
        for(int tail = 1; tail < 3; tail++)
        {
            Fresh(e, 0, 1.f); e.tailMode = tail; e.pitch = 1.f;
            Rendered r = RenderLoops(e, 2);
            CHECK(e.trigFlash <= 4 && MapMismatches(e, r) == 0, "tail %d at native pitch: verbatim run is one voice (%u triggers)", tail, e.trigFlash);
            int looped = 0; for(int v = 0; v < bb::Engine::kVoices; v++) looped += e.voices[v].looping;
            CHECK(looped == 0, "tail %d at native pitch: no voice ever looped", tail);
        }
        // Loop points sit on positive-going zero crossings inside the slice's second half
        Fresh(e, 0, 1.f); e.tailMode = 1; e.FindLoop(3);
        const double a = e.loopA[3], b = e.loopB[3];
        const int ia = (int)a, ib = (int)b;
        CHECK(e.loopState[3] == 1 && a >= 3 * kStep + kStep / 2 - 1 && b <= 4 * kStep - 2 && b - a >= 256, "slice 3 loop region [%.2f, %.2f) in its second half", a - 3 * kStep, b - 3 * kStep);
        CHECK(gMat[ia] <= 0.f && gMat[ia + 1] > 0.f && gMat[ib] <= 0.f && gMat[ib + 1] > 0.f, "…bounded by positive-going zero crossings");
        const float za = gMat[ia] + (float)(a - ia) * (gMat[ia + 1] - gMat[ia]), zbv = gMat[ib] + (float)(b - ib) * (gMat[ib + 1] - gMat[ib]);
        CHECK(fabsf(za) < 1e-4f && fabsf(zbv) < 1e-4f, "…refined to sub-sample zeros (%.5f, %.5f)", za, zbv);
        // Adaptive + Ping-pong together, Collins rolls: finite
        uint8_t cls[64]; float dens[64];
        bb::ClassifySteps(gMat, kLen, kSr, cls, 3, nullptr, dens);
        Fresh(e, 1, 0.2f); memcpy(e.sliceDens, dens, sizeof dens); e.fadeMode = 1; e.tailMode = 2; e.pitch = 1.5f;
        Rendered r = RenderLoops(e, 3);
        bool finite = true; for(float v : r.y) if(!std::isfinite(v)) finite = false;
        CHECK(finite && MaxStep(r.y) < 0.5f, "Adaptive + Ping-pong + rolls: finite, max |Δ| %.3f", MaxStep(r.y));

    }
    /* ───────────────────────── SLICE package ───────────────────────── */
    SECTION("SLICE onsets: mode 0 byte-identical, mode 1 (log-filtered flux) finds the quiet hats");
    {
        static bb::OnsetDetector det;
        auto hash = [](const int* o, int n, const float* s) {
            uint64_t h = 1469598103934665603ull;
            for(int i = 0; i < n; i++) { h ^= (uint64_t)(uint32_t)o[i]; h *= 1099511628211ull; uint32_t u; memcpy(&u, &s[i], 4); h ^= u; h *= 1099511628211ull; }
            return h; };
        static float tb[48000 * 8]; static int on[128]; static float sal[128];
        const int tlen = bb::MakeTestBreak(tb, 48000 * 8, kSr);
        det.mode = 0;
        int n = det.Detect(tb, tlen, kSr, on, 128, sal);
        CHECK(n == 16 && hash(on, n, sal) == 0x3aafccb7403e6f5bull, "mode 0 on the test break: v0.8.0 table (n %d, hash %016llx)", n, (unsigned long long)hash(on, n, sal));
        n = det.Detect(gMat, kLen, kSr, on, 128, sal);
        CHECK(n == 16 && hash(on, n, sal) == 0xddfe11dd6de0abd8ull, "mode 0 on the tone grid: v0.8.0 table (hash %016llx)", (unsigned long long)hash(on, n, sal));
        // 2 bars at 100 BPM: loud kicks (1, 3) and snares (2, 4), hats on every
        // 16th at −30 dB. Truth = the 32 hat times (kicks/snares coincide).
        const float bpm = 100.f, beat = 60.f * kSr / bpm, step = beat / 4.f;
        const int   L = (int)(8.f * beat);
        static float m[48000 * 5]; for(int i = 0; i < L; i++) m[i] = 0.f;
        uint32_t rng = 7;
        auto burst = [&](int at, float f, float amp, int len, bool noise) {
            for(int i = 0; i < len && at + i < L; i++) { rng = rng * 1664525u + 1013904223u; float nz = noise ? ((float)(rng >> 8) / 8388608.f - 1.f) : 0.f;
                float env = expf(-(float)i / (float)len * 5.f); m[at + i] += amp * env * (noise ? nz : sinf(6.2831853f * f * i / kSr)); } };
        static int truth[32]; int nt = 0;
        for(int s16 = 0; s16 < 32; s16++)
        {
            const int at = (int)(s16 * step);
            truth[nt++] = at;
            if(s16 % 8 == 0) burst(at, 60.f, 0.9f, 4000, false);
            else if(s16 % 8 == 4) burst(at, 0.f, 0.8f, 3000, true);
            burst(at, 0.f, 0.03f, 500, true);                          // hat: −30 dB
        }
        auto score = [&](int mode, float& medErr, int& fp) {
            det.mode = mode;
            int nn = det.Detect(m, L, kSr, on, 128, nullptr);
            int hit = 0; float errs[128]; int ne = 0; fp = 0;
            for(int t = 0; t < nt; t++) { int best = 1 << 30; for(int i = 0; i < nn; i++) { int d = abs(on[i] - truth[t]); if(d < best) best = d; } if(best <= (int)(0.010f * kSr)) { hit++; errs[ne++] = (float)best / kSr * 1000.f; } }
            for(int i = 0; i < nn; i++) { int best = 1 << 30; for(int t = 0; t < nt; t++) { int d = abs(on[i] - truth[t]); if(d < best) best = d; } if(best > (int)(0.015f * kSr)) fp++; }
            for(int i = 1; i < ne; i++) for(int j = i; j > 0 && errs[j - 1] > errs[j]; j--) { float t = errs[j]; errs[j] = errs[j - 1]; errs[j - 1] = t; }
            medErr = ne ? errs[ne / 2] : 99.f;
            printf("  mode %d: %d onsets, recall %d/%d, false %d, median |err| %.1f ms\n", mode, nn, hit, nt, fp, medErr);
            return hit; };
        float e0, e1; int fp0, fp1;
        const int h0 = score(0, e0, fp0), h1 = score(1, e1, fp1);
        CHECK(h1 >= 29, "mode 1 recall ≥ 29/32 of the −30 dB hats (%d)", h1);
        CHECK(fp1 <= 1, "mode 1: at most one false onset (%d)", fp1);
        CHECK(h1 >= h0, "mode 1 recall ≥ mode 0 (%d vs %d)", h1, h0);
        CHECK(e1 <= 6.f, "mode 1 timing: median error ≤ 6 ms (%.1f)", e1);
    }

    // A parametric break: kick on 1 and the "and" of 3, snare on 2 and 4,
    // hats on every 16th (odd 16ths at `swing` of their 8th), `lead` samples
    // of hats-only count-in before the first downbeat.
    struct Synth { static int Make(float* m, int maxLen, float bpm, int bars, float swing, int lead, int* ons, int& no, bool snareEveryBeat = false) {
        const float beat = 60.f * kSr / bpm, bar = 4.f * beat, step = beat / 4.f, cell = beat / 2.f;
        const int L = lead + (int)(bars * bar); if(L > maxLen) return 0;
        for(int i = 0; i < L; i++) m[i] = 0.f;
        uint32_t rng = 99; no = 0;
        auto burst = [&](int at, float f, float amp, int len, bool noise) {
            for(int i = 0; i < len && at + i < L; i++) { if(at + i < 0) continue; rng = rng * 1664525u + 1013904223u; float nz = ((float)(rng >> 8) / 8388608.f - 1.f);
                float env = expf(-(float)i / (float)len * 5.f); m[at + i] += amp * env * (noise ? nz : sinf(6.2831853f * f * i / kSr)); } };
        auto pos = [&](int b, int s16) { const int c = s16 / 2; const float off = (s16 & 1) ? swing * cell : 0.f; return lead + (int)(b * bar + c * cell + off); };
        for(int b = -1; b < bars; b++)
            for(int s16 = 0; s16 < 16; s16++)
            {
                const int at = pos(b, s16);
                if(at < 0 || at >= L) continue;
                bool hit = false;
                if(b >= 0 && (s16 == 0 || s16 == 10)) { burst(at, 60.f, 0.9f, 4000, false); hit = true; }
                if(b >= 0 && (s16 == 4 || s16 == 12 || (snareEveryBeat && s16 % 4 == 0))) { burst(at, 0.f, 0.8f, 3000, true); hit = true; }
                burst(at, 0.f, 0.12f, 600, true);
                if(no < 256) ons[no++] = at;
                (void)hit;
            }
        (void)step; return L; } };

    SECTION("SLICE bars: IOI-cluster estimate — tempo, octave, downbeat, varispeed, swing, fallback");
    {
        static float m[48000 * 12]; static int ons[256]; int no;
        auto est = [&](float bpm, int bars, float swing, int lead, bool sev, bb::BarEstimate& e) {
            const int L = Synth::Make(m, 48000 * 12, bpm, bars, swing, lead, ons, no, sev);
            e = bb::EstimateBar(m, L, kSr, ons, no); return L; };
        bb::BarEstimate e;
        const float b96 = 240.f * kSr / 96.f;
        est(96.f, 2, 0.5f, 0, false, e);
        CHECK(e.bar > 0.f && fabsf(e.bar - b96) < 0.02f * b96, "96 BPM, 2 bars: bar %.0f vs %.0f (conf %.2f fit %.2f)", e.bar, b96, e.conf, e.fit);
        CHECK(e.bar > 0.f && (e.downbeat < 0.05f * b96 || e.downbeat > 0.95f * b96), "…downbeat at the start (%.0f)", e.downbeat);
        const float b172 = 240.f * kSr / 172.f;
        est(172.f, 4, 0.5f, 0, false, e);
        CHECK(e.bar > 0.f && fabsf(e.bar - b172) < 0.02f * b172, "172 BPM, 4 bars (jungle): bar %.0f vs %.0f, not the prior's octave", e.bar, b172);
        const int lead = (int)(0.37f * b96);
        est(96.f, 2, 0.5f, lead, false, e);
        CHECK(e.bar > 0.f && fabsf(e.bar - b96) < 0.02f * b96 && fabsf(e.downbeat - (float)lead) < 0.03f * b96, "0.37-bar count-in: bar %.0f, downbeat %.0f vs %d", e.bar, e.downbeat, lead);
        const float b110 = 240.f * kSr / 110.f;
        est(110.f, 2, 0.62f, 0, false, e);
        CHECK(e.bar > 0.f && fabsf(e.bar - b110) < 0.02f * b110, "swung 16ths (ratio 1.6) do not shear the bar: %.0f vs %.0f", e.bar, b110);
        const float b154 = 240.f * kSr / 153.6f;
        est(153.6f, 2, 0.55f, 0, false, e);
        CHECK(e.bar > 0.f && fabsf(e.bar - b154) < 0.02f * b154, "96 BPM material at 1.6× (153.6 BPM): %.0f vs %.0f", e.bar, b154);
        est(96.f, 2, 0.5f, 0, true, e);
        CHECK(e.bar == 0.f || fabsf(e.bar - b96) < 0.02f * b96, "snare on every beat: silent or right, never a wrong confident answer (%.0f)", e.bar);
        CHECK(bb::EstimateBar(gMat, kLen, kSr, nullptr, 0).bar == 0.f, "no onsets → no estimate");
    }

    SECTION("SLICE conditioning: whole bars of the material's own bar, prior as fallback");
    {
        static bb::OnsetDetector det; det.mode = 0;
        static float m[48000 * 12]; static int ons[256]; int no;
        const float b96 = 240.f * kSr / 96.f;
        const int lead = (int)(0.3f * b96);
        int L = Synth::Make(m, 48000 * 12, 96.f, 2, 0.5f, lead, ons, no);
        for(int i = 0; i < (int)(0.2f * b96) && L < 48000 * 12; i++) m[L++] = 0.01f * sinf(0.3f * i);   // a tail of noise
        bb::ConditionInfo ci;
        const int L2 = bb::ConditionBuffer(m, L, kSr, det, &ci);
        CHECK(ci.bars == 2 && fabsf((float)L2 - 2.f * b96) < 0.02f * b96, "count-in + tail → exactly 2 bars (%d bars, %d vs %.0f samples)", ci.bars, L2, 2.f * b96);
        CHECK(abs(ci.trimmedStart - lead) < (int)(0.03f * b96), "…starting at the downbeat (trimmed %d vs lead %d)", ci.trimmedStart, lead);
        // a 4-bar jungle break stays 4 bars (the prior would call it 2 bars of 8ths)
        L = Synth::Make(m, 48000 * 12, 172.f, 4, 0.5f, 0, ons, no);
        const int L3 = bb::ConditionBuffer(m, L, kSr, det, &ci);
        CHECK(ci.bars == 4 && L3 >= (int)(0.99f * L), "172 BPM × 4 bars: kept whole, 4 bars (%d bars, %d of %d)", ci.bars, L3, L);
        CHECK(bb::GridSlices(L3, kSr) == 32, "…where the duration prior alone says 2 bars (%d slices)", bb::GridSlices(L3, kSr));
        // silence → untouched
        static float z[48000]; for(int i = 0; i < 48000; i++) z[i] = 0.f;
        CHECK(bb::ConditionBuffer(z, 48000, kSr, det, &ci) == 48000 && ci.bars == 0, "silence: length kept, no bar");
    }

    SECTION("SLICE swing: estimate; Auto keeps the hats' heads and their timing, straight loses them");
    {
        static float m[48000 * 12]; static int ons[256]; int no;
        const float p = 0.66f;                                           // ratio 1.94
        Synth::Make(m, 48000 * 12, 110.f, 2, p, 0, ons, no);
        const float step = (240.f * kSr / 110.f) / 16.f;
        bb::SwingEstimate sw = bb::EstimateSwing(ons, no, step);
        CHECK(fabsf(sw.p - p) < 0.02f, "swung synthetic: p %.3f vs %.2f (R %.2f, %d off-beat onsets)", sw.p, p, sw.r, sw.nOff);
        Synth::Make(m, 48000 * 12, 110.f, 2, 0.5f, 0, ons, no);
        sw = bb::EstimateSwing(ons, no, step);
        CHECK(sw.p == 0.5f, "straight synthetic: p stays 0.5 (%.3f)", sw.p);

        // Engine material: the tone grid with SWUNG content — step k's tone
        // begins at its swung position (odd steps at p of their 8th).
        static float sm[kLen]; static int son[16];
        for(int k = 0; k < kSteps; k++)
        {
            const int cell = 2 * kStep, c = k / 2;
            const int a = c * cell + ((k & 1) ? (int)(p * cell) : 0);
            const int b = (k + 1 < kSteps) ? ((k + 1) & 1 ? c * cell + (int)(p * cell) : (c + 1) * cell) : kLen;
            son[k] = a;
            for(int i = a; i < b; i++) { float t = (float)(i - a) / kSr; float env = (i - a) < 48 ? (float)(i - a) / 48.f : 1.f; sm[i] = 0.5f * env * sinf(6.2831853f * StepFreq(k) * t); }
        }
        auto run = [&](float swing, int& wrongHeads, int& oddGates, double& oddPhase) {
            bb::Engine es; es = bb::Engine(); es.Init(0x51); es.patternMode = 0; es.patternKnob = 1.f;
            es.swing = swing;
            es.SetMaterial(sm, kLen, kSr, son, 16, gCls, 0);
            es.SetBpm(0.f); es.RegenPattern();
            // parity-keeping rearrangement with no verbatim runs (every step is a
            // fresh trigger): even steps take source k+6, odd steps k+10
            for(int k = 0; k < 16; k++) { es.stepSrc[k] = (int8_t)((k + ((k & 1) ? 10 : 6)) & 15); es.stepCont[k] = false; es.stepRatN[k] = 0; es.stepRev[k] = false; }
            es.gateOnsetsOnly = true;
            static double tlog[256]; es.trigLog = tlog; es.trigLogMax = 256; es.trigLogN = 0;
            Rendered r = RenderLoops(es, 2);
            wrongHeads = 0; oddGates = 0; oddPhase = 0.;
            const double cellSec = 2.0 * kStep / kSr;
            for(int k = 1; k < 16; k += 2)
            {
                // the odd slot's first 12 % of an 8th (before any swung attack could be due)
                const double slotStart = (16 + (k / 2) * 2) * (double)kStep / kSr + (swing > 0.5f ? swing : 0.5) * cellSec;
                const int a = (int)(slotStart * kSr) + 200, n = (int)(0.10 * cellSec * kSr);
                const int src = es.stepSrc[k];
                if(SliceOfFreq(MeasureFreq(&r.y[a], n), Rms(&r.y[a], n)) != src) wrongHeads++;
            }
            oddGates = GateEvents(r, 2);   // hat lane: the odd steps' gates
            int cnt = 0;
            for(int i = 0; i < es.trigLogN; i++) { double ph = fmod(tlog[i] / cellSec, 1.0); if(ph > 0.25 && ph < 0.9) { oddPhase += ph; cnt++; } }
            if(cnt) oddPhase /= cnt;
            return r; };
        int wS, gS, wA, gA; double phS, phA;
        run(0.5f, wS, gS, phS);
        run(p, wA, gA, phA);
        printf("  straight: %d/8 odd slots open with the wrong slice, hat gates %d, odd triggers at %.2f of the 8th\n", wS, gS, phS);
        printf("  auto:     %d/8 odd slots open with the wrong slice, hat gates %d, odd triggers at %.2f of the 8th\n", wA, gA, phA);
        CHECK(wS >= 6, "straight boundaries: the odd slice opens with the even slice's tail (%d/8 heads wrong)", wS);
        CHECK(wA == 0, "Auto: every odd slot opens with its own hat (%d/8 wrong)", wA);
        CHECK(gA >= 2 * gS && gA >= 20, "onsets-only gates: Auto keeps the hat hits (%d vs %d over 2 loops)", gA, gS);
        CHECK(fabs(phA - p) < 0.03 && fabs(phS - 0.5) < 0.03, "odd steps fire at %.2f of the 8th under Auto, 0.50 straight (%.2f)", phA, phS);
        // straight = byte-identical slicing
        bb::Engine e0; e0 = bb::Engine(); e0.Init(1); e0.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls, 0);
        bool same = !e0.swung; for(int k = 0; k < 16; k++) same = same && e0.slices[k].start == k * kStep;
        CHECK(same, "swing 0.5: slices are i·step, swung flag off");
    }

    SECTION("Language hits: map hits gate in Onsets-only; picks prefer slices that open with a drum");
    {
        static const int on4[4] = {0, 4 * kStep, 8 * kStep, 12 * kStep};
        static uint8_t allKick[64]; for(int i = 0; i < 64; i++) allKick[i] = 0;
        // Euclid kick 4/16 = hits at 3,7,11,15; every slice is "kick" but only 0,4,8,12 open with a drum
        Fresh(e, 3, 1.f);
        e.SetMaterial(gMat, kLen, kSr, on4, 4, allKick, 0);
        e.langDens[0] = 4.f / 16.f; e.langDens[1] = e.langDens[2] = 0.f; e.langRot = 0.f;
        e.RegenPattern();
        int hitPicks = 0, hits = 0;
        for(int s = 0; s < 16; s++) if(!e.stepCont[s] && e.stepSrc[s] >= 0) { hits++; hitPicks += e.stepHit[e.stepSrc[s]]; }
        CHECK(hits == 4 && hitPicks == 4, "every map hit plays a slice that opens with a drum (%d/%d)", hitPicks, hits);
        e.gateOnsetsOnly = true;
        Rendered r = RenderLoops(e, 2);
        CHECK(GateEvents(r) == 8, "onsets-only + Euclid: one gate per map hit (%d over 2 loops)", GateEvents(r));
        // even a hit forced onto a non-onset slice still gates in a language mode
        for(int s = 0; s < 16; s++) if(!e.stepCont[s] && e.stepSrc[s] >= 0) e.stepSrc[s] = 1;
        r = RenderLoops(e, 2);
        CHECK(GateEvents(r) == 8, "…and still gates when the slice has no onset (%d)", GateEvents(r));
        // Classic keeps the strict rule: a step whose slice has no onset does not gate
        Fresh(e, 0, 1.f);
        e.SetMaterial(gMat, kLen, kSr, on4, 4, allKick, 0);
        e.RegenPattern(); e.gateOnsetsOnly = true;
        r = RenderLoops(e, 2);
        CHECK(GateEvents(r) == 8, "Classic verbatim, onsets-only: 4 of 16 slices open with a drum → 8 gates (%d)", GateEvents(r));
    }

    SECTION("Zoom tail (default): pitch fills the slot; Play grammar: the bar is what you punch");
    {
        Fresh(e, 0, 1.f);
        e.tailMode = 3; e.pitch = 2.f; e.RegenPattern();
        Rendered r = RenderLoops(e, 2);
        int filled = 0;
        for(int k = 0; k < 14; k++) { const int a = (16 + k) * kStep + kStep * 3 / 4; if(Rms(&r.y[a], kStep / 5) > 0.05f) filled++; }
        CHECK(filled >= 13, "pitch +12, Zoom: the last quarter of each slot still sounds (%d/14)", filled);
        {   // every synthetic slice opens with an attack → zoom must NOT read the next slice: the tail loops its own tone
            int own = 0;
            for(int k = 0; k < 14; k++) { const int a = (16 + k) * kStep + kStep * 3 / 4; const float f = MeasureFreq(&r.y[a], kStep / 5);
                if(fabsf(f - 2.f * StepFreq(e.stepSrc[k])) < 0.15f * 2.f * StepFreq(e.stepSrc[k])) own++; }
            CHECK(own >= 12, "pitch +12, Zoom: the slot's tail is its own slice, not the next one's attack (%d/14)", own);
        }
        Fresh(e, 0, 1.f);
        e.tailMode = 0; e.pitch = 2.f; e.RegenPattern();
        r = RenderLoops(e, 2);
        int gaps = 0;
        for(int k = 0; k < 14; k++) { const int a = (16 + k) * kStep + kStep * 3 / 4; if(Rms(&r.y[a], kStep / 5) < 0.01f) gaps++; }
        CHECK(gaps >= 13, "pitch +12, Gate: the last quarter of each slot is silent (%d/14)", gaps);
        Fresh(e, 0, 1.f);
        e.tailMode = 3; e.pitch = 0.5f; e.RegenPattern();
        r = RenderLoops(e, 2);
        {   // at half rate every slot plays the first half of its own slice, an octave down, for the whole slot
            int ok = 0;
            for(int k = 0; k < 14; k++)
            {
                const int a = (16 + k) * kStep + 300, n = kStep - 600;
                const float f = MeasureFreq(&r.y[a], n), want = 0.5f * StepFreq(e.stepSrc[k]);
                const float tail = Rms(&r.y[(16 + k) * kStep + kStep * 3 / 4], kStep / 5);
                if(fabsf(f - want) < 20.f && tail > 0.05f) ok++;
            }
            CHECK(ok >= 13, "pitch −12, Zoom: each slot plays its own slice an octave down, to the slot end (%d/14)", ok);
        }

        Fresh(e, 5, 1.f);   // Play
        e.looperQuant = 1;  // LOOPER: Quantize = 16ths keeps Play's punch-into-steps memory (Off/8ths: the looper)
        Rendered q = RenderLoops(e, 1);
        CHECK(Rms(q.y.data(), (int)q.y.size()) < 1e-4f, "Play: silent until something is played in");
        e.barPhase = 3.5 / 16.; float o[24]; e.Process(o, 24);
        e.CallSlice(7, 1.f, false, true, false);           // punch slice 7 at step 3
        e.RerollFill();                                     // dice must keep it
        CHECK(e.stepSrc[3] == 7, "Play: a punch survives a regeneration (step 3 = %d)", e.stepSrc[3]);
        q = RenderLoops(e, 2);
        CHECK(SliceAtStep(q, 16 + 3) == 7 && SliceAtStep(q, 16 + 4) == -1, "Play: step 3 sounds slice 7, step 4 stays silent");
        e.Reslice();
        CHECK(e.stepSrc[3] < 0, "Play: B3 (reslice) clears the bar");
    }

    SECTION("Voice calls: staccato length, hit counter for the trigger out");
    {
        Fresh(e, 0, 1.f);
        e.muted = true;   /* only the call sounds */
        e.CallSlice(5, 1.f, false, false, false, 0.5f);
        CHECK(e.callHits == 1 && e.lastCallIdx == 5, "a call counts a hit and remembers its slice");
        float out[24]; e.barPhase = 0.1; float peakEarly = 0.f, peakLate = 0.f;
        for(int b = 0; b < 250; b++) { e.Process(out, 24); for(float v : out) { float a = fabsf(v); if(b < 100 && a > peakEarly) peakEarly = a; if(b > 160 && a > peakLate) peakLate = a; } }
        CHECK(peakEarly > 0.1f && peakLate < 0.02f, "staccato 0.5: sounds for the first half of the slice, silent after (%.2f / %.3f)", peakEarly, peakLate);
        e.callHits = 0;
        e.CallSlice(2, 1.f, false, false, true);            // ratchet: the double fires later
        e.barPhase = 0.2; for(int b = 0; b < 200; b++) e.Process(out, 24);
        CHECK(e.callHits == 2, "a ratcheted call counts two hits (%d)", e.callHits);
    }

    SECTION("Pads grammar: silent, nothing is ever written");
    {
        Fresh(e, 6, 1.f);
        Rendered q = RenderLoops(e, 1);
        CHECK(Rms(q.y.data(), (int)q.y.size()) < 1e-4f, "Pads: silent");
        e.barPhase = 3.5 / 16.; float o[24]; e.Process(o, 24);
        e.CallSlice(7, 1.f, false, true, false);            // even a punching call writes nothing
        e.PunchStep(3, 7, false);                            // nor a web punch
        int written = 0; for(int s = 0; s < 16; s++) written += e.stepSrc[s] >= 0;
        CHECK(written == 0, "Pads: no step is written (%d)", written);
        CHECK(e.callHits == 1, "…but the call sounded (hit counted)");
    }

    /* ── SAMPLE package: key table, held playback, recording, stereo planes ── */
    SECTION("SAMPLE: key table — the span mapping by default, overridable");
    {
        Fresh(e, 0, 1.f);   // 16 slices: keys 0..15 are the slices, 16..23 the last one
        int wrong = 0;
        for(int k = 0; k < 16; k++) if(e.KeySlice(k) != k) wrong++;
        CHECK(wrong == 0 && e.KeySlice(20) == 15 && e.KeySlice(23) == 15 && e.KeySlice(99) == 15,
              "16 slices: key k → slice k, keys past the span → the last slice (%d wrong, 20→%d)", wrong, e.KeySlice(20));
        CHECK(e.nTable == 16 && e.freePos == kLen && e.bufCap == kLen, "no recordings: the table is the grid, no room past it");
        static float longBuf[48000 * 9];
        for(int i = 0; i < 48000 * 9; i++) longBuf[i] = 0.1f * sinf(0.01f * i);
        bb::Engine e2; e2.Init(1); e2.SetMaterial(longBuf, 48000 * 9, kSr, nullptr, 0, nullptr);
        wrong = 0;
        for(int k = 0; k < 24; k++) if(e2.KeySlice(k) != (k * 64) / 24) wrong++;
        CHECK(wrong == 0, "64 slices: key k → k·64/24 (%d wrong)", wrong);
        CHECK(e.SetKeySlice(3, 9) && e.KeySlice(3) == 9, "SetKeySlice overrides");
        CHECK(!e.SetKeySlice(3, 16) && !e.SetKeySlice(24, 1) && e.KeySlice(3) == 9, "…and refuses out-of-range slices and keys");
        Fresh(e, 0, 1.f);
        CHECK(e.KeySlice(3) == 3, "SetMaterial resets the table");
    }

    SECTION("SAMPLE: a held call reads past its slice; ReleaseKey fades it out without a click");
    {
        Fresh(e, 0, 1.f);
        e.muted = true;   // only the calls sound
        float out[24];
        auto renderN = [&](std::vector<float>& y, int nSamp) {
            for(int i = 0; i < nSamp; i += 24) { e.Process(out, 24); y.insert(y.end(), out, out + 24); }
        };
        std::vector<float> one, held;
        e.CallSlice(5, 1.f, false, false, false);                 // one-shot: today's behaviour
        renderN(one, 2 * kStep);
        Fresh(e, 0, 1.f); e.muted = true;
        e.CallSlice(5, 1.f, false, false, false, 1.f, true, 5);   // held, tagged key 5
        renderN(held, 2 * kStep);
        CHECK(SliceOfFreq(MeasureFreq(&one[300], kStep - 600), Rms(&one[300], kStep - 600)) == 5
              && SliceOfFreq(MeasureFreq(&held[300], kStep - 600), Rms(&held[300], kStep - 600)) == 5,
              "both calls open with slice 5");
        CHECK(Rms(&one[kStep + 300], kStep - 600) < 1e-3f, "one-shot: silent after its slice");
        CHECK(SliceOfFreq(MeasureFreq(&held[kStep + 300], kStep - 600), Rms(&held[kStep + 300], kStep - 600)) == 6,
              "held: reads on into slice 6 (natEnd = the material's end)");
        int hv = -1;
        for(int i = 0; i < bb::Engine::kVoices; i++) if(e.voices[i].active && e.voices[i].held) hv = i;
        CHECK(hv >= 0 && e.voices[hv].key == 5 && e.voices[hv].natEnd == kLen, "the held voice carries its key and reads to the end of the buffer");
        e.ReleaseKey(4);                                          // the wrong key: nothing happens
        renderN(held, 24);
        CHECK(e.voices[hv].active && e.voices[hv].held, "ReleaseKey on another key is a no-op");
        const size_t relAt = held.size();
        e.ReleaseKey(5);
        renderN(held, 480);
        CHECK(!e.voices[hv].active, "ReleaseKey(5): the voice has ended within 10 ms (5 ms choke fade)");
        CHECK(Rms(&held[relAt + 300], 180) < 2e-3f, "…and is silent after the fade (rms %.4f)", Rms(&held[relAt + 300], 180));
        std::vector<float> tail(held.begin() + (long)relAt - 48, held.end());
        const float ms = MaxStep(tail);
        CHECK(ms < 0.08f, "no click at the release: max |Δ| %.3f (a hard cut would step ~0.4)", ms);
        // a held reversed call reads BACK to the start of the material
        Fresh(e, 0, 1.f); e.muted = true;
        e.CallSlice(2, 1.f, true, false, false, 1.f, true, 2);
        std::vector<float> rv; renderN(rv, 3 * kStep);
        CHECK(SliceOfFreq(MeasureFreq(&rv[300], kStep - 600), Rms(&rv[300], kStep - 600)) == 2
              && SliceOfFreq(MeasureFreq(&rv[kStep + 300], kStep - 600), Rms(&rv[kStep + 300], kStep - 600)) == 1
              && SliceOfFreq(MeasureFreq(&rv[2 * kStep + 300], kStep - 600), Rms(&rv[2 * kStep + 300], kStep - 600)) == 0,
              "held reverse: slice 2, then 1, then 0");
        // the sequencer's continuity branch leaves a held voice alone
        Fresh(e, 0, 1.f);   // verbatim: step 1 would otherwise adopt a voice sitting at slice 1's start
        e.extPhase = true; e.barPhase = 0.; e.Process(out, 24);
        e.CallSlice(0, 1.f, false, false, false, 1.f, true, 0);
        for(int b = 1; b < 600; b++) { e.barPhase = (double)b * 24. / kSr / (double)e.barSeconds; e.Process(out, 24); }
        hv = -1;
        for(int i = 0; i < bb::Engine::kVoices; i++) if(e.voices[i].active && e.voices[i].held) hv = i;
        CHECK(hv >= 0 && e.voices[hv].natEnd == kLen, "a held voice keeps its end through the grid's steps (continuity does not adopt it)");
    }

    SECTION("SAMPLE: record into a key — arm, feed, stop, trim, analyse, commit, play");
    {
        static float big[kLen + 48000];
        memcpy(big, gMat, sizeof gMat);
        const int cap = kLen + 48000;
        e = bb::Engine(); e.Init(0x1234); e.patternMode = 0; e.patternKnob = 1.f;
        e.SetMaterial(big, kLen, kSr, nullptr, 0, gCls, 0, nullptr, cap);
        e.SetBpm(0.f); e.RegenPattern(); e.muted = true;
        CHECK(e.freePos == kLen && e.bufCap == cap && e.nTable == 16, "fresh material: freePos = len, capacity as given");
        auto take = [&](int key, int nSamp, float f, int& ts, int& tl, uint8_t& cls) -> int {
            if(!e.RecArm(key)) return -100;
            static float in[24]; float out[24]; int fed = 0;
            for(int i = 0; i < nSamp; i += 24)   // the audio thread: feed, then render, block by block
            {
                for(int j = 0; j < 24; j++) in[j] = 0.3f * sinf(6.2831853f * f * (float)(fed + j) / kSr);
                e.RecFeed(in, 24); e.Process(out, 24); fed += 24;
            }
            int rs = 0, rl = 0;
            const int got = e.RecStop(rs, rl);
            if(got != nSamp || rs != e.freePos) return -200;
            if(!bb::TrimRegion(big, nullptr, rs, rl, kSr, ts, tl)) return -300;
            float sal = 0.f, dens = 0.f; bool hit = false;
            bb::AnalyseRegion(big, ts, tl, kSr, 0, cls, sal, dens, hit);
            if(!(sal == 1.f && dens > 0.f && hit)) return -400;
            return e.CommitSlice(key, ts, tl, cls, sal, dens, hit);
        };
        int ts = 0, tl = 0; uint8_t cls = 9;
        const int t1 = take(3, 12000, 1000.f, ts, tl, cls);
        CHECK(t1 == 16, "first take into key 3 → table entry 16 (got %d)", t1);
        CHECK(e.nTable == 17 && e.KeySlice(3) == 16 && e.freePos == ts + tl && e.freePos > kLen,
              "the table grew, key 3 points at it, freePos advanced (nTable %d key→%d free %d)", e.nTable, e.KeySlice(3), e.freePos);
        CHECK(tl >= 11000 && tl <= 12000 && e.slices[16].start == ts && e.slices[16].len == tl, "the trimmed region is the slice (%d samples)", tl);
        CHECK(cls == 1 && e.stepClass[16] == 1 && e.stepHit[16] && e.sliceSal[16] == 1.f, "a 1 kHz tone reads as the mid band (snare), a hit, full salience");
        {
            float pk = 0.f; for(int i = ts; i < ts + tl; i++) { const float a = fabsf(big[i]); if(a > pk) pk = a; }
            CHECK(fabsf(pk - 0.95f) < 0.01f, "normalised to 0.95 (peak %.3f)", pk);
        }
        std::vector<float> y; float out[24];
        e.CallSlice(e.KeySlice(3), 1.f, false, false, false);
        for(int i = 0; i < 4800; i += 24) { e.Process(out, 24); y.insert(y.end(), out, out + 24); }
        CHECK(fabsf(MeasureFreq(&y[300], 4000) - 1000.f) < 30.f && Rms(&y[300], 4000) > 0.1f,
              "key 3 plays the recording (%.0f Hz, rms %.2f)", MeasureFreq(&y[300], 4000), Rms(&y[300], 4000));
        // metadata survives the shell's grid-wide copies
        e.stepClass[16] = 3; e.sliceSal[16] = 0.f; e.SyncRecordedMeta();
        CHECK(e.stepClass[16] == 1 && e.sliceSal[16] == 1.f, "SyncRecordedMeta restores a recorded entry's metadata");
        // arm refusals
        CHECK(e.RecArm(3) && !e.RecArm(4), "a second arm while armed is refused");
        { int rs, rl; e.RecStop(rs, rl); CHECK(rl == 0 && !e.recPending, "stopping an unfed take records nothing"); }
        // a second take into the same key replaces the entry
        const int free1 = e.freePos;
        const int t2 = take(3, 9600, 700.f, ts, tl, cls);
        CHECK(t2 == 16 && e.nTable == 17 && e.KeySlice(3) == 16 && e.freePos > free1 && e.slices[16].start == ts,
              "second take into key 3 replaces entry 16 (got %d, nTable %d)", t2, e.nTable);
        y.clear(); e.CallSlice(e.KeySlice(3), 1.f, false, false, false);
        for(int i = 0; i < 4800; i += 24) { e.Process(out, 24); y.insert(y.end(), out, out + 24); }
        CHECK(fabsf(MeasureFreq(&y[300], 4000) - 700.f) < 30.f, "…and the key now plays it (%.0f Hz)", MeasureFreq(&y[300], 4000));
        // a different key: a new entry; then a take that hits the cap stops itself
        const int t3 = take(7, 6000, 1500.f, ts, tl, cls);
        CHECK(t3 == 17 && e.nTable == 18 && e.KeySlice(7) == 17 && e.KeySlice(3) == 16, "key 7 → entry 17 (got %d)", t3);
        CHECK(e.RecArm(8), "arm key 8 for a take longer than the room left");
        {
            static float in[24]; for(int j = 0; j < 24; j++) in[j] = 0.2f;
            const int room = e.bufCap - e.freePos;
            for(int i = 0; i < room + 4800; i += 24) e.RecFeed(in, 24);
            int rs = 0, rl = 0; const int got = e.RecStop(rs, rl);
            CHECK(e.recFull && got == room && rs + rl == e.bufCap, "the take stops itself at bufCap (got %d of %d)", got, room);
            e.RecDiscard();
            CHECK(!e.recPending && e.nTable == 18 && e.freePos == rs, "discarded: nothing committed, the memory is free again");
        }
        // a recording that cannot fit is refused at arm time
        {
            static float in[24]; for(int j = 0; j < 24; j++) in[j] = 0.2f;
            CHECK(e.RecArm(9), "arm key 9");
            const int room = e.bufCap - e.freePos;
            for(int i = 0; i < room; i += 24) e.RecFeed(in, 24);
            int rs = 0, rl = 0; e.RecStop(rs, rl);
            int s2 = 0, l2 = 0;
            CHECK(bb::TrimRegion(big, nullptr, rs, rl, kSr, s2, l2), "the full take trims");
            float sal, dens; bool hit; uint8_t c2;
            bb::AnalyseRegion(big, s2, l2, kSr, 0, c2, sal, dens, hit);
            CHECK(e.CommitSlice(9, s2, l2, c2, sal, dens, hit) == 18 && e.bufCap - e.freePos < bb::Engine::kRecMin, "…and commits, filling the plane");
            CHECK(!e.RecArm(10), "no room left: RecArm refuses");
            CHECK(e.CommitSlice(10, e.bufCap - 100, 200, 0, 1.f, 0.f, true) < 0
                  && e.CommitSlice(10, 0, bb::Engine::kRecMin - 1, 0, 1.f, 0.f, true) < 0, "CommitSlice refuses a region past the cap or under kRecMin");
        }
        // a re-slice of the same planes keeps the recordings; new material drops them
        {
            const int keyEntry = e.KeySlice(3);
            const bb::Slice s3 = e.slices[keyEntry];
            const int free3 = e.freePos;
            e.SetMaterial(big, kLen, kSr, nullptr, 0, gCls, 32, nullptr, cap);   // "2 bars" over the same material
            CHECK(e.nSlices == 32 && e.nTable == 32 + 3 && e.freePos == free3, "re-slice to 32: the three recordings follow (nTable %d)", e.nTable);
            CHECK(e.KeySlice(3) == 32 && e.slices[32].start == s3.start && e.slices[32].len == s3.len && e.stepClass[32] == 1,
                  "key 3's take is entry 32 now, same region, same class");
            CHECK(e.KeySlice(7) == 33 && e.KeySlice(9) == 34 && e.KeySlice(4) == (4 * 32) / 24, "the other keys follow; plain keys span the new grid (key 4 → %d)", e.KeySlice(4));
            e.SetMaterial(big, kLen, kSr, nullptr, 0, gCls, 0, nullptr, cap);    // back to 16
            CHECK(e.nSlices == 16 && e.nTable == 19 && e.KeySlice(3) == 16 && e.slices[16].start == s3.start, "…and back to 16: entry 16 again");
            e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls);                      // other planes: new material
            CHECK(e.nTable == 16 && e.freePos == kLen && e.KeySlice(3) == 3, "new material: the table starts over");
        }
    }

    SECTION("SAMPLE: stereo planes — R renders independently, the mono path is byte-identical");
    {
        static float sL[kLen], sR[kLen];
        memcpy(sL, gMat, sizeof gMat);
        for(int i = 0; i < kLen; i++) sR[i] = 0.5f * sinf(6.2831853f * 1000.f * (float)i / kSr);   // R: a 1 kHz tone everywhere
        for(int i = 0; i < kStep; i++) sR[15 * kStep + i] = 0.f;
        e = bb::Engine(); e.Init(0x1234); e.patternMode = 0; e.patternKnob = 1.f;
        e.SetMaterial(sL, kLen, kSr, nullptr, 0, gCls, 0, sR, kLen);
        e.SetBpm(0.f); e.RegenPattern(); e.muted = true;
        std::vector<float> yl, yr; float ol[24], orr[24];
        e.CallSlice(3, 1.f, false, false, false);
        for(int i = 0; i < 4800; i += 24) { e.Process(ol, orr, 24); yl.insert(yl.end(), ol, ol + 24); yr.insert(yr.end(), orr, orr + 24); }
        CHECK(SliceOfFreq(MeasureFreq(&yl[300], 4000), Rms(&yl[300], 4000)) == 3, "L plays slice 3's tone");
        CHECK(fabsf(MeasureFreq(&yr[300], 4000) - 1000.f) < 30.f && Rms(&yr[300], 4000) > 0.1f, "R plays the R plane (%.0f Hz)", MeasureFreq(&yr[300], 4000));
        // the stereo entry point on mono material: R is a copy of L
        Fresh(e, 0, 1.f);
        Rendered r1 = RenderLoops(e, 1);
        Fresh(e, 0, 1.f);
        e.extPhase = true; std::vector<float> ml, mr; double t = 0.;
        while(t < (double)e.barSeconds - 1e-9) { e.barPhase = fmod(t / (double)e.barSeconds, 1.0); e.Process(ol, orr, 24); ml.insert(ml.end(), ol, ol + 24); mr.insert(mr.end(), orr, orr + 24); t += 24. / kSr; }
        CHECK(ml == r1.y && mr == ml, "Process(L, R) on mono material: L == Process(out), R == L");
        // stereo recording lands in both planes; a mono feed is copied to R
        {
            static float pL[kLen + 4800], pR[kLen + 4800];
            memcpy(pL, gMat, sizeof gMat); memcpy(pR, sR, sizeof sR);
            e = bb::Engine(); e.Init(1); e.SetMaterial(pL, kLen, kSr, nullptr, 0, gCls, 0, pR, kLen + 4800);
            float inL[24], inR[24]; for(int j = 0; j < 24; j++) { inL[j] = 0.25f; inR[j] = -0.5f; }
            CHECK(e.RecArm(0), "stereo arm");
            for(int i = 0; i < 960; i += 24) e.RecFeed(inL, inR, 24);
            for(int i = 0; i < 960; i += 24) e.RecFeed(inL, 24);
            int rs, rl; e.RecStop(rs, rl);
            CHECK(rl == 1920 && pL[rs] == 0.25f && pR[rs] == -0.5f && pL[rs + 1000] == 0.25f && pR[rs + 1000] == 0.25f,
                  "R plane takes the R input, then the L input when none is given");
            e.RecDiscard();
        }
        // GOLDEN: calls, chokes, a steal, adaptive fades, zoom and a ratchet on
        // the mono path — FNV-1a of the rendered float bits computed from the
        // untouched engine (commit 4136106, before SAMPLE) with this exact
        // driver; identical at -O1 sanitized and -O2 on x86-64 (SSE, no FMA).
        // The same driver through the stereo entry point, on mono material and
        // on stereo material whose R plane duplicates L, must match it too.
        auto drive = [&](bb::Engine& g, float* L, float* R, int mode) -> std::vector<float> {
            g = bb::Engine(); g.Init(0x1234); g.patternMode = 0; g.patternKnob = 0.3f;
            g.SetMaterial(L, kLen, kSr, nullptr, 0, gCls, 0, R, kLen); g.SetBpm(0.f); g.RegenPattern();
            g.fadeMode = 1; g.tailMode = 3; g.pitch = 1.2f;
            for(int k = 0; k < 16; k++) g.sliceDens[k] = 0.01f + 0.002f * (float)k;
            g.RegenPattern(); g.extPhase = true;
            std::vector<float> y; float o[24], o2[24];
            const int blocks = (int)(2.0 * g.barSeconds * kSr / 24.0);
            for(int b = 0; b < blocks; b++)
            {
                if(b == 40)  g.CallSlice(5, 1.f, false, false, false, 0.5f);
                if(b == 90)  g.CallSlice(9, 0.5f, true, true, true);
                if(b == 150) g.CallSlice(2, 2.f, false, false, false);
                if(b == 152) g.CallSlice(3, 1.f, false, false, false);   // steal right behind
                g.barPhase = fmod((double)b * 24. / kSr / (double)g.barSeconds, 1.0);
                if(mode == 0) g.Process(o, 24);
                else { g.Process(o, o2, 24); if(mode == 2) memcpy(o, o2, sizeof o); }
                y.insert(y.end(), o, o + 24);
            }
            return y;
        };
        const uint64_t gold = 0x786e423d477968a2ULL;
        static float dupR[kLen]; memcpy(dupR, gMat, sizeof gMat);
        const uint64_t hMono   = Fnv(drive(e, gMat, nullptr, 0));
        const uint64_t hStL    = Fnv(drive(e, gMat, nullptr, 1));
        const uint64_t hDupL   = Fnv(drive(e, gMat, dupR, 1));
        const uint64_t hDupR   = Fnv(drive(e, gMat, dupR, 2));
        CHECK(hMono == gold, "mono Process: checksum %016llx != golden %016llx", (unsigned long long)hMono, (unsigned long long)gold);
        CHECK(hStL == gold, "Process(L, R) on mono material: L checksum %016llx != golden", (unsigned long long)hStL);
        CHECK(hDupL == gold && hDupR == gold, "stereo material with R = L: both planes match the golden (%016llx / %016llx)",
              (unsigned long long)hDupL, (unsigned long long)hDupR);
    }

    /* ── LOOPER package: the unquantized looper behind Play ───────────────── */
    // Play, Quantize as given, one block rendered at phase 0 so the window
    // frontier is live (as it always is on the module), hit counter cleared.
    auto Prime = [&](int quant) {
        Fresh(e, 5, 1.f); e.looperQuant = quant; e.extPhase = true; e.barPhase = 0.;
        float o[24]; e.Process(o, 24); e.callHits = 0;
    };
    // One block past the loop's end: the playhead CROSSES the wrap (a render
    // that stops on the boundary has not), which is what closes a generation.
    auto Wrap = [&]() { float o[24]; e.barPhase = 0.; e.Process(o, 24); };
    auto FirstNonzero = [](const std::vector<float>& y, size_t from) -> long {
        for(size_t i = from; i < y.size(); i++) if(y[i] != 0.f) return (long)i;
        return -1;
    };

    SECTION("LOOPER: a call in Play writes the loop, not a step; replays every pass after the one being played");
    {
        Prime(0);
        CHECK(e.LooperActive() && e.looper.n == 0 && e.looper.divisions == 16, "Play + Quantize Off: the looper is on, empty, 16 divisions");
        e.barPhase = 0.3;
        e.CallSlice(7, 1.f, false, true, false);            // `punch` in Play = write (into the loop now)
        CHECK(e.looper.n == 1 && e.stepSrc[4] < 0 && e.playSrc[4] < 0, "written into the loop, not into step 4 (n %d, step 4 = %d)", e.looper.n, e.stepSrc[4]);
        const bb::Looper::Event& ev = e.looper.ev[0];
        CHECK(fabs(ev.phase - 0.3) < 1e-12 && ev.key == -1 && ev.slice == 7 && ev.gen == 0 && (ev.style & bb::Looper::kSkip),
              "the event: phase 0.3, keyless → slice 7, generation 0, ahead of the playhead → skipped once");
        CHECK(e.callHits == 1, "the live call sounded (%d)", e.callHits);
        e.callHits = 0;
        Rendered q = RenderLoops(e, 3);
        CHECK(e.callHits == 2, "fires on passes 2 and 3, not on the pass the player is hearing (%d hits)", e.callHits);
        const size_t at = (size_t)(0.3 * kLen);
        CHECK(Rms(&q.y[at + 300], 5400) < 1e-4f, "pass 1: silent at 0.3 (rms %.5f)", Rms(&q.y[at + 300], 5400));
        CHECK(SliceOfFreq(MeasureFreq(&q.y[kLen + at + 300], 5400), Rms(&q.y[kLen + at + 300], 5400)) == 7
              && SliceOfFreq(MeasureFreq(&q.y[2 * kLen + at + 300], 5400), Rms(&q.y[2 * kLen + at + 300], 5400)) == 7,
              "passes 2 and 3: slice 7 sounds at 0.3");
        CHECK(!(e.looper.ev[0].style & bb::Looper::kSkip) && e.looper.n == 1, "the skip is spent; replays never re-record (n %d)", e.looper.n);
        // a call with punch = false (style 6 in Play: choke) sounds but is not written
        e.barPhase = 0.6; e.CallSlice(2, 1.f, false, false, false);
        CHECK(e.looper.n == 1, "an unwritten call leaves the loop alone");
    }

    SECTION("LOOPER: sample-exact within a 24-sample block — an event one sample later renders one sample later");
    {
        std::vector<float> ya, yb;
        for(int which = 0; which < 2; which++)
        {
            Prime(0);
            const double ph = (double)(28807 + which) / (double)kLen;   // 7 (then 8) samples into block 1200
            CHECK(e.looper.Add(ph, -1, 7, 0, 1.f), "Add (the loader's entry: no skip)");
            e.looper.Seal();
            Rendered q = RenderLoops(e, 1);
            (which ? yb : ya) = q.y;
        }
        const long fa = FirstNonzero(ya, 20000), fb = FirstNonzero(yb, 20000);
        CHECK(fa >= 28807 && fa <= 28810 && fb == fa + 1, "onsets at %ld and %ld: one sample apart, at the event's own sample", fa, fb);
        bool same = true;   // up to the voice's natural end: the click-decay after an end is block-aligned (pre-LOOPER semantics, kept)
        for(size_t i = 28000; i + 1 < 34000; i++) if(yb[i + 1] != ya[i]) { same = false; break; }
        CHECK(same, "the two renders are the same samples, shifted by exactly one (segmented render is exact)");
    }

    SECTION("LOOPER: a held note replays with its release");
    {
        Prime(0);
        e.barPhase = 0.25; e.CallSlice(e.KeySlice(3), 1.f, false, true, false, 1.f, true, 3);
        CHECK(e.looper.n == 1 && (e.looper.ev[0].style & bb::Looper::kHeld) && e.looper.ev[0].key == 3 && e.looper.heldMask == (1u << 3),
              "held note-on recorded, key 3 awaiting its release");
        e.barPhase = 0.5; e.ReleaseKey(3);
        CHECK(e.looper.n == 2 && (e.looper.ev[1].style & bb::Looper::kRelease) && e.looper.ev[1].key == 3 && e.looper.heldMask == 0
              && fabs(e.looper.ev[1].phase - 0.5) < 1e-12, "…and its release at 0.5");
        e.ReleaseKey(3);
        CHECK(e.looper.n == 2, "a release with no held note-on pending records nothing");
        Rendered q = RenderLoops(e, 2);
        CHECK(SliceAtStep(q, 16 + 3) == -1 && SliceAtStep(q, 16 + 4) == 3 && SliceAtStep(q, 16 + 6) == 5 && SliceAtStep(q, 16 + 7) == 6,
              "pass 2: from 0.25 the held key 3 reads on through slices 3, 4, 5, 6 (%d %d %d %d)",
              SliceAtStep(q, 16 + 3), SliceAtStep(q, 16 + 4), SliceAtStep(q, 16 + 6), SliceAtStep(q, 16 + 7));
        CHECK(SliceAtStep(q, 16 + 8) == -1 && SliceAtStep(q, 16 + 9) == -1, "…and stops at the recorded release (0.5)");
        int heldNow = 0; for(int i = 0; i < bb::Engine::kVoices; i++) heldNow += e.voices[i].active && e.voices[i].held;
        CHECK(heldNow == 0, "no held voice outlives its release (%d)", heldNow);
        // the loop follows the key table: re-point key 3 and the replay plays the new slice
        e.SetKeySlice(3, 9);
        q = RenderLoops(e, 2);
        CHECK(SliceAtStep(q, 16 + 4) == 9, "key 3 → slice 9 now: the loop replays through the key (%d)", SliceAtStep(q, 16 + 4));
    }

    SECTION("LOOPER: Quantize 16ths = Play's punch, untouched; 8ths snaps the phase at record time");
    {
        Prime(1);
        CHECK(!e.LooperActive(), "16ths: the looper is off");
        e.barPhase = 3.5 / 16.; float o[24]; e.Process(o, 24);
        e.CallSlice(7, 1.f, false, true, false);
        CHECK(e.looper.n == 0 && e.stepSrc[3] == 7 && e.playSrc[3] == 7, "16ths: the call punches step 3, records nothing (n %d, step 3 = %d)", e.looper.n, e.stepSrc[3]);
        Rendered a = RenderLoops(e, 2);
        Fresh(e, 5, 1.f); e.looperQuant = 1; e.extPhase = true; e.barPhase = 0.; e.Process(o, 24);
        e.barPhase = 3.5 / 16.; e.Process(o, 24);
        e.PunchStep(3, 7, false);                            // the web's punch: the same memory
        Rendered b = RenderLoops(e, 2);
        bool same = a.y.size() == b.y.size();
        for(size_t i = kLen; same && i < a.y.size(); i++) if(a.y[i] != b.y[i]) same = false;
        CHECK(same, "16ths: a punched call and a web punch render the same second pass (byte-identical)");
        Prime(2);
        e.barPhase = 0.3;  e.CallSlice(7, 1.f, false, true, false);
        e.barPhase = 0.98; e.CallSlice(2, 1.f, true, true, false);
        CHECK(e.looper.n == 2 && e.stepSrc[4] < 0, "8ths: written into the loop, no step punched (n %d)", e.looper.n);
        CHECK(e.looper.ev[0].phase == 0.0 && e.looper.ev[1].phase == 0.25, "8ths: 0.3 → 0.25 (2/8), 0.98 → the downbeat (%.4f %.4f)", e.looper.ev[0].phase, e.looper.ev[1].phase);
        CHECK(e.looper.ev[0].slice == 2 && (e.looper.ev[0].style & bb::Looper::kRev) && e.looper.ev[1].slice == 7, "…sorted by phase, styles kept");
        CHECK(bb::Looper::Snap(0.3, 16) == 0.3125 && bb::Looper::Snap(0.99, 16) == 0.0 && bb::Looper::Snap(0.3, 0) == 0.3, "Snap: nearest division, 1.0 wraps to 0, no division = exact");
        e.callHits = 0;
        Rendered q = RenderLoops(e, 2);
        CHECK(e.callHits == 2 && SliceAtStep(q, 16 + 4) == 7, "8ths: pass 2 plays slice 7 on step 4 (%d hits, step 4 = %d)", e.callHits, SliceAtStep(q, 16 + 4));
    }

    SECTION("LOOPER: undo removes only the last generation (a pass with something recorded)");
    {
        Prime(0);
        e.barPhase = 0.2; e.CallSlice(2, 1.f, false, true, false);
        e.barPhase = 0.4; e.CallSlice(4, 1.f, false, true, false);
        RenderLoops(e, 1);
        CHECK(e.looper.gen == 0 && e.looper.dirty, "up to the loop's end the generation is still open (gen %d)", e.looper.gen);
        Wrap();                                              // crossing the wrap closes generation 0
        CHECK(e.looper.gen == 1 && !e.looper.dirty, "a wrap after recording opens generation 1 (gen %d)", e.looper.gen);
        RenderLoops(e, 1); Wrap();
        CHECK(e.looper.gen == 1, "a pass with nothing recorded does not open a generation");
        e.barPhase = 0.6; e.CallSlice(6, 1.f, false, true, false);
        RenderLoops(e, 1); Wrap();
        e.barPhase = 0.8; e.CallSlice(8, 1.f, false, true, false);   // generation 2, still open
        CHECK(e.looper.n == 4 && e.looper.ev[0].gen == 0 && e.looper.ev[1].gen == 0 && e.looper.ev[2].gen == 1 && e.looper.ev[3].gen == 2,
              "four events in generations 0 0 1 2 (%d %d %d %d)", e.looper.ev[0].gen, e.looper.ev[1].gen, e.looper.ev[2].gen, e.looper.ev[3].gen);
        CHECK(e.looper.UndoLast() && e.looper.n == 3 && e.looper.ev[2].slice == 6 && e.looper.gen == 2 && !e.looper.dirty,
              "undo: the open generation goes, the rest stay (n %d)", e.looper.n);
        e.barPhase = 0.9; e.CallSlice(9, 1.f, false, true, false);
        CHECK(e.looper.n == 4 && e.looper.ev[3].gen == 2, "a new overdub takes the freed generation");
        CHECK(e.looper.UndoLast() && e.looper.UndoLast() && e.looper.n == 2 && e.looper.ev[0].slice == 2 && e.looper.ev[1].slice == 4 && e.looper.gen == 1,
              "two more undos: back to the first pass, generation 1 free again (n %d, gen %d)", e.looper.n, e.looper.gen);
        CHECK(e.looper.UndoLast() && e.looper.n == 0 && e.looper.gen == 0 && !e.looper.UndoLast(), "the last undo empties; an empty loop refuses");
        // the generation byte wraps: 256 sealed generations still undo one at a time
        bb::Looper L;
        for(int i = 0; i < 256; i++) { L.Add((double)i / 256., -1, (int8_t)(i & 15), 0, 1.f); L.Seal(); }
        CHECK(L.n == 256 && L.UndoLast() && L.n == 255 && L.UndoLast() && L.n == 254, "256 generations: undo still peels one (n %d)", L.n);
    }

    SECTION("LOOPER: clear (B3 in Play) empties; only Play records; the loop waits through other grammars");
    {
        Prime(0);
        e.barPhase = 0.2; e.CallSlice(2, 1.f, false, true, false);
        e.barPhase = 0.4; e.CallSlice(4, 1.f, false, true, false);
        e.Reslice();
        CHECK(e.looper.n == 0 && e.looper.gen == 0, "Reslice in Play clears the loop with the bar");
        e.barPhase = 0.2; e.CallSlice(2, 1.f, false, true, false);
        e.looper.Clear();
        CHECK(e.looper.n == 0 && e.looper.heldMask == 0, "Clear empties");
        Fresh(e, 0, 1.f); e.extPhase = true; e.barPhase = 0.3; float o[24]; e.Process(o, 24);
        e.CallSlice(7, 1.f, false, true, false);
        CHECK(e.looper.n == 0 && e.stepSrc[4] == 7, "Classic: a punch is a punch, nothing recorded");
        Fresh(e, 6, 1.f); e.extPhase = true; e.barPhase = 0.3; e.Process(o, 24);
        e.CallSlice(7, 1.f, false, true, false);
        CHECK(e.looper.n == 0, "Pads: nothing is ever written");
        Prime(0);
        e.barPhase = 0.6; e.CallSlice(6, 1.f, false, true, false);
        e.patternMode = 0; e.RegenPattern(); e.callHits = 0;
        RenderLoops(e, 2);
        CHECK(e.looper.n == 1 && e.callHits == 0, "another grammar: the loop is kept but silent (%d hits)", e.callHits);
        e.patternMode = 5; e.RegenPattern(); e.callHits = 0;
        RenderLoops(e, 2);
        CHECK(e.callHits == 2, "back in Play it plays again (%d hits)", e.callHits);
        // a re-slice of the same material keeps the loop; new material drops it
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls, 32);
        CHECK(e.looper.n == 1 && e.looper.divisions == 32, "the same planes re-sliced: the loop stays, 32 divisions");
        static float other[kLen]; memcpy(other, gMat, sizeof gMat);
        e.SetMaterial(other, kLen, kSr, nullptr, 0, gCls);
        CHECK(e.looper.n == 0, "new material: the loop starts empty");
    }

    // Render `loops` loops at `block` samples, counting hits per block (callHits is a byte).
    auto RenderHits = [&](int loops, int block, std::vector<float>& y) -> int {
        int hits = 0; y.clear();
        e.extPhase = true; double t = 0.; float out[128];
        const double total = (double)loops * e.barSeconds;
        while(t < total - 1e-9)
        {
            e.barPhase = fmod(t / (double)e.barSeconds, 1.0);
            e.Process(out, block); y.insert(y.end(), out, out + block);
            hits += e.callHits; e.callHits = 0;
            t += (double)block / kSr;
        }
        return hits;
    };

    SECTION("LOOPER: events across the wrap fire every pass, at the same sample whatever the block size");
    {
        std::vector<float> y;
        Prime(0); e.looper.Add(0.999, -1, 3, 0, 1.f); e.looper.Seal();    // 96 samples before the wrap
        int h = RenderHits(2, 24, y);
        long f = FirstNonzero(y, 90000);
        CHECK(h == 2 && f >= 95904 && f <= 95907, "0.999: one hit per pass, onset at sample 95904+ (%d hits, %ld)", h, f);
        Prime(0); e.looper.Add(0.0005, -1, 5, 0, 1.f); e.looper.Seal();   // 48 samples after it
        h = RenderHits(2, 24, y);
        f = FirstNonzero(y, 40);
        CHECK(h == 2 && f >= 48 && f <= 51, "0.0005: one hit per pass, onset at sample 48+ (%d hits, %ld)", h, f);
        f = FirstNonzero(y, (size_t)kLen + 40);
        CHECK(f >= kLen + 48 && f <= kLen + 51, "…and again at 96048+ on pass 2 (%ld)", f);
        std::vector<float> y24, y7;
        int hits24 = 0, hits7 = 0;
        for(int which = 0; which < 2; which++)
        {
            Prime(0);
            e.looper.Add(0.999, -1, 3, 0, 1.f); e.looper.Add(0.0005, -1, 5, 0, 1.f); e.looper.Seal();
            (which ? hits7 : hits24) = RenderHits(3, which ? 7 : 24, which ? y7 : y24);   // 24: the wrap on a block boundary · 7: inside one
        }
        CHECK(hits24 == 6 && hits7 == 6, "two events × three passes = 6 hits at 24- and 7-sample blocks (%d, %d)", hits24, hits7);
        // the onsets around the first wrap, on fresh voices, before either ends (a voice's end
        // decay and the residual a re-triggered voice inherits are block-aligned: engine semantics kept)
        bool same = y7.size() >= (size_t)(3 * kLen);
        for(size_t i = 95800; same && i < (size_t)kLen + 3000; i++) if(y7[i] != y24[i]) same = false;
        CHECK(same, "the onsets are block-size invariant: identical samples across the wrap");
    }

    SECTION("LOOPER: the 256-event cap refuses gracefully; deterministic");
    {
        Prime(0);
        for(int i = 0; i < 256; i++) { e.barPhase = (double)i / 256.; e.CallSlice(i & 15, 1.f, (i & 1) != 0, true, false); }
        CHECK(e.looper.n == 256 && e.looper.Full(), "256 events fit (n %d)", e.looper.n);
        e.callHits = 0; e.barPhase = 0.5; e.CallSlice(3, 1.f, false, true, false);
        CHECK(e.looper.n == 256 && e.callHits == 1, "the 257th is refused; the call still sounds");
        CHECK(!e.looper.Add(0.1, -1, 1, 0, 1.f) && !e.looper.RecordRelease(0.1, 3), "Add and a release refuse too");
        int sorted = 1; for(int i = 1; i < e.looper.n; i++) if(e.looper.ev[i].phase < e.looper.ev[i - 1].phase) sorted = 0;
        CHECK(sorted, "the list stays sorted by phase");
        e.callHits = 0;
        std::vector<float> yf;
        const int hits = RenderHits(2, 24, yf);
        bool finite = true; for(float v : yf) if(!std::isfinite(v)) finite = false;
        CHECK(finite && hits == 256 + 127, "a full loop renders finite audio, every event fires (%d hits: 127 on pass 1 — those beyond half a loop ahead of the frontier — 256 on pass 2)", hits);
        CHECK(e.looper.UndoLast() && e.looper.n == 0, "one undo clears a single-pass loop");
        // determinism: two engines, the same gestures, the same bits
        std::vector<float> r1, r2;
        for(int run = 0; run < 2; run++)
        {
            Prime(0);
            e.barPhase = 0.11; e.CallSlice(1, 1.f, false, true, false);
            e.barPhase = 0.37; e.CallSlice(e.KeySlice(5), 2.f, true, true, true, 1.f, true, 5);
            e.barPhase = 0.52; e.ReleaseKey(5);
            e.barPhase = 0.80; e.CallSlice(9, 0.5f, false, true, false, 0.5f);
            RenderLoops(e, 1); Wrap();
            e.barPhase = 0.66; e.CallSlice(6, 1.f, false, true, false);
            Rendered q2 = RenderLoops(e, 3, 24);
            (run ? r2 : r1) = q2.y;
        }
        CHECK(r1 == r2 && Fnv(r1) == Fnv(r2), "the same gestures render the same bits");
        CHECK(e.looper.n == 5 && e.looper.ev[2].gen == 0 && e.looper.ev[3].gen == 1, "…with five events in two generations");
    }

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
