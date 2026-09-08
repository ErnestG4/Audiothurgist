#pragma once
#include <cmath>
#include <cstdint>
#include "bb_gas.h"
#include "bb_pattern.h"
#include "bb_looper.h"   /* LOOPER */

namespace bb {

// Pure engine: no libDaisy, no allocation, no printf. Fixed buffers; the
// shell owns the sample memory and the control mapping.
//
// SAMPLE (docs/packages/sample-engine.md, 2026-09-07): one material buffer,
// one slice table, two playback rules. The buffer is one or two planes
// (L, optional R) of bufCap samples; the material (the loaded break) fills
// [0, bufLen) and recordings are appended after it, so the table has a grid
// part [0, nSlices) — what the grammars, the clock and the bar rule see,
// unchanged — and a recorded part [nSlices, nTable). Every table entry is a
// {start, len} into the same planes; per-slice metadata (stepClass,
// sliceSal, sliceDens, stepHit, stepBands, loop points) is indexed by table
// index and sized kMaxTable. A 24-key table (keySlice) maps the Voice map's
// notes onto table entries. Thread ownership per call is stated at each
// declaration: "control" = the shell's main loop / poll, "audio" = the
// audio callback; the audio thread only ever touches plain arrays.

struct Slice
{
    int start;
    int len;
};

struct Voice
{
    bool   active = false;
    double pos    = 0.; // source sample position
    float  rate   = 1.f;
    int    start = 0, natEnd = 0, end = 0;
    int    bound = 0;        // SAMPLE: this voice's read limit (the material's
                             // end for a grid slice, its own end for a
                             // recorded one) — a fade never reads past it
    uint32_t age      = 0;
    float  lastOut    = 0.f;
    float  clickComp  = 0.f; // decaying offset that bridges a mid-note steal
    float  relScale   = 1.f; // keeps the envelope continuous through a choke
    /* SAMPLE: the R plane's bridge state (one envelope and position serve
     * both planes; the seam offsets are per plane because the material is).
     * Mono material never touches these. */
    float  lastOutR   = 0.f;
    float  clickCompR = 0.f;
    /* SAMPLE: a HELD call (the sampler rule) reads on to the end of the
     * material regardless of its slice, tagged with the key that struck it;
     * Engine::ReleaseKey fades every voice carrying that key. A held voice
     * belongs to its key: the sequencer's continuity branch leaves it alone. */
    bool   held = false;
    int8_t key  = -1;
    /* EDIT-AUDITION: the scratch voice (Engine::CallRegion) — a one-shot
     * with explicit bounds beside whatever plays. The sequencer's continuity
     * branch never adopts it, a hit never chokes it (WouldChoke is false), the
     * looper never sees it (CallRegion records nothing); only CallRegion
     * (which steals it with the 5 ms choke) and ReleaseRegion end it early. */
    bool   scratch = false;

    /* PLAY: per-voice fade geometry (Options → Fades). Trigger resets these
     * to the fixed 1 ms attack / 5 ms release; Adaptive mode then rewrites
     * them from the slice's keyframe density (Engine::ApplyFades). Ramps are
     * multiplies by a reciprocal — no per-sample division. epShape bends
     * both ramps parabolically (x·(2−x)): ≈ equal-power for the uncorrelated
     * cross-slice joins; linear stays for correlated (near-verbatim) ones. */
    float  atkInv   = 1.f / 48.f;  // attack gain per source sample
    float  guardInv = 1.f / 240.f; // edge-guard gain per source sample
    int    chokeLen = 240;         // release length when choked, source samples
    bool   epShape  = false;
    /* PLAY: tail policy (Options → Slice Tail). tail 1 loops [loopA, loopB)
     * forward, 2 ping-pongs between them; 0 plays to the natural end. Set by
     * Engine::SetTail only when the grid slot outlasts the slice. */
    int8_t tail    = 0;
    bool   looping = false;        // has wrapped at least once
    double loopA = 0., loopB = 0.; // sub-sample zero-crossing positions

    // ext > 1 plays past the slice's natural boundary, fading out across the
    // extension, so adjacent slices crossfade instead of butting — a frozen
    // repeating pattern otherwise exposes every seam on every pass.
    void Trigger(const Slice& s, float r, bool rev, int bufLen, float ext)
    {
        if(active) { clickComp = lastOut; clickCompR = lastOutR; } // steal: no waveform step, ~1ms decay
        bound  = bufLen;
        held   = false;
        key    = -1;
        scratch = false;   /* EDIT-AUDITION: a stolen scratch voice is an ordinary one again */
        start  = s.start;
        natEnd = s.start + s.len;
        int e  = s.start + (int)((float)s.len * ext);
        end    = e > bufLen ? bufLen : e;
        rate     = rev ? -r : r;
        // reverse starts at the NATURAL end, not the faded extension — the
        // slice's end-transient attacks immediately instead of swelling in
        // (-2: the interpolator reads i0+1, which must stay inside the slice)
        pos = rev ? (double)(natEnd - 2 > start ? natEnd - 2 : start)
                  : (double)start;
        age      = 0;
        relScale = 1.f;
        active   = true;
        /* PLAY: back to the fixed geometry; the engine overrides per slice */
        atkInv   = 1.f / 48.f;
        guardInv = 1.f / 240.f;
        chokeLen = 240;
        epShape  = false;
        tail     = 0;
        looping  = false;
    }

    // Would a new attack choke this voice? Voices ringing in their extension
    // or within 10 ms of their boundary (a jitter-early trigger must not
    // leave the old voice free to replay the new attack), and every looping
    // tail (PLAY): the tail fills silence and always yields to a hit.
    bool WouldChoke() const
    {
        if(scratch) return false;   /* EDIT-AUDITION: an audition sits beside the hits, never under them */
        return active && (tail != 0 || (rate >= 0.f && pos > (double)(natEnd - 480)));
    }

    // A new attack chokes any voice still ringing in its extension: the tail
    // fills silence, but must never replay the next slice's transient under
    // the incoming one (that reads as every hit doubled a few ms apart).
    void Choke(bool ep = false)
    {
        if(!WouldChoke()) return;
        Fade(ep);
    }

    /* SAMPLE: the key came up — a held voice fades out from here with the
     * normal choke geometry whatever its position (Choke only acts near a
     * boundary). A reversed held voice flips forward first, value-continuous,
     * exactly as a ping-pong tail does when choked. */
    void Release()
    {
        if(!active || !held) return;
        held = false;
        key  = -1;
        if(!tail && rate < 0.f) rate = -rate;
        Fade(epShape);
    }

    // The fade itself: a choke or a release, never past `bound`.
    void Fade(bool ep)
    {
        const int bufLen = bound;
        if(tail) /* PLAY: a tail is mid-slice at unity gain; fade out forward
                  * from here, never past the slice's own end */
        {
            if(rate < 0.f) rate = -rate; // ping-pong heading back: value-continuous flip
            int e = (int)pos + chokeLen;
            if(e > natEnd) e = natEnd;
            natEnd  = (int)pos;
            end     = e > bufLen ? bufLen : e;
            epShape = ep;
            tail    = 0;
            looping = false;
            return;
        }
        float h = (end > natEnd + 8)
                      ? 1.f - (float)(pos - (double)natEnd) / (float)(end - natEnd)
                      : 1.f;
        if(h < 0.f) h = 0.f;
        if(h > 1.f) h = 1.f;
        if(epShape) h = h * (2.f - h); // PLAY: restart at the SHAPED gain
        relScale *= h; // new 5 ms ramp starts exactly where the old gain was
        natEnd = (int)pos;
        int e  = (int)pos + chokeLen;
        end    = e > bufLen ? bufLen : e;
        epShape = ep;
    }

