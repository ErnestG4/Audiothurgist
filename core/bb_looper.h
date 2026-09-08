#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace bb {

// LOOPER (docs/packages/sample-looper.md, phase D of docs/sample-mode-plan.md):
// the unquantized looper behind the Play grammar. Pure, allocation-free,
// owned by Engine as `looper`. The engine records a Voice call into it
// (Engine::CallSlice / ReleaseKey while the looper is active) and fires the
// events back through the very same call path each time the playhead
// crosses their phase (Engine::ProcessT) — sample-exact within the block.
// Nothing here touches audio: this is the event memory and its playhead
// window arithmetic, host-tested on its own.
//
// Events are kept SORTED BY PHASE (an insertion memmove of ≤ 255 16-byte
// entries: O(events) on the audio thread, no allocation), so a block's
// window is two binary searches and a contiguous scan. Undo is by
// GENERATION: every event carries the generation it was recorded in; a
// generation closes at the first loop wrap after something was recorded
// into it, so "undo" is "the last pass's overdub".
struct Looper
{
    static constexpr int kMax = 256;

    // style bits (the wire byte of docs/packages/sample-persist.md's
    // `looper[].style`). kSkip is transient — never persisted (Add clears it).
    enum : uint8_t
    {
        kRev      = 1u << 0,
        kHalf     = 1u << 1,
        kDouble   = 1u << 2,
        kRatchet  = 1u << 3,
        kStaccato = 1u << 4,
        kHeld     = 1u << 5,   // the note was held: a kRelease event follows
        kRelease  = 1u << 6,   // the key came up (no slice: `key` only)
        kSkip     = 1u << 7,   // recorded ahead of the playhead: not fired on its first pass
        kWire     = 0x7Fu
    };

    struct Event
    {
        double  phase;   // 0..1 of the loop (bar-relative, never ≥ 1)
        float   vel;     // velocity 0..1 (reserved: the Voice map has no velocity in — 1.0)
        int8_t  key;     // Voice key 0..23, or -1: a keyless call (Brain map, web)
        int8_t  slice;   // table entry called — the replay target when key < 0
        uint8_t style;   // bits above
        uint8_t gen;     // generation (the undo unit)
    };

    Event    ev[kMax];
    int      n         = 0;
    int      quant     = 0;    // 0 exact · 1 nearest step (16ths) · 2 nearest step pair (8ths)
    int      divisions = 16;   // steps per loop (the engine's nSlices), for the snap
    uint8_t  gen       = 0;    // generation being recorded into
    bool     dirty     = false; // something was recorded into `gen` since the last wrap
    uint32_t heldMask  = 0;    // keys whose held note-on is recorded and awaits its release
    double   frontier  = -1.;  // unwrapped end phase of the last window (-1: none yet)

    // A block's window over the loop: the phase span the playhead crosses in
    // this block, as up to three index ranges of `ev` in time order —
    // range 0: events just before the wrap when the wrap fell between two
    // windows (phase − 1 relative to `start`); range 1: the plain span;
    // range 2: events past the wrap inside this block (phase + 1).
    struct Window
    {
        double start = 0., span = 0.;   // this block's phase and extent
        int    lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
        double shift[3] = {-1., 0., 1.};
        bool   wrapped = false;         // the window reached the loop's end
        bool Range(int pass, int& a, int& b, double& sh) const
        {
            a = lo[pass]; b = hi[pass]; sh = shift[pass];
            return b > a;
        }
    };

    int  Count() const { return n; }
    bool Full() const { return n >= kMax; }

    // Nearest division; 1.0 wraps to 0 (a late hit quantized to the downbeat
    // plays at the NEXT downbeat — the looper's version of Play's punch).
    static double Snap(double phase, int div)
    {
        if(div < 1) return phase;
        double s = floor(phase * (double)div + 0.5) / (double)div;
        if(s >= 1.) s -= 1.;
        if(s < 0.) s = 0.;
        return s;
    }
    double Quantize(double phase) const
    {
        if(quant == 1) return Snap(phase, divisions);
        if(quant == 2) return Snap(phase, divisions / 2 > 0 ? divisions / 2 : 1);
        return phase;
    }

