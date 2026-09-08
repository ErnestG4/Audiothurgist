#pragma once
// SCOPE: the four-channel oscilloscope behind HostLink 0x62, pure so the
// host suite can test it (tests/scope_check.cpp) — docs/packages/scope.md.
//
// The shell keeps four int8 rings (out L, out R, in L, in R) of kScopeRing
// samples, written at full rate by the AUDIO callback under ONE shared write
// index (so the channels are sample-aligned by construction), and answers
// 0x62 on the CONTROL thread from a single snapshot of that index. Nothing
// here touches hardware or allocates; the ring memory belongs to the shell.
//
// Ring positions are absolute sample counts (uint32, wrapping — kScopeRing
// divides 2^32, so `pos & (kScopeRing - 1)` is the slot). At a snapshot
// `wpos` the ring holds [wpos - kScopeRing, wpos); the audio thread keeps
// writing while the reply is built, so the oldest kScopeGuard samples are
// never read (≈ 5 ms — longer than a HostLink handler takes).
#include <cstdint>

namespace bb {

constexpr int     kScopeRing     = 4096;   // samples per channel (≈ 85 ms at 48 kHz)
constexpr int     kScopeGuard    = 256;    // oldest samples left alone (the writer is live)
constexpr int     kScopeChans    = 4;      // out L · out R · in L · in R
constexpr int     kScopeMaxN     = 512;    // samples per channel per reply
constexpr int     kScopeMaxDecim = 16;
constexpr int     kScopeHdr      = 10;     // reply: status mask decim n(2) flags pos(4)
constexpr int     kScopeReqMin   = 5;      // request: mask decim n(2) flags
constexpr int     kScopeReqPos   = 9;      // … + u32 pos (with kScopeAt)
constexpr int8_t  kScopeHyst     = 4;      // trigger hysteresis, int8 units (≈ 3 %)
constexpr int     kScopeUsable   = kScopeRing - kScopeGuard;   // the span a window may cover

/* Request flags. */
enum : uint8_t {
    kScopeTrig = 1,   // align the window to the most recent rising zero crossing of the first selected channel
    kScopePeak = 2,   // each output sample = the max-magnitude sample of its decim group (else the first)
    kScopeAt   = 4,   // the request carries u32 pos: start there (a second request of the same frame)
};
/* Reply flags. */
enum : uint8_t {
    kScopeTriggered = 1,   // the window starts on a crossing
    kScopeAtHonoured = 2,  // the window starts at the requested pos
};

inline int8_t ScopeI8(float v)
{
    if(v > 1.f) v = 1.f; else if(v < -1.f) v = -1.f;
    return (int8_t)(v * 127.f);
}
inline int  ScopeSlot(uint32_t pos) { return (int)(pos & (uint32_t)(kScopeRing - 1)); }
inline int  ScopeChanCount(uint8_t mask) { int c = 0; for(int i = 0; i < kScopeChans; i++) if(mask & (1u << i)) c++; return c; }
inline int  ScopeFirstChan(uint8_t mask) { for(int i = 0; i < kScopeChans; i++) if(mask & (1u << i)) return i; return -1; }

struct ScopeReq
{
    uint8_t  mask  = 0;
    uint8_t  decim = 1;
    uint16_t n     = 0;
    uint8_t  flags = 0;
    uint32_t pos   = 0;
};

/* CONTROL: parse and clamp a 0x62 request body (len ≥ 1; the empty body is
 * the pre-SCOPE reply and is handled by the caller). false = BAD_ARGS:
 * short body, mask 0 or with bits above the four channels, decim 0 or > 16,
 * n 0. n is then clamped to kScopeMaxN, to what fits `maxBody` beside the
 * header, and to the usable ring span at this decimation — the reply says
 * what it got. */
inline bool ScopeParse(const uint8_t* body, int len, int maxBody, ScopeReq& r)
{
    if(len < kScopeReqMin) return false;
    r.mask = body[0]; r.decim = body[1];
    r.n = (uint16_t)(body[2] | (body[3] << 8)); r.flags = body[4];
    if(r.mask == 0u || (r.mask & ~((1u << kScopeChans) - 1u)) != 0u) return false;
    if(r.decim == 0u || r.decim > kScopeMaxDecim || r.n == 0u) return false;
    if((r.flags & kScopeAt) != 0u)
    {
        if(len < kScopeReqPos) return false;
        r.pos = (uint32_t)body[5] | ((uint32_t)body[6] << 8) | ((uint32_t)body[7] << 16) | ((uint32_t)body[8] << 24);
    }
    int n = r.n;
    if(n > kScopeMaxN) n = kScopeMaxN;
    const int fit = (maxBody - kScopeHdr) / ScopeChanCount(r.mask);
    if(n > fit) n = fit;
    const int span = kScopeUsable / r.decim;
    if(n > span) n = span;
    if(n < 1) n = 1;
    r.n = (uint16_t)n;
    return true;
}

/* CONTROL: the most recent rising zero crossing on `ring` that leaves at
 * least `span` samples after it, or 0 with `found` false. A crossing is the
 * sample where the signal goes from < 0 to ≥ 0 after having been ≤ -hyst
 * and before reaching ≥ +hyst without dipping below 0 again — a Schmitt
 * trigger walked forward over the usable ring, so noise around zero does
 * not trigger and the crossing reported is the one that led to the peak.
 * Search cost: one pass over kScopeUsable samples. */
inline uint32_t ScopeCrossing(const int8_t* ring, uint32_t wpos, int span, int8_t hyst, bool& found)
{
    found = false;
    if(span < 1 || span > kScopeUsable) return 0u;
    const uint32_t oldest = wpos - (uint32_t)kScopeUsable;   // first readable sample
    const uint32_t latest = wpos - (uint32_t)span;           // last start that leaves `span`
    uint32_t best = 0u;
    bool armed = false, pending = false; uint32_t cand = 0u;
    int8_t prev = ring[ScopeSlot(oldest)];
    for(uint32_t p = oldest + 1u; p != wpos; p++)
    {
        const int8_t v = ring[ScopeSlot(p)];
        if(v <= -hyst) { armed = true; pending = false; }
        if(pending)
        {
            if(v < 0) pending = false;                          // fell back: not this one
            else if(v >= hyst)                                  // confirmed
            {
                pending = false; armed = false;
                if((int32_t)(cand - latest) <= 0) { best = cand; found = true; }
            }
        }
        else if(armed && prev < 0 && v >= 0) { pending = true; cand = p; }
        prev = v;
    }
    return best;
}

/* CONTROL: where the window starts for this request, given the snapshot.
 * kScopeAt: the requested pos if it is still inside the usable ring and
 * leaves `span` samples (flags bit kScopeAtHonoured), else the newest
 * window. kScopeTrig (without a pos): the crossing above (kScopeTriggered),
 * else the newest window. */
inline uint32_t ScopeWindow(const int8_t* trigRing, uint32_t wpos, int span, const ScopeReq& r, uint8_t& replyFlags)
{
    replyFlags = 0u;
    const uint32_t newest = wpos - (uint32_t)span;
    if(r.flags & kScopeAt)
    {
        const int32_t age = (int32_t)(wpos - r.pos);   // samples between pos and now
        if(age >= span && age <= kScopeUsable) { replyFlags = kScopeAtHonoured; return r.pos; }
        return newest;
    }
    if(r.flags & kScopeTrig)
    {
        bool found = false;
        const uint32_t c = ScopeCrossing(trigRing, wpos, span, kScopeHyst, found);
        if(found) { replyFlags = kScopeTriggered; return c; }
    }
    return newest;
}

/* CONTROL: n output samples from `ring` starting at absolute `start`, one
 * per `decim` source samples: the first of the group, or with `peak` the
 * one of largest magnitude (so a click never falls between picks). */
inline void ScopeExtract(const int8_t* ring, uint32_t start, int n, int decim, bool peak, int8_t* out)
{
    if(!peak || decim == 1)
    {
        for(int i = 0; i < n; i++) out[i] = ring[ScopeSlot(start + (uint32_t)(i * decim))];
        return;
    }
    for(int i = 0; i < n; i++)
    {
        const uint32_t a = start + (uint32_t)(i * decim);
        int8_t best = 0; int mag = -1;
        for(int k = 0; k < decim; k++)
        {
            const int8_t v = ring[ScopeSlot(a + (uint32_t)k)];
            const int m = v < 0 ? -(int)v : (int)v;
            if(m > mag) { mag = m; best = v; }
        }
        out[i] = best;
    }
}

/* CONTROL: the pre-SCOPE reply's samples — (L + R) / 2, one sample in four
 * over the newest 1024, ending on the last multiple of four before the
 * snapshot so the picks fall where the old ×4 writer put them. */
inline void ScopeExtractOld(const int8_t* L, const int8_t* R, uint32_t wpos, int8_t* out256)
{
    const uint32_t start = ((wpos - 1u) & ~3u) - 1020u;
    for(int i = 0; i < 256; i++)
    {
        const int s = ScopeSlot(start + (uint32_t)(i * 4));
        out256[i] = (int8_t)(((int)L[s] + (int)R[s]) / 2);
    }
}

} // namespace bb
