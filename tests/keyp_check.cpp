// keyp_check — KEYP package (docs/packages/keyp.md): per-key settings — gain,
// tune and how the key plays (Follow the option · One-shot · Held · Loop) —
// in the engine (core/bb_engine.h, bb_edit.h, bb_undo.h) and the set JSON
// (core/bb_set.h, bb_sets.h). tests/run.sh builds it under ASan/UBSan:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib
//       tests/keyp_check.cpp -o /tmp/keyp && /tmp/keyp /tmp     (one line)
//
// 1. Back to neutral: a rich session — a stereo break under Classic with the
//    Zoom tail pitched up, adaptive fades, two pads, one-shot / held /
//    reversed / ratcheted / staccato key calls, then Play's looper recording
//    and replaying keyed calls — renders the same with every key set away and
//    back to neutral through the edit op as untouched.
// 2. Gain and tune at the call: a key's gain scales its voice (a ratchet's
//    double too), its tune multiplies the rate the voice already gets (OwnRate,
//    the tempo's varispeed, P4, the style); the sequencer's steps are untouched,
//    a looper replay of a keyed call follows the key's settings now.
// 3. Plays: Follow takes the option, One-shot and Held override it, Loop holds
//    the entry looping (forward, or backward when reversed) until the release —
//    no read past the entry, no choke by another hit, the seams bridged, a
//    clean fade at the release.
// 4. The edit op and the journal: one undo step per edit, a drag's joined edits
//    one step, a join for another key a step of its own, the refusals; new
//    material resets the settings, a re-apply keeps them.
// 5. The set JSON: written only when a key is not neutral, read back exactly,
//    an old file (none) neutral, out-of-range values clamped, unknown fields
//    skipped; an engine → set → engine round trip renders the same bits.
// 6. CHROMA: spread across keys.
// 7. ENV: the amp envelope — Attack / Decay / Sustain / Release at the times
//    asked (the coefficients, the voice's envelope, the output's level), no
//    click; a key without a Release fades as ever; a one-shot's Decay to no
//    sustain ends it, its Release shapes nothing; Loop loops on through its
//    release; Ratchet's retrigger still crossfades; the steps never take it;
//    the edit op's refusals, a drag one step, undo.
// 8. ENV: choke groups — the other keys' voices of the group cut within the
//    choke time (one-shot, held, looping, releasing, ratcheting), never the
//    key's own, never a step's; the spread copies the group.
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

/* ── 1. back to neutral leaves nothing behind ────────────────────────── */
static void TestNeutral()
{
    SECTION("every key set away and back to neutral through the edit op renders as a session never touched");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e;
    Setup(e, L.data(), R.data());
    const uint64_t hs = Session(e, NoHook);
    CHECK(e.looper.n >= 4 && e.patternMode == 5, "the session ended in Play with %d events on the loop", e.looper.n);
    bb::Engine z;
    Setup(z, L.data(), R.data());
    const uint64_t hz = Session(z, [](bb::Engine& x, int b) {
        if(b == 10) for(int k = 0; k < 24; k++) { bb::EditReq q; q.op = bb::EditOp::SetKeyParams; q.key = k; q.kp = {-7, 3, 333, 0, 2, 300, 200, 900, 12, 0}; x.ApplyEdit(q); }   /* ENV: an envelope and a group too */
        if(b == 20) for(int k = 0; k < 24; k++) { bb::EditReq q; q.op = bb::EditOp::SetKeyParams; q.key = k; x.ApplyEdit(q); }
    });
    CHECK(hz == hs && z.KeysNeutral(), "the same bits (%016llx vs %016llx)", (unsigned long long)hz, (unsigned long long)hs);
}

/* ── helpers ──────────────────────────────────────────────────────────── */
static bb::EditStatus SetKp(bb::Engine& e, int key, int gain, int cents, int plays, bool join = false, int rat = 0)
{
    bb::EditReq q; q.op = bb::EditOp::SetKeyParams; q.key = key; q.kp = {(int8_t)gain, (uint8_t)plays, (int16_t)cents, (uint8_t)rat, 0, 0, 0, 0, 0, 0};
    if(join) q.flags = bb::EditReq::kQuiet;
    return e.ApplyEdit(q);
}
// key `key`'s whole settings (ENV: with the envelope and group)
static bb::EditStatus SetP(bb::Engine& e, int key, const bb::KeyParam& p, bool join = false)
{
    bb::EditReq q; q.op = bb::EditOp::SetKeyParams; q.key = key; q.kp = p;
    if(join) q.flags = bb::EditReq::kQuiet;
    return e.ApplyEdit(q);
}
// plays, attack / decay ms, sustain dB below full, release ms, group — gain and tune neutral
static bb::KeyParam Env(int plays, int atk, int dec, int sus, int rel, int grp = 0)
{
    return {0, (uint8_t)plays, 0, 0, (uint8_t)grp, (uint16_t)atk, (uint16_t)dec, (uint16_t)rel, (uint8_t)sus, 0};
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
    e.ReleaseKey(2);
    Silence(e); e.CallSlice(t, 2.f, true, false, false, 1.f, false, 2);
    v = Fresh(e);
    inside = true;
    for(int k = 0; k < 2000 && v; k++) { e.Process(l, r, 24); inside = inside && v->active && v->pos >= (double)a && v->pos < (double)end; }
    CHECK(v && v->rate == -16.f && inside, "…and reversed at rate %g", v ? v->rate : 0.f);
    e.ReleaseKey(2); e.pitch = 1.f;
    // reversed: the entry looping backward from its top, as long, as clean, the same release
    SetKp(e, 2, 0, 0, 3);
    Silence(e); e.CallSlice(t, 1.f, true, false, false, 1.f, false, 2);
    v = Fresh(e);
    CHECK(v && v->held && v->tail == bb::Voice::kTailBack && v->rate < 0.f && v->pos == v->loopB && v->loopB < (double)end && v->loopA > (double)a,
          "Loop reversed: held, the backward loop from the entry's top");
    {
        bool in = true, wrapped = false; double lo = (double)end; float last = 0.f, worst = 0.f;
        for(int k = 0; k < 90000 / 24 && v; k++)
        {
            const double before = v->pos;
            e.Process(l, r, 24);
            in = in && v->active && v->rate < 0.f && v->pos >= (double)a && v->pos < (double)end;
            wrapped = wrapped || v->pos > before; lo = v->pos < lo ? v->pos : lo;
            for(int i = 0; i < 24; i++) { worst = fmaxf(worst, fabsf(l[i] - last)); last = l[i]; }
        }
        printf("  reversed loop: lowest position %.0f (entry from %d), worst step %g\n", lo, a, worst);
        CHECK(in && wrapped && v->looping, "three lengths later it still plays backward, wrapped, never outside its entry");
        CHECK(worst < 0.06f, "the backward seam is bridged: worst sample-to-sample step %g", worst);
    }
    e.ReleaseKey(2);
    CHECK(v && !v->held && v->tail == 0 && v->rate > 0.f, "the release: the loop off, turned forward to fade");
    n = 0; while(v && v->active && n < 100) { e.Process(l, r, 24); n++; }
    CHECK(v && !v->active && n * 24 <= v->chokeLen + 48, "…and gone within its release (%d samples)", n * 24);
    // the forward seam, measured the same way
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 2);
    {
        float last = 0.f, worst = 0.f;
        for(int k = 0; k < 90000 / 24; k++) { e.Process(l, r, 24); for(int i = 0; i < 24; i++) { worst = fmaxf(worst, fabsf(l[i] - last)); last = l[i]; } }
        CHECK(worst < 0.06f, "the forward seam is bridged too: worst step %g", worst);
    }
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

