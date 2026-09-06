// bb_clockmath.h — pure clock arithmetic for the Audiothurgist shell (CLOCK
// package). No SDK types, no allocation, no floating-point surprises: every
// struct here is plain data driven from the control thread (OnPoll) and read,
// by value, from the audio thread through the published ClockSnap.
//
//   ClockMap      NCO ticks → loop ticks, continuous across multiplier changes
//   PhaseGuard    the half-step monotonic guard on the loop phase
//   MultZone      P2 → ÷4 ÷2 ×1 ×2 ×4 with hysteresis and a dwell time
//   JitterMeter   per-pulse period deviation, 1 s peak / RMS
//   PeriodRegress least-squares period over the last N pulse stamps (Link idiom)
//
// Host-tested in tests/clock_check.cpp.
#pragma once
#include <cmath>
#include <cstdint>

namespace bb {

/* ── ClockMap ───────────────────────────────────────────────────────────
 * The audio thread derives the loop position from the NCO as
 *     L = T·mult + off,   lp = fmod(L, loopT) / loopT
 * where T = MasterTick + FracTick (+ extrapolation) in NCO ticks and L is in
 * loop ticks (one 16th = Ppqn/4 loop ticks). A multiplier change is a change
 * of slope; `off` is re-solved at the switch instant so L is CONTINUOUS —
 * the loop never jumps, it simply starts running faster or slower from that
 * tick on.
 *
 * Switches are scheduled, not immediate. The control thread asks for a
 * multiplier every poll (Request); the map picks the first NCO instant T0 at
 * or after T + guard that is (a) an external pulse instant, T0 = pulseTick +
 * n·tpp, and (b) a loop beat: L(T0) ≡ 0 (mod align), to within half a pulse.
 * Both conditions matter: (a) keeps the clock's pulses on the loop's grid
 * after the switch (at ×2 every pulse still lands on an even pulse position,
 * at ÷2 every other pulse does), (b) makes the new speed start on a beat of
 * the break. The scheduled map is published alongside the committed one and
 * the audio thread selects by comparing its own T against swTick, so the
 * switch is block-exact even though the control thread only sees T every
 * millisecond. `guard` (a few ms of ticks) keeps a re-schedule from touching
 * a pending map the audio thread may already be reading.
 */
struct ClockMap
{
    static constexpr double kNever = 1e300;

    double mult = 1.0, off = 0.0;                     /* committed map */
    double pendMult = 1.0, pendOff = 0.0;             /* scheduled map … */
    double swTick = kNever;                           /* … applies from this T */

    bool   Pending() const { return swTick < kNever; }
    double Target()  const { return Pending() ? pendMult : mult; }
    double MultAt(double T) const { return T >= swTick ? pendMult : mult; }
    double Loop  (double T) const { return T >= swTick ? T * pendMult + pendOff : T * mult + off; }

    /* Control thread, every poll: adopt the scheduled map once T has passed it. */
    bool Commit(double T)
    {
        if(!Pending() || T < swTick) return false;
        mult = pendMult; off = pendOff; swTick = kNever;
        return true;
    }
    void Cancel() { swTick = kNever; }

    /* Change the slope right now, continuously (ext/internal transitions). */
    void SetMultNow(double m, double T)
    {
        const double L = Loop(T);
        mult = m; off = L - m * T; swTick = kNever;
    }

    /* Move the loop by dL loop ticks — the one deliberate discontinuity, used
     * once per PLL lock to put the clock's pulses on the loop grid. */
    void Shift(double dL) { off += dL; pendOff += dL; }

    /* Idempotent request: call every poll with the multiplier the user wants.
     * Returns true when the schedule changed. */
    bool Request(double m, double T, double guard, double pulseTick, double tpp, double align)
    {
        if(m == Target()) return false;
        if(Pending() && T + guard >= swTick) return false;   /* too close to touch; retry after Commit */
        if(m == mult) { Cancel(); return true; }             /* user changed their mind in time */
        return Schedule(m, T, guard, pulseTick, tpp, align);
    }

