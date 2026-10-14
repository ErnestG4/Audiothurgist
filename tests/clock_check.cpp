// clock_check — host tests for core/bb_clockmath.h (CLOCK package). Build + run:
//   tests/run.sh
// or by hand:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore tests/clock_check.cpp -o /tmp/cc && /tmp/cc
//
// A miniature of the firmware's clock pipeline: an NCO at `tpu` ticks/µs polled
// every 1 ms by the control thread, an external clock at `tpp` ticks per pulse
// whose pulses sit at an arbitrary NCO phase (the NCO free-ran through
// acquisition), and an audio thread sampling the map every 0.5 ms (one
// 24-sample block) with its own extrapolation. Everything the firmware does
// with the map — lock alignment, zone requests, commits, the phase guard — is
// exercised here with the same calls.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "bb_clockmath.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static double Mod(double x, double m) { double r = std::fmod(x, m); if(r < 0.) r += m; return r; }
static bool   OnGrid(double x, double m, double tol = 1e-6) { const double r = Mod(x, m); return r < tol || m - r < tol; }

struct Sim
{
    double ppqn = 96., tpu = 96. * 120. / 60. / 1e6;   /* 120 BPM at 96 PPQN */
    double tpp  = 4.;                                  /* 24 PPQN external */
    double phi  = 1.37;                                /* pulses at T = phi + k·tpp */
    int    nSlices = 16;
    double loopT() const { return nSlices * ppqn / 4.; }
    double guard() const { return tpu * 3000.; }

    bb::ClockMap   map;
    bb::PhaseGuard guard_;
    double T = 0., pulseTick = -1., want = 1.;
    int    pulses = 0;
    /* audio-side continuity bookkeeping (loop ticks, unwrapped) */
    double prevL = -1., minDl = 1e300, maxDl = 0., lastLp = -1., minLpDrop = 0.;
    int    wraps = 0, commits = 0;
    double lastSw = -1.;

    void LockAlign()
    {
        double A = tpp * (map.mult > 1. ? map.mult : 1.); if(A > ppqn) A = ppqn;
        map.Shift(bb::ClockMap::AlignDelta(map.Loop(pulseTick), A, 0.4 * ppqn / 4.));
    }
    double Audio(double Ta)   /* one audio block at NCO position Ta */
    {
        const double L = map.Loop(Ta);
        if(prevL >= 0.) { const double dL = L - prevL; if(dL < minDl) minDl = dL; if(dL > maxDl) maxDl = dL; }
        prevL = L;
        double lp = std::fmod(L, loopT()) / loopT(); if(lp < 0.) lp += 1.;
        lp = guard_.Apply(lp, nSlices);
        if(lastLp >= 0. && lp < lastLp) { if(lastLp - lp > 0.5) wraps++; else if(lastLp - lp > -minLpDrop) minLpDrop = -(lastLp - lp); }
        lastLp = lp;
        return lp;
    }
    void Poll()   /* one 1 ms control poll, then two audio blocks extrapolated from it */
    {
        const double Tn = T + tpu * 1000.;
        /* pulses in (T, Tn] */
        const double kNext = std::floor((T - phi) / tpp) + 1.;
        const double Tp    = phi + kNext * tpp;
        if(Tp > T && Tp <= Tn) { pulseTick = Tp; pulses++; }
        T = Tn;
        if(map.Commit(T)) { commits++; lastSw = map.mult; }
        map.Request(want, T, guard(), pulseTick, pulseTick >= 0. ? tpp : 0., ppqn);
        Audio(T + tpu * 500.); Audio(T + tpu * 1000.);
    }
    void Run(int polls) { for(int i = 0; i < polls; i++) Poll(); }
    /* the loop position of the next N pulses, for grid checks */
    int PulsesOnGrid(int n, double grid)
    {
        int on = 0; double k = std::floor((T - phi) / tpp) + 1.;
        for(int i = 0; i < n; i++, k += 1.) on += OnGrid(map.Loop(phi + k * tpp), grid, 1e-6);
        return on;
    }
};