/* ── 3b. ratchet ─────────────────────────────────────────────────────── */
// the blocks at which a call (a FireSlice) landed while `blocks` blocks render; worst sample-to-sample step of L
static std::vector<int> Hits(bb::Engine& e, int blocks, float* worst = nullptr, int releaseAt = -1, int key = -1)
{
    std::vector<int> at; float l[24], r[24], last = 0.f, w = 0.f;
    for(int b = 0; b < blocks; b++)
    {
        if(b == releaseAt) e.ReleaseKey(key);
        const uint8_t h0 = e.callHits;
        e.Process(l, r, 24);
        if(e.callHits != h0) at.push_back(b);
        for(int i = 0; i < 24; i++) { w = fmaxf(w, fabsf(l[i] - last)); last = l[i]; }
    }
    if(worst) *worst = w;
    return at;
}
static void TestRatchet()
{
    SECTION("ratchet: while held the entry retriggers at its rate (½ ¼ ⅛ of itself, or 1/8 1/16 1/32 at the tempo), crossfaded, until the release");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; Setup(e, L.data(), R.data()); e.muted = true;
    const int t = e.KeySlice(2), len = e.slices[t].len;   // the 30 000-sample pad
    CHECK(SetKp(e, 2, 0, 0, 4, false, 1) == bb::EditStatus::Ok && e.KeyHeld(2, false), "key 2: Ratchet at ½ — it holds whatever the option says");
    Silence(e); e.callHits = 0;
    e.CallSlice(t, 1.f, false, false, false, 1.f, false, 2);
    CHECK((e.keyRatOn & (1u << 2)) && e.keyRat[2].period == len / 2, "the call starts it: a period of half the entry (%d)", e.keyRat[2].period);
    float worst = 0.f;
    std::vector<int> h = Hits(e, 100000 / 24, &worst);
    bool even = h.size() >= 5;
    for(size_t i = 1; i < h.size(); i++) even = even && abs((h[i] - h[i - 1]) * 24 - len / 2) <= 24;
    CHECK(even, "%zu retriggers in 100 000 samples, each half the entry apart (± one block)", h.size());
    printf("  ratchet seams: worst step %g\n", worst);
    CHECK(worst < 0.06f, "no clicks at the seams: the worst sample-to-sample step %g (a hard cut at the retrigger: 0.08)", worst);
    int held = 0; for(int i = 0; i < bb::Engine::kVoices; i++) held += e.voices[i].active && e.voices[i].held && e.voices[i].key == 2;
    CHECK(held == 1, "one hit sounds at a time (the last one fades out under the new)");
    e.ReleaseKey(2);
    h = Hits(e, 4000);
    bool quiet = true; for(int i = 0; i < bb::Engine::kVoices; i++) quiet = quiet && !e.voices[i].active;
    CHECK(h.empty() && !(e.keyRatOn & (1u << 2)) && quiet, "the release stops it: no more hits, the last one faded");
    // the other rates; the tune shortens a period of the entry's own length (it plays faster)
    const float step = e.barSeconds * e.sr / (float)e.nSlices;
    struct R0 { int rat, cents; float want; } rr[] = {{0, 0, (float)len}, {2, 0, len / 4.f}, {3, 0, len / 8.f}, {0, 1200, len / 2.f}, {4, 0, 2.f * step}, {5, 0, step}, {6, 0, step / 2.f}};
    bool rates = true;
    for(const R0& q : rr) { SetKp(e, 2, 0, q.cents, 4, false, q.rat); Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 2); rates = rates && abs(e.keyRat[2].period - (int)(q.want + 0.5f)) <= 1; e.ReleaseKey(2); }
    CHECK(rates, "full, ¼, ⅛, full an octave up (= ½ the time), and 1/8, 1/16, 1/32 at the tempo: the periods as asked");
    SetKp(e, 2, 0, 0, 4, false, 3); e.pitch = 2.f;
    Silence(e); e.CallSlice(t, 2.f, false, false, false, 1.f, false, 2);
    CHECK(e.keyRat[2].period == bb::Engine::kRatMin || e.keyRat[2].period == (int)(len / 8 / 4.f + 0.5f), "fast: ⅛ of the entry at rate 4 (%d, never under 5 ms)", e.keyRat[2].period);
    e.ReleaseKey(2); e.pitch = 1.f;
    // the gain on every hit
    SetKp(e, 2, -12, 0, 4, false, 1);
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 2);
    Hits(e, 15000 / 24 + 2);
    bool gained = true; int n = 0; for(int i = 0; i < bb::Engine::kVoices; i++) if(e.voices[i].active && e.voices[i].key == 2) { n++; gained = gained && Near(e.voices[i].gain, e.keyGain[2]); }
    CHECK(n == 1 && gained, "a retrigger carries the key's gain");
    // leaving the mode stops it (an edit, or its undo)
    SetKp(e, 2, -12, 0, 2);
    CHECK(!(e.keyRatOn & (1u << 2)), "the key set to Held while it ratchets: the ratchet stops (the sounding hit stays held)");
    e.ReleaseKey(2);
    SetKp(e, 2, 0, 0, 4, false, 1);
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 2);
    Undo(e);
    CHECK(e.keyp[2].plays == 2 && !(e.keyRatOn & (1u << 2)), "…and an undo back out of Ratchet stops it too");
    e.ReleaseKey(2);
    // no gate to go by: a looper replay of an event recorded without a hold — it fills the step
    SetKp(e, 2, 0, 0, 4, false, 6);   // 1/32: two to a step
    Silence(e); e.callHits = 0;
    e.CallSliceRaw(t, 1.f, false, false, false, 1.f, false, 2);
    h = Hits(e, 4000);
    CHECK(e.keyRat[2].period == (int)(step / 2.f + 0.5f) && h.size() == 1 && !(e.keyRatOn & (1u << 2)), "an un-held call at 1/32: the call and one retrigger fill the 16th (%zu more), then it stops by itself", h.size());
    SetKp(e, 2, 0, 0, 4, false, 0);   // the whole entry: longer than a step — still two hits
    Silence(e); e.CallSliceRaw(t, 1.f, false, false, false, 1.f, false, 2);
    h = Hits(e, 4000);
    CHECK(h.size() == 1, "…a period longer than the step: at least two hits (%zu more)", h.size());
    // a held one gives up after eight bars with no release
    SetKp(e, 2, 0, 0, 4, false, 5);
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 2);
    const int bars8 = (int)(8.f * e.barSeconds * e.sr / 24.f) + 400;
    h = Hits(e, bars8);
    CHECK(!(e.keyRatOn & (1u << 2)) && (int)h.size() >= 8 * e.nSlices - 2 && (int)h.size() <= 8 * e.nSlices + 2, "held with no release: 1/16 for eight bars (%zu hits), then it stops", h.size());
    e.ReleaseKey(2);
    // Play's looper: the held ratchet is recorded held with its release, and the replay ratchets until the release
    {
        bb::Engine p; Setup(p, L.data(), R.data());
        p.patternMode = 5; p.looperQuant = 0; p.RegenPattern(); p.extPhase = true; p.barPhase = 0.;
        float o[24], o2[24];
        p.Process(o, o2, 24);
        SetKp(p, 2, 0, 0, 4, false, 6);
        p.barPhase = 0.25; p.CallSlice(p.KeySlice(2), 1.f, false, true, false, 1.f, false, 2);
        p.barPhase = 0.50; p.ReleaseKey(2);
        CHECK(p.looper.n == 2 && (p.looper.ev[0].style & bb::Looper::kHeld), "Play: recorded held, with its release");
        Silence(p); p.keyRatOn = 0; p.callHits = 0;
        int got = 0; bool stopped = false;
        const double dp = 24.0 / (double)(p.barSeconds * p.sr);
        for(int k = 0; k < 9000; k++)
        {
            p.barPhase += dp; if(p.barPhase >= 1.0) p.barPhase -= 1.0;
            const uint8_t h0 = p.callHits; p.Process(o, o2, 24); if(p.callHits != h0) got++;
            if(got > 2 && !(p.keyRatOn & (1u << 2))) { stopped = true; break; }
        }
        const int want = (int)(0.25 * p.nSlices * 2);   // a quarter bar of 32nds
        CHECK(stopped && abs(got - want) <= 2, "the replay ratchets from the event to its release (%d hits, ~%d)", got, want);
    }
    // neutral is untouched: a key never set to Ratchet never starts one
    bb::Engine z; Setup(z, L.data(), R.data());
    Session(z, NoHook, 900);
    CHECK(z.keyRatOn == 0, "an untouched session never starts a ratchet");
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
    struct Bad { int key, gain, cents, plays, rat; } bad[] = {{24, 0, 0, 0, 0}, {-1, 0, 0, 0, 0}, {3, -49, 0, 0, 0}, {3, 13, 0, 0, 0}, {3, 0, 2401, 0, 0}, {3, 0, -2401, 0, 0},
                                                         {3, 0, 0, 5, 0}, {3, 0, 0, -1, 0}, {3, 0, 0, 4, 7}, {3, 0, 0, 4, -1}};
    bool refused = true;
    for(const Bad& b : bad) { g = e.tableGen; refused = refused && SetKp(e, b.key, b.gain, b.cents, b.plays, false, b.rat) == bb::EditStatus::BadArgs && e.tableGen == g; }
    CHECK(refused && e.keyp[3].gain == -13 && e.UndoDepth() == d0 + 1, "out of range (key, gain, cents, plays, the ratchet's rate): BadArgs, nothing written, no step");
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
    w[1] = {-100, 9, 30000, 9, 3}; w[5] = {10, 4, -700, 2, 0};
    e.SetKeyParams(w);
    CHECK(e.keyp[1].gain == -48 && e.keyp[1].cents == 2400 && e.keyp[1].plays == 0 && e.keyp[1].rat == 0 && e.keyp[1].spare == 0 && e.keyp[5].plays == 4 && e.keyp[5].rat == 2
          && Near(e.keyRate[5], powf(2.f, -7.f / 12.f), 1e-5f), "SetKeyParams: out of range clamped (an unknown plays → Follow, rate → full), the rest as given");
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
    SetKp(e, 3, -13, 700, 2); SetKp(e, 23, 12, -2400, 3); SetKp(e, 0, 0, 1, 0); SetKp(e, 11, 0, 0, 1); SetKp(e, 15, -2, 0, 4, false, 5);
    new(&s) bb::SetInfo(); bb::SetFromEngine(e, none, s, true, kSr);
    const std::string js = Emit(s);
    const size_t at = js.find("\"keyp\"");
    CHECK(js.find("\"keyp\":[{\"key\":0,\"gain\":0,\"cents\":1,\"plays\":0},{\"key\":3,\"gain\":-6.5,\"cents\":700,\"plays\":2}") != std::string::npos,
          "the set keys written as {key, gain dB, cents, plays}: %s", js.substr(at == std::string::npos ? 0 : at, 140).c_str());
    CHECK(bb::SetReader::Json(js.data(), js.size(), t), "it reads back");
    bool same = true;
    for(int k = 0; k < 24; k++) same = same && t.keyp[k].gain == e.keyp[k].gain && t.keyp[k].cents == e.keyp[k].cents && t.keyp[k].plays == e.keyp[k].plays;
    for(int k = 0; k < 24; k++) same = same && t.keyp[k].rat == e.keyp[k].rat;
    CHECK(same, "every key exactly as it was (five set — one a 1/16 ratchet — nineteen neutral)");
    CHECK(js.find("{\"key\":15,\"gain\":-1,\"cents\":0,\"plays\":4,\"rat\":5}") != std::string::npos && js.find("\"plays\":2,\"rat\"") == std::string::npos,
          "\"rat\" written only for a key that has one");
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
    {   /* ENV: odd envelope values clamped, an unknown group none */
        const char* h = "{\"audiothurgist\":1,\"len\":1,\"keyp\":[{\"key\":5,\"atk\":99999,\"dec\":-5,\"sus\":-12,\"rel\":4000,\"grp\":2},{\"key\":6,\"sus\":-200,\"grp\":9,\"rel\":4001},{\"key\":7,\"sus\":6,\"dec\":4000}]}";
        CHECK(bb::SetReader::Json(h, strlen(h), t) && t.keyp[5].atk == 2000 && t.keyp[5].dec == 0 && t.keyp[5].sus == 12 && t.keyp[5].rel == 4000 && t.keyp[5].grp == 2
              && t.keyp[6].sus == 60 && t.keyp[6].grp == 0 && t.keyp[6].rel == 4000 && t.keyp[7].sus == 0 && t.keyp[7].dec == 4000,
              "ENV: attack past 2 s → 2 s, a negative decay → 0, sustain −12 dB kept, under −60 → none, above 0 → full, release past 4 s → 4 s, group 9 → none");
    }

    // the round trip: engine → set → engine (the same material and recordings), every key renders the same bits
    {
        static std::vector<float> L2(kCap), R2(kCap);
        bb::Engine a; Setup(a, L.data(), R.data());
        SetKp(a, 2, -9, 1200, 3); SetKp(a, 7, 4, -300, 2); SetKp(a, 5, -30, 50, 1); SetKp(a, 9, 0, 0, 2); SetKp(a, 11, -3, 200, 4, false, 2); SetKp(a, 13, 0, 0, 4, false, 5);
        SetP(a, 2, {-9, 3, 1200, 0, 1, 20, 0, 300, 0, 0}); SetP(a, 7, {4, 2, -300, 0, 1, 0, 150, 600, 6, 0});   /* ENV: envelopes, a choke group */
        SetP(a, 9, {0, 2, 0, 0, 0, 40, 100, 800, 20, 0}); SetP(a, 14, {0, 1, 0, 0, 1, 0, 250, 0, 60, 0});
        new(&s) bb::SetInfo(); bb::SetFromEngine(a, none, s, true, kSr);
        s.bars = 1; s.bpm = a.NativeBpm();
        const std::string j = Emit(s);
        static bb::SetPlan p;
        CHECK(bb::SetReader::Json(j.data(), j.size(), t) && bb::PlanSet(t, p), "saved and planned");
        CHECK(j.find("{\"key\":2,\"gain\":-4.5,\"cents\":1200,\"plays\":3,\"atk\":20,\"rel\":300,\"grp\":1}") != std::string::npos
              && j.find("{\"key\":14,\"gain\":0,\"cents\":0,\"plays\":1,\"dec\":250,\"sus\":-60,\"grp\":1}") != std::string::npos, "ENV: the envelope's fields and the group written only when set (sustain in dB)");
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
        CHECK(kps, "every key's settings, gain and rate identical after the load (two Ratchet keys among them; ENV: four envelopes, three keys in a group)");
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
        CHECK(bits, "every key called (Held, Loop and Ratchet released; ENV: enveloped, choking) renders the same bits, both planes");
    }
}