    /* SAMPLE: one reader for both planes. One position, one envelope, one
     * set of loop points; the sample fetch and the seam bridge are per plane.
     * ST = false is the mono reader, op for op the pre-SAMPLE Render (the
     * host suite holds goldens on it); ST = true adds the R plane's fetch
     * and bridge and leaves the L arithmetic untouched. */
    template <bool ST>
    void RenderT(const float* L, const float* R, float& oL, float& oR)
    {
        clickComp *= 0.93f;
        if constexpr(ST) clickCompR *= 0.93f;
        // direction-aware end test: a reversed voice STARTS at the top of its
        // slice — pos >= end-1 is only "finished" when travelling forward
        // (the symmetric check killed every reverse at birth once ext hit 1.0)
        bool ended = (rate < 0.f) ? (pos < (double)start)
                                  : (pos >= (double)(end - 1));
        if(ended)
        {
            if(active && lastOut != 0.f) clickComp += lastOut; // natural end
            if constexpr(ST)
            {
                if(active && lastOutR != 0.f) clickCompR += lastOutR;
                lastOutR = 0.f;
                oR       = clickCompR;
            }
            active  = false;
            lastOut = 0.f;
            oL      = clickComp;
            return;
        }
        /* PLAY: tail wrap. Loop forward jumps loopB → loopA (both are
         * positive-going zero crossings when the slice has any, so the join
         * is ~0 → ~0); ping-pong reflects at either end (value-continuous).
         * The output then continues from the previous sample through
         * clickComp, exactly as a steal does — the seam becomes a ~1 ms
         * decaying offset instead of a step. */
        bool wrapped = false;
        if(tail)
        {
            if(rate > 0.f && pos >= loopB)
            {
                if(tail == 1) pos -= loopB - loopA;
                else { rate = -rate; pos = 2. * loopB - pos; }
                wrapped = looping = true;
            }
            else if(rate < 0.f && pos <= loopA)
            {
                rate = -rate; pos = 2. * loopA - pos;
                wrapped = true;
            }
        }
        int   i0 = (int)pos;
        float fr = (float)(pos - (double)i0);
        float s  = L[i0] + fr * (L[i0 + 1] - L[i0]);
        float sR = 0.f;
        if constexpr(ST) sR = R[i0] + fr * (R[i0 + 1] - R[i0]);
        // envelope: ~1 ms attack; unity through the natural slice; release
        // across the extension (or a 5 ms edge guard when there is none)
        float dA = (float)(pos - (double)start);
        float g  = dA * atkInv;
        if(g > 1.f) g = 1.f;
        float h;
        if(pos <= (double)natEnd)
            h = 1.f;
        else if(end > natEnd + 8)
            h = 1.f - (float)(pos - (double)natEnd) / (float)(end - natEnd);
        else
            h = (float)((double)(end - 1) - pos) * guardInv;
        if(h > 1.f) h = 1.f;
        if(h < 0.f) h = 0.f;
        if(epShape) { g = g * (2.f - g); h = h * (2.f - h); } // PLAY: ≈ equal power
        pos += (double)rate;
        age++;
        const float o = s * g * h * relScale;
        if(wrapped) clickComp += lastOut - o; // PLAY: bridge the seam
        lastOut = o;
        oL      = lastOut + clickComp;
        if constexpr(ST)
        {
            const float o2 = sR * g * h * relScale;
            if(wrapped) clickCompR += lastOutR - o2;
            lastOutR = o2;
            oR       = lastOutR + clickCompR;
        }
    }
    // mono reader (the pre-SAMPLE signature)
    float Render(const float* buf)
    {
        float l = 0.f, r = 0.f;
        RenderT<false>(buf, buf, l, r);
        return l;
    }
    // SAMPLE: two planes; R == L (or nullptr) is mono and costs nothing extra
    void Render(const float* L, const float* R, float& outL, float& outR)
    {
        if(R && R != L) RenderT<true>(L, R, outL, outR);
        else { RenderT<false>(L, L, outL, outR); outR = outL; }
    }
};

/* ── EDIT (docs/packages/slice-edit-core.md): editing the table ──────────
 * Per-entry metadata as one record (the recorded entries' shadow copy, and
 * what an edit request carries for a new or moved region), and the edit
 * request the shell posts to the audio thread. Engine::ApplyEdit is the
 * only writer of slices[] once audio runs; the control thread analyses
 * (bb::AnalyseEntry, bb_sample.h) and posts, the audio thread applies at
 * block start. Field use per op is the table in Engine::ApplyEdit. */
struct EntryMeta { uint8_t cls; float sal, dens; bool hit; };

enum class EditOp : uint8_t
{
    None = 0,
    SetBoundary,   // grid, linked: slice i starts at `start`; i−1 ends there
    SetEntry,      // marker t = [start, start + len)
    AddEntry,      // a new marker [start, start + len), key assigned if given
    Split,         // marker t = [t.start, start); a new marker [start, t.end)
    Merge,         // marker t swallows the marker ending at its start (else the previous one)
    Tombstone,     // marker t: deleted (len 0, keys re-pointed, region kept)
    SetKey,        // key → entry t (grid or marker)
    SwapKeys,      // keys a and b trade entries
    NudgeAll,      // every grid boundary (b = 0) or marker start (b = 1) by `a` samples
    Promote        // SetBoundary(i, marker t's start)
};
enum class EditStatus : uint8_t
{
    Ok = 0,
    BadOp,        // unknown op
    BadArgs,      // an index, span or key out of range; a neighbour would drop under 64 samples
    Full,         // no table slot (kMaxTable, tombstones reused first)
    NotMarker,    // the op wants a marker (t ≥ nSlices) and got a grid slice
    NotGrid,      // SetBoundary / Promote: i is 0 or ≥ nSlices
    Tomb,         // the entry is a tombstone
    Stale,        // req.gen ≠ 0 and ≠ tableGen: the page's view is old — refetch, retry
    NoMaterial    // nothing loaded
};
struct EditReq
{
    static constexpr uint8_t kOrange = 1u;       // ADD: the entry is user-placed (flag b0)
    static constexpr uint8_t kMeta   = 1u << 7;  // `meta` is filled (AnalyseEntry ran)
    EditOp    op    = EditOp::None;
    uint8_t   gen   = 0;    // 0 = unchecked; else must equal Engine::tableGen
    uint8_t   flags = 0;
    int       t     = -1;   // the entry: SetEntry, Split, Merge, Tombstone, SetKey, Promote
    int       i     = -1;   // the grid slice: SetBoundary, Promote
    int       key   = -1;   // SetKey; AddEntry (−1 or ≥ kKeys = none)
    int       a = 0, b = 0; // SwapKeys (a, b) · NudgeAll (a = samples, b = which)
    int       start = 0;    // SetEntry / AddEntry: the span · SetBoundary / Split: the position
    int       len   = 0;    // SetEntry / AddEntry
    EntryMeta meta  = {3, 0.f, 0.f, false};
};

struct Engine
{
    static constexpr int kMaxSlices = 64;   // grid steps per loop: the grammars' world
    static constexpr int kVoices    = 8;
    /* SAMPLE */
    static constexpr int kMaxTable  = kMaxSlices + 32; // grid + recorded entries
    static constexpr int kKeys      = 24;              // Voice map notes 0..23
    static constexpr int kRecMin    = 480;             // 10 ms: the shortest usable recording
    /* EDIT */
    static constexpr int     kMaxOnsets  = 128;        // the onset table SetMaterial keeps (the shell's size)
    static constexpr int     kGridMin    = 64;         // the shortest grid slice an edit may leave
    static constexpr uint8_t kFlagOrange = 1u;         // tableFlags b0: user-placed / user-moved
    static constexpr uint8_t kFlagTomb   = 2u;         // tableFlags b1: deleted (len 0, no keys)

    float* buf     = nullptr; // L plane: the material, then the recordings
    float* bufR    = nullptr; // R plane, nullptr = mono                    /* SAMPLE */
    int    bufLen  = 0;       // the MATERIAL's length: the grid's domain
    int    bufCap  = 0;       // plane capacity; recordings may fill up to it /* SAMPLE */
    int    freePos = 0;       // end of USED material = the next recording's start /* SAMPLE */
    float  sr      = 48000.f;

    Slice slices[kMaxTable];
    int   nSlices = 0;            // grid entries [0, nSlices)
    int   nTable  = 0;            // grid + recorded entries [0, nTable)  /* SAMPLE */
    float slicePhase[kMaxSlices]; // onset positions as ring angles (gas wells)

    BetaGas gas; // dormant in the trigger path since v3.1 — remains the
                 // Phase 0 analysis object; may return as a selection bias
    Pattern  pat;
    float    patternKnob = 0.85f; // pot 1: 1 verbatim … 0 micro-chop
    uint64_t cellSeed = 0x5EED1, fillSeed = 0x5EED2;
    Rng      seedRng;
    uint8_t  stepClass[kMaxTable]; // per-slice kick/snare/hat/other (grid: per step)
    int      stepsPerBeat = 4;
    bool     flowRegen    = false; // hold-to-flow: new fill each bar wrap

    /* GRAM */ // Options → Swaps / Flow Déjà Vu (docs/packages/gram.md)
    float    sliceSal[kMaxTable] = {};  // per-slice salience 0..1, copied from the
                                        // shell's classifier output (gSal) — the
                                        // engine ranks it into Pattern::salTier
    int      swapsMode = 0;   // 0 class only (shipped), 1 class + salience tier
    int      dejaVu    = 0;   // 0 off, 1 some (¼ of flow bars replay), 2 lots (¾)
    Rng      dejaRng;         // its own stream: the bar sequence replays from Init's seed
    static constexpr int kDejaWindow = 8; // phrase memory: the newest N ring entries
    uint32_t dejaBars = 0, dejaRecalls = 0; // flow bars seen / replayed (diag, tests)
    /* GRAM end */

    double  barPhase   = 0.; // playhead sweep of the ring, [0,1)
    bool    extPhase   = false; // true: barPhase is set by the shell (clock
                               // sync); false: engine self-advances (internal)
    uint8_t gateFired  = 0;    // per-trigger class bitmask (1<<class); the
                               // shell reads+clears it to raise CV gates
    int8_t  gateSlice[4] = {-1, -1, -1, -1}; // source slice of the last trigger
                               // per class (pure data; shell maps to velocity)
    bool    gateAccent[4] = {}; /* GATES */ // the last trigger per class carried a
                               // grammar accent (stepAccent) — shell → velocity ceiling
    uint8_t stepBands[kMaxTable] = {}; /* GATES */ // per-slice band-presence mask
                               // (bit c = class c present, bb_classify.h rule);
                               // 0 = unknown → the dominant class is used
    bool    multiLabel = false; /* GATES */ // Options → Gate Lanes: raise every
                               // lane in stepBands instead of 1 << stepClass
    float   barSeconds = 2.f; // GRID duration: our tempo, not the file's
    float   nativeStep = 0.f; // source samples per source 16th
    float   bpm        = 0.f; // 0 = native tempo
    float   pitch      = 1.f; // extra rate multiplier on top of tempo match
    bool    reverse    = false;
    float   sliceExt   = 1.0f; // NO forward extension: source past a slice
                               // boundary IS the next hit's attack, so any
                               // overlap pre-echoes it (doubles whenever a
                               // slot outlasts its content — off-native tempo
                               // or pitched up). Gate honestly instead; the
                               // continuity path keeps verbatim runs seamless.
                               // (PLAY: Options → Slice Tail can instead loop
                               // the slice's OWN tail — a region that ends
                               // before the boundary, so still no pre-echo.)

    Voice    voices[kVoices];
    uint32_t trigFlash = 0; // incremented per trigger; shell reads + clears for LEDs

    /* PLAY: Options → Fades / Slice Tail. The shell writes the modes per
     * block and sliceDens[] (keyframes per sample per slice, the Density
     * lens of bb_classify.h) with each load or re-slice; 0 = unknown, which
     * falls back to the fixed lengths. Nothing here costs per sample: fade
     * lengths are resolved per trigger, loop points once per slice. */
    int   fadeMode = 0;              // 0 Fixed (1 ms attack, 5 ms release), 1 Adaptive
    int   tailMode = 0;              // 0 Gate (honest), 1 Loop forward, 2 Ping-pong
    float sliceDens[kMaxTable] = {};
    double loopA[kMaxTable] = {}, loopB[kMaxTable] = {}; // sub-sample loop region per slice
    int8_t loopState[kMaxTable] = {};                    // 0 unsearched, 1 found, -1 none
    static constexpr float kFadeKeyframes = 10.f; // a splice spans ~10 keyframes
    static constexpr int   kFadeMin = 48, kFadeMax = 480; // 1–10 ms (Nielsen floor,
                                                          // Driedger & Müller ceiling)