int main()
{
    SECTION("ClockMap: lock alignment puts pulses on the loop grid");
    {
        Sim s; s.Run(300);                                   /* free-run 300 ms, pulses seen */
        CHECK(s.pulseTick > 0., "pulses were stamped (%d)", s.pulses);
        CHECK(!OnGrid(s.map.Loop(s.pulseTick), s.tpp), "before lock the pulse phase is arbitrary (L mod tpp = %.3f)", Mod(s.map.Loop(s.pulseTick), s.tpp));
        s.LockAlign();
        CHECK(OnGrid(s.map.Loop(s.pulseTick), s.tpp), "after lock alignment the lock pulse is on the pulse grid");
        CHECK(s.PulsesOnGrid(24, s.tpp) == 24, "every following pulse lands on the pulse grid (%d/24)", s.PulsesOnGrid(24, s.tpp));
        CHECK(std::fabs(s.map.off) <= 0.5 * s.tpp + 1e-9, "at 24 PPQN the alignment shift is at most half a pulse (%.3f ticks)", s.map.off);
    }

    SECTION("ClockMap: ×1 → ×2 → ÷2 → ×1 with phase continuity, beat- and pulse-aligned");
    {
        Sim s; s.Run(200); s.LockAlign();
        s.prevL = -1.; s.minDl = 1e300; s.maxDl = 0.; s.guard_.jumps = 0;
        const double d1 = s.tpu * 500.;                      /* ×1 loop-tick advance per audio block */
        auto Switch = [&](double m, const char* name) {
            const double Treq = s.T; const int c0 = s.commits;
            s.want = m;
            for(int i = 0; i < 4000 && s.commits == c0; i++) s.Poll();
            CHECK(s.commits == c0 + 1, "%s: committed", name);
            CHECK(s.map.mult == m && !s.map.Pending(), "%s: applied %.2f", name, s.map.mult);
            CHECK(s.T - Treq <= s.ppqn * 4. / (m < 1. ? m : 1.) + s.guard() + s.tpu * 1000., "%s: within the joint-boundary wait (%.1f ticks)", name, s.T - Treq);
            s.Run(400);
        };
        /* record the switch instants: swTick is set when scheduled */
        double sw[3]; int n = 0;
        s.want = 2.; while(!s.map.Pending()) s.Poll(); sw[n++] = s.map.swTick;
        CHECK(OnGrid(sw[0] - s.phi, s.tpp), "×2 switch instant is a pulse instant");
        CHECK(OnGrid(s.map.mult * sw[0] + s.map.off, s.ppqn, 0.5 * s.tpp + 1e-9), "×2 switch instant is a loop beat (L mod 96 = %.3f)", Mod(s.map.mult * sw[0] + s.map.off, s.ppqn));
        Switch(2., "×2");
        CHECK(s.PulsesOnGrid(24, 2. * s.tpp) == 24, "at ×2 every pulse lands on an even pulse position (%d/24)", s.PulsesOnGrid(24, 2. * s.tpp));

        s.want = 0.5; while(!s.map.Pending()) s.Poll(); sw[n++] = s.map.swTick;
        CHECK(OnGrid(sw[1] - s.phi, s.tpp), "÷2 switch instant is a pulse instant");
        CHECK(OnGrid(s.map.mult * sw[1] + s.map.off, s.ppqn, 0.5 * s.tpp + 1e-9), "÷2 switch instant is a loop beat");
        Switch(0.5, "÷2");
        {   const int on = s.PulsesOnGrid(24, s.tpp);
            CHECK(on >= 12 && on <= 13, "at ÷2 every other pulse lands on a pulse position (%d/24)", on); }

        s.want = 1.; while(!s.map.Pending()) s.Poll(); sw[n++] = s.map.swTick;
        Switch(1., "×1");
        CHECK(s.PulsesOnGrid(24, s.tpp) == 24, "back at ×1 every pulse is on the pulse grid (%d/24)", s.PulsesOnGrid(24, s.tpp));

        CHECK(s.minDl >= 0.5 * d1 - 1e-9 && s.maxDl <= 2. * d1 + 1e-9, "loop advance per block stayed within [÷2, ×2] of ×1 — no jump anywhere (min %.4f max %.4f, ×1 = %.4f)", s.minDl, s.maxDl, d1);
        CHECK(s.guard_.jumps == 0, "the phase guard saw no jump across three switches (%u)", s.guard_.jumps);
        CHECK(s.minLpDrop == 0., "lp never went backward except at loop wraps (%d wraps)", s.wraps);
    }

    SECTION("ClockMap: ÷4/×4, re-request and cancel, no-pulse fallback");
    {
        Sim s; s.Run(100); s.LockAlign();
        s.want = 0.25; while(!s.map.Pending()) s.Poll();
        const double first = s.map.swTick;
        s.want = 4.; s.Poll();                                /* changed mind before the switch */
        CHECK(s.map.Pending() && s.map.pendMult == 4. && s.map.swTick >= first - 1e-9, "re-request replaces the pending switch (was ÷4 @%.1f, now ×4 @%.1f)", first, s.map.swTick);
        s.want = 1.; s.Poll();
        CHECK(!s.map.Pending(), "requesting the committed multiplier cancels the pending switch");
        s.want = 4.; while(!s.map.Pending()) s.Poll();
        const double L0 = s.map.Loop(s.map.swTick - 1e-9), L1 = s.map.Loop(s.map.swTick + 1e-9);
        CHECK(std::fabs(L1 - L0) < 1e-6, "L is continuous across the ×4 switch instant (%.9f)", L1 - L0);
        /* guard: a request that lands inside the guard of a pending switch is deferred, not applied */
        Sim g; g.Run(100); g.LockAlign();
        g.want = 2.; while(!g.map.Pending()) g.Poll();
        const double swt = g.map.swTick;
        while(g.T + g.guard() < swt) g.Poll();               /* now inside the guard window */
        g.want = 4.; g.Poll();
        CHECK(g.map.pendMult == 2. || g.map.mult == 2., "inside the guard the pending ×2 is left alone (pend %.1f mult %.1f)", g.map.pendMult, g.map.mult);
        /* no pulse ever seen: scheduling still works on the loop grid */
        bb::ClockMap m; m.Schedule(2., 1000.5, 1., -1., 0., 96.);
        CHECK(m.Pending() && OnGrid(m.swTick, 96.) && m.swTick >= 1001.5, "no-pulse fallback schedules on the next loop beat (%.1f)", m.swTick);
        /* SetMultNow is continuous */
        bb::ClockMap c; c.mult = 2.; c.off = -37.; const double Lb = c.Loop(5000.); c.SetMultNow(1., 5000.);
        CHECK(std::fabs(c.Loop(5000.) - Lb) < 1e-9 && c.mult == 1., "SetMultNow keeps L at the instant");
    }

    SECTION("ClockMap::AlignDelta and the half-step guard");
    {
        const double maxBack = 0.4 * 24.;                     /* 0.4 of a 16th at 96 PPQN */
        CHECK(bb::ClockMap::AlignDelta(5., 4., maxBack)  == -1., "24 PPQN: r=1 of 4 → back 1");
        CHECK(bb::ClockMap::AlignDelta(7., 4., maxBack)  ==  1., "24 PPQN: r=3 of 4 → forward 1");
        CHECK(bb::ClockMap::AlignDelta(5., 96., maxBack) == -5., "1 PPQN: r=5 of 96 → back 5 (under the guard)");
        CHECK(bb::ClockMap::AlignDelta(20., 96., maxBack) == 76., "1 PPQN: r=20 ≥ 0.4 step → forward 76, never a backward retrigger");
        CHECK(bb::ClockMap::AlignDelta(50., 96., maxBack) == 46., "1 PPQN: r=50 > half → forward 46");
        CHECK(bb::ClockMap::AlignDelta(-3., 4., maxBack) == -1., "negative L handled (r=1)");

        bb::PhaseGuard g;
        CHECK(g.Apply(0.5, 16) == 0.5, "first sample passes");
        CHECK(g.Apply(0.5 - 1. / 384., 16) == 0.5, "a torn read (−1 tick) is held");
        CHECK(g.Apply(0.5 - 1. / 384. + 0.001, 16) == 0.5, "…and stays held until the phase catches up");
        CHECK(g.Apply(0.501, 16) == 0.501, "then follows again");
        CHECK(g.Apply(0.3, 16) == 0.3 && g.jumps == 0, "a legitimate backward jump (0.2 loop) passes immediately");
        CHECK(g.Apply(0.35, 16) == 0.35 && g.jumps == 0, "a small forward step is not a jump");
        CHECK(g.Apply(0.6, 16) == 0.6 && g.jumps == 1, "a forward jump > 1.5 steps passes and is counted (%u)", g.jumps);
        /* a lock-alignment forward move at 1 PPQN (up to 3.6 steps) passes */
        const double before = g.last; const double after = before + 76. / 384.;
        CHECK(g.Apply(after, 16) == after && g.jumps == 2, "lock alignment forward 76 ticks passes (counted as a jump)");
        /* a backward lock alignment under 0.4 step is held, never retriggers */
        const double back = g.last - 9. / 384.;
        CHECK(g.Apply(back, 16) == g.last && g.last > back, "backward 9 ticks (< 0.4 step) is held");
        g.Rearm(); CHECK(g.Apply(0.01, 16) == 0.01, "re-armed guard passes anything");
    }

    SECTION("MultZone: hysteresis + dwell");
    {
        bb::MultZone z; uint32_t ms = 0;
        /* parked on the ×1/×2 edge with ±0.03 of noise for 5 s: never flips */
        int flips = 0;
        for(int i = 0; i < 5000; i++, ms++) { const float p = 0.65f + ((i & 1) ? 0.03f : -0.03f); if(z.Update(p, ms) != 2) flips++; }
        CHECK(flips == 0, "±0.03 noise on the 0.65 edge never leaves ×1 (%d)", flips);
        /* 1 o'clock (≈0.61) is comfortably ×1; 2 o'clock (≈0.72) is ×2 after the dwell */
        for(int i = 0; i < 300; i++, ms++) z.Update(0.61f, ms);
        CHECK(z.zone == 2, "0.61 (1 o'clock) is ×1");
        for(int i = 0; i < 50; i++, ms++) z.Update(0.72f, ms);
        CHECK(z.zone == 2, "50 ms at 0.72 (a brush) does not switch");
        for(int i = 0; i < 300; i++, ms++) z.Update(0.5f, ms);
        CHECK(z.zone == 2, "…and returns to rest still ×1");
        for(int i = 0; i < 150; i++, ms++) z.Update(0.72f, ms);
        CHECK(z.zone == 3, "150 ms at 0.72 (2 o'clock) is ×2");
        for(int i = 0; i < 500; i++, ms++) z.Update(0.63f, ms);
        CHECK(z.zone == 3, "back to 0.63 stays ×2 (leave edge is 0.61)");
        for(int i = 0; i < 500; i++, ms++) z.Update(0.60f, ms);
        CHECK(z.zone == 2, "0.60 leaves ×2");
        /* a fast sweep to ×4 never requests ×2 on the way */
        int saw3 = 0;
        for(int i = 0; i < 60; i++, ms++) { const float p = 0.5f + 0.5f * (float)i / 59.f; saw3 += z.Update(p, ms) == 3; }
        for(int i = 0; i < 300; i++, ms++) saw3 += z.Update(1.f, ms) == 3;
        CHECK(z.zone == 4 && saw3 == 0, "sweep to full lands on ×4 without passing through ×2 (%d)", saw3);
        CHECK(z.Mult() == 4.f && bb::MultZone::kMult[0] == 0.25f, "multiplier table");
        for(int i = 0; i < 300; i++, ms++) z.Update(0.0f, ms);
        CHECK(z.zone == 0 && z.Mult() == 0.25f, "full left is ÷4");
        z.Reset(); CHECK(z.zone == 2, "Reset → ×1");
    }

    SECTION("JitterMeter");
    {
        bb::JitterMeter j; uint16_t pk, rms;
        uint32_t t = 4000000000u;                              /* straddles the µs-counter wrap */
        for(int i = 0; i < 50; i++, t += 500000u) j.Pulse(t);
        j.Roll(pk, rms);
        CHECK(pk == 0 && rms == 0, "a steady 1 PPQN clock reads 0/0 across the 32-bit wrap (%u/%u)", pk, rms);
        t += 38000u;                                            /* one pulse 38 ms late, then back on grid */
        j.Pulse(t); t += 500000u - 38000u; j.Pulse(t);
        for(int i = 0; i < 10; i++) { t += 500000u; j.Pulse(t); }
        j.Roll(pk, rms);
        CHECK(pk >= 38000 && pk <= 45000, "a 38 ms late pulse reads as ~38 ms peak (%u)", pk);
        CHECK(rms > 10000 && rms < pk, "RMS is below the peak (%u)", rms);
        j.Roll(pk, rms);
        CHECK(pk == 0 && rms == 0, "the window clears on roll");
        j.Reset(); j.Pulse(0); j.Pulse(20833); j.Pulse(20833 + 90000);
        j.Roll(pk, rms);
        CHECK(pk == 65535, "peak saturates at 65535 µs (%u)", pk);
    }

    SECTION("PeriodRegress vs the EMA under DAW-style jitter (report)");
    {
        /* stamps t_k = k·P + e_k: gaussian σ 2 ms, and every 100th pulse 15 ms late (E-RM 2014's shape) */
        const double P = 20833.;
        uint64_t rs = 0x9E3779B97F4A7C15ull;
        auto U = [&]() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (double)(rs >> 11) / 9007199254740992.; };
        auto G = [&]() { const double u1 = U() + 1e-12, u2 = U(); return std::sqrt(-2. * std::log(u1)) * std::cos(6.283185307 * u2); };
        bb::PeriodRegress<16> ls; double ema = 0.; uint32_t prev = 0;
        double emaMax = 0., lsMax = 0., emaSq = 0., lsSq = 0.; int n = 0;
        for(int k = 0; k < 3000; k++)
        {
            double e = 2000. * G(); if(k % 100 == 50) e += 15000.;
            const uint32_t t = (uint32_t)(k * P + e + 1e6);
            ls.Push(t);
            if(k) { const double dt = (double)(t - prev); ema = ema > 0. ? ema + 0.15 * (dt - ema) : dt; }
            prev = t;
            if(k >= 32)
            {
                const double ee = std::fabs(ema - P), le = std::fabs(ls.PeriodUs() - P);
                if(ee > emaMax) emaMax = ee;
                if(le > lsMax)  lsMax  = le;
                emaSq += ee * ee; lsSq += le * le; n++;
            }
        }
        printf("  period error vs truth %.0f µs — EMA(0.15): max %.0f rms %.0f · LS(16): max %.0f rms %.0f\n",
               P, emaMax, std::sqrt(emaSq / n), lsMax, std::sqrt(lsSq / n));
        CHECK(lsMax < emaMax && std::sqrt(lsSq / n) < std::sqrt(emaSq / n), "the regression period is tighter than the EMA on peak and RMS");
        bb::PeriodRegress<8> r; r.Push(10); r.Push(20); r.Push(30);
        CHECK(std::fabs(r.PeriodUs() - 10.) < 1e-9, "exact slope on a clean ramp");
        r.Reset(); r.Push(5); CHECK(r.PeriodUs() == 0., "no period from one stamp");
    }

    /* BETA (docs/packages/beta-fw.md): the audio callback's load, Kyklophoria's DWT accounting */
    {
        printf("\n== BETA: the load meter — the block budget, ‰ mean and peak ==\n");
        CHECK(bb::CycleBudget(480000000u, 48000u, 24u) == 240000u, "480 MHz, 48 kHz, 24 samples: 240 000 cycles a block");
        CHECK(bb::CycleBudget(400000000u, 48000u, 24u) == 200000u && bb::CycleBudget(480000000u, 0u, 24u) == 0u, "400 MHz: 200 000; no rate: 0");
        CHECK(bb::LoadPermille(0u, 0u, 240000u) == 0u && bb::LoadPermille(5u, 10u, 0u) == 0u, "no blocks / no budget: 0");
        CHECK(bb::LoadPermille(2000ull * 60000u, 2000u, 240000u) == 250u, "60 000 cycles every block for a second: 250 ‰");
        CHECK(bb::LoadPermille(240000u, 1u, 240000u) == 1000u && bb::LoadPermille(300000u, 1u, 240000u) == 1250u, "a peak of the whole block: 1000 ‰; an overrun reads past it (1250)");
        CHECK(bb::LoadPermille(119u, 1u, 240000u) == 0u && bb::LoadPermille(121u, 1u, 240000u) == 1u, "rounded to the nearest ‰");
        CHECK(bb::LoadPermille(2000ull * 4000000000ull, 2000u, 240000u) == 65535u, "saturating at 65 535");
        const uint64_t sum = 1999ull * 50000u + 200000u;   // one heavy block among light ones
        CHECK(bb::LoadPermille(sum, 2000u, 240000u) == 209u && bb::LoadPermille(200000u, 1u, 240000u) == 833u, "the mean hides one heavy block (209 ‰), the peak shows it (833 ‰)");
    }

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