/* ── 7. ENV: the amp envelope ─────────────────────────────────────────── */
// a pad of constant amplitude (the envelope is then the output's): a sine of `len` samples, both planes
static int Tone(bb::Engine& e, int key, int len, float f, float g)
{
    const int start = e.freePos;
    for(int i = 0; i < len; i++) { e.buf[start + i] = g * sinf(6.2831853f * f * (float)i / (float)kSr); if(e.bufR) e.bufR[start + i] = e.buf[start + i]; }
    bb::EditReq q; q.op = bb::EditOp::Record; q.key = key; q.start = start; q.len = len;
    q.flags = bb::EditReq::kMeta; q.meta = {3, 0.5f, 0.02f, true};
    int t = -1;
    return e.ApplyEdit(q, &t) == bb::EditStatus::Ok ? t : -1;
}
// render n samples (whole blocks); the peak |L| over them; the worst sample-to-sample step of L into *worst (running)
static float Run(bb::Engine& e, int n, float* worst = nullptr, float* last = nullptr)
{
    float l[24], r[24], p = 0.f;
    for(int k = 0; k < n / 24; k++)
    {
        e.Process(l, r, 24);
        for(int i = 0; i < 24; i++) { p = fmaxf(p, fabsf(l[i])); if(worst && last) { *worst = fmaxf(*worst, fabsf(l[i] - *last)); *last = l[i]; } }
    }
    return p;
}
// blocks until the voice falls silent (cap: `cap` blocks)
static int UntilQuiet(bb::Engine& e, const bb::Voice* v, int cap, float* worst = nullptr, float* last = nullptr)
{
    int n = 0;
    while(v->active && n < cap) { Run(e, 24, worst, last); n++; }
    return n;
}
static int Sounding(const bb::Engine& e, int key)   // voices of `key` (held or not: src for a grouped key, else key)
{
    int n = 0;
    for(int v = 0; v < bb::Engine::kVoices; v++) if(e.voices[v].active && (e.voices[v].src == key || e.voices[v].key == key)) n++;
    return n;
}
static void TestEnv()
{
    SECTION("ENV: the amp envelope — Attack, Decay, Sustain and Release on top of the click-safe fades, at the times asked, no click");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; Setup(e, L.data(), R.data()); e.muted = true; e.fadeMode = 0;
    const float amp = 0.5f, f = 441.f, slope = 6.2831853f * f / (float)kSr * amp, kOut = 0.8f;   // the tone's own steepest step; the engine's output gain
    const int t = Tone(e, 10, 96000, f, amp);
    CHECK(t >= 0 && e.KeySlice(10) == t, "a 2 s constant tone on key 10");
    // A 100 ms, D 200 ms to −12 dB, R 500 ms, held
    CHECK(SetP(e, 10, Env(2, 100, 200, 12, 500)) == bb::EditStatus::Ok && e.keyEnv[10].on, "key 10: Held, A 100 ms · D 200 ms · S −12 dB · R 500 ms");
    CHECK(Near(e.keyEnv[10].a, 1.f / 4800.f) && Near(e.keyEnv[10].s, powf(10.f, -12.f / 20.f), 1e-5f) && Near(powf(e.keyEnv[10].d, 9600.f), 1e-3f, 2e-3f) && Near(powf(e.keyEnv[10].r, 24000.f), 1e-3f, 2e-3f),
          "the coefficients: 1/4800 a sample, sustain 10^(−12/20), decay and release 60 dB in 9 600 / 24 000 samples");
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 10);
    bb::Voice* v = Fresh(e);
    CHECK(v && v->held && v->envSt == bb::Voice::kEnvAtk && v->env == 0.f, "the call: held, the envelope in its attack from 0");
    float worst = 0.f, last = 0.f;
    Run(e, 2400, &worst, &last);
    CHECK(v && Near(v->env, 0.5f, 2e-3f) && v->envSt == bb::Voice::kEnvAtk, "50 ms in: half way (%g)", v ? v->env : -1.f);
    const float pk1 = Run(e, 2400 - 24, &worst, &last);
    CHECK(v && v->envSt == bb::Voice::kEnvAtk && pk1 < amp, "…still rising just before 100 ms (peak %g)", pk1);
    Run(e, 48, &worst, &last);
    CHECK(v && v->envSt == bb::Voice::kEnvDec, "at 100 ms (± a block) the attack is done: decaying");
    Run(e, 9600, &worst, &last);
    const float S = e.keyEnv[10].s, rest = v ? (v->env - S) / (1.f - S) : -1.f;
    CHECK(rest > 4e-4f && rest < 2.5e-3f, "200 ms of decay later the way left to the sustain is −60 dB (%g of it)", rest);
    const float pkS = Run(e, 4800, &worst, &last);
    CHECK(Near(pkS, kOut * amp * S, 0.02f), "sustaining at −12 dB: the tone's peak %g ≈ %g (× the engine's 0.8 out)", pkS, kOut * amp * S);
    e.ReleaseKey(10);
    CHECK(v && v->active && !v->held && v->envSt == bb::Voice::kEnvRel, "the key comes up: the release runs (the voice reads on)");
    const float e0 = v ? v->env : 0.f;
    Run(e, 12000, &worst, &last);
    CHECK(v && Near(v->env / e0, 0.0316228f, 0.03f), "250 ms into the 500 ms release: −30 dB (%g)", v ? v->env / e0 : -1.f);
    const int n = UntilQuiet(e, v, 2000, &worst, &last);
    CHECK(v && !v->active && n * 24 >= 7200 - 48 && n * 24 <= 7200 + 48, "…and silent once 60 dB below full: the release is a rate (60 dB in 500 ms), so from −12 dB it takes 400 ms (%d more samples)", n * 24);
    CHECK(worst < slope * 1.1f, "no click anywhere — attack, decay, release: worst step %g (the tone's own %g)", worst, slope);
    // a release on a key with none: the choke geometry, as ever (attack only)
    SetP(e, 10, Env(2, 50, 0, 0, 0));
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 10);
    v = Fresh(e);
    Run(e, 480);
    e.ReleaseKey(10);
    int m = UntilQuiet(e, v, 100);
    CHECK(v && !v->active && m * 24 <= v->chokeLen + 48, "attack only: the key's up fades with the choke geometry (%d samples)", m * 24);
    // a one-shot: Decay to no sustain ends it (the drum sampler's decay); the release has nothing to shape
    SetP(e, 10, Env(1, 0, 300, bb::KeyParam::kSusOff, 2000));
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, true, 10);
    v = Fresh(e);
    CHECK(v && !v->held && v->envSt == bb::Voice::kEnvDec && v->env == 1.f && e.keyEnv[10].s == 0.f, "One-shot, D 300 ms, no sustain: from full, decaying to silence");
    m = UntilQuiet(e, v, 4000);
    CHECK(v && !v->active && m * 24 >= 14400 - 48 && m * 24 <= 14400 + 48, "…the voice ends when the decay reaches −60 dB: 300 ms (%d samples) of a 2 s tone", m * 24);
    SetP(e, 10, Env(1, 0, 0, 0, 2000));
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 10);
    v = Fresh(e);
    Run(e, 4800); e.ReleaseKey(10);
    CHECK(v && v->active && v->envSt == bb::Voice::kEnvSus, "a one-shot with a Release: its key's up changes nothing");
    m = UntilQuiet(e, v, 5000);
    CHECK(v && !v->active && 4800 + m * 24 >= 96000 - 48 && 4800 + m * 24 <= 96000 + 300, "…it plays to its own end, the edge guard as ever (%d samples)", 4800 + m * 24);
    // Loop with a release: looping on through it, untouched by other hits and the steps, then quiet
    SetP(e, 10, Env(3, 0, 0, 0, 400));
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 10);
    v = Fresh(e);
    CHECK(v && v->held && v->tail == 1, "Loop: held, looping");
    Run(e, 96000 + 4800);
    CHECK(v && v->active && v->looping, "…past its length");
    e.ReleaseKey(10);
    e.CallSlice(e.KeySlice(9), 1.f, false, false, false, 1.f, false, 9);   // another key
    e.muted = false; Run(e, 4800); e.muted = true;                         // and the steps
    CHECK(v && v->active && v->tail == 1 && v->envSt == bb::Voice::kEnvRel, "released: it loops on through the release; another hit and the steps leave it be");
    m = UntilQuiet(e, v, 2000);
    CHECK(v && !v->active && 4800 + m * 24 >= 19200 - 48 && 4800 + m * 24 <= 19200 + 48, "…quiet at 400 ms (%d samples)", 4800 + m * 24);
    // Ratchet with a release: each hit's predecessor still takes the choke fade (no pile-up); the last hit releases
    SetP(e, 10, {0, bb::KeyParam::kRatchet, 0, bb::KeyParam::kRatEighth, 0, 0, 0, 1500, 0, 0});
    Silence(e); e.CallSlice(t, 1.f, false, false, false, 1.f, false, 10);
    int most = 0;
    for(int k = 0; k < 96000 / 24; k++) { Run(e, 24); int s = 0; for(int q = 0; q < bb::Engine::kVoices; q++) s += e.voices[q].active; most = s > most ? s : most; }
    CHECK(most <= 2, "Ratchet ⅛ with a 1.5 s release: at most two voices at once while it runs (%d) — the retrigger fades the last hit as ever", most);
    e.ReleaseKey(10);
    int rel = 0; for(int q = 0; q < bb::Engine::kVoices; q++) if(e.voices[q].active && e.voices[q].envSt == bb::Voice::kEnvRel) rel++;
    CHECK(rel == 1 && !(e.keyRatOn & (1u << 10)), "the key's up: the ratchet stops, its last hit releases");
    Run(e, 96000);
    // steps never take a key's envelope or group: every key enveloped and grouped, a pattern-only session is the neutral one
    {
        static std::vector<float> L2(kCap), R2(kCap);
        bb::Engine a, b; Setup(a, L2.data(), R2.data()); Setup(b, L2.data(), R2.data());
        for(int k = 0; k < 24; k++) SetP(b, k, Env(k % 5, 30 * k, 100, k % 20, 500, 1 + k % 4));
        a.patternMode = b.patternMode = 0; a.patternKnob = b.patternKnob = 0.45f; a.RegenPattern(); b.RegenPattern();
        float al[24], ar[24], bl[24], br[24]; bool same = true;
        for(int blk = 0; blk < 2000 && same; blk++) { a.Process(al, ar, 24); b.Process(bl, br, 24); same = !memcmp(al, bl, sizeof al) && !memcmp(ar, br, sizeof ar); }
        CHECK(same, "every key with an envelope and a group: the pattern's steps render the neutral bits");
    }
    // the edit op: refusals, one step, a drag one step, undo works the coefficients out again
    {
        const int d0 = e.UndoDepth();
        const bb::KeyParam bad[] = {Env(0, 2001, 0, 0, 0), Env(0, 0, 4001, 0, 0), Env(0, 0, 0, 61, 0), Env(0, 0, 0, 0, 4001), Env(0, 0, 0, 0, 0, 5)};
        bool refused = true; const bb::KeyParam was = e.keyp[4];
        for(const bb::KeyParam& p : bad) refused = refused && SetP(e, 4, p) == bb::EditStatus::BadArgs;
        CHECK(refused && !memcmp(&e.keyp[4], &was, sizeof was) && e.UndoDepth() == d0, "attack > 2 s, decay > 4 s, sustain under −60 dB, release > 4 s, group 5: BadArgs, nothing written");
        CHECK(SetP(e, 4, Env(2, 2000, 4000, 60, 4000, 4)) == bb::EditStatus::Ok && e.UndoDepth() == d0 + 1, "the far ends: Ok, one step");
        SetP(e, 4, Env(2, 2000, 4000, 60, 10, 4)); for(int r2 = 20; r2 <= 400; r2 += 10) SetP(e, 4, Env(2, 2000, 4000, 60, r2, 4), true);
        CHECK(e.UndoDepth() == d0 + 2 && e.keyp[4].rel == 400, "a release slider dragged through 40 values: one more step");
        Undo(e);
        const bb::Engine::KeyEnv x = e.keyEnv[4], y = e.EnvOf(e.keyp[4]);
        CHECK(e.keyp[4].rel == 4000 && x.r == y.r && x.a == y.a && x.on, "Undo: the release back at 4 s, its coefficient worked out again");
        Undo(e);
        CHECK(e.keyp[4].Neutral() && !e.keyEnv[4].on && e.keyEnv[4].r == 0.f, "Undo: neutral, no envelope");
    }
}

