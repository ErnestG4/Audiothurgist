// keyp_check — KEYP package (docs/packages/keyp.md): per-key settings — gain,
// tune and how the key plays (Follow the option · One-shot · Held · Loop) —
// in the engine (core/bb_engine.h, bb_edit.h, bb_undo.h) and the set JSON
// (core/bb_set.h, bb_sets.h). tests/run.sh builds it under ASan/UBSan:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib
//       tests/keyp_check.cpp -o /tmp/keyp && /tmp/keyp /tmp     (one line)
//
// 1. Neutral is today: a rich session — a stereo break under Classic with the
//    Zoom tail pitched up, adaptive fades, two pads, one-shot / held /
//    reversed / ratcheted / staccato key calls, then Play's looper recording
//    and replaying keyed calls, and the mono reader — hashes to the golden
//    taken from the untouched engine (beta 7039813) with this exact driver.
//    The same session with every key set to neutral through the edit op
//    renders the same bits.
// 2. Gain and tune at the call: a key's gain scales its voice (a ratchet's
//    double too), its tune multiplies the rate the voice already gets (OwnRate,
//    the tempo's varispeed, P4, the style); the sequencer's steps are untouched,
//    a looper replay of a keyed call follows the key's settings now.
// 3. Plays: Follow takes the option, One-shot and Held override it, Loop holds
//    the entry looping until the release — no read past the entry, no choke by
//    another hit, a clean fade at the release.
// 4. The edit op and the journal: one undo step per edit, a drag's joined edits
//    one step, a join for another key a step of its own, the refusals; new
//    material resets the settings, a re-apply keeps them.
// 5. The set JSON: written only when a key is not neutral, read back exactly,
//    an old file (none) neutral, out-of-range values clamped, unknown fields
//    skipped; an engine → set → engine round trip renders the same bits.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <new>

#include "bb_engine.h"
#include "bb_set.h"
#include "bb_sets.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static constexpr int kSr = 48000, kCap = 48000 * 8;
static bb::UndoJournal gJ, gJ2;   // 150 KB each: static, not on the stack

static uint64_t Fnv(uint64_t h, const float* y, int n)
{
    for(int k = 0; k < n; k++) { uint32_t b; memcpy(&b, &y[k], 4); for(int i = 0; i < 4; i++) { h ^= (b >> (8 * i)) & 255u; h *= 1099511628211ULL; } }
    return h;
}
static constexpr uint64_t kFnv0 = 1469598103934665603ULL;

// A 2 s "break": a decaying tone burst on every 8th, a different partial on R; onsets on the bursts.
static void MakeBreak(float* L, float* R, int n, int* onsets, int& nOn)
{
    nOn = 0;
    for(int i = 0; i < n; i++)
    {
        const int   ph = i % 12000;
        const float env = expf(-(float)ph / 1800.f);
        const float f = (i / 12000) % 2 ? 180.f : 95.f;
        L[i] = 0.7f * env * sinf(6.2831853f * f * (float)ph / (float)kSr) + 0.05f * sinf(6.2831853f * 3100.f * (float)i / (float)kSr);
        if(R) R[i] = 0.6f * env * sinf(6.2831853f * (f * 1.5f) * (float)ph / (float)kSr);
        if(ph == 0 && nOn < 64) onsets[nOn++] = i;
    }
}
// A pad: a tone of `len` samples written at freePos and published as key `key`'s recording (EditOp::Record).
static int Pad(bb::Engine& e, int key, int len, float f, float g)
{
    const int start = e.freePos;
    for(int i = 0; i < len; i++)
    {
        const float env = i < 2400 ? 1.f : expf(-(float)(i - 2400) / 9000.f);
        e.buf[start + i] = g * env * sinf(6.2831853f * f * (float)i / (float)kSr);
        if(e.bufR) e.bufR[start + i] = 0.8f * e.buf[start + i];
    }
    bb::EditReq q; q.op = bb::EditOp::Record; q.key = key; q.start = start; q.len = len;
    q.flags = bb::EditReq::kMeta; q.meta = {0, 0.8f, 0.05f, true};
    int t = -1;
    return e.ApplyEdit(q, &t) == bb::EditStatus::Ok ? t : -1;
}
static void Setup(bb::Engine& e, float* L, float* R)
{
    static int onsets[64]; int nOn = 0;
    MakeBreak(L, R, 96000, onsets, nOn);
    e.Init(31);
    e.SetUndoJournal(&gJ);
    e.SetMaterial(L, 96000, (float)kSr, onsets, nOn, nullptr, 16, R, kCap);
    for(int i = 0; i < 16; i++) { e.stepClass[i] = (uint8_t)(i % 4); e.sliceSal[i] = 0.2f + 0.05f * (float)i; e.sliceDens[i] = 0.02f + 0.01f * (float)(i % 5); }
    e.fadeMode = 1; e.tailMode = 3;   // the firmware's defaults: Adaptive fades, the Zoom tail
    Pad(e, 2, 30000, 330.f, 0.6f);
    Pad(e, 7, 20000, 523.f, 0.5f);
}
// The session: one script of calls against block numbers. `hook(e, blk)` runs before each block (the tests below
// change key settings there). Stereo output hashed L then R per block; returns the hash.
template <typename Hook>
static uint64_t Session(bb::Engine& e, Hook hook, int blocks = 1200)
{
    uint64_t h = kFnv0;
    float l[24], r[24];
    e.patternMode = 0; e.patternKnob = 0.45f; e.RegenPattern();
    e.pitch = 1.3f; e.SetBpm(140.f);
    for(int b = 0; b < blocks; b++)
    {
        hook(e, b);
        switch(b)
        {
            case 40:  e.CallSlice(e.KeySlice(2), 1.f, false, false, false, 1.f, false, 2); break;    // a pad, one-shot
            case 90:  e.CallSlice(e.KeySlice(5), 2.f, false, false, false, 1.f, false, 5); break;    // double
            case 130: e.CallSlice(e.KeySlice(9), 1.f, true, false, false, 1.f, false, 9); break;     // reverse
            case 170: e.CallSlice(e.KeySlice(7), 1.f, false, false, true, 1.f, false, 7); break;     // a pad, ratchet
            case 220: e.CallSlice(e.KeySlice(11), 1.f, false, false, false, 0.5f, false, 11); break; // staccato
            case 260: e.CallSlice(e.KeySlice(7), 1.f, false, false, false, 1.f, true, 7); break;     // a pad held
            case 330: e.ReleaseKey(7); break;
            case 360: e.CallSlice(e.KeySlice(4), 1.f, true, false, false, 1.f, true, 4); break;      // a grid key held, reversed
            case 420: e.ReleaseKey(4); break;
            case 450: e.pitch = 0.8f; e.SetBpm(0.f); break;
            case 500: e.CallSlice(e.KeySlice(3), 0.5f, false, false, false, 1.f, false, 3); break;   // half
            case 560:   // Play: the looper records keyed calls and replays them
                e.patternMode = 5; e.looperQuant = 0; e.RegenPattern(); e.pitch = 1.f; break;
            case 600: e.CallSlice(e.KeySlice(2), 1.f, false, true, false, 1.f, false, 2); break;
            case 640: e.CallSlice(e.KeySlice(6), 1.f, false, true, false, 1.f, true, 6); break;
            case 700: e.ReleaseKey(6); break;
            case 740: e.CallSlice(e.KeySlice(7), 2.f, false, true, true, 1.f, false, 7); break;
            case 780: e.CallSlice(e.KeySlice(12), 1.f, false, true, false, 1.f, false, -1); break;  // a keyless call
            default: break;
        }
        e.Process(l, r, 24);
        h = Fnv(h, l, 24); h = Fnv(h, r, 24);
    }
    return h;
}
static void NoHook(bb::Engine&, int) {}