    /* Find the switch instant (see the header comment) and solve pendOff for
     * continuity. tpp ≤ 0 (no pulse seen yet) degrades to "any integer tick". */
    bool Schedule(double m, double T, double guard, double pulseTick, double tpp, double align)
    {
        if(!(tpp > 0.)) { tpp = 1.; pulseTick = std::floor(T); }
        if(!(align > 0.)) align = 1.;
        double n = std::ceil((T + guard - pulseTick) / tpp);
        if(n < 1.) n = 1.;
        const double stepL = tpp * mult;                        /* loop ticks between candidates */
        const double tol   = 0.5 * (stepL < align ? stepL : align);
        double best = pulseTick + n * tpp, bestErr = 1e300;
        for(int i = 0; i < 1024; i++, n += 1.)
        {
            const double T0 = pulseTick + n * tpp;
            const double e  = std::fabs(std::remainder(T0 * mult + off, align));
            if(e < bestErr) { bestErr = e; best = T0; }
            if(e <= tol) break;
        }
        pendMult = m;
        swTick   = best;
        pendOff  = (best * mult + off) - m * best;              /* L continuous at swTick */
        return true;
    }

    /* Lock alignment: the loop-tick shift that puts L (the loop position at
     * the lock pulse) on a multiple of A. Nearest direction wins, except that
     * a backward move of maxBack or more becomes the forward move instead —
     * backward moves under half a step are absorbed silently by PhaseGuard,
     * larger ones would retrigger the previous step. */
    static double AlignDelta(double L, double A, double maxBack)
    {
        if(!(A > 0.)) return 0.;
        double r = std::fmod(L, A);
        if(r < 0.) r += A;
        return (r <= 0.5 * A && r < maxBack) ? -r : A - r;
    }
};

/* ── PhaseGuard ──────────────────────────────────────────────────────────
 * Monotonic guard on the loop phase, half a step wide. A torn MasterTick/
 * FracTick read is at most ±1 tick (1/24 step) and must not fire a backward
 * step (double trigger + a bogus flow-reroll); anything larger backward is a
 * legitimate jump (new material, transport reset, lock alignment) and passes
 * immediately. Forward jumps always pass; those over 1.5 steps are counted
 * (the web app's "jumps" diagnostic). Audio thread only. */
struct PhaseGuard
{
    double   last  = -1.;   /* -1 = re-armed */
    uint16_t jumps = 0;

    void   Rearm() { last = -1.; }
    double Apply(double lp, int nSlices)
    {
        const double thr = 0.5 / (double)(nSlices > 0 ? nSlices : 1);
        if(last >= 0. && lp < last && (last - lp) < thr) lp = last;
        if(last >= 0. && lp - last > 3. * thr) jumps = (uint16_t)(jumps + 1u);
        last = lp;
        return lp;
    }
};

/* ── MultZone ────────────────────────────────────────────────────────────
 * P2 while following (spec §6.4): a clock multiplier/divider around the
 * followed tempo with a wide ×1 centre. Zone map on the pot's 0..1 travel
 * (7 o'clock = 0, 12 o'clock = 0.5, 5 o'clock = 1 on a 300° pot):
 *
 *     ÷4      ÷2        ×1          ×2       ×4
 *   0 ─── 0.15 ─── 0.35 ─────── 0.65 ─── 0.85 ─── 1
 *        8:30      10:30        1:30      3:30
 *
 * Two defences against a knob parked on an edge: hysteresis — a zone is left
 * only once the pot is kHyst PAST the edge (so the enter/leave positions are
 * 2·kHyst = 0.08 ≈ 24° apart) — and a dwell — the new zone must hold for
 * kDwellMs before it is accepted, so a hand brushing the knob or a sweep
 * through a zone on the way to another never requests it. */
struct MultZone
{
    static constexpr float    kEdge[4] = {0.15f, 0.35f, 0.65f, 0.85f};
    static constexpr float    kMult[5] = {0.25f, 0.5f, 1.f, 2.f, 4.f};
    static constexpr float    kHyst    = 0.04f;
    static constexpr uint32_t kDwellMs = 100u;

