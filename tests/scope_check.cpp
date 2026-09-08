// scope_check — host tests for core/bb_scope.h (SCOPE package). Build + run:
//   tests/run.sh
// or by hand:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore tests/scope_check.cpp -o /tmp/sc && /tmp/sc
//
// A miniature of the shell: four int8 rings under one write index, fed at
// "full rate" by a fake audio thread, then read the way the 0x62 handler
// reads them — parse, window (trigger / at-pos), extract per channel — and
// the pre-SCOPE (empty body) reply beside it.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "bb_scope.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

using namespace bb;

struct Rings
{
    int8_t   r[kScopeChans][kScopeRing];
    uint32_t w = 0;
    Rings() { memset(r, 0, sizeof r); }
    /* the audio thread: one sample of each channel under the shared index */
    void Push(float oL, float oR, float iL, float iR)
    {
        const int s = ScopeSlot(w);
        r[0][s] = ScopeI8(oL); r[1][s] = ScopeI8(oR); r[2][s] = ScopeI8(iL); r[3][s] = ScopeI8(iR);
        w++;
    }
};

/* The 0x62 handler, as the shell runs it: one snapshot, header, channels in mask order. */
static int Handle(const Rings& R, const uint8_t* body, int len, int maxBody, uint8_t* out)
{
    const uint32_t wpos = R.w;
    if(len == 0)
    {
        out[0] = 0; out[1] = 0; out[2] = 1;   /* status, u16 256 */
        ScopeExtractOld(R.r[0], R.r[1], wpos, (int8_t*)out + 3);
        return 259;
    }
    ScopeReq q;
    if(!ScopeParse(body, len, maxBody, q)) { out[0] = 2; return 1; }
    uint8_t fl = 0;
    const uint32_t start = ScopeWindow(R.r[ScopeFirstChan(q.mask)], wpos, q.n * q.decim, q, fl);
    int o = 0;
    out[o++] = 0; out[o++] = q.mask; out[o++] = q.decim; out[o++] = (uint8_t)q.n; out[o++] = (uint8_t)(q.n >> 8); out[o++] = fl;
    out[o++] = (uint8_t)start; out[o++] = (uint8_t)(start >> 8); out[o++] = (uint8_t)(start >> 16); out[o++] = (uint8_t)(start >> 24);
    for(int c = 0; c < kScopeChans; c++)
        if(q.mask & (1u << c)) { ScopeExtract(R.r[c], start, q.n, q.decim, (q.flags & kScopePeak) != 0, (int8_t*)out + o); o += q.n; }
    return o;
}
static uint32_t U32(const uint8_t* b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static void Req(uint8_t* b, uint8_t mask, uint8_t decim, uint16_t n, uint8_t flags, uint32_t pos = 0)
{
    b[0] = mask; b[1] = decim; b[2] = (uint8_t)n; b[3] = (uint8_t)(n >> 8); b[4] = flags;
    b[5] = (uint8_t)pos; b[6] = (uint8_t)(pos >> 8); b[7] = (uint8_t)(pos >> 16); b[8] = (uint8_t)(pos >> 24);
}

int main()
{
    static uint8_t out[4096]; uint8_t body[9];

    SECTION("parse + clamps");
    {
        ScopeReq q;
        Req(body, 0x0f, 4, 512, 0);
        CHECK(ScopeParse(body, 5, 2048, q) && q.n == 509 && q.mask == 0x0f && q.decim == 4, "4 ch at 2048 → 509 (got %d)", q.n);
        CHECK(ScopeParse(body, 5, 512, q) && q.n == 125, "4 ch at 512 → 125 (got %d)", q.n);
        Req(body, 0x03, 4, 512, 0);
        CHECK(ScopeParse(body, 5, 2048, q) && q.n == 512, "2 ch at 2048 → 512 (got %d)", q.n);
        Req(body, 0x01, 16, 512, 0);
        CHECK(ScopeParse(body, 5, 2048, q) && q.n == kScopeUsable / 16, "×16 clamps to the usable ring: %d", q.n);
        Req(body, 0x01, 1, 9999, 0);
        CHECK(ScopeParse(body, 5, 2048, q) && q.n == 512, "n > 512 clamps to 512 (got %d)", q.n);
        Req(body, 0x00, 4, 256, 0); CHECK(!ScopeParse(body, 5, 2048, q), "mask 0 → BAD_ARGS");
        Req(body, 0x10, 4, 256, 0); CHECK(!ScopeParse(body, 5, 2048, q), "mask bit 4 → BAD_ARGS");
        Req(body, 0x01, 0, 256, 0); CHECK(!ScopeParse(body, 5, 2048, q), "decim 0 → BAD_ARGS");
        Req(body, 0x01, 17, 256, 0); CHECK(!ScopeParse(body, 5, 2048, q), "decim 17 → BAD_ARGS");
        Req(body, 0x01, 4, 0, 0); CHECK(!ScopeParse(body, 5, 2048, q), "n 0 → BAD_ARGS");
        Req(body, 0x01, 4, 256, 0); CHECK(!ScopeParse(body, 4, 2048, q), "short body → BAD_ARGS");
        Req(body, 0x01, 4, 256, kScopeAt, 1234); CHECK(!ScopeParse(body, 5, 2048, q), "kScopeAt without pos → BAD_ARGS");
        CHECK(ScopeParse(body, 9, 2048, q) && q.pos == 1234u, "kScopeAt carries pos");
        CHECK(ScopeChanCount(0x0f) == 4 && ScopeChanCount(0x0a) == 2 && ScopeFirstChan(0x0a) == 1 && ScopeFirstChan(0x08) == 3 && ScopeFirstChan(0) == -1, "mask helpers");
        CHECK(ScopeI8(1.5f) == 127 && ScopeI8(-1.5f) == -127 && ScopeI8(0.5f) == 63 && ScopeI8(0.f) == 0, "float → i8 saturates");
    }

    SECTION("trigger finds the crossing; channels are aligned to one index");
    {
        Rings R;
        /* 2 s of 100 Hz on out L, its inverse on out R, a 440 Hz on in L, a ramp on in R (all distinct) */
        const float sr = 48000.f;
        for(int i = 0; i < 96000; i++)
        {
            const float t = i / sr;
            const float oL = 0.8f * sinf(2 * 3.14159265f * 100.f * t);
            R.Push(oL, -oL, 0.5f * sinf(2 * 3.14159265f * 440.f * t), (float)(i % 480) / 480.f - 0.5f);
        }
        Req(body, 0x0f, 1, 512, kScopeTrig);
        const int len = Handle(R, body, 5, 2048, out);
        CHECK(len == 10 + 4 * 509, "4 ch × 509 + header = %d", len);
        CHECK(out[0] == 0 && out[1] == 0x0f && out[2] == 1 && (out[3] | (out[4] << 8)) == 509, "header echoes the clamped request");
        CHECK(out[5] & kScopeTriggered, "triggered flag");
        const uint32_t pos = U32(out + 6);
        const int8_t* oL = (const int8_t*)out + 10;
        const int8_t* oR = oL + 509; const int8_t* iL = oR + 509; const int8_t* iR = iL + 509;
        /* the window starts on a rising zero crossing of out L (the first selected channel) */
        CHECK(oL[0] >= 0 && R.r[0][ScopeSlot(pos - 1)] < 0, "starts at a rising crossing: prev %d, first %d", R.r[0][ScopeSlot(pos - 1)], oL[0]);
        /* 100 Hz at 48 kHz: the crossing sits on a multiple of 480 samples */
        CHECK(pos % 480 == 0, "crossing on the 100 Hz grid: pos %u", pos);
        /* the window leaves n·decim samples after it */
        CHECK((int32_t)(R.w - pos) >= 512, "window fits: %d after", (int)(R.w - pos));
        /* alignment: every channel was read at the same index, so out R is -out L sample for sample */
        bool aligned = true; for(int i = 0; i < 509; i++) if(oR[i] != -oL[i]) { aligned = false; break; }
        CHECK(aligned, "out R == -out L across the window");
        /* in R is the ramp, read at the same pos: its value at i is ((pos + i) % 480) / 480 - 0.5 */
        bool ramp = true; for(int i = 0; i < 509; i += 7) { const int want = ScopeI8((float)((pos + i) % 480) / 480.f - 0.5f); if(iR[i] != want) { ramp = false; break; } }
        CHECK(ramp, "in R ramp aligned to pos");
        CHECK(iL[0] == 0 || iL[0] != oL[0] || iL[100] != oL[100], "in L is its own signal");
        /* the crossing chosen is the MOST RECENT one that fits */
        bool later = false; for(uint32_t p = pos + 480; (int32_t)(R.w - p) >= 512; p += 480) later = true;
        CHECK(!later, "no later crossing would have fit");
        /* untriggered: the newest window */
        Req(body, 0x01, 1, 512, 0);
        Handle(R, body, 5, 2048, out);
        CHECK(!(out[5] & kScopeTriggered) && U32(out + 6) == R.w - 512, "untriggered → newest window");
        /* first selected channel decides: mask 0x08 (in R, the ramp) triggers on the ramp's rise through zero */
        Req(body, 0x08, 1, 512, kScopeTrig);
        Handle(R, body, 5, 2048, out);
        const uint32_t p2 = U32(out + 6);
        /* the ramp's int8 crossing: the first sample of the cycle whose i8 is ≥ 0 (truncation puts it a few samples before 240) */
        int rampX = 0; for(int k = 1; k < 480; k++) if(ScopeI8((float)(k - 1) / 480.f - 0.5f) < 0 && ScopeI8((float)k / 480.f - 0.5f) >= 0) rampX = k;
        CHECK((out[5] & kScopeTriggered) && (int)(p2 % 480) == rampX, "trigger follows the first selected channel: pos %u (want %%480 == %d)", p2, rampX);
    }

    SECTION("hysteresis: noise around zero does not trigger; silence is untriggered");
    {
        Rings R;
        for(int i = 0; i < 20000; i++) { const float n = ((i * 7919) % 13 - 6) / 127.f * 0.5f; R.Push(n, n, 0, 0); }   /* ±3 int8 units */
        Req(body, 0x01, 1, 256, kScopeTrig);
        Handle(R, body, 5, 2048, out);
        CHECK(!(out[5] & kScopeTriggered) && U32(out + 6) == R.w - 256, "±3 noise (< hyst 4) leaves the scope free-running");
        Rings S;
        for(int i = 0; i < 20000; i++) S.Push(0, 0, 0, 0);
        Handle(S, body, 5, 2048, out);
        CHECK(!(out[5] & kScopeTriggered), "silence is untriggered");
        /* one clean pulse far enough back: a crossing that leaves the span */
        Rings P;
        for(int i = 0; i < 6000; i++) { float v = 0; if(i >= 3000 && i < 3040) v = -0.5f; if(i >= 3040 && i < 3080) v = 0.5f; P.Push(v, 0, 0, 0); }
        Handle(P, body, 5, 2048, out);
        CHECK((out[5] & kScopeTriggered) && U32(out + 6) == 3040u, "a single pulse triggers at its rise (pos %u)", U32(out + 6));
        /* a crossing too recent to leave the span is skipped in favour of an older one (inside the usable ring) */
        Rings Q;
        for(int i = 0; i < 6000; i++) { float v = 0; if((i >= 3000 && i < 3040) || (i >= 5900 && i < 5940)) v = -0.5f; if((i >= 3040 && i < 3080) || (i >= 5940 && i < 5980)) v = 0.5f; Q.Push(v, 0, 0, 0); }
        Handle(Q, body, 5, 2048, out);
        CHECK((out[5] & kScopeTriggered) && U32(out + 6) == 3040u, "the crossing that fits wins over the newer one (pos %u)", U32(out + 6));
        /* …and one that has fallen out of the usable ring is gone */
        Rings O;
        for(int i = 0; i < 6000; i++) { float v = 0; if(i >= 2000 && i < 2040) v = -0.5f; if(i >= 2040 && i < 2080) v = 0.5f; O.Push(v, 0, 0, 0); }
        Handle(O, body, 5, 2048, out);
        CHECK(!(out[5] & kScopeTriggered), "a crossing older than the usable ring is not used");
    }

    SECTION("decimation: plain pick and peak");
    {
        Rings R;
        for(int i = 0; i < 8192; i++) { float v = 0; if(i % 64 == 37) v = 0.9f; if(i % 64 == 38) v = -0.3f; R.Push(v, (float)((i % 128) - 64) / 128.f, 0, 0); }
        /* plain ×16 from the newest window: a click at slot 37 of 64 is missed by the picks at 0,16,32,48 */
        Req(body, 0x01, 16, 100, 0);
        Handle(R, body, 5, 2048, out);
        const uint32_t start = U32(out + 6); const int8_t* s = (const int8_t*)out + 10;
        bool anyHit = false; for(int i = 0; i < 100; i++) if(s[i] != 0) anyHit = true;
        CHECK(start % 64 == 0 ? !anyHit : true, "plain pick can miss the click (start %u)", start);
        bool plainOk = true; for(int i = 0; i < 100; i++) if(s[i] != R.r[0][ScopeSlot(start + i * 16)]) plainOk = false;
        CHECK(plainOk, "plain pick = the first sample of each group");
        /* peak ×16: every group holding the click reports it (sign kept) */
        Req(body, 0x01, 16, 100, kScopePeak);
        Handle(R, body, 5, 2048, out);
        int clicks = 0; bool signOk = true;
        for(int i = 0; i < 100; i++) { if(s[i] != 0) { clicks++; if(s[i] != ScopeI8(0.9f)) signOk = false; } }
        CHECK(clicks == 25 && signOk, "peak keeps the click in every 4th group: %d clicks", clicks);
        /* peak on the ramp channel: the max-magnitude sample of each group, exact */
        Req(body, 0x02, 8, 200, kScopePeak);
        Handle(R, body, 5, 2048, out);
        const uint32_t st2 = U32(out + 6); bool pk = true;
        for(int i = 0; i < 200 && pk; i++) { int best = 0, mag = -1; for(int k = 0; k < 8; k++) { const int v = R.r[1][ScopeSlot(st2 + i * 8 + k)]; const int m = v < 0 ? -v : v; if(m > mag) { mag = m; best = v; } } if(s[i] != best) pk = false; }
        CHECK(pk, "peak = max-magnitude sample of each group");
        /* decim 1 with peak is a plain copy */
        Req(body, 0x02, 1, 300, kScopePeak);
        Handle(R, body, 5, 2048, out);
        const uint32_t st3 = U32(out + 6); bool copy = true; for(int i = 0; i < 300; i++) if(s[i] != R.r[1][ScopeSlot(st3 + i)]) copy = false;
        CHECK(copy, "×1 peak = the samples");
    }

    SECTION("at-pos: a second request of the same frame lands on the same samples");
    {
        Rings R;
        for(int i = 0; i < 30000; i++) { const float t = i / 48000.f; R.Push(0.7f * sinf(2 * 3.14159265f * 60.f * t), 0.2f * sinf(2 * 3.14159265f * 1000.f * t), 0.3f * sinf(2 * 3.14159265f * 150.f * t), 0); }
        Req(body, 0x01, 2, 500, kScopeTrig);
        Handle(R, body, 5, 2048, out);
        const uint32_t pos = U32(out + 6); CHECK(out[5] & kScopeTriggered, "first request triggers");
        int8_t first[500]; memcpy(first, out + 10, 500);
        /* the audio thread runs on a little (as it would between two HostLink requests) */
        for(int i = 0; i < 200; i++) R.Push(0, 0, 0, 0);
        Req(body, 0x0e, 2, 500, kScopeAt, pos);
        const int len = Handle(R, body, 9, 2048, out);
        CHECK(len == 10 + 3 * 500 && (out[5] & kScopeAtHonoured) && !(out[5] & kScopeTriggered) && U32(out + 6) == pos, "second request honoured at pos (flags %u)", out[5]);
        /* channel 1 of the second reply equals what a 4-channel single request would have given at that pos */
        int8_t ref[500]; ScopeExtract(R.r[1], pos, 500, 2, false, ref);
        CHECK(memcmp(out + 10, ref, 500) == 0, "out R at the same pos");
        /* and the first channel is still what it was (the ring has not wrapped) */
        int8_t again[500]; ScopeExtract(R.r[0], pos, 500, 2, false, again);
        CHECK(memcmp(again, first, 500) == 0, "out L unchanged at pos");
        /* a pos that has fallen out of the ring, or is too recent, is refused: newest window, no honour flag */
        Req(body, 0x01, 1, 256, kScopeAt, R.w - 5000u);
        Handle(R, body, 9, 2048, out);
        CHECK(!(out[5] & kScopeAtHonoured) && U32(out + 6) == R.w - 256, "pos too old → newest, not honoured");
        Req(body, 0x01, 1, 256, kScopeAt, R.w - 100u);
        Handle(R, body, 9, 2048, out);
        CHECK(!(out[5] & kScopeAtHonoured) && U32(out + 6) == R.w - 256, "pos too recent → newest, not honoured");
        Req(body, 0x01, 1, 256, kScopeAt, R.w - 256u);
        Handle(R, body, 9, 2048, out);
        CHECK((out[5] & kScopeAtHonoured) && U32(out + 6) == R.w - 256, "pos exactly newest → honoured");
    }

    SECTION("old mode: the empty body is the pre-SCOPE reply");
    {
        Rings R;
        /* the old writer: gScope[w++] = (int8)(0.5·(l+r)·127) on every 4th sample of the block */
        int8_t old[1024]; uint16_t ow = 0;
        for(int i = 0; i < 12345; i++)
        {
            const float t = i / 48000.f, l = 0.6f * sinf(2 * 3.14159265f * 97.f * t), r = 0.4f * cosf(2 * 3.14159265f * 211.f * t);
            R.Push(l, r, 0, 0);
            if((i & 3) == 0) { old[ow & 1023u] = (int8_t)(0.5f * (l + r) * 127.f); ow++; }
        }
        const int len = Handle(R, nullptr, 0, 2048, out);
        CHECK(len == 259 && out[0] == 0 && out[1] == 0 && out[2] == 1, "old reply shape: status, u16 256, 256 samples");
        /* the same instants (multiples of 4), the same value to within 1 LSB (two roundings vs one) */
        int maxErr = 0;
        for(int i = 0; i < 256; i++) { const int a = (int8_t)out[3 + i], b = old[(ow - 256 + i) & 1023u]; const int e = a > b ? a - b : b - a; if(e > maxErr) maxErr = e; }
        CHECK(maxErr <= 1, "old-mode samples within 1 LSB of the old writer (max err %d)", maxErr);
        /* the wrap: absolute positions wrap at 2^32 with the slot arithmetic intact */
        Rings W; W.w = 0xfffffff0u;
        for(int i = 0; i < 5000; i++) W.Push((float)(i % 100) / 100.f - 0.5f, 0, 0, 0);
        Req(body, 0x01, 1, 256, kScopeTrig);
        Handle(W, body, 5, 2048, out);
        const uint32_t p = U32(out + 6);
        CHECK((out[5] & kScopeTriggered) && (int32_t)(W.w - p) >= 256 && (int32_t)(W.w - p) <= kScopeUsable, "trigger across the 2^32 wrap (pos %u, w %u)", p, W.w);
        bool ok = true; for(int i = 0; i < 256; i++) if((int8_t)out[10 + i] != W.r[0][ScopeSlot(p + i)]) ok = false;
        CHECK(ok, "samples read through the wrap");
    }

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