    // Release / edge-guard length for a slice: keyframe-dense (bright,
    // transient) → short, sparse (sustained) → long. Fixed mode: 5 ms.
    int FadeSamples(int s) const
    {
        if(fadeMode == 0 || s < 0 || s >= kMaxTable) return 240;
        const float d = sliceDens[s];
        if(d <= 0.f) return 240;
        const float f = kFadeKeyframes / d;
        return f < (float)kFadeMin ? kFadeMin : (f > (float)kFadeMax ? kFadeMax : (int)f);
    }
    // Attack ramp: a third of the release, floored at 1 ms (a kick can take
    // 2–3 ms without losing its click; a hat cannot).
    int AttackSamples(int s) const
    {
        if(fadeMode == 0) return kFadeMin;
        const int a = FadeSamples(s) / 3;
        return a < kFadeMin ? kFadeMin : a;
    }
    void ApplyFades(Voice& v, int s, bool ep) const
    {
        if(fadeMode == 0) return; // Trigger's defaults ARE the fixed geometry
        const int f = FadeSamples(s), a = AttackSamples(s);
        v.chokeLen = f;
        v.guardInv = 1.f / (float)f;
        v.atkInv   = 1.f / (float)a;
        v.epShape  = ep;
    }
    // Is the join into a slice starting at `cellStart` a cross-slice one?
    // True when some voice about to be choked is NOT within the continuity
    // window of that start (i.e. it was not heading into this very slice).
    bool JoinIsCross(int cellStart) const
    {
        if(fadeMode == 0) return false;
        for(int i = 0; i < kVoices; i++)
            if(voices[i].WouldChoke() && fabs(voices[i].pos - (double)cellStart) >= 160.)
                return true;
        return false;
    }
    // Loop region of a slice for the tail policies: from a positive-going
    // zero crossing at or after the slice's midpoint to one just before its
    // natural end, so the region never reaches the next slice's attack
    // (no pre-echo — the reason sliceExt is 1.0). Both crossings are refined
    // to sub-sample positions (linear, matching the reader's interpolation)
    // so the join carries no phase error. Without usable crossings (DC,
    // silence) the plain midpoint..end region serves and the wrap bridge in
    // Voice::Render declicks. Cached per slice; ≤ len/2 compares, once.
    void FindLoop(int s)
    {
        const Slice& c   = slices[s];
        const int natEnd = c.start + c.len;
        const int a0 = c.start + c.len / 2, b0 = natEnd - 2;
        double a = (double)a0, b = (double)b0;
        const int span = c.len / 4;
        int zb = -1, za = -1;
        for(int i = b0, k = 0; i > c.start && k < span; i--, k++)
            if(buf[i - 1] <= 0.f && buf[i] > 0.f) { zb = i; break; }
        for(int i = a0, k = 0; i + 1 < natEnd && k < span; i++, k++)
            if(buf[i] <= 0.f && buf[i + 1] > 0.f) { za = i + 1; break; }
        if(zb >= 0 && za >= 0 && zb - za >= 256)
        {
            a = (double)(za - 1) + (double)(-buf[za - 1] / (buf[za] - buf[za - 1]));
            b = (double)(zb - 1) + (double)(-buf[zb - 1] / (buf[zb] - buf[zb - 1]));
        }
        if(b - a < 64.) { loopState[s] = -1; return; }
        loopA[s]     = a;
        loopB[s]     = b;
        loopState[s] = 1;
    }
    void SetTail(Voice& v, int s)
    {
        if(tailMode == 0) return;
        if(loopState[s] == 0) FindLoop(s);
        if(loopState[s] < 0) return;
        v.tail  = (int8_t)tailMode;
        v.loopA = loopA[s];
        v.loopB = loopB[s];
    }

    // debug hook (desktop shell): log trigger times in seconds
    double* trigLog    = nullptr;
    int     trigLogMax = 0, trigLogN = 0;
    double  timeSec  = 0.;
    float   gasAccum = 0.f;
    int     curStep          = -1; // grid step the playhead currently occupies
    int     ratchetCountdown = -1; // samples until a 2nd-point 32nd retrigger
    float   ratchetPhase     = 0.f;

    void Init(uint64_t seed)
    {
        gas.Init(16, seed);
        seedRng.Seed(seed ^ 0xBEA7);
        dejaRng.Seed(seed ^ 0xDE7A); /* GRAM */
    }

