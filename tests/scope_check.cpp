// scope_check — host tests for core/bb_scope.h (SCOPE package). Build + run:
//   tests/run.sh
// or by hand:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore tests/scope_check.cpp -o /tmp/sc && /tmp/sc
//
// A miniature of the shell: four audio rings and six CV rings under one
// sample clock, fed by a fake audio thread block by block (24 samples, one CV
// sample per block), then read the way the 0x62 handler reads them — parse,
// window (trigger / at-pos / newest, aligned or detached), extract per
// channel — and the pre-SCOPE (empty body) reply beside it.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "bb_scope.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

using namespace bb;
static const float kSr = 48000.f;

struct Rings
{
    int8_t   a[kScopeChans][kScopeRing];
    int8_t   c[kScopeCvChans][kScopeCvRing];
    uint32_t w = 0;              // the published index: always a multiple of the block
    uint32_t pending = 0;        // samples written in the current block
    Rings() { memset(a, 0, sizeof a); memset(c, 0, sizeof c); }
    /* the audio thread: one sample of each audio channel; the CV channels are sampled at the block's start */
    void Push(float oL, float oR, float iL, float iR, const float* volts = nullptr)
    {
        const uint32_t p = ScopeAdd(w, pending);
        if(pending == 0u)
        {
            const int cs = ScopeCvSlot(ScopeCvPos(p));
            for(int k = 0; k < kScopeCvChans; k++) c[k][cs] = volts ? ScopeCvCode(RawOf(volts[k]), 32768u, ScopeCvScale(kScopeCvVdda)) : 0;
        }
        const int s = ScopeSlot(p);
        a[0][s] = ScopeI8(oL); a[1][s] = ScopeI8(oR); a[2][s] = ScopeI8(iL); a[3][s] = ScopeI8(iR);
        if(++pending == (uint32_t)kScopeCvDiv) { w = ScopeAdd(w, pending); pending = 0u; }
    }
    /* a jack voltage → the raw ADC code the design transfer gives (inverting stage, mid-scale idle) */
    static uint16_t RawOf(float v) { float r = 32768.f - v * 65535.f * kScopeCvGain / kScopeCvVdda; if(r < 0.f) r = 0.f; if(r > 65535.f) r = 65535.f; return (uint16_t)(r + 0.5f); }
};
static Rings* NewRings() { Rings* r = new Rings(); return r; }

