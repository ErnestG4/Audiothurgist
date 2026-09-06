#pragma once
#include <cmath>
#include <cstdint>
#include "bb_gas.h"
#include "bb_pattern.h"

namespace bb {

// Pure engine: no libDaisy, no allocation, no printf. Mono in, mono out,
// fixed buffers. The shell owns the sample memory and the control mapping.

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
    uint32_t age      = 0;
    float  lastOut    = 0.f;
    float  clickComp  = 0.f; // decaying offset that bridges a mid-note steal
    float  relScale   = 1.f; // keeps the envelope continuous through a choke

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
        if(active) clickComp = lastOut; // steal: no waveform step, ~1ms decay
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
        return active && (tail != 0 || (rate >= 0.f && pos > (double)(natEnd - 480)));
    }

    // A new attack chokes any voice still ringing in its extension: the tail
    // fills silence, but must never replay the next slice's transient under
    // the incoming one (that reads as every hit doubled a few ms apart).
    void Choke(int bufLen, bool ep = false)
    {
        if(!WouldChoke()) return;
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

    float Render(const float* buf)
    {
        clickComp *= 0.93f;
        // direction-aware end test: a reversed voice STARTS at the top of its
        // slice — pos >= end-1 is only "finished" when travelling forward
        // (the symmetric check killed every reverse at birth once ext hit 1.0)
        bool ended = (rate < 0.f) ? (pos < (double)start)
                                  : (pos >= (double)(end - 1));
        if(ended)
        {
            if(active && lastOut != 0.f) clickComp += lastOut; // natural end
            active  = false;
            lastOut = 0.f;
            return clickComp;
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
        float s  = buf[i0] + fr * (buf[i0 + 1] - buf[i0]);
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
        return lastOut + clickComp;
    }
};

struct Engine
{
    static constexpr int kMaxSlices = 64;
    static constexpr int kVoices    = 8;

    const float* buf    = nullptr;
    int          bufLen = 0;
    float        sr     = 48000.f;

    Slice slices[kMaxSlices];
    int   nSlices = 0;
    float slicePhase[kMaxSlices]; // onset positions as ring angles (gas wells)

    BetaGas gas; // dormant in the trigger path since v3.1 — remains the
                 // Phase 0 analysis object; may return as a selection bias
    Pattern  pat;
    float    patternKnob = 0.85f; // pot 1: 1 verbatim … 0 micro-chop
    uint64_t cellSeed = 0x5EED1, fillSeed = 0x5EED2;
    Rng      seedRng;
    uint8_t  stepClass[kMaxSlices]; // per-step kick/snare/hat/other
    int      stepsPerBeat = 4;
    bool     flowRegen    = false; // hold-to-flow: new fill each bar wrap

    /* GRAM */ // Options → Swaps / Flow Déjà Vu (docs/packages/gram.md)
    float    sliceSal[kMaxSlices] = {}; // per-slice salience 0..1, copied from the
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
    uint8_t stepBands[kMaxSlices] = {}; /* GATES */ // per-slice band-presence mask
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
    float sliceDens[kMaxSlices] = {};
    double loopA[kMaxSlices] = {}, loopB[kMaxSlices] = {}; // sub-sample loop region per slice
    int8_t loopState[kMaxSlices] = {};                     // 0 unsearched, 1 found, -1 none
    static constexpr float kFadeKeyframes = 10.f; // a splice spans ~10 keyframes
    static constexpr int   kFadeMin = 48, kFadeMax = 480; // 1–10 ms (Nielsen floor,
                                                          // Driedger & Müller ceiling)

    // Release / edge-guard length for a slice: keyframe-dense (bright,
    // transient) → short, sparse (sustained) → long. Fixed mode: 5 ms.
    int FadeSamples(int s) const
    {
        if(fadeMode == 0 || s < 0 || s >= kMaxSlices) return 240;
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
    bool   stepHit[kMaxSlices] = {};  // the source slice begins with a detected
                                      // onset (a drum's "open"); from the load's
                                      // onset table. All true when none given.
    bool   gateOnsetsOnly = false;    // gates fire once per HIT, not on every
                                      // step that carries the class (a snare's
                                      // decay steps are still "snare" — that was
                                      // the rapid-fire the field report meant)
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
    }
    void ApplyPattern(float knob, uint64_t cs, uint64_t fs)
    {
        pat.langX = langX; pat.langY = langY; pat.langRot = langRot;
        for(int c = 0; c < 3; c++) pat.langDens[c] = langDens[c];
        ComputeSalTiers(); /* GRAM */
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
    void Reslice() // button 2: new chop tree
    {
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
    void SetMaterial(const float* b, int len, float sampleRate, const int* onsets,
                     int nOnsets, const uint8_t* classes = nullptr, int nSlicesHint = 0)
    {
        buf    = b;
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
        int step = len / nSlices;
        for(int i = 0; i < nSlices; i++)
            slices[i] = {i * step, step};
        slices[nSlices - 1].len = len - slices[nSlices - 1].start;
        nativeStep = (float)step;

        for(int i = 0; i < nSlices; i++)
            stepClass[i] = classes ? classes[i] : 3;
        for(int i = 0; i < kMaxSlices; i++) loopState[i] = 0; /* PLAY: new grid, re-search */

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
                if(p - slices[s].start <= slices[s].len / 4) stepHit[s] = true;
                else if(s + 1 < nSlices && slices[s + 1].start - p <= (int)(0.010f * sampleRate))
                    stepHit[s + 1] = true;
            }
        SetBpm(0.f); // every load opens at ITS OWN tempo — inheriting the
                     // previous break's BPM chipmunks the new one (field bug)
        RegenPattern();
        barPhase   = 0.;
        for(int i = 0; i < kVoices; i++) voices[i].active = false; // old slice
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
        curStep            = -1;
        ratchetCountdown   = -1;
        ratchetLeft        = 0;
        callCountdown      = -1; // a ratcheted Voice call from the OLD material
        loopCount          = 0;
        fillActive         = false;
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

    void FireSlice(int idx, float rate, bool rev)
    {
        Slice cell = slices[idx];
        if(cell.len < 64) return;
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
            if(&voices[i] != v) voices[i].Choke(bufLen, ep);
        v->Trigger(cell, rate, rev, bufLen, sliceExt);
        ApplyFades(*v, idx, ep); /* PLAY: calls play honestly — no tail loop */
        trigFlash++;
    }

    void CallSlice(int idx, float rateMul, bool rev, bool punch, bool ratchet)
    {
        if(!buf || nSlices < 1) return;
        if(idx < 0) idx = 0;
        if(idx >= nSlices) idx = nSlices - 1;
        float gridStepSamp = barSeconds * sr / (float)nSlices;
        float rate         = pitch * (nativeStep / gridStepSamp) * rateMul;
        FireSlice(idx, rate, rev);
        if(punch && curStep >= 0 && curStep < nSlices)
        {
            stepSrc[curStep] = (int8_t)idx;
            stepRev[curStep] = rev;
            stepRatN[curStep]     = 0;      /* a punched hit is plain: no roll, no jitter */
            stepRatAccel[curStep] = false;
            stepJit[curStep]      = 0;
            stepCont[curStep]     = false;
            stepAccent[curStep]   = false;
        }
        if(ratchet)
        {
            callCountdown = (int)(gridStepSamp * 0.5f);
            callIdx = idx; callRate = rate; callRev = rev;
        }
        else callCountdown = -1;
    }

    void Process(float* out, int n)
    {
        for(int i = 0; i < n; i++) out[i] = 0.f;
        if(!buf || bufLen < 2 || nSlices < 1) return;

        float blockSec = (float)n / sr;

        // Voice: service a pending ratcheted-call double (32nd later)
        if(callCountdown >= 0)
        {
            callCountdown -= n;
            if(callCountdown < 0 && callIdx >= 0) FireSlice(callIdx, callRate, callRev);
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
        int step = (int)(barPhase * (double)nSlices);
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

        for(int v = 0; v < kVoices; v++)
        {
            if(!voices[v].active) continue;
            for(int i = 0; i < n; i++) out[i] += voices[v].Render(buf);
        }
        for(int i = 0; i < n; i++)
        {
            float x = out[i] * 0.8f;
            if(x > 1.f) x = 1.f;
            else if(x < -1.f) x = -1.f;
            out[i] = x;
        }
        timeSec += (double)blockSec;
    }

  private:
    // Fire one grid step: a single natural slice at pitch-control rate.
    // ReCycle paradigm at true slice granularity — tempo (pot 2) re-spaces
    // every hit at unchanged pitch; pitch (encoder) retunes every hit at
    // unchanged onset times; each step's attack re-anchors the grid.
    void TriggerStep(int k)
    {
        int s = stepSrc[k];
        /* a continuation is not a hit: no gate; in onsets-only mode neither
         * is a slice that does not open with a drum */
        if(!stepCont[k] && (!gateOnsetsOnly || stepHit[s]))
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
        const bool outlast = tailMode != 0 && !stepRev[k] && !reverse
                             && gridStepSamp * rate - (float)cell.len > 160.f;
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
                   && fabsf(vv.rate - rate) < 0.02f * rate
                   && fabs(vv.pos - (double)cell.start) < 160.)
                {
                    vv.rate   = rate;
                    vv.natEnd = cell.start + cell.len + grace;
                    int e     = cell.start + (int)((float)cell.len * sliceExt) + grace;
                    vv.end    = e > bufLen ? bufLen : e;
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
            if(&voices[i] != v) voices[i].Choke(bufLen, ep);
        v->Trigger(cell, rate, stepRev[k] || reverse, bufLen, sliceExt);
        ApplyFades(*v, s, ep);           /* PLAY */
        if(outlast) SetTail(*v, s);      /* PLAY */
        if(grace)
        {
            v->natEnd += grace;
            int e  = v->end + grace;
            v->end = e > bufLen ? bufLen : e;
            if(v->natEnd > v->end) v->natEnd = v->end;
        }
        trigFlash++;
        if(trigLog && trigLogN < trigLogMax) trigLog[trigLogN++] = timeSec;
    }
};

} // namespace bb