/* ── 1. neutral is today ─────────────────────────────────────────────── */
// Taken from the untouched engine (beta 7039813) with this exact driver, -O1 sanitized and -O2, x86-64 SSE.
static constexpr uint64_t kGoldStereo = 0x18c1b9d7cd97be77ULL;
static constexpr uint64_t kGoldMono   = 0x73cbbfc7f0973281ULL;
static void TestNeutral()
{
    SECTION("neutral is today: the session hashes to the pre-KEYP golden");
    static std::vector<float> L(kCap), R(kCap), L2(kCap);
    bb::Engine e;
    Setup(e, L.data(), R.data());
    const uint64_t hs = Session(e, NoHook);
    bb::Engine m;
    Setup(m, L2.data(), nullptr);
    uint64_t hm = kFnv0;
    {
        float o[24];
        m.patternMode = 1; m.patternKnob = 0.3f; m.RegenPattern(); m.pitch = 1.1f;
        for(int b = 0; b < 800; b++)
        {
            if(b == 50) m.CallSlice(m.KeySlice(2), 1.f, false, false, false, 1.f, true, 2);
            if(b == 120) m.ReleaseKey(2);
            if(b == 200) m.CallSlice(m.KeySlice(8), 1.f, false, false, true, 1.f, false, 8);
            m.Process(o, 24);
            hm = Fnv(hm, o, 24);
        }
    }
    printf("  stereo session %016llx · mono session %016llx\n", (unsigned long long)hs, (unsigned long long)hm);
    CHECK(hs == kGoldStereo, "the stereo session: %016llx != golden %016llx", (unsigned long long)hs, (unsigned long long)kGoldStereo);
    CHECK(hm == kGoldMono, "the mono session: %016llx != golden %016llx", (unsigned long long)hm, (unsigned long long)kGoldMono);
    // the session exercised what it claims: it ended in Play with the looper's events
    CHECK(e.looper.n >= 4 && e.patternMode == 5, "the session ended in Play with %d events on the loop", e.looper.n);
    // every key edited away and back to neutral through the op (journal and all) — still the golden
    bb::Engine z;
    Setup(z, L.data(), R.data());
    const uint64_t hz = Session(z, [](bb::Engine& x, int b) {
        if(b == 10) for(int k = 0; k < 24; k++) { bb::EditReq q; q.op = bb::EditOp::SetKeyParams; q.key = k; q.a = -7; q.b = 333; q.i = 3; x.ApplyEdit(q); }
        if(b == 20) for(int k = 0; k < 24; k++) { bb::EditReq q; q.op = bb::EditOp::SetKeyParams; q.key = k; q.a = 0; q.b = 0; q.i = 0; x.ApplyEdit(q); }
    });
    CHECK(hz == kGoldStereo && z.KeysNeutral(), "every key set and set back to neutral through the edit op: the golden (%016llx)", (unsigned long long)hz);
}