    int      zone = 2, cand = 2;
    uint32_t candSinceMs = 0;

    void Reset(int z = 2) { zone = cand = z; }
    static int Raw(float p, int from)
    {
        int z = from;
        while(z < 4 && p > kEdge[z] + kHyst)     z++;
        while(z > 0 && p < kEdge[z - 1] - kHyst) z--;
        return z;
    }
    int Update(float p, uint32_t nowMs)
    {
        const int z = Raw(p, zone);
        if(z != cand) { cand = z; candSinceMs = nowMs; }
        if(cand != zone && (uint32_t)(nowMs - candSinceMs) >= kDwellMs) zone = cand;
        return zone;
    }
    float Mult() const { return kMult[zone]; }
};

/* ── JitterMeter ─────────────────────────────────────────────────────────
 * Per pulse: the deviation of this period from the running period (a slow
 * EMA, α = 0.1 — what the PLL has to swallow each pulse; a single late pulse
 * reads as its lateness once, not twice as a strict cycle-to-cycle difference
 * would). Roll() hands out the window's peak and RMS in µs, 16-bit saturated.
 * Honesty note: pulses are stamped on the ~1 ms control poll, so the meter's
 * own floor is about one poll gap — compare with pollGapUs. */
struct JitterMeter
{
    uint32_t prevStamp = 0;
    double   ref = 0.;          /* running period, µs; 0 = unseeded */
    bool     havePrev = false;
    uint32_t peak = 0; double sq = 0.; uint32_t n = 0;

    void Reset() { havePrev = false; ref = 0.; }
    void Pulse(uint32_t stampUs)
    {
        if(havePrev)
        {
            const double per = (double)(uint32_t)(stampUs - prevStamp);
            if(ref > 0.)
            {
                const double d = std::fabs(per - ref);
                const uint32_t du = d > 4e9 ? 4000000000u : (uint32_t)d;
                if(du > peak) peak = du;
                sq += d * d; n++;
                ref += 0.1 * (per - ref);
            }
            else ref = per;
        }
        prevStamp = stampUs; havePrev = true;
    }
    void Roll(uint16_t& peakUs, uint16_t& rmsUs)
    {
        peakUs = (uint16_t)(peak > 65535u ? 65535u : peak);
        const double r = n ? std::sqrt(sq / (double)n) : 0.;
        rmsUs = (uint16_t)(r > 65535. ? 65535. : r);
        peak = 0; sq = 0.; n = 0;
    }
};

/* ── PeriodRegress ───────────────────────────────────────────────────────
 * Least-squares slope of pulse stamp against pulse index over the last N
 * pulses (Ableton Link's HostTimeFilter idiom). One late pulse moves the
 * estimate by ~1/N of its error instead of the EMA's 15 %. Reset it whenever
 * the follower drops a pulse (loss/restart) — the index axis assumes no gaps.
 * Evaluated in tests/clock_check.cpp; not wired into the firmware, because
 * the SDK follower has no hook for an external base period (see the package
 * doc). */
template<int N>
struct PeriodRegress
{
    uint32_t stamp[N];
    int      count = 0, head = 0;

    void Reset() { count = 0; head = 0; }
    void Push(uint32_t us) { stamp[head] = us; head = (head + 1) % N; if(count < N) count++; }
    double PeriodUs() const
    {
        if(count < 2) return 0.;
        const int      start = (head - count + N) % N;
        const uint32_t t0    = stamp[start];
        double sx = 0., sy = 0., sxx = 0., sxy = 0.;
        for(int i = 0; i < count; i++)
        {
            const double x = (double)i;
            const double y = (double)(uint32_t)(stamp[(start + i) % N] - t0);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double den = (double)count * sxx - sx * sx;
        return den > 0. ? ((double)count * sxy - sx * sy) / den : 0.;
    }
};

} // namespace bb
