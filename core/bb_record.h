#pragma once
// RECORD: the pure half of sample mode's recording phases (B: record into a
// key, C: loop record into the break) — docs/packages/sample-record.md.
// Everything here is host-tested (tests/record_check.cpp); the shell
// (src/audiothurgist.cpp, /* RECORD */ fences) supplies the gestures, the
// LEDs and the SDRAM planes. Thread column per helper: AUDIO = the audio
// callback (O(n), no allocation, no branch per sample); CONTROL = the main
// loop / OnPoll.
#include <cmath>
#include <cstdint>
#include <cstring>
#include "bb_engine.h"
#include "bb_condition.h"
#include "bb_yield.h"

namespace bb {

/* Options → Sampling (byte 18) and Options → Record Source (byte 19). */
enum : uint8_t { kSampMono = 0, kSampStereo = 1 };
enum : uint8_t { kSrcInputs = 0, kSrcOutput = 1 };

/* ── AUDIO: the record input for one block ──────────────────────────────
 * Sampling = Mono: L = (a + b) / 2 into `mix`, R = nullptr (mono material
 * gets one plane; stereo material copies L to R — Engine::RecFeed's rule).
 * Sampling = Stereo: L = a, R = b, no copy. `a`/`b` are J1/J2 (Record
 * Source = Inputs) or the module's own post-mix output (= Output). */
struct RecPlanes { const float* L; const float* R; };
inline RecPlanes RecordInput(uint8_t sampling, const float* a, const float* b, float* mix, int n)
{
    if(sampling == kSampStereo) return {a, b};
    for(int i = 0; i < n; i++) mix[i] = 0.5f * (a[i] + b[i]);
    return {mix, nullptr};
}

/* AUDIO: block peak over one or two planes (the P6 input meter). */
inline float BlockPeak(const float* L, const float* R, int n)
{
    float pk = 0.f;
    for(int i = 0; i < n; i++) { const float a = fabsf(L[i]); if(a > pk) pk = a; }
    if(R) for(int i = 0; i < n; i++) { const float a = fabsf(R[i]); if(a > pk) pk = a; }
    return pk;
}

/* ── The kit map on P5's ring: 24 keys over 16 LEDs ─────────────────────
 * Keys 0..7 (the bottom two thirds of the first octave) own LEDs 0..7; keys
 * 8..23 pair up on LEDs 8..15 (8,9 → 8 … 22,23 → 15). */
inline int KeyLed(int key)
{
    if(key < 0) key = 0;
    if(key >= Engine::kKeys) key = Engine::kKeys - 1;
    return key < 8 ? key : 8 + (key - 8) / 2;
}

/* The engine's default key → grid slice mapping (Engine::DefaultKeyTable),
 * for Clear (style 8): the slice key `key` played before it was recorded
 * into — k · nSlices / span, span = min(nSlices, 24), keys past the span
 * play the last slice. */
inline int SpanSlice(int key, int nSlices)
{
    const int span = nSlices < Engine::kKeys ? nSlices : Engine::kKeys;
    if(span <= 0) return 0;
    if(key < 0) key = 0;
    const int kk = key >= span ? span - 1 : key;
    return (kk * nSlices) / span;
}

/* ── Loop record: the take's length when clocked ────────────────────────
 * `bars` bars of 4/4 at `bpm`: bars × 16 sixteenths × (15 / bpm) s each.
 * 0 when the tempo or the count is not positive (= no auto-stop). */
inline int TakeSamples(int bars, float bpm, float sr)
{
    if(bars <= 0 || !(bpm > 0.f) || !(sr > 0.f)) return 0;
    return (int)((double)bars * 16.0 * (15.0 / (double)bpm) * (double)sr + 0.5);
}

/* J6 rising edges dropped during a clocked take (positions relative to
 * the take's start) → a cue list shaped exactly like a WAV's cue chunk
 * (unsorted file positions are fine: CuesToOnsets sorts). Out-of-range
 * markers are dropped, so are markers closer than `minGap` samples to the
 * previous kept one (a double edge). Returns the count. */
inline int MarkersToCues(const int* m, int n, int len, int minGap, int* cues, int maxCues)
{
    int nc = 0;
    for(int i = 0; i < n && nc < maxCues; i++)
    {
        const int p = m[i];
        if(p < 0 || p >= len) continue;
        if(nc > 0 && p - cues[nc - 1] < minGap && p >= cues[nc - 1]) continue;
        cues[nc++] = p;
    }
    return nc;
}

/* The load pipeline's cue path (ServiceLoad, review M1), factored: cue
 * positions are FILE positions — re-base by the conditioning's head cut,
 * drop what fell outside [0, len), sort, and open the table with 0 unless
 * the first cue lies within 20 ms of it. `cues` is rewritten in place
 * (the kept, re-based, sorted list); returns the onset table's length. */
inline int CuesToOnsets(int* cues, int nCues, int trimmedStart, int len, float sr, int* onsets, int maxOnsets)
{
    int nc = 0;
    for(int i = 0; i < nCues; i++) { const int q = cues[i] - trimmedStart; if(q >= 0 && q < len) cues[nc++] = q; }
    for(int i = 1; i < nc; i++)
        for(int j = i; j > 0 && cues[j - 1] > cues[j]; j--)
        { int t = cues[j - 1]; cues[j - 1] = cues[j]; cues[j] = t; }
    int n = 0;
    if(nc == 0 || cues[0] > (int)(0.02f * sr)) onsets[n++] = 0;
    for(int i = 0; i < nc && n < maxOnsets; i++) onsets[n++] = cues[i];
    return n;
}

/* ── Stereo conditioning ────────────────────────────────────────────────
 * ConditionBuffer works on the L plane (trim, one gain, the bar cut, the
 * memmoves) and reports what it did in ConditionInfo; the R plane follows
 * with the same head cut and the same gain so the image holds. CONTROL. */
inline void ConditionPlane(float* r, const ConditionInfo& ci, int outLen)
{
    if(!r || outLen <= 0) return;
    if(ci.trimmedStart > 0) memmove(r, r + ci.trimmedStart, sizeof(float) * (size_t)outLen);
    if(ci.gain != 1.f)
        for(int i = 0; i < outLen; i++) { r[i] *= ci.gain; if((i & 4095) == 4095) Yield(); }
}

/* A clocked take is exactly `bars` bars from the moment it started: no
 * trim (a silent first 16th is still the first 16th), just the peak
 * normalise to 0.95 with one gain for both planes. Returns the gain
 * applied (1 = silent, left alone). CONTROL. */
inline float NormalisePlanes(float* L, float* R, int len)
{
    float peak = 0.f;
    for(int i = 0; i < len; i++)
    {
        float a = fabsf(L[i]); if(a > peak) peak = a;
        if(R) { a = fabsf(R[i]); if(a > peak) peak = a; }
        if((i & 4095) == 4095) Yield();
    }
    if(peak < 1e-4f) return 1.f;
    const float g = 0.95f / peak;
    for(int i = 0; i < len; i++)
    {
        L[i] *= g;
        if(R) R[i] *= g;
        if((i & 4095) == 4095) Yield();
    }
    return g;
}

/* ── The loop take: AUDIO-thread copy into a plane pair ─────────────────
 * The engine records into ITS planes past the material (RecFeed); a loop
 * take goes into the shell's OTHER buffer, which no engine owns, so it has
 * its own copier — the same shape as RecFeed: O(n), one bounds clamp, no
 * per-sample branch. Start/Stop are CONTROL; Feed is AUDIO; `pos` and
 * `running` are what the control thread polls. Stops itself at `target`
 * (a clocked take's bar count in samples) or at `cap`. */
struct TakeRecorder
{
    float*         L       = nullptr;
    float*         R       = nullptr;   // nullptr: mono take
    int            cap     = 0;
    int            target  = 0;         // stop here (≤ cap)
    volatile int   pos     = 0;         // write head = samples taken so far
    volatile bool  running = false;
    volatile float peak    = 0.f;       // running block peak (the meter clears it)