    // Is `phase` ahead of the playhead (within the next half loop)? Such an
    // event would be fired by the window that is about to reach it — the
    // very pass the player is still hearing live — so Record marks it kSkip
    // and it first sounds on the following pass, exactly as a punched step
    // does. Behind the frontier (the normal case: the shell records at the
    // previous block's phase) it fires on the next pass.
    bool Ahead(double phase) const
    {
        if(frontier < 0.) return false;
        const double pe = frontier >= 1. ? frontier - 1. : frontier;
        double d = phase - pe;
        if(d < 0.) d += 1.;
        return d < 0.5;
    }

    // AUDIO. Record a live call at `phase` (quantized per `quant`). false =
    // full (the event is refused; the live call still sounded). A held call
    // (style & kHeld) arms the key for RecordRelease.
    bool Record(double phase, int key, int slice, uint8_t style, float vel)
    {
        if(n >= kMax) return false;
        phase = Quantize(phase);
        style &= kWire;
        if(Ahead(phase)) style |= kSkip;
        Insert(phase, key, slice, style, vel);
        dirty = true;
        if((style & kHeld) && key >= 0 && key < 32) heldMask |= 1u << key;
        return true;
    }
    // AUDIO. The key came up: record its release, if a held note-on for it
    // was recorded and not yet released. false = nothing to release, or full
    // (the replayed note then holds to the material's end).
    bool RecordRelease(double phase, int key)
    {
        if(key < 0 || key >= 32 || !(heldMask & (1u << key))) return false;
        heldMask &= ~(1u << key);
        return Record(phase, key, -1, kRelease, 0.f);
    }
    // CONTROL (before the audio thread runs, or with it stopped): a loaded
    // event — as recorded, no quantize, no skip, into the current generation.
    bool Add(double phase, int key, int slice, uint8_t style, float vel)
    {
        if(n >= kMax) return false;
        phase -= floor(phase);
        Insert(phase, key, slice, (uint8_t)(style & kWire), vel);
        dirty = true;
        return true;
    }
    // Close the generation being recorded (a loader's last step, so the
    // first live overdub is its own undo unit). A wrap does this itself.
    void Seal()
    {
        if(!dirty) return;
        dirty = false;
        NextGen();
    }
    /* SETS: take another looper's events wholesale — one memcpy, so the
     * audio thread can adopt a list the control thread built with Add/Seal
     * (a set load), and the control thread can snapshot the live list
     * (a set save) without racing Insert's memmove. The receiver's window
     * (frontier) and held mask start over; quant/divisions are kept. */
    void Adopt(const Looper& src)
    {
        n = src.n > kMax ? kMax : src.n;
        memcpy(ev, src.ev, (size_t)n * sizeof(Event));
        gen      = src.gen;
        dirty    = src.dirty;
        heldMask = 0;
        frontier = -1.;
    }

    // AUDIO or CONTROL. Remove the newest generation. false = empty.
    bool UndoLast()
    {
        if(n == 0) return false;
        uint8_t g = 0;
        for(int i = 0; i < n; i++) if(ev[i].gen > g) g = ev[i].gen;
        int w = 0;
        for(int i = 0; i < n; i++) if(ev[i].gen != g) ev[w++] = ev[i];
        n        = w;
        gen      = g;      // the next overdub takes the freed slot
        dirty    = false;
        heldMask = 0;      // a pending release of an undone note-on is noise
        return true;
    }
    void Clear()
    {
        n        = 0;
        gen      = 0;
        dirty    = false;
        heldMask = 0;
    }
    // The playhead jumped (new material, barPhase reset): forget the frontier.
    void ResetPlayhead() { frontier = -1.; }
    /* EDIT: table indices moved (a compaction at SetMaterial, a merge, a
     * delete). A KEYLESS event (Brain map, web: key < 0) is bound to its
     * table index, so it follows the map: map[slice] is its new index, −1
     * drops the event (order kept, O(n)). Keyed events resolve through the
     * key table at fire time and are untouched, as are indices ≥ nMap. */
    void RemapSlices(const int8_t* map, int nMap)
    {
        int w = 0;
        for(int i = 0; i < n; i++)
        {
            Event e = ev[i];
            if(e.key < 0 && e.slice >= 0 && e.slice < nMap)
            {
                const int8_t m = map[e.slice];
                if(m < 0) continue;
                e.slice = m;
            }
            ev[w++] = e;
        }
        n = w;
    }