/* ── helpers ──────────────────────────────────────────────────────────── */
static bb::EditStatus SetKp(bb::Engine& e, int key, int gain, int cents, int plays, bool join = false)
{
    bb::EditReq q; q.op = bb::EditOp::SetKeyParams; q.key = key; q.a = gain; q.b = cents; q.i = plays;
    if(join) q.flags = bb::EditReq::kQuiet;
    return e.ApplyEdit(q);
}
static bb::EditStatus Undo(bb::Engine& e, bool redo = false) { bb::EditReq q; q.op = redo ? bb::EditOp::Redo : bb::EditOp::Undo; return e.ApplyEdit(q); }
static void Silence(bb::Engine& e) { for(int v = 0; v < bb::Engine::kVoices; v++) { e.voices[v].active = false; e.voices[v].held = false; e.voices[v].key = -1; } }
// the voice a call just started (age 0, active), or null
static bb::Voice* Fresh(bb::Engine& e)
{
    for(int v = 0; v < bb::Engine::kVoices; v++) if(e.voices[v].active && e.voices[v].age == 0) return &e.voices[v];
    return nullptr;
}
static bool Near(float a, float b, float tol = 1e-6f) { return fabsf(a - b) <= tol * (fabsf(b) > 1.f ? fabsf(b) : 1.f); }

