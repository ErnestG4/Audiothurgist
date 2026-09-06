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
#include "bb_onset.h"
#include "bb_condition.h"
#include "bb_swing.h"
#include "bb_testbreak.h"

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

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