    // AUDIO, once per block BEFORE the events are fired: the span the
    // playhead crosses in this block, [start, start + span), reconciled with
    // where the last window ended so nothing fires twice or is skipped when
    // the shell's phase jitters by a block. Also the loop-wrap detector that
    // closes the generation being recorded. Call it every block whatever the
    // state (it is O(log n)); fire the ranges only while the looper is active.
    // `nSamp` = the block's samples (the window's resolution).
    void Advance(double phase, double span, int nSamp, Window& w)
    {
        w.start = phase;
        w.span  = span;
        w.wrapped = false;
        // The window ends half a sample before the next block's first sample:
        // an event nearer to that sample than to this block's last rolls
        // into the next window (its offset is then 0), so no event is ever
        // fired a sample early by the clamp, and the next window starts here.
        const double half = nSamp > 0 ? 0.5 * span / (double)nSamp : 0.;
        double from;
        if(frontier < 0.) from = phase;
        else
        {
            const double pe  = frontier >= 1. ? frontier - 1. : frontier;
            const double d   = phase - pe;
            const double tol = 4. * span;
            if(d >= -tol && d <= tol) from = pe;                 // continuous (block jitter)
            else if(d > 0. && d < 0.5) from = pe;                // the clock ran ahead: catch up at offset 0
            else if(d < -0.5) { from = pe - 1.; w.wrapped = true; } // the wrap fell between two windows
            else from = phase;                                   // a jump (backward, or half a loop forward)
        }
        const double to = phase + span - half;
        if(to > 1.) w.wrapped = true;
        // range 0: [from + 1, 1) when from < 0
        if(from < 0.) { w.lo[0] = Lower(from + 1.); w.hi[0] = n; }
        else          { w.lo[0] = w.hi[0] = 0; }
        // range 1: [max(from, 0), min(to, 1))
        {
            const double a = from < 0. ? 0. : from, b = to > 1. ? 1. : to;
            w.lo[1] = Lower(a);
            w.hi[1] = b > a ? Lower(b) : w.lo[1];
        }
        // range 2: [0, to − 1) when to > 1
        if(to > 1.) { w.lo[2] = 0; w.hi[2] = Lower(to - 1.); }
        else        { w.lo[2] = w.hi[2] = 0; }
        if(w.wrapped && dirty) { dirty = false; NextGen(); }
        frontier = to;
    }
    // Sample offset of event `e` inside the block of `nSamp` samples for
    // window `w` (`shift` from Window::Range): the nearest sample, clamped.
    static int Offset(const Window& w, double phase, double shift, int nSamp)
    {
        if(w.span <= 0. || nSamp <= 1) return 0;
        const double x = (phase + shift - w.start) / w.span * (double)nSamp + 0.5;
        if(x <= 0.) return 0;
        if(x >= (double)nSamp) return nSamp - 1;
        return (int)x;
    }

  private:
    // first index with phase >= p (events are sorted)
    int Lower(double p) const
    {
        int lo = 0, hi = n;
        while(lo < hi)
        {
            const int mid = (lo + hi) >> 1;
            if(ev[mid].phase < p) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
    // after every event with the same phase (a note-on precedes its release)
    int Upper(double p) const
    {
        int lo = 0, hi = n;
        while(lo < hi)
        {
            const int mid = (lo + hi) >> 1;
            if(ev[mid].phase <= p) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
    void Insert(double phase, int key, int slice, uint8_t style, float vel)
    {
        const int at = Upper(phase);
        if(at < n) memmove(&ev[at + 1], &ev[at], (size_t)(n - at) * sizeof(Event));
        ev[at].phase = phase;
        ev[at].vel   = vel;
        ev[at].key   = (int8_t)key;
        ev[at].slice = (int8_t)slice;
        ev[at].style = style;
        ev[at].gen   = gen;
        n++;
    }
    // Open the next generation. The byte wraps at 255: renumber from the
    // oldest generation still present, and if 256 generations are all
    // present (one event each — the only way), merge the two oldest.
    void NextGen()
    {
        if(gen < 255) { gen++; return; }
        uint8_t mn = 255;
        for(int i = 0; i < n; i++) if(ev[i].gen < mn) mn = ev[i].gen;
        if(n == 0) mn = 255;
        if(mn == 0)
        {
            for(int i = 0; i < n; i++) if(ev[i].gen > 0) ev[i].gen--;
            gen = 254;
        }
        else
        {
            for(int i = 0; i < n; i++) ev[i].gen = (uint8_t)(ev[i].gen - mn);
            gen = (uint8_t)(255 - mn);
        }
        gen++;
    }
};

} // namespace bb