/* ── 2. gain and tune at the call ────────────────────────────────────── */
static void TestGainTune()
{
    SECTION("gain and tune at the call: the key's voice, its rate × the tune; steps, keyless calls and auditions untouched");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; Setup(e, L.data(), R.data()); e.muted = true;
    const int pad = e.KeySlice(2), grid = e.KeySlice(5);
    CHECK(e.OwnRate(pad) && !e.OwnRate(grid), "key 2 is a pad (own rate), key 5 a grid slice");
    CHECK(e.KeysNeutral() && e.keyGain[2] == 1.f && e.keyRate[2] == 1.f, "every key neutral to begin with: gain and rate exactly 1");
    CHECK(SetKp(e, 2, -12, 700, 0) == bb::EditStatus::Ok, "key 2: −6 dB, +7 semitones");
    CHECK(Near(e.keyGain[2], powf(10.f, -6.f / 20.f), 1e-5f) && Near(e.keyRate[2], powf(2.f, 7.f / 12.f), 1e-5f),
          "gain %g = 10^(−6/20), rate %g = 2^(7/12)", e.keyGain[2], e.keyRate[2]);
    e.SetBpm(150.f); e.pitch = 1.25f;
    Silence(e); e.CallSlice(pad, 1.f, false, false, false, 1.f, false, 2);
    bb::Voice* v = Fresh(e);
    CHECK(v && Near(v->gain, e.keyGain[2]) && Near(v->rate, 1.25f * e.keyRate[2]), "a pad: its own rate × P4 × the tune (%g), the key's gain", v ? v->rate : -1.f);
    Silence(e); e.CallSlice(pad, 2.f, true, false, false, 1.f, false, 2);
    v = Fresh(e);
    CHECK(v && Near(v->rate, -2.f * 1.25f * e.keyRate[2]), "…the style's multiplier and reverse as well (%g)", v ? v->rate : 0.f);
    CHECK(SetKp(e, 5, 6, -1200, 0) == bb::EditStatus::Ok, "key 5: +3 dB, an octave down");
    const float grate = 1.25f * (e.nativeStep / (e.barSeconds * e.sr / (float)e.nSlices));
    Silence(e); e.CallSlice(grid, 1.f, false, false, false, 1.f, false, 5);
    v = Fresh(e);
    CHECK(v && Near(v->rate, grate * 0.5f, 1e-5f) && Near(v->gain, powf(10.f, 3.f / 20.f), 1e-5f), "a grid key: the tempo's varispeed × P4 × ½ (%g vs %g)", v ? v->rate : 0.f, grate * 0.5f);
    Silence(e); e.CallSlice(grid, 1.f, false, false, false, 1.f, false, -1);
    v = Fresh(e);
    CHECK(v && v->gain == 1.f && Near(v->rate, grate), "the same slice called keyless (the page's slice click, a Brain-map punch): unity, untuned");
    Silence(e); e.CallSlice(grid, 1.f, false, false, false, 1.f, false, 4);
    v = Fresh(e);
    CHECK(v && v->gain == 1.f, "a neutral key is unity");
    // the ratchet's double: tuned and gained like the call
    Silence(e); e.CallSlice(pad, 1.f, false, false, true, 1.f, false, 2);
    float l[24], r[24];
    for(int b = 0; b < 400 && e.callCountdown >= 0; b++) e.Process(l, r, 24);
    v = nullptr; for(int i = 0; i < bb::Engine::kVoices; i++) if(e.voices[i].active && e.voices[i].age <= 24 && e.voices[i].start == e.slices[pad].start) v = &e.voices[i];
    CHECK(v && Near(v->gain, e.keyGain[2]) && Near(v->rate, 1.25f * e.keyRate[2]), "the ratchet's double carries the gain and the tune");
    // the audition voice
    e.CallRegion(1000, 4800, 1.f, 0);
    v = nullptr; for(int i = 0; i < bb::Engine::kVoices; i++) if(e.voices[i].active && e.voices[i].scratch) v = &e.voices[i];
    CHECK(v && v->gain == 1.f && v->rate == 1.f, "an audition: unity, untuned");

    // the gain is a multiply on the voice: one call, the output scaled exactly (one voice, no clip)
    {
        static std::vector<float> L2(kCap), R2(kCap);
        bb::Engine a, b; Setup(a, L.data(), R.data()); Setup(b, L2.data(), R2.data());
        a.muted = b.muted = true;
        SetKp(b, 7, -24, 0, 0);
        a.CallSlice(a.KeySlice(7), 1.f, false, false, false, 1.f, false, 7);
        b.CallSlice(b.KeySlice(7), 1.f, false, false, false, 1.f, false, 7);
        const float gl = b.keyGain[7];
        float al[24], ar[24], bl[24], br[24], worst = 0.f, peak = 0.f;
        for(int k = 0; k < 600; k++)
        {
            a.Process(al, ar, 24); b.Process(bl, br, 24);
            for(int i = 0; i < 24; i++) { worst = fmaxf(worst, fabsf(bl[i] - al[i] * gl)); worst = fmaxf(worst, fabsf(br[i] - ar[i] * gl)); peak = fmaxf(peak, fabsf(al[i])); }
        }
        CHECK(peak > 0.1f && worst < 1e-6f, "−12 dB: the key's output is the neutral one × %g (worst %g, peak %g)", gl, worst, peak);
    }
    // the sequencer's steps never see a key's settings: every key set, no calls — the bits are the neutral run's
    {
        static std::vector<float> L2(kCap), R2(kCap);
        bb::Engine a, b; Setup(a, L.data(), R.data()); Setup(b, L2.data(), R2.data());
        for(int k = 0; k < 24; k++) SetKp(b, k, -20 + k, 100 * k - 1100, k & 3);
        auto steps = [](bb::Engine& x) {
            uint64_t h = kFnv0; float l2[24], r2[24];
            x.patternMode = 1; x.patternKnob = 0.4f; x.RegenPattern(); x.pitch = 1.4f;
            for(int k = 0; k < 900; k++) { x.Process(l2, r2, 24); h = Fnv(h, l2, 24); h = Fnv(h, r2, 24); }
            return h; };
        CHECK(steps(a) == steps(b), "Collins pitched up, the Zoom tail, every key set: the steps render the neutral bits");
    }
    // a step never adopts a key's gained voice (the continuity branch): verbatim Classic, key 4 called on slice 4 just
    // before step 5 — the step starts a voice of its own at unity instead of running on in the key's −12 dB
    {
        bb::Engine c; Setup(c, L.data(), R.data());
        c.patternMode = 0; c.patternKnob = 1.f; c.RegenPattern(); c.extPhase = true; c.pitch = 1.f; c.SetBpm(0.f);
        CHECK(c.stepSrc[4] == 4 && c.stepSrc[5] == 5, "verbatim: steps 4 and 5 play slices 4 and 5");
        SetKp(c, 4, -24, 0, 0);
        float o[24], o2[24];
        c.muted = true; c.barPhase = 4.0 / 16.0 + 1e-6; c.Process(o, o2, 24);
        c.CallSlice(4, 1.f, false, false, false, 1.f, false, 4);
        c.muted = false;
        const double dp = 24.0 / (double)(c.barSeconds * c.sr);
        bool unity = false, adopted = false;
        for(int k = 0; k < 400 && c.curStep != 5; k++) { c.barPhase += dp; c.Process(o, o2, 24); }
        for(int i = 0; i < bb::Engine::kVoices; i++)
        {
            const bb::Voice& vv = c.voices[i];
            if(!vv.active) continue;
            if(vv.gain == 1.f && vv.start == c.slices[5].start) unity = true;
            if(vv.gain != 1.f && vv.natEnd > c.slices[5].start + 100) adopted = true;
        }
        CHECK(c.curStep == 5 && unity && !adopted, "step 5: a unity voice of its own; the key's voice was not extended into it");
    }
    // Play's looper: a keyed event replays with the key's settings as they are NOW
    {
        bb::Engine p; Setup(p, L.data(), R.data());
        p.patternMode = 5; p.looperQuant = 0; p.RegenPattern(); p.extPhase = true; p.barPhase = 0.;
        float o[24], o2[24];
        p.Process(o, o2, 24);
        p.barPhase = 0.25; p.CallSlice(p.KeySlice(2), 1.f, false, true, false, 1.f, false, 2);
        CHECK(p.looper.n == 1 && p.looper.ev[0].key == 2, "Play: the call is one keyed event");
        SetKp(p, 2, -8, -500, 0);
        Silence(p);
        bb::Voice* got = nullptr;
        for(int k = 0; k < 9000 && !got; k++)
        {
            p.barPhase += 24.0 / (double)(p.barSeconds * p.sr); if(p.barPhase >= 1.0) p.barPhase -= 1.0;
            p.Process(o, o2, 24);
            for(int i = 0; i < bb::Engine::kVoices; i++) if(p.voices[i].active && p.voices[i].start == p.slices[p.KeySlice(2)].start && p.voices[i].age <= 24) got = &p.voices[i];
        }
        CHECK(got && Near(got->gain, p.keyGain[2]) && Near(got->rate, p.pitch * p.keyRate[2], 1e-5f), "the replay: −4 dB and −5 semitones, set after it was played");
    }
}

