#pragma once
// EDIT-PANEL (docs/packages/slice-edit-panel.md): the pure half of the
// module's own Edit page — what the shell's OnPoll / OnRender / ServicePanel
// compute from pot positions and the table, host-tested in
// tests/panel_check.cpp. No engine dependency: everything takes plain ints
// and the per-entry flags byte (b1 = tombstone, as Engine::tableFlags), so
// the walker, the pickup, the rate window, the nudge → request mapping and
// the ring states are the same numbers on the host and on the module. The
// shell turns a NudgePlan into a bb::EditReq and posts it through the edit
// mailbox; nothing here touches the table.
#include <cstdint>

namespace bb {
namespace panel {

constexpr int      kNudgeMaxMs   = 50;        // P2 / P3: ±50 ms about centre
constexpr int      kAuditionMs   = 250;       // every landed nudge re-plays this much
constexpr uint32_t kRateUs       = 50000u;    // ≥ 50 ms between posted nudges (~20/s)
constexpr float    kCentreTol    = 0.005f;    // the SDK's kCatchTolerance: proximity pickup
constexpr float    kDeadBand     = 0.01f;     // pot within this of centre = no change
constexpr float    kP1Wake       = 0.006f;    // P1 must travel this far before it re-selects
constexpr uint8_t  kFlagTomb     = 2u;        // Engine::kFlagTomb
constexpr int      kKeys         = 24;        // Engine::kKeys
constexpr int      kGridMin      = 64;        // Engine::kGridMin
constexpr int      kRecMin       = 480;       // Engine::kRecMin

/* ── The selection walker: live entries in table order (grid, then markers),
 * tombstones skipped. `flags[t]` is the entry's flags byte; nTable the
 * table length. ───────────────────────────────────────────────────────── */
inline bool Live(const uint8_t* flags, int nTable, int t)
{
    return t >= 0 && t < nTable && !(flags[t] & kFlagTomb);
}
inline int LiveCount(const uint8_t* flags, int nTable)
{
    int n = 0;
    for(int t = 0; t < nTable; t++) if(!(flags[t] & kFlagTomb)) n++;
    return n;
}
/* The ordinal-th live entry (0-based), or −1 past the end. */
inline int LiveAt(const uint8_t* flags, int nTable, int ordinal)
{
    if(ordinal < 0) return -1;
    for(int t = 0; t < nTable; t++)
        if(!(flags[t] & kFlagTomb) && ordinal-- == 0) return t;
    return -1;
}
/* The ordinal of entry t among the live ones, or −1 for a tombstone / out of range. */
inline int OrdinalOf(const uint8_t* flags, int nTable, int t)
{
    if(!Live(flags, nTable, t)) return -1;
    int n = 0;
    for(int k = 0; k < t; k++) if(!(flags[k] & kFlagTomb)) n++;
    return n;
}
/* P1's position 0..1 → an entry: the ordinal is floor(v · live), clamped, so
 * the pot's travel spans the live entries evenly; −1 with nothing live. */
inline int SelectByPot(float v, const uint8_t* flags, int nTable)
{
    const int n = LiveCount(flags, nTable);
    if(n <= 0) return -1;
    if(v < 0.f) v = 0.f;
    if(v > 1.f) v = 1.f;
    int ord = (int)(v * (float)n);
    if(ord >= n) ord = n - 1;
    return LiveAt(flags, nTable, ord);
}
/* Engine::SpanSlice — the key's default grid slice (k · nSlices / span). */
inline int SpanSlice(int key, int nSlices)
{
    const int span = nSlices < kKeys ? nSlices : kKeys;
    if(span <= 0) return 0;
    if(key < 0) key = 0;
    const int kk = key >= span ? span - 1 : key;
    return (kk * nSlices) / span;
}
/* The entry a Voice-map key selects: Engine::KeySlice's reading of the key
 * table (clamped key, an index past the table → the last entry), and a
 * tombstone → the key's span slice (the engine re-points keys at a delete;
 * this covers a torn read). −1 with an empty table. */
inline int KeyEntry(const int8_t* keySlice, int key, int nSlices, int nTable, const uint8_t* flags)
{
    if(nTable <= 0) return -1;
    if(key < 0) key = 0;
    if(key >= kKeys) key = kKeys - 1;
    int s = keySlice[key];
    if(s < 0) s = SpanSlice(key, nSlices);
    if(s >= nTable) s = nTable - 1;
    if(flags[s] & kFlagTomb) s = SpanSlice(key, nSlices);
    return s < nTable ? s : -1;
}
/* After the table changed under a selection: the same entry if it is still
 * live, else the live entry at its old ordinal, else the last live one, else −1. */
inline int Reselect(int sel, int oldOrdinal, const uint8_t* flags, int nTable)
{
    if(Live(flags, nTable, sel)) return sel;
    const int n = LiveCount(flags, nTable);
    if(n <= 0) return -1;
    if(oldOrdinal < 0) oldOrdinal = 0;
    if(oldOrdinal >= n) oldOrdinal = n - 1;
    return LiveAt(flags, nTable, oldOrdinal);
}

/* ── Pickup (the SDK's pot-catch, centred): a nudge pot is inert until it
 * passes through centre (crossover) or rests within kCentreTol of it
 * (proximity). Re-armed on every selection and every re-base. ────────── */
struct Pickup
{
    bool caught = false;
    int  sign   = 0;      // sign of (phys − centre) when armed; 0 = caught at arm
};
inline void ArmPickup(Pickup& p, float phys)
{
    const float d = phys - 0.5f;
    if(d > -kCentreTol && d < kCentreTol) { p.caught = true; p.sign = 0; }
    else { p.caught = false; p.sign = d > 0.f ? 1 : -1; }
}
inline bool UpdatePickup(Pickup& p, float phys)
{
    if(p.caught) return true;
    const float d = phys - 0.5f;
    if(d > -kCentreTol && d < kCentreTol) { p.caught = true; return true; }
    const int s = d > 0.f ? 1 : -1;
    if(s != p.sign) { p.caught = true; return true; }
    return false;
}
/* Pot 0..1 → whole milliseconds about centre: ±kNudgeMaxMs at the ends, 0
 * inside the dead band, symmetric, rounded to the nearest ms (1 ms steps:
 * 100 over the travel, so a resting pot's jitter never posts). */
inline int OffsetMs(float phys)
{
    float d = phys - 0.5f;
    if(d > -kDeadBand && d < kDeadBand) return 0;
    if(d > 0.5f) d = 0.5f;
    if(d < -0.5f) d = -0.5f;
    const float ms = d * 2.f * (float)kNudgeMaxMs;
    int r = (int)(ms >= 0.f ? ms + 0.5f : ms - 0.5f);
    if(r > kNudgeMaxMs) r = kNudgeMaxMs;
    if(r < -kNudgeMaxMs) r = -kNudgeMaxMs;
    return r;
}
inline int MsToSamples(int ms, int sr) { return ms * (sr / 1000); }
/* What the ring shows: the offset as −1..+1 of the range. */
inline float RingValue(int ms)
{
    float v = (float)ms / (float)kNudgeMaxMs;
    if(v > 1.f) v = 1.f;
    if(v < -1.f) v = -1.f;
    return v;
}

/* ── The rate window: at most one post per kRateUs, the LAST offered value
 * wins. Offer() answers true when the value should go out now; otherwise it
 * is held and Drain() releases it once the window has passed. ────────── */
struct RateGate
{
    uint32_t lastUs = 0;
    bool     armed  = false;   // lastUs is meaningful
    bool     pend   = false;
    int      val    = 0;
};
inline bool Offer(RateGate& g, int val, uint32_t nowUs, int* out, uint32_t minUs = kRateUs)
{
    if(!g.armed || (uint32_t)(nowUs - g.lastUs) >= minUs)
    {
        g.armed = true; g.lastUs = nowUs; g.pend = false; *out = val;
        return true;
    }
    g.pend = true; g.val = val;
    return false;
}
inline bool Drain(RateGate& g, uint32_t nowUs, int* out, uint32_t minUs = kRateUs)
{
    if(!g.pend) return false;
    if((uint32_t)(nowUs - g.lastUs) < minUs) return false;
    g.pend = false; g.lastUs = nowUs; *out = g.val;
    return true;
}

/* ── Nudge → request. The base geometry is the entry as it was when the pot
 * was armed (so the offset stays "from where it was", not cumulative), and
 * the neighbours' geometry bounds a grid move the way ApplyEdit will:
 * both sides keep kGridMin. A marker keeps kRecMin and stays inside
 * [0, freePos). The plan carries the audition too: the first kAuditionMs of
 * the new span for a start move, the last for an end move. ──────────── */
struct NudgeBase
{
    int t         = -1;   // the selected entry
    int nSlices   = 0;
    int origStart = 0, origEnd = 0;      // the span when the pots were armed: the offsets' origin
    int start     = 0, end = 0;          // the span now (Landed() follows each posted nudge): the clamps' truth
    int prevStart = 0;    // grid: slice t−1's start (a start move keeps it ≥ kGridMin behind)
    int nextEnd   = 0;    // grid: slice t+1's end (an end move keeps it ≥ kGridMin ahead)
    int freePos   = 0;    // markers: the buffer's used extent
};
enum class NudgeKind : uint8_t { Refused, Boundary, Entry };
struct NudgePlan
{
    NudgeKind kind = NudgeKind::Refused;
    int  i = 0;                       // Boundary: the boundary index
    int  t = 0;                       // Entry: the marker
    int  start = 0, len = 0;          // Entry: the new span; Boundary: start = pos
    int  audStart = 0, audLen = 0;    // the audition region (CallRegion)
    bool fromEnd = false;             // the audition plays up to audStart
};
inline int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* which: 0 = the start (P2), 1 = the end (P3). offSamples: the pickup's offset. */
inline NudgePlan PlanNudge(const NudgeBase& b, int which, int offSamples, int sr)
{
    NudgePlan p;
    const int aud = MsToSamples(kAuditionMs, sr);
    if(b.t < 0) return p;
    if(b.t < b.nSlices)
    {
        /* grid: the start is boundary t (needs a predecessor), the end is boundary t+1 (needs a successor) */
        if(which == 0)
        {
            if(b.t == 0) return p;
            const int lo = b.prevStart + kGridMin, hi = b.end - kGridMin;
            if(lo > hi) return p;
            p.kind = NudgeKind::Boundary; p.i = b.t; p.start = ClampInt(b.origStart + offSamples, lo, hi);
            const int len = b.end - p.start;
            p.audStart = p.start; p.audLen = len < aud ? len : aud; p.fromEnd = false;
        }
        else
        {
            if(b.t + 1 >= b.nSlices) return p;
            const int lo = b.start + kGridMin, hi = b.nextEnd - kGridMin;
            if(lo > hi) return p;
            p.kind = NudgeKind::Boundary; p.i = b.t + 1; p.start = ClampInt(b.origEnd + offSamples, lo, hi);
            const int len = p.start - b.start;
            p.audStart = p.start; p.audLen = len < aud ? len : aud; p.fromEnd = true;
        }
        return p;
    }
    /* marker: a free span, kRecMin long, inside [0, freePos) */
    int s = b.start, e = b.end;
    if(which == 0) s = ClampInt(b.origStart + offSamples, 0, b.end - kRecMin);
    else           e = ClampInt(b.origEnd + offSamples, b.start + kRecMin, b.freePos);
    if(s < 0 || e > b.freePos || e - s < kRecMin) return p;
    p.kind = NudgeKind::Entry; p.t = b.t; p.start = s; p.len = e - s;
    const int len = e - s;
    p.audLen = len < aud ? len : aud;
    if(which == 0) { p.audStart = s; p.fromEnd = false; }
    else           { p.audStart = e; p.fromEnd = true; }
    return p;
}

/* A posted plan landed: the current span follows it; the origins stay. */
inline void Landed(NudgeBase& b, const NudgePlan& p, int which)
{
    if(p.kind == NudgeKind::Boundary) { if(which == 0) b.start = p.start; else b.end = p.start; }
    else if(p.kind == NudgeKind::Entry) { b.start = p.start; b.end = p.start + p.len; }
}
/* Arm: the span now is the origin. */
inline void SetSpan(NudgeBase& b, int start, int end)
{
    b.origStart = b.start = start; b.origEnd = b.end = end;
}

/* ── The computed grid (Engine::SetMaterial's boundaries, for B2 hold =
 * revert): i · step, and with swing each odd boundary at `swing` of its
 * 8th; i == nSlices is the material's end. ───────────────────────────── */
inline int GridStart(int i, int bufLen, int nSlices, float swing, bool swung)
{
    if(nSlices <= 0 || i <= 0) return 0;
    if(i >= nSlices) return bufLen;
    const int step = bufLen / nSlices;
    if(swung && (i & 1))
        return (i - 1) * step + (int)((float)(2 * step) * swing + 0.5f);
    return i * step;
}
inline bool Swung(float swing, int nSlices)
{
    return swing > 0.5f && swing < 0.75f && nSlices >= 2 && (nSlices & 1) == 0;
}
/* The revert of a grid slice: boundaries t and t+1 back to the computed
 * grid, each only when it differs (0 = nothing to do; 1 = start; 2 = end;
 * 3 = both). A marker's revert is a tombstone — the shell's. */
inline int RevertMask(int t, int nSlices, int curStart, int curEnd, int gridStart, int gridEnd)
{
    int m = 0;
    if(t <= 0 || t >= nSlices) { if(t == 0 && nSlices > 1 && curEnd != gridEnd) m |= 2; return m; }
    if(curStart != gridStart) m |= 1;
    if(t + 1 < nSlices && curEnd != gridEnd) m |= 2;
    return m;
}

/* ── The bar ring in Edit: the displayed bar is the selection's when the
 * selection is a grid slice (so it is always visible), else the playing
 * one; each of its 16 LEDs is one of four states. ───────────────────── */
inline int BarBase(int sel, int nSlices, int cur)
{
    const int s = (sel >= 0 && sel < nSlices) ? sel : cur;
    return (s / 16) * 16;
}
enum : uint8_t { kLedPlain = 0, kLedSelDim = 1, kLedSel = 2, kLedPulse = 3, kLedHead = 4 };
/* step: the absolute step; src: stepSrc[step] (−1 = rest); cur: the playing
 * step; callFlash: the entry of a recent Voice call (−1 none). The selected
 * entry's own segment is kLedSel; a step that plays it (its source) is
 * kLedSelDim; either one pulses while it is the playing step, and a call of
 * the entry pulses its own segment (a marker's: none — its keys pulse on P5);
 * the playhead elsewhere is kLedHead. */
inline uint8_t BarLed(int step, int sel, int src, int cur, int callFlash, int nSlices)
{
    if(step < 0 || step >= nSlices) return kLedPlain;
    const bool own = step == sel;
    const bool plays = sel >= 0 && src == sel;
    if((own || plays) && (step == cur || (own && callFlash == sel))) return kLedPulse;
    if(own) return kLedSel;
    if(plays) return kLedSelDim;
    if(step == cur) return kLedHead;
    return kLedPlain;
}
/* P5's kit map: the cells of every key pointing at the selection pulse
 * (KeyLed pairs keys 8..23, so a pair lights when either member points). */
inline bool KeyOnSelection(const int8_t* keySlice, int key, int sel, int nSlices, int nTable, const uint8_t* flags)
{
    return sel >= 0 && KeyEntry(keySlice, key, nSlices, nTable, flags) == sel;
}
/* A 0..1 triangle from a frame counter, period `period` frames. */
inline float Pulse(uint32_t frame, uint32_t period)
{
    if(period < 2u) return 1.f;
    const uint32_t ph = frame % period;
    const float half = (float)period * 0.5f;
    const float v = ph < half ? (float)ph / half : (float)(period - ph) / half;
    return v;
}

} // namespace panel
} // namespace bb
