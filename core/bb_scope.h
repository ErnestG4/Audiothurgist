#pragma once
// SCOPE: the ten-channel oscilloscope behind HostLink 0x72, pure so the
// host suite can test it (tests/scope_check.cpp) — docs/packages/scope.md.
//
// Two ring tiers on ONE sample clock, both written by the AUDIO callback:
//   audio  4 × kScopeRing int8 at 48 kHz — out L, out R, in L (J1), in R (J2);
//   cv     6 × kScopeCvRing int8, one sample per audio block (48 kHz / 24 =
//          2 kHz) — J3..J8 DC-coupled, ±5 V = ±127 (the ADC1 scan of the six
//          CV inputs completes about every 0.53 ms, so a block is the natural
//          period: every value is stored once, a few twice).
// The write index is the audio sample count, published once per block; the
// CV index is that count / kScopeCvDiv, so the two tiers are time-coherent
// by construction and a window is one instant across all ten channels.
// Positions wrap at kScopeWrap (a multiple of the block and of both rings,
// not 2^32, so a CV slot never jumps at the wrap). The audio thread keeps
// writing while a reply is built, so the oldest kScopeGuard / kScopeCvGuard
// samples are never read. 0x72 is answered on the CONTROL thread from a
// single snapshot of the index. Nothing here touches hardware or allocates;
// the ring memory belongs to the shell.
//
// A tier read at decimation 1 answers its samples; at any other decimation
// it answers each group's MINIMUM AND MAXIMUM (two bytes a point), so the
// trace is the signal's envelope and nothing above the decimated rate folds
// down into a waveform that was never there (picking one sample in `decim`
// aliased everything above 24 kHz / decim: at ×16 a hat drew as a 1 kHz
// wobble). The audio callback is untouched — the rings stay full rate and
// the groups are read on the control thread.
#include <cstdint>