/* The 0x62 handler, as the shell runs it: one snapshot, header, ext header, channels in mask order. */
static int Handle(const Rings& R, const uint8_t* body, int len, int maxBody, uint8_t* out)
{
    const uint32_t wpos = R.w;
    if(len == 0)
    {
        out[0] = 0; out[1] = 0; out[2] = 1;   /* status, u16 256 */
        ScopeExtractOld(R.a[0], R.a[1], wpos, (int8_t*)out + 3);
        return 259;
    }
    ScopeReq q;
    if(!ScopeParse(body, len, maxBody, q)) { out[0] = 2; return 1; }
    const int8_t* A[kScopeChans] = { R.a[0], R.a[1], R.a[2], R.a[3] };
    const int8_t* C[kScopeCvChans] = { R.c[0], R.c[1], R.c[2], R.c[3], R.c[4], R.c[5] };
    const ScopeWin win = ScopeWindow(A, C, wpos, q);
    const bool ext = (q.flags & kScopeExt) != 0;
    int o = 0;
    out[o++] = 0; out[o++] = q.mask; out[o++] = q.decim; out[o++] = (uint8_t)q.n; out[o++] = (uint8_t)(q.n >> 8); out[o++] = (uint8_t)(win.flags | (ext ? kScopeExtReply : 0));
    out[o++] = (uint8_t)win.pos; out[o++] = (uint8_t)(win.pos >> 8); out[o++] = (uint8_t)(win.pos >> 16); out[o++] = (uint8_t)(win.pos >> 24);
    if(ext) { out[o++] = q.maskHi; out[o++] = q.cvDecim; out[o++] = (uint8_t)kScopeCvDiv; const int ts = ScopeTrigSource(q); out[o++] = (uint8_t)((q.flags & kScopeTrig) && !(q.flags & kScopeAt) && ts >= 0 ? ts : kScopeTrigNone); }
    for(int ch = 0; ch < kScopeChans; ch++)
        if(q.mask & (1u << ch)) { ScopeExtract(R.a[ch], kScopeRing, kScopeWrap, win.pos, q.n, q.decim, (q.flags & kScopePeak) != 0, (int8_t*)out + o); o += q.n; }
    for(int ch = 0; ch < kScopeCvChans; ch++)
        if(q.maskHi & (1u << ch)) { ScopeExtract(R.c[ch], kScopeCvRing, kScopeCvWrap, win.cvPos, q.n, q.cvDecim, (q.flags & kScopePeak) != 0, (int8_t*)out + o); o += q.n; }
    return o;
}
static uint32_t U32(const uint8_t* b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static int Req(uint8_t* b, uint8_t mask, uint8_t decim, uint16_t n, uint8_t flags, uint32_t pos = 0, uint8_t maskHi = 0, uint8_t cvDecim = 1, int8_t level = 0, uint8_t trig = 0)
{
    b[0] = mask; b[1] = decim; b[2] = (uint8_t)n; b[3] = (uint8_t)(n >> 8); b[4] = flags;
    b[5] = (uint8_t)pos; b[6] = (uint8_t)(pos >> 8); b[7] = (uint8_t)(pos >> 16); b[8] = (uint8_t)(pos >> 24);
    b[9] = maskHi; b[10] = cvDecim; b[11] = (uint8_t)level; b[12] = trig;
    return (flags & kScopeExt) ? 13 : (flags & kScopeAt) ? 9 : 5;
}
/* the forward Schmitt walk — the reference the backward search must agree with */
static uint32_t ForwardCrossing(const int8_t* ring, int ringLen, int usable, uint32_t wpos, int span, int level, int hyst, bool& found)
{
    found = false; const uint32_t m = (uint32_t)(ringLen - 1);
    const uint32_t oldest = ScopeSub(wpos, (uint32_t)usable), latest = ScopeSub(wpos, (uint32_t)span);
    uint32_t best = 0u; bool armed = false, pending = false; uint32_t cand = 0u;
    int prev = ring[oldest & m];
    for(uint32_t p = ScopeNext(oldest); p != wpos; p = ScopeNext(p))
    {
        const int v = ring[p & m];
        if(v <= level - hyst) { armed = true; pending = false; }
        if(pending) { if(v < level) pending = false; else if(v >= level + hyst) { pending = false; armed = false; if(ScopeAge(latest, cand) < (uint32_t)usable) { best = cand; found = true; } } }
        else if(armed && prev < level && v >= level)
        {
            pending = true; cand = p;
            if(v >= level + hyst) { pending = false; armed = false; if(ScopeAge(latest, cand) < (uint32_t)usable) { best = cand; found = true; } }   /* a rise that reaches +hyst at once */
        }
        prev = v;
    }
    return best;
}

int main()
{
    static uint8_t out[8192]; uint8_t body[13];
    const float kPi = 3.14159265f;

    SECTION("parse + clamps");
    {
        ScopeReq q; int L;
        L = Req(body, 0x0f, 4, 512, 0);
        CHECK(ScopeParse(body, L, 2048, q) && q.n == 509 && q.mask == 0x0f && q.decim == 4, "4 ch at 2048 → 509 (got %d)", q.n);
        CHECK(ScopeParse(body, L, 1024, q) && q.n == 253, "4 ch at 1024 → 253 (got %d)", q.n);
        CHECK(ScopeParse(body, L, 512, q) && q.n == 125, "4 ch at 512 → 125 (got %d)", q.n);
        L = Req(body, 0x03, 4, 512, 0);
        CHECK(ScopeParse(body, L, 2048, q) && q.n == 512, "2 ch at 2048 → 512 (got %d)", q.n);
        L = Req(body, 0x01, 16, 9999, 0);
        CHECK(ScopeParse(body, L, 2048, q) && q.n == 512, "×16 × 512 fits the 64 K ring: 512 (got %d)", q.n);
        L = Req(body, 0x00, 4, 256, 0); CHECK(!ScopeParse(body, L, 2048, q), "mask 0 → BAD_ARGS");
        L = Req(body, 0x10, 4, 256, 0); CHECK(!ScopeParse(body, L, 2048, q), "mask bit 4 → BAD_ARGS");
        L = Req(body, 0x01, 0, 256, 0); CHECK(!ScopeParse(body, L, 2048, q), "decim 0 → BAD_ARGS");
        L = Req(body, 0x01, 17, 256, 0); CHECK(!ScopeParse(body, L, 2048, q), "decim 17 → BAD_ARGS");
        L = Req(body, 0x01, 4, 0, 0); CHECK(!ScopeParse(body, L, 2048, q), "n 0 → BAD_ARGS");
        L = Req(body, 0x01, 4, 256, 0); CHECK(!ScopeParse(body, 4, 2048, q), "short body → BAD_ARGS");
        L = Req(body, 0x01, 4, 256, kScopeAt, 1234); CHECK(!ScopeParse(body, 5, 2048, q), "kScopeAt without pos → BAD_ARGS");
        CHECK(ScopeParse(body, L, 2048, q) && q.pos == 1234u, "kScopeAt carries pos");
        L = Req(body, 0x01, 4, 256, kScopeAt, kScopeWrap); CHECK(!ScopeParse(body, L, 2048, q), "pos past the wrap → BAD_ARGS");
        /* the ext form: CV jacks, their decimation, the trigger level */
        L = Req(body, 0x01, 4, 512, kScopeExt, 0, 0x3f, 8, 25);
        CHECK(ScopeParse(body, L, 1024, q) && q.maskHi == 0x3f && q.cvDecim == 8 && q.level == 25 && q.Chans() == 7 && q.Hdr() == 14 && q.n == (1024 - 14) / 7, "ext: 1 + 6 ch at 1024 → %d", q.n);
        CHECK(!ScopeParse(body, 12, 1024, q), "ext with a short body → BAD_ARGS");
        L = Req(body, 0x01, 4, 512, kScopeExt, 0, 0x08, 1, 25, kScopeTrigSet | 7);
        CHECK(ScopeParse(body, L, 1024, q) && q.trig == (kScopeTrigSet | 7) && ScopeTrigSource(q) == 7, "ext: explicit trigger source J6 (7)");
        L = Req(body, 0x01, 4, 512, kScopeExt, 0, 0x08, 1, 25, kScopeTrigSet | 10); CHECK(!ScopeParse(body, L, 1024, q), "trigger source 10 → BAD_ARGS");
        L = Req(body, 0x01, 4, 512, kScopeExt, 0, 0x08, 1, 25, 3); CHECK(!ScopeParse(body, L, 1024, q), "trigger source without the set bit → BAD_ARGS");
        L = Req(body, 0x0a, 4, 512, kScopeExt, 0, 0x08, 1, 25, 0); CHECK(ScopeParse(body, L, 1024, q) && ScopeTrigSource(q) == 1, "auto trigger source = first selected audio channel");
        L = Req(body, 0x00, 4, 512, kScopeExt, 0, 0x28, 1, 25, 0); CHECK(ScopeParse(body, L, 1024, q) && ScopeTrigSource(q) == 7, "auto trigger source = first selected cv when no audio");
        L = Req(body, 0x00, 4, 512, kScopeExt, 0, 0x08, 64, 0);
        CHECK(ScopeParse(body, L, 1024, q) && q.mask == 0 && q.n == kScopeCvUsable / 64 && q.SpanA() == 0 && q.SpanC() == q.n * 64, "cv only at ×64: held to the cv ring (%d)", q.n);
        L = Req(body, 0x00, 4, 512, kScopeExt, 0, 0x00, 1, 0); CHECK(!ScopeParse(body, L, 1024, q), "ext with no channel at all → BAD_ARGS");
        L = Req(body, 0x00, 4, 512, kScopeExt, 0, 0x40, 1, 0); CHECK(!ScopeParse(body, L, 1024, q), "maskHi bit 6 → BAD_ARGS");
        L = Req(body, 0x00, 4, 512, kScopeExt, 0, 0x01, 0, 0); CHECK(!ScopeParse(body, L, 1024, q), "cvDecim 0 → BAD_ARGS");
        L = Req(body, 0x00, 4, 512, kScopeExt, 0, 0x01, 65, 0); CHECK(!ScopeParse(body, L, 1024, q), "cvDecim 65 → BAD_ARGS");
        L = Req(body, 0x01, 4, 512, 0); CHECK(ScopeParse(body, L, 1024, q) && q.maskHi == 0 && q.cvDecim == 1 && q.level == 0 && q.Hdr() == 10, "the 5-byte form leaves the ext fields at their defaults");
        CHECK(ScopeChanCount(0x0f) == 4 && ScopeChanCount(0x3f) == 6 && ScopeChanCount(0x0a) == 2 && ScopeFirstChan(0x0a) == 1 && ScopeFirstChan(0x08) == 3 && ScopeFirstChan(0) == -1, "mask helpers");
        CHECK(ScopeI8(1.5f) == 127 && ScopeI8(-1.5f) == -127 && ScopeI8(0.5f) == 63 && ScopeI8(0.f) == 0, "float → i8 saturates");
        /* the clock: wrap arithmetic */
        CHECK(ScopeNext(kScopeWrap - 1u) == 0u && ScopePrev(0u) == kScopeWrap - 1u && ScopeAdd(kScopeWrap - 2u, 5u) == 3u && ScopeSub(3u, 5u) == kScopeWrap - 2u && ScopeAge(3u, kScopeWrap - 2u) == 5u, "positions wrap at kScopeWrap");
        CHECK(kScopeCvWrap * 24u == kScopeWrap && ScopeAge(3u, kScopeCvWrap - 2u, kScopeCvWrap) == 5u && ScopeCvPos(kScopeWrap - 1u) == kScopeCvWrap - 1u, "the cv clock wraps at kScopeWrap / 24");
        CHECK(kScopeWrap % 24u == 0u && kScopeWrap % 65536u == 0u && (kScopeWrap / 24u) % 16384u == 0u, "the wrap is a multiple of the block and of both rings");
    }

    SECTION("cv transform: volts ↔ raw ↔ int8");
    {
        const float sc = ScopeCvScale(kScopeCvVdda);
        CHECK(ScopeCvCode(32768u, 32768u, sc) == 0, "idle code → 0 V");
        CHECK(ScopeCvCode(Rings::RawOf(5.f), 32768u, sc) == 127, "+5 V → 127 (got %d)", ScopeCvCode(Rings::RawOf(5.f), 32768u, sc));
        CHECK(ScopeCvCode(Rings::RawOf(-5.f), 32768u, sc) == -127, "−5 V → −127");
        CHECK(ScopeCvCode(Rings::RawOf(1.f), 32768u, sc) == 25, "+1 V → 25 (got %d)", ScopeCvCode(Rings::RawOf(1.f), 32768u, sc));
        CHECK(ScopeCvCode(Rings::RawOf(8.f), 32768u, sc) == 127 && ScopeCvCode(0u, 32768u, sc) == 127 && ScopeCvCode(65535u, 32768u, sc) == -127, "beyond ±5 V clips (the ADC reaches ±10 V)");
        CHECK(ScopeCvCode(16384u, 32768u, sc) == 127, "raw 16384 is +5 V by design (gain 0.165, VDDA 3.30)");
        /* higher raw = more negative: the input stage inverts */
        CHECK(ScopeCvCode(40000u, 32768u, sc) < 0 && ScopeCvCode(20000u, 32768u, sc) > 0, "polarity: higher raw reads negative");
        /* calibration: a jack idling at 33000 with VDDA 3.25 reads 0 V there */
        const float sc2 = ScopeCvScale(3.25f);
        CHECK(ScopeCvCode(33000u, 33000u, sc2) == 0 && ScopeCvCode(33000u, 32768u, sc) == -2, "the zero code moves the zero; the design fallback would read −0.07 V (%d)", ScopeCvCode(33000u, 32768u, sc));
        CHECK(fabsf(ScopeCvVolts(127) - 5.f) < 0.01f && fabsf(ScopeCvVolts(25) - 0.984f) < 0.01f, "int8 → volts");
    }

    SECTION("trigger finds the crossing; channels are aligned to one index");
    {
        Rings* Rp = NewRings(); Rings& R = *Rp;
        /* 2 s of 100 Hz on out L, its inverse on out R, a 440 Hz on in L, a ramp on in R (all distinct) */
        for(int i = 0; i < 96000; i++)
        {
            const float t = i / kSr;
            const float oL = 0.8f * sinf(2 * kPi * 100.f * t);
            R.Push(oL, -oL, 0.5f * sinf(2 * kPi * 440.f * t), (float)(i % 480) / 480.f - 0.5f);
        }
        int L = Req(body, 0x0f, 1, 512, kScopeTrig);
        const int len = Handle(R, body, L, 2048, out);
        CHECK(len == 10 + 4 * 509, "4 ch × 509 + header = %d", len);
        CHECK(out[0] == 0 && out[1] == 0x0f && out[2] == 1 && (out[3] | (out[4] << 8)) == 509, "header echoes the clamped request");
        CHECK(out[5] & kScopeTriggered, "triggered flag");
        const uint32_t pos = U32(out + 6);
        const int8_t* oL = (const int8_t*)out + 10;
        const int8_t* oR = oL + 509; const int8_t* iL = oR + 509; const int8_t* iR = iL + 509;
        CHECK(oL[0] >= 0 && R.a[0][ScopeSlot(pos - 1)] < 0, "starts at a rising crossing: prev %d, first %d", R.a[0][ScopeSlot(pos - 1)], oL[0]);
        CHECK(pos % 480 == 0, "crossing on the 100 Hz grid: pos %u", pos);
        CHECK(ScopeAge(R.w, pos) >= 512, "window fits: %u after", ScopeAge(R.w, pos));
        bool later = false; for(uint32_t p = pos + 480; ScopeAge(R.w, p) >= 512 && p < R.w; p += 480) later = true;
        CHECK(!later, "the most recent crossing that fits");
        bool aligned = true; for(int i = 0; i < 509; i++) if(oR[i] != -oL[i]) { aligned = false; break; }
        CHECK(aligned, "out R == -out L across the window");
        bool ramp = true; for(int i = 0; i < 509; i += 7) { const int want = ScopeI8((float)((pos + i) % 480) / 480.f - 0.5f); if(iR[i] != want) { ramp = false; break; } }
        CHECK(ramp, "in R ramp aligned to pos");
        CHECK(iL[100] != oL[100] || iL[200] != oL[200], "in L is its own signal");
        L = Req(body, 0x01, 1, 512, 0);
        Handle(R, body, L, 2048, out);
        CHECK(!(out[5] & kScopeTriggered) && U32(out + 6) == R.w - 512, "untriggered → newest window");
        /* first selected channel decides: mask 0x08 (in R, the ramp) triggers on the ramp's rise through zero */
        L = Req(body, 0x08, 1, 512, kScopeTrig);
        Handle(R, body, L, 2048, out);
        const uint32_t p2 = U32(out + 6);
        int rampX = 0; for(int k = 1; k < 480; k++) if(ScopeI8((float)(k - 1) / 480.f - 0.5f) < 0 && ScopeI8((float)k / 480.f - 0.5f) >= 0) rampX = k;
        CHECK((out[5] & kScopeTriggered) && (int)(p2 % 480) == rampX, "trigger follows the first selected channel: pos %u (want %%480 == %d)", p2, rampX);
        /* a long window: ×16 × 512 = 8192 samples, still triggered on the grid */
        L = Req(body, 0x01, 16, 512, kScopeTrig);
        Handle(R, body, L, 2048, out);
        CHECK((out[5] & kScopeTriggered) && U32(out + 6) % 480 == 0 && ScopeAge(R.w, U32(out + 6)) >= 8192 && (out[3] | (out[4] << 8)) == 512, "×16 × 512 triggered (pos %u)", U32(out + 6));
        delete Rp;
    }

    SECTION("hysteresis: noise around zero does not trigger; silence is untriggered; the walk is linear");
    {
        Rings* Rp = NewRings(); Rings& R = *Rp;
        for(int i = 0; i < 20000; i++) { const float n = ((i * 7919) % 13 - 6) / 127.f * 0.5f; R.Push(n, n, 0, 0); }   /* ±3 int8 units */
        int L = Req(body, 0x01, 1, 256, kScopeTrig);
        Handle(R, body, L, 2048, out);
        CHECK(!(out[5] & kScopeTriggered) && U32(out + 6) == R.w - 256, "±3 noise (< hyst 4) leaves the scope free-running");
        Rings* Sp = NewRings(); Rings& S = *Sp;
        for(int i = 0; i < 20000; i++) S.Push(0, 0, 0, 0);
        Handle(S, body, L, 2048, out);
        CHECK(!(out[5] & kScopeTriggered), "silence is untriggered");
        delete Sp;
        /* one clean pulse far enough back: a crossing that leaves the span */
        Rings* Pp = NewRings(); Rings& P = *Pp;
        for(int i = 0; i < 6000; i++) { float v = 0; if(i >= 3000 && i < 3040) v = -0.5f; if(i >= 3040 && i < 3080) v = 0.5f; P.Push(v, 0, 0, 0); }
        Handle(P, body, L, 2048, out);
        CHECK((out[5] & kScopeTriggered) && U32(out + 6) == 3040u, "a single pulse triggers at its rise (pos %u)", U32(out + 6));
        delete Pp;
        /* a crossing too recent to leave the span is skipped in favour of an older one */
        Rings* Qp = NewRings(); Rings& Q = *Qp;
        for(int i = 0; i < 6000; i++) { float v = 0; if((i >= 3000 && i < 3040) || (i >= 5900 && i < 5940)) v = -0.5f; if((i >= 3040 && i < 3080) || (i >= 5940 && i < 5980)) v = 0.5f; Q.Push(v, 0, 0, 0); }
        Handle(Q, body, L, 2048, out);
        CHECK((out[5] & kScopeTriggered) && U32(out + 6) == 3040u, "the crossing that fits wins over the newer one (pos %u)", U32(out + 6));
        delete Qp;
        /* …and one that has fallen out of the usable ring is gone */
        Rings* Op = NewRings(); Rings& O = *Op;
        for(int i = 0; i < 70000; i++) { float v = 0; if(i >= 2000 && i < 2040) v = -0.5f; if(i >= 2040 && i < 2080) v = 0.5f; O.Push(v, 0, 0, 0); }
        Handle(O, body, L, 2048, out);
        CHECK(!(out[5] & kScopeTriggered), "a crossing older than the usable ring is not used");
        delete Op;
        /* backward == forward on random-ish signals, at both levels, with the same hysteresis */
        Rings* Xp = NewRings(); Rings& X = *Xp; uint32_t seed = 12345u; int agree = 0, trials = 0, foundN = 0;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.f * 2.f - 1.f; };
        float ph = 0.f, f = 30.f;
        for(int i = 0; i < 200000; i++) { if((i % 5000) == 0) f = 20.f + 400.f * (rnd() * 0.5f + 0.5f); ph += 2 * kPi * f / kSr; const float v = 0.6f * sinf(ph) + 0.15f * rnd() + ((i / 7000) % 3 == 0 ? 0.3f : 0.f); X.Push(v, 0, 0, 0);
            if(i > 70000 && (i % 997) == 0)
            {
                for(int lv = 0; lv < 2; lv++) { for(int sp = 100; sp <= 8192; sp *= 4) { bool fb = false, ff = false; const int level = lv ? 25 : 0;
                    const uint32_t b = ScopeCrossing(X.a[0], kScopeRing, kScopeUsable, kScopeWrap, X.w, sp, level, kScopeHyst, fb);
                    const uint32_t fw = ForwardCrossing(X.a[0], kScopeRing, kScopeUsable, X.w, sp, level, kScopeHyst, ff);
                    trials++; if(fb == ff && (!fb || b == fw)) agree++; if(fb) foundN++; } }
            } }
        CHECK(agree == trials && foundN > trials / 2, "backward search agrees with the forward Schmitt walk: %d/%d (found %d)", agree, trials, foundN);
        delete Xp;
        delete Rp;
    }

    SECTION("decimation: plain pick and peak");
    {
        Rings* Rp = NewRings(); Rings& R = *Rp;
        for(int i = 0; i < 8192; i++) { float v = 0; if(i % 64 == 37) v = 0.9f; if(i % 64 == 38) v = -0.3f; R.Push(v, (float)((i % 128) - 64) / 128.f, 0, 0); }
        int L = Req(body, 0x01, 16, 100, 0);
        Handle(R, body, L, 2048, out);
        const uint32_t start = U32(out + 6); const int8_t* s = (const int8_t*)out + 10;
        bool plainOk = true; for(int i = 0; i < 100; i++) if(s[i] != R.a[0][ScopeSlot(start + i * 16)]) plainOk = false;
        CHECK(plainOk, "plain pick = the first sample of each group");
        L = Req(body, 0x01, 16, 100, kScopePeak);
        Handle(R, body, L, 2048, out);
        int clicks = 0; bool signOk = true;
        for(int i = 0; i < 100; i++) { if(s[i] != 0) { clicks++; if(s[i] != ScopeI8(0.9f)) signOk = false; } }
        CHECK(clicks == 25 && signOk, "peak keeps the click in every 4th group: %d clicks", clicks);
        L = Req(body, 0x02, 8, 200, kScopePeak);
        Handle(R, body, L, 2048, out);
        const uint32_t st2 = U32(out + 6); bool pk = true;
        for(int i = 0; i < 200 && pk; i++) { int best = 0, mag = -1; for(int k = 0; k < 8; k++) { const int v = R.a[1][ScopeSlot(st2 + i * 8 + k)]; const int m = v < 0 ? -v : v; if(m > mag) { mag = m; best = v; } } if(s[i] != best) pk = false; }
        CHECK(pk, "peak = max-magnitude sample of each group");
        L = Req(body, 0x02, 1, 300, kScopePeak);
        Handle(R, body, L, 2048, out);
        const uint32_t st3 = U32(out + 6); bool copy = true; for(int i = 0; i < 300; i++) if(s[i] != R.a[1][ScopeSlot(st3 + i)]) copy = false;
        CHECK(copy, "×1 peak = the samples");
        delete Rp;
    }

    SECTION("at-pos: a second request of the same frame lands on the same samples");
    {
        Rings* Rp = NewRings(); Rings& R = *Rp;
        for(int i = 0; i < 30000; i++) { const float t = i / kSr; R.Push(0.7f * sinf(2 * kPi * 60.f * t), 0.2f * sinf(2 * kPi * 1000.f * t), 0.3f * sinf(2 * kPi * 150.f * t), 0); }
        int L = Req(body, 0x01, 2, 500, kScopeTrig);
        Handle(R, body, L, 2048, out);
        const uint32_t pos = U32(out + 6); CHECK(out[5] & kScopeTriggered, "first request triggers");
        int8_t first[500]; memcpy(first, out + 10, 500);
        for(int i = 0; i < 240; i++) R.Push(0, 0, 0, 0);   /* the audio thread runs on ten blocks, as between two requests */
        L = Req(body, 0x0e, 2, 500, kScopeAt, pos);
        const int len = Handle(R, body, L, 2048, out);
        CHECK(len == 10 + 3 * 500 && (out[5] & kScopeAtHonoured) && !(out[5] & kScopeTriggered) && U32(out + 6) == pos, "second request honoured at pos (flags %u)", out[5]);
        int8_t ref[500]; ScopeExtract(R.a[1], kScopeRing, kScopeWrap, pos, 500, 2, false, ref);
        CHECK(memcmp(out + 10, ref, 500) == 0, "out R at the same pos");
        int8_t again[500]; ScopeExtract(R.a[0], kScopeRing, kScopeWrap, pos, 500, 2, false, again);
        CHECK(memcmp(again, first, 500) == 0, "out L unchanged at pos");
        L = Req(body, 0x01, 1, 256, kScopeAt, ScopeSub(R.w, 70000u));
        Handle(R, body, L, 2048, out);
        CHECK(!(out[5] & kScopeAtHonoured) && U32(out + 6) == R.w - 256, "pos too old → newest, not honoured");
        L = Req(body, 0x01, 1, 256, kScopeAt, R.w - 100u);
        Handle(R, body, L, 2048, out);
        CHECK(!(out[5] & kScopeAtHonoured) && U32(out + 6) == R.w - 256, "pos too recent → newest, not honoured");
        L = Req(body, 0x01, 1, 256, kScopeAt, R.w - 256u);
        Handle(R, body, L, 2048, out);
        CHECK((out[5] & kScopeAtHonoured) && U32(out + 6) == R.w - 256, "pos exactly newest → honoured");
        delete Rp;
    }

    SECTION("cv tier: J3..J8 on the same clock, gates and LFOs, level trigger, alignment, detach");
    {
        Rings* Rp = NewRings(); Rings& R = *Rp;
        /* 12 s: a kick-ish 60 Hz burst on out L at each step (125 ms), J6 (index 3) a 6 ms 5 V gate at every step,
         * J4 (index 1) a 0.2 Hz ±5 V LFO, J3 (index 0) a 5 ms clock pulse per step, J5 (index 2) a 0..3 V staircase */
        const int stepLen = 6000;
        for(int i = 0; i < 12 * 48000; i++)
        {
            const float t = i / kSr, ts = (i % stepLen) / kSr;
            const float oL = ts < 0.06f ? 0.8f * sinf(2 * kPi * 60.f * ts) * expf(-ts * 20.f) : 0.f;
            float volts[kScopeCvChans] = { ts < 0.005f ? 5.f : 0.f, 5.f * sinf(2 * kPi * 0.2f * t), (float)((i / stepLen) % 4), ts < 0.006f ? 5.f : 0.f, 0.f, -2.5f };
            R.Push(oL, oL, 0, 0, volts);
        }
        /* cv only: J6 (bit 3) triggered at 1 V, ×1 → 512 samples = 256 ms, aligned to the gate's rise */
        int L = Req(body, 0x00, 1, 512, kScopeTrig | kScopeExt, 0, 0x08, 1, 25);
        int len = Handle(R, body, L, 1024, out);
        CHECK(len == 14 + 512 && (out[5] & (kScopeTriggered | kScopeExtReply)) == (kScopeTriggered | kScopeExtReply) && out[10] == 0x08 && out[11] == 1 && out[12] == kScopeCvDiv, "cv-only frame: ext header, triggered (flags %u, len %d)", out[5], len);
        const uint32_t posC = U32(out + 6); const int8_t* g = (const int8_t*)out + 14;
        CHECK(posC % 24 == 0 && (posC / 24) % (stepLen / 24) == 0, "the gate's rise is on a block and on the step grid: pos %u", posC);
        CHECK(g[0] == 127 && g[11] == 127 && g[12] == 0 && g[100] == 0 && g[250] == 127, "the 6 ms gate reads 12 samples of +5 V at 2 kHz, the next gate 250 samples on (%d %d %d %d %d)", g[0], g[11], g[12], g[100], g[250]);
        /* the LFO on J4 at ×32: 512 samples = 8.2 s, 1.6 periods of the 0.2 Hz sine, ±127 */
        L = Req(body, 0x00, 1, 512, kScopeExt, 0, 0x02, 32, 0);
        len = Handle(R, body, L, 1024, out);
        int mn = 127, mx = -127; for(int i = 0; i < (out[3] | (out[4] << 8)); i++) { if(g[i] < mn) mn = g[i]; if(g[i] > mx) mx = g[i]; }
        CHECK((out[3] | (out[4] << 8)) == kScopeCvUsable / 32 && mn <= -125 && mx >= 125, "LFO at ×32: n %d, range %d..%d", out[3] | (out[4] << 8), mn, mx);
        /* audio + cv, triggered on out L (first selected): both tiers start at the same instant, and the audio trigger
         * left room for the cv window too (512 cv samples = 12288 audio samples) */
        L = Req(body, 0x01, 4, 512, kScopeTrig | kScopeExt, 0, 0x08, 1, 0);
        len = Handle(R, body, L, 2048, out);
        const uint32_t posA = U32(out + 6); const int n2 = out[3] | (out[4] << 8);
        CHECK(len == 14 + 2 * n2 && (out[5] & kScopeTriggered) && !(out[5] & kScopeDetached) && out[13] == 0, "audio-triggered mixed frame, both tiers aligned (flags %u, src %u)", out[5], out[13]);
        CHECK(ScopeAge(R.w, posA) >= 512u * 24u, "the audio trigger left the cv span (age %u)", ScopeAge(R.w, posA));
        const int8_t* aL = (const int8_t*)out + 14; const int8_t* j6 = aL + n2;
        CHECK(aL[0] >= 0 && R.a[0][ScopeSlot(posA - 1)] < 0, "audio starts on the crossing");
        /* the crossing is somewhere in the kick burst; the next step's gate rises (6000 − pos % 6000) samples on, i.e. that many / 24 cv samples in */
        { const int k = (int)((stepLen - posA % stepLen) / 24); CHECK(k < n2 && j6[k] == 127 && (k == 0 || j6[k - 1] == 0 || posA % stepLen < 200), "the next gate rises at cv index %d (%d, %d)", k, j6[k], k ? j6[k - 1] : -1); }
        /* the same frame re-read at pos: honoured on both tiers */
        L = Req(body, 0x02, 4, n2, kScopeAt | kScopeExt, posA, 0x08, 1, 0);
        len = Handle(R, body, L, 2048, out);
        CHECK((out[5] & kScopeAtHonoured) && U32(out + 6) == posA && memcmp(out + 14 + n2, j6, n2) == 0, "at-pos honours both tiers, J6 identical");
        /* cv-triggered (explicit source J6) mixed frame: the gate that leaves 256 ms is ≥ 256 ms old — the audio ring
         * (1.37 s) still holds it, so both tiers align and the audio window shows the kick under its gate */
        L = Req(body, 0x01, 4, 512, kScopeTrig | kScopeExt, 0, 0x08, 1, 25, kScopeTrigSet | 7);
        len = Handle(R, body, L, 2048, out);
        CHECK((out[5] & kScopeTriggered) && !(out[5] & kScopeDetached) && out[13] == 7 && U32(out + 6) % 24 == 0 && ScopeAge(R.w, U32(out + 6)) >= 512 * 24, "cv-triggered mixed frame within the audio ring: aligned (flags %u, src %u, age %u)", out[5], out[13], ScopeAge(R.w, U32(out + 6)));
        const int8_t* aL2 = (const int8_t*)out + 14; const int8_t* j62 = aL2 + (out[3] | (out[4] << 8));
        CHECK(j62[0] == 127 && (out[3] | (out[4] << 8)) == 512 && U32(out + 6) % stepLen == 0, "the gate opens the cv window at a step start, n 512 (got %d, pos %% step %u)", out[3] | (out[4] << 8), U32(out + 6) % stepLen);
        { int first = -1; for(int i = 0; i < 100; i++) if(aL2[i] != 0) { first = i; break; } CHECK(first >= 0 && first < 30, "the kick starts with the gate in the audio window (first non-zero at %d)", first); }
        /* …but a cv window of 4 s (×16) starts long before the audio ring: the audio tier detaches to its newest window */
        L = Req(body, 0x01, 4, 512, kScopeTrig | kScopeExt, 0, 0x08, 16, 25, kScopeTrigSet | 7);
        len = Handle(R, body, L, 2048, out);
        const int n3 = out[3] | (out[4] << 8);
        CHECK((out[5] & kScopeTriggered) && (out[5] & kScopeDetached) && U32(out + 6) == ScopeSub(R.w, (uint32_t)(n3 * 4)), "cv window older than the audio ring: audio detached to newest (flags %u)", out[5]);
        const int8_t* j63 = (const int8_t*)out + 14 + n3;
        CHECK(j63[0] == 127, "the cv tier still starts on its trigger");
        /* free-running mixed frame: both tiers at the older of the two newest starts (the cv one here) */
        L = Req(body, 0x01, 4, 512, kScopeExt, 0, 0x08, 1, 0);
        len = Handle(R, body, L, 2048, out);
        const int n4 = out[3] | (out[4] << 8); const uint32_t p4 = U32(out + 6);
        CHECK(!(out[5] & (kScopeTriggered | kScopeDetached)) && p4 == ScopeSub(R.w, (uint32_t)(n4 * 24)) && p4 % 24 == 0, "free-running mixed: start = cv newest, on a block (pos %u)", p4);
        /* at-pos with a pos the cv tier holds but the audio tier does not → not honoured */
        L = Req(body, 0x01, 4, 256, kScopeAt | kScopeExt, ScopeSub(R.w, 100000u), 0x08, 1, 0);
        Handle(R, body, L, 2048, out);
        CHECK(!(out[5] & kScopeAtHonoured), "at-pos needs every selected tier to hold pos");
        /* the staircase on J5 (bit 2) and the clock on J3 (bit 0), free-running ×4 (1 s): four steps visible, 8 pulses */
        L = Req(body, 0x00, 1, 512, kScopeExt, 0, 0x05, 4, 0);
        len = Handle(R, body, L, 1024, out);
        const int8_t* j3 = (const int8_t*)out + 14; const int8_t* j5 = j3 + 505;
        int pulses = 0; for(int i = 1; i < 505; i++) if(j3[i] == 127 && j3[i - 1] == 0) pulses++;
        int levels[4] = {0, 0, 0, 0}; for(int i = 0; i < 505; i++) { const int lv = (int)((ScopeCvVolts(j5[i]) + 0.5f)); if(lv >= 0 && lv < 4) levels[lv]++; }
        CHECK((out[3] | (out[4] << 8)) == 505 && pulses >= 7 && pulses <= 9 && levels[0] && levels[1] && levels[2] && levels[3], "J3 pulses %d, J5 levels %d/%d/%d/%d", pulses, levels[0], levels[1], levels[2], levels[3]);
        CHECK(((const int8_t*)out + 14)[0] == j3[0] && j5[0] >= 0, "channel order: audio in mask order, then J3..J8 in bit order");
        delete Rp;
    }

    SECTION("old mode: the empty body is the pre-SCOPE reply; the wrap");
    {
        Rings* Rp = NewRings(); Rings& R = *Rp;
        int8_t old[1024]; uint16_t ow = 0;
        for(int i = 0; i < 12360; i++)
        {
            const float t = i / kSr, l = 0.6f * sinf(2 * kPi * 97.f * t), r = 0.4f * cosf(2 * kPi * 211.f * t);
            R.Push(l, r, 0, 0);
            if((i & 3) == 0) { old[ow & 1023u] = (int8_t)(0.5f * (l + r) * 127.f); ow++; }
        }
        const int len = Handle(R, nullptr, 0, 2048, out);
        CHECK(len == 259 && out[0] == 0 && out[1] == 0 && out[2] == 1, "old reply shape: status, u16 256, 256 samples");
        int maxErr = 0;
        for(int i = 0; i < 256; i++) { const int a = (int8_t)out[3 + i], b = old[(ow - 256 + i) & 1023u]; const int e = a > b ? a - b : b - a; if(e > maxErr) maxErr = e; }
        CHECK(maxErr <= 1, "old-mode samples within 1 LSB of the old writer (max err %d)", maxErr);
        delete Rp;
        /* the wrap at kScopeWrap: positions, slots (audio and cv), the trigger and at-pos all go through it */
        Rings* Wp = NewRings(); Rings& W = *Wp; W.w = kScopeWrap - 24u * 400u;
        for(int i = 0; i < 24 * 800; i++) { float volts[kScopeCvChans] = { (i / 24) % 50 < 10 ? 5.f : 0.f, 0, 0, 0, 0, 0 }; W.Push((float)(i % 100) / 100.f - 0.5f, 0, 0, 0, volts); }
        CHECK(W.w == 24u * 400u, "index wrapped to %u", W.w);
        /* a cv window of 512 cv samples (12288 audio) forces the start before the wrap point */
        int L = Req(body, 0x01, 1, 256, kScopeTrig | kScopeExt, 0, 0x01, 2, 25);
        Handle(W, body, L, 2048, out);
        const uint32_t p = U32(out + 6);
        CHECK((out[5] & kScopeTriggered) && !(out[5] & kScopeDetached) && ScopeAge(W.w, p) >= 512u * 24u && p > W.w, "trigger across the wrap (pos %u, w %u)", p, W.w);
        bool ok = true; for(int i = 0; i < 256; i++) if((int8_t)out[14 + i] != W.a[0][ScopeSlot(ScopeAdd(p, (uint32_t)i))]) ok = false;
        CHECK(ok, "audio samples read through the wrap");
        const int8_t* j3 = (const int8_t*)out + 14 + 256; bool cvOk = true; for(int i = 0; i < 256; i++) if(j3[i] != W.c[0][ScopeCvSlot(ScopeAdd(ScopeCvPos(p), (uint32_t)(2 * i), kScopeCvWrap))]) cvOk = false;
        int hi = 0; for(int i = 0; i < 256; i++) if(j3[i] == 127) hi++;
        CHECK(cvOk && hi > 0, "cv samples read through the wrap (%d high)", hi);
        L = Req(body, 0x01, 1, 256, kScopeAt | kScopeExt, p, 0x01, 2, 0);
        Handle(W, body, L, 2048, out);
        CHECK((out[5] & kScopeAtHonoured) && U32(out + 6) == p, "at-pos honoured across the wrap");
        delete Wp;
    }

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