/* ── 8. ENV: choke groups ─────────────────────────────────────────────── */
static void TestChoke()
{
    SECTION("ENV: choke groups — a key in a group cuts the other keys' voices in it within the choke time; never its own, never a step's");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; Setup(e, L.data(), R.data()); e.muted = true; e.fadeMode = 0;
    const int open = Tone(e, 10, 96000, 441.f, 0.5f), closed = Tone(e, 11, 24000, 660.f, 0.f), other = Tone(e, 12, 24000, 330.f, 0.5f);
    CHECK(open >= 0 && closed >= 0 && other >= 0, "an open hat (10, a 2 s tone), a closed hat (11, silent: what is heard is the cut), something else (12)");
    SetP(e, 10, Env(1, 0, 0, 0, 0, 1)); SetP(e, 11, Env(1, 0, 0, 0, 0, 1)); SetP(e, 12, Env(1, 0, 0, 0, 0, 2));
    Silence(e); e.CallSlice(open, 1.f, false, false, false, 1.f, false, 10);
    bb::Voice* a = Fresh(e);
    CHECK(a && a->grp == 1 && a->src == 10, "the open hat sounds, tagged group 1 / key 10");
    Run(e, 2400);
    e.CallSlice(other, 1.f, false, false, false, 1.f, false, 12);
    Run(e, 2400);
    CHECK(a && a->active, "a key of group 2 leaves it be");
    e.CallSlice(open, 1.f, false, false, false, 1.f, false, 10);
    bb::Voice* a2 = Fresh(e);
    Run(e, 2400);
    CHECK(a && a->active && a2 && a2->active && Sounding(e, 10) == 2, "struck again: both open hats sound — a key never chokes itself mid-slice (as today)");
    float worst = 0.f, last = 0.f;
    Run(e, 24, &worst, &last); worst = 0.f;   /* primed: the step from 0 into the running tones is not one */
    e.CallSlice(closed, 1.f, false, false, false, 1.f, false, 11);
    bb::Voice* c = Fresh(e);
    int n = 0; while((a->active || a2->active) && n < 100) { Run(e, 24, &worst, &last); n++; }
    CHECK(!a->active && !a2->active && n * 24 <= a->chokeLen + 24, "the closed hat: both open hats gone within the choke time (%d ≤ %d samples)", n * 24, a->chokeLen + 24);
    CHECK(c && c->active && c->src == 11, "…the closed hat sounds");
    CHECK(worst < 2.f * 6.2831853f * 441.f / (float)kSr * 0.5f * 1.1f, "…a fade, not a cut: worst step %g (the two tones' own slopes: %g)", worst, 2.f * 6.2831853f * 441.f / (float)kSr * 0.5f);
    // a held key, a looping key, a releasing key, a ratchet — all cut
    const char* what[] = {"held", "looping", "releasing (2 s release)", "ratcheting"};
    const bb::KeyParam how[] = {Env(2, 0, 0, 0, 0, 1), Env(3, 0, 0, 0, 0, 1), Env(2, 0, 0, 0, 2000, 1), {0, bb::KeyParam::kRatchet, 0, bb::KeyParam::kRatEighth, 1, 0, 0, 0, 0, 0}};
    for(int w = 0; w < 4; w++)
    {
        SetP(e, 10, how[w]);
        Silence(e); e.CallSlice(open, 1.f, false, false, false, 1.f, false, 10);
        Run(e, 9600);
        if(w == 2) e.ReleaseKey(10);
        Run(e, 480);
        const bool before = Sounding(e, 10) >= 1;
        e.CallSlice(closed, 1.f, false, false, false, 1.f, false, 11);
        int q = 0; while(Sounding(e, 10) && q < 100) { Run(e, 24); q++; }
        Run(e, 48000);   // a ratchet would have struck again by now
        CHECK(before && Sounding(e, 10) == 0 && q * 24 <= 240 + 24 && !(e.keyRatOn & (1u << 10)), "a %s open hat: cut within the choke time (%d samples), nothing after", what[w], q * 24);
        e.ReleaseKey(10);
    }
    // the steps: their voices carry no group, so a group key never cuts them, and a step never cuts a group's voice
    {
        e.muted = false; Run(e, 48000);
        bool none = true; for(int q = 0; q < bb::Engine::kVoices; q++) if(e.voices[q].active && e.voices[q].src < 0) none = none && e.voices[q].grp == 0;
        CHECK(none, "the pattern's voices: no group");
        SetP(e, 10, Env(3, 0, 0, 0, 0, 1));
        e.CallSlice(open, 1.f, false, false, false, 1.f, false, 10);
        bb::Voice* h = Fresh(e);
        Run(e, 48000);
        CHECK(h && h->active && h->src == 10, "a grouped Loop key under a running pattern: still sounding after a second of steps");
        e.ReleaseKey(10); e.muted = true;
    }
    // the spread copies the envelope and the group
    {
        SetP(e, 10, Env(2, 120, 300, 9, 800, 3));
        bb::EditReq q; q.op = bb::EditOp::SpreadKeys; q.key = 10; int cnt = 0;
        CHECK(e.ApplyEdit(q, &cnt) == bb::EditStatus::Ok && cnt > 0, "key 10 spread");
        bool same = true;
        for(int k = 0; k < 24; k++) { const bb::KeyParam& p = e.keyp[k]; if(e.KeySlice(k) != open) continue; same = same && p.atk == 120 && p.dec == 300 && p.sus == 9 && p.rel == 800 && p.grp == 3 && e.keyEnv[k].on; }
        CHECK(same, "…every key on its tone has its envelope and group (a chromatic, monophonic instrument: each key chokes the last)");
        Silence(e); e.CallSlice(open, 1.f, false, false, false, 1.f, false, 0);
        bb::Voice* p0 = Fresh(e); Run(e, 2400);
        e.CallSlice(open, 1.f, false, false, false, 1.f, false, 7);
        int q2 = 0; while(p0 && p0->active && q2 < 100) { Run(e, 24); q2++; }
        CHECK(p0 && !p0->active && q2 * 24 <= (int)(240.f / fabsf(p0->rate)) + 24, "C1 then G1 of the spread: C1 cut within the choke time (%d samples: 240 source samples at C1's rate %g)", q2 * 24, p0 ? fabsf(p0->rate) : 0.f);
        e.ReleaseKey(7);
    }
}