/* ── 3. plays ────────────────────────────────────────────────────────── */
static void TestPlays()
{
    SECTION("plays: Follow takes the option, One-shot and Held override it, Loop holds the entry looping");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; Setup(e, L.data(), R.data()); e.muted = true;
    float l[24], r[24];
    CHECK(!e.KeyHeld(3, false) && e.KeyHeld(3, true) && e.KeyHeld(-1, true) && !e.KeyHeld(-1, false), "Follow (and a keyless call): the option");
    SetKp(e, 3, 0, 0, 1); SetKp(e, 4, 0, 0, 2); SetKp(e, 2, 0, 0, 3);
    CHECK(!e.KeyHeld(3, true) && e.KeyHeld(4, false) && e.KeyHeld(2, false), "One-shot never holds, Held and Loop always do");
    Silence(e); e.CallSlice(e.KeySlice(3), 1.f, false, false, false, 1.f, true, 3);
    bb::Voice* v = Fresh(e);
    CHECK(v && !v->held, "One-shot with the option on Held: a one-shot");
    Silence(e); e.CallSlice(e.KeySlice(4), 1.f, false, false, false, 1.f, false, 4);
    v = Fresh(e);
    CHECK(v && v->held && v->key == 4 && v->end == e.bufLen, "Held with the option on One-shot: held, reading on to the material's end");
    e.ReleaseKey(4);
    CHECK(v && !v->held, "…released by its key");

    // Loop: a 30 000-sample pad held for 3× its length
    const int t = e.KeySlice(2);
    const int a = e.slices[t].start, end = a + e.slices[t].len;
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 2);
    v = Fresh(e);
    CHECK(v && v->held && v->tail == 1 && v->loopA == (double)a && v->loopB < (double)end && v->end == end, "Loop: held, the tail loop over the entry");
    bool inside = true; double hi = 0.;
    for(int k = 0; k < 90000 / 24; k++) { e.Process(l, r, 24); inside = inside && v->active && v->pos >= (double)a && v->pos < (double)end; hi = v->pos > hi ? v->pos : hi; }
    CHECK(inside && v->looping, "three lengths later it still sounds, never outside its entry (furthest %.0f < %d)", hi, end);
    e.CallSlice(e.KeySlice(9), 1.f, false, false, false, 1.f, false, 9);   // another key, one-shot
    e.muted = false;
    for(int k = 0; k < 400; k++) e.Process(l, r, 24);                      // the pattern's steps too
    CHECK(v->active && v->held && v->tail == 1, "another key's hit and the pattern's steps do not choke it");
    e.muted = true;
    e.ReleaseKey(2);
    CHECK(!v->held && v->tail == 0, "the release: no longer held, the loop off");
    int n = 0; while(v->active && n < 100) { e.Process(l, r, 24); n++; }
    CHECK(!v->active && n * 24 <= v->chokeLen + 48, "…and it fades out within its release (%d samples)", n * 24);
    // fast: P4 up an octave, double, +24 semitones — rate 16, still never past the entry
    e.pitch = 2.f; SetKp(e, 2, 0, 2400, 3);
    Silence(e); e.CallSlice(t, 2.f, false, false, false, 1.f, false, 2);
    v = Fresh(e);
    inside = true;
    for(int k = 0; k < 2000 && v; k++) { e.Process(l, r, 24); inside = inside && v->active && v->pos >= (double)a && v->pos < (double)end; }
    CHECK(v && v->rate == 16.f && inside, "at rate %g the loop still never leaves its entry", v ? v->rate : 0.f);
    e.ReleaseKey(2); e.pitch = 1.f;
    Silence(e); e.CallSlice(t, 1.f, true, false, false, 1.f, false, 2);
    v = Fresh(e);
    CHECK(v && v->held && v->tail == 0 && v->rate < 0.f, "Loop reversed: plain Held, backwards");
    e.ReleaseKey(2);
    // too short for the rate: a 100-sample grid slice at rate 16 → plain held
    {
        bb::EditReq q; q.op = bb::EditOp::SetBoundary; q.i = 5; q.start = e.slices[4].start + 100;
        CHECK(e.ApplyEdit(q) == bb::EditStatus::Ok && e.slices[4].len == 100, "grid slice 4 made 100 samples long");
        bb::EditReq k; k.op = bb::EditOp::SetKey; k.key = 13; k.t = 4;
        CHECK(e.ApplyEdit(k) == bb::EditStatus::Ok, "key 13 on it");
        SetKp(e, 13, 0, 2400, 3); e.pitch = 2.f;
        Silence(e); e.CallSlice(e.KeySlice(13), 2.f, false, false, false, 1.f, false, 13);
        v = Fresh(e);
        CHECK(v && v->held && v->tail == 0, "an entry too short for the rate's loop: plain Held");
        e.ReleaseKey(13); e.pitch = 1.f;
    }
    // the looper records what the call resolved to: a Held key called "one-shot" leaves a held event (a release follows)
    {
        bb::Engine p; Setup(p, L.data(), R.data());
        p.patternMode = 5; p.looperQuant = 0; p.RegenPattern(); p.extPhase = true; p.barPhase = 0.;
        p.Process(l, r, 24);
        SetKp(p, 6, 0, 0, 2);
        p.barPhase = 0.3; p.CallSlice(p.KeySlice(6), 1.f, false, true, false, 1.f, false, 6);
        p.barPhase = 0.4; p.ReleaseKey(6);
        CHECK(p.looper.n == 2 && (p.looper.ev[0].style & bb::Looper::kHeld) && (p.looper.ev[1].style & bb::Looper::kRelease), "Play: a Held key's call is recorded held, its release follows");
    }
}