    /* CONTROL. Begin a take into planes of `capacity`; `targetLen` > 0 = a
     * fixed length (clamped to the capacity), else run until Stop / cap. */
    void Start(float* l, float* r, int capacity, int targetLen)
    {
        L      = l;
        R      = r;
        cap    = capacity;
        target = (targetLen > 0 && targetLen < capacity) ? targetLen : capacity;
        pos    = 0;
        peak   = 0.f;
        running = true;   // last: publishes the take to the audio thread
    }
    /* AUDIO. Append a block; `inR` feeds R (nullptr: L copied to R). */
    void Feed(const float* in, const float* inR, int n)
    {
        if(!running) return;
        const int p    = pos;
        const int room = target - p;
        if(n > room) n = room;
        if(n <= 0) { running = false; return; }
        float* d = L + p;
        for(int i = 0; i < n; i++) d[i] = in[i];
        if(R)
        {
            const float* s  = inR ? inR : in;
            float*       dr = R + p;
            for(int i = 0; i < n; i++) dr[i] = s[i];
        }
        float pk = peak;
        for(int i = 0; i < n; i++) { const float a = fabsf(in[i]); if(a > pk) pk = a; }
        peak = pk;
        pos  = p + n;
        if(p + n >= target) running = false;
    }
    /* CONTROL. Stop (the audio thread appends nothing after this returns —
     * up to the block in flight) and return the length taken. */
    int Stop() { running = false; return pos; }
};

} // namespace bb
