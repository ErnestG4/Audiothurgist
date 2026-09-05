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
    }

    // A new attack chokes any voice still ringing in its extension: the tail
    // fills silence, but must never replay the next slice's transient under
    // the incoming one (that reads as every hit doubled a few ms apart).
    void Choke(int bufLen)
    {
        // also catch voices within 10 ms of their boundary: a jitter-early
        // trigger must not leave the old voice free to replay the new attack
        if(!active || rate < 0.f || pos <= (double)(natEnd - 480)) return;
        float h = (end > natEnd + 8)
                      ? 1.f - (float)(pos - (double)natEnd) / (float)(end - natEnd)
                      : 1.f;
        if(h < 0.f) h = 0.f;
        if(h > 1.f) h = 1.f;
        relScale *= h; // new 5 ms ramp starts exactly where the old gain was
        natEnd = (int)pos;
        int e  = (int)pos + 240;
        end    = e > bufLen ? bufLen : e;
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
        int   i0 = (int)pos;
        float fr = (float)(pos - (double)i0);
        float s  = buf[i0] + fr * (buf[i0 + 1] - buf[i0]);
        // envelope: ~1 ms attack; unity through the natural slice; release
        // across the extension (or a 5 ms edge guard when there is none)
        float dA = (float)(pos - (double)start);
        float g  = dA * (1.f / 48.f);
        if(g > 1.f) g = 1.f;
        float h;
        if(pos <= (double)natEnd)
            h = 1.f;
        else if(end > natEnd + 8)
            h = 1.f - (float)(pos - (double)natEnd) / (float)(end - natEnd);
        else
            h = (float)((double)(end - 1) - pos) * (1.f / 240.f);
        if(h > 1.f) h = 1.f;
        if(h < 0.f) h = 0.f;
        pos += (double)rate;
        age++;
        lastOut = s * g * h * relScale;
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

    double  barPhase   = 0.; // playhead sweep of the ring, [0,1)
    bool    extPhase   = false; // true: barPhase is set by the shell (clock
                               // sync); false: engine self-advances (internal)
    uint8_t gateFired  = 0;    // per-trigger class bitmask (1<<class); the
                               // shell reads+clears it to raise CV gates
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

    Voice    voices[kVoices];
    uint32_t trigFlash = 0; // incremented per trigger; shell reads + clears for LEDs

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
    bool   stepRat[kMaxSlices];
    int    ratchetK = -1; // step whose 32nd double is pending

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

    void ApplyPattern(float knob, uint64_t cs, uint64_t fs)
    {
        pat.Generate(nSlices, stepsPerBeat, stepClass, knob, cs, fs);
        int c = 0;
        while(c < nSlices)
        {
            int L = pat.len[c] > 0 ? pat.len[c] : 1;
            if(c + L > nSlices) L = nSlices - c;
            for(int j = 0; j < L; j++)
            {
                if(pat.src[c] >= 0)
                {
                    int s = pat.src[c] + (pat.rev[c] ? (L - 1 - j) : j);
                    if(s >= nSlices) s -= nSlices; // wrap, don't stutter the tail
                    stepSrc[c + j] = (int8_t)s;
                    stepRev[c + j] = pat.rev[c];
                    stepRat[c + j] = pat.rat[c];
                }
                else
                {
                    stepSrc[c + j] = -1; // rest or unfilled: silence
                    stepRev[c + j] = false;
                    stepRat[c + j] = false;
                }
            }
            c += L;
        }
    }
    void RerollFill() // button 1: same chop, new arrangement
    {
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
    void SetMaterial(const float* b, int len, float sampleRate, const int* onsets,
                     int nOnsets, const uint8_t* classes = nullptr)
    {
        (void)onsets;
        (void)nOnsets;
        buf    = b;
        bufLen = len;
        sr     = sampleRate;

        float dur  = (float)len / sampleRate;
        int   bars = 1;
        while(dur / (float)bars >= 3.2f && bars < 16) bars <<= 1;
        nSlices = bars * 16;
        while(nSlices > kMaxSlices) nSlices /= 2;
        if(nSlices < 1) nSlices = 1;
        int step = len / nSlices;
        for(int i = 0; i < nSlices; i++)
            slices[i] = {i * step, step};
        slices[nSlices - 1].len = len - slices[nSlices - 1].start;
        nativeStep = (float)step;

        for(int i = 0; i < nSlices; i++)
            stepClass[i] = classes ? classes[i] : 3;
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
    }

    void Process(float* out, int n)
    {
        for(int i = 0; i < n; i++) out[i] = 0.f;
        if(!buf || bufLen < 2 || nSlices < 1) return;

        float blockSec = (float)n / sr;

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
            if(wrapped && flowRegen) RerollFill();
            if(stepSrc[step] >= 0)
            {
                TriggerStep(step);
                if(stepRat[step])
                {
                    ratchetCountdown =
                        (int)(barSeconds * sr / (float)nSlices * 0.5f);
                    ratchetK = step;
                }
            }
        }
        if(ratchetCountdown >= 0)
        {
            ratchetCountdown -= n;
            if(ratchetCountdown < 0 && ratchetK >= 0) TriggerStep(ratchetK);
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
        gateFired |= (uint8_t)(1u << (stepClass[s] & 3));
        // VARISPEED: rate follows tempo like a record — each slot consumes
        // exactly one slice of source (slotSamples × rate == nativeStep), so
        // the continuity branch holds at EVERY tempo: verbatim runs stay
        // literally continuous, just repitched. No gaps, chokes, or doubles
        // from tempo alone. (Only safe with per-step triggers: cells drifted
        // internally when v4.0 tried this — that bug class is dead.)
        float gridStepSamp = barSeconds * sr / (float)nSlices;
        float rate         = pitch * (nativeStep / gridStepSamp);
        Slice cell         = slices[s];
        if(cell.len < 64) return;

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
                if(vv.active && vv.relScale > 0.99f
                   && fabsf(vv.rate - rate) < 0.02f * rate
                   && fabs(vv.pos - (double)cell.start) < 160.)
                {
                    vv.rate   = rate;
                    vv.natEnd = cell.start + cell.len;
                    int e     = cell.start + (int)((float)cell.len * sliceExt);
                    vv.end    = e > bufLen ? bufLen : e;
                    return;
                }
            }

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
            if(&voices[i] != v) voices[i].Choke(bufLen);
        v->Trigger(cell, rate, stepRev[k] || reverse, bufLen, sliceExt);
        trigFlash++;
        if(trigLog && trigLogN < trigLogMax) trigLog[trigLogN++] = timeSec;
    }
};

} // namespace bb
