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
        for(int m = 0; m < 5; m++)   /* CLASSIFY: five lenses */
        {
            bb::ClassifySteps(gMat, kLen, kSr, cls, m, sal, dens);
            int bad = 0; for(int s = 0; s < 16; s++) bad += cls[s] > 3;
            CHECK(bad == 0, "mode %d: classes in 0..3", m);
            CHECK(cls[15] == 3, "mode %d: the silent step is class 3 (got %d)", m, cls[15]);
            CHECK(sal[15] < 1e-3f && sal[0] > 0.5f, "mode %d: salience 0 on silence, high on tone", m);
        }
        bb::ClassifySteps(gMat, kLen, kSr, cls, 3, sal, dens);   /* CLASSIFY: the density checks read mode 3 */
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
        for(int m = 1; m < 5; m++) { bb::ClassifySteps(drum, kLen, kSr, cls, m, nullptr, nullptr, 0, bands); lensDep += memcmp(ref, bands, 64) != 0; }
        CHECK(lensDep == 0, "the mask is lens-independent (%d lenses differ)", lensDep);
        // the classification itself is untouched by asking for the mask
        uint8_t c1[64], c2[64]; float s1[64], s2[64];
        for(int m = 0; m < 5; m++)
        {
            bb::ClassifySteps(gMat, kLen, kSr, c1, m, s1, nullptr, 0, nullptr);
            bb::ClassifySteps(gMat, kLen, kSr, c2, m, s2, nullptr, 0, bands);
            CHECK(memcmp(c1, c2, 16) == 0 && memcmp(s1, s2, 16 * sizeof(float)) == 0, "mode %d: classes and salience identical with bandsOut", m);
        }
    }

    // ── CLASSIFY package (docs/packages/classify.md) ──────────────────────
    SECTION("CLASSIFY: the Adaptive lens — deterministic, onset-placed, the two-class guard, the others untouched");
    {
        /* the GATES drum loop again, with an onset table where the drums are */
        static float drum[kLen];
        for(int i = 0; i < kLen; i++) drum[i] = 0.f;
        uint32_t rng = 7u;
        /* the GATES bursts, but a snare with a BODY: 200 Hz shell tone under
         * noise low-passed at 4 kHz (the GATES snare is a 1.8 kHz tone plus
         * white noise — brighter than any real snare, and the anchors, fitted
         * on real breaks, read it as a hat: the lens telling the truth) */
        auto burst = [&](int at, float f, float amp, int n, bool noise, int tone /* 0 none, 1 bright HP², 2 dull LP */) {
            float lp1 = 0.f, lp2 = 0.f; const float a = 1.f - expf(-2.f * 3.14159f * (tone == 2 ? 4000.f : 6000.f) / kSr);
            for(int i = 0; i < n && at + i < kLen; i++)
            {
                rng = rng * 1664525u + 1013904223u;
                float nz = noise ? ((float)(rng >> 8) / 8388608.f - 1.f) : 0.f;
                if(tone == 1) { lp1 += a * (nz - lp1); float h = nz - lp1; lp2 += a * (h - lp2); nz = h - lp2; }
                if(tone == 2) { lp1 += a * (nz - lp1); nz = lp1 * 2.f; }
                float env = expf(-(float)i / (float)n * 5.f);
                drum[at + i] += amp * env * (0.6f * sinf(6.2831853f * f * i / kSr) + 0.4f * nz);
            } };
        auto kick  = [&](int s) { burst(s * kStep, 60.f, 0.9f, 4000, false, 0); };
        auto snare = [&](int s) { burst(s * kStep, 200.f, 0.8f, 4000, true, 2); };
        auto hat   = [&](int s, float a) { burst(s * kStep, 8000.f, a, 2400, true, 1); };
        kick(0); kick(8); kick(6); kick(14); snare(4); snare(12); hat(2, 0.35f); hat(10, 0.35f);
        hat(6, 0.35f); hat(14, 0.35f); hat(4, 0.35f);
        for(int s = 1; s < 15; s += 2) hat(s, 0.15f);
        int ons[16]; for(int s = 0; s < 15; s++) ons[s] = s * kStep;
        uint8_t a1[64], a2[64], a3[64], bands[64]; float s1[64], s2[64];
        bb::ClassifySteps(drum, kLen, kSr, a1, 4, s1, nullptr, 0, bands, ons, 15);
        bb::ClassifySteps(drum, kLen, kSr, a2, 4, s2, nullptr, 0, nullptr, ons, 15);
        CHECK(memcmp(a1, a2, 16) == 0 && memcmp(s1, s2, 16 * sizeof(float)) == 0, "the same loop twice: identical labels and salience (with and without the mask)");
        int bad = 0; for(int s = 0; s < 16; s++) bad += a1[s] > 3;
        CHECK(bad == 0 && a1[15] == 3 && s1[15] < 1e-3f, "classes in 0..3, the silent step stays class 3 with zero salience");
        CHECK(a1[0] == 0 && a1[8] == 0, "pure kick steps read kick (0:%c 8:%c)", "KSH."[a1[0]], "KSH."[a1[8]]);
        CHECK(a1[12] == 1, "the pure snare step reads snare (%c)", "KSH."[a1[12]]);
        CHECK(a1[2] == 2 && a1[10] == 2, "pure hat steps read hat (2:%c 10:%c)", "KSH."[a1[2]], "KSH."[a1[10]]);
        int carpet = 0; for(int s = 1; s < 15; s += 2) carpet += a1[s] == 2;
        CHECK(carpet >= 5, "the quiet hat carpet reads hat (%d/7)", carpet);
        /* salience is the common rule: identical to the Hockman lens's */
        uint8_t h0[64]; float sh[64];
        bb::ClassifySteps(drum, kLen, kSr, h0, 0, sh, nullptr, 0, nullptr);
        CHECK(memcmp(sh, s1, 16 * sizeof(float)) == 0, "salience is lens-independent (Hockman == Adaptive)");
        /* without an onset table the lens finds the rises itself: same labels on this loop */
        bb::ClassifySteps(drum, kLen, kSr, a3, 4, nullptr, nullptr, 0, nullptr, nullptr, 0);
        int agree = 0; for(int s = 0; s < 15; s++) agree += a3[s] == a1[s];
        CHECK(agree >= 11, "no onset table: the self-found rises give much the same labels (%d/15)", agree);
        /* two-class guard: a loop of kicks and snares only never grows a hat cluster */
        static float ks[kLen];
        for(int i = 0; i < kLen; i++) ks[i] = 0.f;
        { float* keep = drum; (void)keep; }
        for(int i = 0; i < kLen; i++) { float t = drum[i]; drum[i] = ks[i]; ks[i] = t; }   /* swap: build the K/S loop in `drum` */
        rng = 7u;
        kick(0); kick(2); kick(8); kick(10); kick(6); snare(4); snare(12); snare(14); kick(13); snare(7);
        int ons2[16]; int no2 = 0; for(int s : {0, 2, 4, 6, 7, 8, 10, 12, 13, 14}) ons2[no2++] = s * kStep;
        uint8_t g[64];
        bb::ClassifySteps(drum, kLen, kSr, g, 4, nullptr, nullptr, 0, nullptr, ons2, no2);
        int nHat = 0, nK = 0, nS = 0; for(int s = 0; s < 15; s++) { nHat += g[s] == 2; nK += g[s] == 0; nS += g[s] == 1; }
        CHECK(nHat == 0 && nK >= 4 && nS >= 3, "kick/snare loop: no step is called hat (K%d S%d H%d)", nK, nS, nHat);
        CHECK(g[0] == 0 && g[8] == 0 && g[4] == 1 && g[12] == 1, "…and the kicks and snares keep their names (0:%c 8:%c 4:%c 12:%c)", "KSH."[g[0]], "KSH."[g[8]], "KSH."[g[4]], "KSH."[g[12]]);
        for(int i = 0; i < kLen; i++) { float t = drum[i]; drum[i] = ks[i]; ks[i] = t; }   /* swap back */
        /* the other lenses ignore the table: byte-identical with and without it */
        int touched = 0;
        for(int m = 0; m < 4; m++)
        {
            uint8_t c1[64], c2[64]; float t1[64], t2[64];
            bb::ClassifySteps(drum, kLen, kSr, c1, m, t1, nullptr, 0, nullptr);
            bb::ClassifySteps(drum, kLen, kSr, c2, m, t2, nullptr, 0, nullptr, ons, 15);
            touched += memcmp(c1, c2, 16) != 0 || memcmp(t1, t2, 16 * sizeof(float)) != 0;
        }
        CHECK(touched == 0, "modes 0–3 are untouched by an onset table (%d differ)", touched);
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

    /* ── EDIT package: slice editing on the audio thread ──────────────────── */
    using bb::EditOp; using bb::EditReq; using bb::EditStatus;
    // does the grid tile [0, bufLen) with every slice ≥ kGridMin?
    auto Tiles = [](const bb::Engine& g) -> bool {
        if(g.nSlices < 1 || g.slices[0].start != 0) return false;
        for(int i = 0; i < g.nSlices; i++)
        {
            if(g.slices[i].len < bb::Engine::kGridMin) return false;
            const int next = i + 1 < g.nSlices ? g.slices[i + 1].start : g.bufLen;
            if(g.slices[i].start + g.slices[i].len != next) return false;
        }
        return true;
    };
    auto Bnd = [](int i, int pos, uint8_t gen = 0) { EditReq r; r.op = EditOp::SetBoundary; r.i = i; r.start = pos; r.gen = gen; return r; };
    auto Ent = [](int t, int start, int len) { EditReq r; r.op = EditOp::SetEntry; r.t = t; r.start = start; r.len = len; return r; };
    auto Add = [](int key, int start, int len, uint8_t flags = EditReq::kOrange) { EditReq r; r.op = EditOp::AddEntry; r.key = key; r.start = start; r.len = len; r.flags = flags; return r; };
    auto One = [](EditOp op, int t) { EditReq r; r.op = op; r.t = t; return r; };
    auto Nudge = [](int samples, int which) { EditReq r; r.op = EditOp::NudgeAll; r.a = samples; r.b = which; return r; };

    SECTION("EDIT: a moved grid boundary plays the new region on the next trigger; a sounding voice finishes unchanged");
    {
        Fresh(e, 0, 1.f); e.muted = true;
        float out[24];
        auto renderN = [&](std::vector<float>& y, int nSamp) {
            for(int i = 0; i < nSamp; i += 24) { e.Process(out, 24); y.insert(y.end(), out, out + 24); }
        };
        const uint8_t g0 = e.tableGen;
        std::vector<float> y;
        e.CallSlice(5, 1.f, false, false, false);          // slice 5 = [30000, 36000): 450 Hz
        renderN(y, 1200);
        const EditStatus st = e.ApplyEdit(Bnd(5, 27000));  // slice 5 now opens 3000 samples early, inside slice 4's tone
        CHECK(st == EditStatus::Ok && e.tableGen != g0, "SetBoundary(5, 27000) accepted, tableGen bumped (%d)", (int)st);
        CHECK(e.slices[4].start == 24000 && e.slices[4].len == 3000 && e.slices[5].start == 27000 && e.slices[5].len == 9000 && Tiles(e),
              "linked: slice 4 = [24000, 27000), slice 5 = [27000, 36000), the grid still tiles");
        CHECK((e.TableFlags(5) & bb::Engine::kFlagOrange) && !(e.TableFlags(4) & bb::Engine::kFlagOrange), "boundary 5 is orange, slice 4 is not");
        renderN(y, 4800);
        CHECK(SliceOfFreq(MeasureFreq(&y[1500], 4000), Rms(&y[1500], 4000)) == 5, "the voice fired before the edit still plays slice 5's old region (%.0f Hz)", MeasureFreq(&y[1500], 4000));
        renderN(y, 6000);
        int alive = 0; for(int i = 0; i < bb::Engine::kVoices; i++) alive += e.voices[i].active;
        CHECK(alive == 0, "…and ends where it always did");
        std::vector<float> z;
        e.CallSlice(5, 1.f, false, false, false);
        renderN(z, 6000);
        CHECK(SliceOfFreq(MeasureFreq(&z[300], 2400), Rms(&z[300], 2400)) == 4 && SliceOfFreq(MeasureFreq(&z[3300], 2400), Rms(&z[3300], 2400)) == 5,
              "the next call of slice 5 opens with the 3000 samples taken from slice 4, then its own (%.0f / %.0f Hz)",
              MeasureFreq(&z[300], 2400), MeasureFreq(&z[3300], 2400));
        // the sequencer: step 5 fires at its own time (the grid clock is untouched) and plays the moved content
        e.muted = false; e.RegenPattern();
        Rendered q = RenderLoops(e, 2);
        const int a = 16 * kStep + 5 * kStep;
        CHECK(SliceOfFreq(MeasureFreq(&q.y[a + 300], 2400), Rms(&q.y[a + 300], 2400)) == 4
              && SliceOfFreq(MeasureFreq(&q.y[a + 3300], 2400), Rms(&q.y[a + 3300], 2400)) == 5,
              "loop 2, step 5's slot: slice 4's tone for 3000 samples, then slice 5's (%.0f / %.0f Hz)",
              MeasureFreq(&q.y[a + 300], 2400), MeasureFreq(&q.y[a + 3300], 2400));
        CHECK(Rms(&q.y[16 * kStep + 4 * kStep + 3600], 2000) < 1e-3f, "step 4's slot: its 3000-sample slice, then honest silence (moving a boundary moves what plays, not when)");
        // hits follow the geometry: with an onset at every old slice start, slice 5 no longer opens on one
        {
            int on[16]; for(int k = 0; k < 16; k++) on[k] = k * kStep;
            Fresh(e, 0, 1.f); e.SetMaterial(gMat, kLen, kSr, on, 16, gCls);
            CHECK(e.stepHit[4] && e.stepHit[5], "every slice opens on an onset");
            e.ApplyEdit(Bnd(5, 27000));
            CHECK(e.stepHit[4] && !e.stepHit[5], "after the move slice 5 = [27000, 36000): the onset at 30000 is past its first quarter → no hit");
            e.ApplyEdit(Bnd(5, 29800));
            CHECK(e.stepHit[5], "…at [29800, 36000) the onset at 30000 is inside it → hit again");
            CHECK(e.loopState[5] == 0 && e.loopState[4] == 0, "loop points of both neighbours forgotten");
        }
    }

    SECTION("EDIT: contiguity under any SetBoundary / NudgeAll sequence; refusals; tableGen bumps only on success; deterministic");
    {
        Fresh(e, 0, 1.f);
        const uint8_t g0 = e.tableGen;
        int refused = 0;
        auto refuse = [&](const EditReq& r, EditStatus want, const char* what) {
            const uint8_t g = e.tableGen;
            const EditStatus st = e.ApplyEdit(r);
            if(st == want && e.tableGen == g && Tiles(e)) refused++;
            else printf("  (refusal '%s': status %d, wanted %d; gen %d→%d)\n", what, (int)st, (int)want, g, e.tableGen);
        };
        refuse(Bnd(0, 100), EditStatus::NotGrid, "i = 0");
        refuse(Bnd(16, 100), EditStatus::NotGrid, "i = nSlices");
        refuse(Bnd(5, 24000 + 63), EditStatus::BadArgs, "slice 4 would be 63 samples");
        refuse(Bnd(5, 36000 - 63), EditStatus::BadArgs, "slice 5 would be 63 samples");
        refuse(Bnd(5, -1), EditStatus::BadArgs, "pos < 0");
        refuse(Bnd(5, kLen), EditStatus::BadArgs, "pos = bufLen");
        refuse(Bnd(5, 31000, (uint8_t)(e.tableGen + 1)), EditStatus::Stale, "a stale gen");
        refuse(Nudge(0, 0), EditStatus::BadArgs, "nudge by 0");
        refuse(Nudge(100, 1), EditStatus::BadArgs, "nudge markers with none");
        refuse(One(EditOp::Tombstone, 3), EditStatus::NotMarker, "tombstone a grid slice");
        refuse(One(EditOp::Merge, 3), EditStatus::NotMarker, "merge a grid slice");
        refuse(Ent(3, 0, 6000), EditStatus::NotMarker, "SetEntry on a grid slice");
        { EditReq r; r.op = (EditOp)200; refuse(r, EditStatus::BadOp, "an unknown op"); }
        CHECK(refused == 13 && e.tableGen == g0, "%d refusals, each leaving the table and tableGen alone", refused);
        CHECK(e.ApplyEdit(Bnd(5, 31000, e.tableGen)) == EditStatus::Ok, "a matching gen is accepted");
        // a sequence of moves and nudges: the grid always tiles with ≥ 64 per slice
        auto sequence = [&](bb::Engine& g) {
            g.ApplyEdit(Bnd(3, 15000)); g.ApplyEdit(Nudge(1000, 0)); g.ApplyEdit(Bnd(12, 70000));
            g.ApplyEdit(Nudge(-500000, 0)); g.ApplyEdit(Nudge(4000, 0)); g.ApplyEdit(Bnd(1, 90000));
            g.ApplyEdit(Nudge(700000, 0)); g.ApplyEdit(Bnd(8, 40000));
        };
        bool tiled = true;
        {
            Fresh(e, 0, 1.f);
            e.ApplyEdit(Bnd(3, 15000)); tiled = tiled && Tiles(e);
            e.ApplyEdit(Nudge(1000, 0)); tiled = tiled && Tiles(e) && e.slices[3].start == 16000 && e.slices[1].start == 7000;
            e.ApplyEdit(Nudge(-500000, 0)); tiled = tiled && Tiles(e) && e.slices[1].start == 64 && e.slices[15].start == 15 * 64;
            e.ApplyEdit(Nudge(700000, 0)); tiled = tiled && Tiles(e) && e.slices[15].start == kLen - 64 && e.slices[1].start == kLen - 15 * 64;
            CHECK(tiled, "nudges clamp every boundary so its neighbours keep 64 samples, and the grid keeps tiling");
            uint64_t orange = 0; for(int i = 0; i < 16; i++) if(e.TableFlags(i) & bb::Engine::kFlagOrange) orange |= 1ull << i;
            CHECK(orange == 0xFFFEull, "every moved boundary is orange (slice 0's start never moves): %llx", (unsigned long long)orange);
        }
        bb::Engine e2;
        Fresh(e, 0, 1.f); Fresh(e2, 0, 1.f);
        sequence(e); sequence(e2);
        bool same = e.tableGen == e2.tableGen && Tiles(e);
        for(int i = 0; i < 16 && same; i++) same = e.slices[i].start == e2.slices[i].start && e.slices[i].len == e2.slices[i].len && e.tableFlags[i] == e2.tableFlags[i];
        CHECK(same, "the same edit sequence on two engines gives the same table and generation");
    }

    SECTION("EDIT: markers — ADD inside the break, SPLIT, MERGE, TOMBSTONE, keys re-point, tombstones never trigger, slots are reused");
    {
        static float big2[kLen + 48000];
        memcpy(big2, gMat, sizeof gMat);
        for(int i = kLen; i < kLen + 48000; i++) big2[i] = 0.5f * sinf(6.2831853f * 1000.f * (float)(i - kLen) / kSr);   // a "recording": 1 kHz
        auto setup = [&](int mode) {
            e = bb::Engine(); e.Init(0x1234); e.patternMode = mode; e.patternKnob = 1.f;
            e.SetMaterial(big2, kLen, kSr, nullptr, 0, gCls, 0, nullptr, kLen + 48000);
            e.SetBpm(0.f); e.RegenPattern(); e.muted = true;
            CHECK(e.CommitSlice(3, kLen, 12000, 1, 1.f, 0.01f, true) == 16, "a recording on key 3 is entry 16");
        };
        setup(0);
        float out[24];
        auto renderN = [&](std::vector<float>& y, int nSamp) {
            for(int i = 0; i < nSamp; i += 24) { e.Process(out, 24); y.insert(y.end(), out, out + 24); }
        };
        auto callFreq = [&](int idx) -> float {   // call, measure the first 2400 samples, then let the voice drain
            std::vector<float> y; e.CallSlice(idx, 1.f, false, false, false); renderN(y, 3000);
            const float f = Rms(&y[300], 2400) < 0.02f ? 0.f : MeasureFreq(&y[300], 2400);
            std::vector<float> drain; renderN(drain, 24000); return f; };
        // ADD a marker over slice 7's region, key 5, with the control thread's analysis
        EditReq r = Add(5, 7 * kStep, kStep);
        bb::AnalyseEntry(e, r.start, r.len, r.meta); r.flags |= EditReq::kMeta;
        CHECK(r.meta.cls == 1 && r.meta.sal == 1.f && r.meta.hit && r.meta.dens > 0.f, "AnalyseEntry: a 550 Hz tone is mid band, full salience, a hit, some density");
        int idx = -1;
        CHECK(e.ApplyEdit(r, &idx) == EditStatus::Ok && idx == 17 && e.nTable == 18, "ADD → entry 17 (got %d, nTable %d)", idx, e.nTable);
        CHECK(e.KeySlice(5) == 17 && e.stepClass[17] == 1 && e.stepHit[17] && e.tableKey[17] == -1 && e.TableFlags(17) == bb::Engine::kFlagOrange,
              "key 5 → 17 at once; meta copied; no owner key (not a recording); orange");
        CHECK(SliceOfFreq(callFreq(17), 0.5f) == 7, "the marker plays slice 7's tone");
        CHECK(e.RecordedEntry(5) < 0 && e.RecArm(5), "a take into key 5 would append, not replace the marker"); { int rs, rl; e.RecStop(rs, rl); }
        // SPLIT it at +2400: 17 = [42000, 44400), 18 = [44400, 48000)
        { EditReq s = One(EditOp::Split, 17); s.start = 7 * kStep + 2400;
          CHECK(e.ApplyEdit(s, &idx) == EditStatus::Ok && idx == 18 && e.slices[17].len == 2400 && e.slices[18].start == 44400 && e.slices[18].len == 3600,
                "SPLIT → 17 = [42000, 44400), 18 = [44400, 48000) (got %d)", idx);
          CHECK(e.KeySlice(5) == 17 && e.stepClass[18] == 1 && e.TableFlags(18) == bb::Engine::kFlagOrange, "keys stay on 17; the new half inherits the class; orange");
          s.start = 7 * kStep + 100;
          CHECK(e.ApplyEdit(s) == EditStatus::BadArgs, "a split leaving a half under kRecMin is refused"); }
        // a keyless ADD, then SET_KEY onto it and onto a grid slice
        CHECK(e.ApplyEdit(Add(-1, 2 * kStep, kStep, 0), &idx) == EditStatus::Ok && idx == 19 && e.TableFlags(19) == 0 && e.stepClass[19] == 3,
              "ADD without a key or meta → 19, grey, class 3");
        { EditReq k; k.op = EditOp::SetKey; k.key = 6; k.t = 19;
          CHECK(e.ApplyEdit(k) == EditStatus::Ok && e.KeySlice(6) == 19, "SET_KEY 6 → 19");
          k.key = 7; k.t = 2;
          CHECK(e.ApplyEdit(k) == EditStatus::Ok && e.KeySlice(7) == 2, "SET_KEY 7 → grid slice 2");
          k.key = 7; k.t = 19;
          CHECK(e.ApplyEdit(k) == EditStatus::Ok && e.KeySlice(7) == 19 && e.KeySlice(6) == 19, "two keys on one entry (decision 5)"); }
        // SET_ENTRY: free start / len inside [0, freePos), overlaps allowed; refusals
        CHECK(e.ApplyEdit(Ent(19, 1000, 20000)) == EditStatus::Ok && e.slices[19].start == 1000 && e.slices[19].len == 20000, "SET_ENTRY moves a marker freely (overlapping the grid)");
        CHECK(e.ApplyEdit(Ent(19, e.freePos - 1000, 2000)) == EditStatus::BadArgs && e.ApplyEdit(Ent(19, 0, 479)) == EditStatus::BadArgs, "…but not past freePos or under kRecMin");
        // MERGE 18 with the marker ending at its start (17): 17 is tombstoned, key 5 follows to 18
        CHECK(e.ApplyEdit(One(EditOp::Merge, 18)) == EditStatus::Ok && e.slices[18].start == 42000 && e.slices[18].len == 6000, "MERGE(18) swallows 17: [42000, 48000)");
        CHECK(e.IsTomb(17) && e.slices[17].len == 0 && e.tableKey[17] == -1 && e.KeySlice(5) == 18 && (e.TableFlags(17) & bb::Engine::kFlagTomb),
              "17 is a tombstone; key 5 re-pointed to 18");
        // tombstones never trigger
        e.callHits = 0;
        CHECK(callFreq(17) == 0.f && e.callHits == 0, "calling the tombstone: silence, no hit counted");
        int alive = 0; for(int i = 0; i < bb::Engine::kVoices; i++) alive += e.voices[i].active;
        CHECK(alive == 0, "…and no voice");
        CHECK(!e.SetKeySlice(5, 17) && e.ApplyEdit(One(EditOp::Tombstone, 17)) == EditStatus::Tomb && e.ApplyEdit(Ent(17, 0, 6000)) == EditStatus::Tomb,
              "a tombstone refuses keys, a second delete and a move");
        e.PunchStep(3, 17, false);
        CHECK(e.stepSrc[3] != 17, "a tombstone cannot be punched into a step");
        // TOMBSTONE 19 (keys 6 and 7 on it) → both keys back to their span slices; a punched step of it becomes a rest
        e.PunchStep(4, 19, false);
        CHECK(e.stepSrc[4] == 19, "marker 19 punched into step 4");
        CHECK(e.ApplyEdit(One(EditOp::Tombstone, 19)) == EditStatus::Ok && e.KeySlice(6) == 6 && e.KeySlice(7) == 7 && e.IsTomb(19) && e.stepSrc[4] == -1 && e.playSrc[4] == -1,
              "TOMBSTONE(19): keys 6 and 7 → span slices 6 and 7; step 4 is a rest");
        CHECK(e.nTable == 20, "the table does not shrink on a delete (nTable %d)", e.nTable);
        // a new marker takes the first tombstone's slot
        CHECK(e.ApplyEdit(Add(-1, 9 * kStep, kStep), &idx) == EditStatus::Ok && idx == 17 && !e.IsTomb(17) && e.nTable == 20, "ADD reuses slot 17 (got %d)", idx);
        CHECK(SliceOfFreq(callFreq(17), 0.5f) == 9, "…and it plays");
        // MERGE with no adjacent marker: the previous live marker (by index)
        CHECK(e.ApplyEdit(One(EditOp::Merge, 18)) == EditStatus::Ok && e.slices[18].start == 42000 && e.slices[18].len == 18000 && e.IsTomb(17),
              "MERGE(18) with nothing ending at its start joins the previous marker 17 = [54000, 60000): the union [42000, 60000)");
        // fill the table: ADD refuses with Full, RecArm too
        int added = 0; EditStatus last = EditStatus::Ok;
        while(added < 200 && (last = e.ApplyEdit(Add(-1, 0, kStep))) == EditStatus::Ok) added++;
        CHECK(last == EditStatus::Full && e.nTable == bb::Engine::kMaxTable && added == bb::Engine::kMaxTable - 18, "the table fills to kMaxTable (%d added) then refuses Full", added);
        CHECK(!e.RecArm(9) && e.RecArm(3), "RecArm refuses a new key with no slot, accepts a key that has an entry"); { int rs, rl; e.RecStop(rs, rl); }
        CHECK(e.ApplyEdit(One(EditOp::Tombstone, 40)) == EditStatus::Ok && e.RecArm(9), "…a delete frees a slot for it"); { int rs, rl; e.RecStop(rs, rl); }
        // NUDGE markers: every live marker moves, clamped into [0, freePos)
        { const int s18 = e.slices[18].start, s95 = e.slices[95].start;
          CHECK(e.ApplyEdit(Nudge(-100000, 1)) == EditStatus::Ok && e.slices[18].start == 0 && e.slices[95].start == 0 && e.slices[40].len == 0,
                "NUDGE markers −100000: clamped to 0; the tombstone untouched (18: %d → %d, 95: %d → %d)", s18, e.slices[18].start, s95, e.slices[95].start);
          CHECK(e.ApplyEdit(Nudge(100000000, 1)) == EditStatus::Ok && e.slices[18].start + e.slices[18].len == e.freePos, "…and +∞ clamps to freePos"); }
        // SWAP_KEYS trades the recorded entry's owner too
        setup(0);
        { EditReq w; w.op = EditOp::SwapKeys; w.a = 3; w.b = 9;
          CHECK(e.ApplyEdit(w) == EditStatus::Ok && e.KeySlice(9) == 16 && e.KeySlice(3) == 9 && e.tableKey[16] == 9 && e.RecordedEntry(9) == 16,
                "SWAP_KEYS(3, 9): key 9 plays the recording and owns it; key 3 plays what 9 did (grid slice 9)");
          w.b = 3; CHECK(e.ApplyEdit(w) == EditStatus::BadArgs, "a == b is refused"); }
        // compaction at SetMaterial on the same planes: tombstones go, keys and keyless looper events follow
        setup(5); e.looperQuant = 0; e.extPhase = true; e.barPhase = 0.; e.Process(out, 24); e.muted = false;
        CHECK(e.ApplyEdit(Add(5, 7 * kStep, kStep), &idx) == EditStatus::Ok && idx == 17, "Play: marker 17 over slice 7 on key 5");
        CHECK(e.ApplyEdit(Add(-1, 2 * kStep, kStep), &idx) == EditStatus::Ok && idx == 18, "…and a keyless marker 18 over slice 2");
        e.barPhase = 0.25; e.CallSlice(18, 1.f, false, true, false);                         // keyless event on 18
        e.barPhase = 0.5;  e.CallSlice(e.KeySlice(3), 1.f, false, true, false, 1.f, false, 3); // keyed event: key 3 (the recording)
        e.barPhase = 0.75; e.CallSlice(17, 1.f, false, true, false);                         // keyless event on 17
        CHECK(e.looper.n == 3 && e.looper.ev[0].slice == 18 && e.looper.ev[1].key == 3 && e.looper.ev[2].slice == 17, "three events: keyless 18, key 3, keyless 17");
        CHECK(e.ApplyEdit(One(EditOp::Tombstone, 18)) == EditStatus::Ok && e.looper.n == 2 && e.looper.ev[0].key == 3 && e.looper.ev[1].slice == 17,
              "TOMBSTONE(18) drops the keyless event on it from the loop (n %d)", e.looper.n);
        Rendered q1 = RenderLoops(e, 2);
        const float f8 = MeasureFreq(&q1.y[kLen + 8 * kStep + 300], 2400), f12 = MeasureFreq(&q1.y[kLen + 12 * kStep + 300], 2400);
        CHECK(fabsf(f8 - 1000.f) < 30.f && SliceOfFreq(f12, 0.5f) == 7 && SliceAtStep(q1, 16 + 4) == -1,
              "pass 2: the recording at 0.5 (%.0f Hz), marker 17 at 0.75 (%.0f Hz), silence at 0.25", f8, f12);
        e.SetMaterial(big2, kLen, kSr, nullptr, 0, gCls, 32, nullptr, kLen + 48000);   // 2 bars over the same planes
        CHECK(e.nSlices == 32 && e.nTable == 34 && !e.IsTomb(32) && !e.IsTomb(33), "re-slice to 32: the tombstone is compacted away (nTable %d)", e.nTable);
        CHECK(e.KeySlice(3) == 32 && e.KeySlice(5) == 33 && e.slices[33].start == 7 * kStep && e.tableKey[32] == 3 && e.TableFlags(33) == bb::Engine::kFlagOrange,
              "key 3 → 32 (the recording), key 5 → 33 (the marker, still orange)");
        CHECK(e.looper.n == 2 && e.looper.ev[1].slice == 33 && e.looper.ev[0].key == 3, "the keyless event follows 17 → 33; the keyed one is untouched");
        Rendered q2 = RenderLoops(e, 2);
        const float g8 = MeasureFreq(&q2.y[kLen + 8 * kStep + 300], 2400), g12 = MeasureFreq(&q2.y[kLen + 12 * kStep + 300], 2400);
        CHECK(fabsf(g8 - 1000.f) < 30.f && SliceOfFreq(g12, 0.5f) == 7, "…and the loop resolves to the same audio (%.0f / %.0f Hz)", g8, g12);
        // a recording the new material grows over is dropped; a marker inside the break is kept
        e.SetMaterial(big2, kLen + 6000, kSr, nullptr, 0, gCls, 16, nullptr, kLen + 48000);
        CHECK(e.nTable == 17 && e.KeySlice(5) == 16 && e.slices[16].start == 7 * kStep && e.KeySlice(3) == 3, "material grown by 6000: the recording under it goes, the marker stays (nTable %d)", e.nTable);
    }

    SECTION("EDIT: SWAP_KEYS with a looper event recorded on key a — the loop plays b's audio after the swap");
    {
        Prime(0);
        e.barPhase = 0.25; e.CallSlice(e.KeySlice(3), 1.f, false, true, false, 1.f, false, 3);
        CHECK(e.looper.n == 1 && e.looper.ev[0].key == 3, "an event on key 3");
        Rendered q = RenderLoops(e, 2);
        CHECK(SliceAtStep(q, 16 + 4) == 3, "pass 2, step 4: slice 3 (%d)", SliceAtStep(q, 16 + 4));
        EditReq w; w.op = EditOp::SwapKeys; w.a = 3; w.b = 9;
        CHECK(e.ApplyEdit(w) == EditStatus::Ok && e.KeySlice(3) == 9 && e.KeySlice(9) == 3, "swap 3 ↔ 9");
        q = RenderLoops(e, 2);
        CHECK(SliceAtStep(q, 16 + 4) == 9, "…the same event now plays slice 9 — it resolves through KeySlice at fire time (%d)", SliceAtStep(q, 16 + 4));
    }

    SECTION("EDIT: the boundary overlay — edits survive a same-plane SetMaterial, are dropped when nSlices changes; markers survive too");
    {
        Fresh(e, 0, 1.f);
        e.ApplyEdit(Bnd(5, 27000)); e.ApplyEdit(Bnd(9, 52000));
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls);   // the same planes, same count (a Classifier / Onsets re-apply)
        CHECK(e.slices[5].start == 27000 && e.slices[4].len == 3000 && e.slices[9].start == 52000 && Tiles(e), "both moved boundaries are back after the re-apply");
        CHECK((e.TableFlags(5) & bb::Engine::kFlagOrange) && (e.TableFlags(9) & bb::Engine::kFlagOrange) && !(e.TableFlags(3) & bb::Engine::kFlagOrange), "…and still orange; untouched ones white");
        e.swing = 0.6f; e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls);   // a Swing re-apply: the untouched odd boundaries swing, the moved ones hold
        CHECK(e.swung && e.slices[3].start == 2 * kStep + (int)(2.f * kStep * 0.6f + 0.5f) && e.slices[5].start == 27000 && e.slices[9].start == 52000 && Tiles(e),
              "Swing re-apply: boundary 3 swings to %d, boundaries 5 and 9 stay where the user put them", e.slices[3].start);
        e.swing = 0.5f;
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls, 32);               // another count: the overlay is dropped
        bool plain = e.nSlices == 32;
        for(int i = 0; i < 32 && plain; i++) plain = e.slices[i].start == i * 3000 && e.TableFlags(i) == 0;
        CHECK(plain && e.ovOrange == 0, "32 slices: the computed grid, nothing orange, no overlay");
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls, 16);
        CHECK(e.slices[5].start == 30000, "…and back to 16: the plain grid (the overlay did not linger)");
        static float other[kLen]; memcpy(other, gMat, sizeof gMat);
        e.ApplyEdit(Bnd(5, 27000));
        e.SetMaterial(other, kLen, kSr, nullptr, 0, gCls);
        CHECK(e.slices[5].start == 30000 && e.ovOrange == 0, "new planes: the overlay goes with the material");
        // a moved boundary that a re-apply cannot honour is clamped, not dropped
        Fresh(e, 0, 1.f);
        e.ApplyEdit(Bnd(1, 100)); e.ApplyEdit(Bnd(2, 200));
        e.swing = 0.6f; e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls);
        CHECK(Tiles(e) && e.slices[1].start == 100 && e.slices[2].start == 200, "tight boundaries survive a swing re-apply intact (%d, %d)", e.slices[1].start, e.slices[2].start);
        e.swing = 0.5f;
        // markers inside the break survive a same-plane re-apply (the relaxed keep rule), with their keys
        Fresh(e, 0, 1.f);
        int idx = -1;
        e.ApplyEdit(Add(5, 7 * kStep, kStep), &idx);
        { EditReq k; k.op = EditOp::SetKey; k.key = 8; k.t = 2; e.ApplyEdit(k); }   // key 8 on grid slice 2
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls);
        CHECK(e.nTable == 17 && e.slices[16].start == 7 * kStep && e.KeySlice(5) == 16 && e.KeySlice(8) == 2, "same count: the marker and both key overrides survive");
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls, 32);
        CHECK(e.nTable == 33 && e.slices[32].start == 7 * kStep && e.KeySlice(5) == 32 && e.KeySlice(8) == (8 * 32) / 24,
              "32 slices: the marker follows to 32 with key 5; key 8's grid override falls back to the span (→ %d)", e.KeySlice(8));
        e.SetMaterial(other, kLen, kSr, nullptr, 0, gCls);
        CHECK(e.nTable == 16 && e.KeySlice(5) == 5, "new planes: markers gone, keys default");
    }

    SECTION("EDIT: SetGridBoundaries and SetKeyTable — the set loader's round trip");
    {
        Fresh(e, 0, 1.f);
        int starts[16]; for(int i = 0; i < 16; i++) starts[i] = e.slices[i].start;
        const uint8_t g0 = e.tableGen;
        CHECK(e.SetGridBoundaries(starts, 16) && e.tableGen == g0 && e.ovOrange == 0, "the computed grid is a no-op: nothing orange, no gen bump");
        starts[5] = 27000; starts[9] = 52000;
        CHECK(e.SetGridBoundaries(starts, 16) && e.tableGen != g0 && Tiles(e) && e.slices[5].start == 27000 && e.slices[4].len == 3000
              && (e.TableFlags(5) & bb::Engine::kFlagOrange) && (e.TableFlags(9) & bb::Engine::kFlagOrange) && !(e.TableFlags(4) & bb::Engine::kFlagOrange),
              "two moved boundaries: applied, linked, orange where they differ");
        int back[16]; for(int i = 0; i < 16; i++) back[i] = e.slices[i].start;
        e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls);
        bool same = true; for(int i = 0; i < 16; i++) same = same && e.slices[i].start == back[i];
        CHECK(same, "…and they are in the overlay: a re-apply keeps them");
        {   /* EDIT-PERSIST: the file's orange bits ride along — a boundary the file marks orange but leaves where the grid computes it is pinned too */
            Fresh(e, 0, 1.f);
            int comp[16]; for(int i = 0; i < 16; i++) comp[i] = e.slices[i].start;
            uint8_t fl[16] = {}; fl[0] = bb::Engine::kFlagOrange; fl[3] = bb::Engine::kFlagOrange;
            const uint8_t g1 = e.tableGen;
            CHECK(e.SetGridBoundaries(comp, 16, fl) && e.tableGen != g1 && (e.TableFlags(3) & bb::Engine::kFlagOrange) && !(e.TableFlags(0) & bb::Engine::kFlagOrange)
                  && e.slices[3].start == comp[3] && ((e.ovOrange >> 3) & 1) && !((e.ovOrange >> 0) & 1),
                  "flags: an unchanged boundary marked orange by the file is orange and in the overlay; boundary 0 never");
            e.SetMaterial(gMat, kLen, kSr, nullptr, 0, gCls);
            CHECK((e.TableFlags(3) & bb::Engine::kFlagOrange) && e.slices[3].start == comp[3], "…and survives a re-apply");
            uint8_t none[16] = {};
            const uint8_t g2 = e.tableGen;
            CHECK(e.SetGridBoundaries(comp, 16, none) && e.tableGen == g2, "flags with nothing new: still a no-op");
        }
        Fresh(e, 0, 1.f);
        for(int i = 0; i < 16; i++) starts[i] = e.slices[i].start;
        starts[5] = 27000; starts[9] = 52000;
        e.SetGridBoundaries(starts, 16);
        for(int i = 0; i < 16; i++) back[i] = e.slices[i].start;
        int bad[16];
        memcpy(bad, back, sizeof bad); CHECK(!e.SetGridBoundaries(bad, 15), "refused: n ≠ nSlices");
        memcpy(bad, back, sizeof bad); bad[0] = 1;  CHECK(!e.SetGridBoundaries(bad, 16), "refused: starts[0] ≠ 0");
        memcpy(bad, back, sizeof bad); bad[7] = bad[6] + 63; CHECK(!e.SetGridBoundaries(bad, 16), "refused: a 63-sample slice");
        memcpy(bad, back, sizeof bad); bad[15] = kLen - 63; CHECK(!e.SetGridBoundaries(bad, 16), "refused: 63 samples before the end");
        memcpy(bad, back, sizeof bad); bad[8] = bad[9]; CHECK(!e.SetGridBoundaries(bad, 16) && e.slices[8].start == back[8], "refused: not ascending — and nothing was written");
        int8_t keys[24]; for(int k = 0; k < 24; k++) keys[k] = -1;
        keys[3] = 9;
        CHECK(e.SetKeyTable(keys) && e.KeySlice(3) == 9 && e.KeySlice(4) == 4 && e.KeySlice(20) == 15, "SetKeyTable: −1 = the default, 3 → 9");
        keys[0] = 16; CHECK(!e.SetKeyTable(keys) && e.KeySlice(3) == 9, "refused: an index ≥ nTable, nothing written");
        int idx = -1; e.ApplyEdit(Add(-1, 0, kStep), &idx); e.ApplyEdit(One(EditOp::Tombstone, idx));
        keys[0] = (int8_t)idx; CHECK(!e.SetKeyTable(keys), "refused: a tombstone");
        keys[0] = -1; keys[1] = (int8_t)idx; e.ApplyEdit(Add(-1, 0, kStep), &idx);   // the slot is alive again
        CHECK(e.SetKeyTable(keys) && e.KeySlice(1) == idx, "…alive again after the slot is reused: accepted");
    }

    /* ── EDIT-AUDITION package (docs/packages/slice-edit-proto.md): the scratch voice, the tap-mark rule ── */
    SECTION("EDIT-AUDITION: CallRegion plays exactly [start, start + len) with the fades; a retrigger chokes; from-end; loop + ReleaseRegion; refusals");
    {
        float out[24];
        auto renderN = [&](std::vector<float>& y, int nSamp) {
            for(int i = 0; i < nSamp; i += 24) { e.Process(out, 24); y.insert(y.end(), out, out + 24); }
        };
        auto maxAbs = [](const std::vector<float>& y, size_t a, size_t b) { float m = 0.f; for(size_t i = a; i < b && i < y.size(); i++) { const float v = fabsf(y[i]); if(v > m) m = v; } return m; };
        auto scratchVoice = [&]() { int sv = -1; for(int i = 0; i < bb::Engine::kVoices; i++) if(e.voices[i].active && e.voices[i].scratch) sv = i; return sv; };
        auto scratchCount = [&]() { int n = 0; for(int i = 0; i < bb::Engine::kVoices; i++) n += e.voices[i].active && e.voices[i].scratch; return n; };
        // 1. the region, exactly: slice 3 from +1000, 4800 samples
        Fresh(e, 0, 1.f); e.muted = true;
        const int st = 3 * kStep + 1000, ln = 4800;
        CHECK(e.CallRegion(st, ln, 1.f, 0), "CallRegion accepted");
        int sv = scratchVoice();
        CHECK(sv >= 0 && e.voices[sv].start == st && e.voices[sv].end == st + ln && e.voices[sv].natEnd == st + ln - 240 && e.voices[sv].bound == st + ln && !e.voices[sv].held,
              "the scratch voice's bounds are the region's (the release ramp inside its last 5 ms), tagged, not held");
        std::vector<float> y; renderN(y, 9600);
        CHECK(SliceOfFreq(MeasureFreq(&y[300], 4000), Rms(&y[300], 4000)) == 3, "it plays slice 3's tone (%.0f Hz)", MeasureFreq(&y[300], 4000));
        CHECK(maxAbs(y, 0, 24) < 0.21f && maxAbs(y, 100, 300) > 0.3f, "the 1 ms attack: half gain over the first 24 samples (%.3f), full after (%.3f)", maxAbs(y, 0, 24), maxAbs(y, 100, 300));
        CHECK(maxAbs(y, ln - 24, ln) < 0.06f && Rms(&y[ln + 48], 2000) < 1e-4f, "the 5 ms edge guard closes it at start + len (%.3f), then silence", maxAbs(y, ln - 24, ln));
        CHECK(MaxStep(y) < 0.08f, "no click anywhere (max |Δ| %.3f)", MaxStep(y));
        CHECK(scratchCount() == 0, "…and the voice has ended");
        CHECK(e.trigFlash == 0 && e.callHits == 0 && e.gateFired == 0, "not a call: no trigger flash, no hit, no gate");
        // 2. a retrigger: the old audition fades (5 ms) while the new one attacks
        Fresh(e, 0, 1.f); e.muted = true;
        e.CallRegion(3 * kStep, 4800, 1.f, 0);
        std::vector<float> z; renderN(z, 1008);
        e.CallRegion(7 * kStep, 4800, 1.f, 0);
        CHECK(scratchCount() == 2, "right after the retrigger two tagged voices: the fading one and the new one (%d)", scratchCount());
        renderN(z, 480);
        CHECK(scratchCount() == 1, "10 ms on: the old one has faded out (%d)", scratchCount());
        renderN(z, 2400);
        CHECK(SliceOfFreq(MeasureFreq(&z[1488], 2400), Rms(&z[1488], 2400)) == 7, "only the new region sounds (%.0f Hz)", MeasureFreq(&z[1488], 2400));
        CHECK(MaxStep(z) < 0.08f, "the choke is a fade, not a cut (max |Δ| %.3f)", MaxStep(z));
        // 3. from-end: `start` is the END of the region
        Fresh(e, 0, 1.f); e.muted = true;
        CHECK(e.CallRegion(5 * kStep, 2400, 1.f, bb::Engine::kRegionFromEnd), "from-end accepted");
        sv = scratchVoice();
        CHECK(sv >= 0 && e.voices[sv].start == 5 * kStep - 2400 && e.voices[sv].end == 5 * kStep, "from-end: [end − len, end) = the last 2400 samples of slice 4");
        std::vector<float> fe; renderN(fe, 4800);
        CHECK(SliceOfFreq(MeasureFreq(&fe[300], 1800), Rms(&fe[300], 1800)) == 4 && Rms(&fe[2448], 2000) < 1e-4f, "…plays slice 4's tail then stops (%.0f Hz)", MeasureFreq(&fe[300], 1800));
        // 4. loop: repeats until ReleaseRegion, which fades it out without a click
        Fresh(e, 0, 1.f); e.muted = true;
        CHECK(e.CallRegion(2 * kStep, 2400, 1.f, bb::Engine::kRegionLoop), "loop accepted");
        std::vector<float> lp; renderN(lp, 7200);
        sv = scratchVoice();
        CHECK(sv >= 0 && e.voices[sv].looping && SliceOfFreq(MeasureFreq(&lp[4800], 2400), Rms(&lp[4800], 2400)) == 2,
              "three lengths later the loop still plays slice 2 (%.0f Hz)", MeasureFreq(&lp[4800], 2400));
        const size_t relAt = lp.size();
        e.ReleaseRegion();
        renderN(lp, 480);
        CHECK(scratchCount() == 0 && Rms(&lp[relAt + 300], 180) < 2e-3f, "ReleaseRegion: ended within 10 ms, silent after the fade (rms %.4f)", Rms(&lp[relAt + 300], 180));
        CHECK(MaxStep(lp) < 0.1f, "no click at the seams or the release (max |Δ| %.3f)", MaxStep(lp));
        CHECK(e.trigFlash == 0 && e.callHits == 0, "still not a call");
        // 5. refusals and clamps
        Fresh(e, 0, 1.f); e.muted = true;
        CHECK(!e.CallRegion(0, 63, 1.f, 0) && !e.CallRegion(e.freePos + 1, 480, 1.f, 0) && !e.CallRegion(0, 480, 0.f, 0), "refused: under 64 samples, past freePos, rate 0");
        CHECK(e.CallRegion(e.freePos - 100, 4800, 1.f, 0) && scratchVoice() >= 0 && e.voices[scratchVoice()].end == e.freePos, "a region past freePos is clamped to it");
        e.ReleaseRegion();
        CHECK(e.CallRegion(100, 2400, 1.f, bb::Engine::kRegionFromEnd) && e.voices[scratchVoice()].start == 0 && e.voices[scratchVoice()].end == 100 && e.voices[scratchVoice()].natEnd == 50,
              "from-end before the buffer's start is clamped to [0, end); a short region fades over its second half");
        e.ReleaseRegion();
        // 6. the sequencer's continuity is unaffected: (with a scratch voice) − (without) == (the scratch voice alone)
        auto run = [&](bool call, bool mute) {
            Fresh(e, 0, 1.f); e.muted = mute; e.extPhase = true;
            std::vector<float> r; float o[24];
            const int blocks = (int)(2. * e.barSeconds * kSr / 24.);
            for(int b = 0; b < blocks; b++)
            {
                e.barPhase = fmod((double)b * 24. / kSr / (double)e.barSeconds, 1.0);
                if(call && b == 1050) e.CallRegion(9 * kStep, 4800, 1.f, 0);   // sample 25200: mid step 4 of loop 1
                e.Process(o, 24); r.insert(r.end(), o, o + 24);
            }
            return r;
        };
        std::vector<float> a = run(false, false); const unsigned fa = e.trigFlash;
        std::vector<float> b = run(true, false);  const unsigned fb = e.trigFlash;
        std::vector<float> c = run(true, true);
        float worst = 0.f; for(size_t i = 0; i < a.size(); i++) { const float d = fabsf(b[i] - a[i] - c[i]); if(d > worst) worst = d; }
        CHECK(fa <= 4 && fb == fa, "the verbatim run re-splices as often with the audition as without (%u vs %u)", fb, fa);
        CHECK(worst < 1e-5f && Rms(&c[25200], 4800) > 0.1f, "the audition adds exactly itself to the loop (max |Δ| %.2e; its own rms %.3f)", worst, Rms(&c[25200], 4800));
        // 7. the looper never records it
        Prime(0);
        e.barPhase = 0.25; e.CallRegion(3 * kStep, 2400, 1.f, 0);
        CHECK(e.looper.n == 0, "Play with the looper on: CallRegion writes nothing (%d)", e.looper.n);
        e.CallSlice(3, 1.f, false, true, false);
        CHECK(e.looper.n == 1, "…while a written call does (%d)", e.looper.n);
        // 8. SrcPos: the youngest non-scratch voice, else the current step's source
        Fresh(e, 0, 1.f); e.muted = true;
        CHECK(e.SrcPos() == 0, "nothing sounding: the start of what the step plays (%d)", e.SrcPos());
        e.CallSlice(5, 1.f, false, false, false);
        std::vector<float> sp; renderN(sp, 480);
        CHECK(e.SrcPos() >= 5 * kStep + 470 && e.SrcPos() <= 5 * kStep + 490, "a call: its voice's position (%d)", e.SrcPos());
        e.CallRegion(9 * kStep, 2400, 1.f, 0);
        renderN(sp, 24);
        CHECK(e.SrcPos() >= 5 * kStep + 490 && e.SrcPos() < 6 * kStep, "an audition does not count (%d)", e.SrcPos());
    }

    SECTION("EDIT-AUDITION: the TAP_MARK rule (bb::TapMarkAt) — allowance, onset snap, the length to the next onset, the caps and the limit");
    {
        const int on[6] = {0, 6000, 12000, 18000, 18300, 30000};
        bb::TapMark m = bb::TapMarkAt(9000, 0, 480, on, 6, 96000);
        CHECK(!m.snapped && m.start == 9000 && m.len == 3000, "no onset within ±480: raw, to the next onset (%d +%d)", m.start, m.len);
        m = bb::TapMarkAt(12500, 480, 480, on, 6, 96000);
        CHECK(m.snapped && m.start == 12000 && m.len == 6000, "the allowance pulls 12500 back to 12020, which snaps to 12000 (%d +%d)", m.start, m.len);
        m = bb::TapMarkAt(12500, 0, 480, on, 6, 96000);
        CHECK(!m.snapped && m.start == 12500 && m.len == 5500, "…without it 12500 is 500 from the onset: raw, to the next one (%d +%d)", m.start, m.len);
        m = bb::TapMarkAt(12400, 0, 480, on, 6, 96000);
        CHECK(m.snapped && m.start == 12000, "…and 12400 is within ±480 (%d)", m.start);
        m = bb::TapMarkAt(30100, 0, 480, on, 6, 96000);
        CHECK(m.snapped && m.start == 30000 && m.len == 12000, "past the last onset: the 250 ms cap (%d +%d)", m.start, m.len);
        m = bb::TapMarkAt(3000, 0, 3000, on, 6, 96000);
        CHECK(m.snapped && m.start == 0, "a tie snaps to the earlier onset (%d)", m.start);
        m = bb::TapMarkAt(18100, 0, 480, on, 6, 96000);
        CHECK(m.snapped && m.start == 18000 && m.len == 480, "the next onset 300 samples on: the length is held to kRecMin (%d +%d)", m.start, m.len);
        m = bb::TapMarkAt(9000, 0, 0, on, 6, 96000);
        CHECK(!m.snapped && m.start == 9000, "snap 0: only an exact onset snaps");
        m = bb::TapMarkAt(12000, 0, 0, on, 6, 96000);
        CHECK(m.snapped && m.start == 12000, "…which 12000 is");
        m = bb::TapMarkAt(100, 480, 0, on, 6, 96000);
        CHECK(m.start == 0 && m.snapped && m.len == 6000, "an allowance past the start clamps to 0 (an onset)");
        m = bb::TapMarkAt(30300, 0, 480, on, 6, 33000);
        CHECK(m.snapped && m.start == 30000 && m.len == 3000, "a span past the limit is shortened to it (%d +%d)", m.start, m.len);
        m = bb::TapMarkAt(32900, 0, 0, on, 6, 33000);
        CHECK(m.start == 32520 && m.len == 480, "…unless that leaves under kRecMin: then the start is pulled back (%d +%d)", m.start, m.len);
        m = bb::TapMarkAt(50, 0, 0, on, 6, 300);
        CHECK(m.start == 0 && m.len == 300, "a limit under kRecMin: what ApplyEdit will refuse, nothing worse (%d +%d)", m.start, m.len);
    }

    /* ── EDIT-UNDO package (docs/packages/slice-edit-undo.md): the inverse-op journal ── */
    SECTION("EDIT-UNDO: every op undone restores the table byte for byte (redo re-applies); steps; the ring; audio; what ends the history");
    {
        static bb::UndoJournal journal;
        static float big3[kLen + 48000];
        memcpy(big3, gMat, sizeof gMat);
        for(int i = kLen; i < kLen + 48000; i++) big3[i] = 0.5f * sinf(6.2831853f * 1000.f * (float)(i - kLen) / kSr);   // a "recording": 1 kHz
        int on16[16]; for(int k = 0; k < 16; k++) on16[k] = k * kStep;
        // the whole table state an undo must bring back
        struct Snap
        {
            int nTable, nSlices; bb::Slice s[bb::Engine::kMaxTable]; uint8_t fl[bb::Engine::kMaxTable]; int8_t key[bb::Engine::kMaxTable];
            uint8_t cls[bb::Engine::kMaxTable]; float sal[bb::Engine::kMaxTable], dens[bb::Engine::kMaxTable]; uint8_t bands[bb::Engine::kMaxTable];
            bool hit[bb::Engine::kMaxTable]; bb::EntryMeta rm[bb::Engine::kMaxTable]; int8_t ks[bb::Engine::kKeys]; uint64_t ov; uint8_t gen;
        };
        auto Take = [](const bb::Engine& g) {
            Snap p; memset(&p, 0, sizeof p);
            p.nTable = g.nTable; p.nSlices = g.nSlices; p.ov = g.ovOrange; p.gen = g.tableGen;
            for(int t = 0; t < bb::Engine::kMaxTable; t++)
            {
                p.fl[t] = g.tableFlags[t];
                if(t >= g.nTable) continue;
                p.s[t] = g.slices[t]; p.key[t] = g.tableKey[t]; p.cls[t] = g.stepClass[t]; p.sal[t] = g.sliceSal[t]; p.dens[t] = g.sliceDens[t];
                p.bands[t] = g.stepBands[t]; p.hit[t] = g.stepHit[t];
                if(t >= g.nSlices) p.rm[t] = g.recMeta[t];
            }
            memcpy(p.ks, g.keySlice, sizeof p.ks);
            return p;
        };
        auto Same = [](const Snap& a, const Snap& b, const char** why) {
            *why = "";
            if(a.nTable != b.nTable) { *why = "nTable"; return false; }
            if(a.nSlices != b.nSlices) { *why = "nSlices"; return false; }
            if(a.ov != b.ov) { *why = "ovOrange"; return false; }
            if(memcmp(a.ks, b.ks, sizeof a.ks)) { *why = "keySlice"; return false; }
            if(memcmp(a.fl, b.fl, sizeof a.fl)) { *why = "tableFlags"; return false; }
            for(int t = 0; t < a.nTable; t++)
            {
                if(a.s[t].start != b.s[t].start || a.s[t].len != b.s[t].len) { *why = "slices"; return false; }
                if(a.key[t] != b.key[t]) { *why = "tableKey"; return false; }
                if(a.cls[t] != b.cls[t] || a.sal[t] != b.sal[t] || a.dens[t] != b.dens[t] || a.bands[t] != b.bands[t]) { *why = "class/sal/dens/bands"; return false; }
                if(a.hit[t] != b.hit[t]) { *why = "stepHit"; return false; }
                if(t >= a.nSlices && (a.rm[t].cls != b.rm[t].cls || a.rm[t].sal != b.rm[t].sal || a.rm[t].dens != b.rm[t].dens || a.rm[t].hit != b.rm[t].hit)) { *why = "recMeta"; return false; }
            }
            return true;
        };
        auto setup = [&]() {
            e = bb::Engine(); e.Init(0x1234); e.patternMode = 0; e.patternKnob = 1.f;
            e.SetMaterial(big3, kLen, kSr, on16, 16, gCls, 0, nullptr, kLen + 48000);
            e.SetBpm(0.f); e.RegenPattern(); e.muted = true;
            e.CommitSlice(3, kLen, 12000, 1, 1.f, 0.01f, true);   // key 3 owns entry 16
            e.SetUndoJournal(&journal);
        };
        auto Undo = [](uint8_t gen = 0) { EditReq r; r.op = EditOp::Undo; r.gen = gen; return r; };
        auto Redo = []() { EditReq r; r.op = EditOp::Redo; return r; };
        auto Key = [](int key, int t) { EditReq r; r.op = EditOp::SetKey; r.key = key; r.t = t; return r; };
        auto Swap = [](int a, int b) { EditReq r; r.op = EditOp::SwapKeys; r.a = a; r.b = b; return r; };
        auto Split = [](int t, int pos) { EditReq r; r.op = EditOp::Split; r.t = t; r.start = pos; return r; };
        auto Prom = [](int t, int i) { EditReq r; r.op = EditOp::Promote; r.t = t; r.i = i; return r; };
        auto Meta = [](EditReq r, uint8_t cls, float sal, float dens, bool hit) { r.meta = {cls, sal, dens, hit}; r.flags |= EditReq::kMeta; return r; };

        setup();
        CHECK(e.UndoDepth() == 0 && e.RedoDepth() == 0 && e.ApplyEdit(Undo()) == EditStatus::BadArgs && e.ApplyEdit(Redo()) == EditStatus::BadArgs,
              "an empty journal: depths 0, Undo / Redo BadArgs");
        const Snap origin = Take(e);
        // one op at a time, cumulatively: apply → undo = before, redo = after; the depths follow
        struct Step { const char* name; EditReq r; };
        const Step steps[] = {
            {"SET_BOUNDARY(5, 27000) with meta", Meta(Bnd(5, 27000), 0, 0.9f, 0.02f, true)},
            {"ADD(key 5, slice 7's span, orange, meta)", Meta(Add(5, 7 * kStep, 3000), 2, 0.7f, 0.05f, true)},
            {"SET_ENTRY(17, 42100, 4000)", Ent(17, 7 * kStep + 100, 4000)},
            {"SPLIT(17, 44100) with meta", Meta(Split(17, 7 * kStep + 100 + 2000), 1, 0.4f, 0.03f, false)},
            {"SET_KEY(6, 18)", Key(6, 18)},
            {"SET_KEY(7, 2) — a grid slice", Key(7, 2)},
            {"SWAP_KEYS(3, 9) — the recording's owner trades", Swap(3, 9)},
            {"MERGE(18) swallows the adjacent 17", One(EditOp::Merge, 18)},
            {"NUDGE_ALL(+1000, grid)", Nudge(1000, 0)},
            {"NUDGE_ALL(−500000, grid) — every boundary clamps", Nudge(-500000, 0)},
            {"NUDGE_ALL(+700000, grid)", Nudge(700000, 0)},
            {"NUDGE_ALL(+200, markers)", Nudge(200, 1)},
            {"PROMOTE(18, 1) — after the +700000 nudge only boundary 1 can reach 42100", Prom(18, 1)},
            {"ADD(no key, 60000, 6000, grey, no meta) reuses slot 17", Add(-1, 60000, 6000, 0)},
            {"TOMBSTONE(18) with key 6 on it", One(EditOp::Tombstone, 18)},
            {"TOMBSTONE(16) — the recording, keys 9 on it", One(EditOp::Tombstone, 16)},
            {"SET_ENTRY(17, 100, 480)", Ent(17, 100, 480)},
        };
        const int nSteps = (int)(sizeof steps / sizeof steps[0]);
        int okSteps = 0;
        for(int k = 0; k < nSteps; k++)
        {
            const Snap before = Take(e);
            const EditStatus st = e.ApplyEdit(steps[k].r);
            if(st != EditStatus::Ok) { printf("  (step %d '%s' refused: %d)\n", k, steps[k].name, (int)st); continue; }
            okSteps++;
            const Snap after = Take(e);
            const int du = e.UndoDepth();
            const EditStatus su = e.ApplyEdit(Undo());
            const Snap back = Take(e);
            const char* why = "";
            const bool same = Same(before, back, &why);
            CHECK(su == EditStatus::Ok && same && back.gen != after.gen && e.UndoDepth() == du - 1 && e.RedoDepth() == 1,
                  "%s: undo restores the table (%s), gen moved, depths %d/%d", steps[k].name, why, e.UndoDepth(), e.RedoDepth());
            const EditStatus sr = e.ApplyEdit(Redo());
            const Snap again = Take(e);
            const bool same2 = Same(after, again, &why);
            CHECK(sr == EditStatus::Ok && same2 && e.UndoDepth() == du && e.RedoDepth() == 0,
                  "%s: redo re-applies it (%s), depths %d/%d", steps[k].name, why, e.UndoDepth(), e.RedoDepth());
        }
        CHECK(okSteps == nSteps && e.UndoDepth() == nSteps, "all %d steps applied; undo depth %d", nSteps, e.UndoDepth());
        const Snap final1 = Take(e);
        // the whole history back, then forward again
        {
            int n = 0; while(e.ApplyEdit(Undo()) == EditStatus::Ok) n++;
            const char* why = "";
            CHECK(n == nSteps && Same(origin, Take(e), &why) && e.UndoDepth() == 0 && e.RedoDepth() == nSteps, "undo-all: %d steps back to the origin (%s), redo depth %d", n, why, e.RedoDepth());
            int m = 0; while(e.ApplyEdit(Redo()) == EditStatus::Ok) m++;
            CHECK(m == nSteps && Same(final1, Take(e), &why) && e.RedoDepth() == 0, "redo-all: %d steps forward to the end (%s)", m, why);
        }
        // a new forward op clears the redo half; a stale gen on Undo is refused with nothing done
        {
            e.ApplyEdit(Undo()); e.ApplyEdit(Undo());
            CHECK(e.RedoDepth() == 2, "two undone: redo depth 2");
            const uint8_t g = e.tableGen;
            CHECK(e.ApplyEdit(Undo((uint8_t)(g + 1))) == EditStatus::Stale && e.tableGen == g && e.RedoDepth() == 2, "a stale gen on UNDO: Stale, nothing moved");
            CHECK(e.ApplyEdit(Key(0, 1)) == EditStatus::Ok && e.RedoDepth() == 0 && e.ApplyEdit(Redo()) == EditStatus::BadArgs, "a new op clears the redo half; Redo → BadArgs");
        }
        // a step of several records (the shell's FROM_ONSETS: a tombstone, then adds joined with kJoin) undoes as one
        {
            setup();
            e.ApplyEdit(Add(-1, 6000, 6000, 0));                       // a grey marker, 17
            const Snap before = Take(e);
            const int d0 = e.UndoDepth();
            EditReq t = One(EditOp::Tombstone, 17);
            EditReq a1 = Add(-1, 12000, 6000, 0); a1.flags |= EditReq::kJoin;
            EditReq a2 = Add(-1, 18000, 6000, 0); a2.flags |= EditReq::kJoin;
            EditReq a3 = Add(-1, 24000, 6000, 0); a3.flags |= EditReq::kJoin;
            CHECK(e.ApplyEdit(t) == EditStatus::Ok && e.ApplyEdit(a1) == EditStatus::Ok && e.ApplyEdit(a2) == EditStatus::Ok && e.ApplyEdit(a3) == EditStatus::Ok,
                  "the four applies land");
            CHECK(e.UndoDepth() == d0 + 1 && journal.count == d0 + 4, "…as ONE step (depth %d, %d records)", e.UndoDepth(), journal.count);
            const Snap after = Take(e);
            const char* why = "";
            CHECK(e.ApplyEdit(Undo()) == EditStatus::Ok && Same(before, Take(e), &why) && e.UndoDepth() == d0 && e.RedoDepth() == 1, "one Undo takes the whole step back (%s)", why);
            CHECK(e.ApplyEdit(Redo()) == EditStatus::Ok && Same(after, Take(e), &why) && e.RedoDepth() == 0, "one Redo brings it all back (%s)", why);
        }
        // 100 random ops, then undo-all restores the origin and redo-all the end
        {
            setup();
            const Snap o = Take(e);
            uint32_t rng = 0xC0FFEE; auto R = [&](int n) { rng = rng * 1664525u + 1013904223u; return (int)((rng >> 8) % (uint32_t)(n > 0 ? n : 1)); };
            int ok = 0, tried = 0, refused = 0, kinds[12] = {};
            for(int k = 0; k < 100; k++)
            {
                EditReq r; const int which = R(11); tried++;
                const int mk = e.nTable > e.nSlices ? e.nSlices + R(e.nTable - e.nSlices) : e.nSlices;   // some marker index
                switch(which)
                {
                    case 0: { const int i = 1 + R(e.nSlices - 1); r = Bnd(i, e.slices[i].start + R(6000) - 3000); break; }
                    case 1: { const int len = 480 + R(5000); r = Ent(mk, R(e.freePos - len), len); break; }
                    case 2: { const int len = 480 + R(8000); r = Add(R(3) ? R(24) : -1, R(e.freePos - len), len, (uint8_t)R(2)); if(R(2)) r = Meta(r, (uint8_t)R(4), 0.5f, 0.01f, R(2) != 0); break; }
                    case 3: r = Split(mk, e.slices[mk].start + e.slices[mk].len / 2); break;
                    case 4: r = One(EditOp::Merge, mk); break;
                    case 5: r = One(EditOp::Tombstone, mk); break;
                    case 6: r = Key(R(24), R(e.nTable)); break;
                    case 7: r = Swap(R(24), R(24)); break;
                    case 8: r = Nudge(R(4000) - 2000, R(2)); break;
                    case 9: { const int i = 1 + R(e.nSlices - 1); r = Prom(mk, i); break; }
                    default: { const int i = 1 + R(e.nSlices - 1); r = Meta(Bnd(i, e.slices[i].start + R(400) - 200), (uint8_t)R(4), 0.3f, 0.02f, true); break; }
                }
                if(e.ApplyEdit(r) == EditStatus::Ok) { ok++; kinds[which]++; } else refused++;
            }
            const Snap end = Take(e);
            const int expect = ok < bb::UndoJournal::kDepth ? ok : bb::UndoJournal::kDepth;
            CHECK(ok >= 40 && e.UndoDepth() == expect, "%d of %d random ops landed (%d refused; kinds %d %d %d %d %d %d %d %d %d %d %d), undo depth %d", ok, tried, refused,
                  kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], kinds[6], kinds[7], kinds[8], kinds[9], kinds[10], e.UndoDepth());
            int n = 0; while(e.ApplyEdit(Undo()) == EditStatus::Ok) n++;
            const char* why = "";
            if(ok <= bb::UndoJournal::kDepth)
                CHECK(n == ok && Same(o, Take(e), &why), "undo-all (%d) restores the origin exactly (%s)", n, why);
            else
                CHECK(n == bb::UndoJournal::kDepth, "undo-all: the ring held the last %d (%d)", bb::UndoJournal::kDepth, n);
            int m = 0; while(e.ApplyEdit(Redo()) == EditStatus::Ok) m++;
            CHECK(m == n && Same(end, Take(e), &why), "redo-all (%d) restores the end exactly (%s)", m, why);
        }
        // the ring: 70 ops keep the last 64; the 65th undo is refused; the table is where op 6 left it
        {
            setup();
            Snap after6 = Take(e);
            for(int k = 0; k < 70; k++)
            {
                CHECK(e.ApplyEdit(Bnd(5, 30000 + 10 * (k + 1))) == EditStatus::Ok, "op %d", k);
                if(k == 5) after6 = Take(e);
            }
            CHECK(e.UndoDepth() == 64 && journal.count == 64, "70 ops: the ring holds 64 (%d)", e.UndoDepth());
            int n = 0; while(e.ApplyEdit(Undo()) == EditStatus::Ok) n++;
            const char* why = "";
            CHECK(n == 64 && Same(after6, Take(e), &why) && e.UndoDepth() == 0 && e.RedoDepth() == 64, "64 undos, the 65th refused; the table is where op 6 left it (%s)", why);
            n = 0; while(e.ApplyEdit(Redo()) == EditStatus::Ok) n++;
            CHECK(n == 64 && e.slices[5].start == 30000 + 700, "64 redos back to op 70 (%d)", e.slices[5].start);
        }
        // audio: a voice sounding through an undo keeps its bounds; the next call plays the restored region
        {
            setup(); e.muted = true;
            float out[24];
            auto renderN = [&](std::vector<float>& y, int nSamp) { for(int i = 0; i < nSamp; i += 24) { e.Process(out, 24); y.insert(y.end(), out, out + 24); } };
            e.ApplyEdit(Bnd(5, 27000));
            std::vector<float> y;
            e.CallSlice(5, 1.f, false, false, false);          // opens with 3000 samples of slice 4's tone
            renderN(y, 1200);
            CHECK(e.ApplyEdit(Undo()) == EditStatus::Ok && e.slices[5].start == 30000 && e.slices[4].len == 6000, "undo mid-voice: the grid is back");
            renderN(y, 4800);
            CHECK(SliceOfFreq(MeasureFreq(&y[300], 2400), Rms(&y[300], 2400)) == 4 && SliceOfFreq(MeasureFreq(&y[3300], 2400), Rms(&y[3300], 2400)) == 5,
                  "the voice fired before the undo still plays the edited region: slice 4's tone, then 5's (%.0f / %.0f Hz)", MeasureFreq(&y[300], 2400), MeasureFreq(&y[3300], 2400));
            std::vector<float> z;
            e.CallSlice(5, 1.f, false, false, false);
            renderN(z, 2400);
            CHECK(SliceOfFreq(MeasureFreq(&z[300], 2000), Rms(&z[300], 2000)) == 5, "the next call plays slice 5 from its restored start (%.0f Hz)", MeasureFreq(&z[300], 2000));
        }
        // what ends the history: SetMaterial (any count, the same planes — the compaction moves indices), a take, a clear
        {
            setup();
            e.ApplyEdit(Bnd(5, 27000)); e.ApplyEdit(Add(-1, 6000, 6000, EditReq::kOrange));
            CHECK(e.UndoDepth() == 2, "two steps");
            e.SetMaterial(big3, kLen, kSr, on16, 16, gCls, 32, nullptr, kLen + 48000);
            CHECK(e.UndoDepth() == 0 && e.ApplyEdit(Undo()) == EditStatus::BadArgs && e.nSlices == 32, "SetMaterial to 32: the journal is cleared, Undo refused");
            e.ApplyEdit(Bnd(5, 15100));
            e.SetMaterial(big3, kLen, kSr, on16, 16, gCls, 32, nullptr, kLen + 48000);
            CHECK(e.UndoDepth() == 0 && e.slices[5].start == 15100, "the same planes and count: the edit survives through the overlay, the history does not (cleared, by choice)");
            setup();
            e.ApplyEdit(Bnd(5, 27000));
            const uint8_t g1 = e.tableGen;
            CHECK(e.CommitSlice(8, kLen + 12000, 6000, 2, 1.f, 0.01f, true) == 17 && e.tableGen != g1, "a take commits past the journal");
            CHECK(e.UndoDepth() == 0 && e.RedoDepth() == 0, "…the journal is stale: depth 0");
            CHECK(e.ApplyEdit(Undo()) == EditStatus::BadArgs && e.slices[5].start == 27000, "…Undo refused, nothing moved");
            CHECK(e.ApplyEdit(Key(0, 17)) == EditStatus::Ok && e.UndoDepth() == 1, "…the next op restarts the history at 1");
            e.ClearKey(8);
            CHECK(e.UndoDepth() == 0, "a Clear (control thread, past the journal) ends it too");
            // no journal at all: nothing recorded, Undo refused, the op itself unchanged
            e.SetUndoJournal(nullptr);
            CHECK(e.ApplyEdit(Bnd(5, 28000)) == EditStatus::Ok && e.UndoDepth() == 0 && e.ApplyEdit(Undo()) == EditStatus::BadArgs, "without a journal: the op lands, depth 0, Undo BadArgs");
        }
        CHECK(sizeof(bb::UndoRec) == 440 && sizeof(bb::UndoJournal) == 65 * 440 + 16, "record 440 B, journal %zu B", sizeof(bb::UndoJournal));
    }

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