/* ── 4. the edit op and the journal ──────────────────────────────────── */
static void TestJournal()
{
    SECTION("the edit op: one undo step per edit, a drag one step, the refusals; new material resets, a re-apply keeps");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; Setup(e, L.data(), R.data());
    const int d0 = e.UndoDepth();
    uint8_t g = e.tableGen;
    CHECK(SetKp(e, 3, -13, 250, 2) == bb::EditStatus::Ok && e.tableGen != g && e.UndoDepth() == d0 + 1, "an edit: Ok, the generation moves, one undo step");
    CHECK(e.keyp[3].gain == -13 && e.keyp[3].cents == 250 && e.keyp[3].plays == 2, "the settings are in");
    struct Bad { int key, gain, cents, plays; } bad[] = {{24, 0, 0, 0}, {-1, 0, 0, 0}, {3, -49, 0, 0}, {3, 13, 0, 0}, {3, 0, 2401, 0}, {3, 0, -2401, 0}, {3, 0, 0, 4}, {3, 0, 0, -1}};
    bool refused = true;
    for(const Bad& b : bad) { g = e.tableGen; refused = refused && SetKp(e, b.key, b.gain, b.cents, b.plays) == bb::EditStatus::BadArgs && e.tableGen == g; }
    CHECK(refused && e.keyp[3].gain == -13 && e.UndoDepth() == d0 + 1, "out of range (key, gain, cents, plays): BadArgs, nothing written, no step");
    { bb::EditReq q; q.op = bb::EditOp::SetKeyParams; q.key = 3; q.gen = (uint8_t)(e.tableGen + 1); CHECK(e.ApplyEdit(q) == bb::EditStatus::Stale, "a stale gen: Stale"); }
    CHECK(Undo(e) == bb::EditStatus::Ok && e.keyp[3].Neutral() && e.keyGain[3] == 1.f && e.keyRate[3] == 1.f, "Undo: key 3 neutral again, gain and rate exactly 1");
    CHECK(Undo(e, true) == bb::EditStatus::Ok && e.keyp[3].gain == -13 && Near(e.keyRate[3], powf(2.f, 2.5f / 12.f), 1e-5f), "Redo: back, the rate worked out again");
    // a drag: the first move a step, the rest joined
    const int d1 = e.UndoDepth();
    SetKp(e, 8, -2, 0, 0);
    for(int k = 3; k <= 30; k++) SetKp(e, 8, -k, 0, 0, true);
    CHECK(e.UndoDepth() == d1 + 1 && e.keyp[8].gain == -30, "a slider dragged through 29 values: one step (depth %d)", e.UndoDepth());
    CHECK(Undo(e) == bb::EditStatus::Ok && e.keyp[8].Neutral() && e.keyp[3].gain == -13, "one Undo: back to before the drag, the other key kept");
    CHECK(Undo(e, true) == bb::EditStatus::Ok && e.keyp[8].gain == -30, "one Redo: the drag's last value");
    SetKp(e, 9, -4, 0, 0, true);
    CHECK(e.UndoDepth() == d1 + 2, "a joined edit of ANOTHER key: a step of its own");
    Undo(e);
    CHECK(e.keyp[9].Neutral() && e.keyp[8].gain == -30, "…undone alone");
    SetKp(e, 9, -6, 0, 0, true);
    CHECK(e.UndoDepth() == d1 + 2 && e.RedoDepth() == 0, "a join right after an undo: a step of its own, the redo gone");
    { bb::EditReq m; m.op = bb::EditOp::AddEntry; m.start = 20000; m.len = 3000; CHECK(e.ApplyEdit(m) == bb::EditStatus::Ok, "a marker added"); }
    const int nT = e.nTable;
    SetKp(e, 9, -8, 0, 0, true);
    CHECK(e.UndoDepth() == d1 + 4, "a join behind another op (the marker): a step of its own, never folded into the marker's");
    Undo(e);
    CHECK(e.keyp[9].gain == -6 && e.nTable == nT, "…undone alone: the marker stays");
    Undo(e);
    CHECK(e.nTable == nT - 1 && e.keyp[9].gain == -6, "the next undo takes the marker, the settings stay");
    // the key map is not disturbed by a settings step
    { bb::EditReq k; k.op = bb::EditOp::SetKey; k.key = 0; k.t = 9; e.ApplyEdit(k); }
    SetKp(e, 0, -10, 0, 0);
    Undo(e);
    CHECK(e.KeySlice(0) == 9 && e.keyp[0].Neutral(), "settings undone, the key's re-point kept");
    Undo(e);
    CHECK(e.KeySlice(0) == e.SpanSlice(0), "then the re-point");
    // the settings belong to the key: a drop onto it, a clear, a swap keep them where they are
    SetKp(e, 2, -6, 300, 1);
    Pad(e, 2, 8000, 220.f, 0.5f);
    CHECK(e.keyp[2].gain == -6 && e.keyp[2].cents == 300, "a new pad dropped on key 2: the key keeps its settings");
    e.ClearKey(2);
    CHECK(e.keyp[2].gain == -6, "the key cleared: kept");
    { bb::EditReq w; w.op = bb::EditOp::SwapKeys; w.a = 2; w.b = 7; e.ApplyEdit(w); }
    CHECK(e.keyp[2].gain == -6 && e.keyp[7].Neutral(), "keys 2 and 7 swapped: the settings stay with the keys");
    // a re-apply of the same planes keeps them; new material resets them
    static int on[64]; int nOn = 0; MakeBreak(L.data(), R.data(), 96000, on, nOn);
    e.SetMaterial(L.data(), 96000, (float)kSr, on, nOn, nullptr, 32, R.data(), kCap);
    CHECK(e.keyp[2].gain == -6 && e.keyp[0].Neutral() && e.keyp[3].gain == -13, "a re-slice of the same planes (32 steps): kept");
    static std::vector<float> L2(kCap), R2(kCap);
    memcpy(L2.data(), L.data(), 96000 * sizeof(float));
    e.SetMaterial(L2.data(), 96000, (float)kSr, on, nOn, nullptr, 16, nullptr, kCap);
    CHECK(e.KeysNeutral() && e.keyGain[2] == 1.f && e.keyRate[3] == 1.f, "new material (a load, a loop take, New empty kit): every key neutral");
    // wholesale (a set's load), clamped
    bb::KeyParam w[24] = {};
    w[1] = {-100, 9, 30000}; w[5] = {10, 3, -700};
    e.SetKeyParams(w);
    CHECK(e.keyp[1].gain == -48 && e.keyp[1].cents == 2400 && e.keyp[1].plays == 0 && e.keyp[5].plays == 3 && Near(e.keyRate[5], powf(2.f, -7.f / 12.f), 1e-5f),
          "SetKeyParams: out of range clamped (an unknown plays → Follow), the rest as given");
    CHECK(sizeof(bb::UndoRec) == 2336, "the journal's record did not grow (%zu B)", sizeof(bb::UndoRec));
}