namespace bb {

constexpr int      kScopeChans     = 4;          // out L · out R · in L · in R
constexpr int      kScopeRing      = 65536;      // audio samples per channel (1.37 s at 48 kHz)
constexpr int      kScopeGuard     = 256;        // oldest audio samples left alone (5 ms)
constexpr int      kScopeUsable    = kScopeRing - kScopeGuard;
constexpr int      kScopeCvChans   = 6;          // J3..J8
constexpr int      kScopeCvDiv     = 24;         // audio samples per CV sample (= the audio block)
constexpr int      kScopeCvRing    = 16384;      // CV samples per channel (8.2 s at 2 kHz)
constexpr int      kScopeCvGuard   = 16;         // oldest CV samples left alone (8 ms)
constexpr int      kScopeCvUsable  = kScopeCvRing - kScopeCvGuard;
constexpr uint32_t kScopeWrap      = (uint32_t)kScopeCvDiv * (1u << 26);   // 1 610 612 736: 24 · 2^26 (the audio clock)
constexpr uint32_t kScopeCvWrap    = 1u << 26;                              // the cv clock = the audio clock / 24
constexpr int      kScopeMaxN      = 512;        // points per channel per reply (a point is one sample, or a min/max pair)
constexpr int      kScopeMaxDecim  = 16;         // audio: 512 × 16 = 171 ms
constexpr int      kScopeCvMaxDecim = 64;        // cv: 512 × 64 / 2 kHz = 16 s > the ring; 32 = 8 s
constexpr int      kScopeHdr       = 14;         // reply: status mask decim n(2) flags pos(4) level(4)
constexpr int      kScopeHdrExt    = 8;          // … + maskHi cvDecim cvDiv trigSrc cvPos(4) (with kScopeExt)
constexpr int      kScopeReqMin    = 5;          // request: mask decim n(2) flags
constexpr int      kScopeReqPos    = 9;          // … + u32 pos (with kScopeAt)
constexpr int      kScopeReqExt    = 17;         // … + u8 maskHi, u8 cvDecim, i8 trigLevel, u8 trigSrc, u32 cvPos (with kScopeExt; pos and cvPos present, used with kScopeAt)
constexpr uint8_t  kScopeTrigAuto  = 0;          // trigSrc: the first selected channel (audio before cv)
constexpr uint8_t  kScopeTrigSet   = 0x80;       // trigSrc: | 0..3 an audio channel, 4..9 J3..J8 (need not be selected)
constexpr uint8_t  kScopeTrigNone  = 255;        // reply trigSrc when no trigger ran
constexpr int8_t   kScopeHyst      = 4;          // trigger hysteresis, int8 units (≈ 3 % of full scale, 0.16 V on a CV)
constexpr int      kScopeTrigHold  = 4800;       // an AUDIO trigger looks back span + max(span, this) samples (100 ms: down to 10 Hz), then free-runs
constexpr int      kScopeLevelSpan = 2048;       // the reply's four audio levels: peak |x| over the newest 43 ms
constexpr float    kScopeCvPerVolt = 25.4f;      // int8 per volt: ±5 V = ±127
constexpr float    kScopeCvGain    = 0.165f;     // |dV_pin / dV_jack| — alchemy::kV2CvInGainDesign
constexpr float    kScopeCvVdda    = 3.30f;      // alchemy::kV2VddaDesign (the fallback)

static_assert(kScopeWrap % (uint32_t)kScopeRing == 0u, "audio ring must divide the wrap");
static_assert((kScopeWrap / (uint32_t)kScopeCvDiv) % (uint32_t)kScopeCvRing == 0u, "cv ring must divide the wrap in blocks");

/* Request flags. */
enum : uint8_t {
    kScopeTrig = 1,   // align the window to the most recent rising crossing of the trigger level on the trigger channel
    kScopeFrame = 2,  // the first request of a frame split over requests: place the window for BOTH tiers (the audio at decim, and with
                      // kScopeExt the cv at cvDecim) though this request selects channels of one — the rest of the frame is pinned to it
    kScopeAt   = 4,   // the request carries u32 pos (and with kScopeExt u32 cvPos): each tier starts there (a later request of the same frame)
    kScopeExt  = 8,   // the request carries maskHi (J3..J8), cvDecim, trigLevel, trigSrc and cvPos; the reply carries the ext header
};
/* Reply flags. */
enum : uint8_t {
    kScopeTriggered  = 1,   // the window starts on a crossing
    kScopeAtHonoured = 2,   // the window starts at the requested pos
    kScopeExtReply   = 4,   // the ext header follows the 10-byte header
    kScopeDetached   = 8,   // one tier could not hold the window's start and shows its newest window instead
};

/* ── the sample clocks (audio: wrap kScopeWrap; cv: kScopeCvWrap) ─────── */
inline uint32_t ScopeNext(uint32_t p, uint32_t wrap = kScopeWrap)              { p++; return p == wrap ? 0u : p; }
inline uint32_t ScopePrev(uint32_t p, uint32_t wrap = kScopeWrap)              { return p == 0u ? wrap - 1u : p - 1u; }
inline uint32_t ScopeAdd(uint32_t p, uint32_t k, uint32_t wrap = kScopeWrap)   { return (uint32_t)(((uint64_t)p + k) % wrap); }
inline uint32_t ScopeSub(uint32_t p, uint32_t k, uint32_t wrap = kScopeWrap)   { k %= wrap; return p >= k ? p - k : p + (wrap - k); }
inline uint32_t ScopeAge(uint32_t now, uint32_t p, uint32_t wrap = kScopeWrap) { return now >= p ? now - p : now + (wrap - p); }   // samples from p to now
inline int      ScopeSlot(uint32_t pos)             { return (int)(pos & (uint32_t)(kScopeRing - 1)); }
inline uint32_t ScopeCvPos(uint32_t pos)            { return pos / (uint32_t)kScopeCvDiv; }
inline int      ScopeCvSlot(uint32_t cvpos)         { return (int)(cvpos & (uint32_t)(kScopeCvRing - 1)); }

/* ── conversions ──────────────────────────────────────────────────────── */
inline int8_t ScopeI8(float v)
{
    if(v > 1.f) v = 1.f; else if(v < -1.f) v = -1.f;
    return (int8_t)(v * 127.f);
}
/* AUDIO: a raw 16-bit CV reading → int8 at ±5 V = ±127, saturating (the ADC
 * spans ±10 V at the jack, the panel is ±5 V). `zero` is the jack's idle
 * code (the calibration record's adc_zero_code, 32768 by design), `scale`
 * from ScopeCvScale. Same transform as alchemy::CvInput::Volts(): higher
 * raw = more negative jack voltage (the input stage inverts). */
inline float ScopeCvScale(float vdda) { return vdda / 65535.f / kScopeCvGain * kScopeCvPerVolt; }
inline int8_t ScopeCvCode(uint16_t raw, uint16_t zero, float scale)
{
    float c = ((float)zero - (float)raw) * scale;
    if(c > 127.f) c = 127.f; else if(c < -127.f) c = -127.f;
    return (int8_t)(c >= 0.f ? c + 0.5f : c - 0.5f);
}
inline float ScopeCvVolts(int8_t code) { return (float)code / kScopeCvPerVolt; }

/* ── the request ──────────────────────────────────────────────────────── */
inline int ScopeChanCount(uint8_t mask) { int c = 0; for(int i = 0; i < 8; i++) if(mask & (1u << i)) c++; return c; }
inline int ScopeFirstChan(uint8_t mask) { for(int i = 0; i < 8; i++) if(mask & (1u << i)) return i; return -1; }

struct ScopeReq
{
    uint8_t  mask    = 0;   // b0 out L · b1 out R · b2 in L · b3 in R
    uint8_t  decim   = 1;   // audio: 1..16
    uint16_t n       = 0;   // points per channel
    uint8_t  flags   = 0;
    uint32_t pos     = 0;   // with kScopeAt
    uint8_t  maskHi  = 0;   // with kScopeExt: b0..b5 = J3..J8
    uint8_t  cvDecim = 1;   // with kScopeExt: 1..64, on the CV ring
    int8_t   level   = 0;   // with kScopeExt: trigger level (audio: full scale 127; cv: 25.4 per volt)
    uint8_t  trig    = 0;   // with kScopeExt: kScopeTrigAuto, or kScopeTrigSet | channel 0..9
    uint32_t cvPos   = 0;   // with kScopeExt and kScopeAt: the cv tier's start, on the cv clock
    /* the tiers the window is placed for: those with a channel selected — and, in the first request of a split frame, both */
    bool UseA() const { return mask   != 0u || (!(flags & kScopeAt) && (flags & kScopeFrame)); }
    bool UseC() const { return maskHi != 0u || (!(flags & kScopeAt) && (flags & kScopeFrame) && (flags & kScopeExt)); }
    int Hdr()   const { return kScopeHdr + ((flags & kScopeExt) ? kScopeHdrExt : 0); }
    int Chans() const { return ScopeChanCount(mask) + ScopeChanCount(maskHi); }
    int BppA()  const { return decim   > 1u ? 2 : 1; }                  // bytes a point: a sample, or min + max of its group
    int BppC()  const { return cvDecim > 1u ? 2 : 1; }
    int PointBytes() const { return ScopeChanCount(mask) * BppA() + ScopeChanCount(maskHi) * BppC(); }   // one point of every selected channel
    int SpanA() const { return UseA() ? (int)n * (int)decim   : 0; }   // audio samples the window covers
    int SpanC() const { return UseC() ? (int)n * (int)cvDecim : 0; }   // cv samples the window covers
};

/* CONTROL: parse and clamp a 0x72 request body (len ≥ 1; the empty body is
 * the pre-SCOPE reply and is handled by the caller). false = BAD_ARGS:
 * short body, no channel at all, mask bits above the four audio channels
 * or the six CV jacks, decim 0 / > 16, cvDecim 0 / > 64, n 0, an at-pos past
 * its clock's wrap. n is then
 * clamped to kScopeMaxN, to what fits `maxBody` beside the header (a channel
 * costs n bytes at decimation 1, 2n above), and to each selected tier's
 * usable ring at its decimation — the reply says what it got. */
inline bool ScopeParse(const uint8_t* body, int len, int maxBody, ScopeReq& r)
{
    if(len < kScopeReqMin) return false;
    r.mask = body[0]; r.decim = body[1];
    r.n = (uint16_t)(body[2] | (body[3] << 8)); r.flags = body[4];
    r.maskHi = 0u; r.cvDecim = 1u; r.level = 0; r.pos = 0u; r.trig = kScopeTrigAuto; r.cvPos = 0u;
    if((r.mask & ~((1u << kScopeChans) - 1u)) != 0u) return false;
    if(r.decim == 0u || r.decim > kScopeMaxDecim || r.n == 0u) return false;
    if(r.flags & (kScopeAt | kScopeExt))
    {
        if(len < kScopeReqPos) return false;
        r.pos = (uint32_t)body[5] | ((uint32_t)body[6] << 8) | ((uint32_t)body[7] << 16) | ((uint32_t)body[8] << 24);
        if((r.flags & kScopeAt) && r.pos >= kScopeWrap) return false;
    }
    if(r.flags & kScopeExt)
    {
        if(len < kScopeReqExt) return false;
        r.maskHi = body[9]; r.cvDecim = body[10]; r.level = (int8_t)body[11]; r.trig = body[12];
        if((r.maskHi & ~((1u << kScopeCvChans) - 1u)) != 0u) return false;
        if(r.cvDecim == 0u || r.cvDecim > kScopeCvMaxDecim) return false;
        if((r.trig & kScopeTrigSet) ? (r.trig & 0x7fu) >= (uint8_t)(kScopeChans + kScopeCvChans) : r.trig != kScopeTrigAuto) return false;
        r.cvPos = (uint32_t)body[13] | ((uint32_t)body[14] << 8) | ((uint32_t)body[15] << 16) | ((uint32_t)body[16] << 24);
        if((r.flags & kScopeAt) && r.cvPos >= kScopeCvWrap) return false;
    }
    if(r.mask == 0u && r.maskHi == 0u) return false;
    int n = r.n;
    if(n > kScopeMaxN) n = kScopeMaxN;
    const int fit = (maxBody - r.Hdr()) / r.PointBytes();
    if(n > fit) n = fit;
    if(r.UseA()) { const int s = kScopeUsable   / r.decim;   if(n > s) n = s; }
    if(r.UseC()) { const int s = kScopeCvUsable / r.cvDecim; if(n > s) n = s; }
    if(n < 1) n = 1;
    r.n = (uint16_t)n;
    return true;
}

/* CONTROL: the most recent rising crossing of `level` on a ring that leaves
 * at least `span` samples after it, or `found` false. A crossing is the
 * sample where the signal goes from < level to ≥ level after having been
 * ≤ level − hyst and before reaching ≥ level + hyst without dipping below
 * level again — a Schmitt trigger, so noise around the level never
 * triggers and the crossing reported is the one that led to the peak.
 * Searched BACKWARDS from the newest admissible start, so the usual cost
 * is the distance to the last crossing, not the ring; the walk is linear
 * (a failed candidate skips to the ≥ hi sample that failed it — every
 * candidate in between would fail on the same sample), so silence or a
 * unipolar gate resting at the level costs one pass over the usable span
 * at most (65 K samples ≈ 0.3 ms). `ringLen` is a power of two (kScopeRing
 * or kScopeCvRing), `wrap` the tier's clock wrap; `wpos` and the result are
 * positions on that clock. */
inline uint32_t ScopeCrossing(const int8_t* ring, int ringLen, int usable, uint32_t wrap, uint32_t wpos, int span, int level, int hyst, bool& found)
{
    found = false;
    if(span < 1 || span > usable) return 0u;
    const uint32_t m = (uint32_t)(ringLen - 1);
    const int hi = level + hyst, lo = level - hyst;
    /* walk from the NEWEST sample (a crossing older than the span may owe its confirmation
     * to samples newer than the span); only a candidate at least `span` old is taken */
    uint32_t p = ScopePrev(wpos, wrap); int next = ring[p & m];   // x[p]
    uint32_t age = 1u;                                      // wpos − q, kept for the sample q below
    int  left = usable - 1;                                 // samples we may still step back over
    bool high = false;                                      // seen ≥ hi since the last sample < level (walking back)
    for(;;)
    {
        if(left <= 0) return 0u;
        uint32_t q = ScopePrev(p, wrap); int v = ring[q & m]; left--; age++;   // x[p − 1]
        if(next >= hi) high = true;
        if(v < level)
        {
            if(next >= level && high && age - 1u >= (uint32_t)span)
            {
                /* candidate crossing at p: confirm that, going back, the signal reaches ≤ lo
                 * before it reaches ≥ hi (else this rise did not come from the low state) */
                const uint32_t cand = p;
                while(!(v <= lo || v >= hi) && left > 0) { q = ScopePrev(q, wrap); v = ring[q & m]; left--; age++; }
                if(v <= lo) { found = true; return cand; }
                if(v < hi) return 0u;                           // ran out of ring
                p = q; next = v; high = true; continue;         // failed on a high: resume there — the samples between held no ≤ lo
            }
            high = false;
        }
        p = q; next = v;
    }
}

/* CONTROL: where the window starts. `pos` is on the audio clock, `cvPos` on
 * the CV clock (= pos / kScopeCvDiv); flags as above. Every selected tier
 * starts at the same instant when it can hold it; a tier that cannot
 * (its start is older than its usable ring, or newer than its span allows)
 * shows its newest window and the reply says kScopeDetached — an audio
 * ring of 1.4 s cannot follow a CV window that starts seconds ago. The
 * reply carries both starts, and a later request of the frame (kScopeAt)
 * asks for each tier at its own. */
struct ScopeWin { uint32_t pos = 0; uint32_t cvPos = 0; uint8_t flags = 0; };

inline bool ScopeHoldsA(uint32_t wpos, uint32_t pos, int span)   { const uint32_t a = ScopeAge(wpos, pos); return a >= (uint32_t)span && a <= (uint32_t)kScopeUsable; }
inline bool ScopeHoldsC(uint32_t cvW, uint32_t cvPos, int span)  { const uint32_t a = ScopeAge(cvW, cvPos, kScopeCvWrap); return a >= (uint32_t)span && a <= (uint32_t)kScopeCvUsable; }

/* The channel a trigger runs on: 0..3 audio, 4..9 J3..J8, −1 none. Auto = the first selected, audio before cv. */
inline int ScopeTrigSource(const ScopeReq& r)
{
    if(r.trig & kScopeTrigSet) return (int)(r.trig & 0x7fu);
    const int fa = ScopeFirstChan(r.mask); if(fa >= 0) return fa;
    const int fc = ScopeFirstChan(r.maskHi); return fc >= 0 ? kScopeChans + fc : -1;
}

/* Place both tiers at the instant `pos` (audio clock); detach a tier that cannot hold it. */
inline ScopeWin ScopePlace(uint32_t wpos, uint32_t pos, const ScopeReq& r, uint8_t flags)
{
    ScopeWin w; w.flags = flags;
    const uint32_t cvW = ScopeCvPos(wpos);
    w.pos = pos; w.cvPos = ScopeCvPos(pos);
    if(r.UseA() && !ScopeHoldsA(wpos, w.pos, r.SpanA()))  { w.pos   = ScopeSub(wpos, (uint32_t)r.SpanA()); w.flags |= kScopeDetached; }
    if(r.UseC() && !ScopeHoldsC(cvW, w.cvPos, r.SpanC()))  { w.cvPos = ScopeSub(cvW, (uint32_t)r.SpanC(), kScopeCvWrap); w.flags |= kScopeDetached; }
    return w;
}

inline ScopeWin ScopeWindow(const int8_t* const audio[kScopeChans], const int8_t* const cv[kScopeCvChans], uint32_t wpos, const ScopeReq& r)
{
    const uint32_t cvW = ScopeCvPos(wpos);
    /* the newest instant every selected tier can start at: the older of the two newest starts */
    uint32_t newest = r.UseA() ? ScopeSub(wpos, (uint32_t)r.SpanA()) : wpos;
    if(r.UseC()) { const uint32_t nc = ScopeSub(cvW, (uint32_t)r.SpanC(), kScopeCvWrap) * (uint32_t)kScopeCvDiv; if(ScopeAge(wpos, nc) > ScopeAge(wpos, newest)) newest = nc; }
    if(r.flags & kScopeAt)
    {
        /* each tier at its own start: a frame whose audio tier detached has two (the first reply gave both) */
        const uint32_t cvAt = (r.flags & kScopeExt) ? r.cvPos : ScopeCvPos(r.pos);
        const bool okA = !r.mask   || ScopeHoldsA(wpos, r.pos, r.SpanA());
        const bool okC = !r.maskHi || ScopeHoldsC(cvW, cvAt, r.SpanC());
        if(okA && okC) { ScopeWin w; w.pos = r.pos; w.cvPos = cvAt; w.flags = kScopeAtHonoured; return w; }
        return ScopePlace(wpos, newest, r, 0u);
    }
    if(r.flags & kScopeTrig)
    {
        bool found = false;
        const int src = ScopeTrigSource(r);
        if(src >= 0 && src < kScopeChans)
        {
            /* the window must leave room for BOTH tiers after the crossing — a cv window longer
             * than the audio ring cannot be aligned, so ask only for the audio span then (cv detaches) */
            int need = r.SpanA(); if(r.SpanC() * kScopeCvDiv > need) need = r.SpanC() * kScopeCvDiv;
            if(need > kScopeUsable) need = r.SpanA() > 0 ? r.SpanA() : kScopeUsable;
            if(need < 1) need = 1;
            /* an audio trigger gives up after span + max(span, kScopeTrigHold): when the signal stops, the
             * window free-runs on what is there NOW instead of standing on a crossing up to 1.4 s old */
            int look = need + (need > kScopeTrigHold ? need : kScopeTrigHold); if(look > kScopeUsable) look = kScopeUsable;
            const uint32_t c = ScopeCrossing(audio[src], kScopeRing, look, kScopeWrap, wpos, need, r.level, kScopeHyst, found);
            if(found) return ScopePlace(wpos, c, r, kScopeTriggered);
        }
        else if(src >= kScopeChans)
        {
            int need = r.SpanC(); const int fromA = (r.SpanA() + kScopeCvDiv - 1) / kScopeCvDiv; if(fromA > need) need = fromA;
            if(need > kScopeCvUsable) need = kScopeCvUsable;
            if(need < 1) need = 1;
            const uint32_t c = ScopeCrossing(cv[src - kScopeChans], kScopeCvRing, kScopeCvUsable, kScopeCvWrap, cvW, need, r.level, kScopeHyst, found);
            if(found) return ScopePlace(wpos, c * (uint32_t)kScopeCvDiv, r, kScopeTriggered);
        }
    }
    return ScopePlace(wpos, newest, r, 0u);
}

/* CONTROL: n points from a ring starting at `start` (the tier's own clock),
 * one per `decim` source samples. decim 1: the samples, n bytes. Otherwise
 * each point is its group's minimum then its maximum, 2n bytes — the
 * envelope, so a click, a hat or a 6 ms gate is never between picks and
 * nothing aliases. Returns the bytes written (`out` holds 2 · kScopeMaxN).
 * `ringLen` and `wrap` are the tier's ring size and clock wrap. */
inline int ScopeExtract(const int8_t* ring, int ringLen, uint32_t wrap, uint32_t start, int n, int decim, int8_t* out)
{
    const uint32_t m = (uint32_t)(ringLen - 1);
    uint32_t p = start;
    if(decim == 1)
    {
        for(int i = 0; i < n; i++) { out[i] = ring[p & m]; p = ScopeNext(p, wrap); }
        return n;
    }
    for(int i = 0; i < n; i++)
    {
        int8_t lo = ring[p & m], hi = lo; p = ScopeNext(p, wrap);
        for(int k = 1; k < decim; k++)
        {
            const int8_t v = ring[p & m];
            if(v < lo) lo = v; else if(v > hi) hi = v;
            p = ScopeNext(p, wrap);
        }
        out[2 * i] = lo; out[2 * i + 1] = hi;
    }
    return 2 * n;
}

/* CONTROL: the reply's four audio levels — peak |x| of each audio channel
 * over the newest kScopeLevelSpan samples, selected or not, so the page
 * knows an input carries a signal without fetching its trace. */
inline void ScopeLevels(const int8_t* const audio[kScopeChans], uint32_t wpos, uint8_t out[kScopeChans])
{
    const uint32_t start = ScopeSub(wpos, (uint32_t)kScopeLevelSpan);
    for(int c = 0; c < kScopeChans; c++)
    {
        int pk = 0; uint32_t p = start;
        for(int i = 0; i < kScopeLevelSpan; i++)
        {
            const int v = audio[c][ScopeSlot(p)]; const int a = v < 0 ? -v : v;
            if(a > pk) pk = a;
            p = ScopeNext(p);
        }
        out[c] = (uint8_t)(pk > 127 ? 127 : pk);
    }
}

/* CONTROL: the pre-SCOPE reply's samples — (L + R) / 2, one sample in four
 * over the newest 1024, ending on the last multiple of four before the
 * snapshot so the picks fall where the old ×4 writer put them. */
inline void ScopeExtractOld(const int8_t* L, const int8_t* R, uint32_t wpos, int8_t* out256)
{
    uint32_t p = ScopeSub(ScopePrev(wpos) & ~3u, 1020u);
    for(int i = 0; i < 256; i++)
    {
        const int s = ScopeSlot(p);
        out256[i] = (int8_t)(((int)L[s] + (int)R[s]) / 2);
        p = ScopeAdd(p, 4u);
    }
}

} // namespace bb