/* ── 6. CHROMA: spread across keys ────────────────────────────────────── */
static bb::EditStatus Spread(bb::Engine& e, int root, int flags = 0, int* count = nullptr, uint8_t gen = 0)
{
    bb::EditReq q; q.op = bb::EditOp::SpreadKeys; q.key = root; q.a = flags; q.gen = gen;
    int n = -1; const bb::EditStatus st = e.ApplyEdit(q, &n);
    if(count) *count = n;
    return st;
}
struct KeyState { int8_t keys[24]; bb::KeyParam kp[24]; float g[24], r[24]; };
static KeyState Keys(const bb::Engine& e)
{
    KeyState k;
    memcpy(k.keys, e.keySlice, sizeof k.keys); memcpy(k.kp, e.keyp, sizeof k.kp); memcpy(k.g, e.keyGain, sizeof k.g); memcpy(k.r, e.keyRate, sizeof k.r);
    return k;
}
static bool SameKeys(const KeyState& a, const KeyState& b) { return !memcmp(&a, &b, sizeof a); }
// the rate a call through key j starts its voice at (P4, the tempo and the key's tune all in), 0 = none
static float CallRate(bb::Engine& e, int j)
{
    Silence(e); e.CallSlice(e.KeySlice(j), 1.f, false, false, false, 1.f, false, j);
    const bb::Voice* v = Fresh(e);
    return v ? v->rate : 0.f;
}
static void TestSpread()
{
    SECTION("CHROMA: spread across keys — one sample on every key, a semitone a key, one undo step");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; Setup(e, L.data(), R.data()); e.muted = true;
    const int root = 2, t = e.KeySlice(root), pad7 = e.KeySlice(7);
    CHECK(e.OwnRate(t) && pad7 != t, "key 2 is a pad (its own rate), key 7 another pad");
    SetKp(e, root, -12, 30, 2);   // the root: −6 dB, +30 cents, Held
    SetKp(e, 11, 6, -500, 1);     // a key with settings of its own (the spread replaces them)
    const KeyState before = Keys(e);
    const int d0 = e.UndoDepth(); const uint8_t g0 = e.tableGen;
    int n = -1;
    CHECK(Spread(e, root, 0, &n) == bb::EditStatus::Ok && n == 23 && e.tableGen != g0, "spread key 2: Ok, 23 keys on it, the generation moves");
    CHECK(e.UndoDepth() == d0 + 1, "one undo step (depth %d → %d)", d0, e.UndoDepth());
    bool map = true, kp = true;
    for(int j = 0; j < 24; j++)
    {
        map = map && e.KeySlice(j) == t;
        const int want = (j - root) * 100 + 30;
        kp = kp && e.keyp[j].cents == want && e.keyp[j].gain == -12 && e.keyp[j].plays == 2 && e.keyp[j].rat == 0 && e.keyp[j].spare == 0;
    }
    CHECK(map, "every key plays the root's entry (key 7's pad replaced)");
    CHECK(kp, "key j: (j − 2) × 100 + 30 cents, the root's gain and Plays");
    CHECK(!memcmp(&e.keyp[root], &before.kp[root], sizeof(bb::KeyParam)) && e.keyRate[root] == before.r[root], "the root itself untouched");
    CHECK(e.tableKey[pad7] == 7 && !(e.tableFlags[pad7] & bb::Engine::kFlagTomb), "key 7's own recording stays in the table (owned, unplayed — undo brings it back)");
    // the tuning: rate_j = 2^((j − k)/12) × rate_k, at two tempi, and a pad's pitch does not follow the tempo
    float rk90 = 0.f;
    for(int pass = 0; pass < 2; pass++)
    {
        e.SetBpm(pass ? 170.f : 90.f); e.pitch = 1.25f;
        const float rk = CallRate(e, root);
        bool tuned = rk > 0.f; float worst = 0.f;
        for(int j = 0; j < 24; j++)
        {
            const float want = rk * powf(2.f, (float)(j - root) / 12.f), got = CallRate(e, j);
            const float err = fabsf(got - want) / want; if(err > worst) worst = err;
            tuned = tuned && err <= 2e-6f;
        }
        CHECK(tuned, "at %d BPM: key j's rate = 2^((j − 2)/12) × the root's (worst relative error %.2g)", pass ? 170 : 90, worst);
        if(!pass) rk90 = rk;
        else CHECK(rk == rk90 && Near(rk, 1.25f * powf(2.f, 0.3f / 12.f), 1e-6f), "a pad: the same pitch at 90 and 170 BPM (its own rate × P4 × the tune: %g)", rk);
    }
    // the shell's guard: a second spread plans nothing new
    { int8_t k2[24]; bb::KeyParam p2[24]; const int m = e.SpreadPlan(root, 0, k2, p2);
      CHECK(m == 23 && !memcmp(k2, e.keySlice, sizeof k2) && !memcmp(p2, e.keyp, sizeof p2), "the same spread again plans exactly what is there (the shell posts nothing)"); }
    // undo puts back every key's map and settings, gain and rate bit for bit; redo the spread
    const KeyState spread = Keys(e);
    CHECK(Undo(e) == bb::EditStatus::Ok && SameKeys(Keys(e), before), "Undo: all 24 keys' map, settings, gain and rate exactly as before");
    CHECK(e.UndoDepth() == d0, "…one step back");
    CHECK(Undo(e, true) == bb::EditStatus::Ok && SameKeys(Keys(e), spread), "Redo: the spread again, exactly");
    // a drag of the root's slider after the spread: a step of its own, never folded into the spread's record
    SetKp(e, root, -20, 30, 2, true);
    CHECK(e.UndoDepth() == d0 + 2, "a joined KEY_PARAMS right after a spread: its own step (depth %d)", e.UndoDepth());
    Undo(e);
    CHECK(SameKeys(Keys(e), spread), "…undone alone: the spread stays");
    Undo(e);
    CHECK(SameKeys(Keys(e), before), "then the spread");

    // keep pads: a key playing a recording other than the root's is left alone
    {
        const int dk = e.UndoDepth();
        CHECK(Spread(e, root, bb::Engine::kSpreadKeepPads, &n) == bb::EditStatus::Ok && n == 22, "keep pads: 22 keys spread (%d)", n);
        CHECK(e.KeySlice(7) == pad7 && !memcmp(&e.keyp[7], &before.kp[7], sizeof(bb::KeyParam)), "key 7 (its own pad) kept, map and settings");
        CHECK(e.KeySlice(11) == t && e.keyp[11].cents == 930, "key 11 (a grid key with settings) spread: +9 st from the root, +30 cents");
        CHECK(e.UndoDepth() == dk + 1 && Undo(e) == bb::EditStatus::Ok && SameKeys(Keys(e), before), "one step; undone exactly");
        // a key that copies the root's pad is not a pad to keep (it already plays the root's entry)
        { bb::EditReq k; k.op = bb::EditOp::SetKey; k.key = 20; k.t = t; e.ApplyEdit(k); }
        Spread(e, root, bb::Engine::kSpreadKeepPads, &n);
        CHECK(n == 22 && e.keyp[20].cents == 1830, "a key already on the root's pad is spread (+18 st)");
        Undo(e); Undo(e);
        CHECK(SameKeys(Keys(e), before), "(both undone)");
    }
    // the ends of the keyboard: any root fits (±23 semitones), the root's own tune clamped at ±24
    {
        Spread(e, 0); bool ok = true; for(int j = 1; j < 24; j++) ok = ok && e.keyp[j].cents == j * 100;
        CHECK(ok && e.keyp[23].cents == 2300, "root C1 (key 0): +23 st at the top");
        Undo(e);
        Spread(e, 23); ok = true; for(int j = 0; j < 23; j++) ok = ok && e.keyp[j].cents == (j - 23) * 100;
        CHECK(ok && e.keyp[0].cents == -2300, "root B2 (key 23): −23 st at the bottom");
        Undo(e);
        SetKp(e, 0, 0, 500, 0);
        Spread(e, 0);
        CHECK(e.keyp[18].cents == 2300 && e.keyp[19].cents == 2400 && e.keyp[23].cents == 2400, "a root tuned +5 st: the keys past +24 st clamp there (19..23 at +2400)");
        Undo(e); Undo(e);
    }
    // a grid slice spread: tuned the same, but its pitch follows the tempo (a varispeed slice of the break)
    {
        Spread(e, 5);
        e.pitch = 1.f;
        e.SetBpm(90.f);  const float a5 = CallRate(e, 5), a17 = CallRate(e, 17);
        e.SetBpm(170.f); const float b5 = CallRate(e, 5), b17 = CallRate(e, 17);
        CHECK(!e.OwnRate(e.KeySlice(5)) && Near(a17 / a5, 2.f, 2e-6f) && Near(b17 / b5, 2.f, 2e-6f), "a grid key spread: an octave up 12 keys on, at 90 and at 170 BPM");
        CHECK(Near(b5 / a5, 170.f / 90.f, 1e-5f), "…and the whole spread follows the tempo (×%g from 90 to 170 BPM)", b5 / a5);
        Undo(e);
    }
    // a running ratchet on a spread key stops (its key was re-pointed); undo of a re-point stops one too
    {
        SetKp(e, 9, 0, 0, 4, false, 1);
        Silence(e); e.CallSlice(e.KeySlice(9), 1.f, false, false, false, 1.f, true, 9);
        const bool ran = (e.keyRatOn >> 9) & 1u;
        Spread(e, root);
        CHECK(ran && !((e.keyRatOn >> 9) & 1u), "a ratchet running on key 9 stops when the spread re-points it");
        Undo(e);
        SetKp(e, root, 0, 0, 4, false, 1);
        Spread(e, root);   // every key a ratchet on the pad
        Silence(e); e.CallSlice(e.KeySlice(9), 1.f, false, false, false, 1.f, true, 9);
        const bool ran2 = (e.keyRatOn >> 9) & 1u;
        Undo(e);
        CHECK(ran2 && !((e.keyRatOn >> 9) & 1u), "undoing the spread stops key 9's ratchet on the root's pad (a ratchet before and after)");
        Undo(e); Undo(e); Silence(e); e.keyRatOn = 0;
    }
    // the refusals: nothing written, no step
    {
        const KeyState k0 = Keys(e); const int dr = e.UndoDepth(); const uint8_t gr = e.tableGen;
        const bool bad = Spread(e, 24) == bb::EditStatus::BadArgs && Spread(e, -1) == bb::EditStatus::BadArgs && Spread(e, 3, 2) == bb::EditStatus::BadArgs;
        CHECK(bad && SameKeys(Keys(e), k0) && e.UndoDepth() == dr && e.tableGen == gr, "a root out of 0..23, an unknown flag: BadArgs, nothing written, no step");
        CHECK(Spread(e, 3, 0, nullptr, (uint8_t)(e.tableGen + 1)) == bb::EditStatus::Stale && SameKeys(Keys(e), k0), "a stale gen: Stale");
    }
    CHECK(sizeof(bb::UndoRec) == 2336, "the journal's record did not grow (%zu B)", sizeof(bb::UndoRec));

    // the set: a spread (pads replaced) saves and loads as it is — the map onto the root's pad, 24 tunes; the pad
    // it replaced comes back still in the table
    {
        static std::vector<float> L2(kCap), R2(kCap);
        static bb::SetInfo s, u; static bb::SetPlan p;
        bb::Engine a; Setup(a, L.data(), R.data());
        SetKp(a, 7, -4, 0, 0);
        Spread(a, 7);
        bb::Looper none;
        new(&s) bb::SetInfo(); bb::SetFromEngine(a, none, s, true, kSr);
        s.bars = 1; s.bpm = a.NativeBpm();
        const std::string j = Emit(s);
        CHECK(bb::SetReader::Json(j.data(), j.size(), u) && bb::PlanSet(u, p), "a spread kit saved and planned (%zu B of JSON)", j.size());
        memcpy(L2.data(), L.data(), (size_t)a.freePos * sizeof(float)); memcpy(R2.data(), R.data(), (size_t)a.freePos * sizeof(float));
        bb::Engine b; b.Init(31); b.SetUndoJournal(&gJ2);
        b.SetMaterial(L2.data(), p.material, (float)kSr, p.onsets, p.nOnsets, p.cls, p.nGrid, R2.data(), kCap);
        b.SetGridBoundaries(p.starts, p.nGrid, p.gridFlags);
        int8_t live[24];
        bb::CommitSetRecordings(b, u, p, [](int, int) { return 0.f; }, live);
        if(p.hasKeys) b.SetKeyTable(live);
        bb::KeyParam kq[24]; bb::KeyParamsFromSet(u, kq); b.SetKeyParams(kq);
        bool same = true; const int bt = b.KeySlice(7);
        for(int k = 0; k < 24; k++) same = same && b.KeySlice(k) == bt && !memcmp(&a.keyp[k], &b.keyp[k], sizeof(bb::KeyParam)) && a.keyRate[k] == b.keyRate[k];
        CHECK(same && b.slices[bt].start == a.slices[a.KeySlice(7)].start, "after the load: every key on key 7's pad, every tune as saved");
        CHECK(b.RecordedEntry(2) >= 0 && b.RecordedEntry(2) != bt, "key 2's replaced pad is still in the kit (owned by key 2, unplayed)");
    }
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    TestNeutral();
    TestGainTune();
    TestPlays();
    TestRatchet();
    TestJournal();
    TestJson();
    TestSpread();
    TestEnv();     /* ENV */
    TestChoke();   /* ENV */
    printf("\nkeyp_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