/* ── 5. the set JSON ─────────────────────────────────────────────────── */
static std::string Emit(const bb::SetInfo& s)
{
    static char buf[bb::kSetJsonMax];
    size_t n = 0;
    if(!bb::SetWriter::Json(s, buf, sizeof buf, &n)) return std::string();
    return std::string(buf, n);
}
static void TestJson()
{
    SECTION("the set JSON: \"keyp\" only when a key is set, read back exactly, old and odd files");
    static std::vector<float> L(kCap), R(kCap);
    static bb::SetInfo s, t;
    bb::Engine e; Setup(e, L.data(), R.data());
    bb::Looper none;
    new(&s) bb::SetInfo(); bb::SetFromEngine(e, none, s, true, kSr);
    const std::string neutral = Emit(s);
    CHECK(!neutral.empty() && neutral.find("keyp") == std::string::npos, "an untouched kit: no \"keyp\" (the file as it always was)");
    CHECK(bb::SetReader::Json(neutral.data(), neutral.size(), t), "…and it reads");
    bool allNeutral = true; for(int k = 0; k < 24; k++) allNeutral = allNeutral && t.keyp[k].Neutral();
    CHECK(allNeutral, "an old file (no \"keyp\"): every key neutral");
    SetKp(e, 3, -13, 700, 2); SetKp(e, 23, 12, -2400, 3); SetKp(e, 0, 0, 1, 0); SetKp(e, 11, 0, 0, 1);
    new(&s) bb::SetInfo(); bb::SetFromEngine(e, none, s, true, kSr);
    const std::string js = Emit(s);
    const size_t at = js.find("\"keyp\"");
    CHECK(js.find("\"keyp\":[{\"key\":0,\"gain\":0,\"cents\":1,\"plays\":0},{\"key\":3,\"gain\":-6.5,\"cents\":700,\"plays\":2}") != std::string::npos,
          "the set keys written as {key, gain dB, cents, plays}: %s", js.substr(at == std::string::npos ? 0 : at, 140).c_str());
    CHECK(bb::SetReader::Json(js.data(), js.size(), t), "it reads back");
    bool same = true;
    for(int k = 0; k < 24; k++) same = same && t.keyp[k].gain == e.keyp[k].gain && t.keyp[k].cents == e.keyp[k].cents && t.keyp[k].plays == e.keyp[k].plays;
    CHECK(same, "every key exactly as it was (four set, twenty neutral)");
    // what an older firmware does with it: an unknown key, skipped
    std::string old = js; if(at != std::string::npos) old.replace(at, 6, "\"keyq\"");
    CHECK(bb::SetReader::Json(old.data(), old.size(), t) && t.keyp[3].Neutral() && t.nSlices == s.nSlices, "a reader that does not know \"keyp\" skips it whole (the rest intact)");
    // odd values
    const char* odd = "{\"audiothurgist\":1,\"sr\":48000,\"len\":96000,\"slices\":[{\"start\":0,\"len\":96000}],"
                      "\"keyp\":[{\"key\":1,\"gain\":-99,\"cents\":9999,\"plays\":7,\"pan\":0.5},{\"key\":30,\"gain\":-6},{\"gain\":-6},"
                      "{\"key\":2,\"gain\":0.26,\"cents\":-50,\"plays\":3,\"extra\":{\"a\":[1,2]}},{\"key\":4,\"gain\":-1e30},{\"key\":2,\"plays\":1}],\"future\":[[1],{}]}";
    CHECK(bb::SetReader::Json(odd, strlen(odd), t), "a file with odd values and unknown fields reads");
    CHECK(t.keyp[1].gain == -48 && t.keyp[1].cents == 2400 && t.keyp[1].plays == 0, "out of range: clamped, an unknown plays → Follow");
    CHECK(t.keyp[2].gain == 0 && t.keyp[2].cents == 0 && t.keyp[2].plays == 1, "a later entry for the same key wins (whole: its absent fields neutral)");
    CHECK(t.keyp[4].gain == -48, "−1e30 dB: clamped (no float → int overflow)");
    bool rest = true; for(int k = 5; k < 24; k++) rest = rest && t.keyp[k].Neutral();
    CHECK(rest && t.keyp[0].Neutral(), "key 30 and a key-less entry skipped; the others neutral");
    { const char* h = "{\"audiothurgist\":1,\"len\":1,\"keyp\":[{\"key\":5,\"gain\":0.25}]}"; CHECK(bb::SetReader::Json(h, strlen(h), t) && t.keyp[5].gain == 1, "0.25 dB rounds to ½ dB"); }
    { const char* h = "{\"audiothurgist\":1,\"len\":1,\"keyp\":[{\"key\":5,\"gain\":-0.25}]}"; CHECK(bb::SetReader::Json(h, strlen(h), t) && t.keyp[5].gain == -1, "−0.25 dB to −½ dB"); }

    // the round trip: engine → set → engine (the same material and recordings), every key renders the same bits
    {
        static std::vector<float> L2(kCap), R2(kCap);
        bb::Engine a; Setup(a, L.data(), R.data());
        SetKp(a, 2, -9, 1200, 3); SetKp(a, 7, 4, -300, 2); SetKp(a, 5, -30, 50, 1); SetKp(a, 9, 0, 0, 2);
        new(&s) bb::SetInfo(); bb::SetFromEngine(a, none, s, true, kSr);
        s.bars = 1; s.bpm = a.NativeBpm();
        const std::string j = Emit(s);
        static bb::SetPlan p;
        CHECK(bb::SetReader::Json(j.data(), j.size(), t) && bb::PlanSet(t, p), "saved and planned");
        memcpy(L2.data(), L.data(), (size_t)a.freePos * sizeof(float)); memcpy(R2.data(), R.data(), (size_t)a.freePos * sizeof(float));
        bb::Engine b; b.Init(31); b.SetUndoJournal(&gJ2);
        for(int i = 0; i < p.nGrid; i++) b.sliceSal[i] = p.sal[i];
        b.swing = p.swing;
        b.SetMaterial(L2.data(), p.material, (float)kSr, p.onsets, p.nOnsets, p.cls, p.nGrid, R2.data(), kCap);
        b.SetGridBoundaries(p.starts, p.nGrid, p.gridFlags);
        for(int i = 0; i < 16; i++) b.sliceDens[i] = a.sliceDens[i];
        b.fadeMode = a.fadeMode; b.tailMode = a.tailMode;
        int8_t live[24];
        bb::CommitSetRecordings(b, t, p, [&](int st, int ln) { for(int q = a.nSlices; q < a.nTable; q++) if(a.slices[q].start == st && a.slices[q].len == ln) return a.sliceDens[q]; return 0.f; }, live);
        if(p.hasKeys) b.SetKeyTable(live);
        bb::KeyParam kp[24]; bb::KeyParamsFromSet(t, kp); b.SetKeyParams(kp);
        bool kps = true; for(int k = 0; k < 24; k++) kps = kps && memcmp(&a.keyp[k], &b.keyp[k], sizeof(bb::KeyParam)) == 0 && a.keyGain[k] == b.keyGain[k] && a.keyRate[k] == b.keyRate[k];
        CHECK(kps, "every key's settings, gain and rate identical after the load");
        float al[24], ar[24], bl[24], br[24];
        a.muted = b.muted = true;
        bool bits = true;
        for(int blk = 0; blk < 24 * 60 && bits; blk++)
        {
            if(blk % 60 == 0) { const int k = blk / 60; a.CallSlice(a.KeySlice(k), 1.f, false, false, false, 1.f, false, k); b.CallSlice(b.KeySlice(k), 1.f, false, false, false, 1.f, false, k); }
            if(blk % 60 == 40) { const int k = blk / 60; a.ReleaseKey(k); b.ReleaseKey(k); }
            a.Process(al, ar, 24); b.Process(bl, br, 24);
            bits = !memcmp(al, bl, sizeof al) && !memcmp(ar, br, sizeof ar);
        }
        CHECK(bits, "every key called (Held and Loop released) renders the same bits, both planes");
    }
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    TestNeutral();
    TestGainTune();
    TestPlays();
    TestJournal();
    TestJson();
    printf("\nkeyp_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