    /* GRAM: rank the loop's salience into three tiers by tercile so the
     * grammars can refuse tier-crossing swaps. Thresholds are VALUES at the
     * tercile ranks, so ties collapse upward — a loop with uniform salience
     * (no classifier data, or a wall of hats) is ONE tier and swaps freely.
     * n ≤ 64: an insertion sort on a stack copy, no allocation. */
    void ComputeSalTiers()
    {
        pat.salSwaps = swapsMode == 1;
        if(!pat.salSwaps) return;
        const int n = nSlices > 0 ? nSlices : 1;
        float     v[kMaxSlices];
        for(int i = 0; i < n; i++) v[i] = sliceSal[i];
        for(int i = 1; i < n; i++)
        {
            const float x = v[i];
            int         j = i;
            while(j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
            v[j] = x;
        }
        const float t1 = v[n / 3], t2 = v[(2 * n) / 3];
        for(int i = 0; i < kMaxSlices; i++)
            pat.salTier[i] = (uint8_t)(i >= n ? 0 : sliceSal[i] >= t2 ? 2 : sliceSal[i] >= t1 ? 1 : 0);
    }

    // native tempo of the loaded material, from the bar-count estimate
    float NativeBpm() const
    {
        return nativeStep > 0.f ? 15.f * sr / nativeStep : 120.f;
    }

    void SetBpm(float b) // 0 = play at the material's own tempo
    {
        bpm = b;
        float stepSec = (b > 1.f) ? 15.f / b : nativeStep / sr;
        barSeconds    = stepSec * (float)nSlices;
    }

    // Flattened per-step trigger map. Cells define IDENTITY (which source
    // steps, what order); playback re-anchors at EVERY grid step — a 4-step
    // cell is four consecutive slice-triggers. This is what makes tempo and
    // pitch truly decouple: tempo re-spaces every hit, and pitch drift can
    // never exceed one slice before the next attack snaps back to the grid.
    int8_t stepSrc[kMaxSlices];
    bool   stepRev[kMaxSlices];
    int8_t stepRatN[kMaxSlices];     // retriggers after the attack: 0 none,
                                     // 1 = 32nd double (Classic), 2..3 =
                                     // geometric series (Collins WarpCut)
    bool   stepRatAccel[kMaxSlices]; // series gaps halve (accel) or double
    int8_t stepJit[kMaxSlices];      // per-cell repitch jitter ×0.001 on Collins
                                     // repeats (anti machine-gun); 0 in Classic
    bool   stepCont[kMaxSlices] = {}; // rest resolved as a CONTINUATION: plays the
                                      // source slice after the previous step's,
                                      // verbatim through the continuity branch;
                                      // raises no gate and is not a "hit"
    bool   stepAccent[kMaxSlices] = {}; // grammar accent on this step (Pattern::accent)
    bool   stepHit[kMaxTable] = {};   // the source slice begins with a detected
                                      // onset (a drum's "open"); from the load's
                                      // onset table. All true when none given.
    /* SLICE: the material's own 16th swing (Options → Swing: Auto). `swing`
     * is the odd 16th's position inside its 8th, 0.5 = straight; the shell
     * sets it BEFORE SetMaterial from bb::EstimateSwing. When swung, odd
     * slice BOUNDARIES sit at that offset (a straight cut opens the hat
     * slice with the kick's tail and shears the hat off the kick slice) and
     * odd STEPS fire at the same offset, so rearrangement keeps the groove
     * (straight timing of swung slices would straighten it). 0.5 leaves
     * every path below byte-identical to v0.8.0. */
    float  swing = 0.5f;
    bool   swung = false;             // swing applied to the current slices
    bool   gateOnsetsOnly = false;    // gates fire once per HIT, not on every
                                      // step that carries the class (a snare's
                                      // decay steps are still "snare" — that was
                                      // the rapid-fire the field report meant)
    // PLAY grammar (patternMode 5): the bar is what the player put there.
    // Punches (Voice, web) land in this layer as well as in stepSrc, so a
    // regeneration (dice, flow, a Brain knob) re-overlays them; Reslice (B3)
    // clears the bar — "if you don't like it, re-slice it".
    int8_t playSrc[kMaxSlices];
    bool   playRev[kMaxSlices];
    void   ClearPlay() { for(int i = 0; i < kMaxSlices; i++) { playSrc[i] = -1; playRev[i] = false; } }
    void   PunchStep(int step, int src, bool rev)
    {
        if(patternMode == 6) return;   /* Pads: nothing is ever written — you keep the beat */
        if(step < 0 || step >= nSlices || src < 0 || src >= nTable) return;   /* SAMPLE: a recording may be punched in */
        if(tableFlags[src] & kFlagTomb) return;                               /* EDIT: a deleted marker is not */
        stepSrc[step]      = (int8_t)src;
        stepRev[step]      = rev;
        stepRatN[step]     = 0;      /* a punched hit is plain: no roll, no jitter */
        stepRatAccel[step] = false;
        stepJit[step]      = 0;
        stepCont[step]     = false;
        stepAccent[step]   = false;
        playSrc[step]      = (int8_t)src;
        playRev[step]      = rev;
    }
    /* LOOPER (docs/packages/sample-looper.md): the unquantized looper — Play's
     * memory when Options → Quantize is Off or 8ths. A call in Play records
     * itself (its phase, key, style, held) instead of punching a step, and
     * Process fires the events back through the same call path, sample-exact
     * within the block, every pass. Quantize = 16ths keeps Play exactly as
     * it was (punch into steps, byte-identical): the looper never records
     * or fires then. The list survives a grammar change (silent until Play
     * again); B3 (Reslice) clears it with the punch bar; B2 undoes the last
     * pass's overdub. */
    Looper looper;
    int    looperQuant = 0;   // Options → Quantize: 0 Off · 1 16ths (punch, looper off) · 2 8ths
    bool   LooperActive() const { return patternMode == 5 && looperQuant != 1; }
    static uint8_t LooperStyle(float rateMul, bool rev, bool ratchet, float lenMul, bool held)
    {
        uint8_t s = 0;
        if(rev) s |= Looper::kRev;
        if(rateMul < 0.75f) s |= Looper::kHalf;
        else if(rateMul > 1.5f) s |= Looper::kDouble;
        if(ratchet) s |= Looper::kRatchet;
        if(lenMul < 0.999f) s |= Looper::kStaccato;
        if(held) s |= Looper::kHeld;
        return s;
    }
    /* LOOPER end */
    bool   restSustain = false;       // languages (Grids/Euclid): a rest continues
                                      // the break instead of gating a 16th of
                                      // silence — the shell sets this per grammar.
                                      // Collins keeps its rests: they are figures.
    int    ratchetK    = -1;         // step whose retrigger series is pending
    int    ratchetLeft = 0, ratchetIval = 0;
    bool   ratchetAccel = false;
    int    patternMode = 0;   // 0 Classic, 1 Collins, 2 Grids, 3 Euclid, 4 Camp (GRAM) — see bb_pattern.h
    float  langX = 0.5f, langY = 0.5f, langDens[3] = {0.5f, 0.5f, 0.5f}, langRot = 0.f;
    int    loopCount   = 0;   // wraps since (re)generation: fill hyperperiod
    bool   fillActive  = false;
    Rng    jitRng;

    // Pattern history: every generated pattern is a tiny {seeds, knob} tuple,
    // so the last 16 rolls — taps, reslices, and each bar of flow — replay
    // deterministically. The encoder scrubs this timeline; a new roll always
    // appends at the head.
    struct PatState
    {
        uint64_t cell, fill;
        float    knob;
    };
    static constexpr int kHist = 16;
    PatState hist[kHist];
    int      histN = 0, histHead = -1, histPos = -1;

    void PushHistory()
    {
        histHead       = (histHead + 1) % kHist;
        hist[histHead] = {cellSeed, fillSeed, patternKnob};
        histPos        = histHead;
        if(histN < kHist) histN++;
    }

    // dir: -1 = older, +1 = newer. Returns depth from head (0 = latest).
    int Scrub(int dir)
    {
        if(histN == 0) return 0;
        int depth = (histHead - histPos + kHist) % kHist;
        depth -= dir;
        if(depth < 0) depth = 0;
        if(depth > histN - 1) depth = histN - 1;
        histPos = (histHead - depth + kHist) % kHist;
        const PatState& s = hist[histPos];
        ApplyPattern(s.knob, s.cell, s.fill);
        return depth;
    }

    void RegenPattern()
    {
        if(histN == 0) PushHistory(); // boot pattern is scrubbing's floor
        ApplyPattern(patternKnob, cellSeed, fillSeed);
    }

    // Flatten cells of one layer over [from, to) into per-step triggers.
    void FlattenRange(int from, int to, const int8_t* S, const int8_t* L,
                      const bool* R, const int8_t* RN, const bool* RA, const bool* RP,
                      const bool* AC = nullptr)
    {
        int c = from;
        while(c < to)
        {
            int Lc = L[c] > 0 ? L[c] : 1;
            if(c + Lc > to) Lc = to - c;
            // one jitter per CELL so continuity holds inside the cell
            int8_t jit = (patternMode == 1 && RP[c])
                             ? (int8_t)((jitRng.Uniform() - 0.5f) * 30.f) : 0;
            for(int j = 0; j < Lc; j++)
            {
                if(S[c] >= 0)
                {
                    int s = S[c] + (R[c] ? (Lc - 1 - j) : j);
                    if(s >= nSlices) s -= nSlices; // wrap, don't stutter the tail
                    stepSrc[c + j]      = (int8_t)s;
                    stepRev[c + j]      = R[c];
                    stepRatN[c + j]     = RN[c];
                    stepRatAccel[c + j] = RA[c];
                    stepJit[c + j]      = jit;
                    stepCont[c + j]     = false;
                    stepAccent[c + j]   = AC ? AC[c] : false;
                }
                else
                {
                    // rest or unfilled. With restSustain the rest CONTINUES the
                    // break: the slice after whatever the previous step plays
                    // (wrapping), so the continuity branch extends the running
                    // voice verbatim — no gate, no re-splice, no 16th of silence
                    // (the Grids map's last 16th is a rest at most densities:
                    // that hole before the downbeat read as a dropout).
                    // Leading rests (nothing played yet) and rests after a
                    // reversed step continue IN PLACE — so densities at zero
                    // give the plain break, not silence.
                    const int  p      = c + j - 1;
                    const bool prevOk = p >= 0 && stepSrc[p] >= 0 && !stepRev[p];
                    const bool cont   = restSustain;
                    int8_t s = -1;
                    if(cont) { int q = prevOk ? stepSrc[p] + 1 : c + j; if(q >= nSlices) q -= nSlices; s = (int8_t)q; }
                    stepSrc[c + j]      = s;
                    stepRev[c + j]      = false;
                    stepRatN[c + j]     = 0;
                    stepRatAccel[c + j] = false;
                    stepJit[c + j]      = 0;
                    stepCont[c + j]     = cont;
                    stepAccent[c + j]   = false;
                }
            }
            c += Lc;
        }
    }
    void FlattenAll()
    {
        FlattenRange(0, nSlices, pat.src, pat.len, pat.rev, pat.ratN, pat.ratAccel, pat.rep,
                     pat.accent);
        if(fillActive && pat.fillStart >= 0 && pat.fillStart < nSlices)
            FlattenRange(pat.fillStart, nSlices, pat.fsrc, pat.flen, pat.frev,
                         pat.fratN, pat.fratAccel, pat.frep);
        if(patternMode == 5)   /* Play: overlay the bar the player put there */
            for(int i = 0; i < nSlices; i++)
                if(playSrc[i] >= 0 && playSrc[i] < nSlices)
                {
                    stepSrc[i] = playSrc[i]; stepRev[i] = playRev[i];
                    stepRatN[i] = 0; stepRatAccel[i] = false; stepJit[i] = 0; stepCont[i] = false; stepAccent[i] = false;
                }
    }
    void ApplyPattern(float knob, uint64_t cs, uint64_t fs)
    {
        pat.langX = langX; pat.langY = langY; pat.langRot = langRot;
        for(int c = 0; c < 3; c++) pat.langDens[c] = langDens[c];
        ComputeSalTiers(); /* GRAM */
        for(int i = 0; i < kMaxSlices; i++) pat.hit[i] = stepHit[i];   /* language modes prefer onset slices */
        pat.useHits = true;
        pat.Generate(nSlices, stepsPerBeat, stepClass, knob, cs, fs, patternMode);
        jitRng.Seed(cs ^ (fs * 0x9E3779B97F4A7C15ULL));
        loopCount        = 0;
        fillActive       = false;
        ratchetCountdown = -1; // a pending double would read the NEW stepSrc
        ratchetLeft      = 0;  // (possibly a rest) — drop it with the old pattern
        FlattenAll();
    }
    void RerollFill() // button 1: same chop, new arrangement
    {
        /* GRAM: Flow Déjà Vu. A FLOW-bar reroll (flowRegen set — Process
         * calls us at the wrap) may REPLAY a bar from the history ring instead
         * of drawing fresh seeds: Marbles' DEJA VU "probability of re-using the
         * loop", drawn uniformly over the newest kDejaWindow entries (its
         * "jump within the loop"); Margulis 2013 — repetition, immediate or
         * delayed, is rewarded. A replay is not a roll: it does NOT push, so
         * the ring stays the timeline of new ideas and a scrub never lands on
         * a duplicate; histPos moves to the replayed slot so the P3 depth pip
         * shows which bar is sounding. Taps (flowRegen clear) are always
         * fresh. dejaRng is a separate stream: the same Init seed and the
         * same option give the same bar sequence, and with the option off
         * seedRng's stream is untouched (byte-identical flow). */
        if(flowRegen)
        {
            dejaBars++;
            if(dejaVu > 0 && histN > 0)
            {
                const float p = dejaVu >= 2 ? 0.75f : 0.25f;
                if(dejaRng.Uniform() < p)
                {
                    const int win   = histN < kDejaWindow ? histN : kDejaWindow;
                    const int depth = (int)(dejaRng.Uniform() * (float)win) % win;
                    histPos         = (histHead - depth + kHist) % kHist;
                    const PatState& s = hist[histPos];
                    ApplyPattern(s.knob, s.cell, s.fill);
                    dejaRecalls++;
                    return;
                }
            }
        }
        /* GRAM: Camp rings the NEXT change — its fill seed is a row index */
        if(patternMode == 4) fillSeed += 1;
        else
        fillSeed = ((uint64_t)seedRng.Next() << 32) | seedRng.Next();
        PushHistory();
        ApplyPattern(patternKnob, cellSeed, fillSeed);
    }
    void Reslice() // button 2: new chop tree (Play: clear the bar)
    {
        if(patternMode == 5) { ClearPlay(); looper.Clear(); /* LOOPER: B3 clears the loop too */ }
        cellSeed = ((uint64_t)seedRng.Next() << 32) | seedRng.Next();
        PushHistory();
        ApplyPattern(patternKnob, cellSeed, fillSeed);
    }

    // Traditional grid slicing: the loop divides into equal 16th-note steps
    // of its estimated musical bar (8ths past 4 bars, capped at kMaxSlices).
    // Onsets are kept for future use (velocity/salience); the grid is what
    // plays — chop instruments live on the grid, not on free time.
    // nSlicesHint (multiple of 16, 0 = none): the shell's bar estimate or the
    // user's Bars override. Without it the duration prior below applies.
    //
    // SAMPLE (audio thread in the shell, or before audio starts): `b`/`bR`
    // are the L/R planes (bR nullptr = mono), `cap` their capacity in
    // samples (0 = len: no room to record, the pre-SAMPLE behaviour). The
    // material occupies [0, len); recordings are appended past it. A
    // re-slice of the SAME planes (same pointers) keeps every recorded entry
    // that lies past the new material (their keys, metadata and memory);
    // anything else — new planes, a material that grew over them — is new
    // material and the table starts over. A take in flight is cancelled.
    void SetMaterial(float* b, int len, float sampleRate, const int* onsets,
                     int nOnsets, const uint8_t* classes = nullptr, int nSlicesHint = 0,
                     float* bR = nullptr, int cap = 0)
    {
        const bool newPlanes = b != buf;   /* LOOPER: a load empties the loop; a re-apply keeps it */
        /* SAMPLE / EDIT: which entries past the grid survive? The same planes
         * keep every marker that is not a tombstone, still lies inside the
         * used region, and is not a recording the new material grew over
         * (it began past the old material and inside the new). Survivors
         * are compacted in place here — order kept, dest ≤ src, so nothing
         * is overwritten before it moves — and relocated behind the new
         * grid below. `map` (old index → new, −1 dropped; grid indices map
         * to themselves) re-targets the looper's keyless events. */
        const int keepFrom = nSlices;
        const int oldN = nSlices, oldTable = nTable, oldLen = bufLen;
        const bool samePlanes = b == buf && bR == bufR;
        int8_t oldKeys[kKeys];
        for(int k = 0; k < kKeys; k++) oldKeys[k] = keySlice[k];
        int8_t map[kMaxTable];
        for(int t = 0; t < kMaxTable; t++) map[t] = (int8_t)(t < oldN ? t : -1);
        int       keepN    = 0;
        if(samePlanes && nTable > nSlices)
        {
            int w = oldN;
            for(int t = oldN; t < oldTable; t++)
            {
                const Slice& s = slices[t];
                const bool grownOver = s.start >= oldLen && s.start < len;
                const bool keep = !(tableFlags[t] & kFlagTomb) && s.len > 0 && !grownOver
                                  && s.start >= 0 && s.start + s.len <= freePos;
                if(!keep) continue;
                if(w != t) MoveEntry(t, w);
                map[t] = (int8_t)w;
                w++;
            }
            keepN = w - oldN;
        }
        recArmed   = false;
        recPending = false;
        recKey     = -1;
        const int oldFree = freePos;
        /* EDIT: the onset table, for per-entry hit refreshes after an edit */
        onsetsGiven = onsets != nullptr && nOnsets > 0;
        nOnsetTab   = 0;
        if(onsetsGiven)
            for(int o = 0; o < nOnsets && nOnsetTab < kMaxOnsets; o++)
                if(onsets[o] >= 0 && onsets[o] < len) onsetTab[nOnsetTab++] = onsets[o];

        buf    = b;
        bufR   = bR;
        bufLen = len;
        sr     = sampleRate;

        int bars = 1;
        if(nSlicesHint >= 16) nSlices = nSlicesHint;
        else
        {
            float dur = (float)len / sampleRate;
            while(dur / (float)bars >= 3.2f && bars < 16) bars <<= 1;
            nSlices = bars * 16;
        }
        while(nSlices > kMaxSlices) nSlices /= 2;
        if(nSlices < 1) nSlices = 1;
        bars = nSlices / 16 > 0 ? nSlices / 16 : 1;

        /* SAMPLE: relocate the kept recordings to [nSlices, nSlices + keepN)
         * BEFORE the grid is written over [0, nSlices). In place, in the safe
         * direction; a bigger grid may leave no room for the last ones. */
        if(keepN > 0)
        {
            if(nSlices + keepN > kMaxTable) keepN = kMaxTable - nSlices;
            if(nSlices < keepFrom)
                for(int i = 0; i < keepN; i++) MoveEntry(keepFrom + i, nSlices + i);
            else if(nSlices > keepFrom)
                for(int i = keepN - 1; i >= 0; i--) MoveEntry(keepFrom + i, nSlices + i);
        }
        for(int t = oldN; t < oldTable; t++)   /* EDIT: the map's final form */
            if(map[t] >= 0) { const int r = map[t] - oldN; map[t] = (int8_t)(r < keepN ? nSlices + r : -1); }
        nTable  = nSlices + keepN;
        for(int i = 0; i < nSlices; i++) tableFlags[i] = 0;              /* EDIT: the fresh grid is white; the overlay re-marks */
        for(int t = nTable; t < kMaxTable; t++) tableFlags[t] = 0;
        bufCap  = cap > 0 ? cap : (keepN > 0 ? bufCap : len);
        if(bufCap < len) bufCap = len;
        freePos = keepN > 0 ? oldFree : len;

        int step = len / nSlices;
        for(int i = 0; i < nSlices; i++)
            slices[i] = {i * step, step};
        slices[nSlices - 1].len = len - slices[nSlices - 1].start;
        nativeStep = (float)step;
        /* SLICE: swung boundaries — each odd slice starts at `swing` of its
         * 8th; the even slice before it grows by the same amount. The average
         * step (nativeStep) is unchanged, so varispeed still consumes exactly
         * one slice per slot. */
        swung = swing > 0.5f && swing < 0.75f && nSlices >= 2 && (nSlices & 1) == 0;
        if(swung)
        {
            const int cell = 2 * step;
            for(int i = 0; i + 1 < nSlices; i += 2)
            {
                const int odd       = i * step + (int)((float)cell * swing + 0.5f);
                const int next      = (i + 2 < nSlices) ? (i + 2) * step : len;
                slices[i].len       = odd - slices[i].start;
                slices[i + 1].start = odd;
                slices[i + 1].len   = next - odd;
            }
        }

        for(int i = 0; i < nSlices; i++)
            stepClass[i] = classes ? classes[i] : 3;
        for(int i = 0; i < kMaxTable; i++) loopState[i] = 0; /* PLAY: new grid, re-search */

        // Hits: a slice "opens" with a drum if an onset falls in its first
        // quarter (or within 10 ms before its start). No onset table = every
        // slice is a hit, which is the old behaviour.
        for(int i = 0; i < nSlices; i++) stepHit[i] = (onsets == nullptr || nOnsets <= 0);
        if(onsets && nOnsets > 0)
            for(int o = 0; o < nOnsets; o++)
            {
                const int p = onsets[o];
                if(p < 0 || p >= len) continue;
                int s = p / (step > 0 ? step : 1);
                if(s >= nSlices) s = nSlices - 1;
                if(swung)   /* SLICE: boundaries are no longer i·step */
                {
                    while(s + 1 < nSlices && p >= slices[s + 1].start) s++;
                    while(s > 0 && p < slices[s].start) s--;
                }
                if(p - slices[s].start <= slices[s].len / 4) stepHit[s] = true;
                else if(s + 1 < nSlices && slices[s + 1].start - p <= (int)(0.010f * sampleRate))
                    stepHit[s + 1] = true;
            }
        ApplyOverlay(samePlanes);   /* EDIT: user-moved grid boundaries come back on the same material */
        SyncRecordedMeta();   /* SAMPLE: the kept recordings' class/salience/density/hit */
        /* SAMPLE / EDIT: the key table. New material: keys span the grid and
         * every recorded entry claims its key. The same planes: each key
         * follows its entry (a kept marker → its new index; a grid slice →
         * itself when the count is unchanged, else the span default). */
        if(samePlanes) RestoreKeyTable(oldKeys, map, oldN, oldTable);
        else           DefaultKeyTable();
        SetBpm(0.f); // every load opens at ITS OWN tempo — inheriting the
                     // previous break's BPM chipmunks the new one (field bug)
        RegenPattern();
        barPhase   = 0.;
        for(int i = 0; i < kVoices; i++) { voices[i].active = false; voices[i].held = false; voices[i].key = -1; } // old slice
        // bounds are meaningless against new material
        gas.SetN(nSlices); // event density defaults to the material's own
                           // feature count (SetN clamps; shell may override)
        // wells at step CENTERS: a point on a window boundary is a knife edge
        // (jitter re-buckets it into the neighbouring step — the doubling bug
        // twice over); mid-window points are unambiguous
        for(int i = 0; i < nSlices; i++)
            slicePhase[i] = ((float)slices[i].start + 0.5f * (float)slices[i].len)
                            / (float)len * 6.28318531f;
        gas.wells  = slicePhase;
        gas.nWells = nSlices;
        // constant lock tightness in TIME across ring durations (ref: 2 s bar)
        float ts = (barSeconds / 2.f) * (barSeconds / 2.f);
        gas.wellTimeScale = ts < 0.25f ? 0.25f : (ts > 32.f ? 32.f : ts);
        gas.clusterBunches = bars;
        ClearPlay();
        curStep            = -1;
        ratchetCountdown   = -1;
        ratchetLeft        = 0;
        callCountdown      = -1; // a ratcheted Voice call from the OLD material
        loopCount          = 0;
        fillActive         = false;
        /* LOOPER: phases are bar-relative, so a re-slice of the same material
         * keeps the loop (rescaled to the new bar); new planes start empty.
         * barPhase was reset above: the window frontier is forgotten. */
        looper.divisions = nSlices;
        looper.ResetPlayhead();
        if(newPlanes) looper.Clear();
        else if(samePlanes) looper.RemapSlices(map, oldTable);   /* EDIT: keyless events follow the compaction */
        BumpGen();   /* EDIT: indices moved; a page holding the old gen must refetch */
    }

    // ── Voice role (M3): the rack plays slices ────────────────────────
    // Replace-don't-layer: a call chokes everything sounding and fires the
    // slice; PUNCH additionally writes it into the looping bar at the current
    // step (playing IS editing). muted silences pattern steps only — the
    // playhead keeps running so unmute lands in time; calls still sound.
    bool  muted         = false;
    int   callCountdown = -1;   // pending 32nd double for a ratcheted call
    int   callIdx       = -1;
    float callRate      = 1.f;
    bool  callRev       = false;
    bool  callHeld      = false; /* SAMPLE: the double of a held call is held too */
    int   callKey       = -1;

    /* GATES: raise the lanes for source slice `idx`. Dominant class: the
     * lens's label (1 << stepClass). Multi-label: every band the classifier
     * marked present (stepBands), the dominant class when no mask is known.
     * Per-class bookkeeping (slice → salience, accent → ceiling) for the
     * shell's velocity mapping. Callers decide WHETHER a gate is due
     * (continuations, onsets-only) — this only says WHICH lanes. */
    void RaiseGates(int idx, bool accent)
    {
        uint8_t m = (uint8_t)(1u << (stepClass[idx] & 3));
        if(multiLabel && stepBands[idx]) m = stepBands[idx];
        gateFired |= m;
        for(int c = 0; c < 4; c++)
            if(m & (1u << c)) { gateSlice[c] = (int8_t)idx; gateAccent[c] = accent; }
    }

    uint8_t callHits    = 0;    // calls fired since the shell last looked (hit trigger out)
    int     lastCallIdx = -1;   // slice of the last call (velocity out)
    float   callLenMul  = 1.f;  // staccato: fraction of the slice a call plays
    /* SAMPLE: a voice's read limit. A grid slice may fade past its own end
     * but never past the material (the recordings begin there — a choke fade
     * at the last slice would otherwise play 5 ms of the first recording's
     * attack); a recorded entry is bounded by its own end. */
    int ReadEnd(int idx) const
    {
        if(idx < nSlices) return bufLen;
        const int e = slices[idx].start + slices[idx].len;   /* clamped: a torn Slice read must stay in the plane (review L4) */
        return e > bufCap ? bufCap : (e < 0 ? 0 : e);
    }
    void FireSlice(int idx, float rate, bool rev, float lenMul = 1.f, bool held = false, int key = -1)
    {
        Slice cell = slices[idx];
        if(cell.len < 64) return;
        callHits = (uint8_t)(callHits + 1u); lastCallIdx = idx;
        RaiseGates(idx, false); /* GATES: a call is never accented */
        const bool ep = JoinIsCross(cell.start); /* PLAY */
        Voice*   v    = &voices[0];
        uint32_t best = 0;
        for(int i = 0; i < kVoices; i++)
        {
            if(!voices[i].active) { v = &voices[i]; break; }
            if(voices[i].age >= best) { best = voices[i].age; v = &voices[i]; }
        }
        for(int i = 0; i < kVoices; i++)
            if(&voices[i] != v) voices[i].Choke(ep);
        const int bound = ReadEnd(idx);
        v->Trigger(cell, rate, rev, bound, sliceExt);
        ApplyFades(*v, idx, ep); /* PLAY: calls play honestly — no tail loop */
        if(held)   /* SAMPLE: the sampler rule — read on to the end, until the key comes up */
        {
            v->held = true;
            v->key  = (int8_t)key;
            if(!rev) { v->natEnd = bound; v->end = bound; }
            else v->start = idx >= nSlices ? cell.start : 0;   /* backwards to the material's start */
        }
        else if(lenMul < 0.999f && !rev)   /* staccato style: play a fraction, then the edge guard */
        {
            int nl = (int)((float)cell.len * lenMul); if(nl < 64) nl = 64;
            v->natEnd = cell.start + nl; v->end = v->natEnd > bound ? bound : v->natEnd;
        }
        trigFlash++;
    }

    // AUDIO thread (the shell defers a poll-thread call through gCallReq).
    // SAMPLE: `held` = the sampler rule (see FireSlice), `key` tags the voice
    // for ReleaseKey; both default to the one-shot call, which is unchanged.
    void CallSlice(int idx, float rateMul, bool rev, bool punch, bool ratchet, float lenMul = 1.f,
                   bool held = false, int key = -1)
    {
        /* LOOPER: in Play with the looper on, `punch` means "write" — into the
         * loop, at this phase, instead of into a step. The looper IS the
         * memory: no step is punched. A full list refuses; the call still
         * sounds. Quantize = 16ths leaves this whole branch dead. */
        if(LooperActive())
        {
            if(punch && buf && nSlices >= 1)
            {
                int t = idx < 0 ? 0 : (idx >= nTable ? nTable - 1 : idx);
                looper.quant = looperQuant;
                looper.Record(barPhase, key, t, LooperStyle(rateMul, rev, ratchet, lenMul, held), 1.f);
            }
            punch = false;
        }
        CallSliceRaw(idx, rateMul, rev, punch, ratchet, lenMul, held, key);
    }
    // The call itself, never recorded: the looper's replay path.
    void CallSliceRaw(int idx, float rateMul, bool rev, bool punch, bool ratchet, float lenMul,
                      bool held, int key)
    {
        callLenMul = lenMul;
        if(!buf || nSlices < 1) return;
        if(idx < 0) idx = 0;
        if(idx >= nTable) idx = nTable - 1;
        float gridStepSamp = barSeconds * sr / (float)nSlices;
        float rate         = pitch * (nativeStep / gridStepSamp) * rateMul;
        FireSlice(idx, rate, rev, lenMul, held, key);
        if(punch) PunchStep(curStep, idx, rev);
        if(ratchet)
        {
            callCountdown = (int)(gridStepSamp * 0.5f);
            callIdx = idx; callRate = rate; callRev = rev; callHeld = held; callKey = key;
        }
        else callCountdown = -1;
    }

    /* ── SAMPLE: key table, held release, recording ──────────────────────
     * The full contract is docs/packages/sample-engine.md. */

    // CONTROL or AUDIO (reads one byte). Which table entry Voice key `key`
    // (0..kKeys-1, clamped) plays; always a valid index while nTable > 0.
    int KeySlice(int key) const
    {
        if(key < 0) key = 0;
        if(key >= kKeys) key = kKeys - 1;
        int s = keySlice[key];
        if(s < 0) s = 0;
        if(s >= nTable) s = nTable > 0 ? nTable - 1 : 0;
        return s;
    }
    // CONTROL or AUDIO (one byte). Point a key at any table entry (a recorded
    // one or a grid slice). false = out of range, or a tombstone. New material
    // resets the table; a re-apply of the same planes keeps it (EDIT).
    // The audio-thread path is ApplyEdit(SetKey); this direct call is safe
    // from the control thread because it writes one int8 the audio thread
    // reads whole (KeySlice clamps whatever it finds).
    bool SetKeySlice(int key, int slice)
    {
        if(key < 0 || key >= kKeys || slice < 0 || slice >= nTable || (tableFlags[slice] & kFlagTomb)) return false;
        keySlice[key] = (int8_t)slice;
        BumpGen();
        return true;
    }
    // AUDIO. The key came up: every held voice tagged with it fades out
    // with the normal choke geometry (5 ms fixed, or the slice's adaptive
    // release). One-shot voices are untouched; an unknown key is a no-op.
    void ReleaseKey(int key)
    {
        if(LooperActive()) looper.RecordRelease(barPhase, key);   /* LOOPER: a held note keeps its length */
        ReleaseKeyRaw(key);
    }
    void ReleaseKeyRaw(int key)
    {
        for(int i = 0; i < kVoices; i++)
            if(voices[i].active && voices[i].held && voices[i].key == key) voices[i].Release();
    }
    /* LOOPER: fire one recorded event through the live call path. A keyed
     * event resolves its slice through the key table NOW (re-record a key,
     * re-point it, re-slice the bar: the loop follows); a keyless one plays
     * the table entry it was called with. Replays never record. */
    void LooperFire(const Looper::Event& e)
    {
        if(e.style & Looper::kRelease) { ReleaseKeyRaw(e.key); return; }
        const int   idx  = e.key >= 0 ? KeySlice(e.key) : e.slice;
        const float rate = (e.style & Looper::kHalf) ? 0.5f : ((e.style & Looper::kDouble) ? 2.f : 1.f);
        CallSliceRaw(idx, rate, (e.style & Looper::kRev) != 0, false, (e.style & Looper::kRatchet) != 0,
                     (e.style & Looper::kStaccato) ? 0.5f : 1.f, (e.style & Looper::kHeld) != 0, e.key);
    }

    /* ── EDIT-AUDITION (docs/packages/slice-edit-proto.md): the scratch voice ──
     * AUDIO. A one-shot with explicit bounds — the page's "play this region"
     * while a handle is dragged. [start, start + len) at `rateMul` × the
     * file's rate (no tempo varispeed: an audition is about the boundaries,
     * not the groove), the fixed 1 ms attack and a 5 ms fade closing the
     * region (its natural end sits chokeLen short of the region's end, so
     * the release ramp runs inside it — a grid one-shot ends on the click
     * bridge instead), clamped into [0, freePos) and refused under kGridMin.
     * flags b0 (kRegionFromEnd): `start` is the region's END — play
     * [start − len, start), the last len samples up to it. b1
     * (kRegionLoop): repeat the region until ReleaseRegion (the tail loop
     * mechanism: loopA = the start, loopB two samples short of the end, the
     * click bridge covers the seam). One scratch voice sounds at a time: a
     * new call fades the previous one with the normal choke geometry (still
     * tagged, so nothing adopts it while it fades) and starts a fresh
     * voice. Never chokes the sequencer's voices, never counted as a call
     * (no gate, no hit trigger, no trigFlash), never recorded. */
    static constexpr uint8_t kRegionFromEnd = 1u;
    static constexpr uint8_t kRegionLoop    = 2u;
    bool CallRegion(int start, int len, float rateMul, uint8_t flags)
    {
        if(!buf || freePos < kGridMin || !(rateMul > 0.f)) return false;
        if(flags & kRegionFromEnd) start -= len;
        if(start < 0) { len += start; start = 0; }
        if(start > freePos) return false;
        if(len > freePos - start) len = freePos - start;
        if(len < kGridMin) return false;
        for(int i = 0; i < kVoices; i++)
            if(voices[i].active && voices[i].scratch) ScratchFade(voices[i]);
        Voice*   v    = &voices[0];
        uint32_t best = 0;
        for(int i = 0; i < kVoices; i++)
        {
            if(!voices[i].active) { v = &voices[i]; break; }
            if(!voices[i].scratch && voices[i].age >= best) { best = voices[i].age; v = &voices[i]; }
        }
        const Slice cell = {start, len};
        v->Trigger(cell, rateMul, false, start + len, 1.f);   /* bound = end = the region's end */
        v->scratch = true;
        if(flags & kRegionLoop)
        {
            v->tail  = 1;
            v->loopA = (double)start;
            v->loopB = (double)(start + len - 2);
        }
        else
        {
            const int fade = len >= v->chokeLen + kGridMin ? v->chokeLen : len / 2;
            v->natEnd = start + len - fade;   /* the release ramp runs over the region's last 5 ms */
        }
        return true;
    }
    // AUDIO. Every scratch voice fades out from here (5 ms), whatever its position.
    void ReleaseRegion()
    {
        for(int i = 0; i < kVoices; i++)
            if(voices[i].active && voices[i].scratch) ScratchFade(voices[i]);
    }
    static void ScratchFade(Voice& v)
    {
        if(v.tail) { v.Fade(v.epShape); return; }   /* a looping audition: the tail branch fades forward from pos */
        if(v.pos >= (double)v.natEnd) return;        /* already in its edge guard */
        v.Fade(v.epShape);
    }
    /* AUDIO (the shell snapshots it once per block for the state tail's
     * srcPos). The material position of the youngest sounding voice that is
     * not an audition, or — with nothing sounding — the start of what the
     * current step plays (its source slice, else the grid slice). */
    int SrcPos() const
    {
        int      p    = -1;
        uint32_t best = 0xFFFFFFFFu;
        for(int i = 0; i < kVoices; i++)
        {
            const Voice& v = voices[i];
            if(!v.active || v.scratch || v.age >= best) continue;
            best = v.age;
            p    = (int)v.pos;
        }
        if(p >= 0) return p;
        if(nSlices < 1) return 0;
        const int k = curStep < 0 ? 0 : (curStep >= nSlices ? nSlices - 1 : curStep);
        const int s = stepSrc[k] >= 0 && stepSrc[k] < nTable ? stepSrc[k] : k;
        return slices[s].start;
    }

    // Recorded entry owned by `key`, or -1.
    int RecordedEntry(int key) const
    {
        for(int t = nSlices; t < nTable; t++)
            if(tableKey[t] == key) return t;
        return -1;
    }
    // CONTROL. Arm a recording into `key`. Fails (false) when a take is
    // armed or pending, when fewer than kRecMin samples remain before
    // bufCap, or when the table is full and the key has no entry to
    // replace. The audio thread starts appending at freePos on the next
    // RecFeed after this returns.
    bool RecArm(int key)
    {
        if(!buf || key < 0 || key >= kKeys || recArmed || recPending) return false;
        if(bufCap - freePos < kRecMin) return false;
        if(RecordedEntry(key) < 0 && FreeSlot() < 0) return false;   /* EDIT: a tombstone's slot counts as free */
        recKey   = key;
        recStart = freePos;
        recPos   = freePos;
        recFull  = false;
        __asm__ volatile("" ::: "memory"); recArmed = true;   // last: publishes the take to the audio thread (compiler fence, review L3)
        return true;
    }
    // AUDIO. Append n input samples at the write head while armed. `inR`
    // feeds the R plane (nullptr: the L input is copied to both planes);
    // ignored on mono material. O(n), no per-sample branch; stops itself
    // (recFull) when the plane is full.
    void RecFeed(const float* in, const float* inR, int n)
    {
        if(!recArmed) return;
        const int p    = recPos;
        const int room = bufCap - p;
        if(n > room) n = room;
        float* d = buf + p;
        for(int i = 0; i < n; i++) d[i] = in[i];
        if(bufR)
        {
            const float* s  = inR ? inR : in;
            float*       dr = bufR + p;
            for(int i = 0; i < n; i++) dr[i] = s[i];
        }
        recPos = p + n;
        if(p + n >= bufCap) { recFull = true; recArmed = false; }
    }
    void RecFeed(const float* in, int n) { RecFeed(in, nullptr, n); }
    // CONTROL. Stop the take (the audio thread appends nothing after this
    // returns) and hand its raw region back: [start, start + len) in the
    // planes, len = the return value (0 = nothing taken). The region then
    // belongs to the control thread — trim / normalise / analyse it in place
    // (bb_sample.h) and CommitSlice it, or RecDiscard it; RecArm refuses a
    // new take until one of the two.
    int RecStop(int& start, int& len)
    {
        recArmed = false;
        start    = recStart;
        len      = recKey >= 0 ? recPos - recStart : 0;
        if(len < 0) len = 0;
        recPending = len > 0;
        if(!recPending) recKey = -1;
        return len;
    }
    // CONTROL. Drop a stopped take; its memory is free again.
    void RecDiscard()
    {
        recArmed   = false;
        recPending = false;
        recKey     = -1;
    }
    // CONTROL. Publish [start, start + len) as key `key`'s slice: a new
    // table entry (the key's existing recorded entry is replaced instead —
    // its old memory is orphaned until the set is saved and reloaded),
    // metadata as given (see bb_sample.h's AnalyseRegion), keySlice[key]
    // → the entry, freePos advanced past the region. Returns the table
    // index, or -1 (bad range, shorter than kRecMin, table full).
    // Publication order matters for the audio thread: region, entry,
    // count, key — each step leaves a consistent table.
    int CommitSlice(int key, int start, int len, uint8_t cls, float sal, float dens, bool hit)
    {
        if(!buf || key < 0 || key >= kKeys || len < kRecMin || start < 0 || start > bufCap || len > bufCap - start) return -1;   /* overflow-safe (review M1) */
        int t = RecordedEntry(key);
        if(t < 0)
        {
            t = FreeSlot();   /* EDIT: the first tombstone's slot, else the end */
            if(t < 0) return -1;
        }
        recMeta[t] = {cls, sal, dens, hit};
        slices[t]     = {start, len};
        tableKey[t]   = (int8_t)key;
        stepClass[t]  = cls;
        sliceSal[t]   = sal;
        sliceDens[t]  = dens;
        stepHit[t]    = hit;
        stepBands[t]  = 0;
        loopState[t]  = 0;
        tableFlags[t] = 0;   /* EDIT: a recording is neither orange nor a tombstone (a reused slot comes alive here) */
        if(start + len > freePos) freePos = start + len;
        if(t == nTable) nTable = t + 1;
        keySlice[key] = (int8_t)t;
        recPending    = false;
        recKey        = -1;
        BumpGen();
        return t;
    }
    // CONTROL or AUDIO. Re-assert the recorded entries' metadata after the
    // shell has copied a whole per-slice array (stepClass, sliceSal,
    // sliceDens, stepBands) from a fresh classification of the grid — those
    // copies are kMaxSlices wide and would otherwise overwrite entries
    // [nSlices, kMaxSlices). Idempotent.
    void SyncRecordedMeta()
    {
        for(int t = nSlices; t < nTable; t++)
        {
            stepClass[t] = recMeta[t].cls;
            sliceSal[t]  = recMeta[t].sal;
            sliceDens[t] = recMeta[t].dens;
            stepHit[t]   = recMeta[t].hit;
            stepBands[t] = 0;
        }
    }
    // The pre-SAMPLE Voice map: keys span the grid (k · nSlices / span,
    // span = min(nSlices, 24); keys past the span play the last slice), then
    // every recorded entry claims its key.
    static constexpr int kLooperFiresPerBlock = 48;
    /* Style 8 / browser Clear: the key goes back to its span slice AND its
     * recorded entry forgets the key, so a re-apply (DefaultKeyTable) does
     * not re-claim it (review L2). The region stays until the next load. */
    void ClearKey(int key)
    {
        if(key < 0 || key >= kKeys) return;
        for(int t = nSlices; t < nTable; t++) if(tableKey[t] == key) tableKey[t] = -1;
        keySlice[key] = (int8_t)SpanSlice(key);
        BumpGen();
    }
    // The span mapping: key k → grid slice k · nSlices / span, span =
    // min(nSlices, 24); keys past the span play the last slice.
    int SpanSlice(int key) const
    {
        const int span = nSlices < kKeys ? nSlices : kKeys;
        const int kk   = key >= span ? span - 1 : key;
        return span > 0 ? (kk * nSlices) / span : 0;
    }
    void DefaultKeyTable()
    {
        for(int k = 0; k < kKeys; k++) keySlice[k] = (int8_t)SpanSlice(k);
        for(int t = nSlices; t < nTable; t++)
            if(tableKey[t] >= 0 && tableKey[t] < kKeys) keySlice[tableKey[t]] = (int8_t)t;
    }
    /* EDIT: the same planes re-applied — every key follows its entry. */
    void RestoreKeyTable(const int8_t* oldKeys, const int8_t* map, int oldN, int oldTable)
    {
        for(int k = 0; k < kKeys; k++)
        {
            const int o = oldKeys[k];
            if(o >= oldN && o < oldTable && map[o] >= 0) keySlice[k] = map[o];
            else if(o >= 0 && o < oldN && nSlices == oldN) keySlice[k] = (int8_t)o;
            else keySlice[k] = (int8_t)SpanSlice(k);
        }
    }
    void MoveEntry(int from, int to)
    {
        if(from == to) return;
        slices[to]     = slices[from];
        tableKey[to]   = tableKey[from];
        recMeta[to]    = recMeta[from];
        tableFlags[to] = tableFlags[from];   /* EDIT */
    }

    int8_t  keySlice[kKeys]     = {};   // table entry each Voice key plays
    int8_t  tableKey[kMaxTable] = {};   // owner key of a recorded entry (grid entries: unused)
    using RecMeta = EntryMeta;
    RecMeta recMeta[kMaxTable]  = {};   // shadow metadata of the recorded entries
    volatile bool recArmed   = false;   // control arms, audio appends, either clears
    volatile bool recFull    = false;   // the take hit bufCap (audio sets)
    bool          recPending = false;   // stopped, awaiting CommitSlice / RecDiscard
    int           recKey     = -1;      // key of the armed / pending take
    int           recStart   = 0;
    volatile int  recPos     = 0;       // write head (audio advances)
    /* ── SAMPLE end ────────────────────────────────────────────────────── */

    /* ── EDIT: slice editing (docs/packages/slice-edit-core.md) ──────────
     * The bodies live in core/bb_edit.h (included at the foot of this file).
     * THREAD RULE: ApplyEdit, SetGridBoundaries and SetKeyTable write
     * slices[] and run on the AUDIO thread at block start (the shell posts
     * an EditReq through a mailbox with an ack, exactly as gCallReq); the
     * control thread only reads, and analyses a region with
     * bb::AnalyseEntry before posting. No op allocates, searches audio or
     * costs more than O(kMaxTable) (+ O(looper.n) for a delete or merge). */
    uint8_t tableGen = 1;               // bumps on every table change; never 0 (0 = "unchecked" in EditReq)
    uint8_t tableFlags[kMaxTable] = {}; // kFlagOrange · kFlagTomb, per entry
    int     onsetTab[kMaxOnsets]  = {}; // the material's onset table (SetMaterial keeps a copy)
    int     nOnsetTab   = 0;
    bool    onsetsGiven = false;        // false: no onset table → every slice is a hit
    /* the boundary overlay: user-moved grid starts, re-applied by SetMaterial
     * on the same planes when the material length and slice count hold */
    int      ovStart[kMaxSlices] = {};
    uint64_t ovOrange = 0;              // bit i = boundary i was moved by the user
    int      ovN = 0, ovLen = 0;

    EditStatus ApplyEdit(const EditReq& r, int* newIdx = nullptr);   // AUDIO
    bool       SetGridBoundaries(const int* starts, int n);            // AUDIO (or before audio runs)
    bool       SetKeyTable(const int8_t* keys);                        // AUDIO (or before audio runs)
    uint8_t    TableFlags(int t) const { return t >= 0 && t < nTable ? tableFlags[t] : 0; }
    bool       IsTomb(int t) const { return t >= 0 && t < kMaxTable && (tableFlags[t] & kFlagTomb) != 0; }
    bool       OnsetHit(int start, int len) const;                     // the per-entry form of SetMaterial's hit rule
    int        FreeSlot() const;                                       // the first tombstone ≥ nSlices, else nTable, else −1
    void       BumpGen() { tableGen = (uint8_t)(tableGen + 1u); if(tableGen == 0) tableGen = 1; }
    void       RefreshEntry(int t);                                    // loopState, stepHit, recMeta mirror, gas well
    void       SaveOverlay();
    void       ApplyOverlay(bool samePlanes);
    // op bodies (all return without touching anything on refusal)
    EditStatus EdMoveBoundary(int i, int pos);
    EditStatus EdSetEntry(int t, int start, int len);
    EditStatus EdAdd(const EditReq& r, int& idx);
    EditStatus EdSplit(const EditReq& r, int& idx);
    EditStatus EdMerge(int t);
    EditStatus EdTomb(int t);
    EditStatus EdSetKey(int key, int t);
    EditStatus EdSwapKeys(int a, int b);
    EditStatus EdNudge(int delta, int which);
    /* ── EDIT end ──────────────────────────────────────────────────────── */

    // Mono render (the pre-SAMPLE signature): the L plane into `out`.
    void Process(float* out, int n) { ProcessT<false>(out, nullptr, n); }
    // SAMPLE: two planes. Mono material renders once and copies L to R.
    void Process(float* outL, float* outR, int n)
    {
        if(bufR) ProcessT<true>(outL, outR, n);
        else
        {
            ProcessT<false>(outL, nullptr, n);
            for(int i = 0; i < n; i++) outR[i] = outL[i];
        }
    }

    template <bool ST>
    void ProcessT(float* out, float* outR, int n)
    {
        for(int i = 0; i < n; i++) out[i] = 0.f;
        if constexpr(ST) for(int i = 0; i < n; i++) outR[i] = 0.f;
        if(!buf || bufLen < 2 || nSlices < 1) return;

        float blockSec = (float)n / sr;

        // Voice: service a pending ratcheted-call double (32nd later)
        if(callCountdown >= 0)
        {
            callCountdown -= n;
            if(callCountdown < 0 && callIdx >= 0) FireSlice(callIdx, callRate, callRev, callLenMul, callHeld, callKey);
        }

        // Quantized cell sequencing: the grid clock runs at OUR tempo; the
        // pattern (a Brocot tiling of cells) says which source cell each grid
        // position plays. Everything lands on step boundaries.
        if(!extPhase)
        {
            double dp = (double)blockSec / (double)barSeconds;
            barPhase += dp;
            barPhase -= floor(barPhase);
        }
        // (extPhase: the shell has written barPhase from the musical clock)
        int step;
        if(swung)   /* SLICE: the odd step of each 8th fires at the swung offset */
        {
            const double cells = barPhase * (double)(nSlices / 2);
            const int    cell  = (int)cells;
            step = 2 * cell + ((cells - (double)cell) >= (double)swing ? 1 : 0);
        }
        else step = (int)(barPhase * (double)nSlices);
        if(step >= nSlices) step = nSlices - 1;
        if(step != curStep)
        {
            bool wrapped = step < curStep;
            curStep          = step;
            ratchetCountdown = -1; // a new step supersedes any pending double
            if(wrapped)
            {
                if(flowRegen) RerollFill();
                else if(patternMode == 1 && pat.fillStart >= 0
                        && pat.fillStart < nSlices)
                {
                    // Collins fill hyperperiod: every 4th loop plays the fill
                    // layer's last bar; the next downbeat snaps back verbatim.
                    // Only the fill REGION is re-flattened (cells are half-bar
                    // aligned, fillStart is bar aligned) so Voice/web punches
                    // in the rest of the loop survive the toggle.
                    loopCount++;
                    bool want = (loopCount & 3) == 3;
                    if(want != fillActive)
                    {
                        fillActive = want;
                        if(want)
                            FlattenRange(pat.fillStart, nSlices, pat.fsrc, pat.flen,
                                         pat.frev, pat.fratN, pat.fratAccel, pat.frep);
                        else
                            FlattenRange(pat.fillStart, nSlices, pat.src, pat.len,
                                         pat.rev, pat.ratN, pat.ratAccel, pat.rep, pat.accent);
                    }
                }
            }
            if(!muted && stepSrc[step] >= 0)   /* Voice: mute silences pattern steps only */
            {
                TriggerStep(step);
                if(stepRatN[step] > 0)
                {
                    float stepS  = barSeconds * sr / (float)nSlices;
                    ratchetLeft  = stepRatN[step];
                    ratchetAccel = stepRatAccel[step];
                    // single double: at +½ step (Classic, unchanged). Series:
                    // accel gaps ½,¼,⅛ of a step; decel starts small so the
                    // whole series still fits inside the step (⅛,¼,½).
                    ratchetIval = (ratchetAccel || ratchetLeft == 1)
                                      ? (int)(stepS * 0.5f)
                                      : (int)(stepS / (float)(1 << ratchetLeft));
                    if(ratchetIval < 72) ratchetIval = 72; // ≥1.5 ms
                    ratchetCountdown = ratchetIval;
                    ratchetK         = step;
                }
            }
        }
        if(ratchetCountdown >= 0)
        {
            ratchetCountdown -= n;
            if(ratchetCountdown < 0 && ratchetK >= 0)
            {
                TriggerStep(ratchetK);
                if(--ratchetLeft > 0)
                {
                    ratchetIval = ratchetAccel ? (ratchetIval >> 1) : (ratchetIval << 1);
                    if(ratchetIval < 72) ratchetIval = 72;
                    ratchetCountdown = ratchetIval;
                }
                else
                    ratchetCountdown = -1;
            }
        }

        /* LOOPER: the events this block crosses fire at their own sample. The
         * block renders in segments split at each event's offset; a voice
         * sounding at the block's start (or fired inside it) renders to the
         * block's end, exactly as the single loop below used to — with no
         * events the one segment [0, n) IS that loop, op for op. Advance runs
         * every block (the wrap closes a generation) whatever the grammar. */
        Looper::Window lw;
        looper.Advance(barPhase, (double)blockSec / (double)barSeconds, n, lw);
        bool on[kVoices];
        for(int v = 0; v < kVoices; v++) on[v] = voices[v].active;
        int seg = 0;
        int fires = 0;   /* per-block cap: with 8ths quantise 256 events can share one block (review M4) */
        if(looper.n > 0 && LooperActive())
            for(int pass = 0; pass < 3; pass++)
            {
                int lo, hi; double sh;
                if(!lw.Range(pass, lo, hi, sh)) continue;
                for(int k = lo; k < hi; k++)
                {
                    Looper::Event& e = looper.ev[k];
                    if(e.style & Looper::kSkip) { e.style &= (uint8_t)~Looper::kSkip; continue; }
                    if(++fires > kLooperFiresPerBlock) continue;   /* the rest of a pile-up is dropped, not deferred */
                    const int off = Looper::Offset(lw, e.phase, sh, n);
                    if(off > seg) { RenderSpan<ST>(out, outR, on, seg, off); seg = off; }
                    LooperFire(e);
                    for(int v = 0; v < kVoices; v++) on[v] = on[v] || voices[v].active;
                }
            }
        RenderSpan<ST>(out, outR, on, seg, n);
        /* LOOPER end */
        for(int i = 0; i < n; i++)
        {
            float x = out[i] * 0.8f;
            if(x > 1.f) x = 1.f;
            else if(x < -1.f) x = -1.f;
            out[i] = x;
        }
        if constexpr(ST)
            for(int i = 0; i < n; i++)
            {
                float x = outR[i] * 0.8f;
                if(x > 1.f) x = 1.f;
                else if(x < -1.f) x = -1.f;
                outR[i] = x;
            }
        timeSec += (double)blockSec;
    }

  private:
    /* LOOPER: render samples [a, b) of every voice marked `on` into the block */
    template <bool ST>
    void RenderSpan(float* out, float* outR, const bool* on, int a, int b)
    {
        for(int v = 0; v < kVoices; v++)
        {
            if(!on[v]) continue;
            for(int i = a; i < b; i++)
            {
                float l = 0.f, r = 0.f;
                voices[v].RenderT<ST>(buf, bufR, l, r);
                out[i] += l;
                if constexpr(ST) outR[i] += r;
            }
        }
    }

    // Fire one grid step: a single natural slice at pitch-control rate.
    // ReCycle paradigm at true slice granularity — tempo (pot 2) re-spaces
    // every hit at unchanged pitch; pitch (encoder) retunes every hit at
    // unchanged onset times; each step's attack re-anchors the grid.
    void TriggerStep(int k)
    {
        int s = stepSrc[k];
        if(s < 0 || (tableFlags[s] & kFlagTomb)) return;   /* EDIT: a deleted marker punched into a step: no gate, no voice */
        /* a continuation is not a hit: no gate; in onsets-only mode neither
         * is a slice that does not open with a drum */
        /* a map hit (Grids/Euclid) IS a hit by definition — it gates in
         * Onsets-only mode whatever the chosen slice looks like */
        const bool langHit = patternMode == 2 || patternMode == 3;
        if(!stepCont[k] && (!gateOnsetsOnly || langHit || stepHit[s]))
            RaiseGates(s, stepAccent[k]); /* GATES */
        // VARISPEED: rate follows tempo like a record — each slot consumes
        // exactly one slice of source (slotSamples × rate == nativeStep), so
        // the continuity branch holds at EVERY tempo: verbatim runs stay
        // literally continuous, just repitched. No gaps, chokes, or doubles
        // from tempo alone. (Only safe with per-step triggers: cells drifted
        // internally when v4.0 tried this — that bug class is dead.)
        float gridStepSamp = barSeconds * sr / (float)nSlices;
        float rate         = pitch * (nativeStep / gridStepSamp);
        if(stepJit[k]) rate *= 1.f + 0.001f * (float)stepJit[k]; // Collins ±1.5%
        Slice cell         = slices[s];
        if(cell.len < 64) return;

        // LOOKAHEAD: the pattern is known, so we know whether the NEXT step
        // continues this run (its source is the next slice, same direction,
        // no roll). If so, hold this voice past its natural end by a grace
        // window: the step that fires on time (or a block late) then finds it
        // still sounding and extends it, and the run stays one unbroken voice.
        // Without this the voice ended one sample before the boundary and every
        // 16th of a verbatim run re-spliced with a 5 ms notch (host suite). The
        // grace must clear the 5 ms edge fade plus the continuity window.
        const int kn = (k + 1 < nSlices) ? k + 1 : 0;
        int       sn = s + 1; if(sn >= nSlices) sn -= nSlices;
        const bool willCont = !stepRev[k] && !reverse && stepRatN[k] == 0
                              && stepSrc[kn] == sn && !stepRev[kn];

        /* PLAY: does the grid slot OUTLAST the slice? The slot consumes
         * gridStepSamp × rate source samples (varispeed makes that exactly
         * one slice at any tempo, so this is pitch-up, or a Voice pitch
         * multiplier) — with more than the continuity window to spare, the
         * voice would gate before the next hit. The tail policy then loops
         * it; such a voice is not extendable, so no lookahead grace. */
        /* consumption of the slot in SOURCE samples: pitch × the slot's own
         * slice length (under swing the odd slots are shorter — review M2) */
        const float slotSrc = swung ? (float)slices[k].len : nativeStep;
        const bool outlast = (tailMode == 1 || tailMode == 2) && !stepRev[k] && !reverse
                             && pitch * slotSrc - (float)cell.len > 160.f;
        /* ZOOM (tailMode 3, BreakFix's rule, the default): the slot consumes
         * slot × pitch source samples whatever the slice's own length —
         * pitched up it reads on into the next slice and re-anchors at the
         * next step (the anticipated arrival), pitched down it is cut at the
         * slot's end. No gaps, no gating: pitch is a zoom on the material.
         * ("Gate honestly" was the transplant's choice; the field wanted the
         * record's behaviour back — 2026-09-07.) Reverse keeps the slice. */
        const bool zoom = tailMode == 3 && !stepRev[k] && !reverse;
        Slice zc = cell;
        bool  zoomLoop = false;
        const int bound = ReadEnd(s);   /* SAMPLE: the material's end, or a recording's own */
        if(zoom)
        {
            int zl = (int)(pitch * slotSrc + 0.5f);
            if(zl < 64) zl = 64;
            if(cell.start + zl > bound) zl = bound - cell.start;
            if(zl > cell.len + 160)
            {
                /* Never read THROUGH an attack: it would play here, pitched, and
                 * again at its own step (the "duplicating" at high pitch). If the
                 * next slice opens with a drum, loop this slice's tail for the
                 * rest of the slot; otherwise read on through the following
                 * attack-less material and stop at the first drum. */
                if(s + 1 < nSlices ? stepHit[s + 1] : true) { zl = cell.len; zoomLoop = true; }
                else
                {
                    int limit = cell.len, n = s + 1;
                    while(n < nSlices && !stepHit[n] && limit < zl) { limit += slices[n].len; n++; }
                    if(limit < zl) zl = limit;
                }
            }
            zc.len = zl;
        }
        const int grace = (willCont && !outlast) ? 400 : 0;

        // CONTINUITY: if a voice is already playing exactly into this slice
        // (a verbatim run at matching rate), extend it instead of re-splicing
        // — unbroken runs stay literally continuous audio. Splices happen
        // only at true discontinuities, where they are the music.
        if(!stepRev[k] && !reverse)
            for(int i = 0; i < kVoices; i++)
            {
                Voice& vv = voices[i];
                // rate tolerance, not equality: tiny tempo adjustments must
                // not break continuity (adopting the new rate is a sub-cent
                // pitch step; position re-anchors at the next boundary)
                if(vv.active && vv.relScale > 0.99f && vv.tail == 0 /* PLAY */
                   && !vv.held /* SAMPLE: a held voice belongs to its key */
                   && !vv.scratch /* EDIT-AUDITION: an audition is not a run */
                   && fabsf(vv.rate - rate) < 0.02f * rate
                   && fabs(vv.pos - (double)cell.start) < 160.)
                {
                    vv.rate   = rate;
                    vv.natEnd = zc.start + zc.len + grace;
                    int e     = zc.start + (int)((float)zc.len * sliceExt) + grace;
                    vv.end    = e > bound ? bound : e;
                    if(vv.natEnd > vv.end) vv.natEnd = vv.end;
                    return;
                }
            }

        const bool ep = JoinIsCross(cell.start); /* PLAY: uncorrelated join? */
        Voice*   v    = &voices[0];
        uint32_t best = 0;
        for(int i = 0; i < kVoices; i++)
        {
            if(!voices[i].active)
            {
                v = &voices[i];
                break;
            }
            if(voices[i].age >= best)
            {
                best = voices[i].age;
                v    = &voices[i];
            }
        }
        for(int i = 0; i < kVoices; i++)
            if(&voices[i] != v) voices[i].Choke(ep);
        v->Trigger(zc, rate, stepRev[k] || reverse, bound, sliceExt);
        ApplyFades(*v, s, ep);           /* PLAY */
        if(outlast) SetTail(*v, s);      /* PLAY */
        if(zoomLoop)                     /* zoom, but the next slice is a drum: loop this tail */
        {
            if(loopState[s] == 0) FindLoop(s);
            if(loopState[s] > 0) { v->tail = 1; v->loopA = loopA[s]; v->loopB = loopB[s]; }
        }
        if(grace)
        {
            v->natEnd += grace;
            int e  = v->end + grace;
            v->end = e > bound ? bound : e;
            if(v->natEnd > v->end) v->natEnd = v->end;
        }
        trigFlash++;
        if(trigLog && trigLogN < trigLogMax) trigLog[trigLogN++] = timeSec;
    }
};

} // namespace bb

#include "bb_edit.h"   /* EDIT: the ApplyEdit / overlay / key-table bodies */
